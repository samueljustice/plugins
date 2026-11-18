#include "PluginProcessor.h"

#include "Parameters.h"
#include "PluginEditor.h"

namespace
{
    const juce::Identifier stateVersionAttr { "stateVersion" };

    juce::AudioParameterFloatAttributes percent()
    {
        return juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; })
            .withValueFromStringFunction ([] (const juce::String& s) { return s.retainCharacters ("-0123456789.").getFloatValue(); });
    }

    juce::AudioParameterFloatAttributes hertz()
    {
        return juce::AudioParameterFloatAttributes()
            .withLabel ("Hz")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)) + " Hz"; })
            .withValueFromStringFunction ([] (const juce::String& s) { return s.retainCharacters ("0123456789.").getFloatValue(); });
    }

    // A 0..100 control whose ends have names (Sharpness: Broad..Narrow, Speed: Slow..Fast).
    juce::AudioParameterFloatAttributes namedEnds (const char* low, const char* high)
    {
        const juce::String lowName (low), highName (high);
        return juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction ([lowName, highName] (float v, int)
            {
                const int n = juce::roundToInt (v);
                return n <= 0 ? lowName : n >= 100 ? highName : juce::String (n);
            })
            .withValueFromStringFunction ([lowName, highName] (const juce::String& s)
            {
                if (s.trim().equalsIgnoreCase (lowName))  return 0.0f;
                if (s.trim().equalsIgnoreCase (highName)) return 100.0f;
                return s.retainCharacters ("0123456789.").getFloatValue();
            });
    }

    // Where each speaker sits, for the core: the LFE is passed through untouched, and the rest
    // are folded down to stereo to measure the programme level the Threshold compares.
    dehonk::Position positionOf (juce::AudioChannelSet::ChannelType type) noexcept
    {
        using T = juce::AudioChannelSet;
        struct Entry { T::ChannelType type; dehonk::Position position; };
        static constexpr Entry table[] {
            { T::left,              dehonk::Position::leftFront },
            { T::right,             dehonk::Position::rightFront },
            { T::centre,            dehonk::Position::centre },
            { T::LFE,               dehonk::Position::lfe },
            { T::LFE2,              dehonk::Position::lfe },
            { T::leftSurround,      dehonk::Position::leftOther },
            { T::leftCentre,        dehonk::Position::leftOther },
            { T::leftSurroundSide,  dehonk::Position::leftOther },
            { T::leftSurroundRear,  dehonk::Position::leftOther },
            { T::wideLeft,          dehonk::Position::leftOther },
            { T::topFrontLeft,      dehonk::Position::leftOther },
            { T::topRearLeft,       dehonk::Position::leftOther },
            { T::topSideLeft,       dehonk::Position::leftOther },
            { T::rightSurround,     dehonk::Position::rightOther },
            { T::rightCentre,       dehonk::Position::rightOther },
            { T::rightSurroundSide, dehonk::Position::rightOther },
            { T::rightSurroundRear, dehonk::Position::rightOther },
            { T::wideRight,         dehonk::Position::rightOther },
            { T::topFrontRight,     dehonk::Position::rightOther },
            { T::topRearRight,      dehonk::Position::rightOther },
            { T::topSideRight,      dehonk::Position::rightOther },
            { T::centreSurround,    dehonk::Position::middleOther },
            { T::topMiddle,         dehonk::Position::middleOther },
            { T::topFrontCentre,    dehonk::Position::middleOther },
            { T::topRearCentre,     dehonk::Position::middleOther },
        };
        for (const auto& e : table)
            if (e.type == type)
                return e.position;
        return dehonk::Position::unknown;
    }

    dehonk::ChannelLayout coreLayoutFor (const juce::AudioChannelSet& set)
    {
        dehonk::ChannelLayout layout;
        const int n = juce::jmin (set.size(), dehonk::kMaxChannels);
        for (int ch = 0; ch < n; ++ch)
            layout.position[ch] = positionOf (set.getTypeOfChannel (ch));

        // The LFE, found by type, so it is right whatever order the host's format puts it in.
        for (auto type : { juce::AudioChannelSet::LFE, juce::AudioChannelSet::LFE2 })
        {
            const int index = set.getChannelIndexForType (type);
            if (juce::isPositiveAndBelow (index, n))
                layout.position[index] = dehonk::Position::lfe;
        }
        return layout;
    }

    // Every layout De-Honk runs on, input and output the same: mono and stereo, and the beds up
    // to 7.1.4 that Pro Tools and the other hosts offer.
    const std::vector<juce::AudioChannelSet>& supportedLayouts()
    {
        using S = juce::AudioChannelSet;
        static const std::vector<S> sets {
            S::mono(), S::stereo(),
            S::createLCR(), S::createLCRS(), S::quadraphonic(),
            S::create5point0(), S::create5point1(),
            S::create6point0(), S::create6point1(),
            S::create7point0(), S::create7point1(),
            S::create7point0SDDS(), S::create7point1SDDS(),
            S::create5point0point2(), S::create5point1point2(),
            S::create5point0point4(), S::create5point1point4(),
            S::create7point0point2(), S::create7point1point2(),
            S::create7point0point4(), S::create7point1point4(),
        };
        return sets;
    }
}

