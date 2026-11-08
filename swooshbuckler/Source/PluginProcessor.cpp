#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    std::unique_ptr<juce::PropertiesFile> openSettings()
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "Swooshbuckler";
        o.folderName = "Sweet Justice";
        o.filenameSuffix = "settings";
        o.osxLibrarySubFolder = "Application Support";
        o.commonToAllUsers = false;
        return std::make_unique<juce::PropertiesFile>(o);
    }
}

SwooshbucklerAudioProcessor::SwooshbucklerAudioProcessor()
    // Sample-based instrument: no audio input (so no feedback loop, no microphone), MIDI in, stereo out.
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , m_state(*this, nullptr, "Swooshbuckler", Params::createLayout())
{
    auto raw = [this](const char* id) { return m_state.getRawParameterValue(id); };
    for (int i = 0; i < ParamIDs::numCargoTracks; ++i)
    {
        m_raw.tracks[static_cast<size_t>(i)] = raw(ParamIDs::cargoTrack[i]);
        m_raw.trackSemis[static_cast<size_t>(i)] = raw(ParamIDs::trackSemis[i]);
        m_raw.trackCents[static_cast<size_t>(i)] = raw(ParamIDs::trackCents[i]);
        m_raw.trackVari[static_cast<size_t>(i)] = raw(ParamIDs::trackVari[i]);
    }
    m_raw.pitch = raw(ParamIDs::pitch);
    m_raw.input = raw(ParamIDs::input);
    m_raw.rideOn = raw(ParamIDs::rideOn);
    m_raw.energy = raw(ParamIDs::energy);
    m_raw.rate = raw(ParamIDs::rate);
    m_raw.fill = raw(ParamIDs::fill);
    m_raw.chaos = raw(ParamIDs::chaos);
    for (int b = 0; b < ParamIDs::numBuses; ++b)
        for (int k = 0; k < ParamIDs::numBusControls; ++k)
            m_raw.bus[b][k] = ParamIDs::bus[b][k] != nullptr ? raw(ParamIDs::bus[b][k]) : nullptr;
    m_raw.hitSense = raw(ParamIDs::hitSense);
    m_raw.hitBoost = raw(ParamIDs::hitBoost);
    m_raw.hitRelease = raw(ParamIDs::hitRelease);
    m_raw.hitDuck = raw(ParamIDs::hitDuck);
    m_raw.hitSync = raw(ParamIDs::hitSync);
    m_raw.gateMode = raw(ParamIDs::gateMode);
    m_raw.hitHold = raw(ParamIDs::hitHold);
    m_raw.hitFloor = raw(ParamIDs::hitFloor);
    m_raw.duckMode = raw(ParamIDs::duckMode);
    m_raw.duckReturn = raw(ParamIDs::duckReturn);
    m_raw.takeTurns = raw(ParamIDs::takeTurns);
    m_raw.linkBuses = raw(ParamIDs::linkBuses);
    m_raw.space = raw(ParamIDs::space);
    m_raw.inflate = raw(ParamIDs::inflate);
    m_raw.push = raw(ParamIDs::push);
    m_raw.ceiling = raw(ParamIDs::ceiling);
    m_raw.morphOn = raw(ParamIDs::morphOn);
    m_raw.morphMode = raw(ParamIDs::morphMode);
    m_raw.morphX = raw(ParamIDs::morphX);
    m_raw.morphY = raw(ParamIDs::morphY);
    for (int m = 0; m < ParamIDs::numMods; ++m)
    {
        m_raw.modType[m] = raw(ParamIDs::modType[m]);
        m_raw.modShape[m] = raw(ParamIDs::modShape[m]);
        m_raw.modKnob[m] = raw(ParamIDs::modKnob[m]);
        m_raw.modFollow[m] = raw(ParamIDs::modFollow[m]);
    }
    for (const auto& id : ModMatrix::targets())
        m_modTargets.push_back({ m_state.getParameter(id), m_state.getRawParameterValue(id) });


    m_engine.setCargoHold(&m_cargo);
    m_settings = openSettings();
    startTimerHz(4);
}

