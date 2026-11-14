#pragma once

#include <memory>
#include <vector>
#include <cstdint>

namespace RubberBand { class RubberBandStretcher; }

namespace tonalrecall
{
    /**
     * Owns one realtime RubberBand R3 stretcher (time ratio 1) and hides its
     * start padding / start delay so that retrieved samples line up 1:1 with
     * the samples fed in.
     */
    class RubberBandDriver
    {
    public:
        /** quality 0 = standard window, 1 = short window. Allocates; message thread. */
        RubberBandDriver (double sampleRate, int numChannels, int chunkSize, int quality, bool formantPreserve);
        ~RubberBandDriver();

        int getQuality() const { return quality; }
        int getChunkSize() const { return chunk; }

        /** Upper bound on (samples fed - aligned samples available), used to size the output delay. */
        int getHeadroom() const { return headroom; }

        /**
         * How far (in input samples) behind the newest fed sample the pitch scale
         * set with a chunk actually takes effect. Measured with the test harness:
         * ~1.8x RubberBand's start delay, depending on window size.
         */
        int getRatioAlignment() const { return ratioAlignment; }

        /**
         * The stretcher's own delay at a pitch scale of 1, in samples. At a scale r its delay is
         * this divided by r, so output sample i lines up with input sample i only at r = 1: at
         * other scales it sits getBaseDelay() * (1/r - 1) samples later (earlier when negative).
         */
        int getBaseDelay() const { return baseDelay; }

        void reset();
        void setFormantPreserve (bool shouldPreserve);

        /**
         * Feeds exactly getChunkSize() samples per channel at the given pitch
         * scale, then appends whatever aligned output is ready to dest via the
         * sink callback: sink(const float* const* data, int numSamples).
         */
        template <typename Sink>
        void processChunk (const float* const* in, double pitchScale, Sink&& sink)
        {
            feed (in, pitchScale);
            int got;
            while ((got = pull()) > 0)
                sink ((const float* const*) outPtrs.data(), got);
        }

    private:
        void feed (const float* const* in, double pitchScale);
        int pull();   // returns aligned samples now in outBuf
        void prime();

        std::unique_ptr<RubberBand::RubberBandStretcher> rb;
        int numCh = 2, chunk = 256, quality = 0, headroom = 0, ratioAlignment = 0, baseDelay = 0;
        bool formant = false;
        int64_t toDiscard = 0;
        std::vector<std::vector<float>> outBuf;
        std::vector<float*> outPtrs;
        std::vector<std::vector<float>> zeros;
        std::vector<const float*> zeroPtrs;
    };
}
