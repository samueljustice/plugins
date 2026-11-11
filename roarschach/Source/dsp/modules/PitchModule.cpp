#include "PitchModule.h"

#include "../KnobLaws.h"

#include "../common/DspMath.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace roar
{
    namespace
    {
        constexpr double c_psolaSeconds = 0.030;
        constexpr double c_unvoicedPeriodSeconds = 0.010;   // pseudo-period: unvoiced grain half-width and spacing
        constexpr double c_tapeSeconds = 0.020;              // resampling path: read-head sweep
        constexpr double c_tapeFullDeviation = 0.1;          // |1 - phi| at which the sweep is full width
        constexpr double c_tapeCorrelationSeconds = 0.010;   // head-correlation estimate of the crossfade
        constexpr double c_tapeMinCorrelation = -0.5;
        constexpr double c_knobSmoothSeconds = 0.020;
        constexpr double c_slewSeconds = 0.005;              // ratio slew (Shift), Snap / Mono glide
        constexpr double c_wetFadeSeconds = 0.020;
        constexpr double c_driveFadeSeconds = 0.020;
        constexpr double c_ln2 = 0.69314718055994530942;

        // Marks.
        constexpr double c_markTolerance = 0.25;             // search +-25 % of a period around the prediction
        constexpr float c_markPenalty = 0.5f;
        constexpr float c_minMarkLevel = 1.0e-5f;
        // A mark is placed one best-matching lag after the previous one, so the chain follows the
        // waveform, not whichever ripple of the pulse happens to be tallest. The lag is found in
        // two passes on the band-limited copy (a ~6 kHz grid, then single samples, then a
        // fraction of a sample): the whole cycle before the previous mark, searched +-20 % around
        // the period, says which lag repeats; the stretch around the pulse itself, searched close
        // to that lag, says where the next pulse sits (the cycle-long match alone answers with
        // the length of the cycle before, a cycle late on a moving pitch). The chain leans a
        // little towards the peak each cycle so it stays on the pulse, and returns to the peak
        // outright if the two ever come apart by more than c_markChainReach of a period.
        constexpr double c_markLagRange = 0.2;
        constexpr double c_markLagGridHz = 6000.0;
        constexpr double c_markPulseBefore = 0.3, c_markPulseAfter = 0.05;   // periods around the mark
        constexpr double c_markPulseRange = 0.06;
        constexpr double c_markPeakLeak = 0.03;
        constexpr double c_markChainReach = 0.3;
        // A run also continues where the cycles at the chain's own lag correlate this well, even
        // if the peak hopped to another ripple and the peak-to-peak test failed (never below the
        // mode's threshold).
        constexpr double c_markChainCorrelation = 0.8;
        // A voiced grain is created at least this many half-widths of the previous one ahead of
        // its centre, so a period that grows faster than the detector reports does not push the
        // grains late (a flat pitch at the end of a phrase).
        constexpr double c_grainLeadMargin = 1.1;
        // Adjacent periods must look alike to count as voiced. Where the resampling path already
        // gives the right pitch and formants (Follow, Size 0), a mark must be convincing: an
        // irregular (creaky) voice is better resampled than cut into misplaced grains. Where it
        // would not (Keep, Size), the grains are the only way to move the pitch, so accept more.
        constexpr double c_markCorrelationStrict = 0.85;
        constexpr double c_markCorrelationLoose = 0.5;
        constexpr double c_strictWithinSt = 0.5, c_looseBeyondSt = 1.5;
        constexpr double c_minHz = 50.0, c_maxHz = 1000.0;

        // Formant model.
        constexpr double c_sizeFormantSt = 6.0;
        constexpr double c_minRate = 0.25, c_maxRate = 1.41421356237309505;
        constexpr double c_minPitchSt = -24.0, c_maxPitchSt = 12.0;

        // Ring reads: Kaiser-windowed sinc, c_sincZeros zero crossings each side at phi <= 1, the
        // table sampled c_sincResolution times per crossing (linear between points). At phi > 1
        // the kernel is stretched by phi (cut-off at the output Nyquist), up to c_sincMaxHalf taps
        // each side. c_readAhead: input samples a read needs beyond its position.
        constexpr int c_sincZeros = 16;
        constexpr int c_sincResolution = 512;
        constexpr int c_sincLength = c_sincZeros * c_sincResolution;
        constexpr double c_sincBeta = 9.0;
        constexpr int c_sincMaxHalf = 23;                     // ceil (c_sincZeros * c_maxRate)
        constexpr double c_readAhead = c_sincMaxHalf + 2.0;
        static_assert (c_sincMaxHalf >= c_sincZeros * c_maxRate, "kernel too short for the widest read");

        constexpr int c_sincTaps = 2 * c_sincZeros;

        struct SincTable
        {
            // The kernel h (x), x = 0 .. c_sincZeros in steps of 1 / c_sincResolution (stretched reads).
            std::array<float, c_sincLength + 2> value {};
            std::array<float, c_sincLength + 2> slope {};
            // Polyphase rows for phi <= 1: row p holds the c_sincTaps taps for the fraction
            // p / c_sincResolution (tap k at sample floor (pos) + 1 - c_sincZeros + k), each row
            // normalised to sum to one.
            std::array<std::array<float, c_sincTaps>, c_sincResolution + 1> phase {};
        };

        double besselI0 (double x)
        {
            double sum = 1.0, term = 1.0;
            const double q = 0.25 * x * x;
            for (int k = 1; k < 64 && term > 1.0e-17 * sum; ++k)
            {
                term *= q / (static_cast<double> (k) * static_cast<double> (k));
                sum += term;
            }
            return sum;
        }

        /** Built once (the first PitchModule's constructor), shared and read-only afterwards. */
        const SincTable& sincTable()
        {
            static const SincTable table = []
            {
                SincTable t;
                const double norm = 1.0 / besselI0 (c_sincBeta);
                for (int i = 0; i <= c_sincLength; ++i)
                {
                    const double x = static_cast<double> (i) / c_sincResolution;
                    const double r = x / c_sincZeros;
                    double v = 0.0;
                    if (i == 0)
                        v = 1.0;
                    else if (i % c_sincResolution != 0 && r < 1.0)   // exact zeros on the integers
                        v = std::sin (3.141592653589793 * x) / (3.141592653589793 * x) * besselI0 (c_sincBeta * std::sqrt (1.0 - r * r)) * norm;
                    t.value[static_cast<size_t> (i)] = static_cast<float> (v);
                }
                for (int i = 0; i <= c_sincLength; ++i)
                    t.slope[static_cast<size_t> (i)] = t.value[static_cast<size_t> (i + 1)] - t.value[static_cast<size_t> (i)];
                for (int p = 0; p <= c_sincResolution; ++p)
                {
                    auto& row = t.phase[static_cast<size_t> (p)];
                    double sum = 0.0;
                    for (int k = 0; k < c_sincTaps; ++k)
                    {
                        const int i = std::abs ((k + 1 - c_sincZeros) * c_sincResolution - p);
                        row[static_cast<size_t> (k)] = t.value[static_cast<size_t> (std::min (i, c_sincLength))];
                        sum += static_cast<double> (row[static_cast<size_t> (k)]);
                    }
                    for (auto& v : row)
                        v = static_cast<float> (static_cast<double> (v) / sum);
                }
                return t;
            }();
            return table;
        }

        // Modes.
        constexpr double c_snapHysteresisSt = 0.3;
        constexpr double c_monoBaseHz = 130.81;               // C3
        constexpr double c_monoMinRatio = 0.25, c_monoMaxRatio = 4.0;

        // Size body.
        constexpr int c_bodyUpdateInterval = 32;
        constexpr std::array<double, 2> c_bodyHz { 250.0, 500.0 };
        constexpr std::array<double, 2> c_bodyQ { 1.1, 1.3 };
        constexpr double c_bodyMaxDb = 2.0;
        constexpr double c_bodyTrimDb = -1.0;                 // the two peaks add about 1 dB on a voice at full Size
        constexpr double c_throatGain = 0.15;

        // Drive.
        constexpr int c_driveUpdateInterval = 16;
        constexpr double c_driveMaxDb = 24.0;
        constexpr double c_driveShelfHz = 2500.0, c_driveShelfQ = 0.7, c_driveShelfDb = 3.0;
        constexpr double c_driveLpHz0 = 16000.0, c_driveLpHz1 = 7500.0;
        constexpr double c_driveDcHz = 5.0;
        constexpr float c_biasStatic = 0.18f, c_biasSag = 0.25f;
        constexpr double c_biasDriveExponent = 0.6;
        constexpr float c_plateK = 1.4f, c_gridK = 0.8f;
        constexpr double c_envAttackSeconds = 0.005, c_envReleaseSeconds = 0.080;

        // Static makeup vs Drive (0, 0.1, ..., 1), calibrated on voice-shaped pink noise at -20 dBFS
        // RMS so the drive keeps the RMS of its input (on top of the g^-0.75 law).
        constexpr std::array<float, 11> c_makeup { 1.0143f, 1.0000f, 0.9711f, 0.9366f, 0.9019f, 0.8723f, 0.8542f, 0.8562f, 0.8901f, 0.9743f, 1.1349f };

        inline float sanitize (float x)
        {
            return std::abs (x) < 1.0e4f ? x : 0.0f;   // NaN fails the comparison too
        }

        inline double shape (double u)
        {
            constexpr double a = static_cast<double> (c_plateK), c = static_cast<double> (c_gridK);
            return u >= 0.0 ? std::tanh (a * u) / a : std::tanh (c * u) / c;
        }

        /** log cosh x without overflow. */
        inline double logCosh (double x)
        {
            x = std::abs (x);
            return x + std::log1p (std::exp (-2.0 * x)) - c_ln2;
        }

        /** Antiderivative of shape(): log cosh (k u) / k^2 per side (continuous at 0). */
        inline double shapeIntegral (double u)
        {
            constexpr double a = static_cast<double> (c_plateK), c = static_cast<double> (c_gridK);
            return u >= 0.0 ? logCosh (a * u) / (a * a) : logCosh (c * u) / (c * c);
        }

        /** First-order antiderivative anti-aliasing of shape() between the previous and current input. */
        inline double shapeAdaa (double u, double previous)
        {
            const double d = u - previous;
            if (std::abs (d) < 1.0e-5)
                return shape (0.5 * (u + previous));
            return (shapeIntegral (u) - shapeIntegral (previous)) / d;
        }

        inline double clampFinite (double x, double lo, double hi, double fallback)
        {
            return x >= lo ? std::min (x, hi) : (x < lo ? lo : fallback);   // NaN -> fallback
        }
    }

    //==============================================================================
    PitchModule::PitchModule()
    {
        sincTable();   // build the shared kernel table here, never on the audio thread
        for (int i = 0; i <= c_windowSize + 1; ++i)
            m_cosTable[static_cast<size_t> (i)] = static_cast<float> (std::cos (0.5 * dsp::c_pi * std::min (i, c_windowSize) / c_windowSize));
    }

    int PitchModule::latencyFor (double fs)
    {
        const double rate = dsp::safeSampleRate (fs);
        return static_cast<int> (std::ceil (c_psolaSeconds * rate - 1.0e-6)) + Oversampler2x::c_latency;
    }

    void PitchModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        m_maxBlock = std::max (1, maxBlock);
        m_latency = latencyFor (m_fs);
        m_psolaLatency = m_latency - Oversampler2x::c_latency;
        m_minPeriod = std::max (2, static_cast<int> (std::floor (m_fs / c_maxHz)));
        m_maxPeriod = static_cast<int> (std::ceil (m_fs / c_minHz));
        m_unvoicedHalfWidth = c_unvoicedPeriodSeconds * m_fs;

        // History: the latency, plus how far a grain's reads trail the output time. A grain reads
        // its mark +- one period over a window of +- period / phi (four periods each side at
        // phi 0.25), so its end reads (centre - mark) + 3 periods behind the output time. The
        // centre trails its mark by up to 3 periods (a borrowed mark) plus the placement push
        // (after an unvoiced run or a restart the centre moves to now + half-width, up to four
        // periods later): at most ten periods in all. Plus room for the interpolator.
        const int length = dsp::nextPowerOfTwo (m_psolaLatency + 10 * m_maxPeriod + 1024);
        m_inL.assign (static_cast<size_t> (length), 0.0f);
        m_inR.assign (static_cast<size_t> (length), 0.0f);
        m_band.assign (static_cast<size_t> (length), 0.0f);
        m_mask = length - 1;

        m_detector.prepare (m_fs);
        m_bandHpCoeff = static_cast<float> (std::exp (-dsp::c_twoPi * 60.0 / m_fs));

        m_knobCoeff = dsp::onePoleCoeff (c_knobSmoothSeconds, m_fs);
        m_slewCoeff = dsp::onePoleCoeff (c_slewSeconds, m_fs);
        m_slewTauSamples = c_slewSeconds * m_fs;
        m_wetStep = static_cast<float> (1.0 / (c_wetFadeSeconds * m_fs));
        m_driveFadeLength = std::max (1, dsp::samplesFor (c_driveFadeSeconds, m_fs));
        m_envAttack = static_cast<float> (dsp::onePoleCoeff (c_envAttackSeconds, m_fs));
        m_envRelease = static_cast<float> (dsp::onePoleCoeff (c_envReleaseSeconds, m_fs));
        m_shareCoeff = dsp::onePoleCoeff (0.010, m_fs);
        m_tapeCorrCoeff = dsp::onePoleCoeff (c_tapeCorrelationSeconds, m_fs);

        m_throatSize = static_cast<int> (std::ceil (0.0045 * m_fs)) + 4;
        m_throatL.assign (static_cast<size_t> (m_throatSize), 0.0f);
        m_throatR.assign (static_cast<size_t> (m_throatSize), 0.0f);

        m_driveDelayL.prepare (Oversampler2x::c_latency);
        m_driveDelayR.prepare (Oversampler2x::c_latency);
        m_driveDelayL.setFixedDelay (Oversampler2x::c_latency);
        m_driveDelayR.setFixedDelay (Oversampler2x::c_latency);

        const int frames = VoiceStream::maxFramesFor (m_maxBlock);
        m_frameLogRatio.assign (static_cast<size_t> (frames), 0.0f);
        m_frameTargetHz.assign (static_cast<size_t> (frames), 0.0f);
        m_frameCount = 0;

        m_fade.prepare (m_fs, m_maxBlock, m_latency);
        reset();
    }

    void PitchModule::reset()
    {
        std::fill (m_inL.begin(), m_inL.end(), 0.0f);
        std::fill (m_inR.begin(), m_inR.end(), 0.0f);
        std::fill (m_band.begin(), m_band.end(), 0.0f);
        m_detector.reset();

        Core& c = m_core;
        const float pitchTarget = c.pitchTarget, sizeTarget = c.sizeTarget, driveTarget = c.driveTarget;
        const double pitch = static_cast<double> (pitchTarget);
        const double size = static_cast<double> (sizeTarget);
        const double drive = static_cast<double> (driveTarget);
        const auto mode = c.mode;
        const auto formant = c.formant;
        c = Core {};
        c.pitchTarget = pitchTarget;
        c.sizeTarget = sizeTarget;
        c.driveTarget = driveTarget;
        c.mode = mode;
        c.formant = formant;
        c.bandLp.setCoeffs (Biquad::lowpass (m_fs, 900.0, Biquad::c_butterworthQ));

        c.prevSyn = -static_cast<double> (m_psolaLatency);
        c.nextSyn = c.prevSyn + m_unvoicedHalfWidth;
        c.prevHalfWidth = m_unvoicedHalfWidth;

        // A reset is a new start: jump to the targets.
        c.knobSt = pitch;
        c.logRatio = c_ln2 * pitch / 12.0;
        c.ratio = std::exp (c.logRatio);
        c.modeTarget = c.logRatio;
        c.size = size;
        updateRate();
        const bool shifting = mode != Params::PitchMode::shift || std::abs (pitch) > 0.005 || size > 0.001;
        c.wet = shifting ? 1.0f : 0.0f;

        c.drive = drive;
        c.driveRunning = drive >= 0.001;
        c.driveMix = c.driveRunning ? 1.0f : 0.0f;
        c.driveFadePos = c.driveRunning ? m_driveFadeLength : 0;
        resetDriveState();

        for (auto& ch : m_body)
            for (auto& b : ch)
                b.reset();
        std::fill (m_throatL.begin(), m_throatL.end(), 0.0f);
        std::fill (m_throatR.begin(), m_throatR.end(), 0.0f);
        c.size = size;
        updateBodyCoeffs();

        m_driveDelayL.reset();
        m_driveDelayR.reset();
        m_fade.reset();

        std::fill (m_frameLogRatio.begin(), m_frameLogRatio.end(), 0.0f);
        std::fill (m_frameTargetHz.begin(), m_frameTargetHz.end(), 0.0f);
        m_frameCount = 0;

        m_readShiftSt.store (0.0f, std::memory_order_relaxed);
        m_readFormantSt.store (0.0f, std::memory_order_relaxed);
        m_readHeat.store (0.0f, std::memory_order_relaxed);
        m_readF0.store (0.0f, std::memory_order_relaxed);
        m_readVoiced.store (false, std::memory_order_relaxed);
    }

    void PitchModule::setParams (const PitchSettings& s)
    {
        Core& c = m_core;
        c.pitchTarget = static_cast<float> (clampFinite (static_cast<double> (s.pitchSemitones), c_minPitchSt, c_maxPitchSt, 0.0));
        c.sizeTarget = static_cast<float> (clampFinite (static_cast<double> (s.size), 0.0, 1.0, 0.0));
        c.driveTarget = static_cast<float> (laws::pitchDriveAmount (clampFinite (static_cast<double> (s.drive), 0.0, 1.0, 0.0)));
        if (s.mode != c.mode)
        {
            c.haveSnapNote = false;
            c.modeTarget = c.logRatio;   // glide from where the ratio is
        }
        c.mode = s.mode;
        c.formant = s.formant;
        m_fade.setOn (s.on);
    }

    //==============================================================================
    void PitchModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice)
    {
        n = std::clamp (n, 0, m_maxBlock);
        m_fade.captureDry (l, r, n, stereo);

        const bool active = m_fade.isActive();
        if (active && m_fade.consumeRestart())
            restartWet();

        const float fadeWeight = m_fade.getValue();
        const bool fullyOn = m_fade.isFullyOn();
        const int frames = std::min (static_cast<int> (m_frameLogRatio.size()), voice.frameIndexForSample (std::max (0, n - 1)) + 1);
        m_frameCount = frames;

        for (int i = 0; i < n;)
        {
            const int end = voice.frameEnd (i, n);
            for (; i < end; ++i)
            {
                const float xl = sanitize (l[i]);
                const float xr = stereo ? sanitize (r[i]) : 0.0f;
                analyse (xl, xr, stereo);
                smoothParams();

                if (! active)
                    continue;

                float yr = 0.0f;
                float yl = synthesise (yr, stereo);
                applyBody (yl, yr, stereo);
                applyDrive (yl, yr, stereo);

                if (! std::isfinite (yl) || ! std::isfinite (yr))
                {
                    clearAfterNonFinite();
                    yl = yr = 0.0f;
                }

                l[i] = yl;
                if (stereo)
                    r[i] = yr;
            }

            // What the stream downstream should say for this frame (rewriteVoice).
            const int k = std::min (voice.frameIndexForSample (std::max (0, end - 1)), frames - 1);
            if (k >= 0)
            {
                const Core& c = m_core;
                const double w = active ? static_cast<double> (fadeWeight) * static_cast<double> (c.wet) : 0.0;
                m_frameLogRatio[static_cast<size_t> (k)] = static_cast<float> (w * c.logRatio);
                float target = 0.0f;
                if (active && fullyOn && c.wet >= 1.0f && c.voicedShare > 0.5)
                {
                    if (c.mode == Params::PitchMode::snap && c.haveSnapNote)
                        target = static_cast<float> (440.0 * std::exp2 ((c.snapNote - 69) / 12.0));
                    else if (c.mode == Params::PitchMode::mono && c.monoInRange)
                        target = static_cast<float> (c_monoBaseHz * std::exp2 (c.knobSt / 12.0));
                }
                m_frameTargetHz[static_cast<size_t> (k)] = target;
            }
        }

        if (active)
            m_fade.apply (l, r, n, stereo);
        else
            m_fade.outputDry (l, r, n, stereo);

        // Readouts, scaled by the fade.
        const Core& c = m_core;
        const float w = active ? m_fade.getValue() : 0.0f;
        const double wd = static_cast<double> (w);
        const double wet = static_cast<double> (c.wet);
        const double appliedSt = 12.0 * c.logRatio / c_ln2 * wet;
        m_readShiftSt.store (static_cast<float> (wd * appliedSt), std::memory_order_relaxed);
        m_readFormantSt.store (static_cast<float> (wd * 12.0 * std::log2 (c.rate) * wet), std::memory_order_relaxed);
        m_readHeat.store (w * c.driveHeat, std::memory_order_relaxed);
        const VoiceInfo& in = m_detector.getVoiceInfo();
        const bool voicedOut = w > 0.0f && c.voicedShare > 0.5;
        m_readVoiced.store (voicedOut, std::memory_order_relaxed);
        m_readF0.store (in.f0Hz > 0.0f ? in.f0Hz * static_cast<float> (std::exp (wd * c.logRatio * wet)) : 0.0f,
                        std::memory_order_relaxed);
    }

    void PitchModule::rewriteVoice (VoiceFrame* frames, int numFrames)
    {
        if (frames == nullptr || m_frameCount <= 0)
            return;

        for (int k = 0; k < numFrames; ++k)
        {
            VoiceFrame& f = frames[k];
            if (! (f.f0Hz > 0.0f))
                continue;

            const int j = std::min (k, m_frameCount - 1);
            const float target = m_frameTargetHz[static_cast<size_t> (j)];
            if (target > 0.0f && f.voiced)
                f.f0Hz = target;
            else
                f.f0Hz *= std::exp (m_frameLogRatio[static_cast<size_t> (j)]);
        }
    }

    //==============================================================================
    void PitchModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<PitchModule> (other);
        if (&o == this)
            return;

        m_core = o.m_core;
        m_detector = o.m_detector;   // same sample rate: vector assignment reuses the storage

        const auto copyRing = [] (std::vector<float>& dst, const std::vector<float>& src)
        {
            std::copy (src.begin(), src.begin() + static_cast<std::ptrdiff_t> (std::min (dst.size(), src.size())), dst.begin());
        };
        copyRing (m_inL, o.m_inL);
        copyRing (m_inR, o.m_inR);
        copyRing (m_band, o.m_band);
        copyRing (m_throatL, o.m_throatL);
        copyRing (m_throatR, o.m_throatR);
        copyRing (m_frameLogRatio, o.m_frameLogRatio);
        copyRing (m_frameTargetHz, o.m_frameTargetHz);
        m_frameCount = std::min (o.m_frameCount, static_cast<int> (m_frameLogRatio.size()));

        m_body = o.m_body;
        m_driveCh = o.m_driveCh;
        m_driveDelayL.copyStateFrom (o.m_driveDelayL);
        m_driveDelayR.copyStateFrom (o.m_driveDelayR);
        m_fade.copyStateFrom (o.m_fade);
    }

    void PitchModule::copyLeftStateToRight()
    {
        std::copy (m_inL.begin(), m_inL.end(), m_inR.begin());
        std::copy (m_throatL.begin(), m_throatL.end(), m_throatR.begin());
        m_body[1] = m_body[0];
        m_driveCh[1] = m_driveCh[0];
        m_driveDelayR.copyStateFrom (m_driveDelayL);
        m_fade.copyLeftStateToRight();
    }

    //==============================================================================
    void PitchModule::restartWet()
    {
        // Coming back on after being fully off: the rings, detector and marks are current, the
        // synthesis starts fresh from now.
        Core& c = m_core;
        const double now = static_cast<double> (c.writePos - m_psolaLatency);
        c.grainCount = 0;
        c.prevSyn = now - m_unvoicedHalfWidth;
        c.nextSyn = now;
        c.prevHalfWidth = m_unvoicedHalfWidth;
        c.prevVoiced = false;
        c.voicedShare = 0.0;

        for (auto& ch : m_body)
            for (auto& b : ch)
                b.reset();
        std::fill (m_throatL.begin(), m_throatL.end(), 0.0f);
        std::fill (m_throatR.begin(), m_throatR.end(), 0.0f);
        resetDriveState();
        m_driveDelayL.reset();
        m_driveDelayR.reset();
    }

    void PitchModule::clearAfterNonFinite()
    {
        Core& c = m_core;
        c.grainCount = 0;
        for (auto& ch : m_body)
            for (auto& b : ch)
                b.reset();
        std::fill (m_throatL.begin(), m_throatL.end(), 0.0f);
        std::fill (m_throatR.begin(), m_throatR.end(), 0.0f);
        resetDriveState();
    }

    //==============================================================================
    void PitchModule::analyse (float xl, float xr, bool stereo)
    {
        Core& c = m_core;
        const std::int64_t n = c.writePos;
        const auto at = static_cast<size_t> (n & m_mask);
        m_inL[at] = xl;
        if (stereo)
            m_inR[at] = xr;

        const float mid = stereo ? 0.5f * (xl + xr) : xl;

        // Band-limited copy for the peak search: the glottal pulse shows as one clear peak per
        // period once the upper formants are gone.
        const float hp = m_bandHpCoeff * (c.bandHpY1 + mid - c.bandHpX1);
        c.bandHpX1 = mid;
        c.bandHpY1 = dsp::flushTiny (hp);
        m_band[at] = c.bandLp.process (hp);

        m_detector.pushSample (mid);
        ++c.writePos;
        findMarks();
        pruneMarks (c.writePos - 1 - m_psolaLatency);
    }

    void PitchModule::smoothParams()
    {
        Core& c = m_core;
        c.knobSt += m_knobCoeff * (static_cast<double> (c.pitchTarget) - c.knobSt);
        if (std::abs (c.knobSt - static_cast<double> (c.pitchTarget)) < 1.0e-7)
            c.knobSt = static_cast<double> (c.pitchTarget);

        const double knobLog = c_ln2 * c.knobSt / 12.0;
        if (c.mode == Params::PitchMode::shift)
            c.modeTarget = knobLog;

        if (c.mode == Params::PitchMode::shift)
        {
            c.logRatio += m_slewCoeff * (c.modeTarget - c.logRatio);
            if (std::abs (c.logRatio - c.modeTarget) < 1.0e-9)
                c.logRatio = c.modeTarget;
        }
        c.ratio = std::exp (c.logRatio);

        c.size += m_knobCoeff * (static_cast<double> (c.sizeTarget) - c.size);
        if (std::abs (c.size - static_cast<double> (c.sizeTarget)) < 1.0e-7)
            c.size = static_cast<double> (c.sizeTarget);
        updateRate();

        const bool shifting = c.mode != Params::PitchMode::shift || std::abs (c.pitchTarget) > 0.005f || c.sizeTarget > 0.001f;
        c.wet = shifting ? std::min (1.0f, c.wet + m_wetStep) : std::max (0.0f, c.wet - m_wetStep);

        c.drive += m_knobCoeff * (static_cast<double> (c.driveTarget) - c.drive);
        if (std::abs (c.drive - static_cast<double> (c.driveTarget)) < 1.0e-7)
            c.drive = static_cast<double> (c.driveTarget);
    }

    void PitchModule::updateRate()
    {
        Core& c = m_core;
        double formantSt = 0.0;
        if (c.formant == Params::PitchFormant::follow)
            formantSt = c.mode == Params::PitchMode::mono ? c.knobSt : 12.0 * c.logRatio / c_ln2;
        formantSt -= c_sizeFormantSt * c.size;
        c.rate = std::clamp (std::exp (c_ln2 * formantSt / 12.0), c_minRate, c_maxRate);
    }

    float PitchModule::synthesise (float& right, bool stereo)
    {
        Core& c = m_core;
        const std::int64_t outPos = c.writePos - 1 - m_psolaLatency;
        scheduleGrains (outPos);
        float shiftedR = 0.0f;
        const float shiftedL = renderGrains (outPos, shiftedR, stereo);

        const auto at = static_cast<size_t> (outPos & m_mask);
        const float dryL = m_inL[at];
        const float yl = dryL + c.wet * (shiftedL - dryL);
        if (stereo)
        {
            const float dryR = m_inR[at];
            right = dryR + c.wet * (shiftedR - dryR);
        }
        return yl;
    }

    //==============================================================================
    void PitchModule::updateBodyCoeffs()
    {
        Core& c = m_core;
        const bool active = c.size > 1.0e-4;
        if (active && ! c.bodyActive)
        {
            for (auto& ch : m_body)
                for (auto& b : ch)
                    b.reset();
            std::fill (m_throatL.begin(), m_throatL.end(), 0.0f);
            std::fill (m_throatR.begin(), m_throatR.end(), 0.0f);
        }
        c.bodyActive = active;
        if (! active)
            return;

        c.bodyTrim = static_cast<float> (dsp::dbToGain (c_bodyTrimDb * c.size));
        for (size_t k = 0; k < c_bodyHz.size(); ++k)
        {
            const auto coeffs = Biquad::peaking (m_fs, c_bodyHz[k], c_bodyQ[k], c_bodyMaxDb * c.size);
            m_body[0][k].setCoeffs (coeffs);
            m_body[1][k].setCoeffs (coeffs);
        }
    }

    void PitchModule::applyBody (float& yl, float& yr, bool stereo)
    {
        Core& c = m_core;
        if (((c.writePos - 1) & (c_bodyUpdateInterval - 1)) == 0)
            updateBodyCoeffs();
        if (! c.bodyActive)
            return;

        const double delay = (0.001 + 0.003 * c.size) * m_fs;
        const double readPos = static_cast<double> (c.throatWrite) - delay;
        const int i0 = static_cast<int> (std::floor (readPos));
        const float frac = static_cast<float> (readPos - i0);
        const int a = ((i0 % m_throatSize) + m_throatSize) % m_throatSize;
        const int b = (a + 1) % m_throatSize;
        const float g = static_cast<float> (c_throatGain * c.size);
        const float norm = std::sqrt (1.0f - g * g) * c.bodyTrim;

        const auto channel = [&] (float y, std::array<Biquad, 2>& body, std::vector<float>& throat)
        {
            for (auto& f : body)
                y = f.process (y);
            const float delayed = throat[static_cast<size_t> (a)] + frac * (throat[static_cast<size_t> (b)] - throat[static_cast<size_t> (a)]);
            const float combed = y + g * delayed;
            throat[static_cast<size_t> (c.throatWrite)] = dsp::flushTiny (combed);
            return combed * norm;
        };

        yl = channel (yl, m_body[0], m_throatL);
        if (stereo)
            yr = channel (yr, m_body[1], m_throatR);
        c.throatWrite = (c.throatWrite + 1) % m_throatSize;
    }

    //==============================================================================
    float PitchModule::makeup (double d) const
    {
        const double x = std::clamp (d, 0.0, 1.0) * (c_makeupPoints - 1);
        const int i = std::min (static_cast<int> (x), c_makeupPoints - 2);
        const double t = x - i;
        const auto a = static_cast<double> (c_makeup[static_cast<size_t> (i)]);
        const auto b = static_cast<double> (c_makeup[static_cast<size_t> (i + 1)]);
        return static_cast<float> (a + t * (b - a));
    }

    void PitchModule::updateDriveCoeffs()
    {
        Core& c = m_core;
        const double d = std::clamp (c.drive, 0.0, 1.0);
        c.driveCoeffFor = d;

        const double g = dsp::dbToGain (c_driveMaxDb * std::pow (d, 1.5));
        c.driveGain = static_cast<float> (g);
        c.driveOutGain = static_cast<float> (std::pow (g, -0.75)) * makeup (d);
        c.driveBiasScale = static_cast<float> (std::pow (d, c_biasDriveExponent));

        const auto pre = Biquad::highShelf (m_fs, c_driveShelfHz, c_driveShelfQ, c_driveShelfDb * d);
        const auto post = Biquad::highShelf (m_fs, c_driveShelfHz, c_driveShelfQ, -c_driveShelfDb * d);
        const double lpHz = std::min (c_driveLpHz0 * std::pow (c_driveLpHz1 / c_driveLpHz0, d), 0.45 * m_fs);
        const auto lp = Biquad::lowpass1 (m_fs, lpHz);
        for (auto& ch : m_driveCh)
        {
            ch.pre.setCoeffs (pre);
            ch.post.setCoeffs (post);
            ch.lowpass.setCoeffs (lp);
        }
    }

    void PitchModule::resetDriveState()
    {
        const auto dc = Biquad::highpass1 (m_fs, c_driveDcHz);
        for (auto& ch : m_driveCh)
        {
            ch.pre.reset();
            ch.post.reset();
            ch.lowpass.reset();
            ch.dcBlock.reset();
            ch.dcBlock.setCoeffs (dc);
            ch.os.reset();
            ch.previous = 0.0;
        }
        m_core.driveEnv = 0.0f;
        m_core.driveHeat = 0.0f;
        updateDriveCoeffs();
    }

    void PitchModule::applyDrive (float& yl, float& yr, bool stereo)
    {
        Core& c = m_core;
        const float delayedL = m_driveDelayL.processFixed (yl);
        const float delayedR = stereo ? m_driveDelayR.processFixed (yr) : 0.0f;

        // Engage / release with a 20 ms smoothstep against the pure delay.
        const bool want = c.driveTarget >= 0.001f;
        if (want && ! c.driveRunning)
        {
            resetDriveState();
            c.driveRunning = true;
        }
        if (c.driveRunning)
        {
            c.driveFadePos = std::clamp (c.driveFadePos + (want ? 1 : -1), 0, m_driveFadeLength);
            c.driveMix = dsp::smoothstep01 (static_cast<float> (c.driveFadePos) / static_cast<float> (m_driveFadeLength));
            if (! want && c.driveFadePos == 0)
                c.driveRunning = false;
        }

        if (! c.driveRunning)
        {
            yl = delayedL;
            yr = delayedR;
            return;
        }

        if (((c.writePos - 1) & (c_driveUpdateInterval - 1)) == 0 && std::abs (c.drive - c.driveCoeffFor) > 1.0e-6)
            updateDriveCoeffs();

        const float x1L = m_driveCh[0].pre.process (yl);
        const float x1R = stereo ? m_driveCh[1].pre.process (yr) : 0.0f;
        const float level = std::max (std::abs (x1L), std::abs (x1R));
        c.driveEnv += (level > c.driveEnv ? m_envAttack : m_envRelease) * (level - c.driveEnv);
        c.driveEnv = dsp::flushTiny (c.driveEnv);
        c.driveHeat = std::min (1.0f, c.driveEnv * c.driveGain * c.driveBiasScale * 2.0f);

        const double bias = static_cast<double> ((c_biasStatic - c_biasSag * c.driveEnv) * c.driveBiasScale);
        const double shapedBias = shape (bias);
        const float g = c.driveGain;

        const auto channel = [&] (DriveChannel& ch, float x1, float delayed)
        {
            float u0, u1;
            ch.os.upsample (g * x1, u0, u1);
            const double v0 = static_cast<double> (u0) + bias;
            const double v1 = static_cast<double> (u1) + bias;
            const double s0 = shapeAdaa (v0, ch.previous);
            const double s1 = shapeAdaa (v1, v0);
            ch.previous = v1;
            float y = ch.os.downsample (static_cast<float> (s0 - shapedBias), static_cast<float> (s1 - shapedBias));
            y = ch.post.process (y);
            y = ch.lowpass.process (y);
            y = ch.dcBlock.process (y);
            y *= c.driveOutGain;
            return delayed + c.driveMix * (y - delayed);
        };

        yl = channel (m_driveCh[0], x1L, delayedL);
        if (stereo)
            yr = channel (m_driveCh[1], x1R, delayedR);
    }

    //==============================================================================
    double PitchModule::markThreshold() const
    {
        const Core& c = m_core;
        const double mismatchSt = std::abs (12.0 / c_ln2 * (std::log (c.rate) - c.logRatio));
        const double loose = std::clamp ((mismatchSt - c_strictWithinSt) / (c_looseBeyondSt - c_strictWithinSt), 0.0, 1.0);
        return c_markCorrelationStrict + loose * (c_markCorrelationLoose - c_markCorrelationStrict);
    }

    bool PitchModule::searchPeak (std::int64_t from, std::int64_t to, double predicted, double tolerance,
                                  std::int64_t& best, float& bestValue)
    {
        if (to <= from)
            return false;

        Core& c = m_core;
        float hi = -1.0e30f, lo = 1.0e30f;
        for (std::int64_t t = from; t <= to; ++t)
        {
            const float v = m_band[static_cast<size_t> (t & m_mask)];
            hi = std::max (hi, v);
            lo = std::min (lo, v);
        }

        // Slow polarity choice: the glottal pulse may point either way; follow whichever side
        // carries the larger peaks, with hysteresis so marks do not hop half a period.
        c.positivePeaks += 0.2f * (std::max (0.0f, hi) - c.positivePeaks);
        c.negativePeaks += 0.2f * (std::max (0.0f, -lo) - c.negativePeaks);
        if (c.polarity > 0.0f && c.negativePeaks > 1.4f * c.positivePeaks)
            c.polarity = -1.0f;
        else if (c.polarity < 0.0f && c.positivePeaks > 1.4f * c.negativePeaks)
            c.polarity = 1.0f;

        const float range = hi - lo;
        if (range < c_minMarkLevel)
            return false;

        const bool penalise = tolerance > 0.0;
        const float penalty = c_markPenalty * range;
        float bestScore = -1.0e30f;

        for (std::int64_t t = from; t <= to; ++t)
        {
            const float v = c.polarity * m_band[static_cast<size_t> (t & m_mask)];
            float score = v;
            if (penalise)
            {
                const float dd = static_cast<float> ((static_cast<double> (t) - predicted) / tolerance);
                score -= penalty * dd * dd;
            }

            if (score > bestScore)
            {
                bestScore = score;
                best = t;
                bestValue = v;
            }
        }

        return true;
    }

    double PitchModule::markCorrelation (std::int64_t mark, std::int64_t previous) const
    {
        // Normalised correlation of the period ending at `mark` with the one ending at `previous`
        // (band-limited signal): near 1 on a voice, small on noise.
        const std::int64_t length = mark - previous;
        if (length < 2 || length > 2 * static_cast<std::int64_t> (m_maxPeriod))
            return 0.0;

        double xy = 0.0, xx = 0.0, yy = 0.0;
        for (std::int64_t k = 1; k <= length; ++k)
        {
            const auto a = static_cast<double> (m_band[static_cast<size_t> ((previous + k) & m_mask)]);
            const auto b = static_cast<double> (m_band[static_cast<size_t> ((previous - length + k) & m_mask)]);
            xy += a * b;
            xx += a * a;
            yy += b * b;
        }

        const double norm = std::sqrt (xx * yy);
        return norm > 1.0e-20 ? xy / norm : 0.0;
    }

    double PitchModule::similarityLag (double anchor, double period, double before, double after, double centre, double range, bool& found) const
    {
        // The lag, within +-range periods of `centre`, at which the stretch from `before` periods
        // ahead of `anchor` to `after` periods past it best matches the band-limited signal that
        // many samples later (normalised cross-correlation: a coarse pass on a sparse grid,
        // single samples around its best, then the vertex of a parabola through the top three).
        // `found` is false when the best lag sits on the edge of the range, i.e. no match was
        // bracketed; the answer is then `centre`.
        found = false;
        const int step = std::max (1, static_cast<int> (m_fs / c_markLagGridHz));
        const int length = std::max (8, static_cast<int> (std::lround ((before + after) * period))) / step;
        const std::int64_t end = static_cast<std::int64_t> (std::llround (anchor + after * period)) - 2;
        const std::int64_t newest = m_core.writePos - 1;
        const int lo = static_cast<int> (std::floor (centre - range * period));
        const int hi = static_cast<int> (std::min<std::int64_t> (static_cast<std::int64_t> (std::ceil (centre + range * period)), newest - end));
        if (length < 2 || lo < 1 || hi - lo < 2)
            return centre;

        double xx = 1.0e-20;
        for (int k = 0; k < length; ++k)
        {
            const auto u = static_cast<double> (m_band[static_cast<size_t> ((end - static_cast<std::int64_t> (k) * step) & m_mask)]);
            xx += u * u;
        }

        const auto similarity = [&] (int lag)
        {
            double xy = 0.0, yy = 1.0e-20;
            for (int k = 0; k < length; ++k)
            {
                const std::int64_t at = end - static_cast<std::int64_t> (k) * step;
                const auto u = static_cast<double> (m_band[static_cast<size_t> (at & m_mask)]);
                const auto v = static_cast<double> (m_band[static_cast<size_t> ((at + lag) & m_mask)]);
                xy += u * v;
                yy += v * v;
            }
            return xy / std::sqrt (xx * yy);
        };

        int best = lo;
        double bestValue = -2.0;
        for (int lag = lo; lag <= hi; lag += step)
        {
            const double value = similarity (lag);
            if (value > bestValue)
            {
                bestValue = value;
                best = lag;
            }
        }

        const int coarse = best;
        for (int lag = std::max (lo, coarse - step + 1); lag <= std::min (hi, coarse + step - 1); ++lag)
        {
            if (lag == coarse)
                continue;
            const double value = similarity (lag);
            if (value > bestValue)
            {
                bestValue = value;
                best = lag;
            }
        }

        if (best <= lo || best >= hi)
            return centre;

        found = true;
        const double below = similarity (best - 1), above = similarity (best + 1);
        const double curvature = below - 2.0 * bestValue + above;
        double lag = static_cast<double> (best);
        if (curvature < -1.0e-12)
            lag += std::clamp (0.5 * (below - above) / curvature, -0.5, 0.5);
        return lag;
    }

    void PitchModule::pushMark (double position, float period)
    {
        Core& c = m_core;
        const auto pos = static_cast<std::int64_t> (std::llround (position));
        if (c.markCount == c_maxMarks)
        {
            c.markHead = (c.markHead + 1) % c_maxMarks;
            --c.markCount;
        }

        c.marks[static_cast<size_t> ((c.markHead + c.markCount) % c_maxMarks)] = { pos, period, static_cast<float> (position - static_cast<double> (pos)) };
        ++c.markCount;
    }

    void PitchModule::pruneMarks (std::int64_t outPos)
    {
        // Forget marks no grain can use any more.
        Core& c = m_core;
        const std::int64_t oldest = outPos - 6 * static_cast<std::int64_t> (m_maxPeriod);
        while (c.markCount > 0 && c.marks[static_cast<size_t> (c.markHead)].pos < oldest)
        {
            c.markHead = (c.markHead + 1) % c_maxMarks;
            --c.markCount;
        }
    }

    void PitchModule::findMarks()
    {
        Core& c = m_core;
        const VoiceInfo& v = m_detector.getVoiceInfo();
        if (! v.voiced || v.f0Hz <= 0.0f)
        {
            c.haveLastMark = false;
            return;
        }

        const std::int64_t newest = c.writePos - 1;
        const double threshold = markThreshold();
        const double period = std::clamp (m_fs / static_cast<double> (v.f0Hz), static_cast<double> (m_minPeriod), static_cast<double> (m_maxPeriod));
        const double tolerance = c_markTolerance * period;
        const std::int64_t newestMark = c.markCount > 0
                                          ? c.marks[static_cast<size_t> ((c.markHead + c.markCount - 1) % c_maxMarks)].pos
                                          : std::numeric_limits<std::int64_t>::min() / 2;

        // Continue a run. The mark rhythm (the spacing of the last two marks) is trusted over the
        // detector when the two disagree by about an octave: YIN can slip an octave down for a few
        // windows on a jittery voice, and following it would skip a mark, which misaligns the
        // overlapping grains (a doubled pulse). A detector period well below the rhythm (a real
        // jump up, or an octave error the other way) is tried first and kept only if it repeats.
        if (c.haveLastMark)
        {
            const double rel = c.markPeriod > 0.0 ? period / c.markPeriod : 1.0;
            const bool tryShort = rel < 0.75 && c.shortTriedFor != c.lastMark;
            const double runPeriod = (rel > 1.6 || (rel < 0.75 && ! tryShort)) ? c.markPeriod : period;

            if (static_cast<double> (newest - c.lastMark) <= 3.0 * runPeriod)
            {
                const double runTolerance = c_markTolerance * runPeriod;
                const double predicted = static_cast<double> (c.lastMark) + runPeriod;
                if (static_cast<double> (newest) < predicted + runTolerance + 2.0)
                    return;

                std::int64_t best = 0;
                float value = 0.0f;
                const auto from = static_cast<std::int64_t> (std::ceil (predicted - runTolerance));
                const auto to = static_cast<std::int64_t> (std::floor (predicted + runTolerance));

                // The detector judges a window ~20 ms old and lets go late, so it still says voiced
                // well into a following fricative. A mark only counts if the period it closes
                // resembles the one before; otherwise the run ends and that noise goes to the
                // resampling path, not a buzz.
                const bool peakFound = searchPeak (from, to, predicted, runTolerance, best, value);
                bool locked = false;
                const double cycleLag = peakFound ? similarityLag (c.lastFine, runPeriod, 1.0, 0.0, runPeriod, c_markLagRange, locked) : 0.0;
                bool accept = peakFound && markCorrelation (best, c.lastMark) >= threshold;
                if (! accept && locked)
                {
                    const auto previous = static_cast<std::int64_t> (std::llround (c.lastFine));
                    const auto chained = static_cast<std::int64_t> (std::llround (c.lastFine + cycleLag));
                    accept = markCorrelation (chained, previous) >= std::max (threshold, c_markChainCorrelation);
                }

                if (! accept)
                {
                    if (tryShort)
                    {
                        c.shortTriedFor = c.lastMark;   // fall back to the rhythm for this mark
                        return;
                    }
                    c.haveLastMark = false;
                    return;
                }

                // The peak proposes the cycle; the mark itself goes one best-matching lag after
                // the previous mark, so consecutive grains are cut at the same place in the
                // waveform to a fraction of a sample.
                double fine = static_cast<double> (best);
                if (locked)
                {
                    bool bracketed = false;
                    const double pulseLag = similarityLag (c.lastFine, runPeriod, c_markPulseBefore, c_markPulseAfter, cycleLag, c_markPulseRange, bracketed);
                    const double chained = c.lastFine + (bracketed ? pulseLag : cycleLag);
                    const double apart = static_cast<double> (best) - chained;
                    if (std::abs (apart) <= c_markChainReach * runPeriod)
                        fine = chained + c_markPeakLeak * apart;
                }

                c.markPeriod = static_cast<double> (best - c.lastMark);
                pushMark (fine, static_cast<float> (runPeriod));
                c.lastMark = best;
                c.lastFine = fine;
                return;
            }
        }

        // Start of a voiced run: the strongest peak in the last-but-one half period..1.5 periods,
        // then walk back towards the onset so the first grains of the vowel are voiced too.
        // A failed start is retried half a period later, not on every sample.
        if (newest < c.startRetry)
            return;
        c.startRetry = newest + static_cast<std::int64_t> (0.5 * period);

        auto from = newest - static_cast<std::int64_t> (std::lround (1.5 * period));
        const auto to = newest - static_cast<std::int64_t> (std::lround (0.5 * period));
        from = std::max (from, newestMark + static_cast<std::int64_t> (0.5 * period));

        {
            float hi = 0.0f, lo = 0.0f;
            for (std::int64_t t = from; t <= to; ++t)
            {
                const float s = m_band[static_cast<size_t> (t & m_mask)];
                hi = std::max (hi, s);
                lo = std::min (lo, s);
            }
            c.polarity = -lo > hi ? -1.0f : 1.0f;
            c.positivePeaks = hi;
            c.negativePeaks = -lo;
        }

        std::int64_t first = 0;
        float firstValue = 0.0f;
        if (! searchPeak (from, to, 0.0, 0.0, first, firstValue) || firstValue < c_minMarkLevel)
            return;

        // The run only starts where the signal really repeats.
        {
            const double predicted = static_cast<double> (first) - period;
            std::int64_t previous = 0;
            float value = 0.0f;
            const float savedPolarity = c.polarity, savedPositive = c.positivePeaks, savedNegative = c.negativePeaks;
            const bool ok = searchPeak (static_cast<std::int64_t> (std::ceil (predicted - tolerance)),
                                        static_cast<std::int64_t> (std::floor (predicted + tolerance)),
                                        predicted, tolerance, previous, value);
            c.polarity = savedPolarity;
            c.positivePeaks = savedPositive;
            c.negativePeaks = savedNegative;
            if (! ok || markCorrelation (first, previous) < threshold)
                return;
        }

        std::array<std::int64_t, 8> back {};
        int numBack = 0;
        const std::int64_t outPos = newest - m_psolaLatency;
        const std::int64_t limit = std::max (newestMark + static_cast<std::int64_t> (0.5 * period),
                                             outPos + static_cast<std::int64_t> (period));
        std::int64_t mark = first;

        while (numBack < static_cast<int> (back.size()))
        {
            const double predicted = static_cast<double> (mark) - period;
            const auto bFrom = static_cast<std::int64_t> (std::ceil (predicted - tolerance));
            const auto bTo = static_cast<std::int64_t> (std::floor (predicted + tolerance));
            if (bFrom <= limit)
                break;

            std::int64_t found = 0;
            float value = 0.0f;
            const float savedPolarity = c.polarity;
            const bool ok = searchPeak (bFrom, bTo, predicted, tolerance, found, value);
            c.polarity = savedPolarity;   // the onset decides polarity, not the quiet lead-in
            if (! ok || value < 0.2f * firstValue || markCorrelation (mark, found) < threshold)
                break;

            back[static_cast<size_t> (numBack++)] = found;
            mark = found;
        }

        for (int i = numBack - 1; i >= 0; --i)
            pushMark (static_cast<double> (back[static_cast<size_t> (i)]), static_cast<float> (period));

        pushMark (static_cast<double> (first), static_cast<float> (period));
        c.markPeriod = numBack > 0 ? static_cast<double> (first - back[0]) : period;
        c.lastMark = first;
        c.lastFine = static_cast<double> (first);
        c.haveLastMark = true;
    }

    //==============================================================================
    void PitchModule::scheduleGrains (std::int64_t outPos)
    {
        Core& c = m_core;
        const VoiceInfo& v = m_detector.getVoiceInfo();
        double lead = m_unvoicedHalfWidth;
        if (v.voiced && v.f0Hz > 0.0f)
            lead = std::max (lead, std::min (static_cast<double> (m_maxPeriod), m_fs / static_cast<double> (v.f0Hz)) / c.rate);
        if (c.prevVoiced)
            lead = std::max (lead, c_grainLeadMargin * c.prevHalfWidth);

        // Create each grain a little before its window starts.
        int guard = 0;
        while (c.nextSyn - lead - 1.0 <= static_cast<double> (outPos) && guard++ < 8)
            createGrain (outPos);
    }

    void PitchModule::updateModeRatio (double periodSamples, std::int64_t outPos)
    {
        // Snap / Mono: the target ratio from the local input period, glided with tau 5 ms at the
        // grain rate (exact for any spacing of the updates).
        Core& c = m_core;
        if (c.mode == Params::PitchMode::shift || ! (periodSamples > 0.0))
            return;

        const double fIn = m_fs / periodSamples;
        double target = c_ln2 * c.knobSt / 12.0;

        if (c.mode == Params::PitchMode::snap)
        {
            const double mIn = 69.0 + 12.0 * std::log2 (fIn / 440.0);
            const double want = mIn + c.knobSt;
            if (! c.haveSnapNote || std::abs (want - c.snapNote) > 0.5 + c_snapHysteresisSt)
            {
                c.snapNote = static_cast<int> (std::lround (want));
                c.haveSnapNote = true;
            }
            target = c_ln2 * (c.snapNote - mIn) / 12.0;
        }
        else
        {
            const double ft = c_monoBaseHz * std::exp2 (c.knobSt / 12.0);
            const double r = ft / fIn;
            c.monoInRange = r >= c_monoMinRatio && r <= c_monoMaxRatio;
            if (c.monoInRange)
                target = std::log (r);
        }

        c.modeTarget = target;
        const double elapsed = std::max (1.0, static_cast<double> (outPos - c.modeUpdatePos));
        const double k = 1.0 - std::exp (-elapsed / m_slewTauSamples);
        c.logRatio += k * (c.modeTarget - c.logRatio);
        c.modeUpdatePos = outPos;
        c.ratio = std::exp (c.logRatio);
        updateRate();
    }

    void PitchModule::createGrain (std::int64_t outPos)
    {
        Core& c = m_core;
        const double now = static_cast<double> (outPos);
        double syn = c.nextSyn;

        // Nearest known analysis mark to the synthesis point.
        int nearest = -1;
        for (int i = c.markCount - 1; i >= 0; --i)
        {
            const int idx = (c.markHead + i) % c_maxMarks;
            if (static_cast<double> (c.marks[static_cast<size_t> (idx)].pos) <= syn)
            {
                nearest = i;
                if (i + 1 < c.markCount)
                {
                    const int next = (c.markHead + i + 1) % c_maxMarks;
                    if (static_cast<double> (c.marks[static_cast<size_t> (next)].pos) - syn
                        < syn - static_cast<double> (c.marks[static_cast<size_t> (idx)].pos))
                        nearest = i + 1;
                }
                break;
            }
        }
        if (nearest < 0 && c.markCount > 0)
            nearest = 0;

        bool voiced = false;
        Mark mark {};
        if (nearest >= 0)
        {
            mark = c.marks[static_cast<size_t> ((c.markHead + nearest) % c_maxMarks)];
            const double distance = syn - static_cast<double> (mark.pos);
            const bool newest = nearest == c.markCount - 1;
            if (std::abs (distance) <= 1.5 * static_cast<double> (mark.period))
                voiced = true;
            else if (newest && distance > 0.0 && distance <= 3.0 * static_cast<double> (mark.period) && m_detector.getVoiceInfo().voiced)
                voiced = true;   // marks lag behind at low pitch: borrow the latest one
        }

        if (voiced)
            updateModeRatio (std::clamp (localPeriod (nearest), static_cast<double> (m_minPeriod), static_cast<double> (m_maxPeriod)), outPos);

        // Shape and place the voiced grain. It may never read input that has not arrived yet: the
        // read position leads real time most at the grain's start (phi < 1) or end (phi > 1), so
        // check that and step back a period (to the previous mark) if needed.
        double period = 0.0, halfWidth = 0.0, voicedSyn = syn;
        while (voiced)
        {
            period = std::clamp (localPeriod (nearest), static_cast<double> (m_minPeriod), static_cast<double> (m_maxPeriod));
            halfWidth = period / c.rate;
            voicedSyn = c.prevVoiced ? syn : c.prevSyn + 0.5 * (c.prevHalfWidth + halfWidth);
            voicedSyn = std::max (voicedSyn, now + halfWidth);

            const double lead = static_cast<double> (mark.pos) - voicedSyn;
            if (lead <= m_psolaLatency - halfWidth * std::abs (1.0 - c.rate) - c_readAhead)
                break;

            if (nearest == 0)
            {
                voiced = false;
                break;
            }

            --nearest;
            mark = c.marks[static_cast<size_t> ((c.markHead + nearest) % c_maxMarks)];
            if (voicedSyn - static_cast<double> (mark.pos) > 3.0 * static_cast<double> (mark.period))
                voiced = false;
        }

        if (c.grainCount >= c_maxGrains)
        {
            // Cannot happen with the clamps above (overlap is at most ~16); keep time moving.
            c.nextSyn += voiced ? static_cast<double> (mark.period) / c.ratio : m_unvoicedHalfWidth;
            return;
        }

        Grain g;
        g.rate = c.rate;

        if (voiced)
        {
            g.halfWidth = halfWidth;
            syn = voicedSyn;
            g.anaCentre = static_cast<double> (mark.pos) + static_cast<double> (mark.frac);
            // Grains placed ratio times as often, each 1 / phi times longer: power goes as
            // ratio / phi, so the gain is sqrt (phi / ratio) (exactly 1 in Follow).
            g.gain = static_cast<float> (std::sqrt (c.rate / c.ratio));
            g.voiced = true;
            c.nextSyn = syn + period / c.ratio;
            ++c.voicedGrains;
        }
        else
        {
            g.halfWidth = m_unvoicedHalfWidth;
            if (c.prevVoiced)
                syn = c.prevSyn + 0.5 * (c.prevHalfWidth + g.halfWidth);
            syn = std::max (syn, now + g.halfWidth);

            // Unvoiced grains carry no audio of their own: their Hann windows (50 % overlap, sum
            // one) weight the resampling path (readTape), so they only crossfade between it and
            // the voiced grains.
            g.anaCentre = syn;
            g.gain = 1.0f;
            g.voiced = false;
            c.nextSyn = syn + m_unvoicedHalfWidth;
            ++c.unvoicedGrains;
        }

        g.synCentre = syn;
        g.invHalfWidth = 1.0 / g.halfWidth;
        c.grains[static_cast<size_t> (c.grainCount++)] = g;

        c.prevSyn = syn;
        c.prevHalfWidth = g.halfWidth;
        c.prevVoiced = voiced;
    }

    double PitchModule::localPeriod (int index) const
    {
        // Mean spacing of up to two marks either side: the period at this grain, with the
        // few-sample wander of the peak positions averaged out over up to four cycles.
        const Core& c = m_core;
        const Mark& centre = c.marks[static_cast<size_t> ((c.markHead + index) % c_maxMarks)];
        const auto nominal = static_cast<double> (centre.period);
        int lo = index, hi = index;

        const auto spacingOk = [&] (int a, int b)
        {
            const double d = static_cast<double> (c.marks[static_cast<size_t> ((c.markHead + b) % c_maxMarks)].pos
                                                  - c.marks[static_cast<size_t> ((c.markHead + a) % c_maxMarks)].pos);
            return d > 0.6 * nominal && d < 1.4 * nominal;
        };

        for (int k = 0; k < 2; ++k)
        {
            if (lo - 1 >= 0 && spacingOk (lo - 1, lo))
                --lo;
            if (hi + 1 < c.markCount && spacingOk (hi, hi + 1))
                ++hi;
        }

        if (hi == lo)
            return nominal;

        const Mark& last = c.marks[static_cast<size_t> ((c.markHead + hi) % c_maxMarks)];
        const Mark& first = c.marks[static_cast<size_t> ((c.markHead + lo) % c_maxMarks)];
        const double span = static_cast<double> (last.pos - first.pos) + static_cast<double> (last.frac) - static_cast<double> (first.frac);
        return span / (hi - lo);
    }

    float PitchModule::renderGrains (std::int64_t outPos, float& right, bool stereo)
    {
        Core& c = m_core;
        const double now = static_cast<double> (outPos);
        float yl = 0.0f, yr = 0.0f;
        float voicedWeight = 0.0f, totalWeight = 0.0f;

        for (int i = 0; i < c.grainCount;)
        {
            const Grain& g = c.grains[static_cast<size_t> (i)];
            const double d = now - g.synCentre;

            if (d >= g.halfWidth)
            {
                c.grains[static_cast<size_t> (i)] = c.grains[static_cast<size_t> (c.grainCount - 1)];
                --c.grainCount;
                continue;
            }

            if (d > -g.halfWidth)
            {
                const float w = window (d * g.invHalfWidth);
                totalWeight += w;
                if (g.voiced)
                {
                    const double pos = g.anaCentre + d * g.rate;
                    const float gw = g.gain * w;
                    float sr = 0.0f;
                    yl += gw * readRings (pos, g.rate, sr, stereo);
                    yr += gw * sr;
                    voicedWeight += w;
                }
            }

            ++i;
        }

        // Where no voiced grain covers the output, the resampling path does.
        const float tapeWeight = totalWeight - voicedWeight;
        float tl = 0.0f, tr = 0.0f;
        readTape (now, tapeWeight > 0.0f, tl, tr, stereo);
        yl += tapeWeight * tl;
        yr += tapeWeight * tr;

        const double share = totalWeight > 1.0e-6f ? static_cast<double> (voicedWeight / totalWeight) : 0.0;
        c.voicedShare += m_shareCoeff * (share - c.voicedShare);
        right = yr;
        return yl;
    }

    float PitchModule::readRings (double pos, double rate, float& right, bool stereo) const
    {
        // Windowed-sinc interpolation, h (t) = sinc (t / s) w (t / (16 s)) with s = max (1, phi):
        // at phi <= 1 (the grains read slower than the input, the case of every downward shift)
        // this reconstructs the input band: content below 0.41 fs images more than 80 dB down,
        // only content in the top 0.41..0.5 fs (the kernel's transition band, -6 dB at fs / 2)
        // leaves weaker images; at phi > 1 it also band-limits to fs / (2 phi), so nothing
        // folds back. The taps are normalised to sum to one (exact DC at any fraction; at an
        // integer position and phi <= 1 the read is the input sample itself).
        const SincTable& table = sincTable();
        const double fl = std::floor (pos);
        const auto base = static_cast<std::int64_t> (fl);
        const double frac = pos - fl;

        if (rate <= 1.0)
        {
            // Polyphase rows, linear between neighbouring fractions (the rows sum to one, so does the mix).
            const double fp = frac * c_sincResolution;
            const int p = std::min (static_cast<int> (fp), c_sincResolution - 1);
            const auto a = static_cast<float> (fp - p);
            const auto& r0 = table.phase[static_cast<size_t> (p)];
            const auto& r1 = table.phase[static_cast<size_t> (p + 1)];
            std::array<float, c_sincTaps> taps;
            for (int k = 0; k < c_sincTaps; ++k)
                taps[static_cast<size_t> (k)] = r0[static_cast<size_t> (k)] + a * (r1[static_cast<size_t> (k)] - r0[static_cast<size_t> (k)]);

            const auto start = static_cast<size_t> ((base + 1 - c_sincZeros) & m_mask);
            const auto dot = [&] (const std::vector<float>& ring)
            {
                float y = 0.0f;
                if (start + c_sincTaps <= ring.size())
                {
                    const float* x = ring.data() + start;
                    for (int k = 0; k < c_sincTaps; ++k)
                        y += taps[static_cast<size_t> (k)] * x[k];
                }
                else
                {
                    for (int k = 0; k < c_sincTaps; ++k)
                        y += taps[static_cast<size_t> (k)] * ring[(start + static_cast<size_t> (k)) & static_cast<size_t> (m_mask)];
                }
                return y;
            };
            right = stereo ? dot (m_inR) : 0.0f;
            return dot (m_inL);
        }

        const double stretch = std::min (rate, c_maxRate);
        const double step = static_cast<double> (c_sincResolution) / stretch;
        const int half = std::min (c_sincMaxHalf, static_cast<int> (std::ceil (c_sincZeros * stretch - 1.0e-9)));

        std::array<float, 2 * c_sincMaxHalf> taps;
        float sum = 0.0f;
        const int count = 2 * half;
        for (int j = 0; j < count; ++j)
        {
            const double u = std::abs (static_cast<double> (j + 1 - half) - frac) * step;
            const int idx = static_cast<int> (u);
            float h = 0.0f;
            if (idx < c_sincLength)
                h = table.value[static_cast<size_t> (idx)] + static_cast<float> (u - idx) * table.slope[static_cast<size_t> (idx)];
            taps[static_cast<size_t> (j)] = h;
            sum += h;
        }
        const float norm = sum > 0.5f ? 1.0f / sum : 1.0f;

        const std::int64_t first = base + 1 - half;
        float l = 0.0f, r = 0.0f;
        for (int j = 0; j < count; ++j)
            l += taps[static_cast<size_t> (j)] * m_inL[static_cast<size_t> ((first + j) & m_mask)];
        if (stereo)
            for (int j = 0; j < count; ++j)
                r += taps[static_cast<size_t> (j)] * m_inR[static_cast<size_t> ((first + j) & m_mask)];
        right = r * norm;
        return l * norm;
    }

    void PitchModule::readTape (double now, bool needed, float& left, float& right, bool stereo)
    {
        // The resampling path for everything the voiced grains do not cover (consonants, breaths,
        // irregular voice): two read heads half a cycle apart sweep +-c_tapeSeconds / 2 around
        // the output time, so each reads the input at the formant rate phi for a long stretch
        // (c_tapeSeconds / |1 - phi|, 180 ms at -2 st) and they crossfade (sin^2) where they wrap.
        // A long continuous read keeps the fine structure of the input, which short grains at a
        // foreign rate would smear. The sweep narrows to 0 as phi -> 1, where the path is the
        // aligned input. The heads read the same input up to half the sweep apart, so their sum
        // is power-normalised (exact for wideband content); near phi = 1 they read the same
        // samples and the normalisation fades back to a plain amplitude crossfade. The
        // normalisation follows the correlation of the heads (see below).
        Core& c = m_core;
        const double deviation = 1.0 - c.rate;
        const double share = std::min (1.0, std::abs (deviation) / c_tapeFullDeviation);
        const double sweep = c_tapeSeconds * m_fs;
        if (share > 0.0)
            c.tapePhase += deviation / (sweep * share);
        c.tapePhase -= std::floor (c.tapePhase);
        if (! needed)
            return;   // the heads keep moving, so the path is ready whenever it is needed

        const double width = sweep * share;
        std::array<float, 2> hl {}, hr {}, w {};
        for (int head = 0; head < 2; ++head)
        {
            double phase = c.tapePhase + 0.5 * head;
            phase -= std::floor (phase);
            const double pos = now + (0.5 - phase) * width;
            w[static_cast<size_t> (head)] = window (2.0 * phase - 1.0);
            hl[static_cast<size_t> (head)] = readRings (pos, c.rate, hr[static_cast<size_t> (head)], stereo);
        }

        // How alike the two heads are (10 ms running correlation): the crossfade keeps the power
        // of the sum for any correlation, so steady tones and periodic sound do not swell by up
        // to 3 dB mid-crossfade (and wideband sound, rho ~ 0, is power-normalised as before).
        // Correlation of the mid ((L + R) / 2 is exactly L when R == L, so mono == stereo L == R).
        const double a = static_cast<double> (stereo ? 0.5f * (hl[0] + hr[0]) : hl[0]);
        const double b = static_cast<double> (stereo ? 0.5f * (hl[1] + hr[1]) : hl[1]);
        c.tapeXY = dsp::flushTiny (c.tapeXY + m_tapeCorrCoeff * (a * b - c.tapeXY));
        c.tapeXX = dsp::flushTiny (c.tapeXX + m_tapeCorrCoeff * (a * a - c.tapeXX));
        c.tapeYY = dsp::flushTiny (c.tapeYY + m_tapeCorrCoeff * (b * b - c.tapeYY));
        const double den = std::sqrt (c.tapeXX * c.tapeYY);
        const float rho = den > 1.0e-20 ? static_cast<float> (std::clamp (c.tapeXY / den, c_tapeMinCorrelation, 1.0)) : 0.0f;

        const float power = w[0] * w[0] + w[1] * w[1] + 2.0f * rho * w[0] * w[1];
        const float norm = 1.0f + static_cast<float> (share) * (1.0f / std::sqrt (std::max (power, 0.25f)) - 1.0f);
        left = (w[0] * hl[0] + w[1] * hl[1]) * norm;
        right = (w[0] * hr[0] + w[1] * hr[1]) * norm;
    }

    float PitchModule::window (double u) const
    {
        // Hann (cos^2 over -1..1): sums to one at 50 % overlap.
        const double a = std::min (1.0, std::abs (u)) * c_windowSize;
        const int i = static_cast<int> (a);
        const float f = static_cast<float> (a - i);
        const float cs = m_cosTable[static_cast<size_t> (i)] + f * (m_cosTable[static_cast<size_t> (i + 1)] - m_cosTable[static_cast<size_t> (i)]);
        return cs * cs;
    }
}
