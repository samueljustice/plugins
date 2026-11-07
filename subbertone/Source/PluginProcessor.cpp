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
      , m_presetManager(m_parameters)
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

        // Added in 1.2.0 (appended so earlier parameters keep their positions).
        std::make_unique<Float>(juce::ParameterID("pitchGlide", 2), "Glide", Range(5.0f, 500.0f, 0.1f, 0.4f), 40.0f, milliseconds()),

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

// Factory presets are exposed to the host as programs. User presets are not,
// because hosts cache the program list and it would go stale as files change.
int SubbertoneAudioProcessor::getNumPrograms()
{
    return m_presetManager.getNumFactoryPresets();
}

int SubbertoneAudioProcessor::getCurrentProgram()
{
    const int index = m_presetManager.getCurrentIndex();
    return index < m_presetManager.getNumFactoryPresets() ? index : 0;
}

void SubbertoneAudioProcessor::setCurrentProgram(int index)
{
    if (!juce::isPositiveAndBelow(index, m_presetManager.getNumFactoryPresets()))
        return;

    // Preset loading sends parameter gestures, which belong on the message thread.
    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        m_presetManager.loadPreset(index);
    }
    else
    {
        juce::MessageManager::callAsync([safe = juce::WeakReference<SubbertoneAudioProcessor>(this), index]
        {
            if (safe != nullptr)
                safe->m_presetManager.loadPreset(index);
        });
    }
}

const juce::String SubbertoneAudioProcessor::getProgramName(int index)
{
    if (juce::isPositiveAndBelow(index, m_presetManager.getNumFactoryPresets()))
        return m_presetManager.getPresets()[index].name;

    return {};
}

void SubbertoneAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName); // factory programs are read-only
}

void SubbertoneAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // Everything needed for a full recall: every parameter, the preset name and
    // edited flag, and the editor's view options (stored on the state tree).
    juce::ValueTree state = m_parameters.copyState();
    state.setProperty(c_stateVersion, 2, nullptr);
    state.setProperty(c_pluginVersion, JucePlugin_VersionString, nullptr);
    m_presetManager.writeToState(state);

    if (std::unique_ptr<juce::XmlElement> xml { state.createXml() })
        copyXmlToBinary(*xml, destData);
}

void SubbertoneAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState == nullptr || !xmlState->hasTagName(m_parameters.state.getType()))
        return;

    // Version 1 states (Subbertone 1.0/1.1) contain the same parameter IDs, so
    // they load as-is; parameters they lack keep their defaults.
    m_parameters.replaceState(juce::ValueTree::fromXml(*xmlState));
    m_presetManager.restoreFromState(m_parameters.state);
}

bool SubbertoneAudioProcessor::getViewOption(const juce::Identifier& option, bool defaultValue) const
{
    return m_parameters.state.getProperty(option, defaultValue);
}

void SubbertoneAudioProcessor::setViewOption(const juce::Identifier& option, bool value)
{
    m_parameters.state.setProperty(option, value, nullptr);
}

juce::var SubbertoneAudioProcessor::getViewValue(const juce::Identifier& option, const juce::var& defaultValue) const
{
    return m_parameters.state.getProperty(option, defaultValue);
}

void SubbertoneAudioProcessor::setViewValue(const juce::Identifier& option, const juce::var& value)
{
    m_parameters.state.setProperty(option, value, nullptr);
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
    m_master.setParameters(readMasterParameters());
    m_master.prepare(sampleRate, std::max(2, getTotalNumOutputChannels()));
    setLatencySamples(m_master.getLatencySamples());

    constexpr double smoothingSeconds = 0.02;
    m_mixSmoothed.reset(sampleRate, smoothingSeconds);
    m_outputGainSmoothed.reset(sampleRate, smoothingSeconds);
    m_mixSmoothed.setCurrentAndTargetValue(m_mixParam->load() * 0.01f);
    m_outputGainSmoothed.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(m_outputGainParam->load()));

    m_currentFundamental.store(0.0f);
    m_currentSignalLevelDb.store(0.0f);
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
    juce::ScopedNoDenormals noDenormals;

    const int numInputs  = getTotalNumInputChannels();
    const int numOutputs = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();
    const int numChannels = std::min(numInputs, numOutputs);

    for (int ch = numInputs; ch < numOutputs; ++ch)
        buffer.clear(ch, 0, numSamples);

    if (numChannels <= 0 || numSamples <= 0)
        return;

    m_subharmonicEngine.setParameters(readEngineParameters());
    m_mixSmoothed.setTargetValue(m_mixParam->load() * 0.01f);
    m_outputGainSmoothed.setTargetValue(juce::Decibels::decibelsToGain(m_outputGainParam->load()));

    const float channelScale = 1.0f / static_cast<float>(numChannels);

    // Process in fixed-size chunks so any host block size works without allocation.
    for (int start = 0; start < numSamples; start += c_chunkSize)
    {
        const int n = std::min(c_chunkSize, numSamples - start);

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
    }

    if (numInputs == 1 && numOutputs >= 2)
        buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);

    // MASTER: inflate + brickwall limiter on the final output (all channels, linked).
    m_master.setParameters(readMasterParameters());
    m_master.process(buffer.getArrayOfWritePointers(), numOutputs, numSamples);
    m_gainReductionDb.store(m_master.getGainReductionDb(), std::memory_order_relaxed);

    m_currentFundamental.store(m_subharmonicEngine.getFundamental());

    for (int k = 1; k <= 16; ++k)
        m_harmonicActivity[static_cast<size_t>(k)].store(m_subharmonicEngine.getHarmonicActivity(k), std::memory_order_relaxed);
    m_currentSignalLevelDb.store(m_subharmonicEngine.getInputRms());
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