SwooshbucklerAudioProcessor::~SwooshbucklerAudioProcessor()
{
    stopTimer();
    m_recorder.stop();
}

bool SwooshbucklerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    return layouts.getMainInputChannelSet().isDisabled();
}

void SwooshbucklerAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    m_deviceRate = sampleRate;
    m_blockSize = juce::jmax(256, samplesPerBlock);
    m_mods.prepare(sampleRate); // the modulators run once per device block
    m_outRing.prepare(sampleRate);
    prepareInternal();
}

void SwooshbucklerAudioProcessor::prepareInternal()
{
    // The rig renders at the recording rate when that is higher than the device rate, so a 96 kHz
    // recording really has content up to 48 kHz (and 96 kHz samples keep their top end, like a
    // 96 kHz Reaper project). What you hear is a clean windowed-sinc downsample of it.
    const double recordRate = static_cast<double>(getRecordRate());
    m_renderRate = recordRate > m_deviceRate + 0.5 ? recordRate : m_deviceRate;
    const double ratio = m_renderRate / m_deviceRate;

    updateEngineParams();
    m_engine.prepare(m_renderRate, m_blockSize);
    m_master.prepare(m_renderRate, 2);
    m_recorder.prepare(m_renderRate, 2);
    m_work.setSize(2, m_blockSize);
    m_fifo.setSize(2, static_cast<int>(std::ceil(m_blockSize * ratio)) + 1024);
    m_fifo.clear();
    m_fifoCount = 0;
    for (auto& d : m_down)
        d.reset();
    m_pushGain.reset(m_renderRate, 0.02); // short: a wire on Push must come through (retargeted every section)
    m_pushGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(m_pushDb));

    const int resampleLatency = ratio > 1.0 ? juce::roundToInt(juce::WindowedSincInterpolator::getBaseLatency() / ratio) : 0;
    setLatencySamples(juce::roundToInt((m_engine.getLatencySamples() + m_master.getLatencySamples()) / ratio) + resampleLatency);
}

void SwooshbucklerAudioProcessor::renderAtRenderRate(const float* inL, const float* inR, float* l, float* r, int n)
{
    m_engine.process(inL, inR, l, r, n);

    // Push (drive into the limiter; the ceiling stays put)
    m_pushGain.setTargetValue(juce::Decibels::decibelsToGain(m_pushDb));
    for (int i = 0; i < n; ++i)
    {
        const float g = m_pushGain.getNextValue();
        l[i] *= g;
        r[i] *= g;
    }
    float* chans[2] = { l, r };
    m_master.process(chans, 2, n);
    m_recorder.process(chans, 2, n); // the tape gets the full-rate signal
}

void SwooshbucklerAudioProcessor::releaseResources() {}

float SwooshbucklerAudioProcessor::modValue(std::atomic<float>* raw)
{
    const float value = raw->load();
    for (size_t i = 0; i < m_modTargets.size(); ++i)
    {
        if (m_modTargets[i].raw != raw)
            continue;
        const int t = static_cast<int>(i);
        const float offset = m_mods.offset(t);
        const bool wired = m_mods.isWired(t);
        if (! wired && offset == 0.0f)
            return value;
        auto* param = m_modTargets[i].param;
        const float norm = juce::jlimit(0.0f, 1.0f, param->convertTo0to1(value) + offset);
        if (wired)
            m_mods.publishTarget(t, norm);
        return param->convertFrom0to1(norm);
    }
    return value;
}

void SwooshbucklerAudioProcessor::publishModViews()
{
    for (size_t i = 0; i < m_modTargets.size(); ++i)
    {
        const int t = static_cast<int>(i);
        if (m_mods.isWired(t))
            m_mods.publishTarget(t, juce::jlimit(0.0f, 1.0f, m_modTargets[i].param->convertTo0to1(m_modTargets[i].raw->load()) + m_mods.offset(t)));
    }
}

