#include "WaveformVisualizer.h"
#include "PluginProcessor.h"

WaveformVisualizer::WaveformVisualizer(SubbertoneAudioProcessor& p)
    : audioProcessor(p)
{
    // Don't make component opaque so paint() is called for text overlay
    setOpaque(false);
    
    // Enable mouse clicks
    setInterceptsMouseClicks(true, false);
    
    // Initialize history buffers
    inputHistory.resize(historySize);
    outputHistory.resize(historySize);
    harmonicResidualHistory.resize(historySize);
    
    for (int i = 0; i < historySize; ++i)
    {
        inputHistory[i].resize(512, 0.0f);
        outputHistory[i].resize(512, 0.0f);
        harmonicResidualHistory[i].resize(512, 0.0f);
    }
    
    openGLContext.setRenderer(this);
    openGLContext.attachTo(*this);
    openGLContext.setContinuousRepainting(true);
    
    startTimerHz(60); // 60 FPS update
}

WaveformVisualizer::~WaveformVisualizer()
{
    stopTimer();
    openGLContext.detach();
}

void WaveformVisualizer::paint(juce::Graphics& g)
{
    // Don't fill the background - let OpenGL handle everything
    // Only draw text overlay if OpenGL is not active
    if (!openGLContext.isAttached())
    {
        g.fillAll(bgColor);
        g.setColour(juce::Colours::white);
        g.drawText("OpenGL not initialized", getLocalBounds(), juce::Justification::centred);
        return;
    }
    
    // Draw text overlay on top of OpenGL
    g.setFont(juce::Font(juce::FontOptions("Courier New", 18.0f, juce::Font::bold)));
    
    // Draw INPUT with toggle state
    g.setColour(showInput ? inputColor : inputColor.withAlpha(0.3f));
    g.drawText("INPUT" + juce::String(showInput ? "" : " (OFF)"), 10, 10, 150, 25, juce::Justification::left);
    
    // Draw HARMONIC RESIDUAL with toggle state in the middle
    g.setColour(showHarmonicResidual ? harmonicResidualColor : harmonicResidualColor.withAlpha(0.3f));
    g.drawText("HARMONICS" + juce::String(showHarmonicResidual ? "" : " (OFF)"), 10, getHeight() / 2 - 12, 150, 25, juce::Justification::left);
    
    // Draw OUTPUT with toggle state
    g.setColour(showOutput ? outputColor : outputColor.withAlpha(0.3f));
    g.drawText("OUTPUT" + juce::String(showOutput ? "" : " (OFF)"), 10, getHeight() - 35, 150, 25, juce::Justification::left);
    
    // Add click instruction
    g.setColour(textColor.withAlpha(0.6f));
    g.setFont(juce::Font(juce::FontOptions("Courier New", 12.0f, juce::Font::plain)));
    g.drawText("Click top/middle/bottom to toggle waveforms", getWidth() - 280, 10, 270, 20, juce::Justification::right);
    
    // Draw fundamental frequency
    float fundamental = audioProcessor.getCurrentFundamental();
    if (fundamental > 0.0f)
    {
        g.setColour(glowColor);
        g.setFont(juce::Font(juce::FontOptions("Courier New", 20.0f, juce::Font::bold)));
        g.drawText("F0: " + juce::String(fundamental, 1) + " Hz",
                  getWidth() - 180, 10, 170, 25, juce::Justification::right);
        
        // Draw subharmonic frequency
        g.setColour(outputColor);
        g.drawText("SUB: " + juce::String(fundamental * 0.5f, 1) + " Hz",
                  getWidth() - 180, 40, 170, 25, juce::Justification::right);
    }
    else
    {
        g.setColour(juce::Colour(0xff808080)); // Brighter gray color
        g.setFont(juce::Font(juce::FontOptions("Courier New", 16.0f, juce::Font::plain)));
        g.drawText("NO SIGNAL", getWidth() - 180, 10, 170, 25, juce::Justification::right);
    }
}

void WaveformVisualizer::resized()
{
}

