#ifndef WAVEFORM_VISUALIZER_H
#define WAVEFORM_VISUALIZER_H

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>
#include <vector>
#include <deque>
#include <mutex>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class WaveformVisualizer {
public:
    WaveformVisualizer();
    ~WaveformVisualizer();
    
    // Initialize OpenGL resources
    bool initialize(int width, int height);
    void shutdown();
    
    // Update and render
    void updateAudioData(const std::vector<float>& samples, double sampleRate);
    void updateBeatData(double beatTime, double tempo);
    void clearBeatData();
    void render();
    void resize(int width, int height);
    
    // Animation controls
    void setPlaybackPosition(double position) { currentPosition = position; }
    void setIsPlaying(bool playing) { isPlaying = playing; }
    
    // Visualization modes
    enum VisualizationMode {
        MODE_AMBIENT,      // Ambient swirl when idle
        MODE_ANALYZING,    // Waveform during analysis
        MODE_TRANSITION    // Morphing between states
    };
    void setVisualizationMode(VisualizationMode mode);
    
private:
    // Window dimensions
    int viewportWidth;
    int viewportHeight;
    
    // OpenGL resources
    GLuint waveformVBO;
    GLuint trailVBO;
    GLuint shaderProgram;
    GLuint bloomFBO, bloomTexture;
    GLuint blurFBO[2], blurTexture[2];
    
    // Shader locations
    GLint mvpMatrixLoc;
    GLint timeUniformLoc;
    GLint beatPulseLoc;
    GLint fogColorLoc;
    GLint fogDensityLoc;
    GLint positionAttribLoc;
    GLint intensityAttribLoc;
    
    // Audio data
    std::vector<float> audioSamples;
    std::mutex audioMutex;
    double sampleRate;
    double currentPosition;
    bool isPlaying;
    
    // Beat data
    double lastBeatTime;
    double currentTempo;
    double beatPulse;
    std::vector<double> beatPositions;
    std::mutex beatMutex;
    
    // Waveform trail data
    struct WaveformPoint {
        glm::vec3 position;
        float intensity;
        float age;
    };
    std::deque<std::vector<WaveformPoint>> waveformHistory;
    static const int MAX_TRAIL_LENGTH = 60; // frames
    
    // Matrices
    glm::mat4 projectionMatrix;
    glm::mat4 viewMatrix;
    glm::mat4 modelMatrix;
    
    // Visual parameters
    glm::vec3 fogColor;
    float fogDensity;
    float waveformScale;
    float cameraDistance;
    float cameraAngle;
    
    // Visualization state
    VisualizationMode currentMode;
    VisualizationMode targetMode;
    float transitionProgress;
    float ambientTime;
    float analysisProgress;
    
    // Shader compilation
    GLuint compileShader(GLenum type, const char* source);
    GLuint createShaderProgram(const char* vertexSource, const char* fragmentSource);
    
    // Rendering functions
    void updateWaveformGeometry();
    void renderWaveform();
    void renderTrails();
    void applyBloomEffect();
    void generateWaveformPoints(std::vector<WaveformPoint>& points);
    
    // Mode-specific rendering
    void renderAmbientSwirl();
    void renderAnalysisWaveform();
    void generateAmbientPoints(std::vector<WaveformPoint>& points);
    void generateAmbientPoints(std::vector<WaveformPoint>& points, float phase, float scale);
    
    // PS1 style effects
    float quantizePosition(float pos); // Vertex snapping
    glm::vec3 applyFog(const glm::vec3& color, float depth);
};

#endif // WAVEFORM_VISUALIZER_H