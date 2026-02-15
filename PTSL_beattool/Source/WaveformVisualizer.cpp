#define GL_SILENCE_DEPRECATION
#include "WaveformVisualizer.h"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <OpenGL/OpenGL.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Vertex shader - compatible with older GLSL
const char* vertexShaderSource = R"(
#version 120

attribute vec3 aPos;
attribute float aIntensity;

varying float Intensity;

uniform mat4 mvpMatrix;

void main() {
    gl_Position = mvpMatrix * vec4(aPos, 1.0);
    Intensity = aIntensity;
}
)";

// Fragment shader - compatible with older GLSL
const char* fragmentShaderSource = R"(
#version 120

varying float Intensity;

void main() {
    // Dreamy gradient based on intensity
    vec3 color = vec3(
        0.3 + Intensity * 0.7,        // Red channel
        0.2 + Intensity * 0.3,        // Green channel  
        0.8 + Intensity * 0.2         // Blue channel
    );
    
    // Use intensity for alpha to create fade effect
    float alpha = Intensity * 0.8;
    gl_FragColor = vec4(color, alpha);
}
)";

WaveformVisualizer::WaveformVisualizer() 
    : viewportWidth(800)
    , viewportHeight(600)
    , waveformVBO(0)
    , trailVBO(0)
    , shaderProgram(0)
    , bloomFBO(0)
    , bloomTexture(0)
    , sampleRate(44100)
    , currentPosition(0)
    , isPlaying(true)
    , lastBeatTime(0)
    , currentTempo(120)
    , beatPulse(0)
    , fogColor(0.05f, 0.0f, 0.1f)  // Dark purple fog
    , fogDensity(0.1f)
    , waveformScale(5.0f)
    , cameraDistance(10.0f)
    , cameraAngle(0.0f)
    , modelMatrix(1.0f)  // Initialize to identity matrix
    , viewMatrix(1.0f)
    , projectionMatrix(1.0f)
    , positionAttribLoc(-1)
    , intensityAttribLoc(-1)
    , currentMode(MODE_AMBIENT)
    , targetMode(MODE_AMBIENT)
    , transitionProgress(1.0f)
    , ambientTime(0.0f)
    , analysisProgress(0.0f) {
    
    blurFBO[0] = blurFBO[1] = 0;
    blurTexture[0] = blurTexture[1] = 0;
}

WaveformVisualizer::~WaveformVisualizer() {
    shutdown();
}

bool WaveformVisualizer::initialize(int width, int height) {
    viewportWidth = width;
    viewportHeight = height;
    
    // Disable verbose logging
    /*
    std::cout << "WaveformVisualizer::initialize() - viewport: " << width << "x" << height << std::endl;
    
    // Log OpenGL version
    const GLubyte* version = glGetString(GL_VERSION);
    const GLubyte* glslVersion = glGetString(GL_SHADING_LANGUAGE_VERSION);
    std::cout << "OpenGL version: " << version << std::endl;
    std::cout << "GLSL version: " << glslVersion << std::endl;
    */
    
    // Compile shaders
    shaderProgram = createShaderProgram(vertexShaderSource, fragmentShaderSource);
    if (shaderProgram == 0) {
        std::cerr << "Failed to create shader program" << std::endl;
        return false;
    }
    
    // Shader program created
    
    // Get uniform locations
    mvpMatrixLoc = glGetUniformLocation(shaderProgram, "mvpMatrix");
    timeUniformLoc = glGetUniformLocation(shaderProgram, "time");
    beatPulseLoc = glGetUniformLocation(shaderProgram, "beatPulse");
    fogColorLoc = glGetUniformLocation(shaderProgram, "fogColor");
    fogDensityLoc = glGetUniformLocation(shaderProgram, "fogDensity");
    
    // Get attribute locations and store them
    positionAttribLoc = glGetAttribLocation(shaderProgram, "aPos");
    intensityAttribLoc = glGetAttribLocation(shaderProgram, "aIntensity");
    
    // Create VBO for waveform (no VAO in compatibility profile)
    glGenBuffers(1, &waveformVBO);
    
    // Create VBO for trails
    glGenBuffers(1, &trailVBO);
    
    // Create framebuffers for bloom effect
    glGenFramebuffers(1, &bloomFBO);
    glGenTextures(1, &bloomTexture);
    
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO);
    glBindTexture(GL_TEXTURE_2D, bloomTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloomTexture, 0);
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // Setup projection matrix
    projectionMatrix = glm::perspective(glm::radians(45.0f), 
                                       (float)width / (float)height, 
                                       0.1f, 100.0f);
    
    // Enable depth testing with depth writing disabled for transparency
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);  // Don't write to depth buffer for transparent objects
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Set line width for waveform
    glLineWidth(3.0f);
    
    return true;
}

