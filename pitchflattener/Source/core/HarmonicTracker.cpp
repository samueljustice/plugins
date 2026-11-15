#include "HarmonicTracker.h"
#include "HindsightTracker.h"

#include <algorithm>
#include <complex>
#include <cstdint>
#include <limits>
#include <numbers>
#include <numeric>

namespace pitchflattener::core
{

namespace
{
    constexpr double kPi = std::numbers::pi;

    // In-place radix-2 FFT. size must be a power of two.
    void fft(std::vector<std::complex<float>>& a)
    {
        const std::size_t n = a.size();
        for (std::size_t i = 1, j = 0; i < n; ++i)
        {
            std::size_t bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(a[i], a[j]);
        }
        for (std::size_t len = 2; len <= n; len <<= 1)
        {
            const double angle = -2.0 * kPi / static_cast<double>(len);
            const std::complex<float> wLen(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
            for (std::size_t i = 0; i < n; i += len)
            {
                std::complex<float> w(1.0f, 0.0f);
                for (std::size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = a[i + k];
                    const auto v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wLen;
                }
            }
        }
    }

    std::size_t nextPow2(std::size_t v)
    {
        std::size_t p = 1;
        while (p < v)
            p <<= 1;
        return p;
    }

    struct Peak
    {
        float hz = 0.0f;
        float amp = 0.0f;   // linear, window-normalised (a full-scale sine reads about 1)
    };

    // Windowed magnitude spectrum of one frame, and its peaks (parabolic interpolation on the log
    // magnitude: a steady partial is located to a few cents even between bins).
    class SpectrumFrames
    {
    public:
        SpectrumFrames(double sampleRate, std::size_t fftSize)
            : m_rate(sampleRate), m_size(fftSize), m_window(fftSize), m_buffer(fftSize), m_mag(fftSize / 2 + 1)
        {
            double sum = 0.0;
            for (std::size_t i = 0; i < fftSize; ++i)
            {
                m_window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(fftSize)));
                sum += m_window[i];
            }
            m_scale = static_cast<float>(2.0 / sum);
        }

        // Frame of `size` samples starting at start; samples outside the buffer read as zero.
        const std::vector<float>& magnitudes(const float* mono, std::int64_t numSamples, std::int64_t start)
        {
            for (std::size_t i = 0; i < m_size; ++i)
            {
                const auto at = start + static_cast<std::int64_t>(i);
                const float x = at >= 0 && at < numSamples ? mono[at] : 0.0f;
                m_buffer[i] = { x * m_window[i], 0.0f };
            }
            fft(m_buffer);
            for (std::size_t k = 0; k < m_mag.size(); ++k)
                m_mag[k] = std::abs(m_buffer[k]) * m_scale;
            return m_mag;
        }

        void peaks(const std::vector<float>& mag, double lowHz, double highHz, float floorAmp, float relativeFloor,
                   std::vector<Peak>& out) const
        {
            out.clear();
            const double binHz = m_rate / static_cast<double>(m_size);
            const auto lo = std::max<std::size_t>(2, static_cast<std::size_t>(lowHz / binHz));
            const auto hi = std::min<std::size_t>(mag.size() - 2, static_cast<std::size_t>(highHz / binHz));
            float top = 0.0f;
            for (std::size_t k = lo; k <= hi; ++k)
                top = std::max(top, mag[k]);
            const float threshold = std::max(floorAmp, top * relativeFloor);
            for (std::size_t k = lo; k <= hi; ++k)
            {
                const float m = mag[k];
                if (m < threshold || m <= mag[k - 1] || m < mag[k + 1])
                    continue;
                const double a = std::log(std::max(1.0e-12f, mag[k - 1]));
                const double b = std::log(std::max(1.0e-12f, m));
                const double c = std::log(std::max(1.0e-12f, mag[k + 1]));
                const double denom = a - 2.0 * b + c;
                const double shift = denom < 0.0 ? std::clamp(0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
                Peak p;
                p.hz = static_cast<float>((static_cast<double>(k) + shift) * binHz);
                p.amp = static_cast<float>(std::exp(b - 0.25 * (a - c) * shift));
                out.push_back(p);
            }
        }

    private:
        double m_rate;
        std::size_t m_size;
        std::vector<float> m_window;
        std::vector<std::complex<float>> m_buffer;
        std::vector<float> m_mag;
        float m_scale = 1.0f;
    };

    float median(std::vector<float> v)
    {
        if (v.empty())
            return 0.0f;
        const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
        std::nth_element(v.begin(), mid, v.end());
        return *mid;
    }

