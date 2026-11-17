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
        bool levelMatch = true;       // Level Match
    };

    /**
     * OUTPUT (fixed): the end of the chain. Every sample, stereo-linked:
     *
     *   wet --> HPF 30 Hz (24 dB/oct) --> LPF 18 kHz (12 dB/oct) --> x comp --> x match --> Ride
     *       --> Push --> mix with the dry --> MAXIMIZER --> CLIPPER --> SAFETY --> clamp at Ceiling --> out
     *   (the maximizer's reduction and the clipper's residual are each DC-blocked)
     *
     *   Comp        1 / IN's total gain, sample-aligned (IN's gain trace delayed by the chain's
     *               module latency): whatever working level IN chose, the wet comes back at the
     *               input's level. Always on while a dry is present.
     *   Level Match one held gain M (-12..+15 dB) that corrects what the modules change at the
     *               working level. It learns on matched frames only: IN learnt and on its target,
     *               confident sound within L - 10 .. L + 3 dB of the take's loud end (IN's flag
     *               trace), and the wet no more than 24 dB under the dry (20 ms levels, K-style,
     *               the wet's 20-80 Hz band at -6 dB). M is the ratio of the cumulative power
     *               means of dry and wet: held for 0.15 s of matched sound, then moving at most
     *               24 dB/s to 0.5 s, 6 dB/s to 2 s and 0.5 dB/s to 10 s, after which it is a
     *               constant. A settings change starts a probe that replaces the match only when
     *               new evidence disagrees by 4 dB after 0.5 s, 3 dB after 1 s or 2 dB after 2 s
     *               of matched sound (with nothing heard since a hint or a clear, a change
     *               only takes the hint's weight away); an IN gain move of more than 2 dB
     *               re-opens the slow phase. A preset's hint sets M and counts as 2 s of matched
     *               sound, fading to nothing at the stop; while it is in force M moves at most
     *               6 dB/s, and nothing is learnt for the modules' latency plus 100 ms after it.
     *               Level Match Off glides the match to 0 dB over 20 ms (learning continues);
     *               switching it On again learns afresh. Measured before Ride, Push and the
     *               maximizer, so it cannot pump.
     *   Ride       RMS (10 ms) at -8 dBFS, 10:1, 6 dB knee, 3 ms / 150 ms, at most -12 dB.
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
     * the same Push gain as the wet (the match already matches the wet to it), and the two are
     * crossfaded with cos / sin of Mix x 90 degrees times k = sqrt (Pt / (Pt + 2 c s X)), where
     * Pt = c^2 Pd + s^2 Pw and X is the dry / wet cross power held to 0 .. sqrt (Pd Pw) (powers
     * over 400 ms): the mix has the power interpolation of the two, whether they are related (a
     * wet close to the dry) or not, so Mix changes the blend, not the level. k = 1 at 0 and 100 %.
     * The mix happens before the maximizer, so the ceiling holds at any Mix.
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

        /** Without a dry: Mix is taken as 100 %, comp is 1 and the match holds (nothing learns). */
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;

        /**
         * dryL / dryR: the raw input aligned with this module's input (dryR may be null: dryL is
         * used). gainTrace / activeTrace: IN's total gain and matched flag per sample, delayed to
         * line up with this module's input (null: gain 1, never matched).
         */
        void processWithDry (float* l, float* r, const float* dryL, const float* dryR,
                             const float* gainTrace, const float* activeTrace, int n, bool stereo);

        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;
        void publishFeatures (MonsterFeatures& features) const override;

        void setParams (const OutputSettings& s);

        /** Test hooks (all on in the plug-in). autoGain false switches comp and the match off. */
        void setTestOptions (bool filters, bool autoGain, bool ride);
        /** Test hook: the wet's polarity inverted against the dry before the mix. */
        void setTestInvertWet (bool inverted) { m_s.invertWet = inverted; }

        // ---- The level match's learnt state (kept across resets, saved with the session).
        struct MatchState
        {
            double matchDb = 0.0;          // M
            double seconds = 0.0;          // a: matched seconds of the current learn (0..10)
            double dryPower = 0.0, wetPower = 0.0;   // Dacc, Wacc: cumulative power means
            double dataSeconds = 0.0;      // what the accumulators hold
            bool haveGainRef = false;
            double gainRefDb = 0.0;        // IN's gain (dB) when the data began
            bool haveKey = false;
            std::uint64_t key = 0;         // the settings key the data belongs to
            double priorDb = 0.0;          // a hint's prior still in force (getMatchState fills these
            double priorWeight = 0.0;      // from the learn; 0 weight: none), saved with the session
        };
        /** A hint's weight in matched seconds at first (it fades to 0 at the stop). */
        static constexpr double hintWeightSeconds = 2.0;
        MatchState getMatchState() const;
        /** A session restore: the kept state with its hint prior; the probe is cleared, M applies at once. */
        void restoreLearning (const MatchState& m);
        /** Forget the match (M = 0): a fresh learn. */
        void clearLearning();
        /** A hint (a preset's match): M = hint, worth 2 s of matched sound at first and nothing
            once the match stops; no fast phase while it is in force. Nothing is learnt for
            `settleSamples` of signal after it (the chain settling into the loaded settings). */
        void applyHint (double matchDb, int settleSamples = 0);
        /** The settings key of this chunk (every field of the chain's settings but OUTPUT's own). A
            change starts a probe; with `adopt` the key is taken as the one in force without one. */
        void setSettingsKey (std::uint64_t key, bool adopt = false);

        // Readouts (the last chunk)
        float getReductionDb() const { return m_s.reductionDb; }        // maximizer, <= 0
        float getClipDb() const { return m_s.clipActivityDb; }          // peak dB flattened by the clipper, >= 0
        float getSafetyReductionDb() const { return m_s.safetyDb; }     // safety stage, <= 0
        float getMatchDb() const { return static_cast<float> (m_m.matchDb); }
        float getMatchSeconds() const { return static_cast<float> (m_m.seconds); }
        float getMatchDataSeconds() const { return static_cast<float> (m_m.dataSeconds); }
        bool isProbing() const { return m_s.probing; }
        int getProbeAdoptions() const { return m_s.probeAdoptions; }        // since prepare()
        std::uint64_t getMatchedFrames() const { return m_s.matchedFrames; }   // since prepare()
        std::uint64_t getWetRejectedFrames() const { return m_s.wetRejectedFrames; }   // IN matched, the wet gate said no
        float getMixGain() const { return static_cast<float> (m_s.mixK); }  // k of the mix law, the last frame
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
        void endFrame (bool learn);
        void freshLearn();
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
        double m_level20Coeff = 0.0;                      // per frame: the match's 20 ms levels
        double m_matchBlendStep = 0.0;                    // per sample: the Level Match switch's 20 ms ramp
        double m_rhoCoeff = 0.0;                          // per sample: the mix law's 400 ms powers
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
            bool invertWet = false;
            bool snap = true;

            // Level match: the frame measurement (cleared on reset).
            double wetSum = 0.0, drySum = 0.0;
            int framePhase = 0;
            double dry20 = 0.0, wet20 = 0.0;              // 20 ms one-poles of the frame powers
            float lastGain = 1.0f, lastActive = 0.0f;     // IN's traces at the frame's last sample
            bool matchOn = true;                          // the Level Match switch
            double matchBlend = 1.0;                      // its 20 ms ramp
            double matchApplied = 0.0;                    // M through the 20 ms smoother (dB)

            // The probe and the prior (kept across resets, not saved).
            bool probing = false;
            double probeSeconds = 0.0, probeDry = 0.0, probeWet = 0.0;
            double priorDb = 0.0, priorWeight = 0.0;
            int settleFrames = 0;                         // frames not learnt from after a hint

            // Counters for the tests (kept across resets).
            int probeAdoptions = 0;
            std::uint64_t matchedFrames = 0, wetRejectedFrames = 0;

            // Mix law: running powers and cross power of what reaches the mix.
            double mixCount = 0.0;
            double mixDd = 0.0, mixWw = 0.0, mixDw = 0.0;
            double mixK = 1.0, mixKPrev = 1.0;

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
        MatchState m_m;
    };
}
