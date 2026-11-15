#include "SpectralMorph.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr float c_pi = 3.14159265358979f;
    constexpr float c_twoPi = 6.28318530717959f;

    inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
    inline float wrapPhase(float p) { return p - c_twoPi * std::floor((p + c_pi) / c_twoPi); }

    constexpr float c_magFloor = 1.0e-6f;
    constexpr int c_envIterations = 3;   // true-envelope iterations (cepstrum, max with the spectrum, again)
    constexpr int c_modeFadeFrames = 6;
    constexpr float c_onsetFlux = 0.5f;  // positive spectral flux / total magnitude that marks an onset (as in PitchShifter)
    constexpr int c_onsetFrames = 3;     // frames for which an onset sits late enough in the frame to be smeared ahead of itself
}

const char* SpectralMorph::modeName(int mode)
{
    switch (mode)
    {
        case Blend: return "Blend";
        case Glide: return "Glide";
        case Cross: return "Cross";
        default:    return "";
    }
}

//==============================================================================
void SpectralMorph::prepare(double sampleRate, int maxBlock)
{
    juce::ignoreUnused(maxBlock);
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;

    // ~43 ms frames at any rate: 2048 at 44.1/48k, 4096 at 88.2/96k, 8192 at 176.4/192k.
    int order = 11;
    while ((1 << order) < static_cast<int>(2048.0 * m_sampleRate / 48000.0 * 0.75) && order < 14)
        ++order;
    m_fftOrder = order;
    m_fftSize = 1 << order;
    m_hop = m_fftSize / 4;
    m_bins = m_fftSize / 2 + 1;
    m_lifter = std::max(8, static_cast<int>(0.0012 * m_sampleRate)); // 1.2 ms quefrency: formants, not harmonics

    m_fft = std::make_unique<juce::dsp::FFT>(m_fftOrder);

    const auto n = static_cast<size_t>(m_fftSize);
    const auto bins = static_cast<size_t>(m_bins);

    m_window.resize(n);
    for (size_t i = 0; i < n; ++i)
        m_window[i] = 0.5f - 0.5f * std::cos(c_twoPi * static_cast<float>(i) / static_cast<float>(n)); // periodic Hann

    m_inA.assign(n, 0.0f);
    m_inB.assign(n, 0.0f);
    m_outAcc.assign(n, 0.0f);
    m_outFifo.assign(static_cast<size_t>(m_hop), 0.0f);
    m_work.assign(2 * n, 0.0f);

    for (auto* v : { &m_specA, &m_specB, &m_out, &m_tmp })
        v->assign(bins, Cpx {});
    for (auto* v : { &m_magA, &m_magB, &m_phA, &m_phB, &m_prevPhA, &m_prevPhB, &m_logA, &m_logB,
                     &m_envA, &m_envB, &m_scratch, &m_scratch2, &m_envT, &m_outPhase })
        v->assign(bins, 0.0f);
    m_massA.assign(bins, Mass {});
    m_massB.assign(bins, Mass {});

    const float frameRate = static_cast<float>(m_sampleRate) / static_cast<float>(m_hop);
    m_paramCoeff = 1.0f - std::exp(-1.0f / (0.04f * frameRate)); // ~40 ms
    m_gateAttack = 1.0f - std::exp(-1.0f / (0.015f * frameRate));
    m_gateRelease = 1.0f - std::exp(-1.0f / (0.12f * frameRate));
    m_trimCoeff = 1.0f - std::exp(-1.0f / (0.3f * frameRate)); // ~300 ms

    reset();
}

void SpectralMorph::reset()
{
    for (auto* v : { &m_inA, &m_inB, &m_outAcc, &m_outFifo, &m_work, &m_prevPhA, &m_prevPhB, &m_outPhase, &m_magA, &m_magB })
        std::fill(v->begin(), v->end(), 0.0f);
    m_pos = 0;
    m_gateA = m_gateB = 0.0f;
    m_onsetLeft = 0;
    m_first = true;
    std::fill(std::begin(m_targetHistory), std::end(m_targetHistory), 0.0f);
    m_historyPos = 0;
    m_powTarget = m_powOut = 0.0f;
    m_trim = 1.0f;
    m_modeFade = 1.0f;
    m_mode = m_prevMode = juce::jlimit(0, NumModes - 1, m_target.mode);
}

