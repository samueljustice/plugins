#pragma once

#include "../common/DelayLine.h"
#include "../common/Module.h"
#include "../common/Oversampler2x.h"
#include "../common/TptOnePole.h"

#include <array>
#include <vector>

namespace roar
{
    /** RING choice values, in the order of the parameter's choice list. */
    enum class RingCarrier { sine, triangle, square };
    enum class RingTrack { fixed, follow };
    enum class RingMode { ring, am };
    enum class RingBody { off, box, can };

    struct RingSettings
    {
        bool on = false;                          // ringOn, Off by default
        float amount = 0.40f;                     // ring, u 0..1
        float freqHz = 150.0f;                    // ringFreq, 20..4000 Hz (Track Fixed)
        float ratio = 1.0f;                       // ringRatio, 0.25..4 (Track Follow)
        RingCarrier carrier = RingCarrier::sine;
        RingTrack track = RingTrack::fixed;
        RingMode mode = RingMode::ring;
        RingBody body = RingBody::off;
    };

    /**
     * RING: a ring modulator with a band-limited carrier, an optional pitch-tracked carrier, an
     * AM variant and an optional short comb "voice box" body.
     *
     *   x -+------------------------------------------------- dry (OnFade, 23 samples) --+
     *      +- HP 10 Hz -> 2x up -> * kS c(n) -> 2x down -> HP 15 Hz ------------------ p -+
     *                                                                                  |
     *      Ring: y = sqrt (1 - a) dry + sqrt (a) p                                     |
     *      AM:   y = kAM ((1 - a/2) dry + (a/2) p / kS)        (= x ((1 - a) + a (1 + c)/2) kAM)
     *      y -> Body comb (Off / Box / Can) -> OnFade -> out
     *
     * Carrier frequency, every 32 samples (then linear in log2 per sample):
     *   Fixed   fc = Freq (smoothed in log2, 20 ms)
     *   Follow  fc = Ratio f0, f0 from the VoiceStream when its confidence is >= 0.4, otherwise
     *           held (140 Hz before any pitch). The first pitch is taken at once; after that f0
     *           glides in log2 with a 7 ms time constant (settled within about 20 ms), and an
     *           octave jump must persist for 40 ms before it is followed.
     *   A Track switch glides fc in log2 over 30 ms. fc is clamped to [10 Hz, 0.45 fs].
     *   The phase is a double accumulator, never reset by a parameter change, so fc moves without
     *   clicks; reset() sets it to 0.
     *
     * Carriers (unit-peak shapes, kS = 1 / rms):
     *   Sine      a 2048-point sine table, kS = sqrt 2
     *   Triangle  odd harmonics 8 / (pi^2 k^2), alternating sign, kS = sqrt 3
     *   Square    odd harmonics 4 / (pi k), kS = 1
     *   Triangle and Square are mip-mapped: level L covers fc in [10 2^L, 10 2^(L+1)) Hz and holds
     *   the odd harmonics with k 10 2^(L+1) <= fmaxC = min (10 kHz, 0.42 fs) (the fundamental
     *   always; above 10 kHz the products would only add fizz to a voice)
     *   always). The table of a level has max (2048, 8 kmax) points (a power of two), read with
     *   cubic Hermite. Within an octave the reader crossfades from level L to level L+1 by the
     *   log position in the octave, so no harmonic switches on or off as fc sweeps. With the input
     *   below fs/2 and the carrier below fmaxC, no product reaches the 2x Nyquist.
     *   Carrier changes crossfade the two carriers over 10 ms.
     *
     * Mode changes crossfade the Ring and AM outputs over 20 ms. kAM = 1 / rms (g) of the AM gain
     * g = 1 - a/2 + (a/2) c, capped at +3 dB, with a = sqrt (Amount) in AM mode (laws::ringAmAmount:
     * the index is sqrt (Amount) / 2, so the low end of the travel already modulates audibly).
     *
     * Body (on the mixed output, at the base rate):
     *   z = y + fb LP1 (z[n - D]);   out = (1 - bm) y + bm z sqrt (1 - fb^2)
     *   Box  D 2.2 ms, fb -0.55, loop low-pass 5.5 kHz, bm 0.45  (peaks at odd multiples of 227 Hz)
     *   Can  D 0.9 ms, fb +0.62, loop low-pass 7 kHz,   bm 0.50  (peaks at multiples of 1.11 kHz)
     *   The read tap is shortened by the loop low-pass's group delay, so the peaks land on the
     *   nominal frequencies. Both combs run whenever the module is active, so a Body change
     *   crossfades (20 ms) between warm combs and no first echo steps into the fade.
     *
     * Latency: Oversampler2x::c_latency (23 samples) at every rate, setting and On state. Above
     * 100 kHz the product runs at the base rate behind a 23-sample delay (products only fold from
     * above fs/2, far outside the audible band). Linked stereo: one carrier for both channels,
     * per-channel oversamplers, DC blockers and combs; it never makes stereo. When the module comes
     * back On from fully Off, the oversamplers, DC blockers and combs start empty (the phase keeps
     * running). Every control value runs per sample or on a 32-sample grid counted from reset, so
     * the output does not depend on the host block size. No allocation after prepare(); NaN-safe
     * (non-finite input, or |x| >= 1e4, is treated as silence on the wet path).
     */
    class RingModule final : public Module
    {
    public:
        static constexpr int c_controlInterval = 32;
        static constexpr double c_minCarrierHz = 10.0;
        static constexpr double c_maxCarrierFraction = 0.45;     // of fs
        static constexpr double c_minFreqHz = 20.0, c_maxFreqHz = 4000.0;
        static constexpr double c_minRatio = 0.25, c_maxRatio = 4.0;
        static constexpr double c_harmonicMaxHz = 10000.0;
        static constexpr double c_harmonicMaxFraction = 0.42;    // of fs
        static constexpr double c_levelBaseHz = 10.0;
        static constexpr int c_maxLevels = 18;
        static constexpr int c_minTableSize = 2048;
        static constexpr int c_pointsPerHarmonic = 8;
        static constexpr double c_paramSeconds = 0.020;          // Amount, Freq, Ratio
        static constexpr double c_f0GlideTau = 0.007;
        static constexpr double c_octaveConfirmSeconds = 0.040;
        static constexpr double c_octaveTolerance = 0.125;       // octaves
        static constexpr double c_minConfidence = 0.4;
        static constexpr double c_initialF0Hz = 140.0;
        static constexpr double c_minF0Hz = 30.0, c_maxF0Hz = 2500.0;
        static constexpr double c_trackGlideSeconds = 0.030;
        static constexpr double c_shapeFadeSeconds = 0.010;
        static constexpr double c_modeFadeSeconds = 0.020;
        static constexpr double c_bodyFadeSeconds = 0.020;
        static constexpr double c_inputDcHz = 10.0;
        static constexpr double c_wetDcHz = 15.0;
        static constexpr double c_amMakeupCapDb = 3.0;
        static constexpr double c_oversampleMaxRate = 100000.0;  // above: the product runs at 1x

