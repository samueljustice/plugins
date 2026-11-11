#include "PitchShifter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr float c_pi = 3.14159265358979f;
    constexpr float c_twoPi = 6.28318530717959f;

    inline float wrapPhase(float p) { return p - c_twoPi * std::floor((p + c_pi) / c_twoPi); }

    constexpr float c_minRatio = 0.25f, c_maxRatio = 4.0f;
    constexpr float c_unityTolerance = 1.0e-4f;
    constexpr float c_onsetFlux = 0.5f;     // positive spectral flux / total magnitude that marks an onset
    constexpr int c_easeFrames = 16;        // frames at ratio 1 spent easing the rotations back to zero
    constexpr float c_easeFactor = 0.7f;    // per-frame rotation decay while easing
    constexpr int c_cleanFrames = 4;        // then frames with zero rotation (fills the overlap-add) before the bypass
    constexpr float c_onsetStart = 0.2f;    // an onset starts at the first sample over this much of its loudest one
    constexpr float c_windowSum = 1.5f;     // Hann^2 at 75 % overlap
    constexpr float c_minWindowSum = c_windowSum / 4.0f; // the overlap-add make-up before an onset stops at 12 dB
}

//==============================================================================
void PitchShifter::prepare(double sampleRate)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;

    // ~43 ms frames at any rate: 2048 at 44.1/48k, 4096 at 88.2/96k, 8192 at 176.4/192k.
    int order = 11;
    while ((1 << order) < static_cast<int>(2048.0 * m_sampleRate / 48000.0 * 0.75) && order < 14)
        ++order;
    m_fftOrder = order;
    m_fftSize = 1 << order;
    m_hop = m_fftSize / 4;
    m_bins = m_fftSize / 2 + 1;

    m_fft = std::make_unique<juce::dsp::FFT>(m_fftOrder);

    const auto n = static_cast<size_t>(m_fftSize);
    const auto bins = static_cast<size_t>(m_bins);

    m_window.resize(n);
    for (size_t i = 0; i < n; ++i)
        m_window[i] = 0.5f - 0.5f * std::cos(c_twoPi * static_cast<float>(i) / static_cast<float>(n)); // periodic Hann

    m_in.assign(n, 0.0f);
    m_outAcc.assign(n, 0.0f);
    m_normAcc.assign(n, 0.0f);
    m_outFifo.assign(static_cast<size_t>(m_hop), 0.0f);
    m_work.assign(2 * n, 0.0f);
    m_delay.assign(n, 0.0f);

    for (auto* v : { &m_spec, &m_prevSpec, &m_out })
        v->assign(bins, Cpx {});
    for (auto* v : { &m_mag, &m_prevMag, &m_theta, &m_prevTheta })
        v->assign(bins, 0.0f);
    for (auto* v : { &m_owner, &m_prevOwner })
        v->assign(bins, -1);
    m_peaks.assign(bins, 0);
    m_regionLo.assign(bins, 0);
    m_regionHi.assign(bins, 0);

    const double frameSeconds = static_cast<double>(m_hop) / m_sampleRate;
    m_ratioCoeff = static_cast<float>(1.0 - std::exp(-frameSeconds / 0.03)); // ~30 ms glide
    m_mixStep = 1.0f / static_cast<float>(m_hop);
    m_trimCoeff = static_cast<float>(1.0 - std::exp(-frameSeconds / 0.3)); // ~300 ms
    m_gateFade = std::max(8, static_cast<int>(0.001 * m_sampleRate));

    reset();
}