    float percentile(std::vector<float> v, double p)
    {
        if (v.empty())
            return 0.0f;
        const auto at = static_cast<std::size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1) + 0.5);
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(at), v.end());
        return v[at];
    }
}

const char* pitchClassName(int pitchClass) noexcept
{
    static constexpr const char* names[12] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((pitchClass % 12) + 12) % 12];
}

std::string noteName(double midi, int* centsOut)
{
    const int nearest = static_cast<int>(std::lround(midi));
    if (centsOut != nullptr)
        *centsOut = static_cast<int>(std::lround((midi - nearest) * 100.0));
    return std::string(pitchClassName(nearest)) + std::to_string(nearest / 12 - 1);
}

//==============================================================================
namespace
{
    // The strongest ring of the average spectrum: for sounds with no pitch to track (hits,
    // whooshes, noise with a resonance). Returns the frequency and how far it stands out, in dB.
    bool findResonance(const float* mono, int numSamples, double sampleRate, double& hzOut, double& prominenceDbOut)
    {
        const std::size_t fftSize = nextPow2(static_cast<std::size_t>(0.17 * sampleRate));
        const auto hop = static_cast<std::int64_t>(fftSize / 2);
        SpectrumFrames spectrum(sampleRate, fftSize);
        std::vector<double> power(fftSize / 2 + 1, 0.0);
        int frames = 0;
        for (std::int64_t start = -static_cast<std::int64_t>(fftSize) / 2; start < numSamples; start += hop, ++frames)
        {
            const auto& mag = spectrum.magnitudes(mono, numSamples, start);
            for (std::size_t k = 0; k < power.size(); ++k)
                power[k] += static_cast<double>(mag[k]) * mag[k];
        }
        if (frames == 0)
            return false;

        const double binHz = sampleRate / static_cast<double>(fftSize);
        std::vector<double> db(power.size());
        for (std::size_t k = 0; k < power.size(); ++k)
            db[k] = 10.0 * std::log10(power[k] / frames + 1.0e-20);

        const auto lo = std::max<std::size_t>(2, static_cast<std::size_t>(40.0 / binHz));
        const auto hi = std::min<std::size_t>(db.size() - 2, static_cast<std::size_t>(4000.0 / binHz));
        double bestScore = -1.0e9;
        std::size_t bestBin = 0;
        double bestProminence = 0.0;
        std::vector<float> around;
        for (std::size_t k = lo; k <= hi; ++k)
        {
            if (db[k] <= db[k - 1] || db[k] < db[k + 1])
                continue;
            // Baseline: the median level over the octave around the peak.
            const auto a = std::max<std::size_t>(1, static_cast<std::size_t>(static_cast<double>(k) / std::sqrt(2.0)));
            const auto b = std::min<std::size_t>(db.size() - 1, static_cast<std::size_t>(static_cast<double>(k) * std::sqrt(2.0)) + 1);
            around.clear();
            for (std::size_t i = a; i <= b; ++i)
                around.push_back(static_cast<float>(db[i]));
            const double prominence = db[k] - static_cast<double>(median(around));
            // Standing out matters most; among equals, the louder ring wins.
            const double score = prominence + 0.25 * db[k];
            if (score > bestScore)
            {
                bestScore = score;
                bestBin = k;
                bestProminence = prominence;
            }
        }
        if (bestBin == 0)
            return false;

        const double a = db[bestBin - 1], b = db[bestBin], c = db[bestBin + 1];
        const double denom = a - 2.0 * b + c;
        const double shift = denom < 0.0 ? std::clamp(0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
        hzOut = (static_cast<double>(bestBin) + shift) * binHz;
        prominenceDbOut = bestProminence;
        return true;
    }
}

namespace detail
{
    // A pitch tracker working from the period of the sound can slip onto a multiple of the true
    // period: the line then drops by an exact harmonic interval (an octave, an octave and a
    // fifth, two octaves ...) and comes back later. Such a slip is not a change of pitch. This
    // walks out from the loudest stretch of the line and, wherever the line breaks, tries those
    // intervals: if one of them makes the line carry on from where it was, the stretch after the
    // break is put back by it. A break that no harmonic interval mends is left as it is: a real
    // change of pitch.
    void repairHarmonicSlips(std::vector<float>& hz, const std::vector<float>& level)
    {
        const std::size_t n = hz.size();
        std::vector<double> midi(n, 0.0);
        std::size_t loudest = n;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (hz[i] <= 0.0f)
                continue;
            midi[i] = hzToMidi(hz[i]);
            if (loudest == n || level[i] > level[loudest])
                loudest = i;
        }
        if (loudest == n)
            return;

