#include "PluginProcessor.h"
#include "PluginEditor.h"

PitchFlattenerAudioProcessor::PitchFlattenerAudioProcessor()
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
       parameters(*this, nullptr, "Parameters", createParameterLayout())
{
    pitchDetector = std::make_unique<PitchDetector>();
    pitchEngine = std::make_unique<PitchFlattenerEngine>();
}

PitchFlattenerAudioProcessor::~PitchFlattenerAudioProcessor()
{
    // Ensure resources are released before destruction
    releaseResources();
    
    // Clear pointers
    pitchDetector.reset();
    pitchEngine.reset();
}

juce::AudioProcessorValueTreeState::ParameterLayout PitchFlattenerAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "targetPitch", "Target Pitch", 
        juce::NormalisableRange<float>(50.0f, 2000.0f, 1.0f, 0.5f), 
        1200.0f));  // 1200Hz default
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "smoothingTimeMs", "Smoothing Time", 
        juce::NormalisableRange<float>(5.0f, 200.0f, 1.0f), 
        150.0f));  // 150ms default for smooth flattening
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "mix", "Mix", 
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 
        1.0f));
    
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        "manualOverride", "Manual Override", 
        false));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "overrideFreq", "Override Frequency", 
        juce::NormalisableRange<float>(50.0f, 2000.0f, 1.0f, 0.5f), 
        440.0f));
    
    // Pitch detection parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "detectionRate", "Detection Rate", 
        juce::NormalisableRange<float>(64.0f, 1024.0f, 64.0f), 
        64.0f));  // 64 samples = fastest detection for responsive tracking
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "pitchThreshold", "Pitch Threshold", 
        juce::NormalisableRange<float>(0.05f, 0.5f, 0.01f), 
        0.10f));  // Lower threshold for better pitch detection
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "minFreq", "Min Frequency", 
        juce::NormalisableRange<float>(20.0f, 1000.0f, 10.0f), 
        600.0f));  // 600Hz min - aligned with detection highpass
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "maxFreq", "Max Frequency", 
        juce::NormalisableRange<float>(500.0f, 4000.0f, 10.0f), 
        2000.0f));  // 2000Hz max - good headroom for most sources
    
    // Advanced pitch detection parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "pitchHoldTime", "Pitch Hold Time", 
        juce::NormalisableRange<float>(0.0f, 2000.0f, 10.0f), 
        500.0f));  // milliseconds to hold pitch before accepting new one
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "pitchJumpThreshold", "Jump Threshold", 
        juce::NormalisableRange<float>(10.0f, 500.0f, 10.0f), 
        300.0f));  // Hz - higher threshold for pitch sweeps
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "minConfidence", "Min Confidence", 
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 
        0.35f));  // Lower confidence for better tracking
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "pitchSmoothing", "Pitch Smoothing", 
        juce::NormalisableRange<float>(0.0f, 0.99f, 0.01f), 
        0.80f));  // Smoothing for pitch detection
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "volumeThreshold", "Volume Threshold", 
        juce::NormalisableRange<float>(-60.0f, 0.0f, 0.1f), 
        -40.0f));  // dB threshold for pitch detection
    
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        "basePitchLatch", "Base Pitch Latch", 
        true));  // Enable base pitch latching by default
    
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        "resetBasePitch", "Reset Base Pitch", 
        false));  // Momentary button to reset the latch
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "flattenSensitivity", "Flatten Sensitivity", 
        juce::NormalisableRange<float>(0.0f, 50.0f, 0.1f), 
        1.0f));  // Percentage threshold - ignore variations smaller than this
    
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        "hardFlattenMode", "Hard Flatten Mode", 
        false));  // Force output to latched pitch exactly
    
    // Detection filter parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "detectionHighpass", "Detection Highpass", 
        juce::NormalisableRange<float>(20.0f, 2000.0f, 10.0f, 0.5f), 
        600.0f));  // 600Hz default - cuts out low frequency noise
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "detectionLowpass", "Detection Lowpass", 
        juce::NormalisableRange<float>(1000.0f, 20000.0f, 100.0f, 0.5f), 
        6000.0f));  // 6kHz default - cuts out high frequency noise
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "lookahead", "Lookahead", 
        juce::NormalisableRange<float>(1.0f, 8.0f, 0.5f), 
        2.0f));  // Lookahead multiplier for block size
    
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        "pitchAlgorithm", "Pitch Algorithm", 
        juce::StringArray{"YIN", "WORLD (DIO) FFT"}, 
        1));  // Default to WORLD (DIO)
    
    // WORLD DIO specific parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "dioSpeed", "DIO Speed", 
        juce::NormalisableRange<float>(1.0f, 12.0f, 1.0f), 
        1.0f));  // 1 = fastest, 12 = most accurate
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "dioFramePeriod", "DIO Frame Period", 
        juce::NormalisableRange<float>(1.0f, 10.0f, 0.5f), 
        2.0f));  // milliseconds
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "dioAllowedRange", "DIO Allowed Range", 
        juce::NormalisableRange<float>(0.1f, 1.0f, 0.01f), 
        0.1f));  // Threshold for F0 contour fixing
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "dioChannelsInOctave", "DIO Channels/Octave", 
        juce::NormalisableRange<float>(2.0f, 24.0f, 1.0f), 
        2.0f));  // Frequency resolution
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "dioBufferTime", "DIO Buffer Time", 
        juce::NormalisableRange<float>(0.1f, 1.5f, 0.1f), 
        0.5f));  // Buffer time in seconds (limited to 1.5s to prevent crashes)
    
    return { params.begin(), params.end() };
}

