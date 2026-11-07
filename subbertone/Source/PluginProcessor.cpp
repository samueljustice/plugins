#include "PluginProcessor.h"

#include "PluginEditor.h"

namespace
{
    // Host-facing value text. These match the editor's knob readouts, so generic
    // host editors, automation lanes and rack recalls (e.g. Soundminer) show the
    // same values as the plugin UI.
    juce::String signedOneDecimal(float v, const char* unit)
    {
        const float rounded = std::round(v * 10.0f) / 10.0f;
        if (std::abs(rounded) < 0.05f)
            return juce::String("0.0") + unit;
        return (rounded > 0.0f ? "+" : "") + juce::String(rounded, 1) + unit;
    }

    float parseNumber(const juce::String& text)
    {
        const float value = text.retainCharacters("0123456789.-").getFloatValue();
        return text.containsIgnoreCase("k") ? value * 1000.0f : value;
    }

    juce::AudioParameterFloatAttributes hertz()
    {
        return juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return v >= 1000.0f ? juce::String(v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz"
                                                                                : juce::String(juce::roundToInt(v)) + " Hz"; })
            .withValueFromStringFunction(parseNumber);
    }

    juce::AudioParameterFloatAttributes percent()
    {
        return juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return juce::String(juce::roundToInt(v)) + "%"; })
            .withValueFromStringFunction(parseNumber);
    }

    juce::AudioParameterFloatAttributes decibels()
    {
        return juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return signedOneDecimal(v, " dB"); })
            .withValueFromStringFunction(parseNumber);
    }

    juce::AudioParameterFloatAttributes octaves()
    {
        return juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return signedOneDecimal(v, " oct"); })
            .withValueFromStringFunction(parseNumber);
    }

    juce::AudioParameterFloatAttributes milliseconds()
    {
        return juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return v >= 1000.0f ? juce::String(v / 1000.0f, 2) + " s"
                                                                                : juce::String(juce::roundToInt(v)) + " ms"; })
            .withValueFromStringFunction([](const juce::String& t) { const float v = parseNumber(t);
                                                                      return t.containsIgnoreCase("ms") ? v : (t.trim().endsWithIgnoreCase("s") ? v * 1000.0f : v); });
    }

    const juce::Identifier c_stateVersion  { "stateVersion" };
    const juce::Identifier c_pluginVersion { "pluginVersion" };
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SubbertoneAudioProcessor();
}

