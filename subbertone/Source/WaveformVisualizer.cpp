#include "WaveformVisualizer.h"
#include "PluginProcessor.h"

WaveformVisualizer::WaveformVisualizer(SubbertoneAudioProcessor& audioProcessor)
    : m_audioProcessor(audioProcessor)
{
    // Don't make component opaque so paint() is called for text overlay
    setOpaque(false);
    
    // Enable mouse clicks
    setInterceptsMouseClicks(true, false);
    
    // Initialize history buffers
    m_inputHistory.resize(c_historySize);
    m_outputHistory.resize(c_historySize);
    m_harmonicResidualHistory.resize(c_historySize);
    
    for (int i = 0; i < c_historySize; ++i)
    {
        m_inputHistory[i].resize(512, 0.0f);
        m_outputHistory[i].resize(512, 0.0f);
        m_harmonicResidualHistory[i].resize(512, 0.0f);
    }

    m_inputBuffer.resize(2048, 0.0f);
    m_outputBuffer.resize(2048, 0.0f);
    m_harmonicResidualBuffer.resize(2048, 0.0f);
    
    m_openGLContext.setRenderer(this);
    m_openGLContext.attachTo(*this);
    m_openGLContext.setContinuousRepainting(true);
    
    startTimerHz(60); // 60 FPS update
}

WaveformVisualizer::~WaveformVisualizer()
{
    stopTimer();
    m_openGLContext.detach();
}

void WaveformVisualizer::paint(juce::Graphics& g)
{
    // Don't fill the background - let OpenGL handle everything
    // Only draw text overlay if OpenGL is not active
    if (!m_openGLContext.isAttached())
    {
        g.fillAll(m_bgColor);
        g.setColour(juce::Colours::white);
        g.drawText("OpenGL not initialized", getLocalBounds(), juce::Justification::centred);
        return;
    }
    
    // Draw text overlay on top of OpenGL
    g.setFont(juce::Font(juce::FontOptions("Courier New", 18.0f, juce::Font::bold)));
    
    // Draw INPUT with toggle state
    g.setColour(m_showInput ? m_inputColor : m_inputColor.withAlpha(0.3f));
    g.drawText("INPUT" + juce::String(m_showInput ? "" : " (OFF)"), 10, 10, 150, 25, juce::Justification::left);
    
    // Draw HARMONIC RESIDUAL with toggle state in the middle
    g.setColour(m_showHarmonicResidual ? m_harmonicResidualColor : m_harmonicResidualColor.withAlpha(0.3f));
    g.drawText("HARMONICS" + juce::String(m_showHarmonicResidual ? "" : " (OFF)"), 10, getHeight() / 2 - 12, 150, 25, juce::Justification::left);
    
    // Draw OUTPUT with toggle state
    g.setColour(m_showOutput ? m_outputColor : m_outputColor.withAlpha(0.3f));
    g.drawText("OUTPUT" + juce::String(m_showOutput ? "" : " (OFF)"), 10, getHeight() - 35, 150, 25, juce::Justification::left);
    
    // Add click instruction
    g.setColour(m_textColor.withAlpha(0.6f));
    g.setFont(juce::Font(juce::FontOptions("Courier New", 12.0f, juce::Font::plain)));
    g.drawText("Click top/middle/bottom to toggle waveforms", getWidth() - 280, 10, 275, 20, juce::Justification::right);
    
    // Draw fundamental frequency
    const float fundamental = m_audioProcessor.getCurrentFundamental();
    const bool hasSignal = (fundamental > 0.0f);
    if (hasSignal)
    {
        const int bottomTextY = getHeight() - 35;
        g.setColour(m_glowColor);
        g.setFont(juce::Font(juce::FontOptions("Courier New", 16.0f, juce::Font::bold)));
        g.drawText("F0: " + juce::String(fundamental, 1) + " Hz", getWidth() - 180, bottomTextY - 25, 170, 25, juce::Justification::right);
        
        // Draw subharmonic frequency
        g.setColour(m_outputColor);
        g.setFont(juce::Font(juce::FontOptions("Courier New", 16.0f, juce::Font::bold)));
        g.drawText("SUB: " + juce::String(fundamental * 0.5f, 1) + " Hz", getWidth() - 180, bottomTextY, 170, 25, juce::Justification::right);
    }
    else
    {
        g.setColour(juce::Colour(0xff808080)); // Brighter gray color
        g.setFont(juce::Font(juce::FontOptions("Courier New", 16.0f, juce::Font::plain)));
        g.drawText("NO SIGNAL", getWidth() - 180, getHeight() - 35, 170, 25, juce::Justification::right);
    }

    // Draw signal level text near bottom-right (avoid overlap with NO SIGNAL)
    if (hasSignal)
    {
        g.setColour(m_signalAboveThreshold ? m_signalOkColor : m_signalLowColor);
        g.setFont(juce::Font(juce::FontOptions("Courier New", 14.0f, juce::Font::bold)));
        g.drawText(m_signalText, getWidth() / 2 - 85, getHeight() - 35, 170, 25, juce::Justification::centred);
    }
}

