#include "AppOutput.h"
#include "LookAndFeel.h"
#include "PluginProcessor.h"
#include "Theme.h"

//==============================================================================
// AppOutput

AppOutput::Monitor AppOutput::monitorFor(room::Width width, int deviceOutputs)
{
    if (deviceOutputs >= room::layout(width).numChannels || deviceOutputs <= 0)
        return Monitor::Direct;
    return deviceOutputs >= 2 ? Monitor::Stereo : Monitor::Mono;
}

AppOutput::AppOutput(SwooshbucklerAudioProcessor& rig)
    // Its bus is the device's: any number of outputs. An input bus is there (switched off) only so a device with
    // inputs can be matched; nothing is read from it.
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), false)
                                      .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , m_rig(rig)
{
    m_ceiling = m_rig.getState().getRawParameterValue(ParamIDs::ceiling);
}

bool AppOutput::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const int outs = layouts.getMainOutputChannels();
    return outs >= 1 && outs <= 64;
}

void AppOutput::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // The rig plays the Listening room, at its width whatever the device has (set here, while nothing is processed).
    m_room = m_rig.getListening();
    m_roomChannels = room::layout(m_room).numChannels;
    m_order = room::deviceChannels(m_room);
    {
        auto layout = m_rig.getBusesLayout();
        layout.outputBuses.getReference(0) = room::channelSet(m_room);
        [[maybe_unused]] const bool taken = m_rig.setBusesLayout(layout);
        jassert(taken); // the app's rig takes exactly its Listening width
    }
    const int block = juce::jmax(1, samplesPerBlock);
    m_rig.setRateAndBufferSizeDetails(sampleRate, block);
    m_rig.prepareToPlay(sampleRate, block);

    const int outs = getTotalNumOutputChannels();
    m_monitor = monitorFor(m_room, outs);
    m_fold = m_monitor == Monitor::Stereo ? &room::fold(m_room, room::Width::Stereo)
           : m_monitor == Monitor::Mono   ? &room::fold(m_room, room::Width::Mono)
                                          : nullptr;
    m_roomBuffer.setSize(room::c_maxChannels, block);
    m_foldBuffer.setSize(2, block);
    m_chunkMidi.ensureSize(4096);
    m_limiter.prepare(sampleRate, 2);
    m_limiter.reset();

    // Silent until the rig's first sound comes through its look-ahead (and the fold's), then in over 10 ms.
    m_gain = 0.0f;
    m_step = static_cast<float>(1.0 / (c_fadeSeconds * juce::jmax(1000.0, sampleRate)));
    m_fadeOut.store(false);
    m_fadedOut.store(false);
    m_rig.setDeviceOutputs(outs, juce::jmax(outs, m_rig.getDeviceOutputsAvailable()));
    setLatencySamples(m_rig.getLatencySamples() + (m_fold != nullptr ? m_limiter.getLatencySamples() : 0));
    m_holdIn = getLatencySamples();
    m_prepared.fetch_add(1);
}

void AppOutput::releaseResources()
{
    m_rig.releaseResources();
}

void AppOutput::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int capacity = m_roomBuffer.getNumSamples();
    if (n <= capacity)
    {
        play(buffer, 0, n, midi);
        return;
    }
    // More than the device said it would send: in pieces, each with its own keys (no allocation: sized in prepare).
    for (int start = 0; start < n; start += capacity)
    {
        const int len = juce::jmin(capacity, n - start);
        m_chunkMidi.clear();
        m_chunkMidi.addEvents(midi, start, len, -start);
        play(buffer, start, len, m_chunkMidi);
    }
}

