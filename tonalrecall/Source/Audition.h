#pragma once

// Hearing an edit as it is made, through the host's own Preview output.
//
// An AudioSuite plug-in is only given audio to fill while Preview (or Render) runs. So the
// audition rides on Preview: while it plays, the editor can ask for the learned effect to be
// held at one moment, sustained at a pitch like a held key, or played on from a moment along the
// curve; the processor mixes that over the Preview stream, cross-fading in and out. With Preview
// stopped there is nothing to play into, and the editor says so.
//
// It is the real thing, not a sketch: the learned effect's own channels (stereo stays stereo),
// shifted by the same stretch engine Stamp renders with. Playing on is the effect as it will be
// stamped. A held moment has to be made up, since sound cannot stand still: it is that moment
// replayed in overlapping pieces, then shifted by the same engine.
//
// Process-wide, like the capture store: whichever instance Preview drives plays it.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace tonalrecall
{

class RubberBandDriver;

class AuditionBus
{
public:
    using Channels = std::vector<std::vector<float>>;

    static AuditionBus& instance();
    ~AuditionBus();

    // Editor side (message thread). setSource builds the stretcher when the effect changes.
    void setSource(std::shared_ptr<const Channels> channels, double sampleRate, bool preserveFormants);
    void setCurve(const std::vector<float>& semitones, double hopSeconds);
    void hold(double seconds, double semitones);   // sustain this moment, shifted by this much
    void playFrom(double seconds);                 // play on from here along the curve, to the end
    void stop();

    /** Seconds into the effect while sounding, negative when silent. */
    double positionSeconds() const noexcept { return m_shownSeconds.load(std::memory_order_relaxed); }
    /** True while a real-time stream (Preview) is asking for audio, so an audition can be heard. */
    bool previewRunning() const noexcept;

    // Audio side: called for every real-time block. Mixes the audition over what is in the
    // buffers, cross-fading with it; leaves them alone when nothing is sounding. No allocation;
    // it never waits (a busy lock means "not this block").
    void render(float* const* channels, int numChannels, int numSamples, double streamRate) noexcept;
    // Audio side, for a real-time block that is not rendered (Learn is listening): Preview is
    // running all the same, and the editor should know it.
    void notePreviewRunning() noexcept;

    void resetForTesting();

private:
    AuditionBus();

    struct Grain
    {
        int phase = 0;
        double read = 0.0;
        bool live = false;
    };

    void startIfSilent();
    void fillChunk() noexcept;
    double semitonesNow() const noexcept;

    static constexpr int kChunk = 512;

    std::mutex m_lock;   // the audio thread only ever tries it
    std::shared_ptr<const Channels> m_source;
    int m_sourceChannels = 0;
    std::int64_t m_length = 0;
    std::vector<float> m_curve;
    double m_rate = 0.0, m_hop = 0.005;
    bool m_formants = false;

    std::unique_ptr<RubberBandDriver> m_driver;
    std::vector<std::vector<float>> m_feed;      // one chunk of source per channel, into the stretcher
    std::vector<const float*> m_feedPtrs;
    std::vector<std::vector<float>> m_fifo;      // the stretcher's output, waiting to be played
    std::size_t m_fifoRead = 0, m_fifoCount = 0;

    // What is sounding. The source is a mix of two things that fade against each other, so a
    // change between them never steps: the effect played straight (playGain) and a held moment
    // (overlapping grains, which fade themselves).
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

    std::atomic<double> m_shownSeconds { -1.0 };
    std::atomic<std::int64_t> m_lastPulledNs { 0 };
};

} // namespace tonalrecall