void WaveformVisualizer::timerCallback()
{
    // Update waveform history
    auto input = audioProcessor.getInputWaveform();
    auto output = audioProcessor.getOutputWaveform();
    auto harmonicResidual = audioProcessor.getHarmonicResidualWaveform();
    
    // Downsample to 512 points for performance
    const int targetSize = 512;
    inputHistory[historyWritePos].resize(targetSize);
    outputHistory[historyWritePos].resize(targetSize);
    harmonicResidualHistory[historyWritePos].resize(targetSize);
    
    if (!input.empty())
    {
        float ratio = input.size() / float(targetSize);
        for (int i = 0; i < targetSize; ++i)
        {
            int idx = static_cast<int>(i * ratio);
            inputHistory[historyWritePos][i] = input[idx] * 2.0f; // Scale up for visibility
        }
    }
    else
    {
        // Clear if no input
        for (int i = 0; i < targetSize; ++i)
        {
            inputHistory[historyWritePos][i] = 0.0f;
        }
    }
    
    if (!output.empty())
    {
        float ratio = output.size() / float(targetSize);
        for (int i = 0; i < targetSize; ++i)
        {
            int idx = static_cast<int>(i * ratio);
            outputHistory[historyWritePos][i] = output[idx] * 2.0f; // Scale up for visibility
        }
    }
    else
    {
        // Clear if no output
        for (int i = 0; i < targetSize; ++i)
        {
            outputHistory[historyWritePos][i] = 0.0f;
        }
    }
    
    if (!harmonicResidual.empty())
    {
        float ratio = harmonicResidual.size() / float(targetSize);
        for (int i = 0; i < targetSize; ++i)
        {
            int idx = static_cast<int>(i * ratio);
            harmonicResidualHistory[historyWritePos][i] = harmonicResidual[idx] * 2.0f; // Scale up for visibility
        }
    }
    else
    {
        // Clear if no harmonic residual
        for (int i = 0; i < targetSize; ++i)
        {
            harmonicResidualHistory[historyWritePos][i] = 0.0f;
        }
    }
    
    historyWritePos = (historyWritePos + 1) % historySize;
    
    // Update animation
    timeValue += 0.002f; // Slowed down by 10x
    gridDepth += 0.01f;
    if (gridDepth > 1.0f) gridDepth -= 1.0f;
    
    // Signal-based rotation
    float signalEnergy = 0.0f;
    if (!inputHistory.empty() && !inputHistory[historyWritePos].empty())
    {
        // Calculate RMS energy of current input
        for (float sample : inputHistory[historyWritePos])
        {
            signalEnergy += sample * sample;
        }
        signalEnergy = std::sqrt(signalEnergy / inputHistory[historyWritePos].size());
    }
    
    // Oscillate between -20 and +20 degrees based on time and signal
    // Convert degrees to radians: 20 degrees = 0.349 radians
    float oscillationY = std::sin(timeValue * 0.8f) * 0.349f; // Gentle oscillation
    float signalModulationY = signalEnergy * 0.2f * std::sin(timeValue * 2.0f); // Signal adds variation
    
    targetRotationY = oscillationY + signalModulationY;
    targetRotationY = juce::jlimit(-0.349f, 0.349f, targetRotationY); // Clamp to ±20 degrees
    
    // Add X-axis rotation too
    float oscillationX = std::sin(timeValue * 0.6f + 1.57f) * 0.349f; // Different phase for variety
    float signalModulationX = signalEnergy * 0.15f * std::sin(timeValue * 1.5f + 0.785f);
    
    float targetRotationX = 0.5f + oscillationX + signalModulationX; // Base angle + oscillation
    targetRotationX = juce::jlimit(0.15f, 0.85f, targetRotationX); // Keep some perspective
    
    // Smooth the rotation changes
    rotationY = rotationY * 0.9f + targetRotationY * 0.1f; // Smooth interpolation
    rotationX = rotationX * 0.9f + targetRotationX * 0.1f;
    
    // Also vary the camera distance slightly based on signal
    targetCameraZ = -8.0f - signalEnergy * 2.0f;
    cameraZ = cameraZ * 0.95f + targetCameraZ * 0.05f;
    
    // Trigger OpenGL repaint
    openGLContext.triggerRepaint();
    
    // Also trigger normal repaint for text overlay
    repaint();
}