SubbertoneAudioProcessor::SubbertoneAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor(BusesProperties()
                     #if !JucePlugin_IsMidiEffect
                      #if !JucePlugin_IsSynth
                       .withInput("Input", juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
#endif
      , m_parameters(*this, nullptr, juce::Identifier("SubbertoneParameters"), createParameterLayout())
      , m_presetManager(m_parameters, [this](const juce::AudioProcessorListener::ChangeDetails& details) { m_hostDisplay.post(details); })
{
    m_inputVisualBuffer.assign(c_visualBufferSize, 0.0f);
    m_outputVisualBuffer.assign(c_visualBufferSize, 0.0f);
    m_harmonicResidualVisualBuffer.assign(c_visualBufferSize, 0.0f);

    m_mixParam              = m_parameters.getRawParameterValue("mix");
    m_distortionParam       = m_parameters.getRawParameterValue("distortion");
    m_distortionTypeParam   = m_parameters.getRawParameterValue("distortionType");
    m_toneParam             = m_parameters.getRawParameterValue("distortionTone");
    m_postDriveLowpassParam = m_parameters.getRawParameterValue("postDriveLowpass");
    m_outputGainParam       = m_parameters.getRawParameterValue("outputGain");
    m_pitchThresholdParam   = m_parameters.getRawParameterValue("pitchThreshold");
    m_fundamentalLimitParam = m_parameters.getRawParameterValue("fundamentalLimit");
    m_filterResonanceParam  = m_parameters.getRawParameterValue("filterResonance");
    m_filterModeParam       = m_parameters.getRawParameterValue("filterMode");
    m_filterEnvAmountParam  = m_parameters.getRawParameterValue("filterEnvAmount");
    m_filterEnvReleaseParam = m_parameters.getRawParameterValue("filterEnvRelease");
    m_pitchGlideParam       = m_parameters.getRawParameterValue("pitchGlide");
    m_overtoneAmountParam   = m_parameters.getRawParameterValue("overtoneAmount");
    for (int k = 1; k <= 16; ++k)
        m_harmonicParams[static_cast<size_t>(k)] = m_parameters.getRawParameterValue("harmonic" + juce::String(k));
    m_inflateEffectParam    = m_parameters.getRawParameterValue("inflateEffect");
    m_inflateCurveParam     = m_parameters.getRawParameterValue("inflateCurve");
    m_limiterOnParam        = m_parameters.getRawParameterValue("limiterOn");
    m_limiterCeilingParam   = m_parameters.getRawParameterValue("limiterCeiling");
    m_limiterReleaseParam   = m_parameters.getRawParameterValue("limiterRelease");
    m_loCutOnParam          = m_parameters.getRawParameterValue("loCutOn");
    m_loCutFreqParam        = m_parameters.getRawParameterValue("loCutFreq");
}

juce::AudioProcessorValueTreeState::ParameterLayout SubbertoneAudioProcessor::createParameterLayout()
{
    // IDs, ranges, defaults and order are unchanged from earlier versions so
    // saved sessions and automation keep mapping to the same parameters.
    using Float = juce::AudioParameterFloat;
    using Choice = juce::AudioParameterChoice;
    using Range = juce::NormalisableRange<float>;

    juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout
    {
        std::make_unique<Float>(juce::ParameterID("mix", 0), "Mix", Range(0.0f, 100.0f), 50.0f, percent()),

        std::make_unique<Float>(juce::ParameterID("distortion", 0), "Drive", Range(0.0f, 100.0f), 50.0f, percent()),

        std::make_unique<Choice>(juce::ParameterID("distortionType", 0), "Drive Type",
                                 juce::StringArray{ "Tape", "Valve", "Console", "Transformer", "Rectifier", "Wavefolder" }, 0),

        std::make_unique<Float>(juce::ParameterID("distortionTone", 1), "Filter Cutoff", Range(20.0f, 20000.0f, 0.1f, 0.5f), 1000.0f, hertz()),

        std::make_unique<Float>(juce::ParameterID("filterResonance", 1), "Filter Resonance", Range(0.0f, 100.0f, 0.1f), 0.0f, percent()),

        std::make_unique<Choice>(juce::ParameterID("filterMode", 1), "Filter Mode",
                                 juce::StringArray{ "Lowpass", "Bandpass", "Notch" }, 0),

        std::make_unique<Float>(juce::ParameterID("filterEnvAmount", 1), "Filter Env Amount", Range(-4.0f, 4.0f, 0.01f), 0.0f, octaves()),

        std::make_unique<Float>(juce::ParameterID("filterEnvRelease", 1), "Filter Env Release", Range(5.0f, 2000.0f, 0.1f, 0.35f), 150.0f, milliseconds()),

        std::make_unique<Float>(juce::ParameterID("postDriveLowpass", 1), "Hi Cut", Range(20.0f, 20000.0f, 0.1f, 0.5f), 20000.0f, hertz()),

        std::make_unique<Float>(juce::ParameterID("outputGain", 0), "Output Gain", Range(-24.0f, 24.0f), 0.0f, decibels()),

        std::make_unique<Float>(juce::ParameterID("pitchThreshold", 0), "Threshold", Range(-60.0f, -20.0f), -40.0f, decibels()),

        std::make_unique<Float>(juce::ParameterID("fundamentalLimit", 0), "Max Pitch", Range(100.0f, 800.0f), 250.0f, hertz()),

        // Added in 1.2.0 (appended so earlier parameters keep their positions). The default is
        // 1.1.0's fixed glide, so 1.1.0 sessions (which have no pitchGlide) recall unchanged.
        std::make_unique<Float>(juce::ParameterID("pitchGlide", 2), "Glide", Range(5.0f, 500.0f, 0.1f, 0.4f), 12.0f, milliseconds()),

        std::make_unique<Float>(juce::ParameterID("overtoneAmount", 2), "Overtones", Range(0.0f, 100.0f, 0.1f), 0.0f, percent())

    };

    // Harmonic editor: H1 is the sub itself, H2..H16 its harmonics (relative to the sub).
    static constexpr float defaults[17] { 0.0f, 100.0f, 60.0f, 40.0f, 30.0f, 24.0f, 20.0f };
    for (int k = 1; k <= 16; ++k)
        parameterLayout.add(std::make_unique<Float>(juce::ParameterID("harmonic" + juce::String(k), 2),
                                                    k == 1 ? juce::String("Sub Level (H1)") : "Harmonic H" + juce::String(k),
                                                    Range(0.0f, 100.0f, 0.1f), defaults[k], percent()));

    // Master output stage: Inflator-style density, then a look-ahead brickwall limiter.
    parameterLayout.add(std::make_unique<Float>(juce::ParameterID("inflateEffect", 2), "Inflate", Range(0.0f, 100.0f, 0.1f), 0.0f, percent()));
    parameterLayout.add(std::make_unique<Float>(juce::ParameterID("inflateCurve", 2), "Inflate Curve", Range(-100.0f, 100.0f, 0.1f), 0.0f,
                                                juce::AudioParameterFloatAttributes()
                                                    .withStringFromValueFunction([](float v, int) { const int i = juce::roundToInt(v); return (i > 0 ? "+" : "") + juce::String(i); })
                                                    .withValueFromStringFunction(parseNumber)));
    parameterLayout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID("limiterOn", 2), "Limiter", false));
    parameterLayout.add(std::make_unique<Float>(juce::ParameterID("limiterCeiling", 2), "Limiter Ceiling", Range(-12.0f, 0.0f, 0.1f), -0.3f, decibels()));
    parameterLayout.add(std::make_unique<Float>(juce::ParameterID("limiterRelease", 2), "Limiter Release", Range(1.0f, 1000.0f, 0.1f, 0.4f), 100.0f, milliseconds()));

    // Steep (48 dB/oct) low cut on the generated sub: subs below ~30 Hz mostly eat headroom.
    parameterLayout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID("loCutOn", 2), "Lo Cut", false));
    parameterLayout.add(std::make_unique<Float>(juce::ParameterID("loCutFreq", 2), "Lo Cut Frequency", Range(10.0f, 80.0f, 0.1f, 0.6f), 30.0f, hertz()));

    return parameterLayout;
}

