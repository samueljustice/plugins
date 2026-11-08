#include "LiquidStage.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_pi = 3.141592653589793;
        constexpr double c_twoPi = 6.283185307179586;

        // 90-degree allpass pair (two chains of four z^-2 sections; coefficients are squared in
        // prepare). Phase difference within about +-1 degree from ~20 Hz to ~20 kHz at 44.1 kHz.
        constexpr std::array<double, 4> c_hilbertA { 0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737 };
        constexpr std::array<double, 4> c_hilbertB { 0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278 };

        constexpr float c_maxWet = 0.35f;            // shifter and phaser wet at Amount 100 %
        constexpr float c_wetKnee = 0.12f;           // wet rises fast: 28 % at Amount 30 %
        constexpr float c_ensembleWetScale = 1.25f;  // ensemble wet relative to the others

        constexpr float c_shiftMinHz = 2.0f, c_shiftMaxHz = 18.0f;
        constexpr float c_shiftLfoMinHz = 0.3f, c_shiftLfoMaxHz = 2.0f;
        constexpr float c_maxShiftFeedback = 0.35f;

        constexpr std::array<float, 4> c_tapBaseMs { 10.0f, 13.0f, 16.5f, 19.5f };
        constexpr std::array<float, 4> c_tapRateHz { 0.31f, 0.43f, 0.59f, 0.77f };
        constexpr float c_tapMinDepthMs = 0.8f, c_tapMaxDepthMs = 2.0f;
        constexpr float c_tapWanderHz = 0.9f;
        constexpr float c_tapWanderShare = 0.3f;     // random share of each tap's modulation
        constexpr double c_sideHighPassHz = 120.0;

        constexpr float c_phaserMinHz = 0.15f, c_phaserMaxHz = 0.6f;
        constexpr float c_phaserFeedback = 0.4f;
        constexpr float c_phaserMakeup = 0.45f;
        constexpr double c_phaserLowHz = 300.0;
        constexpr double c_phaserOctaves = 3.0;      // sweep 300 Hz .. 2.4 kHz

        constexpr double c_paramSeconds = 0.030;

        float smoothstep01 (float t)
        {
            t = std::clamp (t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        int nextPowerOfTwo (int n)
        {
            int p = 1;
            while (p < n)
                p <<= 1;
            return p;
        }

        float wetFor (float amount)
        {
            return c_maxWet * (1.0f + c_wetKnee) * amount / (amount + c_wetKnee);
        }
    }

    void LiquidStage::prepare (double sampleRate, int /*maxBlockSize*/)
    {
        m_sampleRate = sampleRate > 1000.0 ? sampleRate : 48000.0;
        m_invSampleRate = static_cast<float> (1.0 / m_sampleRate);

        for (int i = 0; i < c_numHilbert; ++i)
        {
            m_hilbertA[static_cast<size_t> (i)].c = static_cast<float> (c_hilbertA[static_cast<size_t> (i)] * c_hilbertA[static_cast<size_t> (i)]);
            m_hilbertB[static_cast<size_t> (i)].c = static_cast<float> (c_hilbertB[static_cast<size_t> (i)] * c_hilbertB[static_cast<size_t> (i)]);
        }

        const float maxDelayMs = c_tapBaseMs.back() + c_tapMaxDepthMs * (1.0f + c_tapWanderShare) + 1.0f;
        const int length = nextPowerOfTwo (static_cast<int> (std::ceil (maxDelayMs * 0.001 * m_sampleRate)) + 8);
        m_line.assign (static_cast<size_t> (length), 0.0f);
        m_mask = length - 1;

        // Side high-pass, Butterworth (RBJ).
        {
            const double w = c_twoPi * c_sideHighPassHz / m_sampleRate;
            const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * 0.7071067811865476);
            const double a0 = 1.0 + alpha;
            m_sideB0 = static_cast<float> ((1.0 + cw) * 0.5 / a0);
            m_sideB1 = static_cast<float> (-(1.0 + cw) / a0);
            m_sideB2 = m_sideB0;
            m_sideA1 = static_cast<float> (-2.0 * cw / a0);
            m_sideA2 = static_cast<float> ((1.0 - alpha) / a0);
        }

        m_paramCoeff = static_cast<float> (1.0 - std::exp (-1.0 / (c_paramSeconds * m_sampleRate)));

        reset();
    }

    void LiquidStage::reset()
    {
        for (auto* chain : { &m_hilbertA, &m_hilbertB })
            for (auto& s : *chain)
                s.x1 = s.x2 = s.y1 = s.y2 = 0.0f;

        m_hilbertDelay = 0.0f;
        m_shiftPhase = 0.0;
        m_shiftOut = 0.0f;
        m_shiftHz = 0.0f;
        m_shiftLfo = {};

        std::fill (m_line.begin(), m_line.end(), 0.0f);
        m_write = 0;

        // Spread the tap LFOs so the ensemble starts decorrelated.
        for (int t = 0; t < c_numTaps; ++t)
        {
            m_tapPhase[static_cast<size_t> (t)] = 0.25 * t + 0.1;
            m_tapWander[static_cast<size_t> (t)] = {};
        }

        m_sideZ1 = m_sideZ2 = 0.0f;

        for (auto* p : { &m_phaserL, &m_phaserR })
        {
            p->state.fill (0.0f);
            p->feedback = 0.0f;
            p->coeff = phaserCoefficient (0.0);
            p->coeffStep = 0.0f;
        }

        m_phaserPhase = 0.0;
        m_coefficientCounter = 0;
        m_random = 0x2545f491u;
        m_snap = true;
    }

    void LiquidStage::setParams (float amount01)
    {
        m_target = std::isfinite (amount01) ? std::clamp (amount01, 0.0f, 1.0f) : 0.0f;

        if (m_snap)
        {
            m_amount = m_target;
            m_snap = false;
        }
    }

    float LiquidStage::nextRandom()
    {
        m_random ^= m_random << 13;
        m_random ^= m_random >> 17;
        m_random ^= m_random << 5;
        return static_cast<float> (m_random) * (2.0f / 4294967296.0f) - 1.0f;
    }

    float LiquidStage::advance (SmoothRandom& r, float rateHz)
    {
        r.phase += rateHz * m_invSampleRate;

        if (r.phase >= 1.0f)
        {
            r.phase -= std::floor (r.phase);
            r.from = r.to;
            r.to = nextRandom();
        }

        return r.from + (r.to - r.from) * smoothstep01 (r.phase);
    }

    float LiquidStage::readTap (float delaySamples) const
    {
        const float pos = static_cast<float> (m_write) - delaySamples;
        const float floorPos = std::floor (pos);
        const float t = pos - floorPos;
        const int i0 = static_cast<int> (floorPos);

        const float xm1 = m_line[static_cast<size_t> ((i0 - 1) & m_mask)];
        const float x0 = m_line[static_cast<size_t> (i0 & m_mask)];
        const float x1 = m_line[static_cast<size_t> ((i0 + 1) & m_mask)];
        const float x2 = m_line[static_cast<size_t> ((i0 + 2) & m_mask)];

        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    float LiquidStage::phaserCoefficient (double phase) const
    {
        const double sweep = 0.5 + 0.5 * std::sin (c_twoPi * phase);
        const double hz = std::min (c_phaserLowHz * std::exp2 (c_phaserOctaves * sweep), 0.45 * m_sampleRate);
        const double t = std::tan (c_pi * hz / m_sampleRate);
        return static_cast<float> ((t - 1.0) / (t + 1.0));
    }

    float LiquidStage::runPhaser (Phaser& p, float x, float feedback)
    {
        float u = x + feedback * p.feedback;
        const float a = p.coeff;

        for (auto& s : p.state)
        {
            const float y = a * u + s;
            s = u - a * y;
            u = y;
        }

        p.feedback = u;
        p.coeff += p.coeffStep;
        return u;
    }

    bool LiquidStage::stateIsFinite() const
    {
        if (! std::isfinite (m_shiftOut) || ! std::isfinite (m_hilbertDelay) || ! std::isfinite (m_sideZ1)
            || ! std::isfinite (m_sideZ2) || ! std::isfinite (m_phaserL.feedback) || ! std::isfinite (m_phaserR.feedback))
            return false;

        for (const auto* chain : { &m_hilbertA, &m_hilbertB })
            for (const auto& s : *chain)
                if (! std::isfinite (s.y1) || ! std::isfinite (s.y2))
                    return false;

        for (const auto* p : { &m_phaserL, &m_phaserR })
            for (float s : p->state)
                if (! std::isfinite (s))
                    return false;

        return true;
    }

    void LiquidStage::process (float* left, float* right, int numSamples, bool, const VoiceInfo&)
    {
        if (m_line.empty() || right == nullptr)
            return;

        const float msToSamples = static_cast<float> (0.001 * m_sampleRate);
        const double invFs = 1.0 / m_sampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            const float x = std::isfinite (left[i]) ? left[i] : 0.0f;

            m_amount += m_paramCoeff * (m_target - m_amount);
            const float a = m_amount;
            const float wet = wetFor (a);
            const float ensembleWet = c_ensembleWetScale * wet;
            const float width = a;

            // ---- frequency shifter (single sideband, wandering shift with feedback)
            const float lfo = advance (m_shiftLfo, c_shiftLfoMinHz + (c_shiftLfoMaxHz - c_shiftLfoMinHz) * a);
            m_shiftHz = lfo * (c_shiftMinHz + (c_shiftMaxHz - c_shiftMinHz) * a);
            m_shiftPhase += static_cast<double> (m_shiftHz) * invFs;
            m_shiftPhase -= std::floor (m_shiftPhase);

            float pathA = x + c_maxShiftFeedback * a * m_shiftOut;
            float pathB = pathA;

            for (auto& s : m_hilbertA)
                pathA = s.process (pathA);
            for (auto& s : m_hilbertB)
                pathB = s.process (pathB);

            const float inPhase = m_hilbertDelay;
            m_hilbertDelay = pathA;
            const float quadrature = pathB;

            const double theta = c_twoPi * m_shiftPhase;
            m_shiftOut = inPhase * static_cast<float> (std::cos (theta)) - quadrature * static_cast<float> (std::sin (theta));
            // The shifted signal is decorrelated from the dry, so the blend is normalised by power.
            const float mixNorm = 1.0f / std::sqrt ((1.0f - wet) * (1.0f - wet) + wet * wet);
            const float shimmer = (x + wet * (m_shiftOut - x)) * mixNorm;

            // ---- ensemble (taps 0, 2 left; 1, 3 right)
            m_line[static_cast<size_t> (m_write)] = shimmer;
            const float depthMs = c_tapMinDepthMs + (c_tapMaxDepthMs - c_tapMinDepthMs) * a;
            const double rateScale = 0.7 + 0.6 * a;
            std::array<float, c_numTaps> taps {};

            for (int t = 0; t < c_numTaps; ++t)
            {
                const auto ti = static_cast<size_t> (t);
                m_tapPhase[ti] += c_tapRateHz[ti] * rateScale * invFs;
                m_tapPhase[ti] -= std::floor (m_tapPhase[ti]);
                const float wander = advance (m_tapWander[ti], c_tapWanderHz);
                const float mod = static_cast<float> (std::sin (c_twoPi * m_tapPhase[ti])) + c_tapWanderShare * wander;
                taps[ti] = readTap ((c_tapBaseMs[ti] + depthMs * mod) * msToSamples);
            }

            m_write = (m_write + 1) & m_mask;

            const float ensL = 0.5f * (taps[0] + taps[2]);
            const float ensR = 0.5f * (taps[1] + taps[3]);
            // The taps are 10-20 ms late, mostly decorrelated from the dry: normalise by power.
            const float norm = 1.0f / std::sqrt (1.0f + 0.25f * ensembleWet * ensembleWet);
            const float mid = (shimmer + ensembleWet * 0.5f * (ensL + ensR)) * norm;
            const float sideIn = ensembleWet * width * 0.5f * (ensL - ensR) * norm;

            const float side = m_sideB0 * sideIn + m_sideZ1;
            m_sideZ1 = m_sideB1 * sideIn - m_sideA1 * side + m_sideZ2;
            m_sideZ2 = m_sideB2 * sideIn - m_sideA2 * side;

            // ---- phaser per channel
            if (m_coefficientCounter == 0)
            {
                const double rate = c_phaserMinHz + (c_phaserMaxHz - c_phaserMinHz) * a;
                const double ahead = m_phaserPhase + rate * c_coefficientInterval * invFs;
                const float targetL = phaserCoefficient (ahead);
                const float targetR = phaserCoefficient (ahead + 0.25 * width);
                m_phaserL.coeffStep = (targetL - m_phaserL.coeff) / static_cast<float> (c_coefficientInterval);
                m_phaserR.coeffStep = (targetR - m_phaserR.coeff) / static_cast<float> (c_coefficientInterval);
            }

            m_coefficientCounter = (m_coefficientCounter + 1) % c_coefficientInterval;
            m_phaserPhase += (c_phaserMinHz + (c_phaserMaxHz - c_phaserMinHz) * a) * invFs;
            m_phaserPhase -= std::floor (m_phaserPhase);

            // The blend's notches (deepened by the feedback) cost about 1 dB of loudness at full
            // wet on speech and noise (measured); this puts it back.
            const float phaserNorm = 1.0f + c_phaserMakeup * wet;
            const float inL = mid + side;
            const float inR = mid - side;
            const float phL = runPhaser (m_phaserL, inL, c_phaserFeedback);
            const float phR = runPhaser (m_phaserR, inR, c_phaserFeedback);

            left[i] = (inL + wet * (phL - inL)) * phaserNorm;
            right[i] = (inR + wet * (phR - inR)) * phaserNorm;
        }

        if (! stateIsFinite())
            reset();
    }
}
