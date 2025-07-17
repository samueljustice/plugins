#pragma once

#include <JuceHeader.h>
#include "PitchDetector.h"
#include "PitchFlattenerEngine.h"
#include <mutex>

class PitchFlattenerAudioProcessor : public juce::AudioProcessor
{
public:
    PitchFlattenerAudioProcessor();
    ~PitchFlattenerAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

#ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
#endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    
    juce::AudioProcessorValueTreeState parameters;
    
    float getDetectedPitch() const { return detectedPitch.load(); }
    float getTargetPitch() const { return targetPitch.load(); }
    bool isProcessing() const { return isActive.load(); }
    float getBasePitch() const { return basePitch; }
    bool getHasBasePitch() const { return hasBasePitch; }
    float getCurrentVolumeDb() const { return currentVolumeDb.load(); }
    float getLatchedBasePitch() const { return latchedBasePitch.load(); }
    bool isBasePitchLocked() const { return basePitchLocked.load(); }
    void resetLatchedBasePitch() { 
        latchedBasePitch.store(0.0f); 
        basePitchLocked.store(false);
        hasBasePitch = false;
        basePitch = 0.0f;
        frozenPitchRatio = 1.0f;
        wasFreezeEnabled = false;
    }
    
    // Audio data access for FFT visualization
    void getLatestAudioBlock(float* buffer, int numSamples);
    bool isUsingDIO() const;
    
    // Get the current pitch ratio for visualization
    float getCurrentPitchRatio() const;

private:
    std::unique_ptr<PitchDetector> pitchDetector;
    std::unique_ptr<PitchFlattenerEngine> pitchEngine;
    
    std::atomic<float> detectedPitch{0.0f};
    std::atomic<float> targetPitch{440.0f};
    std::atomic<bool> isActive{false};
    std::atomic<float> currentVolumeDb{-60.0f};
    
    // Base pitch latching
    std::atomic<float> latchedBasePitch{0.0f};
    std::atomic<bool> basePitchLocked{false};
    
    // Pitch tracking state
    float smoothedPitch = 0.0f;
    float basePitch = 0.0f;  // The initial pitch to flatten to
    bool hasBasePitch = false;  // Whether we've captured a base pitch
    int pitchHoldFrames = 0;
    int silenceFrames = 0;  // Frames of silence to reset base pitch
    int lastAlgorithmChoice = -1;  // Track algorithm changes
    float frozenPitchRatio = 1.0f;  // Locked pitch ratio for hard flatten mode
    bool wasFreezeEnabled = false;  // Track freeze mode state changes
    
    // DIO parameter tracking
    int lastDioSpeed = -1;
    float lastDioFramePeriod = -1.0f;
    float lastDioAllowedRange = -1.0f;
    float lastDioChannels = -1.0f;
    float lastDioBufferTime = -1.0f;
    
    // Pitch stability tracking
    std::vector<float> recentPitches;
    static constexpr int pitchHistorySize = 5;
    int stablePitchCount = 0;
    
    // Pitch trajectory tracking for Doppler compensation
    std::vector<float> pitchTrajectory;
    static constexpr int trajectorySize = 20;  // ~200ms at 100Hz update rate
    float pitchVelocity = 0.0f;  // Hz per second
    float pitchAcceleration = 0.0f;  // Hz per second squared
    float smoothedPitchRatio = 1.0f;  // For smooth Rubber Band transitions
    float lastSetPitchRatio = 1.0f;  // Track last ratio sent to RubberBand
    float dampedInputPitch = 0.0f;  // Smoothed input pitch for stability
    float trailingAveragePitch = 0.0f;  // Average pitch over trajectory window
    
    // Pitch movement tracking for true flattening
    float lastDetectedPitch = 0.0f;  // Previous pitch for delta calculation
    float pitchDelta = 0.0f;  // Hz change per block
    float pitchSlope = 0.0f;  // Hz per second slope
    float flattenedTargetPitch = 0.0f;  // The stable pitch we're flattening to
    
    juce::AudioBuffer<float> analysisBuffer;
    int analysisBufferWritePos = 0;
    static constexpr int analysisBufferSize = 2048;  // Optimized for pitch detection
    
    // Audio delay buffer for DIO compensation
    juce::AudioBuffer<float> dioDelayBuffer;
    int dioDelayBufferSize = 0;
    std::atomic<int> dioDelayWritePos{0};
    std::atomic<int> dioDelayReadPos{0};
    mutable std::mutex delayBufferMutex;
    
    // Bandpass filter for pitch detection
    juce::dsp::IIR::Filter<float> detectionHighpass;
    juce::dsp::IIR::Filter<float> detectionLowpass;
    juce::AudioBuffer<float> filteredAnalysisBuffer;
    
    // Audio buffer for FFT visualization
    juce::AudioBuffer<float> visualizationBuffer;
    std::mutex visualizationBufferMutex;
    int visualizationBufferWritePos = 0;
    
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchFlattenerAudioProcessor)
};