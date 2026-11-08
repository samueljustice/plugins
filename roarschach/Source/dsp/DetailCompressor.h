#pragma once

#include "VoiceInfo.h"

namespace roar
{
    /**
     * DETAIL (movable): a dual-threshold upward + downward level compressor that "ekes out" the
     * detail of a performance: quiet breaths, mouth noises and word tails come up, loud peaks
     * come down, so the creature processing has an even, detailed signal to work on.
     *
     *   LOW  (upward):   signals below -30 dBFS are lifted towards it at 2.5:1,
     *                    boost = min(LOW x 24 dB, (-30 - level) x 0.6).
     *                    Detector: RMS-ish (4 ms window), 1 ms attack, program-dependent release (40 ms after
     *                    short sounds, up to 250 ms after sustained ones: a fast and a slow
     *                    detector run in parallel and the higher one wins).
     *                    Noise-floor guard: below -65 dBFS the boost fades back to 0 (gone at
     *                    -77 dBFS), so silence and room tone are never raised (the make-up below
     *                    fades out the same way).
     *   HIGH (downward): threshold 0 dBFS (0 %) .. -24 dBFS (100 %), ratio 4:1 .. 10:1, 6 dB soft
     *                    knee, 3 ms attack / 120 ms release, make-up = half the threshold change.
     *
     * LOW 0 and HIGH 0 is unity gain. Stereo-linked (one gain for both channels), zero latency,
     * no allocation, NaN-safe.
     */
    class DetailCompressor
    {
    public:
        void prepare (double sampleRate, int maxBlockSize);
        void reset();
        int getLatencySamples() const { return 0; }

        // low01, high01: 0..1, smoothed internally (30 ms).
        void setParams (float low01, float high01);

        // Mono when !stereo (right ignored), else stereo-linked.
        void process (float* left, float* right, int numSamples, bool stereo, const VoiceInfo& voice);

        // For the visualiser, updated per call: current upward boost and downward cut, both in dB
        // and both >= 0 (the cut is the reduction before make-up). Values at the end of the call.
        float getBoostDb() const { return m_boostDb; }
        float getCutDb() const { return m_cutDb; }

        // Total gain applied at the end of the last call (boost - cut + make-up), dB.
        float getGainDb() const { return m_gainDb; }

    private:
        double m_sampleRate = 48000.0;

        float m_lowTarget = 0.0f, m_highTarget = 0.0f;
        float m_low = 0.0f, m_high = 0.0f;
        float m_paramCoeff = 0.0f;
        bool m_snap = true;

        // Upward detector (mean square): short RMS window, then fast and slow followers in parallel
        float m_rmsMs = 0.0f, m_rmsCoeff = 0.0f;
        float m_fastMs = 0.0f, m_slowMs = 0.0f;
        float m_upAttack = 0.0f, m_fastRelease = 0.0f, m_slowAttack = 0.0f, m_slowRelease = 0.0f;

        // Downward detector (mean square)
        float m_downMs = 0.0f;
        float m_downAttack = 0.0f, m_downRelease = 0.0f;

        // Final gain smoothing (keeps the 1 ms detector from zippering)
        float m_gainSmoothedDb = 0.0f;
        float m_gainCoeff = 0.0f;

        float m_boostDb = 0.0f, m_cutDb = 0.0f, m_gainDb = 0.0f;
    };
}
