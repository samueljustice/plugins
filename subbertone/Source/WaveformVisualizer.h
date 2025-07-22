#pragma once

#include <JuceHeader.h>

class SubbertoneAudioProcessor;

class WaveformVisualizer : public juce::Component,
                           public juce::Timer,
                           public juce::OpenGLRenderer
{
public:
    WaveformVisualizer(SubbertoneAudioProcessor& processor);
    ~WaveformVisualizer() override;
    
    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;
    void mouseDown(const juce::MouseEvent& event) override;
    
    // OpenGL callbacks
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;
    
    // Visibility toggles
    bool showInput = true;
    bool showOutput = true;
    bool showHarmonicResidual = true;
    
private:
    SubbertoneAudioProcessor& audioProcessor;
    juce::OpenGLContext openGLContext;
    
    // Waveform data with history for trails
    static constexpr int historySize = 32;
    std::vector<std::vector<float>> inputHistory;
    std::vector<std::vector<float>> outputHistory;
    std::vector<std::vector<float>> harmonicResidualHistory;
    int historyWritePos = 0;
    
    // PS1 Wipeout style colors
    const juce::Colour bgColor{ 0xff000510 };     // Deep blue-black
    const juce::Colour gridColor{ 0xff0a2a4a };   // Dark blue grid
    const juce::Colour inputColor{ 0xff00ffff };   // Cyan
    const juce::Colour outputColor{ 0xffff00ff };  // Magenta
    const juce::Colour harmonicResidualColor{ 0xffffff00 };  // Yellow
    const juce::Colour glowColor{ 0xff4080ff };   // Blue glow
    const juce::Colour textColor{ 0xffffffff };
    
    // Animation
    float timeValue = 0.0f;
    float gridDepth = 0.0f;
    float cameraZ = -8.0f;
    float rotationX = 0.5f;  // More dramatic angle
    float rotationY = 0.0f;
    float targetRotationY = 0.0f;
    float targetCameraZ = -8.0f;
    
    // OpenGL resources
    std::unique_ptr<juce::OpenGLShaderProgram> waveformShader;
    std::unique_ptr<juce::OpenGLShaderProgram> gridShader;
    std::unique_ptr<juce::OpenGLShaderProgram> glowShader;
    
    GLuint vaoWaveform = 0, vboWaveform = 0;
    GLuint vaoGrid = 0, vboGrid = 0;
    
    void createShaders();
    void setupVertexArrays();
    void drawWaveform3D(const std::vector<std::vector<float>>& history, 
                       const juce::Colour& color, float yOffset, bool isInput);
    void drawPerspectiveGrid();
    void drawTextOverlay();
    juce::Matrix3D<float> getProjectionMatrix() const;
    juce::Matrix3D<float> getViewMatrix() const;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformVisualizer)
};