//==============================================================================
void SpectralMorph::process(const float* a, const float* b, float* out, int n)
{
    if (m_fft == nullptr)
    {
        for (int i = 0; i < n; ++i)
            out[i] = 0.0f;
        return;
    }

    const int base = m_fftSize - m_hop;
    for (int i = 0; i < n; ++i)
    {
        const float sa = a != nullptr ? a[i] : 0.0f;
        const float sb = b != nullptr ? b[i] : 0.0f;
        m_inA[static_cast<size_t>(base + m_pos)] = sa;
        m_inB[static_cast<size_t>(base + m_pos)] = sb;
        out[i] = m_outFifo[static_cast<size_t>(m_pos)];

        if (++m_pos == m_hop)
        {
            processFrame();
            m_pos = 0;
        }
    }
}

//==============================================================================
void SpectralMorph::analyse(const std::vector<float>& in, std::vector<Cpx>& spec, std::vector<float>& mag,
                            std::vector<float>& phase, std::vector<float>& logMag, float& energy, float& rms,
                            float& total, float& flux)
{
    const int n = m_fftSize;
    float sumSq = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float v = in[static_cast<size_t>(i)] * m_window[static_cast<size_t>(i)];
        m_work[static_cast<size_t>(i)] = v;
        sumSq += v * v;
    }
    std::fill(m_work.begin() + n, m_work.end(), 0.0f);
    rms = std::sqrt(sumSq / (0.375f * static_cast<float>(n))); // sum of hann^2 = 3N/8

    m_fft->performRealOnlyForwardTransform(m_work.data(), true);

    energy = total = flux = 0.0f;
    for (int k = 0; k < m_bins; ++k)
    {
        const Cpx z(m_work[static_cast<size_t>(2 * k)], m_work[static_cast<size_t>(2 * k + 1)]);
        const auto ks = static_cast<size_t>(k);
        spec[ks] = z;
        const float m2 = std::norm(z);
        energy += m2;
        const float m = std::sqrt(m2);
        total += m;
        flux += std::max(0.0f, m - mag[ks]); // mag still holds the last frame
        mag[ks] = m;
        phase[ks] = std::atan2(z.imag(), z.real());
        logMag[ks] = std::log(mag[ks] + c_magFloor);
    }
}

void SpectralMorph::cepstralSmooth(const float* in, float* out)
{
    const int n = m_fftSize;
    for (int k = 0; k < m_bins; ++k)
    {
        m_work[static_cast<size_t>(2 * k)] = in[k];
        m_work[static_cast<size_t>(2 * k + 1)] = 0.0f;
    }
    std::fill(m_work.begin() + 2 * m_bins, m_work.end(), 0.0f);
    m_fft->performRealOnlyInverseTransform(m_work.data()); // real cepstrum in [0, n)

    // Hann lifter over quefrencies 0..Q (and the mirrored half), zero elsewhere.
    const int q = std::min(m_lifter, n / 2 - 1);
    for (int i = 1; i <= q; ++i)
    {
        const float w = 0.5f + 0.5f * std::cos(c_pi * static_cast<float>(i) / static_cast<float>(q + 1));
        m_work[static_cast<size_t>(i)] *= w;
        m_work[static_cast<size_t>(n - i)] *= w;
    }
    std::fill(m_work.begin() + q + 1, m_work.begin() + (n - q), 0.0f);
    std::fill(m_work.begin() + n, m_work.end(), 0.0f);

    m_fft->performRealOnlyForwardTransform(m_work.data(), true);
    for (int k = 0; k < m_bins; ++k)
        out[k] = m_work[static_cast<size_t>(2 * k)];
}

void SpectralMorph::trueEnvelope(const float* logMag, float* env, int iterations)
{
    // Roebel & Rodet's "true envelope": iterate cepstral smoothing of max(spectrum, envelope),
    // so the envelope rides over the harmonic peaks instead of through their average.
    cepstralSmooth(logMag, env);
    for (int it = 1; it < iterations; ++it)
    {
        for (int k = 0; k < m_bins; ++k)
            m_scratch[static_cast<size_t>(k)] = std::max(logMag[k], env[k]);
        cepstralSmooth(m_scratch.data(), env);
    }
}