juce::AudioProcessorEditor* SubbertoneAudioProcessor::createEditor()
{
    return new SubbertoneAudioProcessorEditor(*this);
}

bool SubbertoneAudioProcessor::hasEditor() const
{
    return true;
}

const juce::String SubbertoneAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool SubbertoneAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}

bool SubbertoneAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}

bool SubbertoneAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

double SubbertoneAudioProcessor::getTailLengthSeconds() const
{
    return 0.15; // voicing-gate release
}

// Host programs: the factory presets, plus a last "User Preset" program for every other
// sound (user presets, unsaved sounds, 1.1.0 sessions). User presets are not listed one by
// one, because hosts cache the program list and it would go stale as files change. Without
// the extra entry, every non-factory sound would report program 0, and a VST3 host could
// then never select "Init": JUCE only calls setCurrentProgram when the program changes.
int SubbertoneAudioProcessor::getNumPrograms()
{
    return PresetManager::getNumFactory() + 1;
}

int SubbertoneAudioProcessor::getCurrentProgram()
{
    const int factoryIndex = m_presetManager.getCurrentFactoryIndex();
    return factoryIndex >= 0 ? factoryIndex : PresetManager::getNumFactory();
}

void SubbertoneAudioProcessor::setCurrentProgram(int index)
{
    // Applied right away on the calling thread (hosts may call this from any thread, and JUCE's
    // AU wrapper does so synchronously for kAudioUnitProperty_PresentPreset), so the program
    // and the state reflect it as soon as this returns. "User Preset" itself does nothing.
    if (juce::isPositiveAndBelow(index, PresetManager::getNumFactory()))
        m_presetManager.loadFactoryPresetForHost(index);
    else if (index == PresetManager::getNumFactory() && getCurrentProgram() != index)
        // "User Preset" can't be chosen while a factory preset is current: tell the host, so its
        // program display snaps back. (JUCE's VST3 wrapper then sets its Program parameter to
        // getCurrentProgram(), which does not call setCurrentProgram again, so nothing reloads.)
        m_hostDisplay.post(juce::AudioProcessorListener::ChangeDetails().withProgramChanged(true));
}