void SwooshbucklerAudioProcessor::runModulators(int numSamples)
{
    std::array<ModMatrix::SourceSettings, ModMatrix::c_numSources> settings;
    for (int m = 0; m < ModMatrix::c_numSources; ++m)
    {
        auto& set = settings[static_cast<size_t>(m)];
        set.type = static_cast<ModMatrix::Type>(juce::jlimit(0, 3, juce::roundToInt(m_raw.modType[m]->load())));
        set.shape = static_cast<ModMatrix::Shape>(juce::jlimit(0, 4, juce::roundToInt(m_raw.modShape[m]->load())));
        set.knob = m_raw.modKnob[m]->load();
        set.follow = juce::jlimit(0, ModMatrix::c_numFollowerInputs - 1, juce::roundToInt(m_raw.modFollow[m]->load()));
    }
    m_mods.process(settings, numSamples, m_followerLevels);
}

void SwooshbucklerAudioProcessor::updateEngineParams()
{
    static_assert(ParamIDs::numCargoTracks == CargoHold::c_numTracks, "one level parameter per track");

    RigEngine::Params p;
    for (size_t i = 0; i < p.levels.size(); ++i)
    {
        p.levels[i] = m_raw.tracks[i]->load();
        // Semitones plus cents, held to two octaves either way so VARI on and off reach the same extremes.
        p.trackPitch[i] = juce::jlimit(-24.0f, 24.0f, std::round(m_raw.trackSemis[i]->load()) + std::round(m_raw.trackCents[i]->load()) / 100.0f);
        p.trackVari[i] = m_raw.trackVari[i]->load() > 0.5f;
    }
    p.pitch = modValue(m_raw.pitch);
    p.input = 0.0f; // sample-based only
    p.rideOn = m_raw.rideOn->load() > 0.5f;
    p.energy = modValue(m_raw.energy);
    p.rate = modValue(m_raw.rate);
    p.fill = modValue(m_raw.fill);
    p.chaos = modValue(m_raw.chaos);
    for (int b = 0; b < RigEngine::c_maxBuses; ++b)
    {
        auto get = [&](int k) { return m_raw.bus[b][k] != nullptr ? modValue(m_raw.bus[b][k]) : 0.0f; };
        auto& d = p.buses[static_cast<size_t>(b)];
        d.doppler = get(ParamIDs::Doppler);
        d.width = get(ParamIDs::Width);
        d.bend = get(ParamIDs::Bend);
        d.low = get(ParamIDs::Low);
        d.swirl = get(ParamIDs::Swirl);
        d.filter = get(ParamIDs::Filter);
        d.drive = get(ParamIDs::Drive);
        d.duck = get(ParamIDs::Duck);
        d.key = get(ParamIDs::Key);
    }
    p.hitSense = modValue(m_raw.hitSense);
    p.hitBoost = modValue(m_raw.hitBoost);
    p.hitRelease = modValue(m_raw.hitRelease);
    p.hitDuck = modValue(m_raw.hitDuck);
    p.hitSync = m_raw.hitSync->load() > 0.5f;
    p.gateMode = juce::jlimit(0, 3, juce::roundToInt(m_raw.gateMode->load()));
    p.hitHold = modValue(m_raw.hitHold);
    p.hitFloor = modValue(m_raw.hitFloor);
    p.duckMode = juce::jlimit(0, 2, juce::roundToInt(m_raw.duckMode->load()));
    p.duckReturn = modValue(m_raw.duckReturn);
    p.takeTurns = m_raw.takeTurns->load() > 0.5f;
    p.linkBuses = m_raw.linkBuses->load() > 0.5f;
    p.space = modValue(m_raw.space);
    p.morphOn = m_raw.morphOn->load() > 0.5f;
    p.morphMode = juce::roundToInt(m_raw.morphMode->load());
    p.morphX = m_raw.morphX->load();
    p.morphY = m_raw.morphY->load();

    p.swell = m_uiSwell.load() || m_midiNotesHeld > 0;

    m_engine.setParams(p);

    MasterProcessor::Parameters mp;
    mp.inflateEffect = modValue(m_raw.inflate);
    mp.inflateCurve = 0.0f;
    mp.limiterOn = true;
    mp.ceilingDb = modValue(m_raw.ceiling);
    m_pushDb = modValue(m_raw.push);
    mp.releaseMs = 80.0f;
    m_master.setParameters(mp);
}

void SwooshbucklerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n == 0)
        return;

    // Stopped: fade out, then rest (the engine isn't run, so nothing starts blasting when the app opens).
    const bool playing = m_playing.load();
    if (! playing && m_transportGain <= 0.0f)
    {
        // Keep the modulator scopes, knob dots and pulled-wire glides moving while stopped (the output is silent).
        m_followerLevels = {};
        runModulators(n);
        publishModViews();
        // Note-offs still count, so a key released while stopped can't leave Swell stuck on.
        for (const auto meta : midi)
        {
            const auto msg = meta.getMessage();
            if (msg.isNoteOff())
                m_midiNotesHeld = juce::jmax(0, m_midiNotesHeld - 1);
            else if (msg.isAllNotesOff() || msg.isAllSoundOff())
                m_midiNotesHeld = 0;
        }
        buffer.clear();
        m_outputPeak.store(m_outputPeak.load(std::memory_order_relaxed) * 0.9f, std::memory_order_relaxed);
        return;
    }

    if (m_restartPending.exchange(false))
        m_engine.restart();

    // Hosts may send more than the prepared block size (offline / freewheel). Never reallocate here: work in
    // sections of the prepared size instead.
    // Sections are also capped so the modulators (and the parameters they move) update at least every 512 samples,
    // so an offline bounce with huge blocks sounds like realtime playback.
    const int maxSection = juce::jmin(m_work.getNumSamples(), 512);
    if (maxSection <= 0)
    {
        buffer.clear();
        return;
    }

    const int inputChannels = getTotalNumInputChannels();
    const int outChannels = buffer.getNumChannels();
    float* outL = m_work.getWritePointer(0);
    float* outR = m_work.getWritePointer(1);
    for (int start = 0; start < n; start += maxSection)
    {
        const int len = juce::jmin(maxSection, n - start);
        runModulators(len);
        updateEngineParams();

        // Note-ons start their whoosh at their own sample offset (to the engine's 32-sample chunk).
        for (const auto meta : midi)
        {
            if (meta.samplePosition < start || meta.samplePosition >= start + len)
                continue;
            const auto msg = meta.getMessage();
            if (msg.isNoteOn())
                ++m_midiNotesHeld;
            else if (msg.isNoteOff())
                m_midiNotesHeld = juce::jmax(0, m_midiNotesHeld - 1);
            else if (msg.isAllNotesOff() || msg.isAllSoundOff())
                m_midiNotesHeld = 0;
        }

        const float* inL = inputChannels > 0 ? buffer.getReadPointer(0, start) : nullptr;
        const float* inR = inputChannels > 1 ? buffer.getReadPointer(1, start) : nullptr;

        if (m_renderRate <= m_deviceRate + 0.5)
        {
            renderAtRenderRate(inL, inR, outL, outR, len);
        }
        else
        {
            // Render ahead at the high rate into a FIFO, then downsample exactly `len` samples for the device.
            // (The audio input is at the device rate, so it isn't used as a source while rendering high.)
            const double ratio = m_renderRate / m_deviceRate;
            const int needed = static_cast<int>(std::ceil(len * ratio)) + 8;
            while (m_fifoCount < needed)
            {
                const int chunk = juce::jmin(256, m_fifo.getNumSamples() - m_fifoCount);
                if (chunk <= 0)
                    break;
                renderAtRenderRate(nullptr, nullptr, m_fifo.getWritePointer(0, m_fifoCount), m_fifo.getWritePointer(1, m_fifoCount), chunk);
                m_fifoCount += chunk;
            }
            int used = 0;
            for (int ch = 0; ch < 2; ++ch)
                used = m_down[static_cast<size_t>(ch)].process(ratio, m_fifo.getReadPointer(ch), ch == 0 ? outL : outR, len);
            used = juce::jlimit(0, m_fifoCount, used);
            for (int ch = 0; ch < 2; ++ch)
            {
                float* d = m_fifo.getWritePointer(ch);
                std::memmove(d, d + used, sizeof(float) * static_cast<size_t>(m_fifoCount - used));
            }
            m_fifoCount -= used;
        }

        // Transport fade
        {
            const float step = static_cast<float>(1.0 / (0.06 * juce::jmax(1.0, m_deviceRate)));
            const float target = m_playing.load() ? 1.0f : 0.0f;
            for (int i = 0; i < len; ++i)
            {
                m_transportGain = target > m_transportGain ? juce::jmin(target, m_transportGain + step)
                                                           : juce::jmax(target, m_transportGain - step);
                outL[i] *= m_transportGain;
                outR[i] *= m_transportGain;
            }
        }

        // Output
        if (outChannels >= 2)
        {
            buffer.copyFrom(0, start, m_work, 0, 0, len);
            buffer.copyFrom(1, start, m_work, 1, 0, len);
            for (int ch = 2; ch < outChannels; ++ch)
                buffer.clear(ch, start, len);
        }
        else if (outChannels == 1)
        {
            buffer.copyFrom(0, start, m_work, 0, 0, len);
            buffer.addFrom(0, start, m_work, 1, 0, len);
            buffer.applyGain(0, start, len, 0.5f);
        }

        // Scope + meter
        int w = m_scopeWrite.load(std::memory_order_relaxed);
        float peak = 0.0f;
        double sumSquares = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const float v = 0.5f * (outL[i] + outR[i]);
            m_outRing.push(v);
            sumSquares += 0.5 * (static_cast<double>(outL[i]) * outL[i] + static_cast<double>(outR[i]) * outR[i]);
            m_scope[static_cast<size_t>(w)] = v;
            w = (w + 1) & (c_scopeSize - 1);
            peak = juce::jmax(peak, std::abs(outL[i]), std::abs(outR[i]));
        }
        m_scopeWrite.store(w, std::memory_order_release);
        m_followerLevels[ModMatrix::FollowOutput] = static_cast<float>(std::sqrt(sumSquares / juce::jmax(1, len)));
        const auto sections = m_engine.takeSectionLevels();
        m_followerLevels[ModMatrix::FollowBusA] = sections[0];
        m_followerLevels[ModMatrix::FollowBusB] = sections[1];
        m_followerLevels[ModMatrix::FollowHits] = sections[2];
        m_outputPeak.store(juce::jmax(peak, m_outputPeak.load(std::memory_order_relaxed) * 0.9f), std::memory_order_relaxed);
    }
}

