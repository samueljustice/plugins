#pragma once

#include <JuceHeader.h>
#include <vector>

class StretchArmstrongAudioProcessor;

class WaveformVisualizer : public juce::Component,
                           public juce::OpenGLRenderer,
                           private juce::Timer
{
public:
    WaveformVisualizer(StretchArmstrongAudioProcessor& processor);
    ~WaveformVisualizer() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // OpenGL callbacks
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

private:
    void timerCallback() override;

    StretchArmstrongAudioProcessor& audioProcessor;
    juce::OpenGLContext openGLContext;

    // Cached waveform data
    std::vector<float> inputWaveform;
    std::vector<float> outputWaveform;

    // Display settings
    float thresholdDb = -30.0f;
    float envelopeValue = 0.0f;
    bool isStretching = false;
    float currentSignalLevel = -100.0f;

    // OpenGL shader
    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    juce::String vertexShader;
    juce::String fragmentShader;

    // Vertex buffer for waveform
    struct Vertex
    {
        float position[2];
        float color[4];
    };

    std::vector<Vertex> inputVertices;
    std::vector<Vertex> outputVertices;
    std::vector<Vertex> thresholdVertices;
    std::vector<Vertex> envelopeVertices;

    GLuint inputVBO = 0;
    GLuint outputVBO = 0;
    GLuint thresholdVBO = 0;
    GLuint envelopeVBO = 0;

    void updateWaveformData();
    void createShaders();
    void drawWaveform(const std::vector<Vertex>& vertices, GLuint vbo);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformVisualizer)
};
