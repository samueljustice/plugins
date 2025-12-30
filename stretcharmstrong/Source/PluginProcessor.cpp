#include "PluginProcessor.h"
#include "PluginEditor.h"

StretchArmstrongAudioProcessor::StretchArmstrongAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor(BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput("Input", juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
#endif
       parameters(*this, nullptr, juce::Identifier("StretchArmstrongParameters"), createParameterLayout())
{
    stretchEngine = std::make_unique<StretchEngine>();

    inputVisualBuffer.resize(visualBufferSize, 0.0f);
    outputVisualBuffer.resize(visualBufferSize, 0.0f);
}

StretchArmstrongAudioProcessor::~StretchArmstrongAudioProcessor()
{
}

juce::AudioProcessorValueTreeState::ParameterLayout StretchArmstrongAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Threshold in dB
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("threshold", 1), "Threshold",
        juce::NormalisableRange<float>(-60.0f, 0.0f, 0.1f), -30.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    // Attack time in milliseconds (1ms to 2000ms)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("attack", 1), "Attack",
        juce::NormalisableRange<float>(1.0f, 2000.0f, 1.0f, 0.4f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")));

    // Sustain time in milliseconds (10ms to 10000ms)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("sustain", 1), "Sustain",
        juce::NormalisableRange<float>(10.0f, 10000.0f, 1.0f, 0.4f), 500.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")));

    // Release time in milliseconds (1ms to 2000ms)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("release", 1), "Release",
        juce::NormalisableRange<float>(1.0f, 2000.0f, 1.0f, 0.4f), 200.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")));

    // Stretch ratio (0.1x to 4.0x, where 1.0 = no stretch)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("stretchRatio", 1), "Stretch Ratio",
        juce::NormalisableRange<float>(0.1f, 4.0f, 0.01f, 0.5f), 2.0f,
        juce::AudioParameterFloatAttributes().withLabel("x")));

    // Stretch type (0 = Varispeed, 1 = Time Stretch)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID("stretchType", 1), "Stretch Type",
        juce::StringArray{"Varispeed", "Time Stretch"}, 1));

    // Mix (dry/wet)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("mix", 1), "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Output gain
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("outputGain", 1), "Output Gain",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    return { params.begin(), params.end() };
}

const juce::String StretchArmstrongAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool StretchArmstrongAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool StretchArmstrongAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool StretchArmstrongAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double StretchArmstrongAudioProcessor::getTailLengthSeconds() const
{
    return 2.0; // Allow for stretched tail
}

int StretchArmstrongAudioProcessor::getNumPrograms()
{
    return 1;
}

int StretchArmstrongAudioProcessor::getCurrentProgram()
{
    return 0;
}

void StretchArmstrongAudioProcessor::setCurrentProgram(int)
{
}

const juce::String StretchArmstrongAudioProcessor::getProgramName(int)
{
    return {};
}

void StretchArmstrongAudioProcessor::changeProgramName(int, const juce::String&)
{
}

void StretchArmstrongAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    int stretchType = static_cast<int>(parameters.getRawParameterValue("stretchType")->load());
    float stretchRatio = parameters.getRawParameterValue("stretchRatio")->load();

    stretchEngine->prepare(sampleRate, samplesPerBlock,
                          static_cast<StretchEngine::StretchType>(stretchType),
                          stretchRatio);

    // Report latency
    setLatencySamples(stretchEngine->getLatencySamples());

    // Reset envelope state
    envelopeState = EnvelopeState::Idle;
    envelopeValue = 0.0f;
    sustainSamplesRemaining = 0;
    signalAboveThreshold = false;
    samplesAboveThreshold = 0;
    samplesBelowThreshold = 0;
}

void StretchArmstrongAudioProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool StretchArmstrongAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
  #else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void StretchArmstrongAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                                   juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // Get parameters
    float thresholdDb = parameters.getRawParameterValue("threshold")->load();
    float attackMs = parameters.getRawParameterValue("attack")->load();
    float sustainMs = parameters.getRawParameterValue("sustain")->load();
    float releaseMs = parameters.getRawParameterValue("release")->load();
    float stretchRatio = parameters.getRawParameterValue("stretchRatio")->load();
    int stretchType = static_cast<int>(parameters.getRawParameterValue("stretchType")->load());
    float mix = parameters.getRawParameterValue("mix")->load() / 100.0f;
    float outputGainDb = parameters.getRawParameterValue("outputGain")->load();
    float outputGain = juce::Decibels::decibelsToGain(outputGainDb);

    // Convert threshold to linear
    float thresholdLinear = juce::Decibels::decibelsToGain(thresholdDb);

    // Calculate envelope coefficients (samples)
    float attackSamples = (attackMs / 1000.0f) * static_cast<float>(currentSampleRate);
    float releaseSamples = (releaseMs / 1000.0f) * static_cast<float>(currentSampleRate);
    int sustainSamples = static_cast<int>((sustainMs / 1000.0f) * currentSampleRate);

    float attackCoeff = 1.0f / std::max(1.0f, attackSamples);
    float releaseCoeff = 1.0f / std::max(1.0f, releaseSamples);

    // Update stretch engine parameters
    stretchEngine->setStretchType(static_cast<StretchEngine::StretchType>(stretchType));
    stretchEngine->setStretchRatio(stretchRatio);

    // Store dry signal
    juce::AudioBuffer<float> dryBuffer;
    dryBuffer.makeCopyOf(buffer);

    int numSamples = buffer.getNumSamples();
    int numChannels = buffer.getNumChannels();

    // Process each sample for threshold detection and envelope
    for (int i = 0; i < numSamples; ++i)
    {
        // Calculate peak level across all channels
        float peakLevel = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            peakLevel = std::max(peakLevel, std::abs(buffer.getSample(ch, i)));
        }

        // Check threshold with hysteresis
        bool currentlyAbove = peakLevel > thresholdLinear;

        if (currentlyAbove)
        {
            samplesAboveThreshold++;
            samplesBelowThreshold = 0;

            if (!signalAboveThreshold && samplesAboveThreshold >= hysteresisOnSamples)
            {
                signalAboveThreshold = true;
            }
        }
        else
        {
            samplesBelowThreshold++;
            samplesAboveThreshold = 0;

            if (signalAboveThreshold && samplesBelowThreshold >= hysteresisOffSamples)
            {
                signalAboveThreshold = false;
            }
        }

        // ASR envelope state machine
        switch (envelopeState)
        {
            case EnvelopeState::Idle:
                if (signalAboveThreshold)
                {
                    envelopeState = EnvelopeState::Attack;
                }
                break;

            case EnvelopeState::Attack:
                envelopeValue += attackCoeff;
                if (envelopeValue >= 1.0f)
                {
                    envelopeValue = 1.0f;
                    envelopeState = EnvelopeState::Sustain;
                    sustainSamplesRemaining = sustainSamples;
                }
                break;

            case EnvelopeState::Sustain:
                sustainSamplesRemaining--;
                if (sustainSamplesRemaining <= 0)
                {
                    envelopeState = EnvelopeState::Release;
                }
                // Re-trigger if signal goes above threshold again during sustain
                else if (signalAboveThreshold)
                {
                    sustainSamplesRemaining = sustainSamples; // Reset sustain
                }
                break;

            case EnvelopeState::Release:
                envelopeValue -= releaseCoeff;
                if (envelopeValue <= 0.0f)
                {
                    envelopeValue = 0.0f;
                    envelopeState = EnvelopeState::Idle;
                }
                // Re-trigger if signal goes above threshold during release
                if (signalAboveThreshold)
                {
                    envelopeState = EnvelopeState::Attack;
                }
                break;
        }
    }

    // Store signal level for visualization
    float rms = 0.0f;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float sample = buffer.getSample(ch, i);
            rms += sample * sample;
        }
    }
    rms = std::sqrt(rms / static_cast<float>(numSamples * numChannels));
    currentSignalLevel.store(juce::Decibels::gainToDecibels(rms, -100.0f));
    currentEnvelopeValue.store(envelopeValue);
    stretchActive.store(envelopeValue > 0.01f);
    currentStretchRatio.store(stretchRatio);

    // Store input for visualization
    if (numChannels > 0)
    {
        const juce::ScopedLock sl(visualBufferLock);
        const float* inputData = buffer.getReadPointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            int writePos = (visualBufferWritePos.load() + i) % visualBufferSize;
            inputVisualBuffer[static_cast<size_t>(writePos)] = inputData[i];
        }
    }

    // Process through stretch engine with envelope
    stretchEngine->process(buffer, envelopeValue);

    // Mix dry/wet and apply output gain
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* wetData = buffer.getWritePointer(ch);
        const float* dryData = dryBuffer.getReadPointer(ch);

        for (int i = 0; i < numSamples; ++i)
        {
            wetData[i] = (dryData[i] * (1.0f - mix) + wetData[i] * mix) * outputGain;
        }
    }

    // Store output for visualization
    if (numChannels > 0)
    {
        const juce::ScopedLock sl(visualBufferLock);
        const float* outputData = buffer.getReadPointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            int writePos = (visualBufferWritePos.load() + i) % visualBufferSize;
            outputVisualBuffer[static_cast<size_t>(writePos)] = outputData[i];
        }
        visualBufferWritePos = (visualBufferWritePos.load() + numSamples) % visualBufferSize;
    }
}

std::vector<float> StretchArmstrongAudioProcessor::getInputWaveform() const
{
    const juce::ScopedLock sl(visualBufferLock);
    std::vector<float> result(visualBufferSize);
    int readPos = visualBufferWritePos.load();

    for (int i = 0; i < visualBufferSize; ++i)
    {
        result[static_cast<size_t>(i)] = inputVisualBuffer[static_cast<size_t>((readPos + i) % visualBufferSize)];
    }
    return result;
}

std::vector<float> StretchArmstrongAudioProcessor::getOutputWaveform() const
{
    const juce::ScopedLock sl(visualBufferLock);
    std::vector<float> result(visualBufferSize);
    int readPos = visualBufferWritePos.load();

    for (int i = 0; i < visualBufferSize; ++i)
    {
        result[static_cast<size_t>(i)] = outputVisualBuffer[static_cast<size_t>((readPos + i) % visualBufferSize)];
    }
    return result;
}

bool StretchArmstrongAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* StretchArmstrongAudioProcessor::createEditor()
{
    return new StretchArmstrongAudioProcessorEditor(*this);
}

void StretchArmstrongAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void StretchArmstrongAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState.get() != nullptr)
        if (xmlState->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StretchArmstrongAudioProcessor();
}
