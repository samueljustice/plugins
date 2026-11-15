#pragma once

#include <JuceHeader.h>

/**
 * The snippet display's OpenGL scene: a road running from NOW (the near edge) to a horizon in the
 * past. The road's width is the input's level at each moment, a grid line crosses it every second,
 * and each read head is a gate of light sweeping back along it. Drawn by one fragment shader over
 * a rectangle, inside the editor's OpenGL context (see ReversinatorAudioProcessorEditor).
 */
namespace TimeHighway
{
    constexpr int numPeaks = 128;

    /** How far up the floor (0 = near edge, 1 = horizon) a moment `age` seconds ago is drawn. */
    float floorPositionForAge(float ageSeconds, float spanSeconds);

    /** The horizon's height as a fraction of the view, measured from the bottom. */
    constexpr float horizon = 0.70f;

    /** One frame's worth of data, written on the message thread and read by the GL thread. */
    struct Frame
    {
        std::array<float, numPeaks> peaks {};   // input level from NOW (0) to spanSeconds ago (last)
        float spanSeconds = 4.35f;
        std::array<float, 2> headAge { 0.0f, 0.0f };
        std::array<float, 2> headGain { 0.0f, 0.0f };
        bool reversing = true;
    };

    class Renderer
    {
    public:
        /** Call with the context active. Returns false (and draws nothing later) if the shader fails. */
        bool create(juce::OpenGLContext&);
        void release();

        /** Draws into `viewport` (GL pixels, origin bottom-left). */
        void draw(juce::OpenGLContext&, juce::Rectangle<int> viewport, const Frame&, double seconds);

        bool isReady() const { return m_program != nullptr; }

    private:
        std::unique_ptr<juce::OpenGLShaderProgram> m_program;
        GLuint m_quad = 0;
        double m_lastSeconds = 0.0;
        float m_flow = 0.0f;
    };
}
