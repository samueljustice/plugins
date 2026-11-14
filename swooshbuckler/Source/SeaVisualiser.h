#pragma once

#include <JuceHeader.h>
#include "LogoOutline.h"
#include "PluginProcessor.h"
#include "Theme.h"

#include <array>
#include <deque>
#include <vector>

/**
 * The sea: the hero of the window. The output spectrum rolls away from the camera as a moonlit swell (a
 * perspective mesh drawn back to front, so near waves hide far ones), and every doppler pass is a comet arcing
 * across the sky in its bus's colour, splashing the water under it. Hits fire a flash on the horizon and a ring
 * on the water; a click drops a ripple where it landed; the moon's halo warms and widens with the limiter; and
 * every so often the mark draws itself in the stars as a constellation.
 *
 * Stowaway shows in its own colour, by its Mode, and not at all while it is switched off: a Sub lifts the whole
 * swell with its level, a Wake is a trail on the water behind bus A's comet, Fly is a third comet, and Dry (which
 * does not move) is a lantern floating in the middle that glows with its level.
 *
 * Drawn with juce::Graphics. One frame clock: a VBlankAttachment when the window is on a screen, a 60 Hz timer
 * when it is not (the offline rigs in tools/ and Tests/ step timerCallback() by hand). After five seconds of
 * quiet the sea steps at a third of the rate.
 *
 * The sky (its gradient and the still stars) is kept in an image and redrawn only when the view changes shape;
 * everything that moves is painted over it.
 */
class SeaVisualiser : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    SeaVisualiser(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts);
    ~SeaVisualiser() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

    std::function<void()> onClick;
    /** The names over the comets are left out while something sits over the sea (the morph pad, with labels of its own). */
    void setLabelsShown(bool shown) { m_labelsShown = shown; }
    /** The morph pad is docked over the sea: the water behind it is dimmed and nothing is drawn under it. */
    void setDimmedUnder(juce::Rectangle<int> dock);
    /** Where a control of the window's own sits over the sea (the Morph button), so labels keep off it. */
    void setReservedArea(juce::Rectangle<int> area) { m_reserved = area.toFloat(); }
    /** Shows the constellation now, and (held) keeps it showing: for the offline pictures. */
    void showConstellation(bool holdOn);

    /** One frame. Public so the offline rigs can step the sea without a message loop. */
    void timerCallback() override;

    /** Which comet a flight belongs to: a bus (0 .. c_maxBuses - 1), or c_maxBuses for the thrown whooshes. */
    static int cometGroup(int flight)
    {
        return flight >= RigEngine::c_maxBuses * 2 ? RigEngine::c_maxBuses : flight / 2;
    }
    static constexpr int c_cometGroups = RigEngine::c_maxBuses + 1;
    /** The names drawn over the sea: one for each comet group, and one for Stowaway. */
    static constexpr int c_stowLabel = c_cometGroups, c_labels = c_cometGroups + 1;
    /** How far Stowaway shows in each Mode right now (0..1; all nothing while it is off or silent). For the tests. */
    float stowawayShown(int mode) const { return m_stowShown[static_cast<size_t>(juce::jlimit(0, ParamIDs::numStowModes - 1, mode))]; }

    /** Where the mark in the stars is drawn: open sky, clear of the moon and of anything over the sea. */
    juce::Rectangle<float> constellationArea() const;

    /** Moves a label down out of whatever it lands on: the readouts and buttons over the sea (keepOut), then the
        labels already placed. Returns where it goes. Static so the rule can be tested on its own. */
    static juce::Rectangle<float> clearOf(juce::Rectangle<float> area, const juce::Rectangle<float>* keepOut, int numKeepOut,
                                          const juce::Rectangle<float>* placed, int numPlaced);