const juce::String SubbertoneAudioProcessor::getProgramName(int index)
{
    // Names come from the immutable factory table, never from the (refreshable) preset list.
    if (juce::isPositiveAndBelow(index, PresetManager::getNumFactory()))
        return PresetManager::getFactoryPresetName(index);

    if (index == PresetManager::getNumFactory())
        return "User Preset";

    return {};
}

void SubbertoneAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName); // factory programs are read-only
}

//==============================================================================
void SubbertoneAudioProcessor::HostDisplayUpdater::post(const juce::AudioProcessorListener::ChangeDetails& details)
{
    const int flags = (details.programChanged ? c_programChanged : 0)
                    | (details.nonParameterStateChanged ? c_stateChanged : 0);

    if (flags == 0)
        return;

    // Always delivered from the message thread, after the current call returns: the JUCE 8
    // wrappers expect updateHostDisplay there (the VST3 one reads its Program parameter), and
    // it is never re-entered from inside a host call. Bursts (an editor resize) coalesce.
    m_flags.fetch_or(flags);
    triggerAsyncUpdate();
}

void SubbertoneAudioProcessor::HostDisplayUpdater::handleAsyncUpdate()
{
    const int flags = m_flags.exchange(0);

    if (flags != 0)
        m_processor.updateHostDisplay(juce::AudioProcessorListener::ChangeDetails()
                                          .withProgramChanged((flags & c_programChanged) != 0)
                                          .withNonParameterStateChanged((flags & c_stateChanged) != 0));
}

//==============================================================================
void SubbertoneAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // Everything needed for a full recall: every parameter, the current preset and its
    // edited flag, and the editor's view options. Built on a private copy of the tree.
    juce::ValueTree state = m_parameters.copyState();
    state.removeAllProperties(nullptr);

    // APVTS only writes a value once a parameter has changed; write every one, so a session
    // keeps its exact sound even if a later version changes a default.
    for (auto child : state)
    {
        if (!child.hasType("PARAM") || child.hasProperty("value"))
            continue;

        if (auto* value = m_parameters.getRawParameterValue(child.getProperty("id").toString()))
            child.setProperty("value", value->load(), nullptr);
    }

    state.setProperty(c_stateVersion, 2, nullptr);
    state.setProperty(c_pluginVersion, JucePlugin_VersionString, nullptr);

    {
        const juce::ScopedLock sl(m_viewLock);
        for (const auto& option : m_viewOptions)
            state.setProperty(option.name, option.value, nullptr);
    }

    m_presetManager.writeToState(state);

    if (std::unique_ptr<juce::XmlElement> xml { state.createXml() })
        copyXmlToBinary(*xml, destData);
}

void SubbertoneAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState == nullptr || !xmlState->hasTagName(m_parameters.state.getType()))
        return;

    // Everything that isn't a parameter is read from the XML (a private copy) first.
    const auto presetInfo = PresetManager::readStateInfo(*xmlState);

    juce::NamedValueSet viewOptions;
    for (int i = 0; i < xmlState->getNumAttributes(); ++i)
    {
        const auto name = xmlState->getAttributeName(i);
        const auto value = xmlState->getAttributeValue(i);

        // A non-finite number ("nan", "inf") counts as missing, so the editor uses its default.
        if (name.startsWith("view") && std::isfinite(value.getDoubleValue()))
            viewOptions.set(name, value);
    }

    {
        const juce::ScopedLock sl(m_viewLock);
        m_viewOptions = std::move(viewOptions);
    }

    // The parameter tree: PARAM children only. Non-finite values are dropped (that parameter
    // takes its default). Version 1 states (Subbertone 1.0/1.1) contain the same parameter
    // IDs, so they load as-is; parameters they lack get their defaults, which match 1.1.0.
    auto tree = juce::ValueTree::fromXml(*xmlState);
    tree.removeAllProperties(nullptr);

    for (auto child : tree)
        if (child.hasProperty("value") && !std::isfinite(static_cast<double>(child.getProperty("value"))))
            child.removeProperty("value", nullptr);

    m_presetManager.restoreState(presetInfo, [this, &tree] { m_parameters.replaceState(tree); });
}