void WaveformVisualizer::timerCallback()
{
    // Update waveform history
    m_audioProcessor.getInputWaveform(m_inputBuffer);
    m_audioProcessor.getOutputWaveform(m_outputBuffer);
    m_audioProcessor.getHarmonicResidualWaveform(m_harmonicResidualBuffer);
    
    // Downsample to 512 points for performance
    const int targetSize = 512;
    m_inputHistory[m_historyWritePos].resize(targetSize);
    m_outputHistory[m_historyWritePos].resize(targetSize);
    m_harmonicResidualHistory[m_historyWritePos].resize(targetSize);
    
    if (!m_inputBuffer.empty())
    {
        const float ratio = m_inputBuffer.size() / float(targetSize);
        for (int i = 0; i < targetSize; ++i)
        {
            const int idx = static_cast<int>(i * ratio);
            m_inputHistory[m_historyWritePos][i] = m_inputBuffer[idx] * 2.0f; // Scale up for visibility
        }
    }
    else
    {
        // Clear if no input
        for (int i = 0; i < targetSize; ++i)
        {
            m_inputHistory[m_historyWritePos][i] = 0.0f;
        }
    }
    
    if (!m_outputBuffer.empty())
    {
        const float ratio = m_outputBuffer.size() / float(targetSize);
        for (int i = 0; i < targetSize; ++i)
        {
            const int idx = static_cast<int>(i * ratio);
            m_outputHistory[m_historyWritePos][i] = m_outputBuffer[idx] * 2.0f; // Scale up for visibility
        }
    }
    else
    {
        // Clear if no output
        for (int i = 0; i < targetSize; ++i)
        {
            m_outputHistory[m_historyWritePos][i] = 0.0f;
        }
    }
    
    if (!m_harmonicResidualBuffer.empty())
    {
        const float ratio = m_harmonicResidualBuffer.size() / float(targetSize);

        for (int i = 0; i < targetSize; ++i)
        {
            const int idx = static_cast<int>(i * ratio);
            m_harmonicResidualHistory[m_historyWritePos][i] = m_harmonicResidualBuffer[idx] * 2.0f; // Scale up for visibility
        }
    }
    else
    {
        // Clear if no harmonic residual
        for (int i = 0; i < targetSize; ++i)
        {
            m_harmonicResidualHistory[m_historyWritePos][i] = 0.0f;
        }
    }
    
    // Update animation
    m_timeValue += 0.002f; // Slowed down by 10x
    
    // Signal-based rotation
    float signalEnergy = 0.0f;

    if (!m_inputHistory.empty() && !m_inputHistory[m_historyWritePos].empty())
    {
        // Calculate RMS energy of current input
        for (float sample : m_inputHistory[m_historyWritePos])
        {
            signalEnergy += sample * sample;
        }
        signalEnergy = std::sqrt(signalEnergy / m_inputHistory[m_historyWritePos].size());
    }

    m_historyWritePos = (m_historyWritePos + 1) % c_historySize;
    
    // Oscillate between -20 and +20 degrees based on time and signal
    // Convert degrees to radians: 20 degrees = 0.349 radians
    const float oscillationY = std::sin(m_timeValue * 0.8f) * 0.349f; // Gentle oscillation
    const float signalModulationY = signalEnergy * 0.2f * std::sin(m_timeValue * 2.0f); // Signal adds variation
    
    m_targetRotationY = oscillationY + signalModulationY;
    m_targetRotationY = std::clamp(m_targetRotationY, -0.349f, 0.349f); // Clamp to +/-20 degrees
    
    // Add X-axis rotation too
    const float oscillationX = std::sin(m_timeValue * 0.6f + 1.57f) * 0.349f; // Different phase for variety
    const float signalModulationX = signalEnergy * 0.15f * std::sin(m_timeValue * 1.5f + 0.785f);
    
    float targetRotationX = 0.5f + oscillationX + signalModulationX; // Base angle + oscillation
    targetRotationX = std::clamp(targetRotationX, 0.15f, 0.85f); // Keep some perspective
    
    // Smooth the rotation changes
    m_rotationY = m_rotationY * 0.9f + m_targetRotationY * 0.1f; // Smooth interpolation
    m_rotationX = m_rotationX * 0.9f + targetRotationX * 0.1f;
    
    // Also vary the camera distance slightly based on signal
    m_targetCameraZ = -8.0f - signalEnergy * 2.0f;
    m_cameraZ = m_cameraZ * 0.95f + m_targetCameraZ * 0.05f;
    
    // Trigger OpenGL repaint
    m_openGLContext.triggerRepaint();
    
    // Also trigger normal repaint for text overlay
    repaint();
}