//==============================================================================
DeHonkAudioProcessor::DeHonkAudioProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "DeHonk", createParameterLayout()),
      presetManager (parameters, [this] (const juce::AudioProcessorListener::ChangeDetails& details)
                                 { updateHostDisplay (details); })
{
    thresholdValue = parameters.getRawParameterValue (ParamIDs::threshold);
    depthValue     = parameters.getRawParameterValue (ParamIDs::depth);
    sharpnessValue = parameters.getRawParameterValue (ParamIDs::sharpness);
    speedValue     = parameters.getRawParameterValue (ParamIDs::speed);
    maxCutValue    = parameters.getRawParameterValue (ParamIDs::maxCut);
    releaseValue   = parameters.getRawParameterValue (ParamIDs::release);
    impactGuardValue = parameters.getRawParameterValue (ParamIDs::impactGuard);
    for (int m = 0; m < dehonk::kNumModules; ++m)
        moduleOnValues[static_cast<size_t> (m)] = parameters.getRawParameterValue (ParamIDs::moduleOn[m]);
    clickOnValue   = parameters.getRawParameterValue (ParamIDs::snapOn);
    for (int e = 0; e < dehonk::kNumEdges; ++e)
        edgeValues[static_cast<size_t> (e)] = parameters.getRawParameterValue (ParamIDs::edges[e]);
    linkValue      = parameters.getRawParameterValue (ParamIDs::link);
    lookaheadValue = parameters.getRawParameterValue (ParamIDs::lookahead);
    mixValue       = parameters.getRawParameterValue (ParamIDs::mix);
    trimValue      = parameters.getRawParameterValue (ParamIDs::trim);
    bypassParameter = dynamic_cast<juce::AudioParameterBool*> (parameters.getParameter (ParamIDs::bypass));
    jassert (bypassParameter != nullptr);

    parameters.addParameterListener (ParamIDs::lookahead, this);

    // Picks up Lookahead changes made off the message thread (host automation).
    startTimerHz (10);
}

DeHonkAudioProcessor::~DeHonkAudioProcessor()
{
    stopTimer();
    parameters.removeParameterListener (ParamIDs::lookahead, this);
}

