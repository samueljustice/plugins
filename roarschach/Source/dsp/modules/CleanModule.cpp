#include "CleanModule.h"

#include "../MonsterFeatures.h"
#include "../common/GainComputer.h"
#include "../KnobLaws.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_controlBlock = 32.0;
        constexpr double c_knobSmoothSeconds = 0.020;
        constexpr double c_toneFadeSeconds = 0.020;

        // 12 dB/oct high-pass Q: a touch above Butterworth so Flat (40 Hz) stays within 0.1 dB from 80 Hz up.
        constexpr double c_hpfQ = 0.75;

        // Gate (ratio and depth: KnobLaws).
        constexpr double c_gateKneeDb = 6.0;
        constexpr double c_gateHysteresisDb = 4.0;
        constexpr double c_gateHoldSeconds = 0.060;
        constexpr double c_gateOpenSeconds = 0.001;
        constexpr double c_gateCloseSeconds = 0.150;
        constexpr double c_gateDetectorSeconds = 0.010;
        constexpr double c_gateFloorOffsetDb = 10.0;
        constexpr double c_gateThresholdMinDb = -62.0;
        constexpr double c_gateThresholdMaxDb = -40.0;
        constexpr double c_floorUpSeconds = 0.4;
        constexpr double c_floorDownSeconds = 0.15;
        constexpr double c_floorDropDb = 6.0;
        constexpr double c_floorInitialDb = -80.0;
        constexpr float c_voicedConfidence = 0.6f;

        // A gate held open by a steady room level (the floor is only learnt while closed, so it
        // could never close): when even the quietest moment of the last c_stuckSeconds stayed
        // above the threshold, the floor keeps learning while open (not on voiced frames), towards
        // the room level but at most c_stuckMarginDb above that quietest moment, until it closes.
        constexpr double c_stuckSeconds = 3.0;
        constexpr double c_stuckMarginDb = 6.0;
        constexpr double c_roomSegmentSeconds = c_stuckSeconds / static_cast<double> (CleanModule::c_roomSegments);

        // Leveller.
        constexpr double c_levDetectorSeconds = 0.020;
        constexpr double c_levKneeDb = 10.0;
        constexpr double c_levAttackSeconds = 0.015;
        constexpr double c_levReleaseSeconds = 0.250;
        constexpr double c_levReleaseUnvoicedSeconds = 5.0;

        // Peak catcher.
        constexpr double c_peakRatio = 6.0;
        constexpr double c_peakKneeDb = 4.0;
        constexpr double c_peakAttackSeconds = 0.0003;
        constexpr double c_peakReleaseSeconds = 0.060;
        constexpr double c_peakHoldSeconds = 0.010;

        // Contain below this fades the leveller and the catcher out (exact unity at 0 %).
        constexpr double c_engageSpan = 0.10;

        constexpr double c_tinyPower = 1.0e-20;

        double levThresholdFor (double u) { return -20.0 - 16.0 * u; }
        double levSlopeFor (double u) { return 1.0 / (1.4 + 1.2 * u) - 1.0; }
        double peakThresholdFor (double u) { return -6.0 - 12.0 * u; }

        double powerToDb (double p) { return 10.0 * std::log10 (p + c_tinyPower); }
    }

    // ------------------------------------------------------------------------------------ design

    int CleanModule::highPassSections (double fs, Params::CleanTone tone, std::array<Biquad::Coeffs, 2>& out)
    {
        switch (tone)
        {
            case Params::CleanTone::flat:  out[0] = Biquad::highpass (fs, 40.0, c_hpfQ); return 1;
            case Params::CleanTone::voice: out[0] = Biquad::highpass (fs, 70.0, c_hpfQ); return 1;
            case Params::CleanTone::heavy: out[0] = Biquad::highpass (fs, 90.0, c_hpfQ); return 1;
            case Params::CleanTone::radio:
                out[0] = Biquad::highpass (fs, 300.0, Biquad::c_bw4Q1);
                out[1] = Biquad::highpass (fs, 300.0, Biquad::c_bw4Q2);
                return 2;
        }
        out[0] = Biquad::highpass (fs, 70.0, c_hpfQ);
        return 1;
    }

    double CleanModule::staticMakeupDb (double contain)
    {
        contain = std::clamp (contain, 0.0, 1.0);
        return -GainComputer::compress (c_referenceDb, levThresholdFor (contain), levSlopeFor (contain), c_levKneeDb);
    }

    void CleanModule::designTones()
    {
        for (int t = 0; t < 4; ++t)
        {
            auto& c = m_toneCoeffs[static_cast<size_t> (t)];
            std::array<Biquad::Coeffs, 2> hp;
            int k = highPassSections (m_fs, static_cast<Params::CleanTone> (t), hp);
            for (int i = 0; i < k; ++i)
                c[static_cast<size_t> (i)] = hp[static_cast<size_t> (i)];

            switch (static_cast<Params::CleanTone> (t))
            {
                case Params::CleanTone::flat:
                    break;
                case Params::CleanTone::voice:
                    // Tuned on the reference take: a light low shelf keeps the fundamental (the
                    // octave layer and the tube's bass build on it) and a deep mud cut does the
                    // containment, so the 160-500 Hz share lands near the reference's.
                    c[static_cast<size_t> (k++)] = Biquad::lowShelf (m_fs, 150.0, 0.7, -1.5);
                    c[static_cast<size_t> (k++)] = Biquad::peaking (m_fs, 320.0, 1.2, -7.0);
                    c[static_cast<size_t> (k++)] = Biquad::peaking (m_fs, 2800.0, 0.9, 1.5);
                    break;
                case Params::CleanTone::heavy:
                    c[static_cast<size_t> (k++)] = Biquad::lowShelf (m_fs, 150.0, 0.7, -8.0);
                    c[static_cast<size_t> (k++)] = Biquad::peaking (m_fs, 320.0, 1.2, -8.5);
                    c[static_cast<size_t> (k++)] = Biquad::peaking (m_fs, 2800.0, 0.9, 2.0);
                    break;
                case Params::CleanTone::radio:
                    c[static_cast<size_t> (k++)] = Biquad::peaking (m_fs, 1800.0, 0.9, 3.0);
                    c[static_cast<size_t> (k++)] = Biquad::lowpass (m_fs, 3500.0, Biquad::c_bw4Q1);
                    c[static_cast<size_t> (k++)] = Biquad::lowpass (m_fs, 3500.0, Biquad::c_bw4Q2);
                    break;
            }
            m_toneSections[static_cast<size_t> (t)] = k;
        }
    }

    void CleanModule::loadBank (ToneBank& bank, int tone)
    {
        bank.tone = tone;
        const auto& c = m_toneCoeffs[static_cast<size_t> (tone)];
        for (size_t i = 0; i < c_maxSections; ++i)
        {
            bank.left[i].setCoeffs (c[i]);
            bank.right[i].setCoeffs (c[i]);
            bank.left[i].reset();
            bank.right[i].reset();
        }
    }

    // ------------------------------------------------------------------------------------ lifecycle

    void CleanModule::prepare (double fs, int maxBlock)
    {
        m_fs = dsp::safeSampleRate (fs);
        m_gateTrace.assign (static_cast<size_t> (std::max (1, maxBlock)), 0.0f);
        designTones();

        m_xfadeLength = std::max (1, dsp::samplesFor (c_toneFadeSeconds, m_fs));
        m_knobCoeff = dsp::onePoleCoeff (c_knobSmoothSeconds, m_fs / c_controlBlock);

        m_gateMsCoeff = dsp::onePoleCoeff (c_gateDetectorSeconds, m_fs);
        m_floorUpCoeff = dsp::onePoleCoeff (c_floorUpSeconds, m_fs);   // power domain
        m_floorDownCoeff = dsp::onePoleCoeff (c_floorDownSeconds, m_fs);
        m_gateOpenCoeff = dsp::onePoleCoeff (c_gateOpenSeconds, m_fs);
        m_gateCloseRate = 1.0 / (c_gateCloseSeconds * m_fs);
        m_holdSamples = dsp::samplesFor (c_gateHoldSeconds, m_fs, 1);

        m_levMsCoeff = dsp::onePoleCoeff (c_levDetectorSeconds, m_fs);
        m_levAttack = dsp::poleFromTau (c_levAttackSeconds, m_fs);
        m_levRelease = dsp::poleFromTau (c_levReleaseSeconds, m_fs);
        m_levReleaseUnvoiced = dsp::poleFromTau (c_levReleaseUnvoicedSeconds, m_fs);

        m_roomSegmentLength = dsp::samplesFor (c_roomSegmentSeconds, m_fs, 1);
        m_stuckFloorCapPower = std::pow (10.0, (c_gateThresholdMaxDb - c_gateFloorOffsetDb) * 0.1);

        m_peakHold.prepare (dsp::samplesFor (c_peakHoldSeconds, m_fs, 1));
        m_peakHold.setWindow (dsp::samplesFor (c_peakHoldSeconds, m_fs, 1));
        m_peakBallistics.prepare (m_fs);
        m_peakBallistics.setTimes (c_peakAttackSeconds, c_peakReleaseSeconds);

        m_fade.prepare (m_fs, maxBlock, 0);
        reset();
    }

    void CleanModule::resetWet()
    {
        loadBank (m_banks[0], m_requestedTone);
        loadBank (m_banks[1], m_requestedTone);
        m_activeBank = 0;
        m_xfadePos = 0;

        m_contain = m_containTarget;
        m_gateU = m_gateTarget;
        m_ctrlCountdown = 0;
        updateControl();

        m_gateMs = 0.0;
        m_floorDb = c_floorInitialDb;
        m_floorPower = std::pow (10.0, c_floorInitialDb * 0.1);
        m_gateThresholdDb = c_gateThresholdMinDb;
        m_gateIsOpen = false;
        m_holdCount = 0;
        m_roomSegments.fill (0.0);
        m_roomSegmentPos = 0;
        m_roomSegmentsFilled = 0;
        m_roomSegmentMin = 0.0;
        m_roomMinDb = 0.0;
        m_floorStuck = false;
        m_gateGainDb = 0.0;   // start open: no gating of the first syllable after a reset
        m_gateOpen = 1.0f;

        // The leveller starts at its reduction for the reference level (it is calibrated to R), so a
        // first line after a reset is not lifted by the static makeup before the leveller engages.
        m_levMs = 0.0;
        m_levG1 = m_levY = -m_makeupDb * m_engage;
        m_levGainDb = 0.0;
        m_peakHold.reset();
        m_peakBallistics.reset();
        m_peakGainDb = 0.0;
        m_totalGainDb = m_levGainDb;
    }

    void CleanModule::reset()
    {
        m_fade.reset();
        resetWet();
    }

    void CleanModule::setParams (const CleanSettings& s)
    {
        m_fade.setOn (s.on);
        m_containTarget = std::clamp (static_cast<double> (s.contain), 0.0, 1.0);
        m_gateTarget = std::clamp (static_cast<double> (s.gate), 0.0, 1.0);
        m_requestedTone = toneIndex (s.tone);
    }

    void CleanModule::updateControl()
    {
        m_contain += m_knobCoeff * (m_containTarget - m_contain);
        m_gateU += m_knobCoeff * (m_gateTarget - m_gateU);
        if (std::abs (m_containTarget - m_contain) < 1.0e-6)
            m_contain = m_containTarget;
        if (std::abs (m_gateTarget - m_gateU) < 1.0e-6)
            m_gateU = m_gateTarget;

        m_gateDepthDb = laws::gateDepthDb (m_gateU);
        m_gateRatio = laws::gateRatio (m_gateU);
        m_gateOffsetDb = laws::gateThresholdOffsetDb (m_gateU);
        const double c = laws::containCurve (m_contain);
        m_levThreshold = levThresholdFor (c);
        m_levSlope = levSlopeFor (c);
        m_peakThreshold = peakThresholdFor (c);
        m_makeupDb = staticMakeupDb (c);
        m_engage = dsp::smoothstep01 (c / c_engageSpan);

        // Tone change: start a crossfade into a freshly designed bank (one at a time).
        if (m_xfadePos == 0 && m_requestedTone != m_banks[static_cast<size_t> (m_activeBank)].tone)
        {
            m_activeBank = 1 - m_activeBank;
            loadBank (m_banks[static_cast<size_t> (m_activeBank)], m_requestedTone);
            m_xfadePos = m_xfadeLength;
        }
    }

    // ------------------------------------------------------------------------------------ process

    void CleanModule::process (float* l, float* r, int n, bool stereo, const VoiceFrameView& voice)
    {
        m_fade.captureDry (l, r, n, stereo);
        if (! m_fade.isActive())
        {
            m_fade.outputDry (l, r, n, stereo);
            m_gateOpen = 0.0f;
            std::fill (m_gateTrace.begin(), m_gateTrace.begin() + std::min (n, static_cast<int> (m_gateTrace.size())), 0.0f);
            return;
        }
        if (m_fade.consumeRestart())
            resetWet();

        const double peakSlope = 1.0 / c_peakRatio - 1.0;
        const double invXfade = 1.0 / static_cast<double> (m_xfadeLength);

        for (int seg = 0; seg < n;)
        {
            const int segEnd = voice.frameEnd (seg, n);
            const VoiceFrame& frame = voice.atSample (seg);
            const bool voicedHint = frame.voiced && frame.confidence > c_voicedConfidence;

            for (int i = seg; i < segEnd; ++i)
            {
                if (m_ctrlCountdown <= 0)
                {
                    updateControl();
                    m_ctrlCountdown = static_cast<int> (c_controlBlock);
                }
                --m_ctrlCountdown;

                // ---- Tone EQ (with the bank crossfade)
                const double xl = dsp::finiteOrZero (static_cast<double> (l[i]));
                const double xr = stereo ? dsp::finiteOrZero (static_cast<double> (r[i])) : 0.0;
                auto& bank = m_banks[static_cast<size_t> (m_activeBank)];
                const int sections = m_toneSections[static_cast<size_t> (bank.tone)];
                double el = xl, er = xr;
                for (int k = 0; k < sections; ++k)
                    el = bank.left[static_cast<size_t> (k)].process (el);
                if (stereo)
                    for (int k = 0; k < sections; ++k)
                        er = bank.right[static_cast<size_t> (k)].process (er);

                if (m_xfadePos > 0)
                {
                    auto& old = m_banks[static_cast<size_t> (1 - m_activeBank)];
                    const int oldSections = m_toneSections[static_cast<size_t> (old.tone)];
                    double ol = xl, orr = xr;
                    for (int k = 0; k < oldSections; ++k)
                        ol = old.left[static_cast<size_t> (k)].process (ol);
                    if (stereo)
                        for (int k = 0; k < oldSections; ++k)
                            orr = old.right[static_cast<size_t> (k)].process (orr);
                    --m_xfadePos;
                    const double w = dsp::smoothstep01 (1.0 - static_cast<double> (m_xfadePos) * invXfade);
                    el = ol + w * (el - ol);
                    er = orr + w * (er - orr);
                }

                const double power = stereo ? std::max (el * el, er * er) : el * el;

                // ---- Gate: floor-tracking threshold, hysteresis, hold, voiced hint
                m_gateMs = dsp::flushTiny (m_gateMs + m_gateMsCoeff * (power - m_gateMs));
                const double gateLevel = powerToDb (m_gateMs);

                // Quietest detector level of the last c_stuckSeconds, from the minima of
                // c_roomSegments segments.
                m_roomSegmentMin = m_roomSegmentPos == 0 ? gateLevel : std::min (m_roomSegmentMin, gateLevel);
                if (++m_roomSegmentPos >= m_roomSegmentLength)
                {
                    std::copy (m_roomSegments.begin() + 1, m_roomSegments.end(), m_roomSegments.begin());
                    m_roomSegments.back() = m_roomSegmentMin;
                    m_roomSegmentsFilled = std::min (m_roomSegmentsFilled + 1, c_roomSegments);
                    m_roomMinDb = *std::min_element (m_roomSegments.begin(), m_roomSegments.end());
                    m_roomSegmentPos = 0;
                }
                if (! m_gateIsOpen)
                    m_floorStuck = false;
                else if (m_roomSegmentsFilled >= c_roomSegments && m_roomMinDb > m_gateThresholdDb)
                    m_floorStuck = true;

                // Floor: learnt while closed (tracks the mean detector level of the room); a much
                // quieter room is followed faster, open or closed. A gate stuck open on a steady
                // room keeps learning (see c_stuckSeconds) until it closes.
                if (gateLevel < m_floorDb - c_floorDropDb)
                    m_floorPower += m_floorDownCoeff * (m_gateMs - m_floorPower);
                else if (! m_gateIsOpen)
                    m_floorPower += m_floorUpCoeff * (m_gateMs - m_floorPower);
                else if (m_floorStuck && ! voicedHint)
                {
                    const double cap = std::min (m_stuckFloorCapPower, std::pow (10.0, (m_roomMinDb + c_stuckMarginDb) * 0.1));
                    const double target = std::min (m_gateMs, cap);
                    if (target > m_floorPower)
                        m_floorPower += m_floorUpCoeff * (target - m_floorPower);
                }
                m_floorPower = std::max (m_floorPower, c_tinyPower);
                m_floorDb = powerToDb (m_floorPower);
                // The threshold sits laws::gateThresholdOffsetDb above the floor (10 dB at the
                // reference), and its limits move with it.
                const double offsetShift = m_gateOffsetDb - c_gateFloorOffsetDb;
                m_gateThresholdDb = std::clamp (m_floorDb + m_gateOffsetDb, c_gateThresholdMinDb + offsetShift, c_gateThresholdMaxDb + offsetShift);

                if (voicedHint || gateLevel > m_gateThresholdDb + c_gateHysteresisDb)
                {
                    m_gateIsOpen = true;
                    m_holdCount = m_holdSamples;
                }
                else if (m_gateIsOpen)
                {
                    if (gateLevel < m_gateThresholdDb)
                    {
                        if (--m_holdCount <= 0)
                            m_gateIsOpen = false;
                    }
                    else
                        m_holdCount = m_holdSamples;
                }

                const double gateTarget = m_gateIsOpen
                    ? 0.0
                    : GainComputer::expand (gateLevel, m_gateThresholdDb, m_gateRatio, c_gateKneeDb, m_gateDepthDb);
                if (gateTarget > m_gateGainDb)
                    m_gateGainDb += m_gateOpenCoeff * (gateTarget - m_gateGainDb);
                else
                    m_gateGainDb = std::max (gateTarget, m_gateGainDb - std::max (-m_gateDepthDb, 1.0) * m_gateCloseRate);
                m_gateGainDb = dsp::flushTiny (std::min (0.0, m_gateGainDb));

                // ---- Leveller: Avg detector of the gated signal (gate gain applied in dB), static makeup
                m_levMs = dsp::flushTiny (m_levMs + m_levMsCoeff * (power - m_levMs));
                const double levLevel = powerToDb (m_levMs) + m_gateGainDb;
                // Held while the gate is closed (pauses), so the next line starts at the running reduction.
                if (m_gateIsOpen)
                {
                    const double levTarget = m_engage * GainComputer::compress (levLevel, m_levThreshold, m_levSlope, c_levKneeDb);
                    const double aR = voicedHint ? m_levRelease : m_levReleaseUnvoiced;
                    m_levG1 = std::min (levTarget, aR * m_levG1 + (1.0 - aR) * levTarget);
                    m_levY = m_levAttack * m_levY + (1.0 - m_levAttack) * m_levG1;
                    m_levG1 = dsp::flushTiny (m_levG1);
                    m_levY = dsp::flushTiny (m_levY);
                }
                m_levGainDb = m_levY + m_engage * m_makeupDb;

                // ---- Peak catcher: 10 ms peak hold of the levelled signal. The gate gain is left out of
                // its detector: a gate that is still opening (1 ms) would hide the first loud cycle of
                // an onset from the hold, and the catcher would then miss the next one. Above the
                // catcher's threshold the gate is always open, so nothing else changes.
                const double peakAbs = stereo ? std::max (std::abs (el), std::abs (er)) : std::abs (el);
                const double peakDb = 20.0 * std::log10 (peakAbs + 1.0e-10) + m_levGainDb;
                const double held = static_cast<double> (m_peakHold.process (static_cast<float> (peakDb)));
                const double peakTarget = m_engage * GainComputer::compress (held, m_peakThreshold, peakSlope, c_peakKneeDb);
                m_peakGainDb = m_peakBallistics.process (peakTarget);

                if (i < static_cast<int> (m_gateTrace.size()))
                    m_gateTrace[static_cast<size_t> (i)] = static_cast<float> (m_gateGainDb) * m_fade.getValue();

                // ---- Apply
                m_totalGainDb = m_gateGainDb + m_levGainDb + m_peakGainDb;
                const double gain = (m_totalGainDb == 0.0) ? 1.0 : std::exp (m_totalGainDb * 0.11512925464970228);
                l[i] = dsp::flushTiny (static_cast<float> (el * gain));
                if (stereo)
                    r[i] = dsp::flushTiny (static_cast<float> (er * gain));
            }
            seg = segEnd;
        }

        m_gateOpen = m_gateDepthDb < -0.01
            ? static_cast<float> (std::clamp (1.0 - m_gateGainDb / m_gateDepthDb, 0.0, 1.0))
            : 1.0f;

        m_fade.apply (l, r, n, stereo);
    }

    // ------------------------------------------------------------------------------------ state

    void CleanModule::copyStateFrom (const Module& other)
    {
        const auto& o = sameType<CleanModule> (other);
        m_fade.copyStateFrom (o.m_fade);
        for (size_t b = 0; b < m_banks.size(); ++b)
        {
            m_banks[b].tone = o.m_banks[b].tone;
            const auto& c = m_toneCoeffs[static_cast<size_t> (m_banks[b].tone)];
            for (size_t k = 0; k < c_maxSections; ++k)
            {
                m_banks[b].left[k].setCoeffs (c[k]);
                m_banks[b].right[k].setCoeffs (c[k]);
                m_banks[b].left[k].copyStateFrom (o.m_banks[b].left[k]);
                m_banks[b].right[k].copyStateFrom (o.m_banks[b].right[k]);
            }
        }
        m_activeBank = o.m_activeBank;
        m_requestedTone = o.m_requestedTone;
        m_xfadePos = o.m_xfadePos;

        m_containTarget = o.m_containTarget;
        m_gateTarget = o.m_gateTarget;
        m_contain = o.m_contain;
        m_gateU = o.m_gateU;
        m_ctrlCountdown = o.m_ctrlCountdown;
        m_gateDepthDb = o.m_gateDepthDb;
        m_gateOffsetDb = o.m_gateOffsetDb;
        m_levThreshold = o.m_levThreshold;
        m_levSlope = o.m_levSlope;
        m_peakThreshold = o.m_peakThreshold;
        m_makeupDb = o.m_makeupDb;
        m_engage = o.m_engage;

        m_gateMs = o.m_gateMs;
        m_floorDb = o.m_floorDb;
        m_floorPower = o.m_floorPower;
        m_gateThresholdDb = o.m_gateThresholdDb;
        m_gateIsOpen = o.m_gateIsOpen;
        m_holdCount = o.m_holdCount;
        m_roomSegments = o.m_roomSegments;
        m_roomSegmentPos = o.m_roomSegmentPos;
        m_roomSegmentsFilled = o.m_roomSegmentsFilled;
        m_roomSegmentMin = o.m_roomSegmentMin;
        m_roomMinDb = o.m_roomMinDb;
        m_floorStuck = o.m_floorStuck;
        m_gateGainDb = o.m_gateGainDb;
        m_gateOpen = o.m_gateOpen;

        m_levMs = o.m_levMs;
        m_levG1 = o.m_levG1;
        m_levY = o.m_levY;
        m_levGainDb = o.m_levGainDb;
        m_peakHold.copyStateFrom (o.m_peakHold);
        m_peakBallistics.copyStateFrom (o.m_peakBallistics);
        m_peakGainDb = o.m_peakGainDb;
        m_totalGainDb = o.m_totalGainDb;
    }

    void CleanModule::copyLeftStateToRight()
    {
        m_fade.copyLeftStateToRight();
        for (auto& bank : m_banks)
            for (size_t k = 0; k < c_maxSections; ++k)
                bank.right[k].copyStateFrom (bank.left[k]);
    }

    void CleanModule::publishFeatures (MonsterFeatures& features) const
    {
        features.gateOpen.store (m_gateOpen * m_fade.getValue(), std::memory_order_relaxed);
        features.cleanGrDb.store (getContainGrDb() * m_fade.getValue(), std::memory_order_relaxed);
    }
}
