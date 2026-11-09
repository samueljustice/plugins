#pragma once

#include <cstdint>
#include <vector>

namespace roar
{
    /**
     * Pitch information that travels down the chain with the audio (spec §3.6).
     *
     * The stream is a sequence of control frames on a fixed grid of c_voiceFrameSize samples,
     * counted from the last reset (the processor's sample clock). Every module processes the same
     * chunk, so every view of a chunk has the same geometry: frame 0 is the frame that holds the
     * chunk's first sample, startOffset() samples into it.
     *
     *   IN       VoiceStream     owns one chunk of frames; IN's detector writes them
     *   modules  VoiceFrameView  the read-only stream a module gets in process()
     *   chain    VoicePass       per module: delays the stream by the module's latency (a ring of
     *                            frames) and lets PITCH rewrite f0 (VoiceRewriter), giving the
     *                            stream the next module sees
     *
     * A frame holds { f0Hz, confidence, voiced }. f0Hz is 0 until a pitch has been found and is
     * held through unvoiced passages (voiced = false) after that.
     */
    inline constexpr int c_voiceFrameSize = 32;
    inline constexpr int c_voiceFrameShift = 5;

    struct VoiceFrame
    {
        float f0Hz = 0.0f;
        float confidence = 0.0f;   // 0..1
        bool voiced = false;
    };

    /** Read-only frames for one chunk. Cheap to copy; valid until the owner's next chunk. */
    class VoiceFrameView
    {
    public:
        /** An empty view: one default frame (no pitch, unvoiced). */
        VoiceFrameView();

        VoiceFrameView (const VoiceFrame* frames, int numFrames, int startOffset, std::int64_t firstFrame, int skewSamples);

        int numFrames() const { return m_numFrames; }

        /** Frame k of the chunk (clamped to the frames present). */
        const VoiceFrame& frame (int k) const
        {
            k = k < 0 ? 0 : (k >= m_numFrames ? m_numFrames - 1 : k);
            return m_frames[k];
        }

        /** Index (into this view) of the frame holding chunk sample i. */
        int frameIndexForSample (int sample) const { return (m_startOffset + sample) >> c_voiceFrameShift; }

        /** The frame holding chunk sample i. */
        const VoiceFrame& atSample (int sample) const { return frame (frameIndexForSample (sample)); }

        /** The last frame of the chunk (the most recent pitch). */
        const VoiceFrame& last() const { return m_frames[m_numFrames - 1]; }

        /**
         * One past the last chunk sample that shares sample i's frame, capped at numSamples. For
         * per-frame loops:
         *   for (int i = 0; i < n;) { const int end = voice.frameEnd (i, n); const auto& f = voice.atSample (i);
         *                             ... samples i..end-1 ...; i = end; }
         */
        int frameEnd (int sample, int numSamples) const
        {
            const int next = ((frameIndexForSample (sample) + 1) << c_voiceFrameShift) - m_startOffset;
            return next < numSamples ? next : numSamples;
        }

        int startOffset() const { return m_startOffset; }
        std::int64_t firstFrameIndex() const { return m_firstFrame; }

        /** How many samples this stream lags behind its audio (|skew| <= 16 through a VoicePass chain). */
        int skewSamples() const { return m_skew; }

        const VoiceFrame* data() const { return m_frames; }

    private:
        const VoiceFrame* m_frames;
        int m_numFrames = 1;
        int m_startOffset = 0;
        std::int64_t m_firstFrame = 0;
        int m_skew = 0;
    };

    /** Frames for one chunk, owned. The IN stage writes one of these per chunk. */
    class VoiceStream
    {
    public:
        /** Frames a chunk of maxBlock samples can touch. */
        static int maxFramesFor (int maxBlock) { return (maxBlock + c_voiceFrameSize - 1) / c_voiceFrameSize + 1; }

        void prepare (int maxBlock);

        /** Sample clock back to 0 and the held frame cleared. */
        void reset();

        /**
         * Producer: open the next chunk of numSamples at the running sample clock. Every frame of
         * the chunk starts as a copy of the last frame written (the held pitch); write the ones
         * that change with frame(k), or fill().
         */
        void beginChunk (int numSamples);

        /** Same geometry as another view (the chunk it describes), frames copied from it. */
        void assign (const VoiceFrameView& source);

        int numFrames() const { return m_numFrames; }
        VoiceFrame& frame (int k) { return m_frames[static_cast<size_t> (k < 0 ? 0 : (k >= m_numFrames ? m_numFrames - 1 : k))]; }
        VoiceFrame& frameForSample (int sample) { return frame ((m_startOffset + sample) >> c_voiceFrameShift); }
        VoiceFrame* frames() { return m_frames.data(); }

        void fill (const VoiceFrame& f);

        VoiceFrameView view() const;

    private:
        std::vector<VoiceFrame> m_frames;
        int m_numFrames = 1;
        int m_startOffset = 0;
        std::int64_t m_firstFrame = 0;
        std::int64_t m_clock = 0;
        int m_skew = 0;
    };

    /**
     * Delays a stream by a fixed number of audio samples, in whole frames, from a ring of frames
     * keyed by the global frame index. The frame delay D = floor ((L - skew_in + 16) / 32) is
     * chosen per chunk from the incoming skew, so the rounding never accumulates down the chain
     * (|skew_out| <= 16 samples). Frames from before the last reset read as the default frame.
     * No allocation after prepare(); copyStateFrom() copies the ring (same latency and size).
     */
    class VoiceDelay
    {
    public:
        void prepare (int latencySamples, int maxBlock);
        void reset();
        int getLatencySamples() const { return m_latency; }

        /** Store the chunk's frames and return them delayed (a view into this object). */
        VoiceFrameView process (const VoiceFrameView& in);

        void copyStateFrom (const VoiceDelay& other);

    private:
        std::vector<VoiceFrame> m_ring;
        std::int64_t m_mask = 0;
        std::int64_t m_oldestValid = 0;
        std::int64_t m_newest = -1;
        bool m_any = false;
        int m_latency = 0;
        int m_maxFrameDelay = 0;
        std::vector<VoiceFrame> m_out;
    };

    /**
     * The PITCH-rewrite hook. A module that changes the pitch of its audio implements this; the
     * chain's VoicePass calls it on the stream after the latency delay, so frames[k] describes
     * the module's output for chunk frame k (the same frame grid as the chunk it just processed).
     * PITCH Shift multiplies f0 by 2^(P/12); Snap and Mono write the target pitch.
     */
    class VoiceRewriter
    {
    public:
        virtual ~VoiceRewriter() = default;
        virtual void rewriteVoice (VoiceFrame* frames, int numFrames) = 0;
    };

    /**
     * What the chain keeps per module: the delay ring for the module's latency plus scratch for a
     * rewrite. process() returns the stream downstream of the module:
     *   latency 0 and no rewriter  ->  the input view itself (no copy)
     *   latency L                  ->  delayed by L
     *   rewriter                   ->  copied, then rewriter->rewriteVoice (frames, numFrames)
     */
    class VoicePass
    {
    public:
        void prepare (int latencySamples, int maxBlock);
        void reset();

        VoiceFrameView process (const VoiceFrameView& in, VoiceRewriter* rewriter);

        void copyStateFrom (const VoicePass& other);

    private:
        VoiceDelay m_delay;
        VoiceStream m_scratch;
    };
}
