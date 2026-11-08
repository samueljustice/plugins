#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace roar
{
    namespace
    {
        constexpr double c_pi = 3.14159265358979323846;
        constexpr double c_targetRate = 11025.0;   // decimated analysis rate (approximately)
        constexpr double c_lowpassHz = 1200.0;      // above the highest fundamental, well below the decimated Nyquist
        constexpr double c_highpassHz = 40.0;
        constexpr double c_hopSeconds = 0.005;
        constexpr double c_minEnergy = 2.5e-7;      // mean square of the band-limited window (~ -66 dBFS)
        constexpr float c_harmonicGuardFloor = 0.02f;   // first-dip depth (at its vertex) trusted as the period
        constexpr float c_harmonicGuardRatio = 0.5f;    // a multiple must be this much deeper to win
    }

    void PitchDetector::Biquad::setLowpass (double fs, double fc, double q)
    {
        const double w = 2.0 * c_pi * fc / fs;
        const double cw = std::cos (w);
        const double alpha = std::sin (w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = static_cast<float> ((1.0 - cw) * 0.5 / a0);
        b1 = static_cast<float> ((1.0 - cw) / a0);
        b2 = b0;
        a1 = static_cast<float> (-2.0 * cw / a0);
        a2 = static_cast<float> ((1.0 - alpha) / a0);
    }

    void PitchDetector::prepare (double sampleRate)
    {
        m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;

        m_decimation = std::max (1, static_cast<int> (std::lround (m_sampleRate / c_targetRate)));
        m_decimatedRate = m_sampleRate / m_decimation;

        // One full period of the lowest pitch is both the window and the largest lag.
        m_tauMax = static_cast<int> (std::ceil (m_decimatedRate / c_minFrequency)) + 2;
        m_window = m_tauMax;
        m_hop = std::max (8, static_cast<int> (std::lround (m_decimatedRate * c_hopSeconds)));
        m_tauMin = std::max (2, static_cast<int> (std::floor (m_decimatedRate / (c_maxFrequency * 1.1))));
        m_analysisDelay = (m_window + m_tauMax) / 2 * m_decimation;

        const int frameLength = m_window + m_tauMax + 2;
        m_ring.assign (static_cast<size_t> (frameLength), 0.0f);
        m_frame.assign (static_cast<size_t> (frameLength), 0.0f);
        m_diff.assign (static_cast<size_t> (m_tauMax + 2), 0.0f);

        m_hpCoeff = static_cast<float> (std::exp (-2.0 * c_pi * c_highpassHz / m_sampleRate));
        // Butterworth 4th order as two biquads.
        m_lp1.setLowpass (m_sampleRate, c_lowpassHz, 0.54119610);
        m_lp2.setLowpass (m_sampleRate, c_lowpassHz, 1.30656296);

        reset();
    }

    void PitchDetector::reset()
    {
        std::fill (m_ring.begin(), m_ring.end(), 0.0f);
        m_ringWrite = 0;
        m_ringFilled = 0;
        m_decimationCounter = 0;
        m_hopCounter = 0;
        m_hpX1 = m_hpY1 = 0.0f;
        m_lp1.clear();
        m_lp2.clear();

        m_history = { 0.0f, 0.0f, 0.0f };
        m_historyCount = 0;
        m_rawFrequency = 0.0f;
        m_rawConfidence = 0.0f;

        m_voice = {};
        m_hasTrack = false;
        m_trackSt = 0.0f;
        m_onsetSt = 0.0f;
        m_onsetHops = 0;
        m_weakHops = 0;
        m_candidateSt = 0.0f;
        m_candidateHops = 0;
        m_smoothedConfidence = 0.0f;
    }

    bool PitchDetector::process (const float* x, int numSamples)
    {
        bool any = false;
        for (int i = 0; i < numSamples; ++i)
            any = pushSample (x[i]) || any;
        return any;
    }

    bool PitchDetector::pushSample (float x)
    {
        if (! (std::abs (x) < 1.0e4f))  // also catches NaN
            x = 0.0f;

        // One-pole DC / rumble high-pass, then the band-limiting low-pass, at full rate.
        const float hp = m_hpCoeff * (m_hpY1 + x - m_hpX1);
        m_hpX1 = x;
        m_hpY1 = std::abs (hp) > 1.0e-20f ? hp : 0.0f;
        float band = m_lp2.process (m_lp1.process (hp));

        if (! std::isfinite (band))
        {
            m_lp1.clear();
            m_lp2.clear();
            m_hpX1 = m_hpY1 = 0.0f;
            band = 0.0f;
        }

        if (++m_decimationCounter < m_decimation)
            return false;

        m_decimationCounter = 0;

        const int ringSize = static_cast<int> (m_ring.size());
        m_ring[static_cast<size_t> (m_ringWrite)] = band;
        m_ringWrite = (m_ringWrite + 1) % ringSize;
        m_ringFilled = std::min (m_ringFilled + 1, ringSize);

        if (++m_hopCounter < m_hop)
            return false;

        m_hopCounter = 0;

        if (m_ringFilled < ringSize)
            return false;

        analyse();
        return true;
    }

    float PitchDetector::dipDepth (const float* d, int tau)
    {
        // Value of the parabola through d[tau - 1], d[tau], d[tau + 1] at its vertex.
        const float s0 = d[tau - 1], s1 = d[tau], s2 = d[tau + 1];
        const float curvature = s0 - 2.0f * s1 + s2;
        if (curvature <= 1.0e-9f)
            return s1;
        const float diff = s2 - s0;
        return std::max (0.0f, s1 - diff * diff / (8.0f * curvature));
    }

    void PitchDetector::analyse()
    {
        const int ringSize = static_cast<int> (m_ring.size());

        // Linearise the ring, oldest sample first.
        for (int i = 0; i < ringSize; ++i)
            m_frame[static_cast<size_t> (i)] = m_ring[static_cast<size_t> ((m_ringWrite + i) % ringSize)];

        const float* frame = m_frame.data();

        double energy = 0.0;
        for (int i = 0; i < m_window + m_tauMax; ++i)
            energy += static_cast<double> (frame[i]) * frame[i];

        float estimate = 0.0f;
        float confidence = 0.0f;

        if (energy > c_minEnergy * (m_window + m_tauMax))
        {
            // YIN steps 1 + 2: difference function and cumulative-mean normalisation.
            float* d = m_diff.data();
            d[0] = 1.0f;
            double runningSum = 0.0;

            for (int tau = 1; tau <= m_tauMax; ++tau)
            {
                // Four independent partial sums: lets the compiler use SIMD without fast-math.
                const float* shifted = frame + tau;
                float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
                int i = 0;
                for (; i + 4 <= m_window; i += 4)
                {
                    const float d0 = frame[i] - shifted[i];
                    const float d1 = frame[i + 1] - shifted[i + 1];
                    const float d2 = frame[i + 2] - shifted[i + 2];
                    const float d3 = frame[i + 3] - shifted[i + 3];
                    s0 += d0 * d0;
                    s1 += d1 * d1;
                    s2 += d2 * d2;
                    s3 += d3 * d3;
                }
                for (; i < m_window; ++i)
                {
                    const float delta = frame[i] - shifted[i];
                    s0 += delta * delta;
                }
                const float sum = (s0 + s1) + (s2 + s3);

                runningSum += sum;
                d[tau] = runningSum > 0.0 ? static_cast<float> (static_cast<double> (sum) * tau / runningSum) : 1.0f;
            }

            // YIN step 3: first dip under the threshold, walked down to its local minimum.
            int tau = -1;
            for (int t = m_tauMin; t < m_tauMax; ++t)
            {
                if (d[t] < c_yinThreshold)
                {
                    while (t + 1 < m_tauMax && d[t + 1] < d[t])
                        ++t;
                    tau = t;
                    break;
                }
            }

            // No dip under the strict threshold (breathy or rough voice): take the deepest dip if it
            // is still plausibly periodic. Its lower confidence lets the tracker decide.
            if (tau < 0)
            {
                int best = -1;
                for (int t = m_tauMin; t < m_tauMax; ++t)
                    if (best < 0 || d[t] < d[best])
                        best = t;

                if (best > 0 && d[best] < c_yinFallbackThreshold)
                    tau = best;
            }

            // Harmonic guard: a voice whose first formant sits on its 2nd or 3rd harmonic has a weak
            // fundamental, so a fraction of the true period also dips under the threshold. When the
            // dip at two or three times that lag is clearly deeper, the voice is really down there.
            // Dip depths are compared at their parabolic vertices: at the decimated rate a high
            // voice's period falls between lags, and the integer-lag values hide how deep the dip is.
            // A dip that already reaches near zero is the period itself: the cumulative-mean
            // normalisation and jitter make its multiples look a little deeper anyway, and following
            // them read steady vowels an octave or a twelfth low (100..900 Hz, every sample rate).
            const float firstDepth = tau > 0 ? dipDepth (d, tau) : 1.0f;
            if (tau > 0 && firstDepth > c_harmonicGuardFloor)
            {
                const int first = tau;
                float bestDepth = firstDepth;

                for (int k = 2; k <= 3; ++k)
                {
                    const int multiple = k * first;
                    if (multiple + k + 1 >= m_tauMax)
                        break;

                    int best = multiple;
                    for (int t = multiple - k; t <= multiple + k; ++t)
                        if (d[t] < d[best])
                            best = t;

                    const float depth = dipDepth (d, best);
                    if (depth < c_harmonicGuardRatio * firstDepth && depth < bestDepth)
                    {
                        tau = best;
                        bestDepth = depth;
                    }
                }
            }

            if (tau > 0)
            {
                // YIN step 4: parabolic interpolation.
                const float s0 = d[tau - 1];
                const float s1 = d[tau];
                const float s2 = d[tau + 1];
                const float denom = 2.0f * (2.0f * s1 - s2 - s0);
                float betterTau = static_cast<float> (tau);

                if (std::abs (denom) > 1.0e-9f)
                    betterTau += std::clamp ((s2 - s0) / denom, -1.0f, 1.0f);

                const float hz = static_cast<float> (m_decimatedRate) / betterTau;

                if (hz >= c_minFrequency * 0.95f && hz <= c_maxFrequency * 1.1f)
                {
                    estimate = hz;
                    confidence = std::clamp (1.0f - s1, 0.0f, 1.0f);
                }
            }
        }

        if (estimate > 0.0f)
        {
            m_history[static_cast<size_t> (m_historyCount % 3)] = estimate;
            ++m_historyCount;

            if (m_historyCount >= 3)
            {
                std::array<float, 3> sorted = m_history;
                std::sort (sorted.begin(), sorted.end());
                m_rawFrequency = sorted[1];
            }
            else
            {
                m_rawFrequency = estimate;
            }
        }
        else
        {
            m_historyCount = 0;
            m_rawFrequency = 0.0f;
        }

        m_rawConfidence = confidence;
        track (m_rawFrequency, confidence);
    }

    void PitchDetector::track (float hz, float confidence)
    {
        m_smoothedConfidence += 0.4f * (confidence - m_smoothedConfidence);

        const bool periodic = hz > 0.0f;
        const float st = periodic ? 12.0f * std::log2 (hz) : 0.0f;
        const bool strong = periodic && confidence >= c_strongConfidence;
        const bool keep = periodic && confidence >= c_keepConfidence;

        if (! m_voice.voiced)
        {
            if (strong)
            {
                if (m_onsetHops > 0 && std::abs (st - m_onsetSt) < 1.0f)
                    ++m_onsetHops;
                else
                {
                    m_onsetSt = st;
                    m_onsetHops = 1;
                }

                if (m_onsetHops >= c_onHops)
                {
                    m_voice.voiced = true;
                    m_trackSt = st;
                    m_hasTrack = true;
                    m_weakHops = 0;
                    m_candidateHops = 0;
                }
                else
                {
                    m_onsetSt = st;
                }
            }
            else
            {
                m_onsetHops = 0;
            }
        }
        else
        {
            bool held = false;

            if (keep)
            {
                const float distance = st - m_trackSt;

                if (std::abs (distance) <= c_followSemitones)
                {
                    m_trackSt = st;
                    m_candidateHops = 0;
                    held = true;
                }
                else if (strong)
                {
                    // A big jump only wins if it keeps coming back; octave flips need longer.
                    if (m_candidateHops > 0 && std::abs (st - m_candidateSt) < 1.5f)
                        ++m_candidateHops;
                    else
                    {
                        m_candidateSt = st;
                        m_candidateHops = 1;
                    }

                    const bool octave = std::abs (std::abs (distance) - 12.0f) < 1.5f;
                    if (m_candidateHops >= (octave ? c_octaveHops : c_jumpHops))
                    {
                        m_trackSt = m_candidateSt;
                        m_candidateHops = 0;
                    }
                    held = true;  // clearly periodic: stay voiced while the jump is confirmed
                }
            }

            if (held)
                m_weakHops = 0;
            else if (++m_weakHops >= c_offHops)
            {
                m_voice.voiced = false;
                m_onsetHops = 0;
                m_candidateHops = 0;
            }
        }

        m_voice.f0Hz = m_hasTrack ? std::exp2 (m_trackSt / 12.0f) : 0.0f;
        m_voice.confidence = std::clamp (m_smoothedConfidence, 0.0f, 1.0f);
    }
}