void WaveformVisualizer::shutdown() {
    if (waveformVBO) glDeleteBuffers(1, &waveformVBO);
    if (trailVBO) glDeleteBuffers(1, &trailVBO);
    if (shaderProgram) glDeleteProgram(shaderProgram);
    if (bloomFBO) glDeleteFramebuffers(1, &bloomFBO);
    if (bloomTexture) glDeleteTextures(1, &bloomTexture);
    if (blurFBO[0]) glDeleteFramebuffers(2, blurFBO);
    if (blurTexture[0]) glDeleteTextures(2, blurTexture);
}

void WaveformVisualizer::updateAudioData(const std::vector<float>& samples, double sr) {
    std::lock_guard<std::mutex> lock(audioMutex);
    audioSamples = samples;
    sampleRate = sr;
    
    // Reset position to start when new audio is loaded
    currentPosition = 0.0f;
}

void WaveformVisualizer::updateBeatData(double beatTime, double tempo) {
    std::lock_guard<std::mutex> lock(beatMutex);
    beatPositions.push_back(beatTime);
    lastBeatTime = beatTime;
    currentTempo = tempo;
    beatPulse = 1.0f; // Reset beat pulse
}

void WaveformVisualizer::clearBeatData() {
    std::lock_guard<std::mutex> lock(beatMutex);
    beatPositions.clear();
    beatPulse = 0.0f;
}