bool SubbertoneAudioProcessor::getViewOption(const juce::Identifier& option, bool defaultValue) const
{
    const juce::ScopedLock sl(m_viewLock);

    if (const auto* value = m_viewOptions.getVarPointer(option))
        return static_cast<bool>(*value);

    return defaultValue;
}

void SubbertoneAudioProcessor::setViewOption(const juce::Identifier& option, bool value)
{
    setViewValue(option, value);
}

juce::var SubbertoneAudioProcessor::getViewValue(const juce::Identifier& option, const juce::var& defaultValue) const
{
    const juce::ScopedLock sl(m_viewLock);
    return m_viewOptions.getWithDefault(option, defaultValue);
}

void SubbertoneAudioProcessor::setViewValue(const juce::Identifier& option, const juce::var& value)
{
    if ((value.isDouble() || value.isInt() || value.isInt64()) && !std::isfinite(static_cast<double>(value)))
        return;

    {
        const juce::ScopedLock sl(m_viewLock);

        // Compared as text: values read back from a state are strings.
        if (const auto* existing = m_viewOptions.getVarPointer(option); existing != nullptr && existing->toString() == value.toString())
            return;

        m_viewOptions.set(option, value);
    }

    // Saved with the session, so the host should see the session as changed.
    m_hostDisplay.post(juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged(true));
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool SubbertoneAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
#else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

#if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif

    return true;
#endif
}
#endif

void SubbertoneAudioProcessor::releaseResources()
{

}

void SubbertoneAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);

    m_monoBuffer.assign(c_chunkSize, 0.0f);
    m_wetBuffer.assign(c_chunkSize, 0.0f);
    m_residualBuffer.assign(c_chunkSize, 0.0f);

    m_subharmonicEngine.setParameters(readEngineParameters());
    m_subharmonicEngine.prepare(sampleRate);

    // The limiter's look-ahead delays the whole output by a fixed amount, whether
    // or not it is switched on, so the reported latency never changes.
    const int numChannels = std::max(2, getTotalNumOutputChannels());
    m_master.setParameters(readMasterParameters());
    m_master.prepare(sampleRate, numChannels);
    setLatencySamples(m_master.getLatencySamples());

    // Bypass path: a delay of exactly the reported latency, per channel.
    m_dryDelayLength = std::max(0, getLatencySamples());
    m_dryDelay.assign(static_cast<size_t>(numChannels), std::vector<float>(static_cast<size_t>(m_dryDelayLength), 0.0f));
    m_dryChunk.assign(static_cast<size_t>(numChannels), std::vector<float>(c_chunkSize, 0.0f));
    m_channelPointers.assign(static_cast<size_t>(numChannels), nullptr);
    m_dryDelayPosition = 0;
    m_bypassFadeStep = static_cast<float>(1.0 / std::max(1.0, c_bypassFadeSeconds * (sampleRate > 0.0 ? sampleRate : 44100.0)));
    m_snapBypassFade = true;
    m_wasBypassed = false;
    m_resetRequestsHandled = m_resetRequests.load(std::memory_order_acquire); // this is a reset already

    constexpr double smoothingSeconds = 0.02;
    m_mixSmoothed.reset(sampleRate, smoothingSeconds);
    m_outputGainSmoothed.reset(sampleRate, smoothingSeconds);
    m_mixSmoothed.setCurrentAndTargetValue(m_mixParam->load() * 0.01f);
    m_outputGainSmoothed.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(m_outputGainParam->load()));

    m_currentFundamental.store(0.0f);
    m_currentSignalLevelDb.store(0.0f);
}