//==============================================================================
void SpectralMorph::processFrame()
{
    float eA = 0, eB = 0, rmsA = 0, rmsB = 0, totA = 0, totB = 0, fluxA = 0, fluxB = 0;
    analyse(m_inA, m_specA, m_magA, m_phA, m_logA, eA, rmsA, totA, fluxA);
    analyse(m_inB, m_specB, m_magB, m_phB, m_logB, eB, rmsB, totB, fluxB);

    // Presence gates (-75 dBFS = absent, -60 dBFS = present), fast attack, slower release.
    auto gateTarget = [](float rms)
    {
        const float db = 20.0f * std::log10(rms + 1.0e-12f);
        return clampf((db + 75.0f) / 15.0f, 0.0f, 1.0f);
    };
    const float tA = gateTarget(rmsA), tB = gateTarget(rmsB);

    if (m_first)
    {
        m_x = clampf(m_target.x, 0.0f, 1.0f);
        m_y = clampf(m_target.y, 0.0f, 1.0f);
        m_amount = clampf(m_target.amount, 0.0f, 1.0f);
        m_gateA = tA;
        m_gateB = tB;
        m_mode = m_prevMode = juce::jlimit(0, NumModes - 1, m_target.mode);
        m_modeFade = 1.0f;
        m_first = false;
    }
    else
    {
        m_gateA += (tA - m_gateA) * (tA > m_gateA ? m_gateAttack : m_gateRelease);
        m_gateB += (tB - m_gateB) * (tB > m_gateB ? m_gateAttack : m_gateRelease);
        m_x += (clampf(m_target.x, 0.0f, 1.0f) - m_x) * m_paramCoeff;
        m_y += (clampf(m_target.y, 0.0f, 1.0f) - m_y) * m_paramCoeff;
        m_amount += (clampf(m_target.amount, 0.0f, 1.0f) - m_amount) * m_paramCoeff;
    }

    const int wanted = juce::jlimit(0, NumModes - 1, m_target.mode);
    if (wanted != m_mode)
    {
        m_prevMode = m_mode;
        m_mode = wanted;
        m_modeFade = 0.0f;
    }

    const auto bins = static_cast<size_t>(m_bins);
    const float advance = c_twoPi * static_cast<float>(m_hop) / static_cast<float>(m_fftSize);
    bool silent = (m_gateA + m_gateB) < 1.0e-3f;

    // Effective x: pulled towards whichever input is present.
    const float gateSum = m_x * m_gateB + (1.0f - m_x) * m_gateA;
    const float xe = gateSum < 1.0e-6f ? (m_gateB >= m_gateA ? 1.0f : 0.0f) : clampf(m_x * m_gateB / gateSum, 0.0f, 1.0f);

    // Level law. With both inputs there the target is the mix of their powers, (1 - x) A + x B, so a swell in one
    // input comes through as a swell (the geometric mean would halve it in dB). The geometric mean only takes
    // over while an input is fading out through its gate, where x is on its way to the other input anyway.
    const float both = m_gateA * m_gateB;
    auto levelTarget = [xe, both](float pa, float pb)
    {
        const float geometric = std::exp(lerpf(std::log(pa + 1.0e-20f), std::log(pb + 1.0e-20f), xe));
        return geometric + both * (lerpf(pa, pb, xe) - geometric);
    };

    // Onsets (for Glide): a jump in spectral flux in what is heard of the two inputs. For the next few frames the
    // onset has most of the frame ahead of it; Glide takes its phases from the analysis for those frames.
    if (m_onsetLeft > 0)
        --m_onsetLeft;
    {
        const float heard = (1.0f - xe) * totA + xe * totB;
        if (heard > 1.0e-9f && (1.0f - xe) * fluxA + xe * fluxB > c_onsetFlux * heard)
            m_onsetLeft = c_onsetFrames;
    }

    if (! silent)
    {

        const bool fading = m_modeFade < 1.0f;
        auto usesEnvelope = [this](int mode) { return mode != Glide || m_y > 0.01f; };
        if (usesEnvelope(m_mode) || (fading && usesEnvelope(m_prevMode)))
        {
            trueEnvelope(m_logA.data(), m_envA.data(), c_envIterations);
            trueEnvelope(m_logB.data(), m_envB.data(), c_envIterations);
        }

        renderMode(m_mode, xe, m_y, m_out);
        if (fading)
        {
            renderMode(m_prevMode, xe, m_y, m_tmp);
            // Level-match the outgoing mode to the incoming one before the crossfade.
            float e1 = 0.0f, e0 = 0.0f;
            for (size_t k = 0; k < bins; ++k) { e1 += std::norm(m_out[k]); e0 += std::norm(m_tmp[k]); }
            const float g0 = e0 > 1.0e-30f ? std::sqrt(e1 / e0) : 0.0f;
            const float f = m_modeFade;
            for (size_t k = 0; k < bins; ++k)
                m_out[k] = m_out[k] * f + m_tmp[k] * (g0 * (1.0f - f));
            m_modeFade = std::min(1.0f, m_modeFade + 1.0f / static_cast<float>(c_modeFadeFrames));
        }

        // Level matching: the frame gets the interpolated energy of the two inputs.
        float eOut = 0.0f;
        for (size_t k = 0; k < bins; ++k)
            eOut += std::norm(m_out[k]);
        const float eTarget = levelTarget(eA, eB);
        const float g = (eOut > 1.0e-30f ? std::min(100.0f, std::sqrt(eTarget / eOut)) : 0.0f) * m_trim;

        // Amount: blend with a plain equal-power crossfade (automation only, default 1).
        const float wet = m_amount;
        const float ca = std::cos(xe * 0.5f * c_pi) * (1.0f - wet);
        const float cb = std::sin(xe * 0.5f * c_pi) * (1.0f - wet);
        for (size_t k = 0; k < bins; ++k)
            m_out[k] = m_out[k] * (g * wet) + m_specA[k] * ca + m_specB[k] * cb;
    }
    else
    {
        std::fill(m_out.begin(), m_out.end(), Cpx {});
        for (size_t k = 0; k < bins; ++k)
            m_outPhase[k] = wrapPhase(m_outPhase[k] + advance * static_cast<float>(k));
    }

    std::copy(m_phA.begin(), m_phA.end(), m_prevPhA.begin());
    std::copy(m_phB.begin(), m_phB.end(), m_prevPhB.begin());

    // Shift the input windows.
    const int n = m_fftSize, hop = m_hop;
    std::memmove(m_inA.data(), m_inA.data() + hop, sizeof(float) * static_cast<size_t>(n - hop));
    std::memmove(m_inB.data(), m_inB.data() + hop, sizeof(float) * static_cast<size_t>(n - hop));

    // Synthesis: inverse FFT, Hann synthesis window, overlap-add (Hann^2 at 75 % overlap sums to 1.5).
    if (! silent)
    {
        for (size_t k = 0; k < bins; ++k)
        {
            m_work[2 * k] = m_out[k].real();
            m_work[2 * k + 1] = m_out[k].imag();
        }
        m_work[1] = 0.0f;
        m_work[2 * bins - 1] = 0.0f;
        std::fill(m_work.begin() + static_cast<long>(2 * bins), m_work.end(), 0.0f);
        m_fft->performRealOnlyInverseTransform(m_work.data());

        constexpr float olaGain = 1.0f / 1.5f;
        for (int i = 0; i < n; ++i)
        {
            const auto is = static_cast<size_t>(i);
            const float v = m_work[is] * m_window[is] * olaGain;
            m_outAcc[is] += std::isfinite(v) ? v : 0.0f;
        }
    }

    std::copy(m_outAcc.begin(), m_outAcc.begin() + hop, m_outFifo.begin());

    // Loudness trim. The target is what this frame should sound like (the interpolated input
    // power); it is heard one latency (4 hops) later, so compare it with the output 4 frames on.
    {
        const float target = silent ? 0.0f : levelTarget(rmsA * rmsA, rmsB * rmsB);
        const float heardTarget = m_targetHistory[m_historyPos];
        m_targetHistory[m_historyPos] = target;
        m_historyPos = (m_historyPos + 1) & 3;

        float outPow = 0.0f;
        for (int i = 0; i < hop; ++i)
            outPow += m_outFifo[static_cast<size_t>(i)] * m_outFifo[static_cast<size_t>(i)];
        outPow /= static_cast<float>(hop);

        // Only learn while there is a steady signal to compare (skips onsets, tails and silence).
        if (heardTarget > 1.0e-8f && outPow > 1.0e-9f && target > 1.0e-8f)
        {
            m_powTarget += (heardTarget - m_powTarget) * m_trimCoeff;
            m_powOut += (outPow / (m_trim * m_trim) - m_powOut) * m_trimCoeff; // untrimmed output power
            if (m_powOut > 1.0e-12f)
            {
                // -3 .. +4 dB; Glide can lose more level, so it may make up to +/-12 dB.
                const float want = m_mode == Glide ? clampf(std::sqrt(m_powTarget / m_powOut), 0.25f, 4.0f)
                                                   : clampf(std::sqrt(m_powTarget / m_powOut), 0.7f, 1.6f);
                m_trim += (want - m_trim) * m_trimCoeff;
            }
        }
    }
    std::memmove(m_outAcc.data(), m_outAcc.data() + hop, sizeof(float) * static_cast<size_t>(n - hop));
    std::fill(m_outAcc.begin() + (n - hop), m_outAcc.end(), 0.0f);
}

