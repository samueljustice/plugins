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
    pitchDetector = std::make_unique<PitchDetector>();

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

    // ===== ENVELOPE FOLLOWER SECTION =====

    // Envelope follower enable
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID("envFollowEnable", 1), "Env Enable", false));

    // Envelope follower amount - how much input level modulates stretch ratio
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("envFollowAmount", 1), "Env Amount",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Envelope follower attack time
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("envFollowAttack", 1), "Env Attack",
        juce::NormalisableRange<float>(0.1f, 100.0f, 0.1f, 0.5f), 5.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")));

    // Envelope follower release time
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("envFollowRelease", 1), "Env Release",
        juce::NormalisableRange<float>(1.0f, 500.0f, 0.1f, 0.5f), 50.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")));

    // ===== PITCH FOLLOWER SECTION =====

    // Pitch follower enable
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID("pitchFollowEnable", 1), "Pitch Enable", false));

    // Pitch follower amount - how much pitch modulates stretch ratio
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("pitchFollowAmount", 1), "Pitch Amount",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // ===== SLEW/SMOOTHING SECTION =====

    // Modulation slew time (affects both env and pitch followers)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("modulationSlew", 1), "Mod Slew",
        juce::NormalisableRange<float>(1.0f, 500.0f, 0.1f, 0.5f), 50.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")));

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

    // Prepare pitch detector
    pitchDetector->prepare(sampleRate, samplesPerBlock);

    // Report latency
    setLatencySamples(stretchEngine->getLatencySamples());

    // Reset envelope state
    envelopeState = EnvelopeState::Idle;
    envelopeValue = 0.0f;
    sustainSamplesRemaining = 0;
    signalAboveThreshold = false;
    samplesAboveThreshold = 0;
    samplesBelowThreshold = 0;

    // Reset modulation state
    envFollowerValue = 0.0f;
    slewedEnvFollower = 0.0f;
    pitchFollowerValue = 0.0f;
    slewedPitchFollower = 0.0f;
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

    // Envelope follower parameters
    bool envFollowEnable = parameters.getRawParameterValue("envFollowEnable")->load() > 0.5f;
    float envFollowAmount = parameters.getRawParameterValue("envFollowAmount")->load() / 100.0f;
    float envFollowAttackMs = parameters.getRawParameterValue("envFollowAttack")->load();
    float envFollowReleaseMs = parameters.getRawParameterValue("envFollowRelease")->load();

    // Pitch follower parameters
    bool pitchFollowEnable = parameters.getRawParameterValue("pitchFollowEnable")->load() > 0.5f;
    float pitchFollowAmount = parameters.getRawParameterValue("pitchFollowAmount")->load() / 100.0f;

    // Slew parameter
    float modulationSlewMs = parameters.getRawParameterValue("modulationSlew")->load();

    // Calculate envelope follower coefficients
    envFollowerAttackCoeff = std::exp(-1.0f / (envFollowAttackMs * 0.001f * static_cast<float>(currentSampleRate)));
    envFollowerReleaseCoeff = std::exp(-1.0f / (envFollowReleaseMs * 0.001f * static_cast<float>(currentSampleRate)));

    // Calculate slew coefficient (for smoothing modulation values)
    modulationSlewCoeff = std::exp(-1.0f / (modulationSlewMs * 0.001f * static_cast<float>(currentSampleRate)));

    // Convert threshold to linear
    float thresholdLinear = juce::Decibels::decibelsToGain(thresholdDb);

    // Get buffer dimensions early (needed for pitch detection)
    int numSamples = buffer.getNumSamples();
    int numChannels = buffer.getNumChannels();

    // Calculate envelope coefficients (samples)
    float attackSamples = (attackMs / 1000.0f) * static_cast<float>(currentSampleRate);
    float releaseSamples = (releaseMs / 1000.0f) * static_cast<float>(currentSampleRate);
    int sustainSamples = static_cast<int>((sustainMs / 1000.0f) * currentSampleRate);

    float attackCoeff = 1.0f / std::max(1.0f, attackSamples);
    float releaseCoeff = 1.0f / std::max(1.0f, releaseSamples);

    // ===== PITCH DETECTION =====
    // Run pitch detection on mono input (use first channel or average)
    // Maps detected pitch directly to modulation: higher pitch = more stretch
    if (pitchFollowEnable && numChannels > 0)
    {
        const float* monoInput = buffer.getReadPointer(0);
        float detectedPitch = pitchDetector->detectPitch(monoInput, numSamples, thresholdLinear);

        if (detectedPitch > 0.0f)
        {
            // Map pitch logarithmically to 0-1 range
            // 80Hz = 0, 640Hz = 1 (covers ~3 octaves of typical bass-to-treble range)
            constexpr float minPitch = 80.0f;
            constexpr float maxPitch = 640.0f;
            float normalized = (std::log2(detectedPitch) - std::log2(minPitch)) /
                              (std::log2(maxPitch) - std::log2(minPitch));
            pitchFollowerValue = juce::jlimit(0.0f, 1.0f, normalized);
        }
        else
        {
            // No pitch detected - decay towards 0
            pitchFollowerValue *= 0.95f;
        }
    }
    else
    {
        pitchFollowerValue *= 0.95f;
    }

    // ===== APPLY SLEWING TO MODULATION SOURCES =====
    // Smooth envelope follower
    slewedEnvFollower = slewedEnvFollower * modulationSlewCoeff +
                        envFollowerValue * (1.0f - modulationSlewCoeff);

    // Smooth pitch follower
    slewedPitchFollower = slewedPitchFollower * modulationSlewCoeff +
                          pitchFollowerValue * (1.0f - modulationSlewCoeff);

    // ===== CALCULATE MODULATED STRETCH RATIO =====
    float totalModulation = 0.0f;

    // Apply envelope follower modulation if enabled
    if (envFollowEnable)
    {
        float envMod = std::min(1.0f, slewedEnvFollower); // 0 to 1
        totalModulation += envMod * envFollowAmount;
    }

    // Apply pitch follower modulation if enabled
    if (pitchFollowEnable)
    {
        // Pitch modulation: higher pitch = more stretch (slewedPitchFollower is 0-1)
        totalModulation += slewedPitchFollower * pitchFollowAmount;
    }

    // Modulate stretch ratio:
    // Base ratio is scaled by (1 + totalModulation)
    // This means 100% env at full level doubles the stretch effect
    float modulatedStretchRatio = 1.0f + (stretchRatio - 1.0f) * (1.0f + totalModulation);
    modulatedStretchRatio = juce::jlimit(0.1f, 8.0f, modulatedStretchRatio); // Safety clamp

    // Update stretch engine parameters
    stretchEngine->setStretchType(static_cast<StretchEngine::StretchType>(stretchType));
    stretchEngine->setStretchRatio(modulatedStretchRatio);

    // Store dry signal
    juce::AudioBuffer<float> dryBuffer;
    dryBuffer.makeCopyOf(buffer);

    // Process each sample for threshold detection, envelope, and envelope follower
    for (int i = 0; i < numSamples; ++i)
    {
        // Calculate peak level across all channels
        float peakLevel = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            peakLevel = std::max(peakLevel, std::abs(buffer.getSample(ch, i)));
        }

        // Update envelope follower (tracks amplitude with attack/release smoothing)
        if (peakLevel > envFollowerValue)
        {
            // Attack: level is rising
            envFollowerValue = envFollowerAttackCoeff * envFollowerValue + (1.0f - envFollowerAttackCoeff) * peakLevel;
        }
        else
        {
            // Release: level is falling
            envFollowerValue = envFollowerReleaseCoeff * envFollowerValue + (1.0f - envFollowerReleaseCoeff) * peakLevel;
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
