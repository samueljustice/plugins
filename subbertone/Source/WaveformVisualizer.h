#pragma once

#include <JuceHeader.h>
#include "Theme.h"

class SubbertoneAudioProcessor;

class WaveformVisualizer : public juce::Component, public juce::Timer, public juce::OpenGLRenderer
{
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformVisualizer)

public:
    explicit WaveformVisualizer(SubbertoneAudioProcessor& audioProcessor);
    ~WaveformVisualizer() override;
    
    void paint(juce::Graphics&) override;
    void timerCallback() override;

    void mouseDown(const juce::MouseEvent& event) override;
    
    // OpenGL callbacks
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;
    
    void setSignalText(const juce::String& text, bool aboveThreshold);

    // Visibility toggles
    bool m_showInput = true;
    bool m_showOutput = true;
    bool m_showHarmonicResidual = true;
    
private:
    void drawWaveform3D(const std::vector<std::vector<float>>& history, const juce::Colour& color, float yOffset, bool isInput);
    void drawPerspectiveGrid();
    void drawLegendRow(juce::Graphics& g, int centreY, const juce::String& name, juce::Colour colour, bool on);

    SubbertoneAudioProcessor& m_audioProcessor;
    juce::OpenGLContext m_openGLContext;
    
    // Waveform data with history for trails
    static constexpr int c_historySize = 32;

    int m_historyWritePos = 0;

    std::vector<std::vector<float>> m_inputHistory;
    std::vector<std::vector<float>> m_outputHistory;
    std::vector<std::vector<float>> m_harmonicResidualHistory;

    std::vector<float> m_inputBuffer;
    std::vector<float> m_outputBuffer;
    std::vector<float> m_harmonicResidualBuffer;
    
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;

    const juce::Colour m_bgColor { Theme::Colours::scope };
    const juce::Colour m_gridColor { Theme::Colours::grid };
    const juce::Colour m_inputColor { Theme::Colours::input };
    const juce::Colour m_outputColor { Theme::Colours::output };
    const juce::Colour m_harmonicResidualColor { Theme::Colours::harmonics };

    juce::String m_signalText { "IN  -inf dB" };

    bool m_signalAboveThreshold = false;
    
    // Animation
    float m_timeValue = 0.0f;
    float m_cameraZ = -8.0f;
    float m_rotationX = 0.5f;  // More dramatic angle
    float m_rotationY = 0.0f;
    float m_targetRotationY = 0.0f;
    float m_targetCameraZ = -8.0f;
};

