#pragma once

#include "../common/Biquad.h"
#include "../common/BoxAverage2.h"
#include "../common/DelayLine.h"
#include "../common/LogBallistics.h"
#include "../common/Module.h"
#include "../common/Oversampler2x.h"
#include "../common/SlidingExtreme.h"

#include <array>
#include <cstdint>

namespace roar
{
    struct OutputSettings
    {
        float pushDb = 3.0f;          // 0..+24 dB
        float ceilingDb = -1.0f;      // -12..0 dBTP
        float clipDb = 0.0f;          // 0..18 dB of each peak clipped instead of limited (0 = Off)
        float clipShape = 0.0f;       // 0 Soft .. 1 Hard
        float attackMs = 2.0f;        // 0.2..10 ms (inside the fixed 10 ms look-ahead)
        float releaseMs = 60.0f;      // 1..1000 ms; with Auto it sets how quickly Auto breathes
        bool autoRelease = true;      // program-dependent release with an adaptive hold
        float mix = 1.0f;             // 0..1
    };

    /**
     * OUTPUT (fixed): the end of the chain. Every sample, stereo-linked:
     *
     *   wet --> HPF 30 Hz (24 dB/oct) --> LPF 18 kHz (12 dB/oct) --> auto gain --> Ride --> Push
     *       --> mix with the dry --> MAXIMIZER --> CLIPPER --> SAFETY --> clamp at Ceiling --> out
     *   (the maximizer's reduction and the clipper's residual are each DC-blocked)
     *
     *   Auto gain   matches the wet's loudness (measured before Push, the 20-80 Hz band at -6 dB)
     *               to the raw input's (K-style, 400 ms); +6 / -12 dB, 400 ms glide, held while
     *               the input is below -50 dBFS. It never pushes on its own: a boost is limited to
     *               Ceiling + 3 dB - the wet's 1 s peak.
     *   Ride        RMS (10 ms) at -8 dBFS, 10:1, 6 dB knee, 3 ms / 150 ms, at most -12 dB.
     *   Push        0..+24 dB, relative to the ceiling: the gain is Push + Ceiling + 1 dB
     *               (laws::pushGainDb), so Ceiling only sets the output level, never the drive. 20 ms.
     *
     *   Maximizer   theta = Ceiling - 0.3 dB. Detector: the max over channels of the sample and an
     *               8x interpolated true peak (161-tap Kaiser beta 5, Dtp = 10), against theta + Clip,
     *               so the maximizer leaves up to Clip dB of each peak for the clipper.
     *               Fixed look-ahead L = round (10 ms fs); the user Attack A = round (Attack fs),
     *               1..L samples, is the gain ramp inside it: sliding minimum over A + 1 samples,
     *               then two box averages of total span A + 1, fed from the envelope delayed by
     *               L - A, so no sample is scaled by more than the gain it needs. A change of Attack
     *               primes a second gain computer from the envelope history and crossfades to it
     *               over 20 ms (a mix of two safe gains is safe); one change at a time, block rate.
     *               Release envelope in dB, instant attack:
     *                 manual   one pole at Release (1..1000 ms), no hold: short times let the gain
     *                          follow the bass waveform (crunch), long times are clean;
     *                 Auto     fast pole laws::autoFastReleaseMs (Release): 3 ms at 1 ms, 15 ms at
     *                          60 ms, log in between, Release / 4 above (at most 250 ms); an adaptive hold
     *                          (25..150 ms: 1.25 x the longest of the last 4 intervals under 150 ms
     *                          between peak events, a peak within 0.5 dB of the 200 ms maximum and
     *                          at least 3 ms after the last), and a 400 ms slow stage; e = max.
     *   Clipper     odd quadratic-knee curve bounded at cb = theta - 0.2 dB, C1, half-width
     *               w = min (0.5, cap) (1 - Shape)^2, cap = 1 - 10^(-Clip/20) (laws::clipKnee: it
     *               never bends material more than Clip dB under the bound, and Shape acts over
     *               its whole travel at any Clip). Only its departure from linear (the residual) is
     *               computed: 4x oversampled (Oversampler2x, then a 25-tap half-band 2x -> 4x stage),
     *               ADAA-1 on the residual in closed form, then a 2-tap average and 3 samples of delay
     *               at 4x so it sits on a whole 2x sample, back to 2x through the half-band, then a
     *               125-tap linear-phase decimator (Kaiser beta 9: flat to 0.417 fs, -90 dB from
     *               0.5 fs) instead of the half-band down, so nothing folds back below Nyquist at any
     *               rate. The linear part is the input delayed by the same 49 samples, so material
     *               below the knee passes bit-exactly. Clip 0 is a pure delay (crossing 0 fades the
     *               residual over 20 ms).
     *   DC blockers one-pole 2.5 Hz high-passes on the maximizer's reduction ((g - 1) x) and on
     *               the clipper's residual (the safety stage after them catches what the filtering
     *               moves): a gain that follows an asymmetric wave (a driven preset at
     *               a high Push, a short Release) leaves no DC offset, and material neither stage
     *               touches passes bit-exactly. No latency.
     *   Safety      the same look-ahead construction with Ls = round (0.5 ms fs), attack = Ls, a
     *               5 ms release, target theta; it only catches the clipper's band-limiting
     *               overshoot. Then a hard clamp at the ceiling, counted (getClampCount()).
     *
     * The dry for Mix (and the auto gain reference) is the raw input aligned with OUTPUT's input:
     * delayed by the chain latency before OUTPUT and phase-matched by the processor. The dry gets
     * the same Push gain as the wet (auto gain already matches the wet to it), and the two are
     * crossfaded at equal power (cos / sin of Mix x 90 degrees), so Mix changes the blend, not the
     * level. The mix happens before the maximizer, so the ceiling holds at any Mix.
     *
     * Latency L + Dtp + 49 + Ls + Dtp (573 at 48 kHz), constant for every setting. No allocation
     * after prepare(), NaN-safe (non-finite input is taken as silence, input held to +80 dBFS).
     */
    class OutputModule : public Module
    {
    public:
        static constexpr int c_truePeakDelay = 10;                // Dtp
        static constexpr int c_truePeakFactor = 8;
        static constexpr int c_truePeakTaps = 2 * c_truePeakDelay * c_truePeakFactor + 1;   // 161
        static constexpr double c_marginDb = 0.3;
        static constexpr double c_clipBelowDb = 0.2;              // clip bound under theta
        static constexpr int c_halfbandTaps = 25;                 // 2x -> 4x stage
        static constexpr int c_decimatorTaps = 125;               // the residual's 2x -> 1x band-limiting decimator