void WaveformVisualizer::mouseDown(const juce::MouseEvent& event)
{
    // Get click position relative to component
    auto clickX = event.x;
    auto clickY = event.y;
    auto width = getWidth();
    auto height = getHeight();
    
    // Divide into three zones: top third (input), middle third (harmonic residual), bottom third (output)
    if (clickY < height / 3)
    {
        // Clicked on input area (top third)
        showInput = !showInput;
    }
    else if (clickY < 2 * height / 3)
    {
        // Clicked on harmonic residual area (middle third)
        showHarmonicResidual = !showHarmonicResidual;
    }
    else
    {
        // Clicked on output area (bottom third)
        showOutput = !showOutput;
    }
    
    repaint();
}

void WaveformVisualizer::newOpenGLContextCreated()
{
    createShaders();
    setupVertexArrays();
}

void WaveformVisualizer::openGLContextClosing()
{
    using namespace juce::gl;
    
    waveformShader.reset();
    gridShader.reset();
    glowShader.reset();
    
    if (vaoWaveform) glDeleteVertexArrays(1, &vaoWaveform);
    if (vboWaveform) glDeleteBuffers(1, &vboWaveform);
    if (vaoGrid) glDeleteVertexArrays(1, &vaoGrid);
    if (vboGrid) glDeleteBuffers(1, &vboGrid);
}

void WaveformVisualizer::createShaders()
{
    using namespace juce::gl;
    
    // Waveform shader with glow effect
    const char* waveformVertexShader = R"(
        #version 150 core
        in vec3 position;
        in float intensity;
        out float vIntensity;
        out float vDepth;
        uniform mat4 projectionMatrix;
        uniform mat4 viewMatrix;
        uniform float time;
        
        void main()
        {
            vec3 pos = position;
            // Add subtle wave motion
            pos.y += sin(position.x * 2.0 + time) * 0.02;
            
            vec4 viewPos = viewMatrix * vec4(pos, 1.0);
            gl_Position = projectionMatrix * viewPos;
            vIntensity = intensity;
            vDepth = -viewPos.z / 10.0;
        }
    )";
    
    const char* waveformFragmentShader = R"(
        #version 150 core
        in float vIntensity;
        in float vDepth;
        uniform vec4 color;
        uniform float glowIntensity;
        out vec4 fragColor;
        
        void main()
        {
            // Fade with depth and intensity
            float alpha = (1.0 - vDepth) * vIntensity;
            vec3 glowColor = color.rgb + vec3(glowIntensity * 0.5);
            
            // Add bloom effect
            float bloom = smoothstep(0.0, 1.0, vIntensity) * glowIntensity;
            glowColor += vec3(bloom);
            
            fragColor = vec4(glowColor, alpha * color.a);
        }
    )";
    
    // Grid shader
    const char* gridVertexShader = R"(
        #version 150 core
        in vec3 position;
        out vec3 vPosition;
        uniform mat4 projectionMatrix;
        uniform mat4 viewMatrix;
        uniform float depth;
        
        void main()
        {
            vec3 pos = position;
            pos.z -= depth * 10.0;
            
            gl_Position = projectionMatrix * viewMatrix * vec4(pos, 1.0);
            vPosition = pos;
        }
    )";
    
    const char* gridFragmentShader = R"(
        #version 150 core
        in vec3 vPosition;
        uniform vec4 color;
        out vec4 fragColor;
        
        void main()
        {
            float fade = 1.0 - smoothstep(0.0, 10.0, -vPosition.z);
            fragColor = vec4(color.rgb, color.a * fade * 0.5);
        }
    )";
    
    // Create shader programs
    waveformShader = std::make_unique<juce::OpenGLShaderProgram>(openGLContext);
    if (!waveformShader->addVertexShader(waveformVertexShader))
    {
        DBG("Waveform vertex shader failed: " << waveformShader->getLastError());
    }
    if (!waveformShader->addFragmentShader(waveformFragmentShader))
    {
        DBG("Waveform fragment shader failed: " << waveformShader->getLastError());
    }
    if (!waveformShader->link())
    {
        DBG("Waveform shader link failed: " << waveformShader->getLastError());
    }
    else
    {
        waveformShader->use();
    }
    
    gridShader = std::make_unique<juce::OpenGLShaderProgram>(openGLContext);
    if (!gridShader->addVertexShader(gridVertexShader))
    {
        DBG("Grid vertex shader failed: " << gridShader->getLastError());
    }
    if (!gridShader->addFragmentShader(gridFragmentShader))
    {
        DBG("Grid fragment shader failed: " << gridShader->getLastError());
    }
    if (!gridShader->link())
    {
        DBG("Grid shader link failed: " << gridShader->getLastError());
    }
    else
    {
        gridShader->use();
    }
}

