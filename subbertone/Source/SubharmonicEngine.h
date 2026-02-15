#pragma once

#include <JuceHeader.h>
#include <atomic>

class SubharmonicEngine
{
public:
    SubharmonicEngine()  = default;
    ~SubharmonicEngine() = default;
    
    void prepare(double sampleRate, int maxBlockSize);
    void process(float* outputBuffer, int numSamples, float fundamental, float distortionAmount, int distortionType, float toneFreq, float postDriveLowpass, bool inputActive);
    
    // Get harmonic residual buffer for visualization
    const std::vector<float>& getHarmonicResidualBuffer() const { return m_harmonicResidualBuffer; }
    
private:
    void updateEnvelope(bool signalDetected);
    void calculateEnvelopeCoefficients();
    float applyDistortion(float sample, float amount, int type);

    static constexpr int c_maxBlockSizeSamples = 8192;
    static constexpr float c_minSignalFrequency = 20.0f;
    static constexpr float c_maxSignalFrequency = 2000.0f;
    static constexpr float c_minToneHz = 40.0f;
    static constexpr float c_maxToneHz = 20000.0f;
    static constexpr float c_envelopeSilenceThreshold = 0.0001f;
    static constexpr float c_sineHeadroom = 0.7f;
    static constexpr float c_lowFreqSmoothingHz = 100.0f;
    static constexpr double c_parameterSmoothingSeconds = 0.02;

    // Core parameters
    double m_sampleRate = 0.0;
    juce::dsp::Oscillator<float> m_sineOscillator { [](float x) { return std::sin(x); } };
    
    // Frequency management
    double m_currentFrequency = 0.0;
    double m_targetFrequency = 0.0;
    double m_lastSetFrequency = 0.0;
    static constexpr double c_frequencySmoothingCoeff = 0.99;
    
    // Envelope parameters
    double m_envelopeFollower = 0.0;
    double m_envelopeTarget = 0.0;
    double m_attackCoeff = 0.0;
    double m_releaseCoeff = 0.0;
    static constexpr double c_attackTimeMs = 20.0;
    static constexpr double c_releaseTimeMs = 100.0;
    static constexpr double c_envelopeFloor = 0.05;
    
    // Signal detection
    bool m_signalPresent = false;
    double m_releaseGain = 1.0;
    int m_signalOnCounter = 0;
    int m_signalOffCounter = 0;
    int m_signalOnThreshold = 64;     // Computed in prepare()
    int m_signalOffThreshold = 24000; // Computed in prepare()
    
    // Filters
    juce::dsp::IIR::Filter<float> m_dcBlockingFilter;
    juce::dsp::StateVariableTPTFilter<float> m_toneFilter;
    juce::dsp::StateVariableTPTFilter<float> m_postDriveLowpassFilter;
    juce::dsp::StateVariableTPTFilter<float> m_highpassFilter;
    juce::dsp::StateVariableTPTFilter<float> m_postSubtractionFilter;
    juce::dsp::StateVariableTPTFilter<float> m_lowFreqSmoothingFilter;
    juce::dsp::IIR::Filter<float> m_preDistortionFilter;
    juce::dsp::IIR::Filter<float> m_antiAliasingFilter1;
    juce::dsp::IIR::Filter<float> m_antiAliasingFilter2;
    int m_currentMaxBlockSize = 0;
    
    // Distortion types
    enum DistortionType
    {
        SoftClip = 0,
        HardClip,
        Tube,
        Foldback
    };
    
    // Processing buffers
    std::vector<float> m_sineBuffer;
    std::vector<float> m_cleanSineBuffer;
    std::vector<float> m_harmonicResidualBuffer;

    // Smoothed parameters (sample-accurate control)
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_distortionSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_toneSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_postDriveLowpassSmoothed;

    static constexpr int c_filterUpdateInterval = 16;
    int m_filterUpdateCounter = 0;
    
    // Thread safety
    std::atomic<bool> m_isPrepared{false};
};
