#include "RubberBandDriver.h"
#include <rubberband/RubberBandStretcher.h>
#include <algorithm>
#include <cmath>

namespace pf
{
    using RBS = RubberBand::RubberBandStretcher;

    RubberBandDriver::RubberBandDriver (double sampleRate, int numChannels, int chunkSize, int q, bool formantPreserve)
        : numCh (std::max (1, numChannels)), chunk (chunkSize), quality (q), formant (formantPreserve)
    {
        RBS::Options opts = RBS::OptionProcessRealTime
                          | RBS::OptionEngineFiner           // R3: best quality, smooth ratio changes
                          | RBS::OptionPitchHighConsistency  // continuous pitch changes without glitches
                          | RBS::OptionThreadingNever
                          | RBS::OptionChannelsTogether
                          | (quality == 1 ? RBS::OptionWindowShort : RBS::OptionWindowStandard)
                          | (formant ? RBS::OptionFormantPreserved : RBS::OptionFormantShifted);

        rb = std::make_unique<RBS> ((size_t) std::lround (sampleRate), (size_t) numCh, opts, 1.0, 1.0);
        rb->setMaxProcessSize ((size_t) chunk);
        rb->setPitchScale (1.0);

        const int maxOut = chunk * 8 + 8192;
        outBuf.assign ((size_t) numCh, std::vector<float> ((size_t) maxOut, 0.0f));
        outPtrs.resize ((size_t) numCh);
        zeros.assign ((size_t) numCh, std::vector<float> ((size_t) chunk, 0.0f));
        zeroPtrs.resize ((size_t) numCh);
        for (int c = 0; c < numCh; ++c)
            zeroPtrs[(size_t) c] = zeros[(size_t) c].data();

        // Worst-case gap between samples fed and aligned samples retrievable. R3 works
        // in hops of up to ~a window; pitch changes in HighConsistency mode also make
        // the output count wobble a little. Generous and independent of quality so the
        // plugin latency does not change when the quality setting does.
        headroom = (int) std::lround (6144.0 * sampleRate / 48000.0) + 2 * chunk;

        // Empirically (tests/FlattenTests.cpp, --align sweeps on sirens, voice and
        // vibrato) the pitch set with a chunk lands ~1.76x (short window) / ~1.88x
        // (standard window) of the start delay behind the newest input sample.
        ratioAlignment = (int) std::lround ((quality == 1 ? 1.76 : 1.88) * (double) rb->getStartDelay());
        prime();
    }

    RubberBandDriver::~RubberBandDriver() = default;

    void RubberBandDriver::prime()
    {
        // Pad the start with silence and discard the start delay, so that
        // aligned output sample i corresponds to input sample i.
        int pad = (int) rb->getPreferredStartPad();
        toDiscard = (int64_t) rb->getStartDelay();

        while (pad > 0)
        {
            const int n = std::min (pad, chunk);
            rb->process (zeroPtrs.data(), (size_t) n, false);
            pad -= n;
            // drain whatever appears (it is all part of the discarded start delay or silence)
            while (rb->available() > 0)
            {
                const int a = std::min (rb->available(), (int) outBuf[0].size());
                for (int c = 0; c < numCh; ++c)
                    outPtrs[(size_t) c] = outBuf[(size_t) c].data();
                const int got = (int) rb->retrieve (outPtrs.data(), (size_t) a);
                const int64_t drop = std::min<int64_t> (toDiscard, got);
                toDiscard -= drop;
                if (drop < got)
                    break;   // should not happen during padding
            }
        }
    }

    void RubberBandDriver::reset()
    {
        rb->reset();
        rb->setPitchScale (1.0);
        prime();
    }

    void RubberBandDriver::setFormantPreserve (bool shouldPreserve)
    {
        if (shouldPreserve == formant)
            return;
        formant = shouldPreserve;
        rb->setFormantOption (formant ? RBS::OptionFormantPreserved : RBS::OptionFormantShifted);
    }

    void RubberBandDriver::feed (const float* const* in, double pitchScale)
    {
        rb->setPitchScale (std::clamp (pitchScale, 0.25, 4.0));
        rb->process (in, (size_t) chunk, false);
    }

    int RubberBandDriver::pull()
    {
        for (;;)
        {
            const int a = rb->available();
            if (a <= 0)
                return 0;

            const int n = std::min (a, (int) outBuf[0].size());
            for (int c = 0; c < numCh; ++c)
                outPtrs[(size_t) c] = outBuf[(size_t) c].data();
            const int got = (int) rb->retrieve (outPtrs.data(), (size_t) n);
            if (got <= 0)
                return 0;

            const int drop = (int) std::min<int64_t> (toDiscard, got);
            toDiscard -= drop;
            if (drop == got)
                continue;

            for (int c = 0; c < numCh; ++c)
                outPtrs[(size_t) c] = outBuf[(size_t) c].data() + drop;
            return got - drop;
        }
    }
}