void WaveformVisualizer::render() {
    // Check if OpenGL context is current
    CGLContextObj currentContext = CGLGetCurrentContext();
    if (!currentContext) {
        std::cerr << "WaveformVisualizer::render() called but no OpenGL context!" << std::endl;
        return;
    }
    
    // Check if shader program is valid
    if (shaderProgram == 0) {
        std::cerr << "WaveformVisualizer::render() called but shader program not initialized!" << std::endl;
        return;
    }
    
    // Clear any existing errors
    while (glGetError() != GL_NO_ERROR) {}
    
    // Set viewport (NSOpenGLView might have changed it)
    glViewport(0, 0, viewportWidth, viewportHeight);
    
    // Clear with fog color
    glClearColor(fogColor.r * 0.3f, fogColor.g * 0.3f, fogColor.b * 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    // Check for OpenGL errors
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "OpenGL error after clear: " << err << std::endl;
        
        // Debug: check current context
        GLint drawFBO = 0, readFBO = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFBO);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFBO);
        std::cerr << "Draw FBO: " << drawFBO << ", Read FBO: " << readFBO << std::endl;
    }
    
    // Update animation time
    ambientTime += 0.016f; // ~60fps
    
    // Update transition
    if (currentMode != targetMode) {
        if (currentMode == MODE_TRANSITION) {
            transitionProgress += 0.05f; // Transition over ~1 second
            if (transitionProgress >= 1.0f) {
                transitionProgress = 1.0f;
                currentMode = targetMode;
            }
        } else {
            currentMode = MODE_TRANSITION;
            transitionProgress = 0.0f;
        }
    }
    
    // Update camera for rotating view
    if (currentMode == MODE_AMBIENT || (currentMode == MODE_TRANSITION && targetMode == MODE_AMBIENT)) {
        cameraAngle += 0.003f;
    } else if (currentMode == MODE_ANALYZING) {
        cameraAngle += 0.008f; // Faster rotation during analysis
    }
    
    float camX = sin(cameraAngle) * cameraDistance;
    float camZ = cos(cameraAngle) * cameraDistance;
    viewMatrix = glm::lookAt(glm::vec3(camX, 5.0f, camZ),
                            glm::vec3(0.0f, 0.0f, 0.0f),
                            glm::vec3(0.0f, 1.0f, 0.0f));
    
    // Use shader program
    glUseProgram(shaderProgram);
    
    // Set uniforms
    glm::mat4 mvp = projectionMatrix * viewMatrix * modelMatrix;
    glUniformMatrix4fv(mvpMatrixLoc, 1, GL_FALSE, &mvp[0][0]);
    
    // Debug log mode periodically (only when there's a problem)
    // Disabled to prevent performance issues
    /*
    static int modeLogCount = 0;
    if (modeLogCount++ % 600 == 0) { // Log less frequently
        std::cout << "Current mode: " << currentMode << ", Target mode: " << targetMode 
                  << ", Transition: " << transitionProgress 
                  << ", Ambient time: " << ambientTime << std::endl;
    }
    */
    
    // Render based on current mode
    if (currentMode == MODE_AMBIENT) {
        renderAmbientSwirl();
    } else if (currentMode == MODE_ANALYZING) {
        renderAnalysisWaveform();
    } else if (currentMode == MODE_TRANSITION) {
        // Blend between modes
        float t = transitionProgress;
        if (targetMode == MODE_ANALYZING) {
            // Transitioning to analysis - fade out ambient
            renderAmbientSwirl();
            if (!audioSamples.empty()) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                renderAnalysisWaveform();
                glDisable(GL_BLEND);
            }
        } else {
            // Transitioning to ambient - fade in ambient
            if (!audioSamples.empty()) {
                renderAnalysisWaveform();
            }
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            renderAmbientSwirl();
            glDisable(GL_BLEND);
        }
    }
    
    // Update beat pulse (decay over time)
    beatPulse *= 0.95f;
    
    // Apply bloom effect for glow
    // applyBloomEffect(); // Commented out for simplicity in initial implementation
}

void WaveformVisualizer::setVisualizationMode(VisualizationMode mode) {
    if (targetMode != mode) {
        targetMode = mode;
        transitionProgress = 0.0f;
        
        // Changing visualization mode
        
        // Reset analysis progress when starting analysis
        if (mode == MODE_ANALYZING) {
            analysisProgress = 0.0f;
            currentPosition = 0.0f;
        }
    }
}

void WaveformVisualizer::resize(int width, int height) {
    viewportWidth = width;
    viewportHeight = height;
    glViewport(0, 0, width, height);
    
    // Update projection matrix
    projectionMatrix = glm::perspective(glm::radians(45.0f), 
                                       (float)width / (float)height, 
                                       0.1f, 100.0f);
}

GLuint WaveformVisualizer::compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::cerr << "Shader compilation failed (" 
                  << (type == GL_VERTEX_SHADER ? "vertex" : "fragment") 
                  << "): " << infoLog << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    
    // Shader compiled successfully
    
    return shader;
}

GLuint WaveformVisualizer::createShaderProgram(const char* vertexSource, const char* fragmentSource) {
    GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    
    if (vertexShader == 0 || fragmentShader == 0) {
        return 0;
    }
    
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        std::cerr << "Program linking failed: " << infoLog << std::endl;
        glDeleteProgram(program);
        program = 0;
    }
    
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    
    return program;
}

