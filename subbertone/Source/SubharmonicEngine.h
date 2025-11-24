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
                 int distortionType, float toneFreq, float postDriveLowpass, bool inverseMixMode = false);
    
    // Get harmonic residual buffer for visualization
    const std::vector<float>& getHarmonicResidualBuffer() const { return harmonicResidualBuffer; }
    
    private:
    // Core parameters
    double sampleRate = 44100.0;
    
    // Frequency management
    double currentFrequency = 0.0;
    double targetFrequency = 0.0;
    double currentAngle = 0.0;
    double angleDelta = 0.0;
    double targetAngleDelta = 0.0;
    double phaseIncrement = 0.0;
    static constexpr double maxDeltaChangePerSample = 0.0001;
    
    // Envelope parameters
    double envelopeFollower = 0.0;
    double envelopeTarget = 0.0;
    double sampleEnvelope = 0.0;
    double attackCoeff = 0.0;
    double releaseCoeff = 0.0;
    static constexpr double attackTimeMs = 20.0;
    static constexpr double releaseTimeMs = 100.0;
    static constexpr double envelopeFloor = 0.05;
    
    // Signal detection
    bool signalPresent = false;
    int signalOnCounter = 0;
    int signalOffCounter = 0;
    int settlingCounter = 0;
    static constexpr int signalOnThreshold = 64;
    static constexpr int signalOffThreshold = 960;
    static constexpr int settlingThreshold = 128;
    
    // Filters
    juce::dsp::StateVariableTPTFilter<float> toneFilter;
    juce::dsp::StateVariableTPTFilter<float> postDriveLowpassFilter;
    juce::dsp::StateVariableTPTFilter<float> highpassFilter;
    juce::dsp::StateVariableTPTFilter<float> postSubtractionFilter;
    
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