void AppOutput::play(juce::AudioBuffer<float>& out, int start, int numSamples, juce::MidiBuffer& midi)
{
    // The rig plays the whole room into its own buffer.
    juce::AudioBuffer<float> roomBlock(m_roomBuffer.getArrayOfWritePointers(), m_roomChannels, numSamples);
    roomBlock.clear();
    m_rig.setPlayHead(getPlayHead());
    {
        const juce::ScopedLock lock(m_rig.getCallbackLock()); // as JUCE's player does for the processor it plays
        if (! m_rig.isSuspended())
            m_rig.processBlock(roomBlock, midi);
    }
    m_rig.setPlayHead(nullptr);

    const int outs = out.getNumChannels();
    int written = 0; // outputs 0 .. written-1 carry sound; the rest are silent
    if (m_monitor == Monitor::Direct)
    {
        // Speaker for speaker, in the order a room is wired (room::deviceChannels: the tops last).
        for (int ch = 0; ch < outs && ch < m_roomChannels; ++ch)
            out.copyFrom(ch, start, roomBlock, m_order[static_cast<size_t>(ch)], 0, numSamples);
        written = juce::jmin(outs, m_roomChannels);
        if (m_roomChannels == 1 && outs >= 2)
        {
            out.copyFrom(1, start, roomBlock, 0, 0, numSamples); // Mono: the one channel on the first two outputs
            written = 2;
        }
    }
    else
    {
        // A fold-down adds speakers together, so it is held under the rig's own Ceiling again.
        const int folded = m_fold->numOut;
        float* foldPtrs[2] = { m_foldBuffer.getWritePointer(0), m_foldBuffer.getWritePointer(1) };
        m_fold->numIn == m_roomChannels ? room::applyFold(*m_fold, roomBlock.getArrayOfReadPointers(), foldPtrs, numSamples)
                                        : juce::FloatVectorOperations::clear(foldPtrs[0], numSamples);
        MasterProcessor::Parameters limit;
        limit.limiterOn = true;
        limit.ceilingDb = m_ceiling != nullptr ? m_ceiling->load(std::memory_order_relaxed) : -0.3f;
        limit.releaseMs = 50.0f;
        m_limiter.setParameters(limit);
        m_limiter.process(foldPtrs, folded, numSamples);
        for (int ch = 0; ch < folded && ch < outs; ++ch)
            out.copyFrom(ch, start, foldPtrs[ch], numSamples);
        written = juce::jmin(outs, folded);
    }
    for (int ch = written; ch < outs; ++ch)
        out.clear(ch, start, numSamples);

    // In after a prepare, out before a change of room: 10 ms either way.
    const bool fadingOut = m_fadeOut.load(std::memory_order_relaxed);
    const float target = fadingOut ? 0.0f : 1.0f;
    if (m_gain <= 0.0f && target <= 0.0f)
    {
        for (int ch = 0; ch < written; ++ch)
            out.clear(ch, start, numSamples); // faded out: silent until prepared again
    }
    else if (! juce::exactlyEqual(m_gain, target))
    {
        for (int i = 0; i < numSamples; ++i)
        {
            if (m_holdIn > 0 && ! fadingOut)
                --m_holdIn; // the rig's look-ahead: nothing to fade in yet
            else
                m_gain = target > m_gain ? juce::jmin(target, m_gain + m_step) : juce::jmax(target, m_gain - m_step);
            for (int ch = 0; ch < written; ++ch)
                out.getWritePointer(ch, start)[i] *= m_gain;
        }
    }
    if (fadingOut && m_gain <= 0.0f)
        m_fadedOut.store(true, std::memory_order_relaxed);
}

//==============================================================================
// AppAudio

AppAudio::AppAudio(SwooshbucklerAudioProcessor& rig, juce::AudioDeviceManager& devices, juce::AudioProcessorPlayer& player)
    : m_rig(rig), m_devices(devices), m_player(player), m_output(rig)
{
    m_rig.onListeningChanged = [this] { listeningChanged(); };
    m_devices.addChangeListener(this);
}

AppAudio::~AppAudio()
{
    stopTimer();
    m_rig.onListeningChanged = nullptr;
    m_devices.removeChangeListener(this);
    if (m_player.getCurrentProcessor() == &m_output)
        m_player.setProcessor(nullptr);
}

juce::BigInteger AppAudio::outputsFor(const juce::BigInteger& active, int available, int needed)
{
    auto wanted = active;
    wanted.setRange(available, juce::jmax(0, wanted.getHighestBit() + 1 - available), false); // only outputs it has
    if (wanted.countNumberOfSetBits() >= needed || available <= wanted.countNumberOfSetBits())
        return wanted;
    // From the lowest output switched on up (so a pair picked further along the device stays where it is), then
    // from the first output, until the room fits.
    const int lowest = juce::jmax(0, wanted.findNextSetBit(0));
    for (int ch = lowest; ch < available && wanted.countNumberOfSetBits() < needed; ++ch)
        wanted.setBit(ch);
    for (int ch = 0; ch < available && wanted.countNumberOfSetBits() < needed; ++ch)
        wanted.setBit(ch);
    return wanted;
}

