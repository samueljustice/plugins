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
    // Double buffering for visualization
    for (int i = 0; i < 2; ++i)
    {
        m_inputVisualBuffer[i].resize(c_visualBufferSize, 0.0f);
        m_outputVisualBuffer[i].resize(c_visualBufferSize, 0.0f);
        m_harmonicResidualVisualBuffer[i].resize(c_visualBufferSize, 0.0f);
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout SubbertoneAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout
    {
        std::make_unique<juce::AudioParameterFloat>("mix", "Mix", 0.0f, 100.0f, 50.0f),

        std::make_unique<juce::AudioParameterFloat>("distortion", "Distortion", 0.0f, 100.0f, 50.0f),

        std::make_unique<juce::AudioParameterChoice>("distortionType", "Distortion Type", juce::StringArray{"Soft Clip", "Hard Clip", "Tube", "Foldback"}, 0),

        std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("distortionTone", 1), "Tone", 
                                                    juce::NormalisableRange<float>(20.0f, 20000.0f, 0.1f, 0.5f), 1000.0f,
                                                    juce::AudioParameterFloatAttributes().withLabel("Hz")),

        std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("postDriveLowpass", 1), "Post-Drive Lowpass",
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
    return 0.0;
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
    m_currentMaxProcessBlockSize = std::clamp(samplesPerBlock, 1, c_maxProcessBlockSize);

    // Prepare processors
    m_pitchDetector.prepare(sampleRate);
    m_subharmonicBuffer.resize(m_currentMaxProcessBlockSize, 0.0f);

    m_subharmonicEngine.prepare(sampleRate, samplesPerBlock);
    m_pitchDetectBuffer.resize(m_currentMaxProcessBlockSize, 0.0f);

    // Prepare parameters
    constexpr double smoothingSeconds = 0.02;
    m_mixSmoothed.reset(sampleRate, smoothingSeconds);
    m_distortionSmoothed.reset(sampleRate, smoothingSeconds);
    m_toneSmoothed.reset(sampleRate, smoothingSeconds);
    m_postDriveLowpassSmoothed.reset(sampleRate, smoothingSeconds);
    m_outputGainSmoothed.reset(sampleRate, smoothingSeconds);

    updateParameterCache();

    m_mixSmoothed.setCurrentAndTargetValue(m_parameterCache.m_mix);
    m_distortionSmoothed.setCurrentAndTargetValue(m_parameterCache.m_distortion);
    m_toneSmoothed.setCurrentAndTargetValue(m_parameterCache.m_distortionTone);
    m_postDriveLowpassSmoothed.setCurrentAndTargetValue(m_parameterCache.m_postDriveLowpass);
    m_outputGainSmoothed.setCurrentAndTargetValue(m_parameterCache.m_outputGain);
}

void SubbertoneAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);

    juce::ScopedNoDenormals noDenormals;

    const int totalNumInputChannels  = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();
    const int numChannelsToProcess = std::min(totalNumInputChannels, totalNumOutputChannels);
    const int numSamples = buffer.getNumSamples();

    if (numSamples > m_currentMaxProcessBlockSize)
    {
        buffer.clear();
        return;
    }

    for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, numSamples);

    const float* const* inputPtrs = buffer.getArrayOfReadPointers();

    // Get parameter values
    updateParameterCache();
    m_mixSmoothed.setTargetValue(m_parameterCache.m_mix);
    m_distortionSmoothed.setTargetValue(m_parameterCache.m_distortion);
    m_toneSmoothed.setTargetValue(m_parameterCache.m_distortionTone);
    m_postDriveLowpassSmoothed.setTargetValue(m_parameterCache.m_postDriveLowpass);
    m_outputGainSmoothed.setTargetValue(m_parameterCache.m_outputGain);

    const float distortion       = m_parameterCache.m_distortion;
    const int distortionType     = m_parameterCache.m_distortionType;
    const float distortionTone   = m_toneSmoothed.getNextValue();
    const float postDriveLowpass = m_postDriveLowpassSmoothed.getNextValue();
    const float pitchThreshold   = m_parameterCache.m_pitchThreshold;
    const float fundamentalLimit = m_parameterCache.m_fundamentalLimit;

    // STEP 1: Detect fundamental FIRST (using first channel)
    float detectedFundamental = 0.0f;

    // Calculate RMS
    float rmsSum = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        float sum = 0.0f;

        for (int ch = 0; ch < numChannelsToProcess; ++ch)
            sum += inputPtrs[ch][i];

        const float monoSample = sum / static_cast<float>(numChannelsToProcess);

        m_pitchDetectBuffer[i] = monoSample;
        rmsSum += monoSample * monoSample;
    }

    const float rms = std::sqrt(rmsSum / static_cast<float>(numSamples));
    m_currentSignalLevelDb.store(rms);
    
    const bool inputActive = rms >= pitchThreshold;

    detectedFundamental = (inputActive) ? m_pitchDetector.detectPitch(m_pitchDetectBuffer.data(), numSamples, pitchThreshold) : 0.0f;

    // Apply fundamental frequency limit
    if (detectedFundamental > fundamentalLimit)
    {
        detectedFundamental = 0.0f;
    }
    
    m_currentFundamental.store(detectedFundamental);

    // STEP 2: Process subharmonic engine with CURRENT fundamental
    m_subharmonicEngine.process(m_subharmonicBuffer.data(), 
                                numSamples,
                                detectedFundamental,
                                distortion, 
                                distortionType,
                                distortionTone, 
                                postDriveLowpass, 
                                inputActive);
    
    // STEP 3: Apply mix + Output gain
    for (int channel = 0; channel < numChannelsToProcess; ++channel)
    {
        const float* inputData = buffer.getReadPointer(channel);
        float* channelData     = buffer.getWritePointer(channel);
        
        // Apply mix and output gain with per-sample smoothing
        for (int i = 0; i < numSamples; ++i)
        {
            const float mixValue = m_mixSmoothed.getNextValue();
            const float outputGainValue = m_outputGainSmoothed.getNextValue();

            const float dry = inputData[i];
            const float wet = m_subharmonicBuffer[i] * -1.0f; // inverse mix? maybe a small delay suit better?

            channelData[i] = outputGainValue * (dry * (1.0f - mixValue) + wet * mixValue);
        }
    }

    updateVisualizerBuffers(buffer);
    
    // Handle mono to stereo if needed
    if (totalNumInputChannels == 1 && totalNumOutputChannels == 2)
    {
        buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
    }
}