void WaveformVisualizer::updateWaveformGeometry() {
    std::lock_guard<std::mutex> lock(audioMutex);
    
    if (audioSamples.empty()) {
        return;
    }
    
    // Generate current waveform points
    std::vector<WaveformPoint> currentPoints;
    generateWaveformPoints(currentPoints);
    
    // Add to history for trails
    waveformHistory.push_front(currentPoints);
    if (waveformHistory.size() > MAX_TRAIL_LENGTH) {
        waveformHistory.pop_back();
    }
    
    // Update waveform VBO
    std::vector<float> vertices;
    vertices.reserve(currentPoints.size() * 4);
    
    for (const auto& point : currentPoints) {
        vertices.push_back(point.position.x);
        vertices.push_back(point.position.y);
        vertices.push_back(point.position.z);
        vertices.push_back(point.intensity);
    }
    
    if (!vertices.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, waveformVBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), 
                     vertices.data(), GL_DYNAMIC_DRAW);
        
        static int updateCount = 0;
        if (updateCount++ % 60 == 0) {
            std::cout << "Updated waveform VBO with " << vertices.size() / 4 << " points" << std::endl;
            // Log first few vertices
            std::cout << "First vertex: (" << vertices[0] << ", " << vertices[1] 
                      << ", " << vertices[2] << ") intensity: " << vertices[3] << std::endl;
            if (vertices.size() >= 8) {
                std::cout << "Second vertex: (" << vertices[4] << ", " << vertices[5] 
                          << ", " << vertices[6] << ") intensity: " << vertices[7] << std::endl;
            }
        }
    }
}

void WaveformVisualizer::generateWaveformPoints(std::vector<WaveformPoint>& points) {
    const int numPoints = 512; // Number of points to display
    points.clear();
    points.reserve(numPoints);
    
    if (audioSamples.empty()) return;
    
    // Calculate starting position based on current playback position
    int startSample = (int)(currentPosition * sampleRate);
    startSample = std::max(0, std::min(startSample, (int)audioSamples.size() - numPoints));
    
    // Generate waveform points in 3D space
    for (int i = 0; i < numPoints; i++) {
        if (startSample + i >= audioSamples.size()) break;
        
        float sample = audioSamples[startSample + i];
        float x = ((float)i / numPoints - 0.5f) * waveformScale * 2.0f;
        float y = sample * waveformScale;
        float z = sin(i * 0.1f + currentPosition) * 0.5f; // Add some depth variation
        
        WaveformPoint point;
        point.position = glm::vec3(x, y, z);
        point.intensity = std::abs(sample);
        point.age = 0.0f;
        
        // Check if this position is near a beat
        double sampleTime = (startSample + i) / sampleRate;
        {
            std::lock_guard<std::mutex> lock(beatMutex);
            for (double beatTime : beatPositions) {
                if (std::abs(sampleTime - beatTime) < 0.05) { // Within 50ms of a beat
                    point.intensity = 1.0f; // Highlight beat positions
                    beatPulse = 1.0f; // Trigger pulse effect
                    break;
                }
            }
        }
        
        points.push_back(point);
    }
}

void WaveformVisualizer::renderWaveform() {
    // Check if we have valid VBO
    if (waveformVBO == 0) {
        std::cerr << "Invalid VBO in renderWaveform" << std::endl;
        return;
    }
    
    // Check if attribute locations are valid
    if (positionAttribLoc < 0 || intensityAttribLoc < 0) {
        std::cerr << "Invalid attribute locations" << std::endl;
        return;
    }
    
    // Bind the VBO directly (no VAO in compatibility profile)
    glBindBuffer(GL_ARRAY_BUFFER, waveformVBO);
    
    // Get the number of vertices from the buffer
    GLint bufferSize = 0;
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &bufferSize);
    
    if (bufferSize <= 0) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }
    
    int numVertices = bufferSize / (4 * sizeof(float));
    
    static int frameCount = 0;
    if (frameCount++ % 60 == 0) { // Log every 60 frames
        std::cout << "Rendering waveform: " << numVertices << " vertices, buffer size: " << bufferSize << std::endl;
        
        // Check if shader program is bound
        GLint currentProgram;
        glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
        std::cout << "Current shader program: " << currentProgram << " (expected: " << shaderProgram << ")" << std::endl;
    }
    
    if (numVertices > 0 && numVertices < 10000) { // Sanity check
        // Enable vertex attributes
        glEnableVertexAttribArray(positionAttribLoc);
        glEnableVertexAttribArray(intensityAttribLoc);
        
        // Set up vertex attribute pointers
        // Data layout: x, y, z, intensity (4 floats per vertex)
        glVertexAttribPointer(positionAttribLoc, 3, GL_FLOAT, GL_FALSE, 
                              4 * sizeof(float), (void*)0);
        glVertexAttribPointer(intensityAttribLoc, 1, GL_FLOAT, GL_FALSE, 
                              4 * sizeof(float), (void*)(3 * sizeof(float)));
        
        // Set line width
        glLineWidth(2.0f);
        
        // Draw the waveform
        glDrawArrays(GL_LINE_STRIP, 0, numVertices);
        
        // Check for errors after draw
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            std::cerr << "OpenGL error after glDrawArrays: " << err << std::endl;
        }
        
        // Disable vertex attributes
        glDisableVertexAttribArray(positionAttribLoc);
        glDisableVertexAttribArray(intensityAttribLoc);
    }
    
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void WaveformVisualizer::renderTrails() {
    // Render each historical waveform with decreasing opacity
    for (size_t i = 0; i < waveformHistory.size(); i++) {
        float alpha = 1.0f - (float)i / MAX_TRAIL_LENGTH;
        alpha *= 0.7f; // Make trails more subtle
        
        // Would need to modify shader to accept alpha uniform for proper trail rendering
        // For now, trails are implied through the persistence of vision effect
    }
}

