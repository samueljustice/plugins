#include "OutputModule.h"

#include "../KnobLaws.h"

#include "../MonsterFeatures.h"
#include "../common/GainComputer.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_smoothSeconds = 0.020 / 4.6;     // Push, Ceiling, Mix, Clip, Shape settle within 20 ms

        // Auto gain
        constexpr double c_meterSeconds = 0.400;
        constexpr double c_glideSeconds = 0.400;
        constexpr double c_learnAboveDb = -50.0;
        constexpr double c_maxBoostDb = 6.0, c_maxCutDb = -12.0;
        constexpr double c_guardAboveCeilingDb = 3.0;
        constexpr double c_peakReleaseSeconds = 1.0;
        constexpr double c_weightHpHz = 80.0, c_wetHpHz = 20.0, c_wetLowShelfHz = 80.0, c_wetLowShelfDb = -6.0;
        constexpr double c_weightShelfHz = 1500.0, c_weightShelfDb = 4.0;

        // Ride
        constexpr double c_rideRmsSeconds = 0.010;
        constexpr double c_rideThresholdDb = -8.0, c_rideRatio = 10.0, c_rideKneeDb = 6.0, c_rideDepthDb = -12.0;
        constexpr double c_rideAttackSeconds = 0.003, c_rideReleaseSeconds = 0.150;

        // Maximizer
        constexpr double c_lookaheadSeconds = 0.010;
        constexpr double c_minAttackMs = 0.2, c_maxAttackMs = 10.0;
        constexpr double c_minReleaseMs = 1.0, c_maxReleaseMs = 1000.0;
        constexpr double c_dcBlockHz = 2.5;
        constexpr double c_autoSlowSeconds = 0.400;
        constexpr double c_attackFadeSeconds = 0.020;
        constexpr double c_truePeakBeta = 5.0;

        // Adaptive hold (Auto)
        constexpr double c_holdMinSeconds = 0.025, c_holdMaxSeconds = 0.150;
        constexpr double c_holdScale = 1.25;
        constexpr double c_holdPeakSeconds = 0.200;          // sliding maximum of the overload
        constexpr float c_eventAboveDb = 0.05f;              // a peak event needs this much overload ...
        constexpr float c_eventToleranceDb = 0.5f;           // ... within this of the 200 ms maximum ...
        constexpr double c_eventGapSeconds = 0.003;          // ... and this long after the last one

        // Clipper
        constexpr double c_maxClipDb = 18.0;
        constexpr double c_clipFadeSeconds = 0.020;
        constexpr double c_halfbandBeta = 9.0;
        constexpr double c_decimatorBeta = 9.0;
        constexpr double c_decimatorCutoff = 0.226;          // of the 2x rate: flat to 0.417 fs, -90 dB from 0.5 fs

        // Safety
        constexpr double c_safetyLookaheadSeconds = 0.0005;
        constexpr double c_safetyReleaseSeconds = 0.005;

        constexpr double c_dbToLog = 0.11512925464970228;   // ln (10) / 20

        // Inputs (wet and dry) are taken as silence when not finite and held to +80 dBFS, so no
        // product further down (Push, auto gain, the gain ramps) can overflow and poison a state.
        constexpr float c_inputLimit = 1.0e4f;

        float safeInput (float x)
        {
            return std::clamp (dsp::finiteOrZero (x), -c_inputLimit, c_inputLimit);
        }

        double besselI0 (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 64; ++k)
            {
                const double f = x / (2.0 * static_cast<double> (k));
                term *= f * f;
                sum += term;
                if (term < 1.0e-17 * sum)
                    break;
            }
            return sum;
        }

        double smooth (double current, double target, double coeff)
        {
            const double d = target - current;
            return std::abs (d) < 1.0e-9 ? target : current + coeff * d;
        }

        // The clipper curve's departure from linear, r (u) = f (u) - u, for the odd quadratic-knee
        // curve bounded at 1 with knee half-width w:
        //   |u| <= 1 - w          f = u
        //   1 - w < |u| <= 1 + w  f = sgn (u) (|u| - (|u| - (1 - w))^2 / (4 w))
        //   |u| > 1 + w           f = sgn (u)
        double clipResidualCurve (double u, double w)
        {
            const double a = std::abs (u);
            const double k0 = 1.0 - w;
            if (a <= k0)
                return 0.0;
            double r;
            if (w > 1.0e-9 && a <= 1.0 + w)
            {
                const double t = a - k0;
                r = -t * t / (4.0 * w);
            }
            else
                r = 1.0 - a;
            return u < 0.0 ? -r : r;
        }

        // Its antiderivative (even, 0 on the linear part, continuous at both knee ends).
        double clipResidualIntegral (double u, double w)
        {
            const double a = std::abs (u);
            const double k0 = 1.0 - w;
            if (a <= k0)
                return 0.0;
            if (w > 1.0e-9 && a <= 1.0 + w)
            {
                const double t = a - k0;
                return -t * t * t / (12.0 * w);
            }
            const double d = a - 1.0;
            return -0.5 * d * d - w * w / 6.0;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Half-band 2x -> 4x stage

    void OutputModule::Halfband::reset()
    {
        upHistory.fill (0.0f);
        downOdd.fill (0.0f);
        downEven.fill (0.0f);
        upPos = downPos = evenPos = 0;
    }

    void OutputModule::Halfband::upsample (const std::array<float, c_odd>& taps, float x, float& y0, float& y1)
    {
        // Zero-stuffed: y[2m] = x[m - 6] (the centre tap), y[2m + 1] = 2 sum_k t_k x[m - k].
        upPos = (upPos == 0 ? c_odd : upPos) - 1;
        upHistory[static_cast<size_t> (upPos)] = x;
        upHistory[static_cast<size_t> (upPos + c_odd)] = x;
        const float* h = upHistory.data() + upPos;               // h[k] = x[m - k]

        float acc = 0.0f;
        for (int k = 0; k < c_odd / 2; ++k)
            acc += taps[static_cast<size_t> (k)] * (h[k] + h[c_odd - 1 - k]);

        y0 = h[c_odd / 2];
        y1 = 2.0f * acc;
    }

    float OutputModule::Halfband::downsample (const std::array<float, c_odd>& taps, float u0, float u1)
    {
        // w[m] = 0.5 u0[m - 6] + sum_k t_k u1[m - 1 - k]: evaluated on the even phase, so up + down
        // is 24 samples at 4x, exactly 6 base samples.
        evenPos = (evenPos + 1) & 7;
        downEven[static_cast<size_t> (evenPos)] = u0;
        const float centre = downEven[static_cast<size_t> ((evenPos - c_odd / 2) & 7)];

        const float* h = downOdd.data() + downPos;                // h[k] = u1[m - 1 - k]
        float acc = 0.0f;
        for (int k = 0; k < c_odd / 2; ++k)
            acc += taps[static_cast<size_t> (k)] * (h[k] + h[c_odd - 1 - k]);

        downPos = (downPos == 0 ? c_odd : downPos) - 1;
        downOdd[static_cast<size_t> (downPos)] = u1;
        downOdd[static_cast<size_t> (downPos + c_odd)] = u1;

        return acc + 0.5f * centre;
    }

    // ---------------------------------------------------------------------------------------------
    // Gain ramp

    void OutputModule::GainRamp::prepare (int maxAttack)
    {
        minimum.prepare (maxAttack + 1);
        box.prepare (maxAttack + 1);
        setAttack (std::min (attack, maxAttack));
    }

    void OutputModule::GainRamp::setAttack (int a)
    {
        attack = std::max (1, a);
        minimum.setWindow (attack + 1);
        minimum.reset();
        box.setSpan (attack + 1);
        box.reset (1.0);
    }

    void OutputModule::GainRamp::copyStateFrom (const GainRamp& o)
    {
        minimum.copyStateFrom (o.minimum);
        box.copyStateFrom (o.box);
        attack = o.attack;
    }

    void OutputModule::Channel::copyStateFrom (const Channel& o)
    {
        hp1.copyStateFrom (o.hp1);
        dcBlock.copyStateFrom (o.dcBlock);
        gainDcBlock.copyStateFrom (o.gainDcBlock);
        hp2.copyStateFrom (o.hp2);
        lp.copyStateFrom (o.lp);
        wetHp.copyStateFrom (o.wetHp);
        wetLowShelf.copyStateFrom (o.wetLowShelf);
        wetShelf.copyStateFrom (o.wetShelf);
        dryHp.copyStateFrom (o.dryHp);
        dryShelf.copyStateFrom (o.dryShelf);
        mainPeak = o.mainPeak;
        safetyPeak = o.safetyPeak;
        mainDelay.copyStateFrom (o.mainDelay);
        clipDelay.copyStateFrom (o.clipDelay);
        os.copyStateFrom (o.os);
        hb = o.hb;
        clipPrev = o.clipPrev;
        residualRing = o.residualRing;
        residualPos = o.residualPos;
        decHistory = o.decHistory;
        decPos = o.decPos;
        decZeroRun = o.decZeroRun;
        safetyDelay.copyStateFrom (o.safetyDelay);
    }

    // ---------------------------------------------------------------------------------------------
    // Design

    void OutputModule::designTruePeak()
    {
        // 8x interpolator: windowed sinc cut at half the input rate, 161 taps, centre 80 (Dtp = 10). 20 taps
        // per phase keep the under-read near Nyquist small enough for the safety stage to see
        // the clipper's band-limited overshoot on dense, bright material.
        std::array<double, c_truePeakTaps> h {};
        const double centre = 0.5 * static_cast<double> (c_truePeakTaps - 1);
        double sum = 0.0;
        for (int i = 0; i < c_truePeakTaps; ++i)
        {
            const double m = static_cast<double> (i) - centre;
            const double x = m / static_cast<double> (c_truePeakFactor);
            const double sinc = std::abs (x) < 1.0e-12 ? 1.0 : std::sin (dsp::c_pi * x) / (dsp::c_pi * x);
            const double r = m / centre;
            const double w = besselI0 (c_truePeakBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / besselI0 (c_truePeakBeta);
            h[static_cast<size_t> (i)] = sinc * w;
            sum += h[static_cast<size_t> (i)];
        }
        for (auto& v : h)
            v *= static_cast<double> (c_truePeakFactor) / sum;

        // Phase j (1..7) between x[t - Dtp] and x[t - Dtp + 1]: y_j = sum_m h[8m + j] x[t - m], m = 0 .. 2 Dtp - 1.
        for (int j = 1; j < c_truePeakFactor; ++j)
            for (int m = 0; m < 2 * c_truePeakDelay; ++m)
                m_tpPhases[static_cast<size_t> (j - 1)][static_cast<size_t> (m)]
                    = static_cast<float> (h[static_cast<size_t> (c_truePeakFactor * m + j)]);
    }

    void OutputModule::designHalfband()
    {
        // 25-tap half-band, Kaiser-windowed sinc cut at a quarter of the 4x rate. The odd-offset
        // taps t_k = h[2k + 1] (k = 0..11) sum to 0.5, matching the centre tap (unity DC gain).
        constexpr int centre = (c_halfbandTaps - 1) / 2;        // 12
        const double i0Beta = besselI0 (c_halfbandBeta);
        std::array<double, Halfband::c_odd> t {};
        double sum = 0.0;
        for (int k = 0; k < Halfband::c_odd; ++k)
        {
            const double offset = static_cast<double> (2 * k + 1 - centre);
            const double arg = 0.5 * dsp::c_pi * offset;
            const double r = offset / static_cast<double> (centre);
            const double window = besselI0 (c_halfbandBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
            t[static_cast<size_t> (k)] = 0.5 * (std::sin (arg) / arg) * window;
            sum += t[static_cast<size_t> (k)];
        }
        for (size_t k = 0; k < t.size(); ++k)
            m_hbTaps[k] = static_cast<float> (t[k] * 0.5 / sum);
    }

    void OutputModule::designDecimator()
    {
        // The residual's 2x -> 1x low-pass: Kaiser-windowed sinc, unity DC gain, linear phase.
        constexpr int centre = (c_decimatorTaps - 1) / 2;
        const double i0Beta = besselI0 (c_decimatorBeta);
        std::array<double, centre + 1> h {};
        double sum = 0.0;
        for (int k = 0; k <= centre; ++k)
        {
            const double offset = static_cast<double> (k - centre);
            const double arg = 2.0 * dsp::c_pi * c_decimatorCutoff * offset;
            const double sinc = k == centre ? 1.0 : std::sin (arg) / arg;
            const double r = offset / static_cast<double> (centre);
            const double window = besselI0 (c_decimatorBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
            h[static_cast<size_t> (k)] = sinc * window;
            sum += (k == centre ? 1.0 : 2.0) * h[static_cast<size_t> (k)];
        }
        for (size_t k = 0; k < h.size(); ++k)
            m_decTaps[k] = static_cast<float> (h[k] / sum);
    }

    float OutputModule::decimate (Channel& c, float u0, float u1) const
    {
        constexpr int n = c_decimatorTaps;
        constexpr int centre = (n - 1) / 2;
        for (const float u : { u0, u1 })
        {
            c.decPos = (c.decPos == 0 ? n : c.decPos) - 1;
            c.decHistory[static_cast<size_t> (c.decPos)] = u;
            c.decHistory[static_cast<size_t> (c.decPos + n)] = u;
            c.decZeroRun = u == 0.0f ? std::min (c.decZeroRun + 1, n) : 0;
        }
        if (c.decZeroRun >= n)
            return 0.0f;                                          // the whole history is silent

        const float* h = c.decHistory.data() + c.decPos;          // h[j] = u[E - j], E the odd phase
        float acc = m_decTaps[static_cast<size_t> (centre)] * h[centre];
        for (int k = 0; k < centre; ++k)
            acc += m_decTaps[static_cast<size_t> (k)] * (h[k] + h[n - 1 - k]);
        return acc;
    }

    // ---------------------------------------------------------------------------------------------

    void OutputModule::prepare (double fs, int maxBlock)
    {
        (void) maxBlock;
        m_fs = dsp::safeSampleRate (fs);
        m_lookahead = std::max (1, static_cast<int> (std::lround (c_lookaheadSeconds * m_fs)));
        m_safetyLookahead = std::max (1, static_cast<int> (std::lround (c_safetyLookaheadSeconds * m_fs)));
        m_latency = m_lookahead + c_truePeakDelay + c_clipLatency + m_safetyLookahead + c_truePeakDelay;
        m_lowPassHz = std::min (c_lowPassTargetHz, 0.45 * m_fs);

        const double frameRate = m_fs / static_cast<double> (c_frame);
        m_smoothCoeff = dsp::onePoleCoeff (c_smoothSeconds, m_fs);
        m_rideMsCoeff = dsp::onePoleCoeff (c_rideRmsSeconds, m_fs);
        m_peakRelease = dsp::poleFromTau (c_peakReleaseSeconds, m_fs);
        m_meterCoeff = dsp::onePoleCoeff (c_meterSeconds, frameRate);
        m_glideCoeff = dsp::onePoleCoeff (c_glideSeconds, frameRate);
        m_meanFrames = std::max (1, static_cast<int> (std::lround (c_meterSeconds * frameRate)));
        m_slowCoeff = dsp::onePoleCoeff (c_autoSlowSeconds, m_fs);
        m_safetyRelease = dsp::onePoleCoeff (c_safetyReleaseSeconds, m_fs);
        m_holdMin = dsp::samplesFor (c_holdMinSeconds, m_fs, 1);
        m_holdMax = dsp::samplesFor (c_holdMaxSeconds, m_fs, 1);
        m_eventGap = dsp::samplesFor (c_eventGapSeconds, m_fs, 1);
        m_rampStep = 1.0 / std::max (1.0, c_attackFadeSeconds * m_fs);
        m_clipStep = 1.0 / std::max (1.0, c_clipFadeSeconds * m_fs);
        designTruePeak();
        designHalfband();
        designDecimator();

        for (auto& c : m_ch)
        {
            c.hp1.setCoeffs (Biquad::highpass (m_fs, c_highPassHz, Biquad::c_bw4Q1));
            c.dcBlock.setCoeffs (Biquad::highpass1 (m_fs, c_dcBlockHz));
            c.gainDcBlock.setCoeffs (Biquad::highpass1 (m_fs, c_dcBlockHz));
            c.hp2.setCoeffs (Biquad::highpass (m_fs, c_highPassHz, Biquad::c_bw4Q2));
            c.lp.setCoeffs (Biquad::lowpass (m_fs, m_lowPassHz, Biquad::c_butterworthQ));
            c.wetHp.setCoeffs (Biquad::highpass (m_fs, c_wetHpHz, Biquad::c_butterworthQ));
            c.wetLowShelf.setCoeffs (Biquad::lowShelf (m_fs, c_wetLowShelfHz, Biquad::c_butterworthQ, c_wetLowShelfDb));
            c.wetShelf.setCoeffs (Biquad::highShelf (m_fs, c_weightShelfHz, Biquad::c_butterworthQ, c_weightShelfDb));
            c.dryHp.setCoeffs (Biquad::highpass (m_fs, c_weightHpHz, Biquad::c_butterworthQ));
            c.dryShelf.setCoeffs (Biquad::highShelf (m_fs, c_weightShelfHz, Biquad::c_butterworthQ, c_weightShelfDb));
            c.mainDelay.prepare (m_lookahead + c_truePeakDelay);
            c.mainDelay.setFixedDelay (m_lookahead + c_truePeakDelay);
            c.clipDelay.prepare (c_clipLatency);
            c.clipDelay.setFixedDelay (c_clipLatency);
            c.safetyDelay.prepare (m_safetyLookahead + c_truePeakDelay);
            c.safetyDelay.setFixedDelay (m_safetyLookahead + c_truePeakDelay);
        }
        m_ceilingRing.prepare (m_latency);
        m_envelopeRing.prepare (2 * m_lookahead + 8);
        for (auto& g : m_gc)
            g.prepare (m_lookahead);
        m_holdPeak.prepare (dsp::samplesFor (c_holdPeakSeconds, m_fs, 1) + 1);
        m_holdPeak.setWindow (dsp::samplesFor (c_holdPeakSeconds, m_fs, 1) + 1);
        m_safetyRamp.prepare (m_safetyLookahead);
        m_safetyRamp.setAttack (m_safetyLookahead);
        m_ride.prepare (m_fs);
        m_ride.setTimes (c_rideAttackSeconds, c_rideReleaseSeconds);

        reset();
    }

    int OutputModule::attackSamples (double ms) const
    {
        return std::clamp (static_cast<int> (std::lround (ms * 1.0e-3 * m_fs)), 1, m_lookahead);
    }

    void OutputModule::fillCeilingRing()
    {
        m_ceilingRing.reset();
        for (int i = 0; i <= m_ceilingRing.getMaxDelay(); ++i)
            m_ceilingRing.push (static_cast<float> (m_s.ceilingDb));
    }

    void OutputModule::primeRamp (GainRamp& ramp, int attack)
    {
        // Rebuild the ramp for a new Attack as if it had always run: feed it the delayed envelope
        // history (2 A + 4 samples cover the minimum's window and both boxes), oldest first. The
        // next process() then reads the envelope ring at L - A as usual.
        ramp.setAttack (attack);
        const int count = 2 * ramp.attack + 4;
        const int base = m_lookahead - ramp.attack;
        for (int k = count - 1; k >= 0; --k)
            (void) ramp.process (m_envelopeRing.read (base + k));
    }

    void OutputModule::resetClipper()
    {
        for (auto& c : m_ch)
        {
            c.os.reset();
            c.hb.reset();
            c.clipPrev = 0.0;
            c.residualRing.fill (0.0);
            c.residualPos = 0;
            c.decHistory.fill (0.0f);
            c.decPos = 0;
            c.decZeroRun = 0;
            c.dcBlock.reset();
        }
    }

    void OutputModule::reset()
    {
        for (auto& c : m_ch)
        {
            c.hp1.reset();
            c.dcBlock.reset();
            c.gainDcBlock.reset();
            c.hp2.reset();
            c.lp.reset();
            c.wetHp.reset();
            c.wetLowShelf.reset();
            c.wetShelf.reset();
            c.dryHp.reset();
            c.dryShelf.reset();
            c.mainPeak = TruePeakHistory {};
            c.safetyPeak = TruePeakHistory {};
            c.mainDelay.reset();
            c.clipDelay.reset();
            c.safetyDelay.reset();
        }
        resetClipper();
        m_holdPeak.reset();
        m_safetyRamp.setAttack (m_safetyLookahead);
        m_ride.reset (0.0);

        const State keep = m_s;
        m_s = State {};
        m_s.pushTarget = m_s.pushDb = keep.pushTarget;
        m_s.ceilingTarget = m_s.ceilingDb = keep.ceilingTarget;
        m_s.pushGain = std::exp (laws::pushGainDb (m_s.pushDb, m_s.ceilingDb) * c_dbToLog);
        m_s.mixTarget = m_s.mix = keep.mixTarget;
        m_s.clipTarget = m_s.clipDb = keep.clipTarget;
        m_s.shapeTarget = m_s.shape = keep.shapeTarget;
        m_s.clipBlend = keep.clipTarget > 0.0 ? 1.0 : 0.0;
        m_s.attackTargetMs = keep.attackTargetMs;
        m_s.releaseMs = keep.releaseMs;
        m_s.autoRelease = keep.autoRelease;
        m_s.filters = keep.filters;
        m_s.autoGainOn = keep.autoGainOn;
        m_s.ride = keep.ride;
        m_s.holdLength = m_holdMin;
        m_s.snap = true;

        fillCeilingRing();
        m_envelopeRing.reset();
        for (int i = 0; i <= m_envelopeRing.getMaxDelay(); ++i)
            m_envelopeRing.push (1.0f);
        primeRamp (m_gc[0], attackSamples (m_s.attackTargetMs));
        m_gc[1].setAttack (m_gc[0].attack);
    }

    void OutputModule::setParams (const OutputSettings& s)
    {
        m_s.pushTarget = std::clamp (static_cast<double> (dsp::finiteOrZero (s.pushDb)), 0.0, 24.0);
        m_s.ceilingTarget = std::clamp (static_cast<double> (dsp::finiteOrZero (s.ceilingDb)), -12.0, 0.0);
        m_s.mixTarget = std::clamp (static_cast<double> (dsp::finiteOrZero (s.mix)), 0.0, 1.0);
        m_s.clipTarget = std::clamp (static_cast<double> (dsp::finiteOrZero (s.clipDb)), 0.0, c_maxClipDb);
        m_s.shapeTarget = std::clamp (static_cast<double> (dsp::finiteOrZero (s.clipShape)), 0.0, 1.0);
        const double attack = std::isfinite (s.attackMs) ? static_cast<double> (s.attackMs) : 2.0;
        m_s.attackTargetMs = std::clamp (attack, c_minAttackMs, c_maxAttackMs);
        const double release = std::isfinite (s.releaseMs) ? static_cast<double> (s.releaseMs) : 60.0;
        m_s.releaseMs = std::clamp (release, c_minReleaseMs, c_maxReleaseMs);
        m_s.autoRelease = s.autoRelease;

        if (m_s.snap)
        {
            m_s.snap = false;
            m_s.pushDb = m_s.pushTarget;
            m_s.ceilingDb = m_s.ceilingTarget;
            m_s.pushGain = std::exp (laws::pushGainDb (m_s.pushDb, m_s.ceilingDb) * c_dbToLog);
            m_s.mix = m_s.mixTarget;
            m_s.clipDb = m_s.clipTarget;
            m_s.shape = m_s.shapeTarget;
            m_s.clipBlend = m_s.clipTarget > 0.0 ? 1.0 : 0.0;
            fillCeilingRing();
            m_s.rampFading = false;
            m_s.rampFade = 0.0;
            primeRamp (m_gc[static_cast<size_t> (m_s.gcCurrent)], attackSamples (m_s.attackTargetMs));
        }
    }

    void OutputModule::setTestOptions (bool filters, bool autoGain, bool ride)
    {
        m_s.filters = filters;
        m_s.autoGainOn = autoGain;
        m_s.ride = ride;
        if (! autoGain)
        {
            m_s.autoGainDb = 0.0;
            m_s.gainPrev = m_s.gainNext = 1.0;
        }
    }

    float OutputModule::truePeakDetect (TruePeakHistory& hist, float x) const
    {
        constexpr int taps = 2 * c_truePeakDelay;
        hist.values[static_cast<size_t> (hist.pos)] = x;
        hist.values[static_cast<size_t> (hist.pos + taps)] = x;
        hist.pos = hist.pos + 1 >= taps ? 0 : hist.pos + 1;
        const float* w = hist.values.data() + hist.pos;   // w[taps - 1] = x[t], w[taps - 1 - m] = x[t - m]

        float peak = std::max (std::abs (w[taps / 2 - 1]), std::abs (w[taps / 2]));   // x[t - Dtp], x[t - Dtp + 1]
        for (const auto& ph : m_tpPhases)
        {
            float y = 0.0f;
            for (int m = 0; m < taps; ++m)
                y += ph[static_cast<size_t> (m)] * w[taps - 1 - m];
            peak = std::max (peak, std::abs (y));
        }
        return peak;
    }

    double OutputModule::residual4x (Channel& c, double v, double invBound, double w)
    {
        // ADAA-1 of the residual only, so the linear part never sees the half-sample average. Its
        // output sits half a 4x sample late; a 2-tap average adds the other half, and 3 more 4x
        // samples put it on a whole 2x sample (2 late), part of c_clipLatency.
        const double u = v * invBound;
        const double u1 = c.clipPrev * invBound;
        c.clipPrev = v;
        const double du = u - u1;
        double r;
        if (std::abs (du) < 1.0e-6)
            r = clipResidualCurve (0.5 * (u + u1), w);
        else
            r = (clipResidualIntegral (u, w) - clipResidualIntegral (u1, w)) / du;

        auto& ring = c.residualRing;
        const int pos = c.residualPos;                               // the oldest entry: r[n - 4]
        const double out = 0.5 * (ring[static_cast<size_t> (pos)] + ring[static_cast<size_t> ((pos + 1) & 3)]);
        ring[static_cast<size_t> (pos)] = r;
        c.residualPos = (pos + 1) & 3;
        return out / invBound;
    }

    float OutputModule::clipResidual (Channel& c, float v, double bound, double w)
    {
        const double invBound = 1.0 / bound;
        float a, b;
        c.os.upsample (v, a, b);
        float u[4];
        c.hb.upsample (m_hbTaps, a, u[0], u[1]);
        c.hb.upsample (m_hbTaps, b, u[2], u[3]);
        float r[4];
        for (int k = 0; k < 4; ++k)
            r[k] = static_cast<float> (residual4x (c, static_cast<double> (u[k]), invBound, w));
        const float ra = c.hb.downsample (m_hbTaps, r[0], r[1]);
        const float rb = c.hb.downsample (m_hbTaps, r[2], r[3]);
        return decimate (c, ra, rb);
    }

    void OutputModule::endFrame()
    {
        auto& s = m_s;
        const double wetMs = s.wetSum / static_cast<double> (c_frame);
        const double dryMs = s.drySum / static_cast<double> (c_frame);
        s.wetSum = s.drySum = 0.0;
        s.framePhase = 0;

        if (s.autoGainOn && 10.0 * std::log10 (dryMs + 1.0e-30) > c_learnAboveDb)
        {
            ++s.learnedFrames;
            const double k = s.learnedFrames < m_meanFrames ? 1.0 / static_cast<double> (s.learnedFrames) : m_meterCoeff;
            s.wetMeter += k * (wetMs - s.wetMeter);
            s.dryMeter += k * (dryMs - s.dryMeter);

            double target = std::clamp (10.0 * std::log10 ((s.dryMeter + 1.0e-30) / (s.wetMeter + 1.0e-30)), c_maxCutDb, c_maxBoostDb);
            if (target > 0.0)
            {
                const double guard = s.ceilingDb + c_guardAboveCeilingDb - 20.0 * std::log10 (s.wetPeak + 1.0e-30);
                target = std::max (0.0, std::min (target, guard));
            }
            s.autoGainDb += m_glideCoeff * (target - s.autoGainDb);
        }

        s.gainPrev = s.gainNext;
        s.gainNext = s.autoGainOn ? std::exp (s.autoGainDb * c_dbToLog) : 1.0;
    }

    void OutputModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& /*voice*/)
    {
        processWithDry (l, r, nullptr, nullptr, n, stereo);
    }

    void OutputModule::processWithDry (float* l, float* r, const float* dryL, const float* dryR, int n, bool stereo)
    {
        auto& s = m_s;
        const int numCh = stereo ? 2 : 1;
        float* io[2] = { l, r };
        const float* dry[2] = { dryL, dryR != nullptr ? dryR : dryL };
        const bool hasDry = dryL != nullptr;

        // Block rate: release coefficient, and one Attack change at a time (primed, then crossfaded).
        const double releaseSeconds = s.releaseMs * 1.0e-3;
        const double releaseCoeff = s.autoRelease
            ? dsp::onePoleCoeff (laws::autoFastReleaseMs (s.releaseMs) * 1.0e-3, m_fs)
            : dsp::onePoleCoeff (releaseSeconds, m_fs);
        {
            const int target = attackSamples (s.attackTargetMs);
            if (! s.rampFading && target != m_gc[static_cast<size_t> (s.gcCurrent)].attack)
            {
                primeRamp (m_gc[static_cast<size_t> (1 - s.gcCurrent)], target);
                s.rampFading = true;
                s.rampFade = 0.0;
            }
        }

        constexpr double invFrame = 1.0 / static_cast<double> (c_frame);
        const double rideSlope = 1.0 / c_rideRatio - 1.0;
        const double clipBlendTarget = s.clipTarget > 0.0 ? 1.0 : 0.0;
        const int safetyCeilingDelay = m_lookahead + c_truePeakDelay + c_clipLatency + c_truePeakDelay;
        double minGain = 1.0, minSafety = 1.0;
        float prePeak = 0.0f, postPeak = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            // Smoothed settings.
            if (s.pushDb != s.pushTarget || s.ceilingDb != s.ceilingTarget)
            {
                s.pushDb = smooth (s.pushDb, s.pushTarget, m_smoothCoeff);
                s.ceilingDb = smooth (s.ceilingDb, s.ceilingTarget, m_smoothCoeff);
                s.pushGain = std::exp (laws::pushGainDb (s.pushDb, s.ceilingDb) * c_dbToLog);
            }
            s.mix = smooth (s.mix, s.mixTarget, m_smoothCoeff);
            s.clipDb = smooth (s.clipDb, s.clipTarget, m_smoothCoeff);
            s.shape = smooth (s.shape, s.shapeTarget, m_smoothCoeff);
            m_ceilingRing.push (static_cast<float> (s.ceilingDb));

            // 1. Filters, loudness and peak measurement of the wet.
            float w[2] = { 0.0f, 0.0f };
            double wetPow = 0.0, dryPow = 0.0, wetAbs = 0.0, ridePow = 0.0;
            for (int c = 0; c < numCh; ++c)
            {
                auto& ch = m_ch[static_cast<size_t> (c)];
                float x = safeInput (io[c][i]);
                if (s.filters)
                    x = ch.lp.process (ch.hp2.process (ch.hp1.process (x)));
                x = dsp::flushTiny (x);
                w[c] = x;
                wetAbs = std::max (wetAbs, static_cast<double> (std::abs (x)));
                const double ww = ch.wetShelf.process (ch.wetLowShelf.process (ch.wetHp.process (static_cast<double> (x))));
                wetPow += ww * ww;
                if (hasDry)
                {
                    const double d = static_cast<double> (safeInput (dry[c][i]));
                    const double dw = ch.dryShelf.process (ch.dryHp.process (d));
                    dryPow += dw * dw;
                }
            }
            const double chScale = 1.0 / static_cast<double> (numCh);
            s.wetSum += wetPow * chScale;
            s.drySum += dryPow * chScale;
            s.wetPeak = std::max (wetAbs, s.wetPeak * m_peakRelease);

            // 2. Auto gain (interpolated across the control frame), 3. Ride, 4. Push.
            const double autoGain = s.gainPrev + (s.gainNext - s.gainPrev) * static_cast<double> (s.framePhase + 1) * invFrame;
            for (int c = 0; c < numCh; ++c)
            {
                const double v = static_cast<double> (w[c]) * autoGain;
                ridePow = std::max (ridePow, v * v);
            }
            double rideGain = 1.0;
            if (s.ride)
            {
                s.rideMs = dsp::flushTiny (s.rideMs + m_rideMsCoeff * (ridePow - s.rideMs));
                const double levelDb = 10.0 * std::log10 (s.rideMs + 1.0e-20);
                const double gr = GainComputer::compress (levelDb, c_rideThresholdDb, rideSlope, c_rideKneeDb, c_rideDepthDb);
                rideGain = std::exp (m_ride.process (gr) * c_dbToLog);
            }
            const auto gain = static_cast<float> (autoGain * rideGain * s.pushGain);

            // 5. Mix with the dry (both aligned with OUTPUT's input): the dry gets Push too, equal power.
            if (s.mix != s.mixGainsFor)
            {
                s.mixGainsFor = s.mix;
                s.mixDry = laws::mixDryGain (s.mix);
                s.mixWet = laws::mixWetGain (s.mix);
            }
            float z[2] = { 0.0f, 0.0f };
            for (int c = 0; c < numCh; ++c)
            {
                float v = w[c] * gain;
                if (hasDry && s.mix < 1.0)
                {
                    const double d = static_cast<double> (safeInput (dry[c][i])) * s.pushGain;
                    v = static_cast<float> (s.mixDry * d + s.mixWet * static_cast<double> (v));
                }
                z[c] = v;
            }

            // 6. Main maximizer: detector against theta + Clip.
            float peak = 0.0f;
            for (int c = 0; c < numCh; ++c)
                peak = std::max (peak, truePeakDetect (m_ch[static_cast<size_t> (c)].mainPeak, z[c]));
            const double detTarget = static_cast<double> (m_ceilingRing.read (c_truePeakDelay)) - c_marginDb + s.clipDb;
            const double q = peak > 0.0f ? std::max (0.0, 20.0 * std::log10 (static_cast<double> (peak)) - detTarget) : 0.0;

            // Adaptive hold: repeating overload (a bass beating against a voice) holds across its
            // whole peak pattern; isolated hits keep the short hold.
            {
                const auto qf = static_cast<float> (q);
                const float recent = m_holdPeak.process (qf);
                if (qf > c_eventAboveDb && qf >= recent - c_eventToleranceDb && s.sinceEvent > m_eventGap)
                {
                    const int interval = s.sinceEvent;
                    s.sinceEvent = 0;
                    if (interval < m_holdMax)
                    {
                        s.intervals[static_cast<size_t> (s.nextInterval)] = interval;
                        s.nextInterval = (s.nextInterval + 1) & 3;
                        s.numIntervals = std::min (4, s.numIntervals + 1);
                    }
                    else
                        s.numIntervals = s.nextInterval = 0;

                    int longest = 0;
                    for (int k = 0; k < s.numIntervals; ++k)
                        longest = std::max (longest, s.intervals[static_cast<size_t> ((s.nextInterval - 1 - k) & 3)]);
                    s.holdLength = s.numIntervals > 0
                        ? std::clamp (static_cast<int> (c_holdScale * static_cast<double> (longest)), m_holdMin, m_holdMax)
                        : m_holdMin;
                }
                s.sinceEvent = std::min (s.sinceEvent + 1, 1 << 30);
            }

            const int hold = s.autoRelease ? s.holdLength : 0;
            if (q >= s.fastDb)
            {
                s.fastDb = q;
                s.holdLeft = hold;
            }
            else if (s.holdLeft > 0)
                --s.holdLeft;
            else
                s.fastDb = dsp::flushTiny (s.fastDb + releaseCoeff * (q - s.fastDb));

            double e = s.fastDb;
            if (s.autoRelease)
            {
                s.slowDb = dsp::flushTiny (s.slowDb + m_slowCoeff * (s.fastDb - s.slowDb));
                e = std::max (e, s.slowDb);
            }
            else
                s.slowDb = s.fastDb;

            m_envelopeRing.push (static_cast<float> (std::exp (-e * c_dbToLog)));
            auto& ramp = m_gc[static_cast<size_t> (s.gcCurrent)];
            double g = ramp.process (m_envelopeRing.read (m_lookahead - ramp.attack));
            if (s.rampFading)
            {
                auto& next = m_gc[static_cast<size_t> (1 - s.gcCurrent)];
                const double gNext = next.process (m_envelopeRing.read (m_lookahead - next.attack));
                s.rampFade = std::min (1.0, s.rampFade + m_rampStep);
                g += s.rampFade * (gNext - g);
                if (s.rampFade >= 1.0)
                {
                    s.gcCurrent = 1 - s.gcCurrent;
                    s.rampFading = false;
                }
            }
            minGain = std::min (minGain, g);

            // 7. Clipper: the linear part as a delay, plus the oversampled residual.
            if (s.clipBlend != clipBlendTarget)
            {
                if (s.clipBlend <= 0.0)
                    resetClipper();
                s.clipBlend = clipBlendTarget > s.clipBlend ? std::min (clipBlendTarget, s.clipBlend + m_clipStep)
                                                            : std::max (clipBlendTarget, s.clipBlend - m_clipStep);
            }
            const bool runClip = s.clipBlend > 0.0;
            double bound = 1.0, knee = 0.0;
            if (runClip)
            {
                bound = std::exp ((static_cast<double> (m_ceilingRing.read (m_lookahead + c_truePeakDelay)) - c_marginDb - c_clipBelowDb) * c_dbToLog);
                const double cap = 1.0 - std::exp (-s.clipDb * c_dbToLog);
                knee = laws::clipKnee (s.shape, cap);
            }
            const auto blend = static_cast<float> (s.clipBlend);

            float y[2] = { 0.0f, 0.0f };
            for (int c = 0; c < numCh; ++c)
            {
                auto& ch = m_ch[static_cast<size_t> (c)];
                // The maximizer's gain moves with the waveform, so on an asymmetric wave its reduction
                // (g - 1) x carries DC: the reduction is DC-blocked, so no gain change leaves an offset
                // and material the maximizer leaves alone passes bit-exactly.
                const auto delayed = static_cast<double> (ch.mainDelay.processFixed (z[c]));
                const auto main = static_cast<float> (delayed + ch.gainDcBlock.process ((g - 1.0) * delayed));
                const float linear = ch.clipDelay.processFixed (main);
                float out = linear;
                if (runClip)
                {
                    // The residual of an asymmetric wave carries DC: it is taken out of the residual
                    // only, so material below the knee still passes bit-exactly.
                    const double residual = ch.dcBlock.process (static_cast<double> (blend * clipResidual (ch, main, bound, knee)));
                    out = dsp::flushTiny (static_cast<float> (static_cast<double> (linear) + residual));
                    prePeak = std::max (prePeak, std::abs (linear));
                    postPeak = std::max (postPeak, std::abs (out));
                }
                y[c] = out;
            }

            // 8. Safety: a 0.5 ms true-peak look-ahead at theta, then the clamp.
            float safetyPeak = 0.0f;
            for (int c = 0; c < numCh; ++c)
                safetyPeak = std::max (safetyPeak, truePeakDetect (m_ch[static_cast<size_t> (c)].safetyPeak, y[c]));
            const double safetyTarget = static_cast<double> (m_ceilingRing.read (safetyCeilingDelay)) - c_marginDb;
            const double qs = safetyPeak > 0.0f ? std::max (0.0, 20.0 * std::log10 (static_cast<double> (safetyPeak)) - safetyTarget) : 0.0;
            if (qs >= s.safetyEnvDb)
                s.safetyEnvDb = qs;
            else
                s.safetyEnvDb = dsp::flushTiny (s.safetyEnvDb + m_safetyRelease * (qs - s.safetyEnvDb));
            const double gs = m_safetyRamp.process (static_cast<float> (std::exp (-s.safetyEnvDb * c_dbToLog)));
            minSafety = std::min (minSafety, gs);

            const auto ceilingLin = static_cast<float> (std::exp (static_cast<double> (m_ceilingRing.read (m_latency)) * c_dbToLog));
            for (int c = 0; c < numCh; ++c)
            {
                auto& ch = m_ch[static_cast<size_t> (c)];
                const auto d = static_cast<double> (ch.safetyDelay.processFixed (y[c]));
                float out = dsp::flushTiny (static_cast<float> (d * gs));
                if (std::abs (out) > ceilingLin)
                {
                    out = out > 0.0f ? ceilingLin : -ceilingLin;
                    ++s.clampCount;
                }
                io[c][i] = out;
            }

            if (++s.framePhase >= c_frame)
                endFrame();
        }

        if (n > 0)
        {
            s.reductionDb = static_cast<float> (20.0 * std::log10 (std::max (minGain, 1.0e-6)));
            s.safetyDb = static_cast<float> (20.0 * std::log10 (std::max (minSafety, 1.0e-6)));
            s.clipActivityDb = postPeak > 0.0f && prePeak > postPeak
                ? static_cast<float> (20.0 * std::log10 (static_cast<double> (prePeak) / static_cast<double> (postPeak)))
                : 0.0f;
        }
    }

    void OutputModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<OutputModule> (other);
        m_ch[0].copyStateFrom (o.m_ch[0]);
        m_ch[1].copyStateFrom (o.m_ch[1]);
        m_ceilingRing.copyStateFrom (o.m_ceilingRing);
        m_envelopeRing.copyStateFrom (o.m_envelopeRing);
        m_gc[0].copyStateFrom (o.m_gc[0]);
        m_gc[1].copyStateFrom (o.m_gc[1]);
        m_holdPeak.copyStateFrom (o.m_holdPeak);
        m_safetyRamp.copyStateFrom (o.m_safetyRamp);
        m_ride.copyStateFrom (o.m_ride);
        m_s = o.m_s;
    }

    void OutputModule::copyLeftStateToRight()
    {
        m_ch[1].copyStateFrom (m_ch[0]);
    }

    void OutputModule::publishFeatures (MonsterFeatures& features) const
    {
        features.limiterGrDb.store (m_s.reductionDb, std::memory_order_relaxed);
        features.pushDb.store (static_cast<float> (m_s.pushDb), std::memory_order_relaxed);
    }
}
