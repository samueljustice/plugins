#include "Analysis.h"
#include "HindsightTracker.h"

#include <algorithm>
#include <complex>
#include <cstdint>
#include <limits>
#include <numbers>
#include <numeric>

namespace tonalrecall::core
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

    int wrap12(int v) noexcept { return ((v % 12) + 12) % 12; }

    // Key profiles (Krumhansl and Kessler's probe-tone ratings).
    constexpr std::array<float, 12> kMajor { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
    constexpr std::array<float, 12> kMinor { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

    float correlation(const std::array<float, 12>& chroma, const std::array<float, 12>& profile, int tonic)
    {
        float meanC = 0.0f, meanP = 0.0f;
        for (int i = 0; i < 12; ++i)
        {
            meanC += chroma[static_cast<std::size_t>(i)];
            meanP += profile[static_cast<std::size_t>(i)];
        }
        meanC /= 12.0f;
        meanP /= 12.0f;
        float num = 0.0f, dc = 0.0f, dp = 0.0f;
        for (int i = 0; i < 12; ++i)
        {
            const float c = chroma[static_cast<std::size_t>(wrap12(i + tonic))] - meanC;
            const float p = profile[static_cast<std::size_t>(i)] - meanP;
            num += c * p;
            dc += c * c;
            dp += p * p;
        }
        return dc > 0.0f && dp > 0.0f ? num / std::sqrt(dc * dp) : 0.0f;
    }

    // The fine pitch grid the salience is built on: 10 cents a bin from MIDI 21 (A0).
    constexpr int kFineLow = 21;
    constexpr int kFineHigh = 108;
    constexpr int kFinePerSemitone = 10;
    constexpr int kFineBins = (kFineHigh - kFineLow) * kFinePerSemitone + 1;
}

//==============================================================================
const char* pitchClassName(int pitchClass) noexcept
{
    static constexpr const char* names[12] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[wrap12(pitchClass)];
}

std::string noteName(double midi, int* centsOut)
{
    const int nearest = static_cast<int>(std::lround(midi));
    if (centsOut != nullptr)
        *centsOut = static_cast<int>(std::lround((midi - nearest) * 100.0));
    return std::string(pitchClassName(nearest)) + std::to_string(nearest / 12 - 1);
}

std::string MusicAnalysis::keyName() const
{
    return std::string(pitchClassName(rootPitchClass)) + (minor ? " minor" : " major");
}

double MusicAnalysis::pitchNear(int pitchClass, double nearMidi) const noexcept
{
    const double base = static_cast<double>(wrap12(pitchClass)) + static_cast<double>(tuningCents) / 100.0;
    return base + 12.0 * std::round((nearMidi - base) / 12.0);
}

//==============================================================================
MusicAnalysis analyseMusic(const float* mono, int numSamples, double sampleRate)
{
    MusicAnalysis out;
    if (mono == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return out;
    out.durationSeconds = static_cast<double>(numSamples) / sampleRate;
    if (out.durationSeconds < 0.1)
        return out;

    // A third of a second per frame: bass notes a semitone apart are only a few hertz apart.
    const std::size_t fftSize = nextPow2(static_cast<std::size_t>(0.3 * sampleRate));
    const auto hop = static_cast<std::int64_t>(fftSize / 8);
    const auto half = static_cast<std::int64_t>(fftSize / 2);
    const int numFrames = std::max(1, static_cast<int>((static_cast<std::int64_t>(numSamples) + hop - 1) / hop));
    out.hopSeconds = static_cast<double>(hop) / sampleRate;

    SpectrumFrames spectrum(sampleRate, fftSize);

    // Pass 1: every frame's partials.
    std::vector<std::vector<Peak>> framePeaks(static_cast<std::size_t>(numFrames));
    float loudest = 0.0f;
    for (int f = 0; f < numFrames; ++f)
    {
        const auto& mag = spectrum.magnitudes(mono, numSamples, static_cast<std::int64_t>(f) * hop - half + hop / 2);
        spectrum.peaks(mag, 27.0, 5000.0, 1.0e-5f, 0.003f, framePeaks[static_cast<std::size_t>(f)]);
        for (const auto& p : framePeaks[static_cast<std::size_t>(f)])
            loudest = std::max(loudest, p.amp);
    }
    if (loudest < 1.0e-4f)
        return out;   // silence

    // Tuning: where the partials sit between the semitones of A = 440, as a circular mean (a
    // partial 49 cents sharp and one 49 cents flat are neighbours, not opposites). Only the
    // lower partials vote: high harmonics of a low note are not on the note grid at all.
    {
        double sx = 0.0, sy = 0.0;
        for (const auto& peaks : framePeaks)
        {
            float top = 0.0f;
            for (const auto& p : peaks)
                top = std::max(top, p.amp);
            for (const auto& p : peaks)
            {
                if (p.hz > 1500.0f || p.amp < top * 0.1f)
                    continue;
                const double midi = hzToMidi(p.hz);
                const double angle = 2.0 * kPi * (midi - std::floor(midi));
                sx += static_cast<double>(p.amp) * std::cos(angle);
                sy += static_cast<double>(p.amp) * std::sin(angle);
            }
        }
        double cents = std::atan2(sy, sx) / (2.0 * kPi) * 100.0;
        if (cents > 50.0)
            cents -= 100.0;
        out.tuningCents = static_cast<float>(cents);
    }
    const double tuning = static_cast<double>(out.tuningCents) / 100.0;   // semitones

    // Pass 2: the notes sounding in each frame, one at a time. The strongest note is the pitch
    // whose harmonics collect the most level; its partials are then taken out of the frame and
    // the next strongest is looked for in what is left. Taking them out is what stops a note's
    // overtones being read as notes of their own (the fifth and the third above every note are
    // in its harmonics), and it leaves a short, clean list instead of a cloud.
    out.laneLowMidi = 24;
    out.lanePitches = 73;   // C1..C7
    out.laneFrames = numFrames;
    out.lane.assign(static_cast<std::size_t>(numFrames) * static_cast<std::size_t>(out.lanePitches), 0.0f);

    std::vector<double> noteStrength(128, 0.0);
    struct FramePitch { float midi; float salience; };
    std::vector<std::vector<FramePitch>> framePitches(static_cast<std::size_t>(numFrames));
    std::vector<float> frameWeights(static_cast<std::size_t>(numFrames), 0.0f);

    constexpr int kMaxNotes = 6;
    constexpr int kPartials = 8;
    constexpr double kToleranceCents = 40.0;   // partials of real instruments wander (vibrato, tremolo, stretch)
    constexpr double kOvertoneToleranceCents = 25.0;
    std::vector<Peak> work;
    std::vector<int> matched(static_cast<std::size_t>(kPartials));

    for (int f = 0; f < numFrames; ++f)
    {
        work = framePeaks[static_cast<std::size_t>(f)];   // sorted by frequency
        if (work.empty())
            continue;
        float frameLevel = 0.0f;
        for (auto& p : work)
        {
            frameLevel = std::max(frameLevel, p.amp);
            p.amp = std::sqrt(p.amp);   // compressed: a quiet partial still counts
        }

        // The strongest partial within the tolerance of a frequency (its index, or -1).
        const auto partialNear = [&](double hz, double toleranceCents = kToleranceCents)
        {
            const double lo = hz * std::exp2(-toleranceCents / 1200.0), hi = hz * std::exp2(toleranceCents / 1200.0);
            auto it = std::lower_bound(work.begin(), work.end(), lo, [](const Peak& p, double v) { return static_cast<double>(p.hz) < v; });
            int best = -1;
            for (; it != work.end() && static_cast<double>(it->hz) <= hi; ++it)
                if (it->amp > 0.0f && (best < 0 || it->amp > work[static_cast<std::size_t>(best)].amp))
                    best = static_cast<int>(it - work.begin());
            return best;
        };

        auto& pitches = framePitches[static_cast<std::size_t>(f)];
        float firstSalience = 0.0f;
        for (int found = 0; found < kMaxNotes; ++found)
        {
            float bestSalience = 0.0f;
            int bestBin = -1;
            for (int bin = 0; bin < kFineBins; ++bin)
            {
                const double f0 = midiToHz(kFineLow + static_cast<double>(bin) / kFinePerSemitone);
                // A note must have its fundamental: without this, every note would also be found
                // an octave below itself, where only the even "harmonics" exist.
                const int fundamental = partialNear(f0);
                if (fundamental < 0)
                    continue;
                const float a1 = work[static_cast<std::size_t>(fundamental)].amp;
                float sum = a1, strongest = a1, weight = 0.85f;
                float odd = a1, even = 0.0f;
                for (int h = 2; h <= kPartials; ++h, weight *= 0.85f)
                {
                    // Overtones are held closer than the fundamental: with a wide net a candidate
                    // borrows other notes' partials as its own.
                    const int at = partialNear(f0 * h, kOvertoneToleranceCents);
                    if (at < 0)
                        continue;
                    const float a = work[static_cast<std::size_t>(at)].amp;
                    sum += a * weight;
                    strongest = std::max(strongest, a);
                    if (h <= 6)
                        (h % 2 == 1 ? odd : even) += a;
                }
                // ... and not a vanishing one (24 dB under its loudest partial is the limit).
                if (a1 < 0.25f * strongest)
                    continue;
                // An octave below a real note, only the even partials exist (they are the real
                // note's own harmonics) and the odd ones are missing or faint: a ghost, even if
                // some hum or rumble happens to sit where its fundamental would be. A real note
                // carries its odd partials too (counted over the first six).
                if (odd < 0.6f * even)
                    continue;
                if (sum > bestSalience)
                {
                    bestSalience = sum;
                    bestBin = bin;
                }
            }
            if (bestBin < 0)
                break;
            if (found == 0)
                firstSalience = bestSalience;
            else if (bestSalience < 0.3f * firstSalience)
                break;

            // The note's pitch from its lower partials, each brought down to the fundamental;
            // then its partials leave the frame.
            const double f0 = midiToHz(kFineLow + static_cast<double>(bestBin) / kFinePerSemitone);
            double num = 0.0, den = 0.0;
            for (int h = 1; h <= kPartials; ++h)
            {
                const int at = partialNear(f0 * h, h == 1 ? kToleranceCents : kOvertoneToleranceCents);
                matched[static_cast<std::size_t>(h - 1)] = at;
                if (at >= 0 && h <= 3)
                {
                    const auto& p = work[static_cast<std::size_t>(at)];
                    num += static_cast<double>(p.amp) * hzToMidi(static_cast<double>(p.hz) / h);
                    den += p.amp;
                }
            }
            for (int at : matched)
                if (at >= 0)
                    work[static_cast<std::size_t>(at)].amp = 0.0f;
            pitches.push_back({ static_cast<float>(den > 0.0 ? num / den : hzToMidi(f0)), bestSalience });
        }

        // Loudness of the frame scales its say in the totals, so a quiet tail does not outvote
        // the body of the music.
        const double frameWeight = std::sqrt(static_cast<double>(frameLevel / loudest));
        frameWeights[static_cast<std::size_t>(f)] = static_cast<float>(frameWeight);
        for (const auto& p : pitches)
        {
            const int note = static_cast<int>(std::lround(static_cast<double>(p.midi) - tuning));
            if (note < 0 || note > 127)
                continue;
            const double s = static_cast<double>(p.salience / firstSalience) * frameWeight;
            noteStrength[static_cast<std::size_t>(note)] += s;
            const int lanePitch = note - out.laneLowMidi;
            if (lanePitch >= 0 && lanePitch < out.lanePitches)
            {
                auto& cell = out.lane[static_cast<std::size_t>(f) * static_cast<std::size_t>(out.lanePitches) + static_cast<std::size_t>(lanePitch)];
                cell = std::max(cell, static_cast<float>(s));
            }
        }
    }

    // Pitch classes.
    std::array<double, 12> chroma {};
    for (int note = 0; note < 128; ++note)
        chroma[static_cast<std::size_t>(note % 12)] += noteStrength[static_cast<std::size_t>(note)];
    const double chromaTop = *std::max_element(chroma.begin(), chroma.end());
    if (chromaTop <= 0.0)
        return out;
    for (int i = 0; i < 12; ++i)
        out.chroma[static_cast<std::size_t>(i)] = static_cast<float>(chroma[static_cast<std::size_t>(i)] / chromaTop);

    const double noteTop = *std::max_element(noteStrength.begin(), noteStrength.end());
    const auto strongestOctaveOf = [&](int pitchClass)
    {
        int best = -1;
        for (int note = wrap12(pitchClass); note < 128; note += 12)
            if (best < 0 || noteStrength[static_cast<std::size_t>(note)] > noteStrength[static_cast<std::size_t>(best)])
                best = note;
        return best;
    };

    // The root. With many notes in play the key profiles decide. With only a few (a drone, a
    // power chord, a braam) there is no key to speak of, and the note in the bass is the root.
    int significant = 0;
    for (float c : out.chroma)
        significant += c >= 0.3f ? 1 : 0;

    float best = -2.0f, second = -2.0f;
    int bestTonic = 0;
    bool bestMinor = false;
    for (int tonic = 0; tonic < 12; ++tonic)
    {
        for (int mode = 0; mode < 2; ++mode)
        {
            const float r = correlation(out.chroma, mode == 0 ? kMajor : kMinor, tonic);
            if (r > best)
            {
                second = best;
                best = r;
                bestTonic = tonic;
                bestMinor = mode == 1;
            }
            else if (r > second)
            {
                second = r;
            }
        }
    }

    if (significant <= 3)
    {
        int bass = -1;
        for (int note = 0; note < 128 && bass < 0; ++note)
            if (noteStrength[static_cast<std::size_t>(note)] >= 0.5 * noteTop)
                bass = note;
        out.rootPitchClass = wrap12(std::max(0, bass));
        const float minorThird = out.chroma[static_cast<std::size_t>(wrap12(out.rootPitchClass + 3))];
        const float majorThird = out.chroma[static_cast<std::size_t>(wrap12(out.rootPitchClass + 4))];
        out.minor = minorThird >= majorThird;   // a bare note or fifth reads as minor: the darker default
        out.keyConfidence = std::clamp(out.chroma[static_cast<std::size_t>(out.rootPitchClass)], 0.0f, 1.0f) * 0.9f;
    }
    else
    {
        out.rootPitchClass = bestTonic;
        out.minor = bestMinor;
        out.keyConfidence = std::clamp(best, 0.0f, 1.0f) * (0.5f + 0.5f * std::clamp((best - second) / 0.15f, 0.0f, 1.0f));
    }

    const auto roleOf = [&](int pitchClass)
    {
        const int interval = wrap12(pitchClass - out.rootPitchClass);
        if (interval == 0) return NoteRole::Root;
        if (interval == 7) return NoteRole::Fifth;
        if (interval == (out.minor ? 3 : 4)) return NoteRole::Third;
        return NoteRole::Other;
    };

    for (int pc = 0; pc < 12; ++pc)
    {
        const float strength = out.chroma[static_cast<std::size_t>(pc)];
        if (strength < 0.2f && pc != out.rootPitchClass)
            continue;
        NoteCandidate n;
        n.pitchClass = pc;
        n.strength = strength;
        n.role = roleOf(pc);
        n.midi = static_cast<double>(strongestOctaveOf(pc)) + tuning;
        out.notes.push_back(n);
    }
    std::sort(out.notes.begin(), out.notes.end(), [](const auto& a, const auto& b)
    {
        if (a.strength > b.strength) return true;
        if (a.strength < b.strength) return false;
        return a.pitchClass < b.pitchClass;
    });
    if (out.notes.size() > 6)
    {
        // Keep the six strongest, but never drop the root.
        const auto root = std::find_if(out.notes.begin(), out.notes.end(), [&](const auto& n) { return n.pitchClass == out.rootPitchClass; });
        if (root - out.notes.begin() >= 6)
            std::iter_swap(out.notes.begin() + 5, root);
        out.notes.resize(6);
    }

    const int rootNote = strongestOctaveOf(out.rootPitchClass);
    out.rootMidi = static_cast<double>(rootNote) + tuning;

    // The root's drift: its measured pitch per frame against its own median.
    {
        std::vector<float> measured(static_cast<std::size_t>(numFrames), std::numeric_limits<float>::quiet_NaN());
        std::vector<float> present;
        for (int f = 0; f < numFrames; ++f)
        {
            float bestSalience = 0.0f;
            for (const auto& p : framePitches[static_cast<std::size_t>(f)])
            {
                if (std::abs(static_cast<double>(p.midi) - out.rootMidi) <= 1.5 && p.salience > bestSalience)
                {
                    bestSalience = p.salience;
                    measured[static_cast<std::size_t>(f)] = p.midi;
                }
            }
            if (bestSalience > 0.0f)
                present.push_back(measured[static_cast<std::size_t>(f)]);
        }
        out.rootDriftCents.assign(static_cast<std::size_t>(numFrames), 0.0f);
        if (! present.empty())
        {
            const float centre = median(present);
            // Hold the last reading through frames where the root is not sounding (and the first
            // reading back to the start).
            float held = std::numeric_limits<float>::quiet_NaN();
            for (int f = 0; f < numFrames; ++f)
            {
                if (! std::isnan(measured[static_cast<std::size_t>(f)]))
                    held = measured[static_cast<std::size_t>(f)];
                if (! std::isnan(held))
                    out.rootDriftCents[static_cast<std::size_t>(f)] = (held - centre) * 100.0f;
            }
            for (int f = 0; f < numFrames && std::isnan(measured[static_cast<std::size_t>(f)]); ++f)
            {
                int next = f;
                while (next < numFrames && std::isnan(measured[static_cast<std::size_t>(next)]))
                    ++next;
                if (next < numFrames)
                    out.rootDriftCents[static_cast<std::size_t>(f)] = (measured[static_cast<std::size_t>(next)] - centre) * 100.0f;
            }
        }
    }

    // How strongly each note sounds in each frame.
    out.frameNoteLevels.assign(static_cast<std::size_t>(numFrames) * 12, 0);
    for (int f = 0; f < numFrames; ++f)
    {
        const auto& pitches = framePitches[static_cast<std::size_t>(f)];
        if (pitches.empty() || frameWeights[static_cast<std::size_t>(f)] <= 0.2f)
            continue;
        const float top = pitches[0].salience;
        for (const auto& p : pitches)
        {
            const int pc = wrap12(static_cast<int>(std::lround(static_cast<double>(p.midi) - tuning)));
            auto& cell = out.frameNoteLevels[static_cast<std::size_t>(f) * 12 + static_cast<std::size_t>(pc)];
            cell = std::max(cell, static_cast<std::uint8_t>(std::lround(255.0f * std::clamp(p.salience / top, 0.0f, 1.0f))));
        }
    }

    // The lead line. First, which notes ever carry the music: the strongest note of each frame,
    // steadied over a few frames, kept where it lasts.
    {
        std::vector<int> top(static_cast<std::size_t>(numFrames), -1);
        for (int f = 0; f < numFrames; ++f)
            if (! framePitches[static_cast<std::size_t>(f)].empty() && frameWeights[static_cast<std::size_t>(f)] > 0.2f)
                top[static_cast<std::size_t>(f)] = static_cast<int>(std::lround(static_cast<double>(framePitches[static_cast<std::size_t>(f)][0].midi) - tuning));

        std::vector<int> steady(top.size(), -1);
        for (int f = 0; f < numFrames; ++f)
        {
            int commonest = top[static_cast<std::size_t>(f)], bestCount = 0;
            for (int a = std::max(0, f - 2); a <= std::min(numFrames - 1, f + 2); ++a)
            {
                const int v = top[static_cast<std::size_t>(a)];
                if (v < 0)
                    continue;
                int count = 0;
                for (int b = std::max(0, f - 2); b <= std::min(numFrames - 1, f + 2); ++b)
                    count += top[static_cast<std::size_t>(b)] == v ? 1 : 0;
                if (count > bestCount)
                {
                    bestCount = count;
                    commonest = v;
                }
            }
            steady[static_cast<std::size_t>(f)] = commonest;
        }

        // (Up to eight of them: the ones that carry it for longest in all.)
        std::vector<std::pair<int, int>> carried;   // frames carried, note
        for (int f = 0; f < numFrames;)
        {
            int end = f;
            while (end < numFrames && steady[static_cast<std::size_t>(end)] == steady[static_cast<std::size_t>(f)])
                ++end;
            const int note = steady[static_cast<std::size_t>(f)];
            if (note >= 0 && end - f >= 4)
            {
                auto it = std::find_if(carried.begin(), carried.end(), [&](const auto& entry) { return entry.second == note; });
                if (it == carried.end())
                    carried.emplace_back(end - f, note);
                else
                    it->first += end - f;
            }
            f = end;
        }
        std::sort(carried.begin(), carried.end(), [](const auto& x, const auto& y) { return x.first != y.first ? x.first > y.first : x.second < y.second; });
        if (carried.size() > 8)
            carried.resize(8);
        std::vector<int> candidates;
        for (const auto& entry : carried)
            candidates.push_back(entry.second);

        if (candidates.size() == 1)
        {
            out.leadNotes.push_back({ 0.0, static_cast<double>(candidates[0]) + tuning });
        }
        else if (candidates.size() >= 2)
        {
            // Then when each of them takes over, on a much shorter frame than the note analysis
            // uses: note changes have to land in time, to a few tens of milliseconds.
            int lowest = candidates[0];
            for (int note : candidates)
                lowest = std::min(lowest, note);
            const std::size_t fineSize = nextPow2(static_cast<std::size_t>((midiToHz(lowest) < 200.0 ? 0.17 : 0.085) * sampleRate));
            const auto fineHop = static_cast<std::int64_t>(fineSize / 8);
            SpectrumFrames fine(sampleRate, fineSize);
            const double binHz = sampleRate / static_cast<double>(fineSize);

            // A note's level: its first `partials` partials (4 to judge which note leads; 1, the
            // fundamental alone, to judge whether a note has let go: upper partials are shared
            // between the notes of a chord and beat against each other, the fundamental is the
            // note's own).
            const auto score = [&](const std::vector<float>& mag, int note, int partials)
            {
                const double f0 = midiToHz(static_cast<double>(note) + tuning);
                double total = 0.0, weight = 1.0;
                for (int h = 1; h <= partials; ++h, weight *= 0.8)
                {
                    const double centre = f0 * h;
                    const auto lo = static_cast<std::size_t>(std::max(1.0, std::floor(centre * std::exp2(-35.0 / 1200.0) / binHz)));
                    const auto hi = std::min(mag.size() - 1, static_cast<std::size_t>(std::ceil(centre * std::exp2(35.0 / 1200.0) / binHz)));
                    float peak = 0.0f;
                    for (std::size_t k = lo; k <= hi; ++k)
                        peak = std::max(peak, mag[k]);
                    total += weight * static_cast<double>(peak);   // plain level: a note that stops must be seen to drop
                }
                return total;
            };

            int current = -1, pending = -1, pendingCount = 0;
            double pendingTime = 0.0;
            double held = 0.0;   // how strong the note in hand has been lately (its recent peak)
            // Only frames that lie wholly inside the music: one hanging over either end reads a
            // falling level as notes letting go.
            for (std::int64_t start = 0; start + static_cast<std::int64_t>(fineSize) <= numSamples; start += fineHop)
            {
                const auto& mag = fine.magnitudes(mono, numSamples, start);
                const double time = static_cast<double>(start + static_cast<std::int64_t>(fineSize) / 2) / sampleRate;
                float level = 0.0f;
                for (float v : mag)
                    level = std::max(level, v);
                if (level < 0.03f * loudest)
                    continue;   // too quiet to say: the note in hand carries on

                int leader = candidates[0];
                double leaderScore = -1.0, currentScore = 0.0;
                for (int note : candidates)
                {
                    const double s = score(mag, note, 4);
                    if (s > leaderScore)
                    {
                        leaderScore = s;
                        leader = note;
                    }
                    if (note == current)
                        currentScore = s;
                }

                if (current < 0)
                {
                    current = leader;
                    held = score(mag, leader, 1);
                    out.leadNotes.push_back({ 0.0, static_cast<double>(leader) + tuning });
                    continue;
                }

                const double currentOwn = score(mag, current, 1);
                if (leader != current && leaderScore > 1.25 * currentScore && currentOwn < 0.5 * held)
                {
                    // A new note takes over only when it leads clearly, for two frames running,
                    // AND the note in hand has let go. In a held chord the notes trade places in
                    // loudness all the time without any of them stopping: that is not a melody,
                    // and following it would make the effect warble.
                    if (leader != pending)
                    {
                        pending = leader;
                        pendingCount = 0;
                        pendingTime = time;
                    }
                    if (++pendingCount >= 2)
                    {
                        current = leader;
                        held = score(mag, leader, 1);
                        out.leadNotes.push_back({ pendingTime, static_cast<double>(leader) + tuning });
                        pending = -1;
                    }
                }
                else
                {
                    pending = -1;
                    // What "letting go" is measured against: the note's own recent peak. (An
                    // average would follow the note down as it fades, and it could then never be
                    // seen to have let go.)
                    held = std::max(currentOwn, held * 0.99);
                }
            }

            // A note that lasts under 120 ms is a flicker between two others, not a note.
            for (std::size_t k = 1; k + 1 < out.leadNotes.size();)
            {
                if (out.leadNotes[k + 1].startSeconds - out.leadNotes[k].startSeconds < 0.12)
                    out.leadNotes.erase(out.leadNotes.begin() + static_cast<std::ptrdiff_t>(k));
                else
                    ++k;
            }
            // The same note in another octave is not a new note (instruments double at the octave,
            // and which octave reads strongest shifts about).
            const auto sameNote = [](double a, double b)
            {
                const double d = std::abs(a - b);
                return std::abs(d - 12.0 * std::round(d / 12.0)) < 0.5;
            };
            for (std::size_t k = 1; k < out.leadNotes.size();)
            {
                if (sameNote(out.leadNotes[k].midi, out.leadNotes[k - 1].midi))
                    out.leadNotes.erase(out.leadNotes.begin() + static_cast<std::ptrdiff_t>(k));
                else
                    ++k;
            }
        }
    }

    // The music's continuous pitch movement: the lead voice's pitch, followed finely from frame
    // to frame, so a glide between notes or a scoop into one is kept as the curve it is.
    if (! out.leadNotes.empty())
    {
        double lowest = out.leadNotes[0].midi;
        for (const auto& note : out.leadNotes)
            lowest = std::min(lowest, note.midi);
        const std::size_t size = nextPow2(static_cast<std::size_t>((midiToHz(lowest) < 200.0 ? 0.17 : 0.085) * sampleRate));
        const auto step = static_cast<std::int64_t>(size / 8);
        SpectrumFrames frames(sampleRate, size);
        const double binHz = sampleRate / static_cast<double>(size);

        out.contourHopSeconds = static_cast<double>(step) / sampleRate;
        out.contourStartSeconds = 0.5 * static_cast<double>(size) / sampleRate;
        std::size_t note = 0;
        double previous = out.leadNotes[0].midi;
        for (std::int64_t start = 0; start + static_cast<std::int64_t>(size) <= numSamples; start += step)
        {
            const auto& mag = frames.magnitudes(mono, numSamples, start);
            const double time = static_cast<double>(start + static_cast<std::int64_t>(size) / 2) / sampleRate;
            while (note + 1 < out.leadNotes.size() && out.leadNotes[note + 1].startSeconds <= time)
                ++note;
            // Carry on from where the voice was, unless the lead has moved to a note far from
            // there (then pick the voice up at the new note).
            const double anchor = out.leadNotes[note].midi;
            if (std::abs(previous - anchor) > 3.5)
                previous = anchor;

            float frameTop = 0.0f;
            for (float v : mag)
                frameTop = std::max(frameTop, v);

            // The voice's strongest partial near where it was (among its first four), brought
            // down to the fundamental. Higher partials place the pitch more finely.
            double bestPitch = previous;
            float bestLevel = 0.0f;
            for (int partial = 1; partial <= 4; ++partial)
            {
                const double centre = midiToHz(previous) * partial;
                const auto lo = static_cast<std::size_t>(std::max(2.0, std::floor(centre * std::exp2(-3.0 / 12.0) / binHz)));
                const auto hi = std::min(mag.size() - 2, static_cast<std::size_t>(std::ceil(centre * std::exp2(3.0 / 12.0) / binHz)));
                for (std::size_t k = lo; k <= hi; ++k)
                {
                    if (mag[k] <= bestLevel || mag[k] < mag[k - 1] || mag[k] < mag[k + 1])
                        continue;
                    const double a = std::log(std::max(1.0e-12f, mag[k - 1])), b = std::log(std::max(1.0e-12f, mag[k])), cc = std::log(std::max(1.0e-12f, mag[k + 1]));
                    const double denom = a - 2.0 * b + cc;
                    const double shift = denom < 0.0 ? std::clamp(0.5 * (a - cc) / denom, -0.5, 0.5) : 0.0;
                    bestLevel = mag[k];
                    bestPitch = hzToMidi((static_cast<double>(k) + shift) * binHz / partial);
                }
            }
            // Too quiet to say where the voice is: it stays where it was.
            if (bestLevel >= 0.1f * frameTop && frameTop >= 0.03f * loudest)
                previous = bestPitch;
            out.contourMidi.push_back(static_cast<float>(previous));
        }
    }

    out.valid = true;
    return out;
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

        // How loud the sound is at each frame (20 ms around it): the loud part of an effect is
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
            // The pitch the effect sits on: the middle of its line, counted by level, so a loud
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

//==============================================================================
MatchResult match(const MusicAnalysis& music, const SfxAnalysis& sfx, const MatchOptions& options)
{
    MatchResult out;
    if (! music.valid || ! sfx.valid || ! sfx.pitched)
        return out;

    const double centre = sfx.centreMidi;
    int pitchClass = music.rootPitchClass;
    switch (options.target)
    {
        case Target::Root:       pitchClass = music.rootPitchClass; break;
        case Target::Fifth:      pitchClass = music.rootPitchClass + 7; break;
        case Target::Third:      pitchClass = music.rootPitchClass + (music.minor ? 3 : 4); break;
        case Target::PitchClass: pitchClass = options.pitchClass; break;
        case Target::Follow:      // falls back to the nearest note where there is no line to follow
        case Target::Nearest:
        {
            // The clearly sounding note of the music that needs the smallest move.
            double bestDistance = 1.0e9;
            for (const auto& n : music.notes)
            {
                if (n.strength < 0.35f && n.pitchClass != music.rootPitchClass)
                    continue;
                const double distance = std::abs(music.pitchNear(n.pitchClass, centre) - centre);
                if (distance < bestDistance - 1.0e-9)
                {
                    bestDistance = distance;
                    pitchClass = n.pitchClass;
                }
            }
            break;
        }
    }
    pitchClass = wrap12(pitchClass);

    out.targetPitchClass = pitchClass;
    out.targetMidi = music.pitchNear(pitchClass, centre) + 12.0 * options.octaveShift;
    out.semitones = out.targetMidi - centre;
    out.speedRatio = std::exp2(out.semitones / 12.0);
    out.hopSeconds = sfx.hopSeconds;

    const std::size_t n = sfx.hz.size();
    out.curve.assign(n, static_cast<float>(out.semitones));
    const double offset = options.offsetKnown ? options.sfxOffsetInMusicSeconds : 0.0;

    // Follow: the effect steps through the music's lead line, each note at the moment the music
    // plays it. The line is kept whole (its leaps and its direction) and put in the octave that
    // asks the least of the effect. Past either end of the learned music, the nearest note holds.
    // Follow: the effect rests on the nearest note the music is sounding, and stays on it for as
    // long as the music keeps sounding it. Only when that note has been gone for a while does the
    // effect move, to the nearest note sounding then. So it settles into the harmony and moves
    // with it, by the smallest steps there are, rather than playing the tune.
    struct Rest { double startSeconds, midi; };
    std::vector<Rest> line;
    if (options.target == Target::Follow && n > 0 && ! music.frameNoteLevels.empty() && music.hopSeconds > 0.0)
    {
        const auto frames = static_cast<std::ptrdiff_t>(music.frameNoteLevels.size() / 12);
        const auto frameAt = [&](double seconds) { return std::clamp(static_cast<std::ptrdiff_t>(std::floor(seconds / music.hopSeconds)), std::ptrdiff_t { 0 }, frames - 1); };
        const auto levelOf = [&](std::ptrdiff_t f, int pc) { return static_cast<double>(music.frameNoteLevels[static_cast<std::size_t>(f) * 12 + static_cast<std::size_t>(pc)]) / 255.0; };
        const double effectSeconds = static_cast<double>(n) * sfx.hopSeconds;
        const auto firstFrame = frameAt(offset), lastFrame = frameAt(offset + effectSeconds);

        // The nearest (smallest move) of the notes leading a frame (at least half as strong as
        // its strongest); -1 if the frame says nothing.
        // (The smallest move from `from`: the effect's own pitch to begin with, and after that the
        // note it is resting on, so a step in the music is a step for the effect, never a leap to
        // the far side of the octave.)
        const auto nearestIn = [&](std::ptrdiff_t f, double from)
        {
            int best = -1;
            double bestMove = 1.0e9;
            for (int pc = 0; pc < 12; ++pc)
            {
                if (levelOf(f, pc) < 0.5)
                    continue;
                const double move = std::abs(music.pitchNear(pc, from) - from);
                if (move < bestMove - 1.0e-9)
                {
                    bestMove = move;
                    best = pc;
                }
            }
            return best;
        };

        // A change is timed by the lead line where it has a note starting close by (it is timed
        // far more finely than the note analysis).
        const auto timed = [&](double seconds)
        {
            double best = seconds, bestGap = 0.25;
            for (const auto& note : music.leadNotes)
            {
                const double gap = std::abs(note.startSeconds - seconds);
                if (gap < bestGap)
                {
                    bestGap = gap;
                    best = note.startSeconds;
                }
            }
            return best;
        };

        int current = -1;
        double resting = centre;        // the pitch the effect is resting on
        double held = 0.0;              // how strong the note in hand has been lately (its recent peak)
        std::ptrdiff_t goneSince = -1;
        for (auto f = firstFrame; f <= lastFrame; ++f)
        {
            bool quiet = true;
            for (int pc = 0; pc < 12 && quiet; ++pc)
                quiet = levelOf(f, pc) <= 0.0;
            if (quiet)
                continue;   // too quiet to say: whatever was sounding carries on
            if (current < 0)
            {
                current = nearestIn(f, centre);
                if (current >= 0)
                {
                    held = levelOf(f, current);
                    resting = music.pitchNear(current, centre);
                    line.push_back({ -1.0e9, resting });
                }
                continue;
            }
            // The note in hand is still there while it keeps at least half the strength it had:
            // a note fading into the room, or sitting under a louder one, has let go.
            const double now = levelOf(f, current);
            if (now >= 0.5 * held)
            {
                held = std::max(now, held * 0.98);
                goneSince = -1;
                continue;
            }
            if (goneSince < 0)
                goneSince = f;
            if (static_cast<double>(f - goneSince + 1) * music.hopSeconds < options.shortestNoteSeconds)
                continue;
            const int next = nearestIn(f, resting);
            if (next >= 0 && next != current)
            {
                // The move is timed to when the new note came in, not to when the wait ran out.
                auto cameIn = f;
                while (cameIn > goneSince && levelOf(cameIn - 1, next) >= 0.5)
                    --cameIn;
                current = next;
                held = levelOf(f, next);
                // Next to where the effect is; if a run of steps has carried it more than a
                // tritone and a half from its own pitch, the octave nearer home instead.
                resting = music.pitchNear(next, resting);
                if (std::abs(resting - centre) > 9.0)
                    resting = music.pitchNear(next, centre);
                line.push_back({ timed(static_cast<double>(cameIn) * music.hopSeconds), resting });
            }
            else if (next == current)
            {
                held = now;   // it is still the nearest of what leads: carry on with it as it is
            }
            goneSince = -1;
        }
    }

    const bool following = line.size() >= 2;
    if (following)
    {
        const auto leadAt = [&](double musicSeconds)
        {
            std::size_t at = 0;
            while (at + 1 < line.size() && line[at + 1].startSeconds <= musicSeconds)
                ++at;
            return at;
        };
        const std::size_t first = leadAt(offset);
        std::vector<double> placed(line.size());
        for (std::size_t k = 0; k < line.size(); ++k)
            placed[k] = line[k].midi;

        // The line as corner points in music time. With no glide each note change is a step; with
        // glide, the step opens into a ramp centred on the change, as wide as that share of the
        // shorter of the two notes: at full glide the line runs straight from the middle of one
        // note to the middle of the next.
        const double glide = std::clamp(options.glide, 0.0, 1.0);
        const auto noteLength = [&](std::size_t k)
        {
            const double end = k + 1 < line.size() ? line[k + 1].startSeconds : std::max(music.durationSeconds, line[k].startSeconds);
            return std::max(0.0, end - line[k].startSeconds);
        };
        struct Corner { double seconds, value; };
        std::vector<Corner> corners;
        corners.push_back({ -1.0e9, placed[0] });
        for (std::size_t k = 1; k < placed.size(); ++k)
        {
            const double change = line[k].startSeconds;
            const double half = 0.5 * glide * std::min(noteLength(k - 1), noteLength(k));
            corners.push_back({ change - half, placed[k - 1] });
            corners.push_back({ change + half, placed[k] });
        }
        corners.push_back({ 1.0e9, placed.back() });

        const double octave = 12.0 * options.octaveShift;
        std::size_t last = first, corner = 0;
        for (std::size_t k = 0; k < n; ++k)
        {
            const double seconds = offset + static_cast<double>(k) * sfx.hopSeconds;
            last = leadAt(seconds);
            while (corner + 2 < corners.size() && corners[corner + 1].seconds <= seconds)
                ++corner;
            const auto& p = corners[corner];
            const auto& q = corners[corner + 1];
            const double span = q.seconds - p.seconds;
            const double t = span > 1.0e-9 ? std::clamp((seconds - p.seconds) / span, 0.0, 1.0) : 1.0;
            out.curve[k] = static_cast<float>(p.value + (q.value - p.value) * t + octave - centre);
        }
        out.notesFollowed = static_cast<int>(last - first + 1);
        out.targetMidi = placed[first] + octave;
        out.targetPitchClass = wrap12(static_cast<int>(std::lround(out.targetMidi - static_cast<double>(music.tuningCents) / 100.0)));
        out.semitones = out.targetMidi - centre;
        out.speedRatio = std::exp2(out.semitones / 12.0);
    }
    const double bend = std::clamp(options.bend, -1.0, 1.0);

    if (bend < 0.0 && n > 0)
    {
        // Flatten: take out the effect's own movement around its centre. Frames with no pitch
        // take the movement of their neighbours, so the correction never jumps.
        std::vector<float> deviation(n, std::numeric_limits<float>::quiet_NaN());
        for (std::size_t i = 0; i < n; ++i)
            if (sfx.hz[i] > 0.0f)
                deviation[i] = static_cast<float>(hzToMidi(sfx.hz[i]) - centre);

        std::size_t i = 0;
        float last = std::numeric_limits<float>::quiet_NaN();
        while (i < n)
        {
            if (! std::isnan(deviation[i]))
            {
                last = deviation[i++];
                continue;
            }
            std::size_t next = i;
            while (next < n && std::isnan(deviation[next]))
                ++next;
            const float after = next < n ? deviation[next] : last;
            const float before = std::isnan(last) ? after : last;
            for (std::size_t k = i; k < next; ++k)
            {
                const float t = static_cast<float>(k - i + 1) / static_cast<float>(next - i + 1);
                deviation[k] = std::isnan(before) ? 0.0f : before + (after - before) * t;
            }
            i = next;
        }
        for (std::size_t k = 0; k < n; ++k)
            out.curve[k] -= static_cast<float>(-bend) * deviation[k];
    }
    else if (bend > 0.0 && n > 0 && music.contourMidi.size() >= 4 && music.contourHopSeconds > 0.0 && ! music.leadNotes.empty())
    {
        // Take on the music's movement: how far the lead voice is, at each moment, from the note
        // it is on. While the voice holds a note that is nothing, so the effect stays exactly on
        // its own note; where the voice slides into a note, scoops, or drifts, the effect leans
        // the same way by the same amount (times the bend). It is the voice's inflection that is
        // added, never its tune: the notes themselves are the resting line's business.
        const std::size_t frames = music.contourMidi.size();
        std::vector<double> lean(frames);
        {
            std::size_t note = 0;
            for (std::size_t i = 0; i < frames; ++i)
            {
                const double time = music.contourStartSeconds + static_cast<double>(i) * music.contourHopSeconds;
                while (note + 1 < music.leadNotes.size() && music.leadNotes[note + 1].startSeconds <= time)
                    ++note;
                double d = static_cast<double>(music.contourMidi[i]) - music.leadNotes[note].midi;
                d -= 12.0 * std::round(d / 12.0);          // the same note an octave away is no lean at all
                lean[i] = std::clamp(d, -3.5, 3.5);
            }
            // Eased over 150 ms, so vibrato and flutter do not come through.
            const auto radius = static_cast<std::size_t>(std::max(1.0, 0.075 / music.contourHopSeconds));
            std::vector<double> sums(frames + 1, 0.0);
            for (std::size_t i = 0; i < frames; ++i)
                sums[i + 1] = sums[i] + lean[i];
            for (std::size_t i = 0; i < frames; ++i)
            {
                const auto from = i >= radius ? i - radius : 0, to = std::min(frames - 1, i + radius);
                lean[i] = (sums[to + 1] - sums[from]) / static_cast<double>(to - from + 1);
            }
        }

        // Where the effect runs past the learned music (either end), the lean carries on at the
        // pace it had over the music's nearest second and eases to a stop: no jump, no runaway.
        const double first = music.contourStartSeconds, last = first + static_cast<double>(frames - 1) * music.contourHopSeconds;
        const auto slopeAt = [&](bool atEnd)
        {
            const auto count = std::min(frames, static_cast<std::size_t>(std::max(2.0, 1.0 / music.contourHopSeconds)));
            const std::size_t from = atEnd ? frames - count : 0;
            double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
            for (std::size_t i = 0; i < count; ++i)
            {
                const double x = static_cast<double>(i) * music.contourHopSeconds, y = lean[from + i];
                sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            const double denom = static_cast<double>(count) * sxx - sx * sx;
            return denom > 0.0 ? (static_cast<double>(count) * sxy - sx * sy) / denom : 0.0;
        };
        const double slopeEnd = slopeAt(true), slopeStart = slopeAt(false);
        constexpr double settle = 1.5;   // seconds for the carried-on movement to die away

        for (std::size_t k = 0; k < n; ++k)
        {
            const double seconds = offset + static_cast<double>(k) * sfx.hopSeconds;
            double value;
            if (seconds > last)
                value = lean.back() + slopeEnd * settle * (1.0 - std::exp(-(seconds - last) / settle));
            else if (seconds < first)
                value = lean.front() - slopeStart * settle * (1.0 - std::exp((seconds - first) / settle));
            else
            {
                const double at = (seconds - first) / music.contourHopSeconds;
                const auto lo = std::min(frames - 1, static_cast<std::size_t>(at));
                const auto hi = std::min(lo + 1, frames - 1);
                const double frac = at - static_cast<double>(lo);
                value = lean[lo] * (1.0 - frac) + lean[hi] * frac;
            }
            out.curve[k] += static_cast<float>(bend * value);
        }
    }

    // The automatic curve is eased: a step between two notes becomes a 25 ms glide, which the
    // stretcher takes far better than a jittery ratio and is kinder on the ear ...
    if ((bend != 0.0 || following) && n > 2)
    {
        const int radius = std::max(1, static_cast<int>(std::lround(0.0125 / sfx.hopSeconds)));
        std::vector<float> smooth(n);
        for (std::size_t k = 0; k < n; ++k)
        {
            const auto a = k >= static_cast<std::size_t>(radius) ? k - static_cast<std::size_t>(radius) : 0;
            const auto b = std::min(n - 1, k + static_cast<std::size_t>(radius));
            float sum = 0.0f;
            for (std::size_t i = a; i <= b; ++i)
                sum += out.curve[i];
            smooth[k] = sum / static_cast<float>(b - a + 1);
        }
        out.curve = std::move(smooth);
    }

    // ... and Smooth (the same control as the glide between resting notes) eases it further, over
    // up to a quarter of a second each side.
    if (options.glide > 0.0 && n > 4)
    {
        const int radius = static_cast<int>(std::lround(std::clamp(options.glide, 0.0, 1.0) * 0.25 / sfx.hopSeconds));
        for (int pass = 0; pass < 2 && radius > 0; ++pass)   // twice: a rounded corner, not a clipped one
        {
            std::vector<double> sums(n + 1, 0.0);
            for (std::size_t k = 0; k < n; ++k)
                sums[k + 1] = sums[k] + static_cast<double>(out.curve[k]);
            for (std::size_t k = 0; k < n; ++k)
            {
                const auto from = k >= static_cast<std::size_t>(radius) ? k - static_cast<std::size_t>(radius) : 0;
                const auto to = std::min(n - 1, k + static_cast<std::size_t>(radius));
                out.curve[k] = static_cast<float>((sums[to + 1] - sums[from]) / static_cast<double>(to - from + 1));
            }
        }
    }

    // Hand adjustments go on last, exactly as drawn (eased over 25 ms and no more): what the user
    // sets at a moment is what that moment gets, whatever Smooth is doing to the rest. Snapping,
    // and what is heard while dragging, depend on that.
    if (! options.edits.empty() && n > 0)
    {
        const double duration = static_cast<double>(n - 1) * sfx.hopSeconds;
        std::vector<float> added(n);
        for (std::size_t k = 0; k < n; ++k)
            added[k] = static_cast<float>(editOffsetAt(options.edits, static_cast<double>(k) * sfx.hopSeconds, duration));
        const int radius = std::max(1, static_cast<int>(std::lround(0.0125 / sfx.hopSeconds)));
        for (std::size_t k = 0; k < n; ++k)
        {
            const auto a = k >= static_cast<std::size_t>(radius) ? k - static_cast<std::size_t>(radius) : 0;
            const auto b = std::min(n - 1, k + static_cast<std::size_t>(radius));
            float sum = 0.0f;
            for (std::size_t i = a; i <= b; ++i)
                sum += added[i];
            out.curve[k] += sum / static_cast<float>(b - a + 1);
        }
    }

    out.afterHz.assign(n, 0.0f);
    out.afterMidi.assign(n, 0.0f);
    for (std::size_t k = 0; k < n; ++k)
    {
        if (sfx.hz[k] > 0.0f)
            out.afterHz[k] = sfx.hz[k] * std::exp2(out.curve[k] / 12.0f);
        out.afterMidi[k] = static_cast<float>(centre) + out.curve[k];
    }

    // Which of the effect's frames lie under the learned music.
    {
        const double offsetSeconds = options.offsetKnown ? options.sfxOffsetInMusicSeconds : 0.0;
        const double from = -offsetSeconds / sfx.hopSeconds;
        const double to = (music.durationSeconds - offsetSeconds) / sfx.hopSeconds;
        out.coveredFrom = static_cast<int>(std::clamp(std::ceil(from), 0.0, static_cast<double>(n)));
        out.coveredTo = static_cast<int>(std::clamp(std::floor(to), static_cast<double>(out.coveredFrom), static_cast<double>(n)));
    }

    out.valid = true;
    return out;
}

double editOffsetAt(const std::vector<CurvePoint>& edits, double seconds, double durationSeconds)
{
    if (edits.empty())
        return 0.0;

    // The points either side of `seconds`, with nothing (0) at the effect's two ends unless a
    // point sits within 50 ms of one. Works whether or not the points are sorted.
    constexpr double edge = 0.05;
    const CurvePoint* before = nullptr;
    const CurvePoint* after = nullptr;
    const CurvePoint* first = &edits.front();
    const CurvePoint* last = &edits.front();
    for (const auto& p : edits)
    {
        if (p.seconds < first->seconds) first = &p;
        if (p.seconds > last->seconds) last = &p;
        if (p.seconds <= seconds && (before == nullptr || p.seconds > before->seconds)) before = &p;
        if (p.seconds >= seconds && (after == nullptr || p.seconds < after->seconds)) after = &p;
    }
    const CurvePoint startAnchor { 0.0, first->seconds > edge ? 0.0 : first->semitones };
    const CurvePoint endAnchor { durationSeconds, last->seconds < durationSeconds - edge ? 0.0 : last->semitones };
    const CurvePoint& p = before != nullptr ? *before : startAnchor;
    const CurvePoint& q = after != nullptr ? *after : endAnchor;
    if (q.seconds - p.seconds < 1.0e-9)
        return q.semitones;
    const double t = std::clamp((seconds - p.seconds) / (q.seconds - p.seconds), 0.0, 1.0);
    return p.semitones + (q.semitones - p.semitones) * t;
}

double snapToMusic(const MusicAnalysis& music, double midi, bool scaleOnly) noexcept
{
    const double tuning = music.valid ? static_cast<double>(music.tuningCents) / 100.0 : 0.0;
    const int nearest = static_cast<int>(std::lround(midi - tuning));
    if (! scaleOnly || ! music.valid)
        return static_cast<double>(nearest) + tuning;

    static constexpr bool major[12] { true, false, true, false, true, true, false, true, false, true, false, true };
    static constexpr bool minor[12] { true, false, true, true, false, true, false, true, true, false, true, false };
    const bool* scale = music.minor ? minor : major;
    double best = static_cast<double>(nearest) + tuning;
    double bestDistance = 1.0e9;
    for (int note = nearest - 3; note <= nearest + 3; ++note)
    {
        if (! scale[wrap12(note - music.rootPitchClass)])
            continue;
        const double pitch = static_cast<double>(note) + tuning;
        const double distance = std::abs(pitch - midi);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = pitch;
        }
    }
    return best;
}

} // namespace tonalrecall::core
