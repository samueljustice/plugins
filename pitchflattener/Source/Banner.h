#pragma once

#include <JuceHeader.h>

#include "Engine.h"
#include "Hud.h"

#include <cstdint>

/**
 * The big flashes over the display: what just happened or what to do, in huge sheared type on a
 * slanted band, so nobody misses that Learn comes first or that the sound was flattened.
 *
 * BannerLogic decides which banner shows from the engine's state as the editor reads it (pure, no
 * JUCE component, so the tests can drive it with times of their choosing). Banner draws it.
 *
 * Rules:
 *  - Persistent banners follow the state: LEARN FIRST (idle), PLAY THE SOUND (armed), LISTENING,
 *    ANALYSING, WRONG SAMPLE RATE (learned at another rate).
 *  - One-shots mark a change: LEARNED then PLAY IT AGAIN (a new sound flattened), FLATTENED (the
 *    flattened sound starts playing), NO STEADY PITCH (a new sound with nothing to flatten),
 *    CLEARED. Each fires once per change: a state that stays the same, a new render of the same
 *    sound (another line or target) and a play position that blinks for a block never re-fire.
 */
class BannerLogic
{
public:
    enum class Kind
    {
        None,
        LearnFirst,
        PlayTheSound,
        Listening,
        Analysing,
        Learned,
        PlayItAgain,
        Flattened,
        NoSteadyPitch,
        WrongSampleRate,
        Cleared
    };

    struct Input
    {
        pitchflattener::Engine::State state = pitchflattener::Engine::State::Idle;
        bool playing = false;              // the engine is playing a render of the learned sound
        bool rateMismatch = false;
        std::uint64_t learnedId = 0;       // the learned sound (0 = none); new renders of it keep the id
    };

    // How long each one-shot holds, in seconds.
    static constexpr double kLearnedSeconds = 1.5;
    static constexpr double kPlayItAgainSeconds = 2.0;
    static constexpr double kFlattenedSeconds = 1.0;
    static constexpr double kNoSteadySeconds = 3.0;
    static constexpr double kClearedSeconds = 0.8;
    // Playback that stopped for less than this and starts again is the same playback (a re-render
    // swaps the voice for a block or two; that is not a new start).
    static constexpr double kPlayGapSeconds = 0.4;

    /** Reads the state at time `now` (seconds on the editor's clock). */
    void update (const Input&, double now);

    Kind kind() const noexcept { return m_kind; }
    /** When the banner showing now appeared. */
    double since() const noexcept { return m_since; }
    /** The small FLAT tag at the display's corner: the flattened sound plays and its banner has gone. */
    bool flatTag() const noexcept { return m_flatTag; }
    /** Banners raised so far (each change of kind to something shown counts once). */
    int raised() const noexcept { return m_raised; }

    static juce::String textFor (Kind);
    static juce::Colour colourFor (Kind);

private:
    Input m_last;
    bool m_started = false;
    Kind m_kind = Kind::None;
    double m_since = 0.0;
    int m_raised = 0;
    bool m_flatTag = false;

    Kind m_oneShot = Kind::None;          // Learned stands for the LEARNED -> PLAY IT AGAIN chain
    double m_oneShotStart = 0.0;
    std::uint64_t m_announcedId = 0;      // the learned sound already announced
    double m_lastPlaying = -1.0e9;        // the last time playback was seen
};

/**
 * Draws BannerLogic's banner across the display: wipes in from the left (180 ms), blinks at 2 Hz
 * for its first second (LEARN FIRST: for one second in every four), then stays lit; wipes out when
 * it goes. Under LEARN FIRST, chevrons point up at the Learn button. Never takes a click:
 * everything under it keeps working. (The small FLAT tag is the editor's, in the display's header.)
 */
class Banner final : public juce::Component
{
public:
    explicit Banner (const Hud::Clock&);

    /** The state to show (the editor calls this on every refresh). */
    void show (BannerLogic::Kind, double since, double captureSeconds);
    /** Where the Learn button's centre is, in this component's x, for LEARN FIRST's chevrons. */
    void setLearnButtonX (float x) { m_learnX = x; }
    /** Whether anything moves now (the editor repaints only then). */
    bool isAnimating() const noexcept;
    void tick();   // repaints when something moves

    void paint (juce::Graphics&) override;

    static constexpr double kWipeSeconds = 0.18;

private:
    void drawBand (juce::Graphics&, BannerLogic::Kind, double age, float wipeIn, float wipeOut) const;

    const Hud::Clock& m_clock;
    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    BannerLogic::Kind m_kind = BannerLogic::Kind::None, m_outgoing = BannerLogic::Kind::None;
    double m_since = 0.0, m_outSince = -1.0e9;
    double m_captureSeconds = 0.0;
    float m_learnX = 0.0f;
};
