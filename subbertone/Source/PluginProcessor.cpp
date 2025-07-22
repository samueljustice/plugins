#include "PluginProcessor.h"
#include "PluginEditor.h"

SubbertoneAudioProcessor::SubbertoneAudioProcessor()
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
       parameters(*this, nullptr, juce::Identifier("SubbertoneParameters"), createParameterLayout())
{
    pitchDetector = std::make_unique<PitchDetector>();
    subharmonicEngine = std::make_unique<SubharmonicEngine>();
    
    inputVisualBuffer.resize(visualBufferSize, 0.0f);
    outputVisualBuffer.resize(visualBufferSize, 0.0f);
    harmonicResidualVisualBuffer.resize(visualBufferSize, 0.0f);
}

SubbertoneAudioProcessor::~SubbertoneAudioProcessor()
{
}

juce::AudioProcessorValueTreeState::ParameterLayout SubbertoneAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "mix", "Mix", 0.0f, 100.0f, 50.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "distortion", "Distortion", 0.0f, 100.0f, 50.0f));
    
    
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        "distortionType", "Distortion Type", 
        juce::StringArray{"Soft Clip", "Hard Clip", "Tube", "Foldback"}, 0));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("distortionTone", 1), "Tone", 
        juce::NormalisableRange<float>(20.0f, 20000.0f, 0.1f, 0.5f), 1000.0f,
        juce::AudioParameterFloatAttributes().withLabel("Hz")));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("postDriveLowpass", 1), "Post-Drive Lowpass", 
        juce::NormalisableRange<float>(20.0f, 20000.0f, 0.1f, 0.5f), 20000.0f,
        juce::AudioParameterFloatAttributes().withLabel("Hz")));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "outputGain", "Output Gain", -24.0f, 24.0f, 0.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "pitchThreshold", "Pitch Threshold", -60.0f, -20.0f, -40.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "fundamentalLimit", "Max Fundamental", 100.0f, 800.0f, 250.0f));
    
    return { params.begin(), params.end() };
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
}

const juce::String SubbertoneAudioProcessor::getProgramName(int index)
{
    return {};
}

void SubbertoneAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
}

void SubbertoneAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    pitchDetector->prepare(sampleRate, samplesPerBlock);
    subharmonicEngine->prepare(sampleRate, samplesPerBlock);
}

void SubbertoneAudioProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool SubbertoneAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
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

void SubbertoneAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                           juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // Get parameter values
    float mix = parameters.getRawParameterValue("mix")->load() / 100.0f;
    float distortion = parameters.getRawParameterValue("distortion")->load() / 100.0f;
    float inverseMix = 1.0f; // Always 100% inverse mix - the unique feature
    int distortionType = static_cast<int>(parameters.getRawParameterValue("distortionType")->load());
    float distortionTone = parameters.getRawParameterValue("distortionTone")->load();
    float postDriveLowpass = parameters.getRawParameterValue("postDriveLowpass")->load();
    float outputGain = juce::Decibels::decibelsToGain(
        parameters.getRawParameterValue("outputGain")->load());
    float pitchThreshold = parameters.getRawParameterValue("pitchThreshold")->load();
    float fundamentalLimit = parameters.getRawParameterValue("fundamentalLimit")->load();

    // Handle mono/stereo configurations
    const int numChannelsToProcess = juce::jmin(totalNumInputChannels, totalNumOutputChannels);
    
    // Process each channel
    for (int channel = 0; channel < numChannelsToProcess; ++channel)
    {
        auto* channelData = buffer.getWritePointer(channel);
        const auto* inputData = buffer.getReadPointer(channel);
        
        // Store input for visualization (only for first channel to avoid duplication)
        if (channel == 0)
        {
            const juce::ScopedLock sl(visualBufferLock);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                int writePos = (visualBufferWritePos.load() + i) % visualBufferSize;
                inputVisualBuffer[writePos] = inputData[i];
            }
        }
        
        // Detect fundamental (using first channel only)
        if (channel == 0)
        {
            float maxInput = 0.0f;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                maxInput = std::max(maxInput, std::abs(inputData[i]));
            }
            
            // Store signal level in dB
            float signalDb = maxInput > 0.0f ? 20.0f * std::log10(maxInput) : -100.0f;
            currentSignalLevel.store(signalDb);
            
            // Convert pitch threshold (in dB) to linear threshold for pitch detector
            float linearThreshold = std::pow(10.0f, pitchThreshold / 20.0f);
            float fundamental = pitchDetector->detectPitch(
                inputData, buffer.getNumSamples(), linearThreshold);
            
            // Apply fundamental frequency limit
            if (fundamental > fundamentalLimit)
            {
                fundamental = 0.0f; // Treat as no detection if above limit
            }
            
            currentFundamental.store(fundamental);
            
        }
        
        // Create temporary buffer for subharmonic processing
        std::vector<float> subharmonicBuffer(buffer.getNumSamples());
        
        // Process with subharmonic engine
        subharmonicEngine->process(subharmonicBuffer.data(), buffer.getNumSamples(),
                                  currentFundamental.load(),
                                  distortion, inverseMix, distortionType, 
                                  distortionTone, postDriveLowpass);
        
        // Get harmonic residual buffer for visualization
        const auto& harmonicResidual = subharmonicEngine->getHarmonicResidualBuffer();
        
        // Apply mix and output gain
        static int mixDebugCount = 0;
        if (channel == 0 && mixDebugCount < 10)
        {
            DBG("=== MIX STAGE DEBUG ===");
            DBG("Mix: " << mix << " (0=dry, 1=wet)");
            DBG("Output Gain: " << outputGain);
            DBG("First 5 dry samples: " << inputData[0] << " " << inputData[1] << " " << inputData[2] << " " << inputData[3] << " " << inputData[4]);
            DBG("First 5 wet samples: " << subharmonicBuffer[0] << " " << subharmonicBuffer[1] << " " << subharmonicBuffer[2] << " " << subharmonicBuffer[3] << " " << subharmonicBuffer[4]);
            mixDebugCount++;
        }
        
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            float dry = inputData[i];
            float wet = subharmonicBuffer[i];
            channelData[i] = (dry * (1.0f - mix) + wet * mix) * outputGain;
            
            // Store output for visualization (only for first channel)
            if (channel == 0)
            {
                const juce::ScopedLock sl(visualBufferLock);
                int writePos = (visualBufferWritePos.load() + i) % visualBufferSize;
                outputVisualBuffer[writePos] = channelData[i];
                
                // Store harmonic residual if available
                if (i < harmonicResidual.size())
                {
                    harmonicResidualVisualBuffer[writePos] = harmonicResidual[i];
                }
                else
                {
                    harmonicResidualVisualBuffer[writePos] = 0.0f;
                }
            }
        }
    }
    
    // Handle mono output by copying channel 0 to channel 1 if needed
    if (totalNumInputChannels == 1 && totalNumOutputChannels == 2)
    {
        buffer.copyFrom(1, 0, buffer, 0, 0, buffer.getNumSamples());
    }
    
    // Update write position for next block
    visualBufferWritePos = (visualBufferWritePos.load() + buffer.getNumSamples()) % visualBufferSize;
}

std::vector<float> SubbertoneAudioProcessor::getInputWaveform() const
{
    const juce::ScopedLock sl(visualBufferLock);
    std::vector<float> result(visualBufferSize);
    int readPos = visualBufferWritePos.load();
    
    // Copy from circular buffer in the correct order
    for (int i = 0; i < visualBufferSize; ++i)
    {
        result[i] = inputVisualBuffer[(readPos + i) % visualBufferSize];
    }
    return result;
}

std::vector<float> SubbertoneAudioProcessor::getOutputWaveform() const
{
    const juce::ScopedLock sl(visualBufferLock);
    std::vector<float> result(visualBufferSize);
    int readPos = visualBufferWritePos.load();
    
    // Copy from circular buffer in the correct order
    for (int i = 0; i < visualBufferSize; ++i)
    {
        result[i] = outputVisualBuffer[(readPos + i) % visualBufferSize];
    }
    return result;
}

std::vector<float> SubbertoneAudioProcessor::getHarmonicResidualWaveform() const
{
    const juce::ScopedLock sl(visualBufferLock);
    std::vector<float> result(visualBufferSize);
    int readPos = visualBufferWritePos.load();
    
    // Copy from circular buffer in the correct order
    for (int i = 0; i < visualBufferSize; ++i)
    {
        result[i] = harmonicResidualVisualBuffer[(readPos + i) % visualBufferSize];
    }
    return result;
}

bool SubbertoneAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* SubbertoneAudioProcessor::createEditor()
{
    return new SubbertoneAudioProcessorEditor(*this);
}

void SubbertoneAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void SubbertoneAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState.get() != nullptr)
        if (xmlState->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SubbertoneAudioProcessor();
}