juce::AudioProcessorValueTreeState::ParameterLayout DeHonkAudioProcessor::createParameterLayout()
{
    using namespace ParamRanges;
    const auto id = [] (const char* name) { return juce::ParameterID { name, ParamIDs::versionHint }; };

    // The order here is the order hosts see. Once released, new parameters go at the end.
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (id (ParamIDs::threshold), "Threshold",
                    juce::NormalisableRange<float> (thresholdMin, thresholdMax, 0.1f), thresholdDefault,
                    juce::AudioParameterFloatAttributes()
                        .withLabel ("dBFS")
                        .withStringFromValueFunction ([] (float v, int)
                        {
                            return v <= thresholdAlwaysAtOrBelow ? juce::String ("Always")
                                                                : juce::String (v, 1) + " dBFS";
                        })
                        .withValueFromStringFunction ([] (const juce::String& s)
                        {
                            if (s.trim().equalsIgnoreCase ("Always"))
                                return thresholdAlways;
                            return s.retainCharacters ("-0123456789.").getFloatValue();
                        })));
    layout.add (std::make_unique<juce::AudioParameterFloat> (id (ParamIDs::depth), "Depth",
                    juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, percent()));
    layout.add (std::make_unique<juce::AudioParameterFloat> (id (ParamIDs::sharpness), "Sharpness",
                    juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), sharpnessDefault, namedEnds ("Broad", "Narrow")));
    layout.add (std::make_unique<juce::AudioParameterFloat> (id (ParamIDs::speed), "Speed",
                    juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, namedEnds ("Slow", "Fast")));

    for (int m = 0; m < dehonk::kNumModules; ++m)
        layout.add (std::make_unique<juce::AudioParameterBool> (id (ParamIDs::moduleOn[m]), moduleNames[m],
                                                                coreDefaults.moduleOn[m]));
    layout.add (std::make_unique<juce::AudioParameterBool> (id (ParamIDs::snapOn), "Click", coreDefaults.snapOn));

    for (int e = 0; e < dehonk::kNumEdges; ++e)
        layout.add (std::make_unique<juce::AudioParameterFloat> (id (ParamIDs::edges[e]), edgeNames[e],
                                                                 edgeRange (e), dehonk::kDefaultEdgesHz[e], hertz()));

    layout.add (std::make_unique<juce::AudioParameterFloat> (id (ParamIDs::link), "Link",
                    juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, percent()));
    layout.add (std::make_unique<juce::AudioParameterChoice> (id (ParamIDs::lookahead), "Lookahead",
                    juce::StringArray { "Off", "5 ms", "10 ms", "20 ms" }, 0));
    layout.add (std::make_unique<juce::AudioParameterFloat> (id (ParamIDs::mix), "Mix",
                    juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, percent()));
    layout.add (std::make_unique<juce::AudioParameterFloat> (id (ParamIDs::trim), "Trim",
                    juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
                    juce::AudioParameterFloatAttributes()
                        .withLabel ("dB")
                        .withStringFromValueFunction ([] (float v, int)
                        {
                            const auto rounded = std::round (v * 10.0f) / 10.0f;
                            return (rounded > 0.0f ? "+" : "") + juce::String (rounded, 1) + " dB";
                        })
                        .withValueFromStringFunction ([] (const juce::String& s)
                        { return s.retainCharacters ("-+0123456789.").getFloatValue(); })));
    layout.add (std::make_unique<juce::AudioParameterBool> (id (ParamIDs::bypass), "Bypass", false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (id (ParamIDs::maxCut), "Max Cut",
                    juce::NormalisableRange<float> (maxCutMin, maxCutMax, 0.1f), maxCutDefault,
                    juce::AudioParameterFloatAttributes()
                        .withLabel ("dB")
                        .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " dB"; })
                        .withValueFromStringFunction ([] (const juce::String& s)
                        { return s.retainCharacters ("0123456789.").getFloatValue(); })));
    layout.add (std::make_unique<juce::AudioParameterFloat> (id (ParamIDs::release), "Release",
                    releaseRange(), releaseDefault,
                    juce::AudioParameterFloatAttributes()
                        .withLabel ("ms")
                        .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)) + " ms"; })
                        .withValueFromStringFunction ([] (const juce::String& s)
                        {
                            // "1.2 s" as well as "1200 ms" or "1200".
                            const auto text = s.trim().toLowerCase();
                            const float number = text.retainCharacters ("0123456789.").getFloatValue();
                            return text.endsWithChar ('s') && ! text.endsWith ("ms") ? number * 1000.0f : number;
                        })));
    layout.add (std::make_unique<juce::AudioParameterFloat> (id (ParamIDs::impactGuard), "Impact Guard",
                    juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), impactGuardDefault,   // whole percent: what reads Off is 0
                    juce::AudioParameterFloatAttributes()
                        .withLabel ("%")
                        .withStringFromValueFunction ([] (float v, int)
                        {
                            const int n = juce::roundToInt (v);
                            return n <= 0 ? juce::String ("Off") : juce::String (n) + " %";
                        })
                        .withValueFromStringFunction ([] (const juce::String& s)
                        {
                            if (s.trim().equalsIgnoreCase ("Off"))
                                return 0.0f;
                            return s.retainCharacters ("0123456789.").getFloatValue();
                        })));

    return layout;
}