        // Octaves (they do not change the note) and the other harmonic ratios 3, 5 and 6 (they do).
        static constexpr double octaves[] { 12.0, 24.0, 36.0 };
        static constexpr double others[] { 19.0196, 27.8631, 31.0196 };
        constexpr double kBreak = 3.0;          // semitones between neighbouring frames
        constexpr int kOtherGap = 30;           // frames: a ratio-3 or -5 slip is only trusted close by

        const auto walk = [&](bool forwards)
        {
            const auto at = [&](std::size_t step) { return forwards ? loudest + step : loudest - step; };
            const std::size_t steps = forwards ? n - 1 - loudest : loudest;

            // The line as it settles after a break: the middle of the next few pitched frames.
            // (Right at a break the tracker passes through in-between values for a frame or two.)
            const auto settled = [&](std::size_t fromStep)
            {
                double values[8];
                int count = 0;
                for (std::size_t s = fromStep; s <= steps && count < 8; ++s)
                    if (hz[at(s)] > 0.0f)
                        values[count++] = midi[at(s)];
                std::sort(values, values + count);
                return count > 0 ? values[count / 2] : 0.0;
            };

            double offset = 0.0;                // what is being added to the stretch in hand
            double recent[8];                   // the last frames as placed
            int recentCount = 0, recentAt = 0;
            const auto remember = [&](double v)
            {
                recent[recentAt] = v;
                recentAt = (recentAt + 1) % 8;
                recentCount = std::min(recentCount + 1, 8);
            };
            const auto placedLately = [&]
            {
                double values[8];
                std::copy(recent, recent + recentCount, values);
                std::sort(values, values + recentCount);
                return values[recentCount / 2];
            };
            remember(midi[loudest]);

            double raw = midi[loudest];         // the last frame as tracked
            int gap = 0, hold = 0;
            double beforeBreak = midi[loudest];   // the line as it stood before the break in hand
            for (std::size_t step = 1; step <= steps; ++step)
            {
                const std::size_t i = at(step);
                if (hz[i] <= 0.0f)
                {
                    ++gap;
                    continue;
                }
                // A break is judged twice: when it happens, and again once the in-between frames
                // have ridden through. A slip only a few frames long is over by then: the line is
                // already back where it was, and must not keep the slip's correction.
                bool recheck = false;
                if (hold > 0)
                    recheck = --hold == 0;
                if (recheck || (hold == 0 && (std::abs(midi[i] - raw) > kBreak || gap > 3)))
                {
                    // The line breaks here. Which harmonic interval, if any, carries it on?
                    const double next = settled(step), before = recheck ? beforeBreak : placedLately();
                    if (! recheck)
                        beforeBreak = before;
                    double best = 0.0, bestJump = std::abs(next - before);
                    const double plain = bestJump;
                    const auto consider = [&](double interval, double tolerance)
                    {
                        for (double sign : { 1.0, -1.0 })
                        {
                            const double jump = std::abs(next + sign * interval - before);
                            if (jump <= tolerance && jump < bestJump)
                            {
                                bestJump = jump;
                                best = sign * interval;
                            }
                        }
                    };
                    for (double interval : octaves)
                        consider(interval, 3.0);
                    if (gap <= kOtherGap)
                        for (double interval : others)
                            consider(interval, 1.2);
                    offset = plain - bestJump >= 6.0 ? best : 0.0;
                    if (! recheck)
                        hold = 6;               // the in-between frames ride along
                }
                raw = midi[i];
                gap = 0;
                remember(midi[i] + offset);
                if (offset != 0.0)
                    hz[i] = static_cast<float>(midiToHz(midi[i] + offset));
            }
        };
        walk(true);
        walk(false);
    }
} // namespace detail