void WaveformVisualizer::mouseDown(const juce::MouseEvent& event)
{
    // Get click position relative to component
    const int clickY = event.y;
    const int height = getHeight();
    
    // Divide into three zones: top third (input), middle third (harmonic residual), bottom third (output)
    if (clickY < height / 3)
    {
        // Clicked on input area (top third)
        m_showInput = !m_showInput;
    }
    else if (clickY < 2 * height / 3)
    {
        // Clicked on harmonic residual area (middle third)
        m_showHarmonicResidual = !m_showHarmonicResidual;
    }
    else
    {
        // Clicked on output area (bottom third)
        m_showOutput = !m_showOutput;
    }
    
    repaint();
}

void WaveformVisualizer::setSignalText(const juce::String& text, bool aboveThreshold)
{
    m_signalText = text;
    m_signalAboveThreshold = aboveThreshold;
    repaint();
}

void WaveformVisualizer::newOpenGLContextCreated()
{
}

void WaveformVisualizer::renderOpenGL()
{
    using namespace juce::gl;
    
    const float desktopScale = static_cast<float>(m_openGLContext.getRenderingScale());
    glViewport(0, 0, 
               juce::roundToInt(desktopScale * getWidth()),
               juce::roundToInt(desktopScale * getHeight()));
    
    // Clear with deep blue-black
    glClearColor(m_bgColor.getFloatRed(), m_bgColor.getFloatGreen(), 
                 m_bgColor.getFloatBlue(), m_bgColor.getFloatAlpha());
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    
    // Draw perspective grid
    drawPerspectiveGrid();
    
    // Draw waveforms with trails
    if (m_showInput)
        drawWaveform3D(m_inputHistory, m_inputColor, 0.5f, true);

    if (m_showHarmonicResidual)
        drawWaveform3D(m_harmonicResidualHistory, m_harmonicResidualColor, 0.0f, false);

    if (m_showOutput)
        drawWaveform3D(m_outputHistory, m_outputColor, -0.5f, false);
    
    glDisable(GL_DEPTH_TEST);
}

void WaveformVisualizer::openGLContextClosing()
{
}

void WaveformVisualizer::drawPerspectiveGrid()
{
    using namespace juce::gl;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -5.0f);
    glRotatef(m_rotationX * 57.3f, 1.0f, 0.0f, 0.0f);
    glRotatef(m_rotationY * 57.3f, 0.0f, 1.0f, 0.0f);
    glRotatef(10.0f, 0.0f, 0.0f, 1.0f); // Slight tilt for style
    
    // Draw a simple 3D grid
    glColor4f(m_gridColor.getFloatRed(), m_gridColor.getFloatGreen(), m_gridColor.getFloatBlue(), 0.5f);
    
    glBegin(GL_LINES);
    // Grid lines - wider to match full width
    for (int i = -20; i <= 20; ++i)
    {
        // X direction lines
        glVertex3f(i * 0.8f, -1.0f, -10.0f);
        glVertex3f(i * 0.8f, -1.0f, 0.0f);
        
        // Z direction lines
        glVertex3f(-16.0f, -1.0f, i * 0.5f);
        glVertex3f(16.0f, -1.0f, i * 0.5f);
    }
    glEnd();
}

void WaveformVisualizer::drawWaveform3D(const std::vector<std::vector<float>>& history, const juce::Colour& color, float yOffset, bool isInput)
{
    using namespace juce::gl;
    
    juce::ignoreUnused(isInput);

    if (history.empty())
        return;
    
    // Use immediate mode for 3D drawing
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, m_cameraZ);
    glRotatef(m_rotationX * 57.3f, 1.0f, 0.0f, 0.0f);
    glRotatef(m_rotationY * 57.3f, 0.0f, 1.0f, 0.0f);
    glRotatef(10.0f, 0.0f, 0.0f, 1.0f); // Slight tilt for style
    
    // Draw waveform trails
    for (int h = 0; h < c_historySize; ++h)
    {
        const int histIdx = (m_historyWritePos - h - 1 + c_historySize) % c_historySize;

        const std::vector<float>& waveform = history[histIdx];
        
        if (waveform.empty() || waveform.size() < 2)
            continue;
        
        const float intensity = 1.0f - static_cast<float>(h) / c_historySize;
        const float zPos = -static_cast<float>(h) * 0.3f;
        
        // Set color with fade
        glColor4f(color.getFloatRed(), color.getFloatGreen(), color.getFloatBlue(), intensity * 0.8f);
        
        const float lineWidth = std::max(0.001f, 3.0f - static_cast<float>(h) * 0.1f);
        glLineWidth(lineWidth);
        
        glBegin(GL_LINE_STRIP);
        for (size_t i = 0; i < waveform.size(); ++i)
        {
            const float x = (i / float(waveform.size() - 1)) * 16.0f - 8.0f; // Full width
            const float y = waveform[i] * 2.0f + yOffset;
            glVertex3f(x, y, zPos);
        }
    glEnd();
    }
}