float WaveformVisualizer::quantizePosition(float pos) {
    // PS1-style vertex snapping
    const float gridSize = 0.02f;
    return std::floor(pos / gridSize) * gridSize;
}

glm::vec3 WaveformVisualizer::applyFog(const glm::vec3& color, float depth) {
    float fogFactor = exp(-fogDensity * depth);
    return glm::mix(fogColor, color, fogFactor);
}

void WaveformVisualizer::renderAmbientSwirl() {
    // Enable blending for dreamy effect
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);  // Additive blending for glow
    
    // Render multiple layers with different phases for depth
    for (int layer = 0; layer < 3; layer++) {
        std::vector<WaveformPoint> ambientPoints;
        float layerPhase = layer * 2.0f;
        float layerScale = 1.0f + layer * 0.3f;
        float layerAlpha = 0.4f - layer * 0.1f;
        
        generateAmbientPoints(ambientPoints, layerPhase, layerScale);
        
        if (ambientPoints.empty()) continue;
        
        // Create vertex data
        std::vector<float> vertices;
        vertices.reserve(ambientPoints.size() * 4);
        
        for (const auto& point : ambientPoints) {
            vertices.push_back(point.position.x);
            vertices.push_back(point.position.y);
            vertices.push_back(point.position.z);
            vertices.push_back(point.intensity * layerAlpha);
        }
        
        // Update VBO
        glBindBuffer(GL_ARRAY_BUFFER, waveformVBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), 
                     vertices.data(), GL_DYNAMIC_DRAW);
        
        // Setup attributes
        glEnableVertexAttribArray(positionAttribLoc);
        glEnableVertexAttribArray(intensityAttribLoc);
        glVertexAttribPointer(positionAttribLoc, 3, GL_FLOAT, GL_FALSE, 
                              4 * sizeof(float), (void*)0);
        glVertexAttribPointer(intensityAttribLoc, 1, GL_FLOAT, GL_FALSE, 
                              4 * sizeof(float), (void*)(3 * sizeof(float)));
        
        // Vary line width by layer
        glLineWidth(3.0f - layer * 0.5f);
        
        // Draw multiple passes for glow effect
        for (int pass = 0; pass < 2; pass++) {
            glDrawArrays(GL_LINE_STRIP, 0, ambientPoints.size());
        }
        
        glDisableVertexAttribArray(positionAttribLoc);
        glDisableVertexAttribArray(intensityAttribLoc);
    }
    
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
}

