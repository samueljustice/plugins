#include "WaveformVisualizer.h"
#include "PluginProcessor.h"

WaveformVisualizer::WaveformVisualizer(SubbertoneAudioProcessor& audioProcessor)
    : m_audioProcessor(audioProcessor)
{
    m_logo = std::make_unique<LogoOutline>(BinaryData::sjlogo_svg, static_cast<size_t>(BinaryData::sjlogo_svgSize));

    // Restore the trace toggles saved with the session.
    m_showInput = m_audioProcessor.getViewOption("viewShowInput", true);
    m_showHarmonicResidual = m_audioProcessor.getViewOption("viewShowHarmonics", true);
    m_showOutput = m_audioProcessor.getViewOption("viewShowOutput", true);

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

void WaveformVisualizer::drawLegendRow(juce::Graphics& g, int centreY, const juce::String& name, juce::Colour colour, bool on)
{
    const auto chip = juce::Rectangle<float>(14.0f, static_cast<float>(centreY) - 9.0f, 112.0f, 18.0f);
    const float alpha = on ? 1.0f : 0.35f;

    g.setColour(Theme::Colours::scope.withAlpha(0.65f));
    g.fillRoundedRectangle(chip, 9.0f);

    g.setColour(colour.withAlpha(alpha));
    g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre({ chip.getX() + 11.0f, chip.getCentreY() }));

    g.setFont(m_fonts->label(10.5f));
    g.setColour(Theme::Colours::text.withAlpha(on ? 0.9f : 0.35f));
    g.drawText(name, chip.withTrimmedLeft(22.0f), juce::Justification::centredLeft, false);
}

void WaveformVisualizer::paint(juce::Graphics& g)
{
    using namespace Theme::Colours;

    const auto bounds = getLocalBounds();

    if (!m_openGLContext.isAttached())
    {
        g.fillAll(m_bgColor);
        g.setColour(textDim);
        g.setFont(m_fonts->body(12.0f));
        g.drawText("OpenGL not available", bounds, juce::Justification::centred);
    }

    // Legend rows double as the click-to-toggle zones (top / middle / bottom thirds).
    const int third = getHeight() / 3;
    drawLegendRow(g, third / 2,             "INPUT",     m_inputColor,            m_showInput);
    drawLegendRow(g, third + third / 2,     "HARMONICS", m_harmonicResidualColor, m_showHarmonicResidual);
    drawLegendRow(g, 2 * third + third / 2, "SUB OUT",   m_outputColor,           m_showOutput);

    // Readouts, top right
    const float fundamental = m_audioProcessor.getCurrentFundamental();
    auto readout = bounds.reduced(14, 12).removeFromTop(18);

    g.setFont(m_fonts->body(12.5f));

    if (fundamental > 0.0f)
    {
        const auto subText = "SUB " + juce::String(fundamental * 0.5f, 1) + " Hz";
        const auto f0Text  = "IN " + juce::String(fundamental, 1) + " Hz";

        g.setColour(m_outputColor);
        g.drawText(subText, readout.removeFromRight(120), juce::Justification::centredRight, false);
        g.setColour(m_inputColor);
        g.drawText(f0Text, readout.removeFromRight(118), juce::Justification::centredRight, false);
    }
    else
    {
        g.setColour(textFaint);
        g.drawText("NO PITCH", readout.removeFromRight(120), juce::Justification::centredRight, false);
    }

    // Level, bottom right
    g.setFont(m_fonts->body(11.5f));
    g.setColour(m_signalAboveThreshold ? good : textFaint);
    g.drawText(m_signalText, bounds.reduced(14, 10).removeFromBottom(16), juce::Justification::centredRight, false);

    g.setColour(textFaint.withAlpha(0.7f));
    g.setFont(m_fonts->label(9.5f));
    g.drawText("CLICK A ROW TO TOGGLE", bounds.reduced(14, 10).removeFromBottom(16), juce::Justification::centred, false);

    // Frame
    g.setColour(outlineSoft);
    g.drawRect(bounds, 1);
}

void WaveformVisualizer::updateIdleState()
{
    // "Quiet" = nothing tracked and the input below about -50 dBFS. After 1.5 s of
    // quiet, fade the logo in slowly; fade it out quickly the moment sound returns.
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const bool quiet = m_audioProcessor.getCurrentFundamental() <= 0.0f
                    && m_audioProcessor.getCurrentSignalLevel() < 0.003f;

    if (!quiet)
        m_quietSinceSeconds = -1.0;
    else if (m_quietSinceSeconds < 0.0)
        m_quietSinceSeconds = now;

    const bool idle = quiet && (now - m_quietSinceSeconds) > 1.5;
    float opacity = m_logoOpacity.load();

    if (idle)
    {
        if (opacity <= 0.0f)
            m_logoStartSeconds = now; // restart the sketch each time we go idle
        opacity = std::min(1.0f, opacity + 1.0f / (60.0f * 0.9f));
    }
    else
    {
        opacity = std::max(0.0f, opacity - 1.0f / (60.0f * 0.15f));
    }

    m_logoOpacity = opacity;
}