void WaveformVisualizer::setupVertexArrays()
{
    using namespace juce::gl;
    
    glGenVertexArrays(1, &vaoWaveform);
    glGenBuffers(1, &vboWaveform);
    
    glGenVertexArrays(1, &vaoGrid);
    glGenBuffers(1, &vboGrid);
}

juce::Matrix3D<float> WaveformVisualizer::getProjectionMatrix() const
{
    float w = (float) getWidth();
    float h = (float) getHeight();
    
    return juce::Matrix3D<float>::fromFrustum(
        -w/h, w/h,    // left, right
        -1.0f, 1.0f,  // bottom, top
        1.0f, 100.0f  // near, far
    );
}

juce::Matrix3D<float> WaveformVisualizer::getViewMatrix() const
{
    auto matrix = juce::Matrix3D<float>();
    
    // Translation
    matrix = juce::Matrix3D<float>::fromTranslation({ 0, 0, cameraZ }) * matrix;
    
    // Rotation around X axis
    auto rotX = juce::Matrix3D<float>();
    float cosX = std::cos(rotationX);
    float sinX = std::sin(rotationX);
    rotX.mat[5] = cosX;
    rotX.mat[6] = -sinX;
    rotX.mat[9] = sinX;
    rotX.mat[10] = cosX;
    matrix = rotX * matrix;
    
    // Rotation around Y axis
    auto rotY = juce::Matrix3D<float>();
    float cosY = std::cos(rotationY);
    float sinY = std::sin(rotationY);
    rotY.mat[0] = cosY;
    rotY.mat[2] = sinY;
    rotY.mat[8] = -sinY;
    rotY.mat[10] = cosY;
    matrix = rotY * matrix;
    
    return matrix;
}

void WaveformVisualizer::renderOpenGL()
{
    using namespace juce::gl;
    
    const float desktopScale = static_cast<float>(openGLContext.getRenderingScale());
    glViewport(0, 0, 
               juce::roundToInt(desktopScale * getWidth()),
               juce::roundToInt(desktopScale * getHeight()));
    
    // Clear with deep blue-black
    glClearColor(bgColor.getFloatRed(), bgColor.getFloatGreen(), 
                 bgColor.getFloatBlue(), bgColor.getFloatAlpha());
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    // Check if shaders are ready
    if (!waveformShader || !gridShader)
    {
        // Draw red X to indicate shaders aren't ready
        glBegin(GL_LINES);
        glColor3f(1.0f, 0.0f, 0.0f);
        glVertex2f(-0.5f, -0.5f);
        glVertex2f(0.5f, 0.5f);
        glVertex2f(-0.5f, 0.5f);
        glVertex2f(0.5f, -0.5f);
        glEnd();
        
        DBG("Shaders not ready!");
        return;
    }
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    
    // Draw perspective grid
    drawPerspectiveGrid();
    
    // Draw waveforms with trails
    if (showInput)
        drawWaveform3D(inputHistory, inputColor, 0.5f, true);
    if (showHarmonicResidual)
        drawWaveform3D(harmonicResidualHistory, harmonicResidualColor, 0.0f, false);
    if (showOutput)
        drawWaveform3D(outputHistory, outputColor, -0.5f, false);
    
    // Draw UI text overlay
    glDisable(GL_DEPTH_TEST);
    
    // Draw text overlay
    drawTextOverlay();
}