const juce::String PitchFlattenerAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool PitchFlattenerAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool PitchFlattenerAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool PitchFlattenerAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double PitchFlattenerAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int PitchFlattenerAudioProcessor::getNumPrograms()
{
    return 1;
}

int PitchFlattenerAudioProcessor::getCurrentProgram()
{
    return 0;
}

void PitchFlattenerAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String PitchFlattenerAudioProcessor::getProgramName (int index)
{
    return {};
}

void PitchFlattenerAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

void PitchFlattenerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    analysisBuffer.setSize(1, analysisBufferSize);
    analysisBuffer.clear();
    analysisBufferWritePos = 0;
    
    filteredAnalysisBuffer.setSize(1, analysisBufferSize);
    filteredAnalysisBuffer.clear();
    
    pitchDetector->prepare(sampleRate);
    pitchEngine->prepare(sampleRate, samplesPerBlock);
    
    targetPitch.store(*parameters.getRawParameterValue("targetPitch"));
    
    // Initialize detection filters
    float highpassFreq = *parameters.getRawParameterValue("detectionHighpass");
    float lowpassFreq = *parameters.getRawParameterValue("detectionLowpass");
    
    detectionHighpass.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, highpassFreq);
    detectionLowpass.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass(sampleRate, lowpassFreq);
    
    detectionHighpass.reset();
    detectionLowpass.reset();
    
    // Initialize DIO delay buffer (max 1.5 seconds to prevent crashes)
    dioDelayBufferSize = static_cast<int>(sampleRate * 1.5); // Max 1.5 seconds
    dioDelayBuffer.setSize(2, dioDelayBufferSize, false, true, false); // Stereo, clear, don't allocate on audio thread
    dioDelayWritePos = 0;
    dioDelayReadPos = 0;
}