void PitchShifter::reset()
{
    for (auto* v : { &m_in, &m_outAcc, &m_outFifo, &m_work, &m_delay, &m_mag, &m_prevMag, &m_theta, &m_prevTheta })
        std::fill(v->begin(), v->end(), 0.0f);
    std::fill(m_spec.begin(), m_spec.end(), Cpx {});
    std::fill(m_prevSpec.begin(), m_prevSpec.end(), Cpx {});
    std::fill(m_owner.begin(), m_owner.end(), -1);
    std::fill(m_prevOwner.begin(), m_prevOwner.end(), -1);
    m_pos = 0;
    m_delayPos = 0;
    m_transientPos = -1;
    m_gatePos = 0;
    std::fill(std::begin(m_targetHistory), std::end(m_targetHistory), 0.0f);
    m_historyPos = 0;
    m_powTarget = m_powOut = 0.0f;
    m_trim = 1.0f;

    // The window sum as three earlier (silent) frames would have left it, so the first frames are divided by the
    // full sum, as in the steady state.
    const int size = static_cast<int>(std::min(m_normAcc.size(), m_window.size())); // 0 before prepare()
    for (int i = 0; i < size; ++i)
    {
        float sum = 0.0f;
        for (int j = 1; j <= 3 && i + j * m_hop < size; ++j)
            sum += m_window[static_cast<size_t>(i + j * m_hop)] * m_window[static_cast<size_t>(i + j * m_hop)];
        m_normAcc[static_cast<size_t>(i)] = sum;
    }

    // Start in bypass at the current target, so a fresh track at 0 st is a pure delay from sample one.
    const bool unity = std::abs(m_targetRatio - 1.0f) <= c_unityTolerance;
    m_ratio = unity ? 1.0f : m_targetRatio;
    m_settle = unity ? c_easeFrames + c_cleanFrames : 0;
    m_mix = m_mixTarget = unity ? 0.0f : 1.0f;
}

void PitchShifter::reset(float ratio, float hopPhase)
{
    if (! std::isfinite(ratio))
        ratio = 1.0f;
    ratio = juce::jlimit(c_minRatio, c_maxRatio, ratio);
    m_targetRatio = std::abs(ratio - 1.0f) <= c_unityTolerance ? 1.0f : ratio;
    reset();
    // The write and read positions inside the hop move together, so the latency doesn't change.
    m_pos = juce::jlimit(0, juce::jmax(0, m_hop - 1), static_cast<int>(juce::jlimit(0.0f, 1.0f, hopPhase) * static_cast<float>(m_hop)));
}

//==============================================================================
void PitchShifter::process(const float* in, float* out, int numSamples, float ratio)
{
    if (! std::isfinite(ratio))
        ratio = 1.0f;
    ratio = juce::jlimit(c_minRatio, c_maxRatio, ratio);
    m_targetRatio = std::abs(ratio - 1.0f) <= c_unityTolerance ? 1.0f : ratio;

    if (m_fft == nullptr)
    {
        if (out != in)
            std::memmove(out, in, sizeof(float) * static_cast<size_t>(std::max(0, numSamples)));
        return;
    }

    const int n = m_fftSize;
    const int base = n - m_hop;
    for (int i = 0; i < numSamples; ++i)
    {
        const float x = in[i]; // read before out[i] is written: in and out may alias

        const float dry = m_delay[static_cast<size_t>(m_delayPos)];
        m_delay[static_cast<size_t>(m_delayPos)] = x;
        if (++m_delayPos == n)
            m_delayPos = 0;

        const float wet = m_outFifo[static_cast<size_t>(m_pos)];
        if (! juce::exactlyEqual(m_mix, m_mixTarget))
            m_mix = m_mixTarget > m_mix ? std::min(m_mixTarget, m_mix + m_mixStep)
                                        : std::max(m_mixTarget, m_mix - m_mixStep);
        out[i] = m_mix <= 0.0f ? dry : (m_mix >= 1.0f ? wet : dry + (wet - dry) * m_mix);

        m_in[static_cast<size_t>(base + m_pos)] = x;
        if (++m_pos == m_hop)
        {
            processFrame();
            m_pos = 0;
        }
    }
}