void WaveformVisualizer::drawPerspectiveGrid()
{
    using namespace juce::gl;
    
    if (!gridShader) return;
    
    // Test with simple immediate mode first
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -5.0f);
    glRotatef(rotationX * 57.3f, 1.0f, 0.0f, 0.0f);
    glRotatef(rotationY * 57.3f, 0.0f, 1.0f, 0.0f);
    glRotatef(10.0f, 0.0f, 0.0f, 1.0f); // Slight tilt for style
    
    // Draw a simple 3D grid
    glColor4f(gridColor.getFloatRed(), gridColor.getFloatGreen(), 
              gridColor.getFloatBlue(), 0.5f);
    
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
    
    return; // Skip shader version for now
    
    gridShader->use();
    
    auto projMatrix = getProjectionMatrix();
    auto viewMatrix = getViewMatrix();
    
    gridShader->setUniformMat4("projectionMatrix", projMatrix.mat, 1, false);
    gridShader->setUniformMat4("viewMatrix", viewMatrix.mat, 1, false);
    gridShader->setUniform("depth", gridDepth);
    gridShader->setUniform("color", 
        gridColor.getFloatRed(), gridColor.getFloatGreen(), 
        gridColor.getFloatBlue(), 0.3f);
    
    // Generate grid vertices
    std::vector<float> gridVertices;
    const int gridLines = 20;
    const float gridSize = 10.0f;
    const float spacing = gridSize / gridLines;
    
    // Horizontal lines
    for (int i = 0; i <= gridLines; ++i)
    {
        float z = -gridSize/2 + i * spacing;
        // Left to right
        gridVertices.push_back(-gridSize/2);
        gridVertices.push_back(-1.0f);
        gridVertices.push_back(z);
        
        gridVertices.push_back(gridSize/2);
        gridVertices.push_back(-1.0f);
        gridVertices.push_back(z);
    }
    
    // Vertical lines
    for (int i = 0; i <= gridLines; ++i)
    {
        float x = -gridSize/2 + i * spacing;
        // Front to back
        gridVertices.push_back(x);
        gridVertices.push_back(-1.0f);
        gridVertices.push_back(-gridSize/2);
        
        gridVertices.push_back(x);
        gridVertices.push_back(-1.0f);
        gridVertices.push_back(gridSize/2);
    }
    
    glBindVertexArray(vaoGrid);
    glBindBuffer(GL_ARRAY_BUFFER, vboGrid);
    glBufferData(GL_ARRAY_BUFFER, gridVertices.size() * sizeof(float),
                 gridVertices.data(), GL_DYNAMIC_DRAW);
    
    auto positionAttribute = glGetAttribLocation(gridShader->getProgramID(), "position");
    glEnableVertexAttribArray(positionAttribute);
    glVertexAttribPointer(positionAttribute, 3, GL_FLOAT, GL_FALSE, 
                         3 * sizeof(float), nullptr);
    
    glLineWidth(1.0f);
    glDrawArrays(GL_LINES, 0, gridVertices.size() / 3);
    
    glDisableVertexAttribArray(positionAttribute);
    glBindVertexArray(0);
}

