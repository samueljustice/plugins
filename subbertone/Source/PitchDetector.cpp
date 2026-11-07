#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

void PitchDetector::prepare(double sampleRate)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    // Decimate to roughly 11 kHz. Fundamentals of interest are < 1 kHz and the
    // caller band-limits the input well below the decimated Nyquist.
    m_decimation = std::max(1, static_cast<int>(std::lround(m_sampleRate / 11025.0)));
    m_decimatedRate = m_sampleRate / m_decimation;

    m_tauMax = static_cast<int>(std::ceil(m_decimatedRate / c_minFrequency)) + 2;
    m_window = m_tauMax; // one full period of the lowest pitch
    m_hop = std::max(16, static_cast<int>(m_decimatedRate * 0.005)); // ~5 ms

    const int frameLength = m_window + m_tauMax + 2;
    m_ring.assign(static_cast<size_t>(frameLength), 0.0f);
    m_frame.assign(static_cast<size_t>(frameLength), 0.0f);
    m_diff.assign(static_cast<size_t>(m_tauMax + 2), 0.0f);

    setMaxFrequency(m_maxFrequency);
    reset();
}

void PitchDetector::reset()
{
    std::fill(m_ring.begin(), m_ring.end(), 0.0f);
    m_ringWrite = 0;
    m_ringFilled = 0;
    m_decimationCounter = 0;
    m_hopCounter = 0;
    m_frequency = 0.0f;
    m_confidence = 0.0f;
    m_history = { 0.0f, 0.0f, 0.0f };
    m_historyCount = 0;
}

void PitchDetector::setMaxFrequency(float hz)
{
    // Written so that NaN lands on the lower bound (std::clamp would pass it through, and the
    // float-to-int conversion below would then be undefined).
    const float lowest = c_minFrequency * 1.5f;
    m_maxFrequency = hz >= lowest ? std::min(hz, c_maxFrequency) : lowest;
    // Allow a little above the limit so notes right at the limit still track.
    m_tauMin = std::max(2, static_cast<int>(std::floor(m_decimatedRate / (m_maxFrequency * 1.1))));
    m_tauMin = std::min(m_tauMin, m_tauMax - 2);
}

bool PitchDetector::pushSample(float x)
{
    if (++m_decimationCounter < m_decimation)
        return false;

    m_decimationCounter = 0;

    const int ringSize = static_cast<int>(m_ring.size());
    m_ring[static_cast<size_t>(m_ringWrite)] = x;
    m_ringWrite = (m_ringWrite + 1) % ringSize;
    m_ringFilled = std::min(m_ringFilled + 1, ringSize);

    if (++m_hopCounter < m_hop)
        return false;

    m_hopCounter = 0;

    if (m_ringFilled < ringSize)
        return false;

    analyse();
    return true;
}

void PitchDetector::analyse()
{
    const int ringSize = static_cast<int>(m_ring.size());

    // Linearise the ring, oldest sample first.
    for (int i = 0; i < ringSize; ++i)
        m_frame[static_cast<size_t>(i)] = m_ring[static_cast<size_t>((m_ringWrite + i) % ringSize)];

    const float* frame = m_frame.data();

    double energy = 0.0;
    for (int i = 0; i < m_window; ++i)
        energy += static_cast<double>(frame[i]) * frame[i];

    float estimate = 0.0f;
    m_confidence = 0.0f;

    if (energy > 1.0e-9 * m_window)
    {
        // YIN step 1+2: difference function and cumulative-mean normalisation.
        float* d = m_diff.data();
        d[0] = 1.0f;
        double runningSum = 0.0;

        for (int tau = 1; tau <= m_tauMax; ++tau)
        {
            float sum = 0.0f;
            for (int i = 0; i < m_window; ++i)
            {
                const float delta = frame[i] - frame[i + tau];
                sum += delta * delta;
            }

            runningSum += sum;
            d[tau] = runningSum > 0.0 ? static_cast<float>(static_cast<double>(sum) * tau / runningSum) : 1.0f;
        }

        // YIN step 3: first dip under threshold, then walk to its local minimum.
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

        // No dip under the strict threshold (common on noisy sources such as
        // engines): fall back to the deepest dip if it is still plausibly periodic.
        // It is reported with its (lower) confidence; the caller's tracker decides
        // whether to trust it.
        if (tau < 0)
        {
            int best = -1;
            for (int t = m_tauMin; t < m_tauMax; ++t)
                if (best < 0 || d[t] < d[best])
                    best = t;

            if (best > 0 && d[best] < c_yinFallbackThreshold)
                tau = best;
        }

        if (tau > 0)
        {
            // YIN step 4: parabolic interpolation.
            const float s0 = d[tau - 1];
            const float s1 = d[tau];
            const float s2 = d[tau + 1];
            const float denom = 2.0f * (2.0f * s1 - s2 - s0);
            float betterTau = static_cast<float>(tau);

            if (std::abs(denom) > 1.0e-9f)
                betterTau += std::clamp((s2 - s0) / denom, -1.0f, 1.0f);

            const float hz = static_cast<float>(m_decimatedRate) / betterTau;

            if (hz >= c_minFrequency && hz <= m_maxFrequency * 1.1f)
            {
                estimate = hz;
                m_confidence = 1.0f - s1;
            }
        }
    }

    if (estimate > 0.0f)
    {
        m_history[static_cast<size_t>(m_historyCount % 3)] = estimate;
        ++m_historyCount;

        if (m_historyCount >= 3)
        {
            std::array<float, 3> sorted = m_history;
            std::sort(sorted.begin(), sorted.end());
            m_frequency = sorted[1];
        }
        else
        {
            m_frequency = estimate;
        }
    }
    else
    {
        m_historyCount = 0;
        m_frequency = 0.0f;
    }
}