//==============================================================================
bool DeHonkAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    if (in != out || out.size() > dehonk::kMaxChannels)
        return false;

    // JUCE's AAX wrapper offers Pro Tools every stem format this accepts, so nothing else is
    // needed for the surround formats to appear there.
    const auto& sets = supportedLayouts();
    return std::find (sets.begin(), sets.end(), out) != sets.end();
}

void DeHonkAudioProcessor::processorLayoutsChanged()
{
    // Hosts change the layout only while not processing and call prepareToPlay afterwards.
    // Until they do, render() passes the audio through rather than run the core on a layout it
    // was not prepared for.
    layoutGeneration.fetch_add (1, std::memory_order_acq_rel);
}

void DeHonkAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int channels = juce::jlimit (1, dehonk::kMaxChannels, getTotalNumOutputChannels());
    const int blockSize = juce::jmax (1, samplesPerBlock);

    preparedLayoutGeneration = layoutGeneration.load (std::memory_order_acquire);
    core.prepare (sampleRate, blockSize, channels, coreLayoutFor (getChannelLayoutOfBus (false, 0)));
    core.setParams (readParams (false));

    preparedChannels = channels;
    preparedBlockSize = blockSize;
    preparedSampleRate = sampleRate;

    resetRequestsHandled = resetRequests.load();
    wasBypassed = false;
    samplesSinceBypassOn = 0;

    samplesPerDisplayFrame = juce::jmax (1, juce::roundToInt (sampleRate / displayFramesPerSecond));
    samplesSinceDisplayFrame = 0;
    lastFrameCounter = core.frameCounter();
    clickFrameSeen = lastFrameCounter;
    clickHeldDb = 0.0f;

    setLatencySamples (core.latencySamplesFor (ParamRanges::lookaheadMsForIndex (juce::roundToInt (lookaheadValue->load()))));
}

void DeHonkAudioProcessor::releaseResources() {}

void DeHonkAudioProcessor::reset()
{
    // May arrive on the UI thread while audio renders: the audio thread does the work.
    resetRequests.fetch_add (1, std::memory_order_acq_rel);
}

void DeHonkAudioProcessor::handlePendingReset (bool bypassed) noexcept
{
    const auto requests = resetRequests.load (std::memory_order_acquire);
    const auto pending = requests - resetRequestsHandled;

    if (pending == 0)
        return;

    resetRequestsHandled = requests;

    // JUCE's AU wrapper resets the plug-in each time bypass is switched on. That is not a
    // transport jump: the core is crossfading to the latency-matched dry signal and must keep
    // running, or the output would drop out. So one reset arriving within a moment of entering
    // bypass is ignored; anything else is a real reset.
    const bool justEnteredBypass = bypassed && samplesSinceBypassOn < juce::roundToInt (preparedSampleRate * 0.25);
    if (justEnteredBypass && pending == 1)
        return;

    core.reset();
}

//==============================================================================
dehonk::Params DeHonkAudioProcessor::readParams (bool bypassed) const noexcept
{
    dehonk::Params p;
    p.thresholdDb = juce::jlimit (ParamRanges::thresholdMin, ParamRanges::thresholdMax, thresholdValue->load());
    p.depth = depthValue->load() * 0.01f;
    p.sharpness = sharpnessValue->load() * 0.01f;
    p.speed = speedValue->load() * 0.01f;
    p.maxCutDb = juce::jlimit (ParamRanges::maxCutMin, ParamRanges::maxCutMax, maxCutValue->load());
    p.releaseMs = juce::jlimit (ParamRanges::releaseMin, ParamRanges::releaseMax, releaseValue->load());
    p.impactGuard = juce::jlimit (0.0f, 1.0f, impactGuardValue->load() * 0.01f);
    for (size_t m = 0; m < dehonk::kNumModules; ++m)
        p.moduleOn[m] = moduleOnValues[m]->load() >= 0.5f;
    p.snapOn = clickOnValue->load() >= 0.5f;
    for (size_t e = 0; e < dehonk::kNumEdges; ++e)
        p.edgeHz[e] = juce::jlimit (dehonk::kEdgeMinHz[e], dehonk::kEdgeMaxHz[e], edgeValues[e]->load());
    p.link = linkValue->load() * 0.01f;
    p.lookaheadMs = ParamRanges::lookaheadMsForIndex (juce::roundToInt (lookaheadValue->load()));
    p.mix = mixValue->load() * 0.01f;
    p.trimDb = trimValue->load();
    p.soloHonk = soloHonk.load (std::memory_order_relaxed);
    p.diff = diff.load (std::memory_order_relaxed);
    p.bypass = bypassed;
    return p;
}

void DeHonkAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, false);
}

void DeHonkAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, true);
}

void DeHonkAudioProcessor::render (juce::AudioBuffer<float>& buffer, bool hostBypassed) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), getTotalNumOutputChannels());

    // Not prepared, or the layout changed without a prepareToPlay: pass the audio through.
    if (preparedChannels == 0 || numChannels != preparedChannels || numSamples <= 0
        || layoutGeneration.load (std::memory_order_relaxed) != preparedLayoutGeneration)
        return;

    // The bypass parameter (every wrapper here uses it) or a host that bypasses through
    // processBlockBypassed: either way the core runs, so the output stays latency-matched and
    // the switch is crossfaded.
    const bool bypassed = hostBypassed || bypassParameter->get();

    if (bypassed && ! wasBypassed)
        samplesSinceBypassOn = 0;
    wasBypassed = bypassed;

    handlePendingReset (bypassed);

    const auto params = readParams (bypassed);
    core.setParams (params);

    // The core takes at most the prepared block size per call; while Click's meter is showing,
    // at most one detector tick per call, so no tick's click goes unseen.
    const bool watchClick = params.snapOn && editorsOpen.load (std::memory_order_relaxed) > 0;
    const int piece = watchClick ? juce::jmin (preparedBlockSize, clickPieceSamples) : preparedBlockSize;

    float* channels[dehonk::kMaxChannels] {};
    for (int start = 0; start < numSamples; start += piece)
    {
        const int n = juce::jmin (piece, numSamples - start);
        for (int ch = 0; ch < numChannels; ++ch)
            channels[ch] = buffer.getWritePointer (ch, start);

        core.process (channels, numChannels, n);
        if (watchClick)
            collectClick();
        pushDisplayFrame (n);
    }

    if (bypassed)
        samplesSinceBypassOn += numSamples;
}

void DeHonkAudioProcessor::collectClick() noexcept
{
    const auto counter = core.frameCounter();
    if (counter == clickFrameSeen)
        return;

    clickFrameSeen = counter;
    clickHeldDb = juce::jmax (clickHeldDb, core.displayFrame().snapCutDb);
}

void DeHonkAudioProcessor::pushDisplayFrame (int numSamples) noexcept
{
    if (editorsOpen.load (std::memory_order_relaxed) <= 0)
        return;  // no editor: nothing is written

    // Capped, so a long run with a full FIFO or an unchanged frame cannot overflow it.
    samplesSinceDisplayFrame = juce::jmin (samplesSinceDisplayFrame + numSamples, samplesPerDisplayFrame);
    if (samplesSinceDisplayFrame < samplesPerDisplayFrame)
        return;

    const auto counter = core.frameCounter();
    if (counter == lastFrameCounter)
        return;

    const auto scope = displayFifo.write (1);
    dehonk::DisplayFrame* slot = nullptr;
    if (scope.blockSize1 > 0)
        slot = &displayFrames[static_cast<size_t> (scope.startIndex1)];
    else if (scope.blockSize2 > 0)
        slot = &displayFrames[static_cast<size_t> (scope.startIndex2)];
    else
        return;  // full: the reader is behind; try again next block

    *slot = core.displayFrame();
    slot->snapCutDb = juce::jmax (slot->snapCutDb, clickHeldDb);
    clickHeldDb = 0.0f;
    lastFrameCounter = counter;
    samplesSinceDisplayFrame = 0;
}