void SubbertoneAudioProcessor::reset()
{
    // May be called from any thread while audio is running: only count the request.
    m_resetRequests.fetch_add(1, std::memory_order_release);
}

MasterProcessor::Parameters SubbertoneAudioProcessor::readMasterParameters() const
{
    MasterProcessor::Parameters p;
    p.inflateEffect = m_inflateEffectParam->load() * 0.01f;
    p.inflateCurve  = m_inflateCurveParam->load() * 0.01f;
    p.limiterOn     = m_limiterOnParam->load() >= 0.5f;
    p.ceilingDb     = m_limiterCeilingParam->load();
    p.releaseMs     = m_limiterReleaseParam->load();
    return p;
}

SubharmonicEngine::Parameters SubbertoneAudioProcessor::readEngineParameters() const
{
    SubharmonicEngine::Parameters p;
    p.distortion       = m_distortionParam->load() * 0.01f;
    p.saturationType   = static_cast<int>(m_distortionTypeParam->load());
    p.toneHz           = m_toneParam->load();
    p.postLowpassHz    = m_postDriveLowpassParam->load();
    p.thresholdDb      = m_pitchThresholdParam->load();
    p.maxFundamentalHz = m_fundamentalLimitParam->load();
    p.resonance        = m_filterResonanceParam->load() * 0.01f;
    p.filterMode       = static_cast<int>(m_filterModeParam->load());
    p.envAmountOctaves = m_filterEnvAmountParam->load();
    p.envReleaseMs     = m_filterEnvReleaseParam->load();
    p.glideMs          = m_pitchGlideParam->load();
    p.overtoneAmount   = m_overtoneAmountParam->load() * 0.01f;
    for (int k = 1; k <= 16; ++k)
        p.harmonicLevels[static_cast<size_t>(k)] = m_harmonicParams[static_cast<size_t>(k)]->load() * 0.01f;
    p.loCutOn          = m_loCutOnParam->load() >= 0.5f;
    p.loCutHz          = m_loCutFreqParam->load();
    return p;
}

void SubbertoneAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    render(buffer, false);
}

void SubbertoneAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    render(buffer, true);
}

void SubbertoneAudioProcessor::handlePendingReset(bool bypassed)
{
    const auto requests = m_resetRequests.load(std::memory_order_acquire);
    const auto pending = requests - m_resetRequestsHandled;

    if (pending == 0)
        return;

    m_resetRequestsHandled = requests;

    // JUCE's AU wrapper resets the plug-in once each time bypass is switched on. That is not a
    // transport jump: the processed sound is fading out and keeps running underneath, and the
    // delayed input must continue without a gap. So one reset arriving together with entering
    // bypass is ignored; any further one (e.g. the host's own reset at transport start, after
    // bypass was switched on while stopped) is a real reset.
    if (bypassed && !m_wasBypassed && pending == 1)
        return;

    m_subharmonicEngine.setParameters(readEngineParameters());
    m_subharmonicEngine.reset();
    m_master.reset(); // the next setParameters() jumps straight to the current values

    for (auto& channel : m_dryDelay)
        std::fill(channel.begin(), channel.end(), 0.0f);
    m_dryDelayPosition = 0;

    m_mixSmoothed.setCurrentAndTargetValue(m_mixParam->load() * 0.01f);
    m_outputGainSmoothed.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(m_outputGainParam->load()));
    m_bypassFade = bypassed ? 1.0f : 0.0f;
}