//==============================================================================
void SpectralMorph::renderMode(int mode, float xe, float y, std::vector<Cpx>& dest)
{
    switch (mode)
    {
        case Glide:
            renderGlide(xe, y, dest);
            break;

        case Cross:
        {
            // x travels A -> hybrid -> B. y = 1: envelope moves first (A's fine structure through B's
            // envelope in the middle); y = 0: fine structure moves first (B through A's envelope).
            const float first = std::min(1.0f, 2.0f * xe), second = std::max(0.0f, 2.0f * xe - 1.0f);
            const float envW = lerpf(second, first, y);
            const float fineW = lerpf(first, second, y);
            renderBlend(xe, 0.0f, envW, fineW, dest); // geometric: a clean swap of the fine structures
            break;
        }

        case Blend:
        default:
            // y = 0: power mean of the fine structures (both pitches), y = 1: geometric (fused). Square law, so the
            // lower half of Y moves too (the power mean barely changes between p = 2 and p = 1.2).
            renderBlend(xe, 2.0f * (1.0f - y) * (1.0f - y), xe, xe, dest);
            break;
    }
}

void SpectralMorph::renderBlend(float xe, float p, float envW, float fineW, std::vector<Cpx>& dest)
{
    juce::ignoreUnused(xe);
    const bool geometric = p < 0.001f;
    const float invP = geometric ? 0.0f : 1.0f / p;

    for (int k = 0; k < m_bins; ++k)
    {
        const auto ks = static_cast<size_t>(k);
        const float env = lerpf(m_envA[ks], m_envB[ks], envW);
        const float fa = m_logA[ks] - m_envA[ks]; // whitened fine structure (log)
        const float fb = m_logB[ks] - m_envB[ks];

        float logMag;
        if (geometric || fineW <= 0.0f || fineW >= 1.0f)
        {
            logMag = env + lerpf(fa, fb, fineW);
        }
        else
        {
            // Power mean in the log domain: log(((1-w) e^{p fa} + w e^{p fb})^(1/p)).
            const float mx = std::max(fa, fb);
            const float s = (1.0f - fineW) * std::exp(p * (fa - mx)) + fineW * std::exp(p * (fb - mx));
            logMag = env + mx + std::log(s) * invP;
        }

        // Phase from the weighted complex sum (follows the fine-structure source).
        Cpx z = m_specA[ks] * (1.0f - fineW) + m_specB[ks] * fineW;
        float az = std::abs(z);
        if (az < 1.0e-12f)
        {
            z = fineW < 0.5f ? m_specA[ks] : m_specB[ks];
            az = std::abs(z);
        }
        const float mag = std::exp(logMag) - c_magFloor;
        dest[ks] = az > 1.0e-12f && mag > 0.0f ? z * (mag / az) : Cpx {};
    }
}

