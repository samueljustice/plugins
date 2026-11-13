#pragma once

#include <JuceHeader.h>
#include "CreatureEvolution.h"
#include "CreatureMood.h"

#include <optional>
#include "CreatureMotion.h"
#include "LogoSdf.h"
#include "MonsterRenderer.h"
#include "Theme.h"
#include "dsp/MonsterFeatures.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

/**
 * The creature view: a swirling ink creature in the clouds, animated by what the audio is doing.
 * Asleep it is lit cloud carrying a little pigment; a voice turns the cloud and draws that pigment
 * in along mirrored eddies until it gathers into one folded, wet ink blot; quiet lets it go back to
 * the flow as ribbons and smoke. The voice works a heavy, damped jaw (JawMotion) that tears a
 * ragged rift on the fold; a roar swells and darkens the mass and breathes lit vapour out of the
 * rift; each of the fifteen modules leaves its own trace on the shape, the flow or the pigment.
 * After 1.5 s of silence it falls asleep.
 *
 * What the blot is grows from a seed and with the session (CreatureEvolution, stepped by the
 * processor: the stage switches structure in, the characters bend it; CreatureAnatomy turns both
 * into the shape); the view only reads the evaluated parameters and smooths them. The renderer
 * (MonsterRenderer) steps an ink buffer on capable GPUs and draws one stateless pass on weak or
 * software ones. A right-click on the view offers Live / Frozen evolution and a reset.
 *
 * Threads: the audio thread writes MonsterFeatures (atomics); the message thread writes the knob
 * values with setParams() (atomics); the GL thread reads both in renderOpenGL() and keeps its own
 * smoothing state. A small Graphics overlay draws the readouts, and a still cloud with a line of
 * text where OpenGL can't run.
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
        float swirl = 0.0f;        // SWIRL amount (the bound eddy pair's strength)
        float ensemble = 0.0f;     // ENSEMBLE amount (the pale echoes' spread)
        float overtone = 0.0f;     // OVERTONE amount (the bristles' length)
        float ring = 0.0f;         // RING amount (the ripples from the rift)
        float cleanOn = 1.0f;      // CLEAN is On: its gate may close the lips
        float clean = 0.0f;        // CLEAN's containment above its usual setting (0..1): how tidy the edge is
        roar::CreatureMood::Axes mood {};   // the colour mood of the settings (CreatureMood::evaluate)
        bool moodFrozen = false;   // the creature's evolution is Frozen: the colour holds too
        std::optional<roar::CreatureMood::Axes> moodKept;   // Frozen: the colour saved with the session (if any)
    };

    /** creature: the evolved form published by the processor (nullptr: a fixed, fully formed one). */
    explicit MonsterView (const roar::MonsterFeatures& features, const roar::CreatureShared* creature = nullptr);
    ~MonsterView() override;

    /** The right-click menu's actions (set by the editor; the menu shows only when they are set). */
    std::function<bool()> isCreatureFrozen;
    std::function<void (bool)> setCreatureFrozen;
    std::function<void()> resetCreature;
    /** Keeps the colour a Frozen creature holds with the session (nullopt: none, it is Live). */
    std::function<void (std::optional<roar::CreatureMood::Axes>)> keepCreatureColour;

    void mouseDown (const juce::MouseEvent&) override;

    /** One quiet line of help at the bottom of the view, shown while no audio has arrived yet and
        the creature has slept through 1.5 s of silence. The first sound takes it away for good and
        calls onAudioHeard (once). An empty text shows nothing. Message thread. */
    void setHint (const juce::String& text);
    std::function<void()> onAudioHeard;
    bool isHintShowing() const noexcept { return m_hintShown; }
    /** Refreshes the overlay (the readouts and the hint) after this much time; the view's timer
        calls it on every tick. */
    void refreshOverlay (float seconds);

    /** Tests and snapshots: draw what the view shows where OpenGL can't run. */
    void showFallbackForTests() { m_shaderFailed.store (true, std::memory_order_relaxed); refreshOverlay (0.0f); }

    /** Tests and snapshots: a fixed 1/30 s step, so frames repeat. */
    void setFixedStepForTests (bool fixed) { m_testFixedStep.store (fixed, std::memory_order_relaxed); }

    /** The renderer's quality tier in use (0..3, -1 before the first frame). */
    int getQualityTier() const { return m_tier.load (std::memory_order_relaxed); }

    /** Tests and snapshots: hold the creature's formation (< 0 = live). */
    void setFormationForTests (float formation) { m_testFormation.store (formation, std::memory_order_relaxed); }

    /** Tests: hold the drawn jaw (0..1; < 0 = live). */
    void setJawForTests (float jaw) { m_testJaw.store (jaw, std::memory_order_relaxed); }
    /** Tests and snapshots: the ink creature's pass at full size (1), half size (0) or by the GPU (-1). */
    void setInkFullSizeForTests (int full) { m_renderer.setInkFullSize (full); }
    /** Tests: the cloud in front of the ink creature at this share of its opacity (0: the ink bare). */
    void setInkFrontCloudForTests (float share) { m_renderer.setInkFrontCloud (share); }
    /** Tests: the light in the ink creature's rift held at this share of the blot's own (< 0 = live). */
    void setInkMomentForTests (float moment) { m_renderer.setInkMoment (moment); }
    /** Tests: the light in the ink creature's rift itself, whatever the blot's own is (< 0 = the blot's). */
    void setInkLightForTests (float light) { m_renderer.setInkLight (light); }
    void setInkTracesOffForTests (bool off) { m_renderer.setInkTracesOff (off); }
    /** Tests and snapshots: hold the colour mood at these axes whatever the settings are (any < 0 = live). */
    void setMoodForTests (const roar::CreatureMood::Axes& axes)
    {
        for (size_t i = 0; i < axes.size(); ++i)
            m_testMood[i].store (axes[i], std::memory_order_relaxed);
    }

    /** Message thread. Stores into atomics read by the GL thread. */
    void setParams (const Params& params);

    /** Message thread. Radius (component px) of the rounded corners and the colour drawn outside them. */
    void setCorner (float radius, juce::Colour outside);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Tests and snapshots: the next GL frame is read back on the GL thread; takeCapture() returns it
        (an invalid image until a frame has been read). */
    void requestCapture() { m_captureRequested.store (true, std::memory_order_relaxed); }
    juce::Image takeCapture();

    /** Tests and snapshots: one frame of this view (the same animation, frame and renderer) into the
        framebuffer bound in a GL context the caller has made current; the view's own context is not
        used. dt: seconds since the frame before. Follow with requestCapture() / takeCapture(), and
        call releaseOffline() with that context current before the view goes. */
    void renderOffline (float dt, int width, int height, bool coreProfile = true);
    void releaseOffline();
    const roar::MonsterRenderer& getRendererForTests() const { return m_renderer; }
    /** Tests and the screenshot tool (their own thread is the GL thread): hold a tier, read the ink buffer. */
    roar::MonsterRenderer& rendererForTests() { return m_renderer; }

    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