void DeHonkAudioProcessor::setEditorOpen (bool open) noexcept
{
    editorsOpen.fetch_add (open ? 1 : -1);
}

bool DeHonkAudioProcessor::popLatestDisplayFrame (dehonk::DisplayFrame& latest) noexcept
{
    bool any = false;
    float click = 0.0f;   // a click lasts milliseconds: keep the deepest of every frame taken

    while (displayFifo.getNumReady() > 0)
    {
        const auto scope = displayFifo.read (1);
        if (scope.blockSize1 > 0)
            latest = displayFrames[static_cast<size_t> (scope.startIndex1)];
        else if (scope.blockSize2 > 0)
            latest = displayFrames[static_cast<size_t> (scope.startIndex2)];
        else
            break;

        click = juce::jmax (click, latest.snapCutDb);
        any = true;
    }

    if (any)
        latest.snapCutDb = click;
    return any;
}

//==============================================================================
void DeHonkAudioProcessor::parameterChanged (const juce::String& parameterID, float)
{
    if (parameterID != ParamIDs::lookahead)
        return;

    // Called on whichever thread changed the value. Latency is only reported from the message thread.
    if (juce::MessageManager::existsAndIsCurrentThread())
        updateReportedLatency();
    else
        latencyDirty.store (true);
}

void DeHonkAudioProcessor::timerCallback()
{
    if (latencyDirty.exchange (false))
        updateReportedLatency();

    if (programDisplayDirty.exchange (false))
        updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
}

void DeHonkAudioProcessor::updateReportedLatency()
{
    if (preparedSampleRate <= 0.0)
        return;  // prepareToPlay reports it

    const int samples = core.latencySamplesFor (ParamRanges::lookaheadMsForIndex (juce::roundToInt (lookaheadValue->load())));
    if (samples != getLatencySamples())
        setLatencySamples (samples);
}

//==============================================================================
// Host programs: the factory presets, then one "User preset" entry that stands for anything else
// (a user preset or edited settings). Without that entry the host would be told a factory preset
// is current when it is not, and JUCE's VST3 wrapper ignores a request for the program it was told
// is current, so choosing that factory preset from the host's list would do nothing.
int DeHonkAudioProcessor::getNumPrograms()
{
    return PresetManager::getNumFactory() + 1;
}

int DeHonkAudioProcessor::getCurrentProgram()
{
    const int factoryIndex = presetManager.getCurrentFactoryIndex();
    return factoryIndex >= 0 ? factoryIndex : PresetManager::getNumFactory();
}

void DeHonkAudioProcessor::setCurrentProgram (int index)
{
    if (juce::isPositiveAndBelow (index, PresetManager::getNumFactory()))
        presetManager.loadFactoryPresetForHost (index);
    else if (index == PresetManager::getNumFactory() && getCurrentProgram() != index)
        // "User preset" cannot be chosen while a factory preset is current. Tell the host later, from
        // the timer, so its program display snaps back without re-entering the wrapper from here.
        programDisplayDirty.store (true);
}

const juce::String DeHonkAudioProcessor::getProgramName (int index)
{
    if (juce::isPositiveAndBelow (index, PresetManager::getNumFactory()))
        return PresetManager::getFactoryName (index);

    return index == PresetManager::getNumFactory() ? juce::String ("User preset") : juce::String();
}

//==============================================================================
void DeHonkAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty (stateVersionAttr, stateVersion, nullptr);
    presetManager.writeToState (state);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void DeHonkAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // The listening tools are never recalled as on (SPEC 4.1).
    soloHonk.store (false);
    diff.store (false);

    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    presetManager.restoreState (*xml, [this, &xml]
    {
        parameters.replaceState (juce::ValueTree::fromXml (*xml));
    });

    if (juce::MessageManager::existsAndIsCurrentThread())
        updateReportedLatency();
    else
        latencyDirty.store (true);
}

//==============================================================================
juce::AudioProcessorEditor* DeHonkAudioProcessor::createEditor()
{
    return new DeHonkAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DeHonkAudioProcessor();
}