void SwooshbucklerAudioProcessor::copyScope(float* dest, int count) const
{
    count = juce::jmin(count, c_scopeSize);
    const int w = m_scopeWrite.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
        dest[i] = m_scope[static_cast<size_t>((w - count + i) & (c_scopeSize - 1))];
}

//==============================================================================
void SwooshbucklerAudioProcessor::newSession()
{
    setPlaying(false);
    if (isRecording())
        stopRecording();
    for (int t = 0; t < CargoHold::c_numTracks; ++t)
    {
        m_cargo.clear(t);
        m_cargo.rename(t, {});
        m_cargo.setRoleOverride(t, -1);
        m_cargo.setStart(t, 0.0f);
    }
    for (int lane = 0; lane < CargoHold::c_numLanes; ++lane)
    {
        m_cargo.setMute(lane, false);
        m_cargo.setSolo(lane, false);
    }
    for (auto* p : getParameters())
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*>(p))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost(param->getDefaultValue());
            param->endChangeGesture();
        }
    m_mods.clear();
    currentPresetName = "Default";
}

bool SwooshbucklerAudioProcessor::arm(juce::String& error)
{
    m_recorder.setSignalGate(true);
    return startRecording(error);
}

bool SwooshbucklerAudioProcessor::startRecording(juce::String& error)
{
    TapeRecorder::Options o;
    o.bitDepth = getBitDepth();
    o.fileRate = static_cast<double>(getRecordRate()); // 0 = the session rate
    o.type = static_cast<TapeRecorder::FileType>(getFileType());
    o.mono = getMono();
    return m_recorder.start(getRecordFolder(), o, error);
}

