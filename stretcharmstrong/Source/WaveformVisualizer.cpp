#include "WaveformVisualizer.h"
#include "PluginProcessor.h"
#include <cmath>

using namespace juce::gl;

WaveformVisualizer::WaveformVisualizer(StretchArmstrongAudioProcessor& processor)
    : audioProcessor(processor)
{
    openGLContext.setRenderer(this);
    openGLContext.attachTo(*this);
    openGLContext.setContinuousRepainting(true);

    startTimerHz(30);
}

WaveformVisualizer::~WaveformVisualizer()
{
    stopTimer();
    openGLContext.detach();
}

void WaveformVisualizer::timerCallback()
{
    thresholdDb = audioProcessor.getThresholdDb();
    envelopeValue = audioProcessor.getEnvelopeValue();
    isStretching = audioProcessor.isStretching();
    currentSignalLevel = audioProcessor.getCurrentSignalLevel();

    inputWaveform = audioProcessor.getInputWaveform();
    outputWaveform = audioProcessor.getOutputWaveform();

    repaint();
}

void WaveformVisualizer::paint(juce::Graphics& g)
{
    // OpenGL handles the main rendering, but we draw text overlays here
    g.setColour(juce::Colour(0xffffffff));
    g.setFont(juce::Font(juce::FontOptions(12.0f)));

    // Draw status text
    juce::String status = isStretching ? "STRETCHING" : "IDLE";
    juce::Colour statusColor = isStretching ? juce::Colour(0xff00ff00) : juce::Colour(0xff888888);
    g.setColour(statusColor);
    g.drawText(status, 10, 10, 100, 20, juce::Justification::left);

    // Draw signal level
    g.setColour(juce::Colour(0xffffffff));
    g.drawText(juce::String(currentSignalLevel, 1) + " dB", getWidth() - 80, 10, 70, 20, juce::Justification::right);

    // Draw envelope value
    g.setColour(juce::Colour(0xffff00ff));
    g.drawText("ENV: " + juce::String(envelopeValue * 100.0f, 0) + "%", 10, getHeight() - 25, 100, 20, juce::Justification::left);

    // Draw threshold indicator
    g.setColour(juce::Colour(0xffff6600));
    g.drawText("THR: " + juce::String(thresholdDb, 1) + " dB", getWidth() - 120, getHeight() - 25, 110, 20, juce::Justification::right);
}

void WaveformVisualizer::resized()
{
}

void WaveformVisualizer::newOpenGLContextCreated()
{
    createShaders();

    // Generate VBOs
    openGLContext.extensions.glGenBuffers(1, &inputVBO);
    openGLContext.extensions.glGenBuffers(1, &outputVBO);
    openGLContext.extensions.glGenBuffers(1, &thresholdVBO);
    openGLContext.extensions.glGenBuffers(1, &envelopeVBO);
}

void WaveformVisualizer::renderOpenGL()
{
    jassert(juce::OpenGLHelpers::isContextActive());

    auto desktopScale = static_cast<float>(openGLContext.getRenderingScale());
    juce::OpenGLHelpers::clear(juce::Colour(0xff0a0a0a));

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);

    auto bounds = getLocalBounds().toFloat() * desktopScale;
    glViewport(0, 0, static_cast<GLsizei>(bounds.getWidth()), static_cast<GLsizei>(bounds.getHeight()));

    updateWaveformData();

    if (shader)
    {
        shader->use();

        // Draw threshold line
        if (!thresholdVertices.empty())
        {
            glLineWidth(2.0f);
            drawWaveform(thresholdVertices, thresholdVBO);
        }

        // Draw input waveform (cyan, semi-transparent)
        if (!inputVertices.empty())
        {
            glLineWidth(1.5f);
            drawWaveform(inputVertices, inputVBO);
        }

        // Draw output waveform (magenta when stretching, white otherwise)
        if (!outputVertices.empty())
        {
            glLineWidth(2.0f);
            drawWaveform(outputVertices, outputVBO);
        }

        // Draw envelope bar at bottom
        if (!envelopeVertices.empty())
        {
            drawWaveform(envelopeVertices, envelopeVBO);
        }
    }

    glDisable(GL_BLEND);
    glDisable(GL_LINE_SMOOTH);
}

void WaveformVisualizer::openGLContextClosing()
{
    shader.reset();

    if (inputVBO != 0)
        openGLContext.extensions.glDeleteBuffers(1, &inputVBO);
    if (outputVBO != 0)
        openGLContext.extensions.glDeleteBuffers(1, &outputVBO);
    if (thresholdVBO != 0)
        openGLContext.extensions.glDeleteBuffers(1, &thresholdVBO);
    if (envelopeVBO != 0)
        openGLContext.extensions.glDeleteBuffers(1, &envelopeVBO);

    inputVBO = outputVBO = thresholdVBO = envelopeVBO = 0;
}

void WaveformVisualizer::createShaders()
{
    vertexShader =
        "attribute vec2 position;\n"
        "attribute vec4 color;\n"
        "varying vec4 fragColor;\n"
        "void main()\n"
        "{\n"
        "    gl_Position = vec4(position, 0.0, 1.0);\n"
        "    fragColor = color;\n"
        "}\n";

    fragmentShader =
        "varying vec4 fragColor;\n"
        "void main()\n"
        "{\n"
        "    gl_FragColor = fragColor;\n"
        "}\n";

    shader = std::make_unique<juce::OpenGLShaderProgram>(openGLContext);

    if (!shader->addVertexShader(vertexShader) ||
        !shader->addFragmentShader(fragmentShader) ||
        !shader->link())
    {
        DBG("Shader compilation failed: " + shader->getLastError());
        shader.reset();
    }
}