        // The residual's path in 2x samples: Oversampler2x up (its half of c_latency, 23 at 2x), the
        // half-band round trip (12), the ADAA alignment (2), the decimator (62), read on the odd phase.
        static constexpr int c_clipPath2x = Oversampler2x::c_latency + (c_halfbandTaps - 1) / 2 + 2 + (c_decimatorTaps - 1) / 2 - 1;
        static_assert (c_clipPath2x % 2 == 0, "the residual must land on a whole base sample");
        static constexpr int c_clipLatency = c_clipPath2x / 2;     // 49

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return m_latency; }
        bool makesStereo() const override { return false; }

        /** Without a dry: Mix is taken as 100 % and auto gain holds its gain. */
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;

        /** dryL / dryR: the raw input aligned with this module's input (dryR may be null: dryL is used). */
        void processWithDry (float* l, float* r, const float* dryL, const float* dryR, int n, bool stereo);

        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const OutputSettings& s);

        /** Test hooks (all on in the plug-in). */
        void setTestOptions (bool filters, bool autoGain, bool ride);

        // Readouts (the last chunk)
        float getReductionDb() const { return m_s.reductionDb; }        // maximizer, <= 0
        float getClipDb() const { return m_s.clipActivityDb; }          // peak dB flattened by the clipper, >= 0
        float getSafetyReductionDb() const { return m_s.safetyDb; }     // safety stage, <= 0
        float getAutoGainDb() const { return static_cast<float> (m_s.autoGainDb); }
        float getRideDb() const { return static_cast<float> (m_ride.getValue()); }
        float getPushDb() const { return static_cast<float> (m_s.pushDb); }
        std::uint64_t getClampCount() const { return m_s.clampCount; }
        int getAttackSamples() const { return m_gc[static_cast<size_t> (m_s.gcCurrent)].attack; }
        int getHoldSamples() const { return m_s.holdLength; }

        int getLookahead() const { return m_lookahead; }
        int getSafetyLookahead() const { return m_safetyLookahead; }
        double getLowPassHz() const { return m_lowPassHz; }
        static constexpr double c_highPassHz = 30.0;
        static constexpr double c_lowPassTargetHz = 18000.0;

    private:
        /** The 2x -> 4x half-band stage: 25 taps, the even phase a pure delay; 6 base samples round trip. */
        struct Halfband
        {
            static constexpr int c_odd = 12;                      // non-zero odd-offset taps

            std::array<float, 2 * c_odd> upHistory {};            // double buffered
            int upPos = 0;
            std::array<float, 2 * c_odd> downOdd {};              // u1 of the last 12 pairs
            int downPos = 0;
            std::array<float, 8> downEven {};                     // u0 of the last 8 pairs
            int evenPos = 0;

            void reset();
            void upsample (const std::array<float, c_odd>& taps, float x, float& y0, float& y1);
            float downsample (const std::array<float, c_odd>& taps, float u0, float u1);
        };

        /** Sliding minimum + two box averages (span A + 1): the look-ahead gain ramp. */
        struct GainRamp
        {
            SlidingExtreme minimum { SlidingExtreme::Kind::min };
            BoxAverage2 box;
            int attack = 1;

            void prepare (int maxAttack);
            void setAttack (int a);
            double process (float g) { return box.process (static_cast<double> (minimum.process (g))); }
            void copyStateFrom (const GainRamp& o);
        };

        struct TruePeakHistory
        {
            std::array<float, 4 * c_truePeakDelay> values {};     // last 2 Dtp inputs, double buffered
            int pos = 0;
        };

        struct Channel
        {
            Biquad hp1, hp2, lp;
            Biquad dcBlock;                          // 2.5 Hz one-pole on the clipper's residual
            Biquad gainDcBlock;                      // 2.5 Hz one-pole on the maximizer's reduction
            Biquad wetHp, wetLowShelf, wetShelf;     // wet loudness weighting
            Biquad dryHp, dryShelf;                  // raw input loudness weighting
            TruePeakHistory mainPeak, safetyPeak;
            DelayLine mainDelay;                     // L + Dtp
            DelayLine clipDelay;                     // the clipper's linear part: c_clipLatency
            Oversampler2x os;                        // up only: the residual comes down through the decimator
            Halfband hb;
            std::array<float, 2 * c_decimatorTaps> decHistory {};   // 2x residual, double buffered
            int decPos = 0;
            int decZeroRun = 0;                      // consecutive exact zeros in (skips the FIR)
            double clipPrev = 0.0;                   // last 4x input to the residual ADAA
            std::array<double, 4> residualRing {};   // last 4 residual ADAA outputs
            int residualPos = 0;
            DelayLine safetyDelay;                   // Ls + Dtp

            void copyStateFrom (const Channel& o);
        };

        float truePeakDetect (TruePeakHistory& h, float x) const;
        float clipResidual (Channel& c, float v, double bound, double w);
        double residual4x (Channel& c, double v, double invBound, double w);
        void resetClipper();
        void primeRamp (GainRamp& ramp, int attack);
        int attackSamples (double ms) const;
        void endFrame();
        void designTruePeak();
        void designHalfband();
        void designDecimator();
        float decimate (Channel& c, float u0, float u1) const;
        void fillCeilingRing();

        // Constants of prepare().
        double m_fs = 48000.0;
        int m_lookahead = 480;
        int m_safetyLookahead = 24;
        int m_latency = 573;
        double m_lowPassHz = 18000.0;
        double m_smoothCoeff = 0.0;
        double m_rideMsCoeff = 0.0;
        double m_peakRelease = 0.0;
        double m_meterCoeff = 0.0, m_glideCoeff = 0.0;    // per frame
        int m_meanFrames = 1;
        double m_slowCoeff = 0.0, m_safetyRelease = 0.0;
        int m_holdMin = 1200, m_holdMax = 7200, m_eventGap = 144;
        double m_rampStep = 0.0, m_clipStep = 0.0;
        std::array<std::array<float, 2 * c_truePeakDelay>, c_truePeakFactor - 1> m_tpPhases {};
        std::array<float, Halfband::c_odd> m_hbTaps {};
        std::array<float, c_decimatorTaps / 2 + 1> m_decTaps {};   // symmetric: h[0] .. h[centre]

        static constexpr int c_frame = 32;

        std::array<Channel, 2> m_ch;
        DelayLine m_ceilingRing;                          // ceiling (dB) per sample, for every stage
        DelayLine m_envelopeRing;                         // main gain before its ramp (2 L + 8)
        std::array<GainRamp, 2> m_gc;                     // main gain ramps: current and the next Attack
        SlidingExtreme m_holdPeak { SlidingExtreme::Kind::max };   // 200 ms max of the main q
        GainRamp m_safetyRamp;
        LogBallistics m_ride;

        struct State
        {
            double pushTarget = 3.0, pushDb = 3.0, pushGain = 1.4125375446227544;
            double ceilingTarget = -1.0, ceilingDb = -1.0;
            double mixTarget = 1.0, mix = 1.0;
            double mixGainsFor = -1.0, mixDry = 0.0, mixWet = 1.0;   // the crossfade gains for `mix`
            double clipTarget = 0.0, clipDb = 0.0;
            double shapeTarget = 0.0, shape = 0.0;
            double clipBlend = 0.0;                       // fades the residual in and out around Clip 0
            double attackTargetMs = 2.0;
            double releaseMs = 60.0;
            bool autoRelease = true;
            bool filters = true, autoGainOn = true, ride = true;
            bool snap = true;

            // Auto gain
            double wetSum = 0.0, drySum = 0.0;
            int framePhase = 0;
            double wetMeter = 0.0, dryMeter = 0.0;
            int learnedFrames = 0;
            double autoGainDb = 0.0;
            double gainPrev = 1.0, gainNext = 1.0;
            double wetPeak = 0.0;

            // Ride
            double rideMs = 0.0;

            // Main envelope
            double fastDb = 0.0, slowDb = 0.0;
            int holdLeft = 0;
            int holdLength = 0;                           // the adaptive hold (Auto)
            int sinceEvent = 1 << 30;
            std::array<int, 4> intervals {};
            int numIntervals = 0, nextInterval = 0;

            // Attack crossfade
            int gcCurrent = 0;
            bool rampFading = false;
            double rampFade = 0.0;

            // Safety envelope
            double safetyEnvDb = 0.0;

            float reductionDb = 0.0f;
            float clipActivityDb = 0.0f;
            float safetyDb = 0.0f;
            std::uint64_t clampCount = 0;
        };

        State m_s;
    };
}
