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
    
    // Oscillator
    double phase = 0.0;
    double currentFrequency = 0.0;
    double targetFrequency = 0.0;
    
    // Frequency slewing
    static constexpr double frequencySlewRate = 0.95; // How quickly frequency changes (0.0 = instant, 0.99 = very slow)
    static constexpr double frequencyThreshold = 0.5; // Hz - ignore smaller changes
    
    // ADSR Envelope
    enum class EnvelopeState
    {
        Idle,
        Attack,
        Sustain,
        Release
    };
    
    EnvelopeState envelopeState = EnvelopeState::Idle;
    double envelopeLevel = 0.0;
    double attackTime = 0.005;  // 5ms attack
    double releaseTime = 0.05;  // 50ms release
    double attackRate = 0.0;
    double releaseRate = 0.0;
    
    // Signal detection
    bool signalDetected = false;
    bool previousSignalDetected = false;
    int signalHoldoffCounter = 0;
    static constexpr int signalHoldoffSamples = 512; // Hold signal for 512 samples to avoid flutter
    
    // Filters for smoothing
    juce::dsp::StateVariableTPTFilter<float> lowpassFilter;      // Pre-distortion tone control
    juce::dsp::StateVariableTPTFilter<float> postDriveLowpassFilter; // Post-drive lowpass
    juce::dsp::StateVariableTPTFilter<float> highpassFilter;
    
    // Anti-aliasing filters
    juce::dsp::IIR::Filter<float> antiAliasingFilter1;
    juce::dsp::IIR::Filter<float> antiAliasingFilter2;
    juce::dsp::IIR::Filter<float> antiAliasingFilter3;
    juce::dsp::IIR::Filter<float> antiAliasingFilter4;
    
    // Pre-distortion filter
    juce::dsp::IIR::Filter<float> preDistortionFilter;
    
    // Oversampling
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    
    // DC blocker
    float dcBlockerX1 = 0.0f;
    float dcBlockerY1 = 0.0f;
    static constexpr float dcBlockerCutoff = 0.999f;
    
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
    std::vector<float> cleanSineBuffer;  // Store clean sine before distortion
    std::vector<float> distortedBuffer;
    
    // Helper functions
    void updateEnvelope();
    double generateSineWave();
};