//==============================================================================
void PitchShifter::processFrame()
{
    const int n = m_fftSize, hop = m_hop, bins = m_bins;
    const float omega = c_twoPi * static_cast<float>(hop) / static_cast<float>(n); // phase advance per hop, per bin

    // Ratio glide (log domain), snapping onto the target when it is close.
    {
        const float target = m_targetRatio;
        if (! juce::exactlyEqual(m_ratio, target))
        {
            const float lr = std::log(m_ratio);
            m_ratio = std::exp(lr + (std::log(target) - lr) * m_ratioCoeff);
            if (std::abs(m_ratio / target - 1.0f) <= c_unityTolerance)
                m_ratio = target;
        }
    }
    const bool unity = juce::exactlyEqual(m_ratio, 1.0f);
    if (unity)
        m_settle = std::min(m_settle + 1, c_easeFrames + c_cleanFrames);
    else
        m_settle = 0;
    m_mixTarget = (unity && m_settle >= c_easeFrames + c_cleanFrames) ? 0.0f : 1.0f;
    const float ratioMinus1 = m_ratio - 1.0f;

    // Analysis.
    for (int i = 0; i < n; ++i)
        m_work[static_cast<size_t>(i)] = m_in[static_cast<size_t>(i)] * m_window[static_cast<size_t>(i)];
    std::fill(m_work.begin() + n, m_work.end(), 0.0f);
    m_fft->performRealOnlyForwardTransform(m_work.data(), true);

    float total = 0.0f, flux = 0.0f, maxMag = 0.0f;
    int maxBin = 0;
    for (int k = 0; k < bins; ++k)
    {
        const auto ks = static_cast<size_t>(k);
        const Cpx z(m_work[2 * ks], m_work[2 * ks + 1]);
        m_spec[ks] = z;
        const float m = std::abs(z);
        m_mag[ks] = m;
        total += m;
        flux += std::max(0.0f, m - m_prevMag[ks]);
        if (m > maxMag) { maxMag = m; maxBin = k; }
    }

    // Onsets: track the onset sample through the 4 frames that contain it.
    if (m_transientPos >= 0)
    {
        m_transientPos -= hop;
        m_gatePos -= hop;
        if (m_transientPos < 0)
            m_transientPos = -1;
    }
    if (m_transientPos < 0 && total > 1.0e-4f * static_cast<float>(n) && flux > c_onsetFlux * total)
    {
        // The new material came in with the newest hop or two: the loudest sample there is the onset.
        int best = n - 1;
        float bestAbs = -1.0f;
        for (int i = n - 2 * hop; i < n; ++i)
        {
            const float a = std::abs(m_in[static_cast<size_t>(i)]);
            if (a > bestAbs) { bestAbs = a; best = i; }
        }
        m_transientPos = best;

        // Where it starts: the first sample there that reaches a fifth of the loudest one.
        m_gatePos = best;
        for (int i = n - 2 * hop; i < best; ++i)
            if (std::abs(m_in[static_cast<size_t>(i)]) >= c_onsetStart * bestAbs) { m_gatePos = i; break; }
    }

    // Slide the analysis buffer for the next frame.
    std::memmove(m_in.data(), m_in.data() + hop, sizeof(float) * static_cast<size_t>(n - hop));

    std::fill(m_out.begin(), m_out.end(), Cpx {});
    std::fill(m_owner.begin(), m_owner.end(), -1);

    // Level trim: back to exactly 1 on the way to the bypass.
    if (unity)
        m_trim = m_settle >= c_easeFrames ? 1.0f : 1.0f + (m_trim - 1.0f) * c_easeFactor;

    float inBandPower = 0.0f; // what this frame should sound like: the power of the input bins that stay in band
    const bool resetting = m_transientPos >= 0;

    if (maxMag > 1.0e-9f)
    {
        // Peaks: local maxima over +-2 bins, above -120 dB of the frame's loudest bin.
        const float floor = maxMag * 1.0e-6f;
        int numPeaks = 0;
        for (int k = 1; k < bins - 1; ++k)
        {
            const float m = m_mag[static_cast<size_t>(k)];
            if (m <= floor || m <= m_mag[static_cast<size_t>(k - 1)] || m < m_mag[static_cast<size_t>(k + 1)])
                continue;
            if (k >= 2 && m <= m_mag[static_cast<size_t>(k - 2)])
                continue;
            if (k + 2 < bins && m < m_mag[static_cast<size_t>(k + 2)])
                continue;
            m_peaks[static_cast<size_t>(numPeaks++)] = k;
        }
        if (numPeaks == 0)
            m_peaks[static_cast<size_t>(numPeaks++)] = maxBin;

        // Regions: each peak owns the bins down to the dip between it and its neighbours.
        int lo = 0;
        for (int p = 0; p < numPeaks; ++p)
        {
            const int pk = m_peaks[static_cast<size_t>(p)];
            int hi = bins - 1;
            if (p + 1 < numPeaks)
            {
                const int next = m_peaks[static_cast<size_t>(p + 1)];
                hi = pk;
                for (int k = pk + 1; k < next; ++k)
                    if (m_mag[static_cast<size_t>(k)] < m_mag[static_cast<size_t>(hi)])
                        hi = k;
                if (hi == next) hi = next - 1;
            }
            m_regionLo[static_cast<size_t>(p)] = lo;
            m_regionHi[static_cast<size_t>(p)] = hi;
            lo = hi + 1;
        }

        const float n0 = static_cast<float>(m_transientPos);
        float eIn = 0.0f;

        for (int p = 0; p < numPeaks; ++p)
        {
            const int pk = m_peaks[static_cast<size_t>(p)];
            const auto pks = static_cast<size_t>(pk);

            // True frequency of the peak, in bins, from its phase advance over the hop.
            float dev = 0.0f;
            if (m_prevMag[pks] > 1.0e-3f * m_mag[pks])
            {
                const Cpx d = m_spec[pks] * std::conj(m_prevSpec[pks]);
                dev = wrapPhase(std::atan2(d.imag(), d.real()) - omega * static_cast<float>(pk)) / omega;
                dev = juce::jlimit(-2.0f, 2.0f, dev);
            }
            const float freq = static_cast<float>(pk) + dev;
            const float shiftExact = unity ? 0.0f : freq * ratioMinus1;
            const int shift = static_cast<int>(std::lround(shiftExact));

            float theta;
            if (resetting)
            {
                // Phase reset, aligned so every moved region is in phase at the onset sample.
                theta = -c_twoPi * static_cast<float>(shift) * n0 / static_cast<float>(n);
            }
            else
            {
                const int prev = m_prevOwner[pks];
                const float inherited = prev >= 0 ? m_prevTheta[static_cast<size_t>(prev)] : 0.0f;
                theta = inherited + omega * shiftExact;
            }
            theta = wrapPhase(theta);
            if (unity)
                theta = m_settle >= c_easeFrames ? 0.0f : theta * c_easeFactor;
            m_theta[pks] = theta;

            const Cpx rot = std::polar(1.0f, theta);
            const int rlo = m_regionLo[static_cast<size_t>(p)];
            const int rhi = m_regionHi[static_cast<size_t>(p)];
            const int jlo = std::max(rlo, -shift);
            const int jhi = std::min(rhi, bins - 1 - shift);
            for (int k = rlo; k <= rhi; ++k)
                m_owner[static_cast<size_t>(k)] = pk;
            for (int k = jlo; k <= jhi; ++k)
                eIn += m_mag[static_cast<size_t>(k)] * m_mag[static_cast<size_t>(k)];
            if (juce::exactlyEqual(theta, 0.0f))
            {
                for (int k = jlo; k <= jhi; ++k)
                    m_out[static_cast<size_t>(k + shift)] += m_spec[static_cast<size_t>(k)];
            }
            else
            {
                for (int k = jlo; k <= jhi; ++k)
                    m_out[static_cast<size_t>(k + shift)] += m_spec[static_cast<size_t>(k)] * rot;
            }
        }

        // Level: the frame gets the energy of the input bins that stayed in band (regions that land on each
        // other, or part-moved lobes, would otherwise change it), times the slow trim. Exactly 1 at ratio 1.
        float gain = 1.0f;
        if (! unity)
        {
            float eOut = 0.0f;
            for (size_t k = 0; k < static_cast<size_t>(bins); ++k)
                eOut += std::norm(m_out[k]);
            if (eOut > 1.0e-30f)
                gain = juce::jlimit(0.5f, 2.0f, std::sqrt(eIn / eOut));
        }
        if (! resetting)
            gain *= m_trim; // an onset is rebuilt in phase and loses nothing, so it is left alone
        // One-sided spectrum sum to the power of the signal under the window (sum of hann^2 = 3N/8).
        inBandPower = eIn * 2.0f / (0.375f * static_cast<float>(n) * static_cast<float>(n));

        // Synthesis: inverse FFT, Hann synthesis window, overlap-add (Hann^2 at 75 % overlap sums to 1.5).
        for (size_t k = 0; k < static_cast<size_t>(bins); ++k)
        {
            m_work[2 * k] = m_out[k].real() * gain;
            m_work[2 * k + 1] = m_out[k].imag() * gain;
        }
        m_work[1] = 0.0f;
        m_work[2 * static_cast<size_t>(bins) - 1] = 0.0f;
        std::fill(m_work.begin() + 2 * bins, m_work.end(), 0.0f);
        m_fft->performRealOnlyInverseTransform(m_work.data());

        // Nothing of an onset is written before it: the frames that hold it start with a short fade just ahead
        // of it. The window sum each output sample really got is kept alongside, and divides the output.
        const int gateEnd = (resetting && ! unity) ? juce::jlimit(0, n, m_gatePos) : 0;
        const int gateStart = std::max(0, gateEnd - m_gateFade);
        for (int i = gateStart; i < gateEnd; ++i)
        {
            const auto is = static_cast<size_t>(i);
            const float w = m_window[is] * (0.5f - 0.5f * std::cos(c_pi * static_cast<float>(i - (gateEnd - m_gateFade)) / static_cast<float>(m_gateFade)));
            const float v = m_work[is] * w;
            m_outAcc[is] += std::isfinite(v) ? v : 0.0f;
            m_normAcc[is] += m_window[is] * w;
        }
        for (int i = gateEnd; i < n; ++i)
        {
            const auto is = static_cast<size_t>(i);
            const float v = m_work[is] * m_window[is];
            m_outAcc[is] += std::isfinite(v) ? v : 0.0f;
            m_normAcc[is] += m_window[is] * m_window[is];
        }
    }
    else
    {
        // A silent frame still takes its place in the window sum.
        for (int i = 0; i < n; ++i)
            m_normAcc[static_cast<size_t>(i)] += m_window[static_cast<size_t>(i)] * m_window[static_cast<size_t>(i)];
    }

    for (int i = 0; i < hop; ++i)
        m_outFifo[static_cast<size_t>(i)] = m_outAcc[static_cast<size_t>(i)] / std::max(c_minWindowSum, m_normAcc[static_cast<size_t>(i)]);
    for (auto* acc : { &m_outAcc, &m_normAcc })
    {
        std::memmove(acc->data(), acc->data() + hop, sizeof(float) * static_cast<size_t>(n - hop));
        std::fill(acc->begin() + (n - hop), acc->end(), 0.0f);
    }

    // Level trim. Frames of noise do not line up after a shift, so their overlap-add comes out 1 to 3 dB low.
    // This frame is heard one latency (4 hops) later: compare the output with the target from 4 frames ago,
    // and only learn on steady sound (not on onsets, tails or silence, nor at ratio 1).
    {
        const float heardTarget = m_targetHistory[m_historyPos];
        m_targetHistory[m_historyPos] = (resetting || unity) ? 0.0f : inBandPower;
        m_historyPos = (m_historyPos + 1) & 3;

        if (! unity && ! resetting && heardTarget > 1.0e-8f && inBandPower > 1.0e-8f)
        {
            float outPow = 0.0f;
            for (int i = 0; i < hop; ++i)
                outPow += m_outFifo[static_cast<size_t>(i)] * m_outFifo[static_cast<size_t>(i)];
            outPow /= static_cast<float>(hop);
            if (outPow > 1.0e-9f)
            {
                m_powTarget += (heardTarget - m_powTarget) * m_trimCoeff;
                m_powOut += (outPow / (m_trim * m_trim) - m_powOut) * m_trimCoeff; // untrimmed output power
                if (m_powOut > 1.0e-12f)
                    m_trim += (juce::jlimit(0.7f, 1.6f, std::sqrt(m_powTarget / m_powOut)) - m_trim) * m_trimCoeff;
            }
        }
    }

    // This frame becomes the previous one (swaps: no allocation).
    m_spec.swap(m_prevSpec);
    m_mag.swap(m_prevMag);
    m_theta.swap(m_prevTheta);
    m_owner.swap(m_prevOwner);
}