void SubbertoneAudioProcessor::delayDry(const juce::AudioBuffer<float>& buffer, int start, int numSamples, int numInputs, int numChannels)
{
    for (int ch = 0; ch < numChannels; ++ch)
    {
        // A mono input feeding a stereo output is heard on both channels.
        const int source = ch < numInputs ? ch : (numInputs == 1 ? 0 : -1);
        const float* in = source >= 0 ? buffer.getReadPointer(source, start) : nullptr;
        float* out = m_dryChunk[static_cast<size_t>(ch)].data();

        if (m_dryDelayLength == 0)
        {
            for (int i = 0; i < numSamples; ++i)
                out[i] = in != nullptr ? in[i] : 0.0f;
            continue;
        }

        float* ring = m_dryDelay[static_cast<size_t>(ch)].data();
        int position = m_dryDelayPosition;

        for (int i = 0; i < numSamples; ++i)
        {
            out[i] = ring[position];
            ring[position] = in != nullptr ? in[i] : 0.0f;

            if (++position == m_dryDelayLength)
                position = 0;
        }
    }

    if (m_dryDelayLength > 0)
        m_dryDelayPosition = (m_dryDelayPosition + numSamples) % m_dryDelayLength;
}

void SubbertoneAudioProcessor::applyBypassFade(juce::AudioBuffer<float>& buffer, int start, int numSamples, int numChannels, float target)
{
    if (juce::exactlyEqual(m_bypassFade, target))
    {
        // Settled. Bypassed: the delayed input, bit for bit. Active: the processed output as is.
        if (target >= 1.0f)
            for (int ch = 0; ch < numChannels; ++ch)
                juce::FloatVectorOperations::copy(buffer.getWritePointer(ch, start), m_dryChunk[static_cast<size_t>(ch)].data(), numSamples);
        return;
    }

    // Fading: one weight per sample, shared by every channel (smoothstep of a linear ramp).
    float fade = m_bypassFade;
    for (int i = 0; i < numSamples; ++i)
    {
        fade = target > fade ? std::min(target, fade + m_bypassFadeStep) : std::max(target, fade - m_bypassFadeStep);
        m_fadeWeights[static_cast<size_t>(i)] = fade * fade * (3.0f - 2.0f * fade);
    }
    m_bypassFade = fade;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* out = buffer.getWritePointer(ch, start);
        const float* dry = m_dryChunk[static_cast<size_t>(ch)].data();

        for (int i = 0; i < numSamples; ++i)
        {
            const float w = m_fadeWeights[static_cast<size_t>(i)];
            out[i] = w >= 1.0f ? dry[i] : (w <= 0.0f ? out[i] : out[i] + w * (dry[i] - out[i]));
        }
    }
}

