#pragma once

#include <JuceHeader.h>

/**
 * The three Backwards Machine effects.
 *
 * Every effect is built from the same voice: a delay line whose delay is swept
 * by a sawtooth from 0 to 2T over each period T (so the read head runs
 * backwards through the input at normal speed), faded in and out by a
 * 0.5 + 0.5*sin window that is silent exactly when the sweep jumps back.
 *
 *  - Reverse Playback:  two voices half a period apart; their windows sum to 1.
 *  - Forward Backwards: one voice cross-faded against the undelayed input on
 *                       the complementary window.
 *  - Reverse Repeat:    one voice, plus a copy of it delayed by T/2 with a 5 Hz
 *                       delay vibrato, so each snippet plays twice.
 *
 * Feedback sends each voice's windowed output back into its own delay line.
 * The input to every delay line is halved and the output doubled, as in the
 * original, so FEEDBACK DEPTH F is the true loop gain.
 *
 * Up to 50 kHz the oscillators accumulate their phase in float32 exactly as the
 * original does, so the grain timing is sample-compatible with it: the period
 * comes out up to about 0.25 % longer or shorter than T, and parts of each
 * snippet play a few cents sharp or flat. Float32 steps get coarser relative to
 * the phase increment as the sample rate rises (half a semitone out at 96 kHz,
 * a semitone at 192 kHz), so above 50 kHz the clock and the delay arithmetic run
 * in double precision instead, and pitch and period are exact. Either way the
 * window gain and the vibrato arrive one sample late, as in the original.
 *
 * reset() is O(1) in the buffer sizes, so it is safe on the audio thread.
 *
 * Output is pre-wet: the caller applies the wet gain.
 */
class ReverseEngine
{
public:
    enum class Mode { ReversePlayback = 0, ForwardBackwards = 1, ReverseRepeat = 2 };

    static constexpr float minTimeSeconds = 0.5f;
    static constexpr float maxTimeSeconds = 4.0f;
    static constexpr float maxFeedback = 0.75f;
    static constexpr int maxVoices = 2;

    struct VoiceView
    {
        float delaySeconds = 0.0f;   // how far back in the input this voice is playing
        float gain = 0.0f;           // its window gain, 0..1
    };

    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    /** Mode changes cross-fade from the old effect to the new one. */
    void setMode(Mode mode, bool immediate = false);
    /** TIME changes cross-fade from the old read heads to the new ones. */
    void setTime(float seconds, bool immediate = false);
    /** Feedback depth 0..0.75 (smoothed). */
    void setFeedback(float amount, bool immediate = false);

    void process(const float* const* input, float* const* output, int numChannels, int numSamples);

    std::array<VoiceView, maxVoices> getVoiceViews(int& count) const;
    static double tailSeconds(float timeSeconds, float feedback);

private:
    class DelayLine
    {
    public:
        void allocate(int minimumLength);
        /** O(1): rather than zeroing the buffer, reads simply treat anything older
            than the last clear as silence. */
        void clear();
        void push(float x)
        {
            m_write = (m_write + 1) & m_mask;
            m_data[static_cast<size_t>(m_write)] = x;
            m_filled = std::min(m_filled + 1, m_mask + 1);
        }
        float read(double delay) const;   // delay in samples behind the newest sample

    private:
        std::vector<float> m_data;
        int m_mask = 0;
        int m_write = 0;
        int m_filled = 0;                  // samples written since the last clear
    };

    struct Channel
    {
        DelayLine history;                 // 0.5 * input, shared by every voice
        std::array<DelayLine, 2> feedback; // each voice's own feedback (F * y)
        DelayLine repeat;                  // voice 0's output, for Reverse Repeat's second pass
        std::array<float, 2> lastVoice {}; // y[n-1] per voice, for the feedback send
    };

    /** The sweep/window oscillators of both voices, running at 1/T Hz. */
    struct Clock
    {
        // 0: voice 0 sweep, 1: voice 0 window, 2: voice 1 sweep, 3: voice 1 window
        // (also the forward window in Forward Backwards). Radians. Unless `exact`,
        // every value is a float32 (held exactly in the double) and the arithmetic
        // is float32, as in the original.
        std::array<double, 4> phase {};
        std::array<float, 2> windowGain { 0.5f, 0.5f };   // last sample's window values
        double increment = 0.0;
        float time = 2.0f;
        bool exact = false;                                // double-precision clock

        void start(float timeSeconds, float radiansPerSampleHz, bool exactClock, double sampleRate);
        void advance();
        void resync();
        std::array<float, 2> windows() const;              // this sample's window values
    };

    struct Taps
    {
        std::array<double, 2> delay {};    // per voice, samples
        std::array<float, 2> window {};    // per voice, 0..1
    };

    Taps tapsOf(const Clock& clock) const;

    double m_sampleRate = 44100.0;
    float m_radiansPerSampleHz = 0.0f;     // f32(f32(1/SR) * 2pi), as in the original
    bool m_exactClock = false;             // above 50 kHz: double-precision clock and delays
    int m_numChannels = 0;
    std::vector<Channel> m_channels;

    // TIME: the clock in use, the requested time, and a cross-fade from heads still
    // running on the previous clock.
    Clock m_clock, m_oldClock;
    float m_targetTime = 2.0f;
    int m_crossfadeLength = 1;
    int m_crossfadePos = 1;                // >= length: no cross-fade running

    // Reverse Repeat's 5 Hz delay vibrato (its value arrives one sample late).
    float m_vibratoPhase = 0.0f;
    float m_vibratoIncrement = 0.0f;
    float m_vibratoValue = 0.0f;

    // Mode: the effect in use, the requested one, and a cross-fade from the previous one.
    Mode m_mode = Mode::ReversePlayback;
    Mode m_targetMode = Mode::ReversePlayback;
    Mode m_previousMode = Mode::ReversePlayback;
    int m_modeFadeLength = 1;
    int m_modeFadePos = 1;                 // >= length: no cross-fade running

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_feedback;

    std::array<VoiceView, maxVoices> m_views {};
};
