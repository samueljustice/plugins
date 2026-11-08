#pragma once

#include <JuceHeader.h>
#include "Theme.h"
#include "dsp/MonsterFeatures.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>

/**
 * The creature view: one fragment shader on a full-screen quad draws a mirror-symmetric ink-blot
 * creature head that is animated by what the audio is doing (roar opens the maw, growl shakes the
 * teeth, liquid ripples the skin and drips ink, sub sends out rings, the vocoder lights a frill of
 * 32 ridges under the jaw). After 1.5 s of silence it falls asleep.
 *
 * Threads: the audio thread writes MonsterFeatures (atomics); the message thread writes the knob
 * values with setParams() (atomics); the GL thread reads both in renderOpenGL() and keeps its own
 * smoothing state. A small Graphics overlay draws the readouts and the shader-failure fallback.
 */
class MonsterView : public juce::Component, public juce::OpenGLRenderer, private juce::Timer
{
public:
    /** The current knob values. Percent controls are 0..1, pitch is in semitones. */
    struct Params
    {
        float pitchSt = -2.0f;
        float size = 0.35f;
        float growl = 0.30f;
        float liquid = 0.30f;
        float roar = 0.35f;
        float sub = 0.25f;
        float gravel = 0.0f;
        float detailLow = 0.30f;
        float detailHigh = 0.20f;
    };

    explicit MonsterView (const roar::MonsterFeatures& features);
    ~MonsterView() override;

    /** Message thread. Stores into atomics read by the GL thread. */
    void setParams (const Params& params);

    /** Message thread. Radius (component px) of the rounded corners and the colour drawn outside them. */
    void setCorner (float radius, juce::Colour outside);

    void paint (juce::Graphics&) override;
    void resized() override;

    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

private:
    void timerCallback() override;
    bool buildShader();
    void releaseGL();
    void updateAnimation (float dt);

    static constexpr int c_numBands = roar::MonsterFeatures::numBands;

    const roar::MonsterFeatures& m_features;
    juce::OpenGLContext m_context;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;

    // Written on the message thread, read on the GL thread.
    std::atomic<float> m_pitchSt { -2.0f }, m_size { 0.35f }, m_growl { 0.30f }, m_liquid { 0.30f },
                       m_roar { 0.35f }, m_sub { 0.25f }, m_gravel { 0.0f },
                       m_detailLow { 0.30f }, m_detailHigh { 0.20f };
    std::atomic<int> m_width { 0 }, m_height { 0 };
    std::atomic<float> m_cornerRadius { 8.0f };
    std::atomic<juce::uint32> m_outsideArgb { Theme::Colours::background.getARGB() };

    // Written on the GL thread, read by the overlay.
    std::atomic<bool> m_shaderFailed { false };
    std::atomic<bool> m_glReady { false };   // a context was created and the shader built
    std::atomic<bool> m_asleep { false };
    std::atomic<bool> m_stale { false };     // the audio thread stopped publishing (host not processing)
    std::atomic<bool> m_idle { false };      // asleep and fully settled: frames can come slowly

    // GL thread only.
    std::unique_ptr<juce::OpenGLShaderProgram> m_program;
    GLuint m_vbo = 0, m_vao = 0;
    GLint m_positionAttrib = -1;

    enum Uniform
    {
        uRes, uPxScale, uRadius, uTime, uJaw, uGrowl, uGrowlMod, uLiquid, uLiquidPhase, uRoar,
        uRoarDrive, uSub, uSubLevel, uSubPhase, uPitch, uSize, uGravelOn, uDetailLow, uDetailHigh,
        uBoost, uCut, uSleep, uBlink, uLevel, uBands,
        uColScope, uColGrid, uColOuter, uColPitch, uColGrowl, uColLiquid, uColRoar, uColSub,
        uColDetail, uColVocoder, uColOutput, uColText,
        numUniforms
    };
    std::array<GLint, numUniforms> m_uniforms {};

    using Clock = std::chrono::steady_clock;
    Clock::time_point m_startTime { Clock::now() };
    Clock::time_point m_lastFrame {};
    bool m_firstFrame = true;

    // Smoothed animation state (GL thread only).
    float m_jaw = 0.0f, m_growlMod = 0.0f, m_roarDrive = 0.0f, m_subLevel = 0.0f, m_level = 0.0f;
    float m_boost = 0.0f, m_cut = 0.0f, m_gravelOn = 0.0f, m_sleep = 0.0f, m_subPhase = 0.0f;
    float m_quietSeconds = 0.0f, m_staleSeconds = 0.0f;
    int m_lastGeneration = -1;
    float m_blinkClock = 0.0f, m_nextBlink = 3.0f;
    Params m_knobs;  // the knob values, smoothed so turning one animates instead of jumping
    std::array<float, c_numBands> m_bands {};
    juce::Random m_random { 0x5ea1 };

    // Message thread only (overlay readouts).
    juce::String m_f0Text, m_detailText;
    bool m_gravelShown = false, m_sleepShown = false, m_failShown = false;
    int m_frameTick = 0;       // counts timer ticks for the slow idle frame rate
    int m_ticksWithoutGL = 0;  // timer ticks spent on screen without a working GL context

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MonsterView)
};