void AppAudio::start()
{
    // The player lets go of the rig first (JUCE's holder gave it the rig itself): were it still the player's when
    // AppOutput is given, the player would release the rig after AppOutput had prepared it.
    m_player.setProcessor(nullptr);
    openRoomOutputs();
    m_player.setProcessor(&m_output);
    publish();
}

void AppAudio::openRoomOutputs()
{
    auto* device = m_devices.getCurrentAudioDevice();
    if (device == nullptr)
        return;
    m_deviceName = device->getName();
    const int available = device->getOutputChannelNames().size();
    const auto active = device->getActiveOutputChannels();
    const auto wanted = outputsFor(active, available, room::layout(m_rig.getListening()).numChannels);
    if (wanted == active)
        return;
    auto setup = m_devices.getAudioDeviceSetup();
    setup.outputChannels = wanted;
    setup.useDefaultOutputChannels = false;
    m_devices.setAudioDeviceSetup(setup, true); // the device starts again: the player prepares what it plays
}

void AppAudio::listeningChanged()
{
    // Out first (10 ms), then the new room.
    m_output.fadeOut();
    m_fadeStarted = juce::Time::getMillisecondCounterHiRes();
    startTimer(5);
}

void AppAudio::timerCallback()
{
    if (finishChange())
        stopTimer();
}

bool AppAudio::finishChange()
{
    // Waits for the fade, but not for ever: a device that has stopped calls back no more.
    const bool running = m_devices.getCurrentAudioDevice() != nullptr && m_devices.getCurrentAudioDevice()->isPlaying();
    if (! m_output.isFadedOut() && running && juce::Time::getMillisecondCounterHiRes() - m_fadeStarted < 300.0)
        return false;
    stopTimer();
    apply();
    return true;
}

void AppAudio::apply()
{
    const int before = m_output.timesPrepared();
    openRoomOutputs();
    if (m_output.timesPrepared() == before)
    {
        // The device did not start again (it had the outputs already): prepare the rig at the new width here.
        m_player.setProcessor(nullptr);
        m_player.setProcessor(&m_output);
    }
    publish();
}

void AppAudio::publish()
{
    auto* device = m_devices.getCurrentAudioDevice();
    if (device == nullptr)
    {
        m_rig.setDeviceOutputs(0, 0);
        return;
    }
    m_rig.setDeviceOutputs(device->getActiveOutputChannels().countNumberOfSetBits(), device->getOutputChannelNames().size());
}

void AppAudio::changeListenerCallback(juce::ChangeBroadcaster*)
{
    deviceChanged();
}

void AppAudio::deviceChanged()
{
    // Another device chosen: switch on the outputs the room needs (outputs switched off by hand on the same
    // device stay off).
    if (auto* device = m_devices.getCurrentAudioDevice(); device != nullptr && device->getName() != m_deviceName)
        openRoomOutputs();
    publish();
}

juce::String AppAudio::dialogText(room::Width width, const juce::BigInteger& active, int available)
{
    const auto& room = room::layout(width);
    const int on = active.countNumberOfSetBits();
    const auto order = room::deviceChannels(width);
    const auto nameAt = [&](int slot) { return juce::String(room::info(room.speakers[static_cast<size_t>(order[static_cast<size_t>(slot)])]).name); };
    const juce::String glue = juce::String::charToString(0x00a0); // a number stays on the line of its speaker
    juce::String s;
    s << "Listening: " << room.name << ", ";
    if (width == room::Width::Mono)
        s << "one channel, played on the first two outputs switched on.";
    else if (on >= room.numChannels)
    {
        // Each speaker beside the number of the output it plays on (the device's own numbering).
        s << room.numChannels << " outputs. The speakers play on these outputs: ";
        int output = -1;
        for (int slot = 0; slot < room.numChannels; ++slot)
        {
            output = active.findNextSetBit(output + 1);
            s << (slot > 0 ? ", " : "") << (output + 1) << glue << nameAt(slot);
        }
        s << ".";
    }
    else
    {
        s << room.numChannels << " outputs. The speakers go out in this order, lowest output first: ";
        for (int slot = 0; slot < room.numChannels; ++slot)
            s << (slot > 0 ? " " : "") << nameAt(slot);
        s << ".";
    }
    if (const auto status = SwooshbucklerAudioProcessor::listeningStatus(width, on, available); status.isNotEmpty())
        s << "\n" << status << " Switch on more outputs below, or choose a device that has them.";
    return s;
}