//==============================================================================
int SpectralMorph::findMasses(const std::vector<float>& mag, const std::vector<float>& phase,
                              const std::vector<float>& prevPhase, std::vector<Mass>& masses)
{
    const int bins = m_bins;
    float total = 0.0f;
    for (int k = 0; k < bins; ++k)
        total += mag[static_cast<size_t>(k)];

    if (total <= 1.0e-20f)
    {
        masses[0] = { 0, bins, 0, 0.0f, 1.0f };
        return 1;
    }

    const float invTotal = 1.0f / total;
    const float pvScale = static_cast<float>(m_fftSize) / (c_twoPi * static_cast<float>(m_hop));
    const float advance = c_twoPi * static_cast<float>(m_hop) / static_cast<float>(m_fftSize);

    int count = 0, lo = 0, prevPeak = -1;
    auto finish = [&](int l, int h, int peak)
    {
        Mass& m = masses[static_cast<size_t>(count++)];
        m.lo = l;
        m.hi = h;
        m.peak = peak;
        const auto ps = static_cast<size_t>(peak);
        const float dphi = wrapPhase(phase[ps] - prevPhase[ps] - advance * static_cast<float>(peak));
        m.freq = static_cast<float>(peak) + clampf(dphi * pvScale, -1.0f, 1.0f);
        float w = 0.0f;
        for (int k = l; k < h; ++k)
            w += mag[static_cast<size_t>(k)];
        m.weight = w * invTotal;
    };

    for (int k = 0; k < bins; ++k)
    {
        const float v = mag[static_cast<size_t>(k)];
        const float left = k > 0 ? mag[static_cast<size_t>(k - 1)] : -1.0f;
        const float right = k + 1 < bins ? mag[static_cast<size_t>(k + 1)] : -1.0f;
        if (! (v > left && v >= right))
            continue;

        if (prevPeak >= 0)
        {
            int mn = prevPeak + 1;
            for (int j = prevPeak + 2; j < k; ++j)
                if (mag[static_cast<size_t>(j)] < mag[static_cast<size_t>(mn)])
                    mn = j;
            finish(lo, mn, prevPeak);
            lo = mn;
        }
        prevPeak = k;
    }

    if (prevPeak < 0)
        prevPeak = 0;
    finish(lo, bins, prevPeak);
    return count;
}