        void prepare (double fs, int maxBlock) override;
        void reset() override;
        int getLatencySamples() const override { return Oversampler2x::c_latency; }
        void process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice) override;
        void copyStateFrom (const Module& other) override;
        void copyLeftStateToRight() override;

        void setParams (const RingSettings& s);

        // Readouts (audio-thread values). For MonsterFeatures: ringAmount = getAmountFeature(),
        // ringHz = getCarrierHz(), ringBody = getBodyIndex() (body depth getBodyFeature()).
        float getCarrierHz() const { return static_cast<float> (m_s.fcNow); }
        float getAmountFeature() const { return m_s.amount * m_fade.getValue(); }
        int getBodyIndex() const { return static_cast<int> (m_settings.body); }
        float getBodyFeature() const;                         // body mix bm in effect x On fade
        float getOnValue() const { return m_fade.getValue(); }
        double getPhase() const { return m_s.phase; }
        float getFollowedF0Hz() const;                        // Follow's glided f0
        bool isOversampling() const { return m_oversample; }
        double getHarmonicLimitHz() const { return m_harmonicMaxHz; }

        /** The unit-peak band-limited carrier (no kS) at phase 0..1 for a carrier at fcHz. */
        float carrierValue (RingCarrier carrier, double phase, double fcHz) const;

