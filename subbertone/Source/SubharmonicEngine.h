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
    double sampleRateReciprocal = 1.0 / 44100.0;
    
    // PolyBLEP Oscillator
    double phase = 0.0;
    double phaseIncrement = 0.0;
    double lastPhaseIncrement = 0.0;
    
    // Frequency management
    double currentFrequency = 0.0;
    double targetFrequency = 0.0;
    static constexpr double frequencySmoothingCoeff = 0.999; // Much smoother frequency transitions
    
    // Envelope - using one-pole filters for smooth transitions
    double envelopeFollower = 0.0;
    double envelopeTarget = 0.0;
    double attackCoeff = 0.0;
    double releaseCoeff = 0.0;
    static constexpr double attackTimeMs = 10.0;  // 10ms attack
    static constexpr double releaseTimeMs = 100.0; // 100ms release
    
    // Signal detection with hysteresis
    bool signalPresent = false;
    int silenceCounter = 0;
    static constexpr int silenceThreshold = 2048; // ~43ms at 48kHz
    
    // DC offset removal
    double dcBlockerState = 0.0;
    static constexpr double dcBlockerCoeff = 0.995;
    
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
    inline double polyBlepResidue(double t);
    double generatePolyBlepSine();
    void updateEnvelope(bool signalDetected);
    
    // Calculate coefficients
    void calculateEnvelopeCoefficients();
};