int SwooshbucklerAudioProcessor::getRecordRate() const
{
    const int rate = m_settings->getIntValue("recordRate", 0);
    switch (rate)
    {
        case 44100: case 48000: case 88200: case 96000: case 176400: case 192000: return rate;
        default: return 0;
    }
}

void SwooshbucklerAudioProcessor::setRecordRate(int rate)
{
    m_settings->setValue("recordRate", rate);
    m_settings->saveIfNeeded();
    if (m_deviceRate > 0.0 && ! isRecording())
    {
        // Re-render at the new rate (the tape stays closed while it changes).
        suspendProcessing(true);
        prepareInternal();
        suspendProcessing(false);
    }
}

int SwooshbucklerAudioProcessor::getFileType() const { return juce::jlimit(0, 2, m_settings->getIntValue("fileType", 0)); }
void SwooshbucklerAudioProcessor::setFileType(int type) { m_settings->setValue("fileType", juce::jlimit(0, 2, type)); m_settings->saveIfNeeded(); }
bool SwooshbucklerAudioProcessor::getMono() const { return m_settings->getBoolValue("mono", false); }
void SwooshbucklerAudioProcessor::setMono(bool mono) { m_settings->setValue("mono", mono); m_settings->saveIfNeeded(); }

void SwooshbucklerAudioProcessor::stopRecording()
{
    m_recorder.stop();
}

bool SwooshbucklerAudioProcessor::saveRetro(juce::File& written, juce::String& error)
{
    return m_recorder.saveRetro(getRecordFolder(), getBitDepth(), written, error);
}

juce::File SwooshbucklerAudioProcessor::getRecordFolder() const
{
    const auto path = m_settings->getValue("recordFolder");
    const juce::File folder = path.isNotEmpty() ? juce::File(path) : TapeRecorder::defaultFolder();
    return folder;
}

void SwooshbucklerAudioProcessor::setRecordFolder(const juce::File& folder)
{
    m_settings->setValue("recordFolder", folder.getFullPathName());
    m_settings->saveIfNeeded();
}

int SwooshbucklerAudioProcessor::getBitDepth() const
{
    const int bits = m_settings->getIntValue("bitDepth", 24);
    return (bits == 16 || bits == 24 || bits == 32) ? bits : 24;
}

void SwooshbucklerAudioProcessor::setBitDepth(int bits)
{
    m_settings->setValue("bitDepth", bits);
    m_settings->saveIfNeeded();
}

bool SwooshbucklerAudioProcessor::getAutoRecord() const
{
    return m_settings->getBoolValue("autoRecord", false);
}

void SwooshbucklerAudioProcessor::setAutoRecord(bool on)
{
    m_settings->setValue("autoRecord", on);
    m_settings->saveIfNeeded();
}

void SwooshbucklerAudioProcessor::timerCallback()
{
    // Mirror the editor fields for host saves from other threads (they are message-thread objects).
    {
        const juce::ScopedLock sl(m_stateLock);
        m_mirrorWidth = savedEditorWidth;
        m_mirrorPreset = currentPresetName;
    }

    // Auto-roll the tape once audio is running (message thread, never the audio thread).
    if (! m_autoRecordDone && getSampleRate() > 0.0)
    {
        m_autoRecordDone = true;
        if (getAutoRecord() && ! isRecording())
        {
            juce::String error;
            arm(error);
        }
    }
}

//==============================================================================
double SwooshbucklerAudioProcessor::getRecordingDroppedSeconds() const
{
    const double rate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    return static_cast<double>(m_recorder.getDroppedSamples()) / rate;
}

void SwooshbucklerAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = m_state.copyState();
    state.removeChild(state.getChildWithName("Cargo"), nullptr);
    state.removeChild(state.getChildWithName("Mods"), nullptr);
    state.appendChild(m_mods.toValueTree(), nullptr); // the wires (thread safe snapshot)

    juce::ValueTree cargo;
    int width = 0;
    juce::String preset;
    {
        const juce::ScopedLock sl(m_stateLock);
        if (m_pending.valid)
        {
            // A restore that hasn't reached the message thread yet: save what was restored, not the old cargo.
            cargo = m_pending.cargo.createCopy();
            width = m_pending.editorWidth;
            preset = m_pending.presetName;
        }
        else
        {
            width = m_mirrorWidth;
            preset = m_mirrorPreset;
        }
    }

    if (! cargo.isValid())
    {
        if (juce::MessageManager::existsAndIsCurrentThread())
        {
            width = savedEditorWidth;
            preset = currentPresetName;
        }
        cargo = m_cargo.toValueTree(); // snapshots under CargoHold's lock: safe from a host save thread
    }

    state.setProperty("editorWidth", width, nullptr);
    state.setProperty("presetName", preset, nullptr);
    state.removeProperty("throwPath", nullptr); // v2 sessions
    state.appendChild(cargo, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void SwooshbucklerAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr)
        return;

    auto tree = juce::ValueTree::fromXml(*xml);
    if (! tree.hasType(m_state.state.getType()))
        return;

    const int width = tree.getProperty("editorWidth", 0);
    const juce::String preset = tree.getProperty("presetName", "Default").toString();
    tree.removeProperty("throwPath", nullptr); // v2 sessions

    // The wires (a state without them has none).
    {
        auto mods = tree.getChildWithName("Mods");
        m_mods.fromValueTree(mods.isValid() ? mods.createCopy() : juce::ValueTree("Mods"));
        if (mods.isValid())
            tree.removeChild(mods, nullptr);
    }

    // A state without cargo (host default state, parameters only) clears the sample lanes.
    auto cargoChild = tree.getChildWithName("Cargo");
    const auto cargo = cargoChild.isValid() ? cargoChild.createCopy() : juce::ValueTree("Cargo");
    if (cargoChild.isValid())
        tree.removeChild(cargoChild, nullptr);

    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        m_state.replaceState(tree);
        applyRestoredState(cargo, width, preset);
        return;
    }

    // Off the message thread: CargoHold and the editor fields are message-thread objects. Keep the state pending
    // (saves return it until applied) and apply it on the message thread, guarded against our destruction.
    {
        const juce::ScopedLock sl(m_stateLock);
        m_pending.valid = true;
        m_pending.id = ++m_pendingCounter;
        m_pending.cargo = cargo;
        m_pending.editorWidth = width;
        m_pending.presetName = preset;
    }
    m_state.replaceState(tree);

    juce::WeakReference<SwooshbucklerAudioProcessor> weak(this);
    juce::MessageManager::callAsync([weak]
    {
        if (auto* self = weak.get())
            self->applyPendingState();
    });
}

void SwooshbucklerAudioProcessor::applyPendingState()
{
    PendingState pending;
    {
        const juce::ScopedLock sl(m_stateLock);
        if (! m_pending.valid)
            return;
        pending = m_pending;
    }

    applyRestoredState(pending.cargo, pending.editorWidth, pending.presetName);

    // Only now may saves read the live cargo again (unless a newer restore arrived meanwhile).
    const juce::ScopedLock sl(m_stateLock);
    if (m_pending.valid && m_pending.id == pending.id)
        m_pending = PendingState {};
}

void SwooshbucklerAudioProcessor::applyRestoredState(const juce::ValueTree& cargo, int editorWidth, const juce::String& presetName)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    m_mods.notifyChanged(); // the wires were restored (possibly off the message thread): redraw them
    savedEditorWidth = editorWidth;
    // Sessions saved before the preset names were Title Case ("Steady energy"): show today's spelling.
    currentPresetName = presetName;
    for (const auto& preset : Presets::factory())
        if (preset.name.equalsIgnoreCase(presetName))
            currentPresetName = preset.name;
    {
        const juce::ScopedLock sl(m_stateLock);
        m_mirrorWidth = editorWidth;
        m_mirrorPreset = presetName;
    }
    m_cargo.fromValueTree(cargo);
    stateGeneration.fetch_add(1);
}

std::function<void()> SwooshbucklerAudioProcessor::showAudioSettings;

juce::AudioProcessorEditor* SwooshbucklerAudioProcessor::createEditor()
{
    return new SwooshbucklerAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SwooshbucklerAudioProcessor();
}