void AppAudio::showDialog()
{
    juce::DialogWindow::LaunchOptions o;
    auto panel = std::make_unique<DevicePanel>(m_devices, m_rig);
    o.content.setOwned(panel.release());
    o.dialogTitle = "Audio Device";
    o.dialogBackgroundColour = Theme::Colours::window;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

//==============================================================================
// The dialog

AppAudio::DevicePanel::DevicePanel(juce::AudioDeviceManager& devices, SwooshbucklerAudioProcessor& rig)
    : m_devices(devices), m_rig(rig),
      // No inputs (the rig has none); up to twelve outputs, one by one (not in pairs), so any output can carry any
      // speaker of the room.
      m_selector(devices, 0, 0, 1, room::c_maxChannels, true, false, false, false)
{
    addAndMakeVisible(m_selector);
    m_devices.addChangeListener(this);
    refresh();
    fit();
    startTimer(500); // the fold's status follows the device as it starts again
}

AppAudio::DevicePanel::~DevicePanel()
{
    m_devices.removeChangeListener(this);
}

void AppAudio::DevicePanel::refresh()
{
    juce::BigInteger active;
    int available = 0;
    if (auto* device = m_devices.getCurrentAudioDevice())
    {
        active = device->getActiveOutputChannels();
        available = device->getOutputChannelNames().size();
    }
    const auto text = dialogText(m_rig.getListening(), active, available);
    if (text == m_text)
        return;
    const bool hadWarning = m_text.containsChar('\n');
    m_text = text;
    if (hadWarning != m_text.containsChar('\n'))
        fit(); // the fold's line comes and goes: the panel is as tall as what it says
    repaint();
}

int AppAudio::DevicePanel::textHeight() const
{
    return m_text.containsChar('\n') ? c_orderHeight + c_gap + c_warningHeight : c_orderHeight;
}

void AppAudio::DevicePanel::fit()
{
    // The plate's title bar, what the room needs and where it goes, then JUCE's device settings, which take the
    // height they need (the panel follows them: childBoundsChanged).
    const int height = c_margin + c_title + textHeight() + c_gap + juce::jmax(m_selector.getHeight(), 100) + c_foot;
    if (height != getHeight() || getWidth() != c_width)
        setSize(c_width, height);
    else
        resized();
}

void AppAudio::DevicePanel::resized()
{
    auto r = getLocalBounds().reduced(c_margin, 0).withTrimmedTop(c_margin + c_title);
    m_textArea = r.removeFromTop(textHeight());
    r.removeFromTop(c_gap);
    m_selector.setBounds(r.withHeight(juce::jmax(m_selector.getHeight(), 100)));
}

void AppAudio::DevicePanel::childBoundsChanged(juce::Component* child)
{
    if (child == &m_selector)
        fit();
}

void AppAudio::DevicePanel::paint(juce::Graphics& g)
{
    juce::SharedResourcePointer<Theme::Fonts> fonts;
    g.fillAll(Theme::Colours::window);
    SwooshLookAndFeel::drawPanel(g, getLocalBounds().toFloat().reduced(6.0f), "Audio Device", "Where the room plays",
                                 Theme::Colours::brass, *fonts, Icons::Icon::Settings);
    const auto lines = juce::StringArray::fromLines(m_text);
    auto area = m_textArea.reduced(8, 0);
    Theme::fitLines(g, lines[0], area.removeFromTop(c_orderHeight), fonts->body(), Theme::Colours::textDim);
    if (lines.size() > 1)
        Theme::fitLines(g, lines[1], area.withTrimmedTop(c_gap), fonts->body(), Theme::Colours::brass);
}