void WaveformVisualizer::drawWaveform3D(const std::vector<std::vector<float>>& history,
                                        const juce::Colour& color, float yOffset, bool isInput)
{
    using namespace juce::gl;
    
    if (history.empty()) return;
    
    // Use immediate mode for 3D drawing
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, cameraZ);
    glRotatef(rotationX * 57.3f, 1.0f, 0.0f, 0.0f);
    glRotatef(rotationY * 57.3f, 0.0f, 1.0f, 0.0f);
    glRotatef(10.0f, 0.0f, 0.0f, 1.0f); // Slight tilt for style
    
    // Draw waveform trails
    for (int h = 0; h < historySize; ++h)
    {
        int histIdx = (historyWritePos - h - 1 + historySize) % historySize;
        const auto& waveform = history[histIdx];
        
        if (waveform.empty() || waveform.size() < 2) continue;
        
        float intensity = 1.0f - (float)h / historySize;
        float zPos = -h * 0.3f;
        
        // Set color with fade
        glColor4f(color.getFloatRed(), color.getFloatGreen(), 
                  color.getFloatBlue(), intensity * 0.8f);
        
        glLineWidth(3.0f - h * 0.1f);
        
        glBegin(GL_LINE_STRIP);
        for (size_t i = 0; i < waveform.size(); ++i)
        {
            float x = (i / float(waveform.size() - 1)) * 16.0f - 8.0f; // Full width
            float y = waveform[i] * 2.0f + yOffset;
            glVertex3f(x, y, zPos);
        }
        glEnd();
    }
    
    return; // Skip shader version for now
    
    if (!waveformShader) return;
    
    waveformShader->use();
    
    auto projMatrix = getProjectionMatrix();
    auto viewMatrix = getViewMatrix();
    
    waveformShader->setUniformMat4("projectionMatrix", projMatrix.mat, 1, false);
    waveformShader->setUniformMat4("viewMatrix", viewMatrix.mat, 1, false);
    waveformShader->setUniform("time", timeValue);
    
    // Calculate current fundamental for glow effect
    float fundamental = audioProcessor.getCurrentFundamental();
    float glow = fundamental > 0 ? 0.5f + 0.5f * std::sin(timeValue * 10.0f) : 0.0f;
    waveformShader->setUniform("glowIntensity", glow);
    
    glBindVertexArray(vaoWaveform);
    glBindBuffer(GL_ARRAY_BUFFER, vboWaveform);
    
    // Draw each history slice with decreasing intensity
    for (int h = 0; h < historySize; ++h)
    {
        int histIdx = (historyWritePos - h - 1 + historySize) % historySize;
        const auto& waveform = history[histIdx];
        
        if (waveform.empty()) continue;
        
        float intensity = 1.0f - (float)h / historySize;
        float zPos = -h * 0.2f;
        
        // Create vertex data with position and intensity
        std::vector<float> vertices;
        vertices.reserve(waveform.size() * 4); // x, y, z, intensity
        
        for (size_t i = 0; i < waveform.size(); ++i)
        {
            float x = (i / float(waveform.size() - 1)) * 8.0f - 4.0f;
            float y = waveform[i] * 2.0f + yOffset;
            
            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(zPos);
            vertices.push_back(intensity);
        }
        
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                     vertices.data(), GL_DYNAMIC_DRAW);
        
        auto positionAttribute = glGetAttribLocation(waveformShader->getProgramID(), "position");
        auto intensityAttribute = glGetAttribLocation(waveformShader->getProgramID(), "intensity");
        
        glEnableVertexAttribArray(positionAttribute);
        glVertexAttribPointer(positionAttribute, 3, GL_FLOAT, GL_FALSE, 
                             4 * sizeof(float), nullptr);
        
        glEnableVertexAttribArray(intensityAttribute);
        glVertexAttribPointer(intensityAttribute, 1, GL_FLOAT, GL_FALSE, 
                             4 * sizeof(float), (void*)(3 * sizeof(float)));
        
        // Set color with alpha fade
        float alpha = intensity * 0.8f;
        waveformShader->setUniform("color", 
            color.getFloatRed(), color.getFloatGreen(), 
            color.getFloatBlue(), alpha);
        
        glLineWidth(2.0f - h * 0.05f); // Thinner lines as they recede
        glDrawArrays(GL_LINE_STRIP, 0, waveform.size());
        
        glDisableVertexAttribArray(positionAttribute);
        glDisableVertexAttribArray(intensityAttribute);
    }
    
    glBindVertexArray(0);
}

void WaveformVisualizer::drawTextOverlay()
{
    // Note: This is a placeholder - proper text rendering in OpenGL requires
    // either bitmap fonts or using JUCE's OpenGL text rendering capabilities
    // For now, we'll handle text in the paint() method after OpenGL rendering
}