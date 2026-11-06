#include "PluginProcessor.h"

#include "PluginEditor.h"

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
}

juce::AudioProcessorValueTreeState::ParameterLayout SubbertoneAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout
    {
        std::make_unique<juce::AudioParameterFloat>("mix", "Mix", 0.0f, 100.0f, 50.0f),

        std::make_unique<juce::AudioParameterFloat>("distortion", "Distortion", 0.0f, 100.0f, 50.0f),

        std::make_unique<juce::AudioParameterChoice>("distortionType", "Distortion Type", juce::StringArray{"Tape", "Valve", "Console", "Transformer", "Rectifier", "Wavefolder"}, 0),

        std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("distortionTone", 1), "Filter Cutoff", 
                                                    juce::NormalisableRange<float>(20.0f, 20000.0f, 0.1f, 0.5f), 1000.0f,
                                                    juce::AudioParameterFloatAttributes().withLabel("Hz")),

        std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("filterResonance", 1), "Filter Resonance",
                                                    juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 0.0f,
                                                    juce::AudioParameterFloatAttributes().withLabel("%")),

        std::make_unique<juce::AudioParameterChoice>(juce::ParameterID("filterMode", 1), "Filter Mode",
                                                     juce::StringArray{"Lowpass", "Bandpass", "Notch"}, 0),

        std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("filterEnvAmount", 1), "Filter Env Amount",
                                                    juce::NormalisableRange<float>(-4.0f, 4.0f, 0.01f), 0.0f,
                                                    juce::AudioParameterFloatAttributes().withLabel("oct")),

        std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("filterEnvRelease", 1), "Filter Env Release",
                                                    juce::NormalisableRange<float>(5.0f, 2000.0f, 0.1f, 0.35f), 150.0f,
                                                    juce::AudioParameterFloatAttributes().withLabel("ms")),

        std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("postDriveLowpass", 1), "Hi Cut",
                                                    juce::NormalisableRange<float>(20.0f, 20000.0f, 0.1f, 0.5f), 20000.0f,
                                                    juce::AudioParameterFloatAttributes().withLabel("Hz")),

        std::make_unique<juce::AudioParameterFloat>("outputGain", "Output Gain", -24.0f, 24.0f, 0.0f),

        std::make_unique<juce::AudioParameterFloat>("pitchThreshold", "Pitch Threshold", -60.0f, -20.0f, -40.0f),

        std::make_unique<juce::AudioParameterFloat>("fundamentalLimit", "Max Fundamental", 100.0f, 800.0f, 250.0f)
    };

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

int SubbertoneAudioProcessor::getNumPrograms()
{
    return 1;
}

int SubbertoneAudioProcessor::getCurrentProgram()
{
    return 0;
}

void SubbertoneAudioProcessor::setCurrentProgram(int index)
{
    juce::ignoreUnused(index);
}

const juce::String SubbertoneAudioProcessor::getProgramName(int index)
{
    juce::ignoreUnused(index);
    return {};
}

void SubbertoneAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

void SubbertoneAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree state = m_parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void SubbertoneAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState.get() != nullptr && xmlState->hasTagName(m_parameters.state.getType()))
        m_parameters.replaceState(juce::ValueTree::fromXml(*xmlState));
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

    constexpr double smoothingSeconds = 0.02;
    m_mixSmoothed.reset(sampleRate, smoothingSeconds);
    m_outputGainSmoothed.reset(sampleRate, smoothingSeconds);
    m_mixSmoothed.setCurrentAndTargetValue(m_mixParam->load() * 0.01f);
    m_outputGainSmoothed.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(m_outputGainParam->load()));

    m_currentFundamental.store(0.0f);
    m_currentSignalLevelDb.store(0.0f);
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

    m_currentFundamental.store(m_subharmonicEngine.getFundamental());
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
