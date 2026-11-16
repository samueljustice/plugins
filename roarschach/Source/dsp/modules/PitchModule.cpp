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
        // A head that wraps is placed where the input best matches what the other head is reading
        // (within c_tapeAlignBack..c_tapeAlignAhead of its nominal place, compared over
        // +-c_tapeAlignWindow), so the two stay in step on anything that repeats. A match below
        // c_tapeAlignMatch is no match (noise finds 0.3 somewhere almost every time): the head
        // then takes its nominal place. So it does where the band-limited copy holds less than
        // c_tapeAlignBandShare of the input's power: what repeats down there says too little
        // about a sound that lives higher up (a whistle, a sibilant, an ultrasonic tone).
        constexpr double c_tapeAlignBack = 0.009;
        constexpr double c_tapeAlignAhead = 0.005;
        constexpr double c_tapeAlignWindow = 0.004;
        constexpr double c_tapeAlignMatch = 0.7;
        constexpr double c_tapeAlignBandShare = 0.01;
        constexpr double c_tapeAlignBias = 0.1;              // of a match, across the whole range, towards the nominal place
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

        // Level of the overlapped voiced grains. With q = phi / ratio (the grain spacing over the
        // grain's own period), sqrt (q) keeps the power of grains that add at random; a voice's
        // grains do not quite, so on top of it comes a correction fitted to renders of speech,
        // singing and growls (0 dB at q = 1). Grains further apart than their period (q > 1:
        // Keep, downwards) lose a fixed amount once q passes about 1.15; grains closer than
        // their period (q < 1: Size, Keep upwards) lose in proportion to log q at first. Past
        // q = 2 / 3 what is lost depends on the voice (two grains per period add in step on a
        // rich voice and cancel on a pure one), so the correction fades out again by q = 1 / 2.
        constexpr double c_grainApartDb = 0.93;
        constexpr double c_grainApartKnee = 0.075;           // in ln q
        constexpr double c_grainCloseDb = 1.7;
        constexpr double c_grainCloseSlope = 4.9;            // dB per unit of ln (1 / q), near q = 1
        constexpr double c_grainCloseHold = 0.40546510810816438;   // ln (3 / 2): full correction up to here
        constexpr double c_grainCloseEnd = 0.69314718055994531;    // ln 2: none from here
        // Adjacent periods must look alike to count as voiced. Where the resampling path already
        // gives the right pitch and formants (Follow, Size 0), a mark must be convincing: an
        // irregular (creaky) voice is better resampled than cut into misplaced grains. Where it
        // would not (Keep, Size), the grains are the only way to move the pitch, so accept more.
        constexpr double c_markCorrelationStrict = 0.85;
        constexpr double c_markCorrelationLoose = 0.5;
        constexpr double c_strictWithinSt = 0.5, c_looseBeyondSt = 1.5;
        constexpr double c_minHz = 50.0, c_maxHz = 1000.0;

        // The tail and head of a run of marks (a grain is voiced when its synthesis point lies, in
        // periods T of its nearest mark, ...):
        constexpr double c_markInside = 0.5;                  // ... within half a period
        constexpr double c_bridgeSpan = 3.2;                  // ... towards an adjacent mark at most this far away,
        constexpr double c_bridgeReach = 1.5;                 //     up to this far from its own
        constexpr double c_borrowReach = 3.0;                 // ... past the newest mark of a run still open
        constexpr double c_tailReach = 0.75;                  // ... a little past the last mark of a closed run
        // ... or further (c_bridgeReach, c_borrowReach past the newest mark) only where the input
        // still sounds there: the 60..900 Hz band over half a period around the point holds at
        // least c_liveShare of the power of the mark's own cycle, and no onset lies between.
        constexpr double c_liveShare = 0.1;
        constexpr double c_letGoSeconds = 0.003;              // a grain past a run's end fades over this

        // Air (sibilants and fricatives) is left at phi in Keep; everything else that is not
        // marked moves with the pitch. Share of the mid's power below 2.5 kHz (5 ms smoothing):
        // air under c_airOn, not air over c_airOff, held between and below c_airLevelFloor.
        constexpr double c_airLowpassHz = 2500.0;
        constexpr double c_airSmoothSeconds = 0.005;
        constexpr double c_airOn = 0.20, c_airOff = 0.35;
        constexpr double c_airLevelFloor = 1.0e-10;
        constexpr double c_airFadeSeconds = 0.008;

        // The reader (single-head resampling path). A splice crossfades (sin^2) from the previous place
        // to the new over c_spliceSeconds. Nominal travel: c_readerBound x min (1, |1 - rate| / 0.1)
        // either side of the output time. Splices are steered so the head reaches the next onset
        // c_spliceGoalSeconds after the output time does, never move a head across a protected
        // stretch, are not made shorter than c_minJumpSeconds, and are forced only past the hard
        // limits (c_hardBehind behind the output time, c_hardAhead ahead of it).
        constexpr double c_spliceSeconds = 0.004;
        constexpr double c_readerBound = 0.010;
        constexpr double c_spliceGoalSeconds = 0.001;
        constexpr double c_steerTolerance = 0.001;
        constexpr double c_lastChanceSeconds = 0.001;
        constexpr double c_minJumpSeconds = 0.002;
        constexpr double c_hardBehind = 0.020, c_hardAhead = 0.016;

        // Onset detector (input time, on the mid tilted towards the band where bursts live).
        constexpr double c_onsetHopSeconds = 0.002;
        constexpr double c_onsetRise = 7.9432823472428150;    // 9 dB over the loudest of the 8 hops before (16 ms)
        constexpr int c_onsetQuietHops = 8;
        constexpr int c_pulseMemoryHops = 5;                  // pulse memory: 9 dB over the 5 hops before (10 ms)
        constexpr double c_trainFromSeconds = 0.011, c_trainToSeconds = 0.060;
        constexpr double c_trainLike = 7.9432823472428150;    // within 9 dB either way
        constexpr double c_onsetAbsFloor = 1.0e-9, c_onsetRelFloor = 1.0e-5, c_onsetRefFloor = 1.0e-30;
        constexpr double c_onsetEndShare = 0.063;             // -12 dB from its first hop
        constexpr double c_onsetMinSeconds = 0.004, c_onsetMaxSeconds = 0.016;
        constexpr double c_onsetLeadSeconds = 0.002;          // protection starts this far before an onset

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

        /** Gain of a voiced grain for q = phi / ratio (see c_grainApartDb): exactly 1 at q = 1. */
        inline double grainGain (double q)
        {
            const double x = std::log (q);
            double db = 0.0;
            if (x >= 0.0)
            {
                db = c_grainApartDb * (1.0 - std::exp (-std::pow (x / c_grainApartKnee, 1.5)));
            }
            else
            {
                const double fade = std::clamp ((c_grainCloseEnd + x) / (c_grainCloseEnd - c_grainCloseHold), 0.0, 1.0);
                db = c_grainCloseDb * std::tanh (-x * c_grainCloseSlope / c_grainCloseDb) * fade * fade * (3.0 - 2.0 * fade);
            }
            return std::sqrt (q) * std::exp (0.11512925464970229 * db);   // ln (10) / 20
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
        m_onsetTilt = static_cast<float> (std::pow (0.9, 48000.0 / m_fs));
        m_onsetHop = std::max (8, static_cast<int> (std::lround (c_onsetHopSeconds * m_fs)));
        m_letGoStep = static_cast<float> (1.0 / (c_letGoSeconds * m_fs));
        m_airCoeff = dsp::onePoleCoeff (c_airSmoothSeconds, m_fs);
        m_airStep = static_cast<float> (1.0 / (c_airFadeSeconds * m_fs));
        m_onsetRefFall = std::exp (-static_cast<double> (m_onsetHop) / m_fs);   // 4.3 dB per second

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
        c.airLp.setCoeffs (Biquad::lowpass (m_fs, c_airLowpassHz, Biquad::c_butterworthQ));

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

        for (int i = 0; i < n;)
        {
            const int end = voice.frameEnd (i, n);
            for (; i < end; ++i)
            {
                if (((voice.startOffset() + i) & (c_voiceFrameSize - 1)) == 0)
                    recordFrame (voice.firstFrameIndex() + ((voice.startOffset() + i) >> c_voiceFrameShift), active, fadeWeight, fullyOn);

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

    void PitchModule::recordFrame (std::int64_t g, bool active, float fadeWeight, bool fullyOn)
    {
        // What the stream downstream should say for the audio of global frame g: the ratio
        // applied to it, and the Snap / Mono target where the output is clearly the shifted voice.
        Core& c = m_core;
        const auto at = static_cast<size_t> (g & (c_frameRing - 1));
        const double w = active ? static_cast<double> (fadeWeight) * static_cast<double> (c.wet) : 0.0;
        c.ringLogRatio[at] = static_cast<float> (w * c.logRatio);
        float target = 0.0f;
        if (active && fullyOn && c.wet >= 1.0f && c.voicedShare > 0.5)
        {
            if (c.mode == Params::PitchMode::snap && c.haveSnapNote)
                target = static_cast<float> (440.0 * std::exp2 ((c.snapNote - 69) / 12.0));
            else if (c.mode == Params::PitchMode::mono && c.monoInRange)
                target = static_cast<float> (c_monoBaseHz * std::exp2 (c.knobSt / 12.0));
        }
        c.ringTargetHz[at] = target;
    }

    void PitchModule::rewriteVoice (VoiceFrame* frames, int numFrames, std::int64_t firstFrame, int skewSamples)
    {
        if (frames == nullptr)
            return;

        // Frame firstFrame + k describes the audio skewSamples before that output frame: it takes
        // what was recorded for the output frame that many samples earlier.
        const std::int64_t lag = std::clamp ((skewSamples + c_voiceFrameSize / 2) >> c_voiceFrameShift, 0, c_maxRewriteLag);
        const Core& c = m_core;
        for (int k = 0; k < numFrames; ++k)
        {
            VoiceFrame& f = frames[k];
            const std::int64_t g = firstFrame + k - lag;
            if (! (f.f0Hz > 0.0f) || g < 0)
                continue;

            const auto at = static_cast<size_t> (g & (c_frameRing - 1));
            const float target = c.ringTargetHz[at];
            if (target > 0.0f && f.voiced)
                f.f0Hz = target;
            else
                f.f0Hz *= std::exp (c.ringLogRatio[at]);
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
        c.airMix = airTarget (c.writePos - 1 - m_psolaLatency);   // no fade from a stale value
        for (auto& rd : c.readers)
        {
            rd.started = false;   // each reader starts again on the output time
            rd.xfLeft = 0;
        }

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

        // Air: how much of the mid's power sits below 2.5 kHz, stored once per frame.
        {
            const double lo = static_cast<double> (c.airLp.process (mid));
            c.airLo = dsp::flushTiny (c.airLo + m_airCoeff * (lo * lo - c.airLo));
            c.airAll = dsp::flushTiny (c.airAll + m_airCoeff * (static_cast<double> (mid) * static_cast<double> (mid) - c.airAll));
            if (c.airAll > c_airLevelFloor)
            {
                const double share = c.airLo / c.airAll;
                if (share < c_airOn)
                    c.airNow = true;
                else if (share > c_airOff)
                    c.airNow = false;
            }
            if ((n & (c_voiceFrameSize - 1)) == 0)
                c.airRing[static_cast<size_t> ((n >> c_voiceFrameShift) & (c_airRing - 1))] = c.airNow ? 1 : 0;
        }

        detectOnsets (mid);
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
        // The curve rests at its bias (envelope 0): its smoothing memory starts there, so the
        // curve's output is exactly 0 on silence from the first sample.
        for (auto& ch : m_driveCh)
            ch.previous = static_cast<double> (c_biasStatic * m_core.driveBiasScale);
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

    void PitchModule::closeRun()
    {
        // The run of marks ends here. Grains already placed from its last mark further than
        // c_tailReach periods past it would repeat the last cycle into whatever follows: they let
        // go, unless the input still sounds where they play.
        Core& c = m_core;
        if (! c.haveLastMark)
            return;
        c.haveLastMark = false;
        const double period = c.markPeriod > 0.0 ? c.markPeriod : static_cast<double> (m_maxPeriod);
        const double limit = c.lastFine + c_tailReach * period;
        for (int i = 0; i < c.grainCount; ++i)
        {
            Grain& g = c.grains[static_cast<size_t> (i)];
            if (g.voiced && g.anaCentre >= c.lastFine - 0.5 && g.synCentre > limit
                && ! (g.synCentre <= c.lastFine + c_borrowReach * period && liveAt (g.synCentre, c.lastFine, period)))
                g.fadeStep = m_letGoStep;
        }
    }

    double PitchModule::bandPower (std::int64_t from, std::int64_t to) const
    {
        // Mean square of the 60..900 Hz copy over [from, to].
        if (to < from)
            return 0.0;
        double sum = 0.0;
        for (std::int64_t t = from; t <= to; ++t)
        {
            const auto v = static_cast<double> (m_band[static_cast<size_t> (t & m_mask)]);
            sum += v * v;
        }
        return sum / static_cast<double> (to - from + 1);
    }

    bool PitchModule::protectedNear (double lo, double hi) const
    {
        // Does the protected stretch of an onset ([pos - 2 ms, end], the end "everything after"
        // until it is known) overlap [lo, hi]?
        const double lead = c_onsetLeadSeconds * m_fs;
        for (const Onset& o : m_core.onsets)
            if (o.pos >= 0 && static_cast<double> (o.pos) - lead <= hi && (o.end < 0 || static_cast<double> (o.end) >= lo))
                return true;
        return false;
    }

    bool PitchModule::liveAt (double syn, double markPos, double period) const
    {
        // Is the input still sounding where a grain past the end of a run would play? Band power
        // over half a period around that point against the mark's own cycle; never across an
        // onset (what follows is another sound, not this voice).
        const std::int64_t newest = m_core.writePos - 1;
        const auto a = static_cast<std::int64_t> (std::floor (syn - 0.25 * period));
        const auto b = std::min (newest, static_cast<std::int64_t> (std::floor (syn + 0.25 * period)));
        if (static_cast<double> (b - a) < 0.25 * period)
            return false;   // the input there has not arrived
        if (protectedNear (std::min (syn, markPos) - 0.5 * period, std::max (syn, markPos) + 0.5 * period))
            return false;
        const auto m = static_cast<std::int64_t> (markPos);
        const auto half = static_cast<std::int64_t> (0.5 * period);
        return bandPower (a, b) >= c_liveShare * bandPower (m - half + 1, m + half);
    }

    void PitchModule::detectOnsets (float mid)
    {
        // Onsets for the splices and the tail rule, found at input time (spec: bursts once, not
        // doubled or smeared). Energy of the tilted mid per 2 ms hop; an onset is a hop 9 dB over
        // the loudest of the 16 ms before it, unless it is one pulse of a train (the last pulse
        // remembered 11 to 60 ms back, within 9 dB): a slow train is one onset, its first pulse.
        Core& c = m_core;
        const std::int64_t n = c.writePos;   // this sample's index
        const float d = mid - m_onsetTilt * c.onsetPrev;
        c.onsetPrev = mid;
        c.hopSum += static_cast<double> (d) * static_cast<double> (d);
        if (++c.hopCount < m_onsetHop)
            return;

        const double e = c.hopSum / m_onsetHop;
        c.hopSum = 0.0;
        c.hopCount = 0;
        const std::int64_t hopStart = n - m_onsetHop + 1;
        const bool floors = e > c_onsetAbsFloor && e > c_onsetRelFloor * c.onsetRef;

        if (c.onsetOpen)
        {
            Onset& o = c.onsets[static_cast<size_t> ((c.onsetWrite + c_maxOnsets - 1) % c_maxOnsets)];
            const std::int64_t since = hopStart - o.pos;
            if ((e < c.onsetEndLevel && since >= static_cast<std::int64_t> (c_onsetMinSeconds * m_fs))
                || since >= static_cast<std::int64_t> (c_onsetMaxSeconds * m_fs))
            {
                o.end = hopStart;
                c.onsetOpen = false;
            }
        }
        else if (floors)
        {
            double before = 0.0;
            for (double h : c.hopEnergy)
                before = std::max (before, h);
            if (e > c_onsetRise * before)
            {
                const std::int64_t since = hopStart - c.pulsePos;
                const bool train = since >= static_cast<std::int64_t> (c_trainFromSeconds * m_fs)
                                && since <= static_cast<std::int64_t> (c_trainToSeconds * m_fs)
                                && e <= c_trainLike * c.pulseEnergy && e * c_trainLike >= c.pulseEnergy;
                if (! train)
                {
                    Onset& o = c.onsets[static_cast<size_t> (c.onsetWrite)];
                    o.pos = hopStart;
                    o.end = -1;
                    c.onsetWrite = (c.onsetWrite + 1) % c_maxOnsets;
                    c.onsetEndLevel = e * c_onsetEndShare;
                    c.onsetOpen = true;
                    ++c.onsetCount;
                }
            }
        }

        // Pulse memory, on every hop: a hop 9 dB over the 10 ms before it.
        if (floors)
        {
            double recent = 0.0;
            for (int j = 1; j <= c_pulseMemoryHops; ++j)
                recent = std::max (recent, c.hopEnergy[static_cast<size_t> ((c.hopIndex - j + 8) & 7)]);
            if (e > c_onsetRise * recent)
            {
                c.pulsePos = hopStart;
                c.pulseEnergy = e;
            }
        }
        c.onsetRef = std::max ({ e, c.onsetRef * m_onsetRefFall, c_onsetRefFloor });
        c.hopEnergy[static_cast<size_t> (c.hopIndex)] = e;
        c.hopIndex = (c.hopIndex + 1) & 7;
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
            closeRun();
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
                    closeRun();
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
            const double T = static_cast<double> (mark.period);
            const auto posOf = [&] (int i) { return static_cast<double> (c.marks[static_cast<size_t> ((c.markHead + i) % c_maxMarks)].pos); };
            if (std::abs (distance) <= c_markInside * T)
                voiced = true;
            else if (distance > 0.0)
            {
                // Past the mark: towards the next one (a missed cycle), the newest mark of a run
                // still open (marks lag at low pitch), a little way past a run's end, or further
                // only where the input still sounds there.
                const bool bridge = ! newest && posOf (nearest + 1) - static_cast<double> (mark.pos) <= c_bridgeSpan * T;
                if (bridge && distance <= c_bridgeReach * T)
                    voiced = true;
                else if (newest && c.haveLastMark && distance <= c_borrowReach * T)
                    voiced = true;
                else if (distance <= c_tailReach * T)
                    voiced = true;
                else if (distance <= (newest ? c_borrowReach : c_bridgeReach) * T && liveAt (syn, static_cast<double> (mark.pos), T))
                    voiced = true;
            }
            else
            {
                // Before the mark: only towards an earlier mark close enough to bridge.
                const bool bridge = nearest > 0 && static_cast<double> (mark.pos) - posOf (nearest - 1) <= c_bridgeSpan * T;
                if (bridge && -distance <= c_bridgeReach * T)
                    voiced = true;
            }
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
            // ratio / phi, so the gain is sqrt (phi / ratio) and a small correction for how a
            // voice's grains really add (exactly 1 in Follow).
            g.gain = static_cast<float> (grainGain (c.rate / c.ratio));
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
            Grain& g = c.grains[static_cast<size_t> (i)];
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
                    // A grain that lets go hands its weight to the resampling path over 3 ms.
                    g.fade = std::max (0.0f, g.fade - g.fadeStep);
                    const float gw = g.gain * w * g.fade;
                    if (gw > 0.0f)
                    {
                        const double pos = g.anaCentre + d * g.rate;
                        float sr = 0.0f;
                        yl += gw * readRings (pos, g.rate, sr, stereo);
                        yr += gw * sr;
                    }
                    voicedWeight += w * g.fade;
                }
            }

            ++i;
        }

        // Where no voiced grain covers the output, the resampling path does: the voice read at the
        // Follow rate, crossfaded (8 ms) to the air read at phi where the input is air. Where
        // the two rates are equal (Follow) it is one path and only the air read runs.
        const float tapeWeight = totalWeight - voicedWeight;
        const float target = airTarget (outPos);
        c.airMix = target > c.airMix ? std::min (target, c.airMix + m_airStep) : std::max (target, c.airMix - m_airStep);
        const bool needed = tapeWeight > 0.0f;
        float vl = 0.0f, vr = 0.0f, al = 0.0f, ar = 0.0f;
        readResampled (0, followRate(), now, needed && c.airMix < 1.0f, vl, vr, stereo);
        readResampled (1, c.rate, now, needed && c.airMix > 0.0f, al, ar, stereo);
        const float tl = vl + c.airMix * (al - vl);
        const float tr = vr + c.airMix * (ar - vr);
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

    double PitchModule::alignTapeHead (double other, double nominal, double share, bool stereo, double* matchOut) const
    {
        if (matchOut != nullptr)
            *matchOut = 0.0;
        // The offset from `nominal` (samples, fractional) at which the band-limited input around
        // the wrapping head best matches the input around `other`, the position of the head that
        // is at full weight; 0 where nothing matches. Coarse grid, then single samples, then the
        // vertex of a parabola, as for the marks.
        const int step = std::max (1, static_cast<int> (m_fs / c_markLagGridHz));
        const int half = static_cast<int> (c_tapeAlignWindow * m_fs) / step;
        const auto a = static_cast<std::int64_t> (std::llround (other));
        const auto b = static_cast<std::int64_t> (std::llround (nominal));
        const std::int64_t newest = m_core.writePos - 1;
        const std::int64_t reach = static_cast<std::int64_t> (half) * step;
        const int lo = -static_cast<int> (c_tapeAlignBack * m_fs * share);
        const int hi = static_cast<int> (std::min<std::int64_t> (static_cast<std::int64_t> (c_tapeAlignAhead * m_fs * share), newest - reach - b));
        if (half < 2 || a + reach > newest || hi - lo < 2)
            return 0.0;

        double xx = 1.0e-20, full = 0.0;
        for (int j = -half; j <= half; ++j)
        {
            const auto at = static_cast<size_t> ((a + j * step) & m_mask);
            const auto u = static_cast<double> (m_band[at]);
            const auto x = static_cast<double> (stereo ? 0.5f * (m_inL[at] + m_inR[at]) : m_inL[at]);
            xx += u * u;
            full += x * x;
        }
        if (xx < c_tapeAlignBandShare * full)
            return 0.0;

        const auto match = [&] (int d)
        {
            double xy = 0.0, yy = 1.0e-20;
            for (int j = -half; j <= half; ++j)
            {
                const auto u = static_cast<double> (m_band[static_cast<size_t> ((a + j * step) & m_mask)]);
                const auto v = static_cast<double> (m_band[static_cast<size_t> ((b + d + j * step) & m_mask)]);
                xy += u * v;
                yy += v * v;
            }
            return xy / std::sqrt (xx * yy);
        };

        const double bias = c_tapeAlignBias / static_cast<double> (hi - lo);
        int best = 0;
        double bestScore = -1.0e9, bestMatch = 0.0;
        const auto consider = [&] (int d)
        {
            const double m = match (d);
            const double score = m - bias * std::abs (static_cast<double> (d));
            if (score > bestScore)
            {
                bestScore = score;
                bestMatch = m;
                best = d;
            }
        };

        for (int d = lo; d <= hi; d += step)
            consider (d);
        const int coarse = best;
        for (int d = std::max (lo, coarse - step + 1); d <= std::min (hi, coarse + step - 1); ++d)
            if (d != coarse)
                consider (d);

        if (bestMatch < c_tapeAlignMatch)
            return 0.0;
        if (matchOut != nullptr)
            *matchOut = bestMatch;

        double offset = static_cast<double> (best);
        if (best > lo && best < hi)
        {
            const double below = match (best - 1), above = match (best + 1);
            const double curvature = below - 2.0 * bestMatch + above;
            if (curvature < -1.0e-12)
                offset += std::clamp (0.5 * (below - above) / curvature, -0.5, 0.5);
        }
        return offset;
    }

    double PitchModule::followRate() const
    {
        // The rate phi would have in Follow: the applied shift (the knob in Mono) and Size.
        const Core& c = m_core;
        const double shiftSt = c.mode == Params::PitchMode::mono ? c.knobSt : 12.0 * c.logRatio / c_ln2;
        return std::clamp (std::exp (c_ln2 * (shiftSt - c_sizeFormantSt * c.size) / 12.0), c_minRate, c_maxRate);
    }

    float PitchModule::airTarget (std::int64_t outPos) const
    {
        const Core& c = m_core;
        if (std::abs (followRate() - c.rate) < 1.0e-9)
            return 1.0f;
        const bool air = m_airForTests >= 0 ? m_airForTests > 0
                                            : c.airRing[static_cast<size_t> ((outPos >> c_voiceFrameShift) & (c_airRing - 1))] != 0;
        return air ? 1.0f : 0.0f;
    }

    void PitchModule::readTape (Tape& t, double rate, double now, bool needed, float& left, float& right, bool stereo)
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
        // A head wraps at zero weight, while the other is at full weight: it is placed where the
        // input best matches what the other head reads (alignTapeHead), so on periodic sound the
        // crossfade joins like to like instead of combing, and a head that can start on the very
        // samples the other is reading does so. The offset shrinks with the sweep, so the path
        // is still the aligned input at phi = 1.
        const double deviation = 1.0 - rate;
        const double share = std::min (1.0, std::abs (deviation) / c_tapeFullDeviation);
        const double sweep = c_tapeSeconds * m_fs;
        if (share > 0.0)
            t.phase += deviation / (sweep * share);
        t.phase -= std::floor (t.phase);

        const double width = sweep * share;
        const auto offsetOf = [&] (int head)
        {
            const double chosenAt = t.offsetShare[static_cast<size_t> (head)];
            return chosenAt > 0.0 ? t.offset[static_cast<size_t> (head)] * std::min (1.0, share / chosenAt) : 0.0;
        };

        for (int head = 0; head < 2; ++head)
        {
            double phase = t.phase + 0.5 * head;
            phase -= std::floor (phase);
            if (std::abs (phase - t.lastPhase[static_cast<size_t> (head)]) > 0.5 && share > 0.0)
            {
                // Wrapped: the other head is half a cycle away, at the output time plus its offset.
                const double other = now + offsetOf (1 - head);
                t.offset[static_cast<size_t> (head)] = alignTapeHead (other, now + (0.5 - phase) * width, share, stereo);
                t.offsetShare[static_cast<size_t> (head)] = share;
            }
            t.lastPhase[static_cast<size_t> (head)] = phase;
        }

        if (! needed)
            return;   // the heads keep moving, so the path is ready whenever it is needed

        std::array<float, 2> hl {}, hr {}, w {};
        for (int head = 0; head < 2; ++head)
        {
            double phase = t.phase + 0.5 * head;
            phase -= std::floor (phase);
            const double pos = now + (0.5 - phase) * width + offsetOf (head);
            w[static_cast<size_t> (head)] = window (2.0 * phase - 1.0);
            hl[static_cast<size_t> (head)] = readRings (pos, rate, hr[static_cast<size_t> (head)], stereo);
        }

        // How alike the two heads are (10 ms running correlation): the crossfade keeps the power
        // of the sum for any correlation, so steady tones and periodic sound do not swell by up
        // to 3 dB mid-crossfade (and wideband sound, rho ~ 0, is power-normalised as before).
        // Correlation of the mid ((L + R) / 2 is exactly L when R == L, so mono == stereo L == R).
        const double a = static_cast<double> (stereo ? 0.5f * (hl[0] + hr[0]) : hl[0]);
        const double b = static_cast<double> (stereo ? 0.5f * (hl[1] + hr[1]) : hl[1]);
        t.xy = dsp::flushTiny (t.xy + m_tapeCorrCoeff * (a * b - t.xy));
        t.xx = dsp::flushTiny (t.xx + m_tapeCorrCoeff * (a * a - t.xx));
        t.yy = dsp::flushTiny (t.yy + m_tapeCorrCoeff * (b * b - t.yy));
        const double den = std::sqrt (t.xx * t.yy);
        const float rho = den > 1.0e-20 ? static_cast<float> (std::clamp (t.xy / den, c_tapeMinCorrelation, 1.0)) : 0.0f;

        const float power = w[0] * w[0] + w[1] * w[1] + 2.0f * rho * w[0] * w[1];
        const float norm = 1.0f + static_cast<float> (share) * (1.0f / std::sqrt (std::max (power, 0.25f)) - 1.0f);
        left = (w[0] * hl[0] + w[1] * hl[1]) * norm;
        right = (w[0] * hr[0] + w[1] * hr[1]) * norm;
    }

    void PitchModule::readResampled (int which, double rate, double now, bool needed, float& left, float& right, bool stereo)
    {
        Core& c = m_core;
       #if ROARSCHACH_PITCH_READER
        readReader (c.readers[static_cast<size_t> (which)], rate, now, needed, left, right, stereo);
       #else
        readTape (c.tapes[static_cast<size_t> (which)], rate, now, needed, left, right, stereo);
       #endif
    }

    void PitchModule::decideSplice (Reader& t, double rate, double now, bool stereo)
    {
        // Two rules decide every splice (only while no crossfade runs): 1. arrive at the next onset
        // on time; 2. stay within the nominal travel without reading a protected stretch twice or
        // skipping one, waiting (up to the hard limits) where a jump would.
        Core& c = m_core;
        const double fs = m_fs;
        const int xf = std::max (8, static_cast<int> (std::lround (c_spliceSeconds * fs)));
        const double span = rate * xf;                       // input one crossfade covers
        const double lead = c_onsetLeadSeconds * fs;
        const double newest = static_cast<double> (c.writePos - 1);
        const double share = std::min (1.0, std::abs (1.0 - rate) / c_tapeFullDeviation);
        const double bound = c_readerBound * fs * share;
        const double lag = now - t.pos;
        const auto protEnd = [&] (const Onset& o) { return o.end >= 0 ? static_cast<double> (o.end) : newest + 1.0e9; };
        const auto start = [&] (double target, float join)
        {
            t.oldPos = t.pos;
            t.pos = target;
            t.xfLen = t.xfLeft = xf;
            t.join = join;
            ++c.splices;
        };

        // 1. Arrive at the next onset on time: the place from which a head moving at `rate`
        // reaches it 1 ms after the output time does.
        const Onset* next = nullptr;
        double prevEnd = -1.0e18;
        for (const Onset& o : c.onsets)
        {
            if (o.pos < 0)
                continue;
            if (static_cast<double> (o.pos) - lead > t.pos)
            {
                if (next == nullptr || o.pos < next->pos)
                    next = &o;
            }
            else
                prevEnd = std::max (prevEnd, protEnd (o));
        }
        if (next != nullptr)
        {
            const double edge = static_cast<double> (next->pos) - lead;
            const double toOnset = static_cast<double> (next->pos) - now;
            const double goal = c_spliceGoalSeconds * fs;
            double target = toOnset + goal > 0.0 ? static_cast<double> (next->pos) - rate * (toOnset + goal) : edge - span - 2.0;
            target = std::min (target, edge - span - 1.0);
            if (std::abs (t.pos - target) <= c_steerTolerance * fs)
                return;   // on its way to the onset: nothing may move it now
            const bool outside = lag > bound + 0.5 || lag < -bound - 0.5;
            if (next->pos != t.handledOnset
                && (outside || std::min (edge - t.pos, edge - target) - span <= rate * (xf + c_lastChanceSeconds * fs)))
            {
                t.handledOnset = next->pos;
                if (t.pos >= prevEnd && target >= prevEnd && edge - t.pos - span >= 0.0 && target + span + c_readAhead < newest)
                {
                    start (target, 0.0f);
                    ++c.steered;
                    return;
                }
            }
        }

        // 2. Stay in bounds.
        const bool behind = lag > bound + 0.5, ahead = lag < -bound - 0.5;
        if (! behind && ! ahead)
            return;
        double target = behind ? now + bound : now - bound;
        const bool hard = lag > c_hardBehind * fs || lag < -c_hardAhead * fs;
        bool blocked = false;
        if (target > t.pos)
        {
            // Forward: nothing protected may be skipped or cut by a crossfade.
            const Onset* first = nullptr;
            for (const Onset& o : c.onsets)
                if (o.pos >= 0 && protEnd (o) > t.pos && static_cast<double> (o.pos) - lead < target + span)
                    if (first == nullptr || o.pos < first->pos)
                        first = &o;
            if (first != nullptr)
            {
                const double edge = static_cast<double> (first->pos) - lead;
                if (t.pos + span >= edge)
                    blocked = true;
                else
                    target = std::min (target, edge - span - 1.0);
            }
        }
        else
        {
            // Back: nothing protected may be read twice.
            for (const Onset& o : c.onsets)
                if (o.pos >= 0 && static_cast<double> (o.pos) - lead <= t.pos + span && protEnd (o) >= target)
                {
                    if (protEnd (o) >= t.pos)
                        blocked = true;
                    else
                        target = std::max (target, protEnd (o) + 1.0);
                }
        }
        // A jump shorter than 2 ms is not made (where the travel itself is longer than 1 ms:
        // near rate 1 every jump is short).
        if (std::abs (target - t.pos) < c_minJumpSeconds * fs && bound > 0.001 * fs)
            blocked = true;
        if (blocked)
        {
            if (! hard)
                return;
            target = behind ? now + bound : now - bound;
            ++c.forced;
        }
        target = std::min (target, newest - span - c_readAhead - 1.0);

        // Where the waveform matches, the head lands there, carrying its fraction of a sample
        // (the match is found between whole samples), so periodic sound stays periodic.
        double match = 0.0;
        const double offset = alignTapeHead (t.pos, target, share, stereo, &match);
        double landing = target;
        if (offset != 0.0 && ! blocked)
        {
            landing = std::round (target) + offset + (t.pos - std::round (t.pos));
            bool ok = landing + span + c_readAhead < newest;
            for (const Onset& o : c.onsets)
                if (o.pos >= 0)
                {
                    const double lo = static_cast<double> (o.pos) - lead, hi = protEnd (o);
                    if (std::min (t.pos, landing) < hi && std::max (t.pos, landing) + span > lo)
                        ok = false;
                }
            if (! ok)
            {
                landing = target;
                match = 0.0;
            }
        }
        else
            match = 0.0;
        start (landing, match > 0.0 ? 1.0f : 0.0f);
    }

    void PitchModule::readReader (Reader& t, double rate, double now, bool needed, float& left, float& right, bool stereo)
    {
        // The resampling path as one read head at `rate` (see decideSplice for where it jumps).
        // At rate 1 the travel is 0 and the head sits on the output time: the aligned input.
        Core& c = m_core;
        if (! t.started)
        {
            t.pos = now;
            t.started = true;
        }
        if (t.xfLeft == 0)
            decideSplice (t, rate, now, stereo);

        if (needed)
        {
            const double newest = static_cast<double> (c.writePos - 1);
            if (std::max (t.pos, t.xfLeft > 0 ? t.oldPos : t.pos) + c_readAhead > newest + 1.0e-6)
                ++c.readsPastNewest;
            float r1 = 0.0f;
            const float l1 = readRings (t.pos, rate, r1, stereo);
            if (t.xfLeft > 0)
            {
                float r0 = 0.0f;
                const float l0 = readRings (t.oldPos, rate, r0, stereo);
                const double u = 1.0 - static_cast<double> (t.xfLeft) / static_cast<double> (t.xfLen);
                const float w1 = 1.0f - window (u);   // sin^2 (pi u / 2)
                const float w0 = 1.0f - w1;
                // Like joined to like: a plain crossfade; unrelated sound keeps its power.
                const float power = w0 * w0 + w1 * w1 + 2.0f * t.join * w0 * w1;
                const float norm = 1.0f / std::sqrt (std::max (power, 0.25f));
                left = (w0 * l0 + w1 * l1) * norm;
                right = (w0 * r0 + w1 * r1) * norm;
            }
            else
            {
                left = l1;
                right = r1;
            }
        }
        t.pos += rate;
        if (t.xfLeft > 0)
        {
            t.oldPos += rate;
            --t.xfLeft;
        }
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