void WaveformVisualizer::updateWaveformData()
{
    auto bounds = getLocalBounds().toFloat();
    float width = bounds.getWidth();
    float height = bounds.getHeight();

    if (width <= 0 || height <= 0)
        return;

    // Threshold line (horizontal, orange)
    float thresholdLinear = juce::Decibels::decibelsToGain(thresholdDb);
    float thresholdY = -thresholdLinear; // Convert to normalized coordinates (-1 to 1)

    thresholdVertices.clear();
    thresholdVertices.push_back({{-1.0f, thresholdY}, {1.0f, 0.4f, 0.0f, 0.8f}});
    thresholdVertices.push_back({{1.0f, thresholdY}, {1.0f, 0.4f, 0.0f, 0.8f}});
    // Also draw negative threshold
    thresholdVertices.push_back({{-1.0f, -thresholdY}, {1.0f, 0.4f, 0.0f, 0.8f}});
    thresholdVertices.push_back({{1.0f, -thresholdY}, {1.0f, 0.4f, 0.0f, 0.8f}});

    // Input waveform (cyan)
    inputVertices.clear();
    if (!inputWaveform.empty())
    {
        size_t numSamples = inputWaveform.size();
        float xStep = 2.0f / static_cast<float>(numSamples);

        for (size_t i = 0; i < numSamples; ++i)
        {
            float x = -1.0f + static_cast<float>(i) * xStep;
            float y = inputWaveform[i] * 0.8f; // Scale to fit
            inputVertices.push_back({{x, y}, {0.0f, 1.0f, 1.0f, 0.5f}});
        }
    }

    // Output waveform (magenta when stretching, white otherwise)
    outputVertices.clear();
    if (!outputWaveform.empty())
    {
        size_t numSamples = outputWaveform.size();
        float xStep = 2.0f / static_cast<float>(numSamples);

        float r = isStretching ? 1.0f : 1.0f;
        float g = isStretching ? 0.0f : 1.0f;
        float b = isStretching ? 1.0f : 1.0f;

        for (size_t i = 0; i < numSamples; ++i)
        {
            float x = -1.0f + static_cast<float>(i) * xStep;
            float y = outputWaveform[i] * 0.8f;
            outputVertices.push_back({{x, y}, {r, g, b, 0.9f}});
        }
    }

    // Envelope bar at bottom
    envelopeVertices.clear();
    float envWidth = envelopeValue * 2.0f; // Scale to -1 to 1 range
    float barHeight = 0.05f;
    float barY = -0.95f;

    // Background bar (dark)
    envelopeVertices.push_back({{-1.0f, barY}, {0.2f, 0.2f, 0.2f, 1.0f}});
    envelopeVertices.push_back({{1.0f, barY}, {0.2f, 0.2f, 0.2f, 1.0f}});
    envelopeVertices.push_back({{1.0f, barY + barHeight}, {0.2f, 0.2f, 0.2f, 1.0f}});
    envelopeVertices.push_back({{-1.0f, barY + barHeight}, {0.2f, 0.2f, 0.2f, 1.0f}});

    // Filled envelope bar (green gradient)
    if (envelopeValue > 0.001f)
    {
        float fillRight = -1.0f + envWidth;
        envelopeVertices.push_back({{-1.0f, barY}, {0.0f, 0.8f, 0.2f, 1.0f}});
        envelopeVertices.push_back({{fillRight, barY}, {0.0f, 1.0f, 0.5f, 1.0f}});
        envelopeVertices.push_back({{fillRight, barY + barHeight}, {0.0f, 1.0f, 0.5f, 1.0f}});
        envelopeVertices.push_back({{-1.0f, barY + barHeight}, {0.0f, 0.8f, 0.2f, 1.0f}});
    }
}

void WaveformVisualizer::drawWaveform(const std::vector<Vertex>& vertices, GLuint vbo)
{
    if (vertices.empty() || vbo == 0)
        return;

    openGLContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, vbo);
    openGLContext.extensions.glBufferData(GL_ARRAY_BUFFER,
                                          static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                                          vertices.data(),
                                          GL_DYNAMIC_DRAW);

    GLint positionAttr = openGLContext.extensions.glGetAttribLocation(shader->getProgramID(), "position");
    GLint colorAttr = openGLContext.extensions.glGetAttribLocation(shader->getProgramID(), "color");

    openGLContext.extensions.glEnableVertexAttribArray(static_cast<GLuint>(positionAttr));
    openGLContext.extensions.glEnableVertexAttribArray(static_cast<GLuint>(colorAttr));

    openGLContext.extensions.glVertexAttribPointer(static_cast<GLuint>(positionAttr), 2, GL_FLOAT, GL_FALSE,
                                                   sizeof(Vertex), nullptr);
    openGLContext.extensions.glVertexAttribPointer(static_cast<GLuint>(colorAttr), 4, GL_FLOAT, GL_FALSE,
                                                   sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, color)));

    glDrawArrays(GL_LINE_STRIP, 0, static_cast<GLsizei>(vertices.size()));

    openGLContext.extensions.glDisableVertexAttribArray(static_cast<GLuint>(positionAttr));
    openGLContext.extensions.glDisableVertexAttribArray(static_cast<GLuint>(colorAttr));
    openGLContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
}