void SubbertoneAudioProcessor::render(juce::AudioBuffer<float>& buffer, bool bypassed)
{
    juce::ScopedNoDenormals noDenormals;

    const int numInputs  = getTotalNumInputChannels();
    const int numOutputs = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();
    const int numBufferChannels = buffer.getNumChannels();
    const int numChannels = std::min({ numInputs, numOutputs, numBufferChannels });

    for (int ch = numInputs; ch < std::min(numOutputs, numBufferChannels); ++ch)
        buffer.clear(ch, 0, numSamples);

    if (m_monoBuffer.size() < static_cast<size_t>(c_chunkSize))
    {
        buffer.clear(); // not prepared (hosts must call prepareToPlay first)
        return;
    }

    handlePendingReset(bypassed);

    if (m_snapBypassFade)
    {
        m_snapBypassFade = false;
        m_bypassFade = bypassed ? 1.0f : 0.0f;
    }

    if (numChannels <= 0 || numSamples <= 0)
    {
        m_wasBypassed = bypassed;
        return;
    }

    m_subharmonicEngine.setParameters(readEngineParameters());
    m_master.setParameters(readMasterParameters());
    m_mixSmoothed.setTargetValue(m_mixParam->load() * 0.01f);
    m_outputGainSmoothed.setTargetValue(juce::Decibels::decibelsToGain(m_outputGainParam->load()));

    // Output channels with a MASTER stage and a bypass path (every output channel in practice).
    const int numOutChannels = std::min({ numOutputs, numBufferChannels, static_cast<int>(m_dryDelay.size()) });
    const float bypassTarget = bypassed ? 1.0f : 0.0f;
    const float channelScale = 1.0f / static_cast<float>(numChannels);

    // Process in fixed-size chunks so any host block size works without allocation.
    for (int start = 0; start < numSamples; start += c_chunkSize)
    {
        const int n = std::min(c_chunkSize, numSamples - start);

        // The input through the bypass delay, before it is processed in place.
        delayDry(buffer, start, n, numInputs, numOutChannels);

        // Mono sum for analysis/generation
        for (int i = 0; i < n; ++i)
        {
            float sum = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                sum += buffer.getReadPointer(ch)[start + i];
            m_monoBuffer[static_cast<size_t>(i)] = sum * channelScale;
        }

        m_subharmonicEngine.process(m_monoBuffer.data(), m_wetBuffer.data(), m_residualBuffer.data(), n);

        // Mix: smoothers advance once per sample, shared by every channel.
        for (int i = 0; i < n; ++i)
        {
            const float mix  = m_mixSmoothed.getNextValue();
            const float gain = m_outputGainSmoothed.getNextValue();
            const float wet  = m_wetBuffer[static_cast<size_t>(i)];

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* data = buffer.getWritePointer(ch);
                const float dry = data[start + i];
                data[start + i] = gain * (dry * (1.0f - mix) + wet * mix);
            }
        }

        pushVisualSamples(m_monoBuffer.data(), buffer.getReadPointer(0) + start, m_residualBuffer.data(), n);

        if (numInputs == 1 && numOutputs >= 2 && numBufferChannels >= 2)
            buffer.copyFrom(1, start, buffer, 0, start, n);

        // MASTER: inflate + brickwall limiter on the final output (all channels, linked).
        for (int ch = 0; ch < numOutChannels; ++ch)
            m_channelPointers[static_cast<size_t>(ch)] = buffer.getWritePointer(ch, start);
        m_master.process(m_channelPointers.data(), numOutChannels, n);

        // Host bypass: crossfade to / from the delayed input.
        applyBypassFade(buffer, start, n, numOutChannels, bypassTarget);
    }

    m_gainReductionDb.store(m_master.getGainReductionDb(), std::memory_order_relaxed);

    m_currentFundamental.store(m_subharmonicEngine.getFundamental());

    for (int k = 1; k <= 16; ++k)
        m_harmonicActivity[static_cast<size_t>(k)].store(m_subharmonicEngine.getHarmonicActivity(k), std::memory_order_relaxed);
    m_currentSignalLevelDb.store(m_subharmonicEngine.getInputRms());

    m_wasBypassed = bypassed;
}

void SubbertoneAudioProcessor::pushVisualSamples(const float* input, const float* output, const float* residual, int numSamples)
{
    int pos = m_visualWritePos.load(std::memory_order_relaxed);

    for (int i = 0; i < numSamples; ++i)
    {
        m_inputVisualBuffer[static_cast<size_t>(pos)] = input[i];
        m_outputVisualBuffer[static_cast<size_t>(pos)] = output[i];
        m_harmonicResidualVisualBuffer[static_cast<size_t>(pos)] = residual[i];
        pos = (pos + 1) % c_visualBufferSize;
    }

    m_visualWritePos.store(pos, std::memory_order_release);
}

void SubbertoneAudioProcessor::copyVisualRing(const std::vector<float>& ring, std::vector<float>& dest) const
{
    if (dest.size() != static_cast<size_t>(c_visualBufferSize))
        dest.resize(c_visualBufferSize);

    const int readPos = m_visualWritePos.load(std::memory_order_acquire);

    for (int i = 0; i < c_visualBufferSize; ++i)
        dest[static_cast<size_t>(i)] = ring[static_cast<size_t>((readPos + i) % c_visualBufferSize)];
}

void SubbertoneAudioProcessor::getInputWaveform(std::vector<float>& dest) const
{
    copyVisualRing(m_inputVisualBuffer, dest);
}

void SubbertoneAudioProcessor::getOutputWaveform(std::vector<float>& dest) const
{
    copyVisualRing(m_outputVisualBuffer, dest);
}

void SubbertoneAudioProcessor::getHarmonicResidualWaveform(std::vector<float>& dest) const
{
    copyVisualRing(m_harmonicResidualVisualBuffer, dest);
}