void WaveformVisualizer::generateAmbientPoints(std::vector<WaveformPoint>& points, float phase, float scale) {
    const int numPoints = 256;
    points.clear();
    points.reserve(numPoints);
    
    // Create flowing, dreamy patterns
    for (int i = 0; i < numPoints; i++) {
        float t = (float)i / numPoints;
        
        // Multiple sine waves for complex motion
        float time = ambientTime + phase;
        float angle = t * M_PI * 4.0f + time * 0.3f;
        
        // Flowing radius with multiple frequencies
        float radius = 2.0f * scale;
        radius += sin(time * 0.2f + t * M_PI * 2.0f + phase) * 1.5f;
        radius += cos(time * 0.15f + t * M_PI * 3.0f) * 0.8f;
        
        // Vertical motion with drift
        float height = (t - 0.5f) * 6.0f * scale;
        height += sin(time * 0.4f + t * M_PI * 2.0f + phase * 0.5f) * 2.0f;
        height += cos(time * 0.3f + t * M_PI * 4.0f) * 1.0f;
        
        // Complex wave distortions for dreamy effect
        float wave1 = sin(time * 0.8f + t * M_PI * 6.0f + phase) * 0.5f;
        float wave2 = cos(time * 0.6f + t * M_PI * 5.0f - phase * 0.7f) * 0.4f;
        float wave3 = sin(time * 1.1f + t * M_PI * 7.0f) * 0.3f;
        
        WaveformPoint point;
        point.position = glm::vec3(
            cos(angle) * radius + wave1 + wave3 * 0.5f,
            height + wave2,
            sin(angle) * radius + wave2 + wave3 * 0.5f
        );
        
        // Pulsing intensity with multiple waves
        float intensity = 0.5f;
        intensity += 0.3f * sin(time * 1.5f + t * M_PI * 4.0f + phase);
        intensity += 0.2f * cos(time * 2.0f + t * M_PI * 6.0f);
        intensity = glm::clamp(intensity, 0.1f, 1.0f);
        
        point.intensity = intensity;
        point.age = 0.0f;
        
        points.push_back(point);
    }
}

// Add overload for backward compatibility
void WaveformVisualizer::generateAmbientPoints(std::vector<WaveformPoint>& points) {
    generateAmbientPoints(points, 0.0f, 1.0f);
}

void WaveformVisualizer::renderAnalysisWaveform() {
    if (audioSamples.empty()) return;
    
    // Update analysis progress
    analysisProgress += 0.01f;
    if (analysisProgress > 1.0f) analysisProgress = 1.0f;
    
    // Move through the waveform based on analysis progress
    currentPosition = analysisProgress;
    
    // Generate waveform points
    std::vector<WaveformPoint> wavePoints;
    generateWaveformPoints(wavePoints);
    
    if (wavePoints.empty()) return;
    
    // Create vertex data
    std::vector<float> vertices;
    vertices.reserve(wavePoints.size() * 4);
    
    for (const auto& point : wavePoints) {
        vertices.push_back(point.position.x);
        vertices.push_back(point.position.y);
        vertices.push_back(point.position.z);
        vertices.push_back(point.intensity);
    }
    
    // Update VBO
    glBindBuffer(GL_ARRAY_BUFFER, waveformVBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), 
                 vertices.data(), GL_DYNAMIC_DRAW);
    
    // Render
    glEnableVertexAttribArray(positionAttribLoc);
    glEnableVertexAttribArray(intensityAttribLoc);
    glVertexAttribPointer(positionAttribLoc, 3, GL_FLOAT, GL_FALSE, 
                          4 * sizeof(float), (void*)0);
    glVertexAttribPointer(intensityAttribLoc, 1, GL_FLOAT, GL_FALSE, 
                          4 * sizeof(float), (void*)(3 * sizeof(float)));
    
    glLineWidth(2.0f);
    glDrawArrays(GL_LINE_STRIP, 0, wavePoints.size());
    
    // Also render beat markers if we have them
    glPointSize(8.0f);
    for (const auto& beatPos : beatPositions) {
        if (beatPos <= currentPosition) {
            // This beat has been analyzed, highlight it
            float beatPoint[] = {0.0f, 2.0f, 0.0f, 1.0f};
            glBufferData(GL_ARRAY_BUFFER, sizeof(beatPoint), beatPoint, GL_DYNAMIC_DRAW);
            glDrawArrays(GL_POINTS, 0, 1);
        }
    }
    
    glDisableVertexAttribArray(positionAttribLoc);
    glDisableVertexAttribArray(intensityAttribLoc);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}