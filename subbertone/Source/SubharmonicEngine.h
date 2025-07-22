#pragma once

#include <JuceHeader.h>
#include <atomic>

class SubharmonicEngine
{
public:
    SubharmonicEngine();
    ~SubharmonicEngine();
    
    void prepare(double sampleRate, int maxBlockSize);
    void process(float* outputBuffer, int numSamples, float fundamental,
                float distortionAmount, float inverseMixAmount, 
                int distortionType, float toneFreq, float postDriveLowpass);
    
    // Get harmonic residual buffer for visualization
    const std::vector<float>& getHarmonicResidualBuffer() const { return harmonicResidualBuffer; }
    
private:
    // Core parameters
    double sampleRate = 44100.0;
    juce::dsp::Oscillator<float> sineOscillator { [](float x) { return std::sin(x); } };
    
    // Frequency management
    double currentFrequency = 0.0;
    double targetFrequency = 0.0;
    double lastSetFrequency = 0.0;
    static constexpr double frequencySmoothingCoeff = 0.99;
    
    // Envelope parameters
    double envelopeFollower = 0.0;
    double envelopeTarget = 0.0;
    double attackCoeff = 0.0;
    double releaseCoeff = 0.0;
    static constexpr double attackTimeMs = 20.0;
    static constexpr double releaseTimeMs = 100.0;
    static constexpr double envelopeFloor = 0.05;
    
    // Signal detection
    bool signalPresent = false;
    int signalOnCounter = 0;
    int signalOffCounter = 0;
    static constexpr int signalOnThreshold = 64;     // ~1.3ms at 48kHz
    static constexpr int signalOffThreshold = 24000; // ~500ms at 48kHz
    
    // Filters
    juce::dsp::IIR::Filter<float> dcBlockingFilter;
    juce::dsp::StateVariableTPTFilter<float> toneFilter;
    juce::dsp::StateVariableTPTFilter<float> postDriveLowpassFilter;
    juce::dsp::StateVariableTPTFilter<float> highpassFilter;
    juce::dsp::StateVariableTPTFilter<float> postSubtractionFilter;
    juce::dsp::StateVariableTPTFilter<float> lowFreqSmoothingFilter;
    juce::dsp::IIR::Filter<float> preDistortionFilter;
    juce::dsp::IIR::Filter<float> antiAliasingFilter1;
    juce::dsp::IIR::Filter<float> antiAliasingFilter2;
    juce::dsp::IIR::Filter<float> antiAliasingFilter3;
    juce::dsp::IIR::Filter<float> antiAliasingFilter4;
    
    // Oversampling
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    int currentMaxBlockSize = 0;
    bool oversamplerReady = false;
    
    // Distortion types
    enum DistortionType
    {
        SoftClip = 0,
        HardClip,
        Tube,
        Foldback
    };
    
    // Processing buffers
    std::vector<float> sineBuffer;
    std::vector<float> cleanSineBuffer;
    std::vector<float> distortedBuffer;
    std::vector<float> harmonicResidualBuffer;
    
    // Helper functions
    void updateEnvelope(bool signalDetected);
    void calculateEnvelopeCoefficients();
    float applyDistortion(float sample, float amount, int type);
    
    // Thread safety
    std::atomic<bool> isPrepared{false};
    
};