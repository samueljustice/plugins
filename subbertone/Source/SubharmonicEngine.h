#pragma once

#include <JuceHeader.h>

class SubharmonicEngine
{
public:
    SubharmonicEngine();
    ~SubharmonicEngine();
    
    void prepare(double sampleRate, int maxBlockSize);
    void process(const float* inputBuffer, float* outputBuffer, int numSamples, float fundamental,
                float distortionAmount, float inverseMixAmount, 
                int distortionType, float toneFreq, float postDriveLowpass);
    
private:
    double sampleRate = 44100.0;
    
    // Oscillator phase and smoothing
    double phase = 0.0;
    double phaseIncrement = 0.0;
    double targetPhaseIncrement = 0.0;
    double lastPhase = 0.0;
    bool phaseReset = false;
    static constexpr double phaseIncrementSmoothingFactor = 0.95;
    
    // Amplitude envelope
    float currentAmplitude = 0.0f;
    float targetAmplitude = 0.0f;
    int amplitudeRampSamplesRemaining = 0;
    static constexpr int fadeInSamples = 1024; // ~21ms at 48kHz for smoother fade-in
    static constexpr float amplitudeRelease = 0.999f;
    
    // Phase management
    double lastStableIncrement = 0.0;
    
    // Frequency tracking
    float lastFundamental = 0.0f;
    float lockedFundamental = 0.0f; // The frequency we're locked to
    float smoothedFrequency = 0.0f; // Smoothed frequency for phase increment calculation
    int lockCounter = 0; // How many buffers we've been locked
    static constexpr int lockDuration = 100; // Lock frequency for 100 buffers (~1 second at 512 samples/48kHz)
    static constexpr float maxFrequencyJump = 100.0f; // Hz
    static constexpr float frequencyJitterThreshold = 1.0f; // Hz - ignore changes smaller than this
    
    // Filters for smoothing
    juce::dsp::StateVariableTPTFilter<float> lowpassFilter;      // Pre-distortion tone control
    juce::dsp::StateVariableTPTFilter<float> postDriveLowpassFilter; // Post-drive lowpass
    juce::dsp::StateVariableTPTFilter<float> highpassFilter;
    
    // Anti-aliasing filters - using IIR for steeper rolloff
    juce::dsp::IIR::Filter<float> antiAliasingFilter1;
    juce::dsp::IIR::Filter<float> antiAliasingFilter2;
    juce::dsp::IIR::Filter<float> antiAliasingFilter3;
    juce::dsp::IIR::Filter<float> antiAliasingFilter4;
    
    // Pre-distortion filter to limit harmonic content
    juce::dsp::IIR::Filter<float> preDistortionFilter;
    
    // Oversampling for anti-aliasing
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    
    // DC blocker
    float dcBlockerX1 = 0.0f;
    float dcBlockerY1 = 0.0f;
    static constexpr float dcBlockerCutoff = 0.999f; // Higher for more stability
    
    // Distortion types
    enum DistortionType
    {
        SoftClip = 0,
        HardClip,
        Tube,
        Foldback
    };
    
    // Distortion
    float applyDistortion(float sample, float amount, int type);
    
    // Temporary buffers
    std::vector<float> sineBuffer;
    std::vector<float> distortedBuffer;
    
    // Delay compensation buffer for phase alignment
    std::vector<float> delayBuffer;
    int delayBufferSize = 32; // Compensate for oversampling latency
    int delayWritePos = 0;
    
    // Final output smoothing
    float outputSmoothPrev = 0.0f;
    
    // Debug logging
    std::unique_ptr<juce::FileOutputStream> debugFileStream;
    juce::File debugFile;
    int debugSampleCount = 0;
    int generationSampleCount = 0; // Track samples since generation started
    static constexpr int debugMaxSamples = 50000; // Log first 50k samples to catch more data
    void openDebugFile();
    void closeDebugFile();
    void logDebugData(float fundamental, float phase, float phaseInc, float output);
    void writeDebugLine(const juce::String& text);
    bool isDebugFileOpen() const;
};