void SubbertoneAudioProcessor::updateParameterCache()
{
    m_parameterCache.m_mix              = m_parameters.getRawParameterValue("mix")->load() * 0.01f;
    m_parameterCache.m_distortion       = m_parameters.getRawParameterValue("distortion")->load() * 0.01f;
    m_parameterCache.m_distortionType   = static_cast<int>(m_parameters.getRawParameterValue("distortionType")->load());
    m_parameterCache.m_distortionTone   = m_parameters.getRawParameterValue("distortionTone")->load();
    m_parameterCache.m_postDriveLowpass = m_parameters.getRawParameterValue("postDriveLowpass")->load();
    m_parameterCache.m_outputGain       = juce::Decibels::decibelsToGain(m_parameters.getRawParameterValue("outputGain")->load());
    m_parameterCache.m_pitchThreshold   = std::pow(10.0f, m_parameters.getRawParameterValue("pitchThreshold")->load() / 20.0f);
    m_parameterCache.m_fundamentalLimit = m_parameters.getRawParameterValue("fundamentalLimit")->load();
}

void SubbertoneAudioProcessor::updateVisualizerBuffers(juce::AudioBuffer<float>& buffer)
{
    // Store visualization data once per block (only for first channel)
    const int numSamples = buffer.getNumSamples();

    constexpr int channel0 = 0;
    const float* outputData = buffer.getReadPointer(channel0);

    const std::vector<float>& harmonicResidual = m_subharmonicEngine.getHarmonicResidualBuffer();

    const int writeIndex = 1 - m_visualReadIndex.load(std::memory_order_relaxed);
    const int baseWritePos = m_visualBufferWritePos[writeIndex].load(std::memory_order_relaxed);

    for (int i = 0; i < numSamples; ++i)
    {
        const int writePos = (baseWritePos + i) % c_visualBufferSize;

        m_inputVisualBuffer[writeIndex][writePos]  = m_pitchDetectBuffer[i];
        m_outputVisualBuffer[writeIndex][writePos] = outputData[i];
        m_harmonicResidualVisualBuffer[writeIndex][writePos] = (i < static_cast<int>(harmonicResidual.size())) ? harmonicResidual[i] : 0.0f;
    }

    m_visualBufferWritePos[writeIndex].store((baseWritePos + numSamples) % c_visualBufferSize, std::memory_order_relaxed);
    m_visualReadIndex.store(writeIndex, std::memory_order_release);
}

void SubbertoneAudioProcessor::getInputWaveform(std::vector<float>& dest) const
{
    if (dest.size() != c_visualBufferSize)
        dest.resize(c_visualBufferSize);

    const int readIndex = m_visualReadIndex.load(std::memory_order_acquire);
    const int readPos   = m_visualBufferWritePos[readIndex].load(std::memory_order_relaxed);
    
    // Copy from circular buffer in the correct order
    for (int i = 0; i < c_visualBufferSize; ++i)
    {
        dest[i] = m_inputVisualBuffer[readIndex][(readPos + i) % c_visualBufferSize];
    }
}

void SubbertoneAudioProcessor::getOutputWaveform(std::vector<float>& dest) const
{
    if (dest.size() != c_visualBufferSize)
        dest.resize(c_visualBufferSize);

    const int readIndex = m_visualReadIndex.load(std::memory_order_acquire);
    const int readPos   = m_visualBufferWritePos[readIndex].load(std::memory_order_relaxed);
    
    // Copy from circular buffer in the correct order
    for (int i = 0; i < c_visualBufferSize; ++i)
    {
        dest[i] = m_outputVisualBuffer[readIndex][(readPos + i) % c_visualBufferSize];
    }
}

void SubbertoneAudioProcessor::getHarmonicResidualWaveform(std::vector<float>& dest) const
{
    if (dest.size() != c_visualBufferSize)
        dest.resize(c_visualBufferSize);

    const int readIndex = m_visualReadIndex.load(std::memory_order_acquire);
    const int readPos   = m_visualBufferWritePos[readIndex].load(std::memory_order_relaxed);
    
    // Copy from circular buffer in the correct order
    for (int i = 0; i < c_visualBufferSize; ++i)
    {
        dest[i] = m_harmonicResidualVisualBuffer[readIndex][(readPos + i) % c_visualBufferSize];
    }
}