void PitchFlattenerAudioProcessor::releaseResources()
{
    if (pitchEngine)
        pitchEngine->reset();
    
    // Clear delay buffer
    dioDelayBuffer.setSize(0, 0);
    dioDelayBufferSize = 0;
    dioDelayWritePos = 0;
    dioDelayReadPos = 0;
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool PitchFlattenerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
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

void PitchFlattenerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // Always update target pitch
    targetPitch.store(*parameters.getRawParameterValue("targetPitch"));
    
    isActive.store(true);  // Always processing
    float smoothingTimeMs = *parameters.getRawParameterValue("smoothingTimeMs");
    float mix = *parameters.getRawParameterValue("mix");
    
    // Convert smoothing time to exponential smoothing coefficient
    float smoothingTimeSec = smoothingTimeMs / 1000.0f;
    float smoothingCoeff = 1.0f - std::exp(-1.0f / (smoothingTimeSec * getSampleRate()));
    
    DBG("PluginProcessor - mix parameter: " << mix << " smoothing time: " << smoothingTimeMs << "ms");
    DBG("Detected pitch: " << detectedPitch.load() << " Hz");
    
    // Mono analysis for pitch detection
    auto* channelData = buffer.getReadPointer(0);
    int numSamples = buffer.getNumSamples();
    
    // Get pitch detection parameters
    int detectionRate = static_cast<int>(*parameters.getRawParameterValue("detectionRate"));
    float pitchThreshold = *parameters.getRawParameterValue("pitchThreshold");
    float minFreq = *parameters.getRawParameterValue("minFreq");
    float maxFreq = *parameters.getRawParameterValue("maxFreq");
    float pitchHoldTimeMs = *parameters.getRawParameterValue("pitchHoldTime");
    float pitchJumpThreshold = *parameters.getRawParameterValue("pitchJumpThreshold");
    float minConfidence = *parameters.getRawParameterValue("minConfidence");
    float pitchSmoothingCoeff = *parameters.getRawParameterValue("pitchSmoothing");
    float volumeThresholdDb = *parameters.getRawParameterValue("volumeThreshold");
    float volumeThreshold = juce::Decibels::decibelsToGain(volumeThresholdDb);
    
    // Get filter parameters and update if changed
    float highpassFreq = *parameters.getRawParameterValue("detectionHighpass");
    float lowpassFreq = *parameters.getRawParameterValue("detectionLowpass");
    
    // Update filters if frequencies have changed
    static float lastHighpass = 0.0f;
    static float lastLowpass = 0.0f;
    if (std::abs(highpassFreq - lastHighpass) > 0.1f || std::abs(lowpassFreq - lastLowpass) > 0.1f)
    {
        detectionHighpass.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass(getSampleRate(), highpassFreq);
        detectionLowpass.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass(getSampleRate(), lowpassFreq);
        lastHighpass = highpassFreq;
        lastLowpass = lowpassFreq;
        
        // Reset filters when coefficients change to avoid clicks
        detectionHighpass.reset();
        detectionLowpass.reset();
    }
    
    // Update pitch detector settings
    pitchDetector->setThreshold(pitchThreshold);
    pitchDetector->setFrequencyBounds(minFreq, maxFreq);
    
    // Set pitch detection algorithm
    int algorithmChoice = static_cast<int>(*parameters.getRawParameterValue("pitchAlgorithm"));
    
    // Only update algorithm if it changed
    if (algorithmChoice != lastAlgorithmChoice)
    {
        pitchDetector->setAlgorithm(static_cast<PitchDetector::Algorithm>(algorithmChoice));
        lastAlgorithmChoice = algorithmChoice;
        DBG("Algorithm changed to: " << (algorithmChoice == 0 ? "YIN" : "WORLD DIO"));
    }
    
    // Update DIO-specific parameters if DIO is selected
    if (algorithmChoice == 1) // WORLD_DIO
    {
        
        int dioSpeed = static_cast<int>(*parameters.getRawParameterValue("dioSpeed"));
        float dioFramePeriod = *parameters.getRawParameterValue("dioFramePeriod");
        float dioAllowedRange = *parameters.getRawParameterValue("dioAllowedRange");
        float dioChannels = *parameters.getRawParameterValue("dioChannelsInOctave");
        float dioBufferTime = *parameters.getRawParameterValue("dioBufferTime");
        
        // Only update if values have changed
        if (dioSpeed != lastDioSpeed)
        {
            pitchDetector->setDIOSpeed(dioSpeed);
            lastDioSpeed = dioSpeed;
        }
        
        if (dioFramePeriod != lastDioFramePeriod)
        {
            pitchDetector->setDIOFramePeriod(dioFramePeriod);
            lastDioFramePeriod = dioFramePeriod;
        }
        
        if (dioAllowedRange != lastDioAllowedRange)
        {
            pitchDetector->setDIOAllowedRange(dioAllowedRange);
            lastDioAllowedRange = dioAllowedRange;
        }
        
        if (dioChannels != lastDioChannels)
        {
            pitchDetector->setDIOChannelsInOctave(dioChannels);
            lastDioChannels = dioChannels;
        }
        
        if (dioBufferTime != lastDioBufferTime)
        {
            pitchDetector->setDIOBufferTime(dioBufferTime);
            lastDioBufferTime = dioBufferTime;
            
            // Also resize the audio delay buffer to match
            // Limit to 1.5 seconds to prevent memory issues
            int newDelaySize = static_cast<int>(getSampleRate() * 1.5); // Max 1.5 seconds
            if (newDelaySize > 0 && newDelaySize != dioDelayBufferSize)
            {
                // Create new buffer with proper size
                juce::AudioBuffer<float> newDelayBuffer(2, newDelaySize);
                newDelayBuffer.clear();
                
                // Swap buffers atomically
                {
                    std::lock_guard<std::mutex> lock(delayBufferMutex);
                    dioDelayBuffer = std::move(newDelayBuffer);
                    dioDelayBufferSize = newDelaySize;
                    dioDelayWritePos.store(0);
                    dioDelayReadPos.store(0);
                }
            }
            
            DBG("DIO Buffer time changed to: " << dioBufferTime << " seconds");
        }
    }
    
    // Calculate current volume level (RMS over the block)
    float rms = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        rms += channelData[i] * channelData[i];
    }
    rms = std::sqrt(rms / numSamples);
    currentVolumeDb.store(juce::Decibels::gainToDecibels(rms, -60.0f));
    
    // Handle pitch detection differently for DIO vs YIN
    if (algorithmChoice == 1) // WORLD_DIO
    {
        // Get DIO buffer time
        float dioBufferTime = *parameters.getRawParameterValue("dioBufferTime");
        int delayInSamples = static_cast<int>(getSampleRate() * dioBufferTime);
        
        // Store incoming audio in delay buffer
        {
            std::lock_guard<std::mutex> lock(delayBufferMutex);
            if (dioDelayBuffer.getNumChannels() > 0 && dioDelayBufferSize > 0)
            {
                for (int channel = 0; channel < totalNumInputChannels; ++channel)
                {
                    const float* inputData = buffer.getReadPointer(channel);
                    int delayChannel = channel % dioDelayBuffer.getNumChannels();
                    float* delayData = dioDelayBuffer.getWritePointer(delayChannel);
                    
                    int writePos = dioDelayWritePos.load();
                    for (int i = 0; i < numSamples; ++i)
                    {
                        if (writePos >= 0 && writePos < dioDelayBufferSize)
                        {
                            delayData[writePos] = inputData[i];
                        }
                        writePos = (writePos + 1) % dioDelayBufferSize;
                    }
                }
                // Update write position once for all channels
                dioDelayWritePos.store((dioDelayWritePos.load() + numSamples) % dioDelayBufferSize);
            }
        }
        
        // Apply detection filters to DIO input
        // Create a temporary buffer for filtered audio
        juce::AudioBuffer<float> filteredBuffer(1, numSamples);
        filteredBuffer.copyFrom(0, 0, channelData, numSamples);
        
        // Apply highpass and lowpass filters
        float* filteredData = filteredBuffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            float sample = filteredData[i];
            sample = detectionHighpass.processSample(sample);
            sample = detectionLowpass.processSample(sample);
            filteredData[i] = sample;
        }
        
        // For DIO, continuously feed filtered samples and get pitch
        float pitch = pitchDetector->detectPitch(filteredData, numSamples);
        
        // Check if we're still in prebuffer phase
        bool inPrebufferPhase = (pitch == 0.0f && smoothedPitch == 0.0f);
        
        // Debug output for pitch detection
        static int debugCounter = 0;
        if (++debugCounter % 10 == 0)  // Log every 10th detection
        {
            DBG("DIO Pitch detection: " << pitch << " Hz, smoothed: " << smoothedPitch << " Hz");
            DBG("DIO Buffer filled: " << (pitchDetector->isDIOBufferFilled() ? "YES" : "NO"));
            DBG("DIO Total samples received: " << pitchDetector->getDIOTotalSamplesReceived());
            if (inPrebufferPhase)
            {
                DBG("DIO: Still in prebuffer phase");
            }
        }
        
        // Only process if volume is above threshold
        if (rms >= volumeThreshold)
        {
            if (pitch > 0)
            {
                // Minimal smoothing for DIO to track changes quickly
                if (smoothedPitch <= 0.0f)
                {
                    smoothedPitch = pitch;
                }
                else
                {
                    // Very light smoothing for responsiveness
                    smoothedPitch += (pitch - smoothedPitch) * 0.8f; // 80% blend for fast tracking
                }
                
                detectedPitch.store(smoothedPitch);
                silenceFrames = 0;
                pitchHoldFrames = 0;
            }
            else
            {
                // During prebuffer phase, keep last known pitch (or 0 if no pitch yet)
                if (smoothedPitch > 0.0f)
                {
                    detectedPitch.store(smoothedPitch);
                }
            }
        }
        else
        {
            // Volume below threshold
            if (pitch <= 0 && ++pitchHoldFrames > 96000) // ~2 seconds at 48kHz
            {
                smoothedPitch = 0.0f; // Reset after extended silence
                detectedPitch.store(0.0f);
            }
        }
        
        // Read delayed audio for processing (aligned with pitch detection)
        dioDelayReadPos.store((dioDelayWritePos.load() - delayInSamples + dioDelayBufferSize) % dioDelayBufferSize);
        
        // During prebuffer phase, output silence but continue to process below
        if (inPrebufferPhase)
        {
            // Clear the buffer to output silence during prebuffer
            for (int channel = 0; channel < totalNumOutputChannels; ++channel)
            {
                buffer.clear(channel, 0, numSamples);
            }
        }
        else
        {
            // After prebuffer, copy delayed audio back to buffer for processing
            {
                std::lock_guard<std::mutex> lock(delayBufferMutex);
                if (dioDelayBuffer.getNumChannels() > 0 && dioDelayBufferSize > 0)
                {
                    for (int channel = 0; channel < totalNumOutputChannels; ++channel)
                    {
                        float* outputData = buffer.getWritePointer(channel);
                        int delayChannel = channel % dioDelayBuffer.getNumChannels();
                        const float* delayData = dioDelayBuffer.getReadPointer(delayChannel);
                        
                        int currentReadPos = dioDelayReadPos.load();
                        for (int i = 0; i < numSamples; ++i)
                        {
                            if (currentReadPos >= 0 && currentReadPos < dioDelayBufferSize)
                            {
                                outputData[i] = delayData[currentReadPos];
                            }
                            else
                            {
                                outputData[i] = 0.0f; // Safety fallback
                            }
                            currentReadPos = (currentReadPos + 1) % dioDelayBufferSize;
                        }
                    }
                }
            }
        }
        
        // Debug output for delay compensation
        static int delayDebugCounter = 0;
        if (++delayDebugCounter % 100 == 0)
        {
            DBG("DIO Delay: " << dioBufferTime << "s = " << delayInSamples << " samples");
            DBG("Write pos: " << dioDelayWritePos << ", Read pos: " << dioDelayReadPos);
            DBG("Prebuffer phase: " << (inPrebufferPhase ? "YES" : "NO"));
        }
    }
    else // YIN algorithm
    {
        // Fill analysis buffer for YIN
        static int detectionCounter = 0;
        for (int i = 0; i < numSamples; ++i)
        {
            analysisBuffer.setSample(0, analysisBufferWritePos, channelData[i]);
            analysisBufferWritePos = (analysisBufferWritePos + 1) % analysisBufferSize;
            detectionCounter++;
            
            // Perform pitch detection at user-specified rate
            if (detectionCounter >= detectionRate)
            {
                detectionCounter = 0;
                
                // Only detect pitch if volume is above threshold
                if (rms >= volumeThreshold)
                {
                    // Create a properly ordered buffer for pitch detection
                    static AudioBuffer<float> orderedBuffer(1, analysisBufferSize);
                    int readPos = analysisBufferWritePos;
                    for (int j = 0; j < analysisBufferSize; ++j)
                    {
                        orderedBuffer.setSample(0, j, analysisBuffer.getSample(0, readPos));
                        readPos = (readPos + 1) % analysisBufferSize;
                    }
                    
                    // Apply bandpass filter to the ordered buffer for pitch detection
                    filteredAnalysisBuffer.copyFrom(0, 0, orderedBuffer, 0, 0, analysisBufferSize);
                    
                    // Process through filters - highpass first, then lowpass
                    // Don't reset filters here as it would clear their state
                    for (int j = 0; j < analysisBufferSize; ++j)
                    {
                        float sample = filteredAnalysisBuffer.getSample(0, j);
                        sample = detectionHighpass.processSample(sample);
                        sample = detectionLowpass.processSample(sample);
                        filteredAnalysisBuffer.setSample(0, j, sample);
                    }
                    
                    float pitch = pitchDetector->detectPitch(filteredAnalysisBuffer.getReadPointer(0), analysisBufferSize);
            
                    // Debug output for pitch detection
                    static int debugCounter = 0;
                    if (++debugCounter % 10 == 0)  // Log every 10th detection
                    {
                        DBG("YIN Pitch detection: " << pitch << " Hz, smoothed: " << smoothedPitch << " Hz");
                        DBG("Filter settings - HP: " << highpassFreq << " Hz, LP: " << lowpassFreq << " Hz");
                    }
                    
                    if (pitch > 0)
            {
                // Calculate confidence based on pitch stability
                static float lastValidPitch = 0.0f;
                static int stablePitchCount = 0;
                float pitchDiff = std::abs(pitch - lastValidPitch);
                
                // Check if pitch jump is within threshold
                bool isValidJump = (lastValidPitch == 0.0f) || (pitchDiff < pitchJumpThreshold);
                
                if (isValidJump)
                {
                    // Track pitch stability
                    if (pitchDiff < 20.0f) // Very stable
                        stablePitchCount = std::min(stablePitchCount + 1, 10);
                    else
                        stablePitchCount = std::max(stablePitchCount - 1, 0);
                    
                    float confidence = static_cast<float>(stablePitchCount) / 10.0f;
                    
                    // Only update if we have sufficient confidence or hold time has expired
                    int holdFrames = static_cast<int>((pitchHoldTimeMs / 1000.0f) * getSampleRate() / detectionRate);
                    static int framesSinceLastUpdate = 0;
                    framesSinceLastUpdate++;
                    
                    if (confidence >= minConfidence || framesSinceLastUpdate > holdFrames)
                    {
                        // Apply smoothing
                        if (smoothedPitch <= 0.0f)
                        {
                            smoothedPitch = pitch;
                        }
                        else
                        {
                            // Use the pitch smoothing parameter
                            smoothedPitch += (pitch - smoothedPitch) * (1.0f - pitchSmoothingCoeff);
                        }
                        
                        detectedPitch.store(smoothedPitch);
                        lastValidPitch = pitch;
                        framesSinceLastUpdate = 0;
                        pitchHoldFrames = 0;
                        silenceFrames = 0;
                        
                        if (debugCounter % 10 == 0)
                        {
                            DBG("Pitch updated - Confidence: " << confidence << " Stable count: " << stablePitchCount);
                        }
                    }
                }
                else if (debugCounter % 10 == 0)
                {
                    DBG("Pitch jump rejected - Diff: " << pitchDiff << " Hz (threshold: " << pitchJumpThreshold << " Hz)");
                }
            }
            else // No pitch detected (for either algorithm)
            {
                silenceFrames++;
                if (++pitchHoldFrames > 96000) // ~2 seconds at 48kHz
                {
                    smoothedPitch = 0.0f; // Reset after silence
                    detectedPitch.store(0.0f);
                }
                
                        // Just track silence frames
                    }
                }  // End of volume threshold check
                else
                {
                    // Volume below threshold - count as silence
                    silenceFrames++;
                    if (++pitchHoldFrames > 96000) // ~2 seconds at 48kHz
                    {
                        smoothedPitch = 0.0f; // Reset after silence
                        detectedPitch.store(0.0f);
                    }
                }
            } // End of detection counter check
        } // End of for loop
    } // End of YIN algorithm section
    
    // Get base pitch latch parameters
    bool basePitchLatchEnabled = *parameters.getRawParameterValue("basePitchLatch") > 0.5f;
    bool resetBasePitch = *parameters.getRawParameterValue("resetBasePitch") > 0.5f;
    
    // Handle base pitch reset
    if (resetBasePitch)
    {
        basePitchLocked.store(false);
        latchedBasePitch.store(0.0f);
        hasBasePitch = false;
        // Reset tracking variables
        dampedInputPitch = 0.0f;
        lastSetPitchRatio = 1.0f;
        smoothedPitchRatio = 1.0f;
        pitchTrajectory.clear();
        flattenedTargetPitch = 0.0f;
        lastDetectedPitch = 0.0f;
        // Reset the parameter to false (it's a momentary button)
        auto* resetParam = parameters.getParameter("resetBasePitch");
        if (resetParam)
            resetParam->setValueNotifyingHost(0.0f);
    }
    
    // Process audio through pitch flattener
    bool manualOverride = *parameters.getRawParameterValue("manualOverride") > 0.5f;
    float overrideFreq = *parameters.getRawParameterValue("overrideFreq");
    
    float currentPitch = detectedPitch.load();
    float targetFreq;
    
    if (manualOverride)
    {
        // Use manual override frequency for flattening
        targetFreq = overrideFreq;
        basePitchLocked.store(false);  // Disable latching in manual mode
    }
    else if (basePitchLatchEnabled)
    {
        // Base pitch latching mode
        if (!basePitchLocked.load() && currentPitch > 0)
        {
            // Check pitch stability before latching
            recentPitches.push_back(currentPitch);
            if (recentPitches.size() > pitchHistorySize)
                recentPitches.erase(recentPitches.begin());
            
            // Calculate variance in recent pitches
            if (recentPitches.size() >= pitchHistorySize)
            {
                float avgPitch = 0;
                for (float p : recentPitches)
                    avgPitch += p;
                avgPitch /= recentPitches.size();
                
                float variance = 0;
                for (float p : recentPitches)
                    variance += std::abs(p - avgPitch);
                variance /= recentPitches.size();
                
                // Latch if pitch is stable (low variance)
                if (variance < 10.0f)  // 10Hz variance threshold
                {
                    latchedBasePitch.store(avgPitch);
                    basePitchLocked.store(true);
                    hasBasePitch = true;
                    DBG("Base pitch latched at: " << avgPitch << " Hz");
                }
            }
        }
        
        // Use latched base pitch or target parameter
        if (basePitchLocked.load())
        {
            targetFreq = latchedBasePitch.load();
        }
        else
        {
            targetFreq = *parameters.getRawParameterValue("targetPitch");
        }
    }
    else
    {
        // Normal mode - use target parameter for flattening
        targetFreq = *parameters.getRawParameterValue("targetPitch");
        basePitchLocked.store(false);
    }
    
    // Update atomic targetPitch for UI display
    targetPitch.store(targetFreq);
    
    // Process with delta inversion logic
    float effectivePitchRatio = 1.0f;
    float flattenSensitivity = *parameters.getRawParameterValue("flattenSensitivity");
    bool hardFlattenMode = *parameters.getRawParameterValue("hardFlattenMode") > 0.5f;
    
    // Smooth the input pitch before processing to reduce jitter
    if (currentPitch > 0)
    {
        if (dampedInputPitch <= 0.0f)
            dampedInputPitch = currentPitch;
        else
            dampedInputPitch = 0.8f * dampedInputPitch + 0.2f * currentPitch; // 80/20 smoothing
        
        // Use smoothed pitch for trajectory
        pitchTrajectory.push_back(dampedInputPitch);
        if (pitchTrajectory.size() > trajectorySize)
            pitchTrajectory.erase(pitchTrajectory.begin());
        
        // Calculate trailing average for stable Doppler compensation
        if (pitchTrajectory.size() >= 5)
        {
            trailingAveragePitch = 0.0f;
            int count = std::min(10, static_cast<int>(pitchTrajectory.size()));
            for (int i = pitchTrajectory.size() - count; i < pitchTrajectory.size(); ++i)
            {
                trailingAveragePitch += pitchTrajectory[i];
            }
            trailingAveragePitch /= count;
            
            // Calculate velocity for Doppler detection
            float dt = 0.01f; // ~10ms per measurement
            int n = pitchTrajectory.size();
            
            float v1 = (pitchTrajectory[n-1] - pitchTrajectory[n-2]) / dt;
            float v2 = (pitchTrajectory[n-2] - pitchTrajectory[n-3]) / dt;
            
            pitchVelocity = v1;
            pitchAcceleration = (v1 - v2) / dt;
            
            // Lower threshold for Doppler compensation (was 50.0f)
            if (std::abs(pitchVelocity) > 20.0f) // 20 Hz/s threshold
            {
                // Use trailing average for more stable compensation
                currentPitch = trailingAveragePitch;
            }
            else
            {
                // Use damped pitch for stability
                currentPitch = dampedInputPitch;
            }
        }
        else
        {
            currentPitch = dampedInputPitch;
        }
    }
    
    if (basePitchLatchEnabled && basePitchLocked.load() && currentPitch > 0)
    {
        float lockedBase = latchedBasePitch.load();
        if (lockedBase > 0)
        {
            if (hardFlattenMode)
            {
                // Hard flatten mode - RubberBand expects source/target ratio
                effectivePitchRatio = currentPitch / lockedBase;
            }
            else
            {
                // Use trailing average for stable Doppler curve compensation
                float compensationPitch = (trailingAveragePitch > 0) ? trailingAveragePitch : currentPitch;
                float variationRatio = compensationPitch / lockedBase;
                float variationPercent = std::abs(1.0f - variationRatio) * 100.0f;
                
                if (variationPercent > flattenSensitivity)
                {
                    // RubberBand ratio: source/target
                    effectivePitchRatio = compensationPitch / lockedBase;
                }
                else
                {
                    effectivePitchRatio = 1.0f;
                }
            }
            
            // Smooth the pitch ratio transitions to avoid artifacts
            float ratioSmoothingCoeff = 0.95f; // Heavy smoothing for stability
            smoothedPitchRatio += (effectivePitchRatio - smoothedPitchRatio) * (1.0f - ratioSmoothingCoeff);
            effectivePitchRatio = smoothedPitchRatio;
            
            // Clamp to reasonable bounds
            effectivePitchRatio = std::clamp(effectivePitchRatio, 0.25f, 4.0f);
            
            static int deltaDebugCounter = 0;
            if (++deltaDebugCounter % 50 == 0)
            {
                DBG("Pitch Flatten - Base: " << lockedBase << " Hz, Current: " << currentPitch << " Hz");
                DBG("Hard Flatten: " << (hardFlattenMode ? "ON" : "OFF") << ", Trailing avg: " << trailingAveragePitch << " Hz");
                DBG("Pitch velocity: " << pitchVelocity << " Hz/s, Acceleration: " << pitchAcceleration << " Hz/s²");
                DBG("RubberBand ratio (source/target): " << effectivePitchRatio << ", Last set: " << lastSetPitchRatio);
            }
        }
    }
    else if (currentPitch > 0 && targetFreq > 0)
    {
        // Normal mode - RubberBand ratio: source/target
        effectivePitchRatio = currentPitch / targetFreq;
    }
    
    // Always process to avoid clicks
    if (currentPitch <= 0)
    {
        currentPitch = targetFreq; // Use target as fallback
        effectivePitchRatio = 1.0f;
        smoothedPitchRatio = 1.0f; // Reset smoothing
        dampedInputPitch = 0.0f; // Reset damping
        lastSetPitchRatio = 1.0f; // Reset ratio tracking
        flattenedTargetPitch = 0.0f; // Reset flattened target
        lastDetectedPitch = 0.0f; // Reset pitch tracking
    }
    
    // Debug output
    static int processDebugCounter = 0;
    if (++processDebugCounter % 50 == 0)
    {
        DBG("PluginProcessor - Detected: " << currentPitch << " Hz -> Target: " << targetFreq << " Hz");
        DBG("Manual Override: " << (manualOverride ? "ON" : "OFF") << " Mix: " << mix);
        DBG("Effective pitch ratio: " << effectivePitchRatio);
        if (basePitchLocked.load())
            DBG("Base Pitch Locked at: " << latchedBasePitch.load() << " Hz");
    }
    
    // Get lookahead parameter
    float lookahead = *parameters.getRawParameterValue("lookahead");
    
    // Only update RubberBand if ratio has changed significantly (5% threshold)
    float ratioDelta = std::abs(effectivePitchRatio - lastSetPitchRatio);
    if (ratioDelta > 0.05f || lastSetPitchRatio == 1.0f)
    {
        // RubberBand setParameters expects: source pitch, target pitch
        // So for ratio 2.0 (up an octave): source=440, target=220
        float targetPitchForEngine = currentPitch / effectivePitchRatio;
        pitchEngine->setParameters(currentPitch, targetPitchForEngine, smoothingCoeff, lookahead);
        lastSetPitchRatio = effectivePitchRatio;
    }
    
    pitchEngine->process(buffer, mix);
}

bool PitchFlattenerAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* PitchFlattenerAudioProcessor::createEditor()
{
    return new PitchFlattenerAudioProcessorEditor (*this);
}

void PitchFlattenerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void PitchFlattenerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
 
    if (xmlState.get() != nullptr)
        if (xmlState->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PitchFlattenerAudioProcessor();
}