void WaveformVisualizer::timerCallback()
{
    updateIdleState();

    // Follow the saved view options, so a host state recall updates an open editor.
    m_showInput = m_audioProcessor.getViewOption("viewShowInput", true);
    m_showHarmonicResidual = m_audioProcessor.getViewOption("viewShowHarmonics", true);
    m_showOutput = m_audioProcessor.getViewOption("viewShowOutput", true);

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
    
    // Three zones: top third input, middle harmonics, bottom sub output.
    if (clickY < height / 3)
    {
        m_showInput = !m_showInput;
        m_audioProcessor.setViewOption("viewShowInput", m_showInput);
    }
    else if (clickY < 2 * height / 3)
    {
        m_showHarmonicResidual = !m_showHarmonicResidual;
        m_audioProcessor.setViewOption("viewShowHarmonics", m_showHarmonicResidual);
    }
    else
    {
        m_showOutput = !m_showOutput;
        m_audioProcessor.setViewOption("viewShowOutput", m_showOutput);
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
    
    // Additive blending gives the trails a soft glow where they overlap.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    
    const float logoOpacity = m_logoOpacity.load();
    m_traceAlpha = 1.0f - logoOpacity;

    // Draw perspective grid
    drawPerspectiveGrid();
    
    // Draw waveforms with trails
    if (m_showInput)
        drawWaveform3D(m_inputHistory, m_inputColor, 0.5f, true);

    if (m_showHarmonicResidual)
        drawWaveform3D(m_harmonicResidualHistory, m_harmonicResidualColor, 0.0f, false);

    if (m_showOutput)
        drawWaveform3D(m_outputHistory, m_outputColor, -0.5f, false);

    if (m_logo != nullptr && m_logo->isLoaded() && logoOpacity > 0.001f)
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        m_logo->render(static_cast<float>(now - m_logoStartSeconds.load()), logoOpacity,
                       static_cast<float>(getWidth()) / static_cast<float>(std::max(1, getHeight())));
    }
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
    
    // Floor grid that fades into the distance.
    const float r = m_gridColor.getFloatRed(), gr = m_gridColor.getFloatGreen(), b = m_gridColor.getFloatBlue();

    glLineWidth(1.0f);
    glBegin(GL_LINES);
    for (int i = -20; i <= 20; ++i)
    {
        // Lines running into the screen: bright near, faded far.
        glColor4f(r, gr, b, 0.9f);
        glVertex3f(i * 0.8f, -1.0f, 0.0f);
        glColor4f(r, gr, b, 0.0f);
        glVertex3f(i * 0.8f, -1.0f, -10.0f);

        // Cross lines: alpha by depth.
        const float depth = (i + 20) / 40.0f; // 0 far .. 1 near
        glColor4f(r, gr, b, 0.9f * depth);
        glVertex3f(-16.0f, -1.0f, i * 0.25f - 5.0f);
        glVertex3f(16.0f, -1.0f, i * 0.25f - 5.0f);
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
        
        const float age = static_cast<float>(h) / c_historySize;
        const float intensity = (1.0f - age) * (1.0f - age);
        const float zPos = -static_cast<float>(h) * 0.3f;

        const auto strip = [&]
        {
            glBegin(GL_LINE_STRIP);
            for (size_t i = 0; i < waveform.size(); ++i)
            {
                const float x = (i / float(waveform.size() - 1)) * 16.0f - 8.0f;
                const float y = waveform[i] * 2.0f + yOffset;
                glVertex3f(x, y, zPos);
            }
            glEnd();
        };

        if (h == 0)
        {
            // Wide, faint halo under the newest trace.
            glColor4f(color.getFloatRed(), color.getFloatGreen(), color.getFloatBlue(), 0.12f * m_traceAlpha);
            glLineWidth(9.0f);
            strip();
            glColor4f(color.getFloatRed(), color.getFloatGreen(), color.getFloatBlue(), 0.25f * m_traceAlpha);
            glLineWidth(4.5f);
            strip();
        }

        glColor4f(color.getFloatRed(), color.getFloatGreen(), color.getFloatBlue(), (h == 0 ? 1.0f : 0.45f) * intensity * m_traceAlpha);
        glLineWidth(h == 0 ? 2.0f : std::max(1.0f, 1.8f - h * 0.05f));
        strip();
    }
}