SfxAnalysis analyseSfx(const float* mono, int numSamples, double sampleRate, SfxMethod method)
{
    SfxAnalysis out;
    if (mono == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return out;
    out.durationSeconds = static_cast<double>(numSamples) / sampleRate;
    if (out.durationSeconds < 0.05)
        return out;

    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        peak = std::max(peak, std::abs(mono[i]));
    if (peak < 1.0e-4f)
        return out;

    const int numFrames = std::max(1, static_cast<int>(std::ceil(out.durationSeconds / out.hopSeconds)));

    if (method != SfxMethod::Resonance)
    {
        hindsight::Settings settings;
        settings.method = method == SfxMethod::Engine ? hindsight::Method::Engine
                        : method == SfxMethod::Tonal  ? hindsight::Method::Tonal
                                                      : hindsight::Method::Auto;
        settings.hopSeconds = out.hopSeconds;
        // The tracker's usual ceiling (1 kHz) first. Raising it lets a bright edge or chirp at the
        // front of a sound win over its body, and the line then drops by an octave or two where
        // the body takes over. Only a sound with no pitch under the ceiling is tried again higher.
        auto analysis = hindsight::analyse(mono, numSamples, sampleRate, settings);
        if (analysis.summary.pitchedShare < 0.25f)
        {
            settings.highestHz = 2000.0;
            auto higher = hindsight::analyse(mono, numSamples, sampleRate, settings);
            if (higher.summary.pitchedShare > analysis.summary.pitchedShare)
                analysis = std::move(higher);
        }

        out.hz.assign(analysis.line.frames.size(), 0.0f);
        for (std::size_t i = 0; i < analysis.line.frames.size(); ++i)
            out.hz[i] = analysis.line.frames[i].hz;

        // How loud the sound is at each frame (20 ms around it): the loud part of a sound is
        // the part whose pitch matters.
        std::vector<float> level(out.hz.size(), 0.0f);
        {
            const auto half = static_cast<std::int64_t>(0.01 * sampleRate);
            for (std::size_t i = 0; i < level.size(); ++i)
            {
                const auto centreSample = static_cast<std::int64_t>(static_cast<double>(i) * out.hopSeconds * sampleRate);
                const auto from = std::max<std::int64_t>(0, centreSample - half);
                const auto to = std::min<std::int64_t>(numSamples, centreSample + half);
                double sum = 0.0;
                for (auto k = from; k < to; ++k)
                    sum += static_cast<double>(mono[k]) * mono[k];
                level[i] = to > from ? static_cast<float>(std::sqrt(sum / static_cast<double>(to - from))) : 0.0f;
            }
        }
        detail::repairHarmonicSlips(out.hz, level);

        std::vector<float> midi;
        std::vector<std::pair<float, float>> weighted;   // pitch, level
        for (std::size_t i = 0; i < out.hz.size(); ++i)
        {
            if (out.hz[i] > 0.0f && ! analysis.line.frames[i].bridged)
            {
                midi.push_back(static_cast<float>(hzToMidi(out.hz[i])));
                weighted.emplace_back(midi.back(), level[i]);
            }
        }
        out.usedMethod = analysis.usedMethod == hindsight::Method::Engine ? SfxMethod::Engine : SfxMethod::Tonal;
        out.tonalness = std::clamp(analysis.summary.pitchedShare * analysis.summary.meanConfidence, 0.0f, 1.0f);

        // Enough of the sound must carry a pitch for the pitch to mean anything.
        const bool engineTrusted = out.usedMethod != SfxMethod::Engine || analysis.engineOnGrid;
        out.pitched = analysis.summary.pitchedShare >= 0.25f && midi.size() >= 8 && engineTrusted;
        if (out.pitched)
        {
            // The pitch the sound sits on: the middle of its line, counted by level, so a loud
            // body outweighs a quiet tail however long the tail is.
            std::sort(weighted.begin(), weighted.end());
            double total = 0.0;
            for (const auto& w : weighted)
                total += w.second;
            double running = 0.0;
            out.centreMidi = static_cast<double>(median(midi));
            for (const auto& w : weighted)
            {
                running += w.second;
                if (total > 0.0 && running >= 0.5 * total)
                {
                    out.centreMidi = static_cast<double>(w.first);
                    break;
                }
            }
            out.driftSemitones = percentile(midi, 0.95) - percentile(midi, 0.05);
            out.valid = true;
            return out;
        }
        if (method != SfxMethod::Auto)
        {
            // The method the user asked for found nothing to hold on to: say so, pitch unknown.
            out.valid = true;
            return out;
        }
    }

    double hz = 0.0, prominence = 0.0;
    out.usedMethod = SfxMethod::Resonance;
    out.hz.assign(static_cast<std::size_t>(numFrames), 0.0f);
    if (findResonance(mono, numSamples, sampleRate, hz, prominence) && hz > 0.0)
    {
        std::fill(out.hz.begin(), out.hz.end(), static_cast<float>(hz));
        out.centreMidi = hzToMidi(hz);
        out.driftSemitones = 0.0f;
        // A ring 6 dB proud of its surroundings is barely there; 30 dB is a clear tone.
        out.tonalness = static_cast<float>(std::clamp((prominence - 6.0) / 24.0, 0.0, 1.0));
        out.pitched = prominence >= 6.0;
    }
    out.valid = true;
    return out;
}

} // namespace pitchflattener::core