void SpectralMorph::renderGlide(float xe, float y, std::vector<Cpx>& dest)
{
    // Displacement interpolation between the two spectra (after Henderson & Solomon, "Audio
    // Transport", DAFx 2019): cut each spectrum into peak-centred masses, match the masses with the
    // monotone 1-D optimal-transport plan, and move each matched slice rigidly to the interpolated
    // frequency. Phases are locked to the moved peak, whose phase is advanced at the moved frequency.
    const int bins = m_bins;
    const auto binsS = static_cast<size_t>(bins);
    const float t = xe * xe; // the tonal / noise change otherwise all happens in the first few percent of X
    const float advance = c_twoPi * static_cast<float>(m_hop) / static_cast<float>(m_fftSize);

    std::fill(dest.begin(), dest.end(), Cpx {});

    const int nA = findMasses(m_magA, m_phA, m_prevPhA, m_massA);
    const int nB = findMasses(m_magB, m_phB, m_prevPhB, m_massB);

    float totA = 0.0f, totB = 0.0f;
    for (size_t k = 0; k < binsS; ++k) { totA += m_magA[k]; totB += m_magB[k]; }
    const float invA = totA > 1.0e-20f ? 1.0f / totA : 0.0f;
    const float invB = totB > 1.0e-20f ? 1.0f / totB : 0.0f;

    struct Side
    {
        const std::vector<Mass>* masses;
        const std::vector<float>* mag;
        const std::vector<float>* phase;
        float inv;
        int index = 0, bin = 0;
        float massLeft = 0.0f, binLeft = 0.0f;

        void start(int i)
        {
            index = i;
            const Mass& m = (*masses)[static_cast<size_t>(i)];
            massLeft = m.weight;
            bin = m.lo;
            binLeft = (*mag)[static_cast<size_t>(bin)] * inv;
        }

        // Moves `amount` of normalised mass from the current mass by `shift` bins into dest.
        void consume(float amount, float gain, int shift, float centrePhase, std::vector<Cpx>& out, int nBins)
        {
            const Mass& m = (*masses)[static_cast<size_t>(index)];
            const float refPhase = (*phase)[static_cast<size_t>(m.peak)];
            while (amount > 1.0e-12f && bin < m.hi)
            {
                const float take = std::min(binLeft, amount);
                const int dst = bin + shift;
                if (gain > 0.0f && take > 0.0f && dst >= 0 && dst < nBins)
                    out[static_cast<size_t>(dst)] += std::polar(take * gain, centrePhase + (*phase)[static_cast<size_t>(bin)] - refPhase);
                binLeft -= take;
                amount -= take;
                if (binLeft <= 1.0e-12f)
                {
                    ++bin;
                    binLeft = bin < m.hi ? (*mag)[static_cast<size_t>(bin)] * inv : 0.0f;
                }
            }
        }
    };

    Side sa { &m_massA, &m_magA, &m_phA, invA };
    Side sb { &m_massB, &m_magB, &m_phB, invB };
    sa.start(0);
    sb.start(0);

    while (sa.index < nA && sb.index < nB)
    {
        const Mass& ma = m_massA[static_cast<size_t>(sa.index)];
        const Mass& mb = m_massB[static_cast<size_t>(sb.index)];
        const float amount = std::min(sa.massLeft, sb.massLeft);

        const float f = (1.0f - t) * ma.freq + t * mb.freq;
        const int centre = juce::jlimit(0, bins - 1, t < 0.5f ? ma.peak + static_cast<int>(std::lround(f - ma.freq))
                                                              : mb.peak + static_cast<int>(std::lround(f - mb.freq)));
        const float phi = m_outPhase[static_cast<size_t>(centre)] + advance * f;

        sa.consume(amount, 1.0f - t, centre - ma.peak, phi, dest, bins);
        sb.consume(amount, t, centre - mb.peak, phi, dest, bins);

        sa.massLeft -= amount;
        sb.massLeft -= amount;
        if (sa.massLeft <= 1.0e-7f && ++sa.index < nA)
            sa.start(sa.index);
        if (sb.massLeft <= 1.0e-7f && ++sb.index < nB)
            sb.start(sb.index);
    }

    // Back to absolute magnitude (the masses were normalised so A and B weigh the same in the plan).
    const float scale = std::exp(lerpf(std::log(totA + 1.0e-20f), std::log(totB + 1.0e-20f), t));
    for (size_t k = 0; k < binsS; ++k)
        dest[k] *= scale;

    // An onset is inside the frame: the accumulated phases would smear it over the whole frame (a pre-echo), so
    // the moved magnitudes take their phases from the weighted sum of the two analysis frames (as Blend does),
    // which puts the attack where it is in the inputs. The phase memory below restarts from there.
    if (m_onsetLeft > 0)
    {
        for (size_t k = 0; k < binsS; ++k)
        {
            const Cpx z = m_specA[k] * (1.0f - t) + m_specB[k] * t;
            const float az = std::abs(z);
            if (az > 1.0e-12f)
                dest[k] = z * (std::abs(dest[k]) / az);
        }
    }

    // Phase memory for the next frame.
    for (size_t k = 0; k < binsS; ++k)
    {
        const Cpx z = dest[k];
        m_outPhase[k] = std::norm(z) > 1.0e-20f ? std::atan2(z.imag(), z.real())
                                                : wrapPhase(m_outPhase[k] + advance * static_cast<float>(k));
    }

    // y: re-impose the in-between spectral envelope (formants morph in place instead of travelling).
    if (y > 0.01f)
    {
        for (size_t k = 0; k < binsS; ++k)
            m_scratch2[k] = std::log(std::abs(dest[k]) + 1.0e-9f);
        trueEnvelope(m_scratch2.data(), m_envT.data(), 2);

        float mean = 0.0f;
        for (size_t k = 0; k < binsS; ++k)
        {
            m_scratch2[k] = lerpf(m_envA[k], m_envB[k], t) - m_envT[k];
            mean += m_scratch2[k];
        }
        mean /= static_cast<float>(bins);
        for (size_t k = 0; k < binsS; ++k)
            dest[k] *= std::exp(clampf(0.18f * y * (m_scratch2[k] - mean), -1.5f, 1.5f)); // Y spans the even part of the correction
    }
}

