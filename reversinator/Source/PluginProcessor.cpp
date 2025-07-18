#include "PluginProcessor.h"
#include "PluginEditor.h"

ReversinatorAudioProcessor::ReversinatorAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
#endif
    valueTreeState(*this, nullptr, juce::Identifier("ReversinatorState"), createParameterLayout())
{
    reverserEnabled = valueTreeState.getRawParameterValue("reverser");
    windowTime = valueTreeState.getRawParameterValue("time");
    feedbackDepth = valueTreeState.getRawParameterValue("feedback");
    wetMix = valueTreeState.getRawParameterValue("wetmix");
    dryMix = valueTreeState.getRawParameterValue("drymix");
    effectMode = valueTreeState.getRawParameterValue("mode");
    crossfadeTime = valueTreeState.getRawParameterValue("crossfade");
    envelopeTime = valueTreeState.getRawParameterValue("envelope");
    
    reverseEngine = std::make_unique<ReverseEngine>();
}

ReversinatorAudioProcessor::~ReversinatorAudioProcessor()
{
}

juce::AudioProcessorValueTreeState::ParameterLayout ReversinatorAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        "reverser", "Reverser", false));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "time", "Time", 
        juce::NormalisableRange<float>(0.03f, 5.0f, 0.001f, 0.5f),  // min 30ms, max 5s
        2.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "feedback", "Feedback Depth", 
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 
        0.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "wetmix", "Wet Mix", 
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 
        100.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "drymix", "Dry Mix", 
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 
        0.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        "mode", "Effect Mode", 
        juce::StringArray{"Reverse Playback", "Forward Backwards", "Reverse Repeat"}, 
        0));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "crossfade", "Crossfade Time", 
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 
        20.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "envelope", "Envelope", 
        juce::NormalisableRange<float>(10.0f, 100.0f, 1.0f), 
        30.0f));
    
    return { params.begin(), params.end() };
}

const juce::String ReversinatorAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool ReversinatorAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool ReversinatorAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool ReversinatorAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double ReversinatorAudioProcessor::getTailLengthSeconds() const
{
    return 5.0;
}

int ReversinatorAudioProcessor::getNumPrograms()
{
    return 1;
}

int ReversinatorAudioProcessor::getCurrentProgram()
{
    return 0;
}

void ReversinatorAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String ReversinatorAudioProcessor::getProgramName (int index)
{
    return {};
}

void ReversinatorAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

void ReversinatorAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    reverseEngine->prepare(sampleRate, samplesPerBlock, getTotalNumOutputChannels());
    reverserCrossfade.reset(sampleRate, 0.1); // 100ms crossfade for smooth on/off
    reverserCrossfade.setCurrentAndTargetValue(reverserEnabled->load() ? 1.0f : 0.0f);
}

void ReversinatorAudioProcessor::releaseResources()
{
    reverseEngine->reset();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool ReversinatorAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
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

void ReversinatorAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    bool currentReverserState = reverserEnabled->load() > 0.5f;
    if (currentReverserState != previousReverserState)
    {
        reverserCrossfade.setTargetValue(currentReverserState ? 1.0f : 0.0f);
        previousReverserState = currentReverserState;
    }
    
    reverseEngine->setParameters(
        windowTime->load(),
        feedbackDepth->load() / 100.0f,
        wetMix->load() / 100.0f,
        dryMix->load() / 100.0f,
        static_cast<int>(effectMode->load()),
        crossfadeTime->load(),
        envelopeTime->load() / 1000.0f  // Convert ms to seconds
    );
    
    if (reverserCrossfade.isSmoothing() || currentReverserState)
    {
        juce::AudioBuffer<float> wetBuffer(totalNumOutputChannels, buffer.getNumSamples());
        for (int channel = 0; channel < totalNumOutputChannels; ++channel)
        {
            wetBuffer.copyFrom(channel, 0, buffer, channel, 0, buffer.getNumSamples());
        }
        
        reverseEngine->process(wetBuffer);
        
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            float crossfade = reverserCrossfade.getNextValue();
            
            for (int channel = 0; channel < totalNumOutputChannels; ++channel)
            {
                float dry = buffer.getSample(channel, sample);
                float wet = wetBuffer.getSample(channel, sample);
                buffer.setSample(channel, sample, dry * (1.0f - crossfade) + wet * crossfade);
            }
        }
    }
}

bool ReversinatorAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* ReversinatorAudioProcessor::createEditor()
{
    return new ReversinatorAudioProcessorEditor (*this);
}

void ReversinatorAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = valueTreeState.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void ReversinatorAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    
    if (xmlState.get() != nullptr)
        if (xmlState->hasTagName (valueTreeState.state.getType()))
            valueTreeState.replaceState (juce::ValueTree::fromXml (*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ReversinatorAudioProcessor();
}