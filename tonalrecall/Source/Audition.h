#pragma once

// Hearing what was learned, and an edit as it is made, through the host's own Preview output.
//
// An AudioSuite plug-in is only given audio to fill while Preview (or Render) runs. So everything
// heard from the editor rides on Preview: while it plays, the processor mixes what the editor
// asked for over the Preview stream, cross-fading in and out. With Preview stopped there is
// nothing to play into: a request waits, and starts the moment Preview does.
//
// What can be played:
//   - the music as it was learned, and the effect as it was learned: straight playback of the
//     learned channels, sample for sample, from the start to the end;
//   - the effect as it will be stamped: the learned effect's own channels shifted along the curve
//     by the same stretch engine Stamp renders with, at the stamp's trim. It follows the curve as
//     it changes. A click or drag on the effect's scan plays this too: on from a moment along
//     the curve, or a moment held at a pitch like a held key. A held moment has to be made up,
//     since sound cannot stand still: it is that moment replayed in overlapping pieces, then
//     shifted by the same engine.
// One thing sounds at a time: starting one cross-fades from whatever was sounding.
//
// Stereo stays stereo; a mono clip goes to both sides, and a stereo one into a mono stream is
// summed at half.
//
// Process-wide, like the capture store: whichever instance Preview drives plays it.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace tonalrecall
{

class RubberBandDriver;

// What is playing. Output is the effect as it will be stamped (the play button and the scan's
// auditions alike).
enum class Playback : int { None = 0, Music = 1, Input = 2, Output = 3 };

class AuditionBus
{
public:
    using Channels = std::vector<std::vector<float>>;

    // Where in a clip the sound being heard is.
    struct Playhead
    {
        Playback what = Playback::None;
        double seconds = -1.0;          // into the clip; negative when nothing is heard
        double lengthSeconds = 0.0;     // the clip's length
        bool moving = false;            // advancing in real time (false for a held moment)
        std::int64_t atNs = 0;          // when it was read (steady clock), to move it on between blocks
    };

    static AuditionBus& instance();
    ~AuditionBus();

    // ---- editor side (message thread) ----
    // The effect: setSource builds the stretcher when it changes.
    void setSource(std::shared_ptr<const Channels> channels, double sampleRate, bool preserveFormants);
    void setMusic(std::shared_ptr<const Channels> channels, double sampleRate);
    void setCurve(const std::vector<float>& semitones, double hopSeconds);

    // Plays one thing from its start to its end, then stops. Starting one stops the others.
    void play(Playback what);
    void hold(double seconds, double semitones);   // sustain this moment, shifted by this much
    void playFrom(double seconds);                 // play on from here along the curve, to the end
    void stop();                                   // fades out whatever is sounding
    // At once, and nothing is left waiting: for Learn, which must hear the stream untouched.
    void stopNow();

    /** What is sounding, or asked for and waiting for Preview; None once it has finished. */
    Playback playing() const noexcept { return static_cast<Playback>(m_role.load(std::memory_order_acquire)); }
    /** True when what is playing was started by play() (a play button), not by the scan. */
    bool startedByPlay() const noexcept { return m_byPlay.load(std::memory_order_acquire); }
    Playhead playhead() const noexcept;
    /** Seconds into the clip being heard, negative when silent. */
    double positionSeconds() const noexcept { return playhead().seconds; }
    /** True while a real-time stream (Preview) is asking for audio, so something can be heard. */
    bool previewRunning() const noexcept;

    /** Where Preview itself is in the learned effect while it plays it stamped (what is none). */
    Playhead stampPlayhead() const noexcept;

    // ---- audio side ----
    // Called for every real-time block. Mixes what is playing over what is in the buffers,
    // cross-fading with it; leaves them alone when nothing is sounding. No allocation; it never
    // waits (a busy lock means "not this block"). trim is the gain the stamp is turned down by
    // (see MatchHub::trimFor), so the effect as stamped is heard at the level it will have.
    void render(float* const* channels, int numChannels, int numSamples, double streamRate, float trim = 1.0f) noexcept;
    // For a real-time block that is not rendered (Learn is listening): Preview is running all the
    // same, and the editor should know it. Whatever was playing is dropped, not resumed after.
    void notePreviewRunning() noexcept;
    // The processor's word on where in the learned effect its stamped Preview is heard (negative
    // seconds: it is not playing the effect).
    void publishStampPosition(double seconds, double lengthSeconds) noexcept;

    void resetForTesting();

private:
    AuditionBus();

    struct Grain
    {
        int phase = 0;
        double read = 0.0;
        bool live = false;
    };

    // Straight playback of learned channels. Two, so that one can fade out as the next fades in.
    struct Voice
    {
        Playback what = Playback::None;
        const Channels* source = nullptr;   // kept alive by m_music or m_source
        int channels = 0;
        std::int64_t length = 0, position = 0, fade = 256;
        double rate = 0.0;
        float gain = 0.0f, step = 0.0f;     // the level, and how it moves each sample
        bool live = false;
    };

    // Where each chunk fed to the stretcher came from, so what is heard can be placed in the effect.
    struct Mark
    {
        double from = 0.0, to = 0.0;        // source samples at the chunk's first and last sample
    };

    void startIfSilent();
    void fillChunk() noexcept;
    double semitonesNow() const noexcept;
    double heardSourcePosition() const noexcept;
    void startVoice(Playback what);
    void fadeOutVoices() noexcept;
    void killVoices(Playback what) noexcept;   // None: all
    void setRole(Playback what, bool byPlay) noexcept;

    static constexpr int kChunk = 512;
    static constexpr int kMarks = 128;      // chunks remembered: more than the stretcher and the fifo hold

    std::mutex m_lock;   // the audio thread only ever tries it
    std::shared_ptr<const Channels> m_source;
    int m_sourceChannels = 0;
    std::int64_t m_length = 0;
    std::vector<float> m_curve;
    double m_rate = 0.0, m_hop = 0.005;
    bool m_formants = false;

    std::shared_ptr<const Channels> m_music;
    double m_musicRate = 0.0;
    std::array<Voice, 2> m_voices;
    bool m_yieldToStretch = false;   // the voices give way once the stretcher sounds

    std::unique_ptr<RubberBandDriver> m_driver;
    std::vector<std::vector<float>> m_feed;      // one chunk of source per channel, into the stretcher
    std::vector<const float*> m_feedPtrs;
    std::vector<std::vector<float>> m_fifo;      // the stretcher's output, waiting to be played
    std::size_t m_fifoRead = 0, m_fifoCount = 0;
    std::int64_t m_fedCount = 0, m_playedCount = 0;   // stretcher samples in and out since it was reset
    std::array<Mark, kMarks> m_marks {};
    double m_lastScale = 1.0;
    float m_trimNow = 1.0f;
    bool m_trimSet = false;

    // What the stretcher is playing. The source is a mix of two things that fade against each
    // other, so a change between them never steps: the effect played straight (playGain) and a
    // held moment (overlapping grains, which fade themselves).
    bool m_active = false;        // anything at all
    bool m_holding = false;       // new grains are being started
    bool m_started = false;       // the stretcher has delivered its first sound
    bool m_priming = false;
    double m_position = 0.0;      // source samples: the play position, or the held moment
    double m_pendingPosition = -1.0;
    double m_holdSemitones = 0.0;
    float m_playGain = 0.0f, m_playTarget = 0.0f;
    float m_gain = 0.0f, m_fadeTarget = 0.0f;
    Grain m_grains[2];
    std::uint32_t m_random = 12345u;

    std::atomic<int> m_role { 0 };
    std::atomic<bool> m_byPlay { false };
    std::atomic<int> m_shownWhat { 0 };
    std::atomic<double> m_shownSeconds { -1.0 }, m_shownLength { 0.0 };
    std::atomic<bool> m_shownMoving { false };
    std::atomic<std::int64_t> m_shownNs { 0 };
    std::atomic<double> m_stampSeconds { -1.0 }, m_stampLength { 0.0 };
    std::atomic<std::int64_t> m_stampNs { 0 };
    std::atomic<std::int64_t> m_lastPulledNs { 0 };
    std::atomic<bool> m_abandon { false };   // Learn listened: drop everything at the next block
};

} // namespace tonalrecall