//==============================================================================
void MorphInsert::prepare(double sampleRate, int maxBlock)
{
    m_morph.prepare(sampleRate, maxBlock);
    m_latency = m_morph.getLatencySamples();
    m_maxBlock = std::max(1, maxBlock);
    m_delayA.assign(static_cast<size_t>(m_latency), 0.0f);
    m_delayB.assign(static_cast<size_t>(m_latency), 0.0f);
    m_wet.assign(static_cast<size_t>(m_maxBlock), 0.0f);
    m_alignStep = 1.0f / std::max(1.0f, 0.01f * static_cast<float>(sampleRate > 0.0 ? sampleRate : 48000.0)); // 10 ms splice
    reset(false);
}

void MorphInsert::reset(bool on)
{
    m_morph.reset();
    std::fill(m_delayA.begin(), m_delayA.end(), 0.0f);
    std::fill(m_delayB.begin(), m_delayB.end(), 0.0f);
    m_delayPos = 0;
    m_mix = on ? 1.0f : 0.0f;
    m_align = on ? 1.0f : 0.0f;
    m_running = on;
    m_filled = on ? 2 * m_latency : 0; // starting switched in: everything before now is silence, nothing to wait for
    const float x = clampf(m_morph.getParams().x, 0.0f, 1.0f);
    m_toA = std::cos(0.5f * c_pi * x);
    m_toB = std::sin(0.5f * c_pi * x);
}