        /** kS = 1 / rms of the unit-peak carrier: sqrt 2, sqrt 3, 1. */
        static float shapeMakeup (RingCarrier carrier);

        /** kAM for amount a and a carrier of makeup kS: 1 / rms ((1 - a/2) + (a/2) c), capped at +3 dB. */
        static float amMakeupFor (float amount, float shapeMakeupValue);

    private:
        struct BodySpec
        {
            double delaySeconds;
            float feedback;
            double lowpassHz;
            float mix;
        };

        static constexpr std::array<BodySpec, 2> c_bodies { {
            { 0.0022, -0.55f, 5500.0, 0.45f },     // Box
            { 0.0009, 0.62f, 7000.0, 0.50f },      // Can
        } };

        struct TableRef
        {
            int offset = 0;     // index of point 0 in m_tables (one guard point before, two after)
            int size = 1;
        };

        struct Comb
        {
            DelayLine line;
            TptOnePole lowpass;
        };

        struct Channel
        {
            Oversampler2x os;
            DelayLine direct;          // 1x path above c_oversampleMaxRate
            TptOnePole dcIn, dcOut;
            std::array<Comb, 2> combs;
        };

        struct State
        {
            double phase = 0.0;
            double logFreq = 0.0;      // smoothed log2 Freq
            double logRatio = 0.0;     // smoothed log2 Ratio
            double f0Log = 0.0;        // glided log2 f0
            double f0Target = 0.0;     // followed log2 f0 (after the octave check)
            double candidateLog = 0.0;
            int candidateSamples = 0;
            bool hasPitch = false;
            double trackMix = 0.0;     // 0 Fixed .. 1 Follow (linear, smoothstep applied)
            double logNow = 0.0;       // per-sample log2 fc
            double logStep = 0.0;
            double fcNow = 150.0;
            int tickCountdown = 0;
            float amount = 0.4f;
            int shapeFrom = 0, shapeTo = 0;
            float shapeMix = 1.0f;     // 0..1 from -> to (linear)
            float modeMix = 0.0f;      // 0 Ring .. 1 AM (linear, smoothstep applied)
            int bodyFrom = 0, bodyTo = 0;
            float bodyMix = 1.0f;      // 0..1 from -> to (linear, smoothstep applied)
        };

        void resetWet();
        void snapMixState();       // Amount, Carrier, Mode and Body mixes to the settings
        void snapToSettings();
        void applySettingChanges();
        void controlTick (const VoiceFrame& frame);
        void updateTracker (const VoiceFrame& frame);
        double targetLog() const;
        void advanceControl (int i, const VoiceFrameView& voice);
        float shapeAt (int shape, double phase, int level, float t) const;
        float carrierAt (double phase, int level, float t) const;
        float readTable (const TableRef& table, double phase) const;
        float combOutput (Channel& ch, int comb, float y);   // comb 0 Box, 1 Can

        static float bodyPick (int body, float y, float box, float can)
        {
            return body == static_cast<int> (RingBody::box) ? box : (body == static_cast<int> (RingBody::can) ? can : y);
        }
        void startShape (int shape);
        void startBody (int body);
        void buildTables();

        double m_fs = 48000.0;
        double m_fsOs = 96000.0;
        double m_invFsOs = 1.0 / 96000.0;
        bool m_oversample = true;
        int m_maxBlock = 512;
        double m_harmonicMaxHz = 10000.0;
        double m_logMin = 0.0, m_logMax = 0.0, m_logBase = 0.0;
        int m_numLevels = 2;

        float m_amountCoeff = 0.0f;
        double m_ctlParamCoeff = 0.0, m_f0Coeff = 0.0, m_trackStep = 0.0;
        int m_octaveConfirmSamples = 1920;
        float m_shapeStep = 0.0f, m_modeStep = 0.0f, m_bodyStep = 0.0f;
        std::array<float, 2> m_combRead {};     // Hermite read position per body
        std::array<float, 2> m_combNorm {};     // sqrt (1 - fb^2)

        std::vector<float> m_tables;
        TableRef m_sineTable;
        std::array<std::array<TableRef, c_maxLevels>, 2> m_levels {};   // triangle, square

        RingSettings m_settings;
        State m_s;
        std::array<Channel, 2> m_channels;
        OnFade m_fade;
    };
}