private:
    void timerCallback() override;
    void releaseGL();
    void createRenderer (bool coreProfile);
    void drawFrame (float dt, float scale, int width, int height);
    void updateAnimation (float dt);
    void showCreatureMenu();

    static constexpr int c_numBands = roar::MonsterFeatures::numBands;

    const roar::MonsterFeatures& m_features;
    std::unique_ptr<roar::CreatureShared> m_ownCreature;   // when no processor publishes one
    const roar::CreatureShared& m_creature;
    const LogoSdf& m_logo { LogoSdf::shared() };            // built once per process (on the first editor)
    std::atomic<float> m_testFormation { -1.0f }, m_testJaw { -1.0f };
    std::atomic<bool> m_testFixedStep { false };
    juce::OpenGLContext m_context;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;

    // Written on the message thread, read on the GL thread.
    std::atomic<float> m_pitchSt { -2.0f }, m_size { 0.35f }, m_growl { 0.30f }, m_liquid { 0.30f },
                       m_roar { 0.35f }, m_sub { 0.25f }, m_gravel { 0.0f },
                       m_detailLow { 0.30f }, m_detailHigh { 0.20f },
                       m_swirlKnob { 0.0f }, m_ensembleKnob { 0.0f }, m_overtoneKnob { 0.0f }, m_ringKnob { 0.0f },
                       m_cleanOn { 1.0f }, m_clean { 0.0f };
    std::array<std::atomic<float>, roar::CreatureMood::numAxes> m_moodTarget {};
    std::array<std::atomic<float>, roar::CreatureMood::numAxes> m_testMood { { { -1.0f }, { -1.0f }, { -1.0f }, { -1.0f } } };
    std::atomic<bool> m_moodFrozen { false };
    std::atomic<int> m_moodResets { 0 };     // bumped by the menu's reset (message thread)
    std::array<std::atomic<float>, roar::CreatureMood::numAxes> m_moodKept {};   // the saved frozen colour
    std::atomic<bool> m_moodHasKept { false };
    std::array<std::atomic<float>, roar::CreatureMood::numAxes> m_moodShown {};  // written on the GL thread
    std::atomic<int> m_width { 0 }, m_height { 0 };
    std::atomic<float> m_cornerRadius { 8.0f };
    std::atomic<juce::uint32> m_outsideArgb { Theme::Colours::background.getARGB() };

    // Written on the GL thread, read by the overlay.
    std::atomic<bool> m_shaderFailed { false };
    std::atomic<bool> m_glReady { false };   // a context was created and the shader built
    std::atomic<bool> m_asleep { false };
    std::atomic<bool> m_stale { false };     // the audio thread stopped publishing (host not processing)
    std::atomic<bool> m_idle { false };      // asleep and fully settled: frames can come slowly
    std::atomic<int> m_tier { -1 };          // the renderer's tier (the idle frame rate depends on it)

    // Frame read-back for snapshots (never used while a host runs the plug-in).
    std::atomic<bool> m_captureRequested { false };
    juce::CriticalSection m_captureLock;
    juce::Image m_capture;

    // GL thread only.
    roar::MonsterRenderer m_renderer;
    roar::MonsterFrame m_frame;
    roar::JawMotion m_jawMotion;
    roar::HeadMotion m_head;
    roar::AudioForces m_audioForces;
    float m_jawFast = 0.0f;
    float m_lastInterval = 1.0f / 30.0f;
    bool m_lastClamped = false;
    float m_inputLoud = 0.0f;        // the input's loudness, 0 (-30 dB) .. 1 (-8 dB), smoothed

    using Clock = std::chrono::steady_clock;
    Clock::time_point m_lastFrame {};
    bool m_firstFrame = true;

    // Smoothed animation state (GL thread only).
    float m_jaw = 0.0f, m_growlMod = 0.0f, m_roarDrive = 0.0f, m_subLevel = 0.0f, m_level = 0.0f;
    float m_gravelOn = 0.0f, m_sleep = 0.0f, m_subPhase = 0.0f;
    float m_swirlNotch = 0.0f, m_ensWidth = 0.0f, m_deess = 0.0f, m_tame = 0.0f, m_gate = 1.0f;
    // CHAMBER: how much the room rings (with Resonance), the Boom pulse's depth and its slow phase.
    float m_chamberEcho = 0.0f, m_chamberBoom = 0.0f, m_chamberPhase = 0.0f;
    float m_ovLevel = 0.0f, m_ovReplace = 0.0f, m_ring = 0.0f, m_ringPhase = 0.0f, m_ringBody = 0.0f;
    float m_strain = 0.0f, m_clip = 0.0f;
    std::array<float, roar::MonsterFeatures::numOvertoneHarmonics> m_ovHarm {};
    float m_quietSeconds = 0.0f, m_staleSeconds = 0.0f;
    int m_lastGeneration = -1;
    Params m_knobs;  // the knob values, smoothed so turning one animates instead of jumping
    roar::MoodSpring m_mood;                    // the clouds' colour mood, drifting to the settings'
    roar::ToneFollower m_tone;                  // its colour on screen (a bounded turn)
    int m_moodResetsSeen = 0;
    bool m_moodSnap = true;                     // the first frame takes the colour as it is
    std::array<float, c_numBands> m_bands {};
    roar::CreatureEvolution::Params m_evo {};   // the evolved form, smoothed per frame
    roar::CreatureFormation m_formation;
    bool m_evoSnap = true;                      // the first frame takes the form as it is

    // Message thread only (overlay readouts).
    juce::String m_f0Text, m_detailText;
    bool m_gravelShown = false, m_sleepShown = false, m_failShown = false;
    juce::String m_hintText;         // "" once audio has been heard (or when the editor set none)
    float m_hintQuietSeconds = 0.0f; // silence since the view opened
    bool m_hintShown = false;
    int m_timerHz = 30;        // the frame timer's current rate (30 awake, slower once asleep and settled)
    int m_ticksWithoutGL = 0;  // timer ticks spent on screen without a working GL context

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MonsterView)
};