void MorphInsert::process(float* a, float* b, int numSamples, float mix, float* wetA, float* wetB)
{
    if (m_wet.empty() || numSamples <= 0)
        return;

    mix = clampf(mix, 0.0f, 1.0f);
    const float from = m_mix;
    for (int done = 0; done < numSamples; done += m_maxBlock)
    {
        const int n = std::min(m_maxBlock, numSamples - done);
        processBlock(a + done, b + done, n,
                     from + (mix - from) * static_cast<float>(done) / static_cast<float>(numSamples),
                     from + (mix - from) * static_cast<float>(done + n) / static_cast<float>(numSamples),
                     wetA != nullptr ? wetA + done : nullptr, wetB != nullptr ? wetB + done : nullptr);
    }
    m_mix = mix;
}

void MorphInsert::processBlock(float* a, float* b, int n, float mixFrom, float mixTo, float* wetA, float* wetB)
{
    const bool on = mixFrom > 1.0e-4f || mixTo > 1.0e-4f;
    // The morph is also kept fed through the splice back to the live sound: if it is switched in again before
    // that splice is over it carries on without a gap, still lined up with the delayed copy.
    const bool feed = on || (m_running && m_align > 0.0f);
    if (feed)
    {
        m_running = true;
        m_morph.process(a, b, m_wet.data(), n); // reads the live buses; its output is one latency late
    }
    else if (m_running)
    {
        // Fully out and back on the live sound: clear the morph, so the next time starts from nothing.
        m_morph.reset();
        m_running = false;
        m_filled = 0;
    }

    // Equal power between the buses, so the middle of the pad isn't a dip in level.
    const float x = clampf(m_morph.getParams().x, 0.0f, 1.0f);
    const float toA1 = std::cos(0.5f * c_pi * x), toB1 = std::sin(0.5f * c_pi * x);
    const float toA0 = m_toA, toB0 = m_toB;
    m_toA = toA1;
    m_toB = toB1;

    const float alignTarget = on ? 1.0f : 0.0f;
    const float inv = 1.0f / static_cast<float>(n);
    const int hop = std::max(1, m_latency / 4);

    for (int i = 0; i < n; ++i)
    {
        const auto ps = static_cast<size_t>(m_delayPos);
        const float liveA = a[i], liveB = b[i];
        const float lateA = m_delayA[ps], lateB = m_delayB[ps];
        m_delayA[ps] = liveA;
        m_delayB[ps] = liveB;
        if (++m_delayPos == m_latency)
            m_delayPos = 0;

        if (! juce::exactlyEqual(m_align, alignTarget))
            m_align = alignTarget > m_align ? std::min(1.0f, m_align + m_alignStep) : std::max(0.0f, m_align - m_alignStep);
        if (m_align <= 0.0f)
            continue; // live: the buses are untouched

        float dryA = lateA, dryB = lateB;
        if (m_align < 1.0f)
        {
            // The splice between the live sound and its late copy (two different moments: equal power).
            const float gl = std::cos(0.5f * c_pi * m_align), gd = std::sin(0.5f * c_pi * m_align);
            dryA = gl * liveA + gd * lateA;
            dryB = gl * liveB + gd * lateB;
        }

        float w = 0.0f;
        if (on)
        {
            // The morph's output only holds the whole sound once it has been fed for one latency: let it in over
            // the hop after that.
            const float t = static_cast<float>(i) * inv;
            const float filled = clampf(static_cast<float>(m_filled + i - m_latency) / static_cast<float>(hop), 0.0f, 1.0f);
            w = (mixFrom + (mixTo - mixFrom) * t) * filled;
            const float wet = m_wet[static_cast<size_t>(i)];
            // The morph's share: crossfaded into the same buffer, or added to where the caller wants it.
            if (wetA != nullptr) { wetA[i] += w * (toA0 + (toA1 - toA0) * t) * wet; dryA *= 1.0f - w; }
            else                 dryA += w * ((toA0 + (toA1 - toA0) * t) * wet - dryA);
            if (wetB != nullptr) { wetB[i] += w * (toB0 + (toB1 - toB0) * t) * wet; dryB *= 1.0f - w; }
            else                 dryB += w * ((toB0 + (toB1 - toB0) * t) * wet - dryB);
        }
        a[i] = dryA;
        b[i] = dryB;
    }

    if (feed)
        m_filled = std::min(m_filled + n, 2 * m_latency);
}