private:
    static constexpr int c_fftOrder = 11;
    static constexpr int c_fftSize = 1 << c_fftOrder;
    static constexpr int c_bands = 80;
    static constexpr int c_rows = 34;
    static constexpr int c_trail = 34;
    static constexpr float c_rowsPerSecond = 20.0f; // how fast the swell rolls away (rows are drawn between steps)
    static constexpr float c_stepSeconds = 1.0f / 75.0f;  // never step more often than this, however fast the screen is
    static constexpr float c_idleStepSeconds = 1.0f / 20.0f; // and after five seconds of quiet, only this often

    struct P3 { float x, y, z; };

    void step(float dt);
    void analyse();
    juce::Point<float> project(P3 p) const;
    void buildSky(float pixelScale);
    void buildConstellation();
    void paintWater(juce::Graphics&, float h);
    void paintComets(juce::Graphics&, float w);
    void paintStowaway(juce::Graphics&, float h);
    void paintConstellation(juce::Graphics&, float w);

    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;

    juce::dsp::FFT m_fft { c_fftOrder };
    juce::dsp::WindowingFunction<float> m_window { c_fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::array<float, c_fftSize * 2> m_fftData {};
    std::array<float, c_bands> m_smooth {};
    std::array<std::array<float, c_bands>, c_rows> m_rows {};
    int m_rowHead = 0;
    float m_rowPhase = 0.0f;   // 0..1 between two row steps: the swell rolls on smoothly

    // The wake a pass leaves in the water, one bump per band, and the ripple a click drops.
    std::array<float, c_bands> m_wake {};
    struct Ripple { float x = 0.0f, z = 0.0f, age = 1.0f; };
    std::array<Ripple, 3> m_ripples {};
    int m_rippleNext = 0;

    std::array<std::deque<P3>, RigEngine::c_maxFlights> m_trails;
    std::array<float, RigEngine::c_maxFlights> m_flightEnv {};
    std::array<float, RigEngine::c_maxFlights> m_flash {};
    std::array<float, RigEngine::c_maxFlights> m_lastX {};
    // Stowaway. Fly's two passes are comets like the buses', kept beside theirs.
    std::array<std::deque<P3>, 2> m_stowTrails;
    std::array<float, 2> m_stowEnv {}, m_stowFlash {}, m_stowLastX {};
    std::array<float, ParamIDs::numStowModes> m_stowShown {}; // its level, in the Mode it is in (the others fall away)
    static constexpr int c_wakePoints = 64;
    struct WakePoint { float x = 0.0f, z = 0.0f, strength = 0.0f; };
    std::deque<WakePoint> m_wakeTrail;                         // where bus A's comet has been over the water (Wake)
    juce::Point<float> m_stowLabelAt;                          // where Stowaway's name goes (set as it is painted)
    float m_stowLabelAlpha = 0.0f;
    bool stepFlight(const RigEngine::FlightView&, std::deque<P3>& trail, float& env, float& flash, float& lastX, float dt);

    // Hits: a flash on the horizon and a ring on the water.
    float m_hitFlash = 0.0f, m_hitRing = 0.0f, m_lastGate = 0.0f;
    float m_limiterGlow = 0.0f;   // the moon's halo, from the limiter's gain reduction
    float m_swell = 0.0f;         // how high the ambient swell is riding (Swell held)

    struct Star { float x, y, size, phase; };
    std::vector<Star> m_stars;    // the twinkling few; the rest are in the sky image

    // The mark in the stars: brighter stars on the logo's outline, joined by faint lines.
    struct ConstellationStar { juce::Point<float> at; float size, phase; };
    std::vector<ConstellationStar> m_constellation;          // normalised, centred, y-up
    std::vector<std::pair<int, int>> m_constellationLines;
    float m_constellationOpacity = 0.0f, m_constellationTime = 0.0f, m_constellationLeft = 0.0f, m_constellationNext = 20.0f;
    bool m_constellationHeld = false;
    int m_takesSeen = -1;
    juce::Random m_random { 1337 };

    std::unique_ptr<LogoOutline> m_logo;
    juce::Image m_sky;            // the gradient and the still stars, redrawn only when the view changes shape
    float m_skyScale = 0.0f;
    double m_time = 0.0;
    float m_quietSeconds = 0.0f;
    float m_sinceStep = 0.0f;     // time gathered up since the last step: the sea's own rate, not the screen's
    bool m_labelsShown = true;
    juce::Rectangle<float> m_reserved, m_dock;
    juce::Rectangle<float> m_titleBox, m_recBox;

    juce::VBlankAttachment m_vblank;
    double m_lastVBlank = 0.0;
    juce::uint32 m_vblankSeenMs = 0;
    bool m_timerIsWatch = false;  // the timer has been put down to its watchdog rate (so it is not reset every frame)

    // The comets' names, kept rather than built in paint, and when they were last made up.
    std::array<juce::String, c_labels> m_busLabels;
    double m_busLabelsAt = -1.0;
    void refreshBusLabels();

    static constexpr float c_frame = 4.0f, c_gusset = 18.0f; // the frame's band, and the leg of its corner plates
    juce::Path m_ironFrame, m_frameInner; // the iron frame around the sea and its inner edge (built in resized)

    // Projection, refreshed in paint
    float m_focal = 1.0f, m_horizon = 0.0f, m_cx = 0.0f, m_zNear = 0.85f, m_zStep = 0.12f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SeaVisualiser)
};
