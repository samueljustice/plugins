#include "Flatten.h"
#include "HarmonicTracker.h"
#include "RidgeTracker.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace pitchflattener::core
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    bool stop(const Cancel& cancelled) { return cancelled && cancelled(); }

    std::vector<float> mixToMono(const Channels& sound)
    {
        std::vector<float> mono(sound.empty() ? 0 : sound[0].size(), 0.0f);
        for (const auto& channel : sound)
            for (std::size_t i = 0; i < mono.size(); ++i)
                mono[i] += channel[i];
        const float scale = sound.empty() ? 1.0f : 1.0f / static_cast<float>(sound.size());
        for (auto& v : mono)
            v *= scale;
        return mono;
    }

    // Turns a tracker's reading into the line the flattener uses.
    PitchLine buildLine(std::vector<float> trackedHz, double hop, const std::vector<float>& mono, double rate, Tracker tracker)
    {
        PitchLine line;
        line.tracker = tracker;
        line.hopSeconds = hop;
        const int frames = static_cast<int>(trackedHz.size());
        line.trackedHz = std::move(trackedHz);
        if (frames == 0)
            return line;

        // Level per frame, for weighing the line and for knowing which frames are audible at all.
        const int hopSamples = std::max(1, static_cast<int>(std::lround(hop * rate)));
        std::vector<float> level(static_cast<std::size_t>(frames), 0.0f);
        float loudest = 0.0f;
        for (int f = 0; f < frames; ++f)
        {
            const auto from = static_cast<std::size_t>(f) * static_cast<std::size_t>(hopSamples);
            const auto to = std::min(mono.size(), from + static_cast<std::size_t>(hopSamples));
            double energy = 0.0;
            for (auto i = from; i < to; ++i)
                energy += static_cast<double>(mono[i]) * mono[i];
            level[static_cast<std::size_t>(f)] = to > from ? static_cast<float>(std::sqrt(energy / static_cast<double>(to - from))) : 0.0f;
            loudest = std::max(loudest, level[static_cast<std::size_t>(f)]);
        }

        std::vector<float> raw(static_cast<std::size_t>(frames), 0.0f);
        int voiced = 0, audible = 0;
        for (int f = 0; f < frames; ++f)
        {
            const auto uf = static_cast<std::size_t>(f);
            if (level[uf] > loudest * 0.01f)
                ++audible;
            if (line.trackedHz[uf] > 0.0f)
            {
                raw[uf] = static_cast<float>(hzToMidi(line.trackedHz[uf]));
                ++voiced;
            }
        }
        line.coverage = audible > 0 ? std::min(1.0, static_cast<double>(voiced) / audible) : 0.0;
        if (voiced == 0)
            return line;

        // One continuous line. A sound's pitch can sweep fast (a ricochet falls more than 100
        // semitones a second) but it does not leap; a leap in the reading is the tracker stepping
        // to another partial or another octave. Each leap is taken out and everything after it
        // moved by the same amount, so the line carries the sound's movement with no steps for
        // the flattener to act on. A sound that really does leap again and again (a yelping
        // siren snapping back to the bottom of each sweep) would send that correction running
        // away, so when the leaps do not roughly cancel out the reading is kept as it is.
        {
            std::vector<float> mended(raw);
            float offset = 0.0f, before = 0.0f, furthest = 0.0f;
            int at = -1, leaps = 0;
            for (int f = 0; f < frames; ++f)
            {
                const auto uf = static_cast<std::size_t>(f);
                if (raw[uf] == 0.0f)
                    continue;
                const float here = raw[uf];
                if (at >= 0)
                {
                    const float allowed = 2.5f + 0.3f * static_cast<float>(f - at - 1);
                    if (std::abs(here - before) > allowed)
                    {
                        offset -= here - before;
                        ++leaps;
                    }
                }
                before = here;
                at = f;
                furthest = std::max(furthest, std::abs(offset));
                mended[uf] = here + offset;
            }
            if (furthest <= 14.0f)
            {
                raw = std::move(mended);
                line.leaps = leaps;
            }
        }

        // Gaps bridged in straight lines, the ends held.
        std::vector<float> filled(static_cast<std::size_t>(frames), 0.0f);
        int previous = -1;
        for (int f = 0; f < frames; ++f)
        {
            const auto uf = static_cast<std::size_t>(f);
            if (raw[uf] == 0.0f)
                continue;
            if (previous < 0)
                std::fill(filled.begin(), filled.begin() + f, raw[uf]);
            else
                for (int k = previous + 1; k < f; ++k)
                    filled[static_cast<std::size_t>(k)] = raw[static_cast<std::size_t>(previous)]
                        + (raw[uf] - raw[static_cast<std::size_t>(previous)]) * static_cast<float>(k - previous) / static_cast<float>(f - previous);
            filled[uf] = raw[uf];
            previous = f;
        }
        std::fill(filled.begin() + previous + 1, filled.end(), raw[static_cast<std::size_t>(previous)]);

        // How unsteady the reading is: the typical change of slope between neighbouring frames
        // that both have a pitch. A real pitch moves smoothly, even in a fast sweep (a constant
        // slope); a tracker wandering through noise does not.
        {
            std::vector<float> bends;
            for (int f = 1; f + 1 < frames; ++f)
            {
                const auto uf = static_cast<std::size_t>(f);
                if (line.trackedHz[uf - 1] > 0.0f && line.trackedHz[uf] > 0.0f && line.trackedHz[uf + 1] > 0.0f)
                    bends.push_back(std::abs(raw[uf - 1] - 2.0f * raw[uf] + raw[uf + 1]));
            }
            if (! bends.empty())
            {
                std::nth_element(bends.begin(), bends.begin() + static_cast<std::ptrdiff_t>(bends.size() / 2), bends.end());
                line.jitter = bends[bends.size() / 2];
            }
        }

        // Jitter out. Played as a changing speed, jitter in the line would be heard as flutter,
        // but smoothing also rounds off what is really there (vibrato, the corners of a fast
        // sweep), so the line is smoothed only as much as it needs: a 45 ms median first (single
        // wrong frames), then a Gaussian whose width follows how unsteady the reading is (10 ms
        // for a clear pitch, up to 30 ms for a noisy one) and narrows where the pitch is moving
        // fast.
        std::vector<float> steadied(static_cast<std::size_t>(frames));
        for (int f = 0; f < frames; ++f)
        {
            float window[9];
            int n = 0;
            for (int k = std::max(0, f - 4); k <= std::min(frames - 1, f + 4); ++k)
                window[n++] = filled[static_cast<std::size_t>(k)];
            std::nth_element(window, window + n / 2, window + n);
            steadied[static_cast<std::size_t>(f)] = window[n / 2];
        }
        const double widest = 2.0 + 4.0 * std::clamp((line.jitter - 0.01) / 0.07, 0.0, 1.0);   // frames
        line.midi.assign(static_cast<std::size_t>(frames), 0.0f);
        for (int f = 0; f < frames; ++f)
        {
            const int a = std::max(0, f - 3), b = std::min(frames - 1, f + 3);
            const double slope = b > a ? std::abs(steadied[static_cast<std::size_t>(b)] - steadied[static_cast<std::size_t>(a)]) / ((b - a) * hop) : 0.0;   // st a second
            const double sigma = std::max(1.5, widest / (1.0 + slope / 40.0));
            const int reach = static_cast<int>(std::ceil(3.0 * sigma));
            double sum = 0.0, weights = 0.0;
            for (int k = std::max(0, f - reach); k <= std::min(frames - 1, f + reach); ++k)
            {
                const double g = std::exp(-0.5 * (k - f) * (k - f) / (sigma * sigma));
                sum += g * steadied[static_cast<std::size_t>(k)];
                weights += g;
            }
            line.midi[static_cast<std::size_t>(f)] = static_cast<float>(sum / weights);
        }

        // Where the line sits, counted by level.
        std::vector<std::pair<float, float>> points;
        double total = 0.0;
        std::vector<float> opening;
        for (int f = 0; f < frames; ++f)
        {
            const auto uf = static_cast<std::size_t>(f);
            if (line.trackedHz[uf] <= 0.0f)
                continue;
            points.push_back({ line.midi[uf], level[uf] });
            total += level[uf];
            if (static_cast<double>(opening.size()) * hop < 0.3)
                opening.push_back(line.midi[uf]);
        }
        std::sort(points.begin(), points.end());
        const auto percentile = [&](double q)
        {
            double sum = 0.0;
            for (const auto& p : points)
            {
                sum += p.second;
                if (sum >= q * total)
                    return static_cast<double>(p.first);
            }
            return static_cast<double>(points.back().first);
        };
        line.lowMidi = percentile(0.05);
        line.centreMidi = percentile(0.5);
        line.highMidi = percentile(0.95);
        std::nth_element(opening.begin(), opening.begin() + static_cast<std::ptrdiff_t>(opening.size() / 2), opening.end());
        line.startMidi = opening[opening.size() / 2];
        return line;
    }

    PitchLine readHarmonic(const std::vector<float>& mono, double rate)
    {
        auto sfx = analyseSfx(mono.data(), static_cast<int>(mono.size()), rate, SfxMethod::Auto);
        // One fixed ring says nothing about how the pitch moves.
        const bool moving = sfx.valid && sfx.pitched && sfx.usedMethod != SfxMethod::Resonance;
        auto line = buildLine(std::move(sfx.hz), sfx.hopSeconds, mono, rate, Tracker::Harmonic);
        line.usable = moving && ! line.midi.empty() && line.coverage >= 0.3;
        return line;
    }

    PitchLine readRidge(const std::vector<float>& mono, double rate)
    {
        auto ridge = trackRidge(mono.data(), static_cast<int>(mono.size()), rate);
        detail::repairHarmonicSlips(ridge.hz, ridge.strength);
        // How much of what it followed lies in stretches of some length. In plain noise it
        // strings together short runs of chance peaks; a real partial is followed for longer.
        int voiced = 0, inLongRuns = 0, run = 0;
        const int longRun = static_cast<int>(std::ceil(0.06 / ridge.hop));
        for (std::size_t f = 0; f <= ridge.hz.size(); ++f)
        {
            if (f < ridge.hz.size() && ridge.hz[f] > 0.0f)
            {
                ++voiced;
                ++run;
                continue;
            }
            if (run >= longRun)
                inLongRuns += run;
            run = 0;
        }
        const bool sustained = voiced > 0 && inLongRuns >= 0.6 * voiced;
        auto line = buildLine(std::move(ridge.hz), ridge.hop, mono, rate, Tracker::Ridge);
        // The ridge tracker always finds something to follow, even in noise, where its line
        // wanders from frame to frame. Measured on real recordings, a ridge on a real partial
        // (sirens, ricochet whines) is unsteady by up to about 0.28 st and one wandering through
        // an ambience, a drone's upper partials or noise by 0.36 st and more.
        constexpr double kWandering = 0.32;
        line.usable = voiced * ridge.hop >= 0.08 && sustained && ! line.midi.empty() && line.coverage >= 0.3 && line.jitter < kWandering;
        return line;
    }

    // How far a sound's pitch travels, as both trackers read it (an unreadable result counts as
    // an octave: it is not a flat sound).
    double travelOf(const Channels& sound, double rate)
    {
        const auto mono = mixToMono(sound);
        const auto a = readHarmonic(mono, rate), b = readRidge(mono, rate);
        return (a.usable ? a.highMidi - a.lowMidi : 12.0) + (b.usable ? b.highMidi - b.lowMidi : 12.0);
    }
}

//==============================================================================
Analysis analyse(const Channels& sound, double sampleRate, bool score, const Cancel& cancelled)
{
    Analysis analysis;
    if (sound.empty() || sound[0].empty() || sampleRate <= 0.0)
        return analysis;
    for (const auto& channel : sound)
        if (channel.size() != sound[0].size())
            return analysis;
    analysis.sampleRate = sampleRate;
    analysis.durationSeconds = static_cast<double>(sound[0].size()) / sampleRate;
    if (analysis.durationSeconds < 0.05)
        return analysis;

    const auto mono = mixToMono(sound);
    analysis.lines[0] = readHarmonic(mono, sampleRate);
    if (stop(cancelled))
        return analysis;
    analysis.lines[1] = readRidge(mono, sampleRate);
    if (stop(cancelled))
        return analysis;
    analysis.valid = true;

    const bool usable[2] { analysis.lines[0].usable, analysis.lines[1].usable };
    if (! usable[0] && ! usable[1])
        return analysis;
    analysis.pick = usable[0] ? 0 : 1;
    if (! score || ! (usable[0] && usable[1]))
        return analysis;

    // Both lines are usable: flatten by each and read the results back. The flatter result
    // wins.
    for (int k = 0; k < kNumTrackers; ++k)
    {
        Options options;
        options.line = k;
        const auto result = flatten(sound, sampleRate, analysis, options, cancelled);
        if (! result.valid || stop(cancelled))
        {
            analysis.valid = false;
            return analysis;
        }
        analysis.lines[static_cast<std::size_t>(k)].residual = travelOf(result.channels, sampleRate);
    }
    analysis.pick = analysis.lines[0].residual <= analysis.lines[1].residual ? 0 : 1;
    return analysis;
}

//==============================================================================
const PitchLine* lineFor(const Analysis& analysis, const Options& options) noexcept
{
    if (analysis.pick < 0)
        return nullptr;
    // A line asked for by name is used if it can be; if it reads nothing in this sound, the
    // other one is, rather than leaving a sound that can be flattened untouched.
    int index = options.line >= 0 && options.line < kNumTrackers ? options.line : analysis.pick;
    if (! analysis.lines[static_cast<std::size_t>(index)].usable)
        index = analysis.pick;
    const auto& line = analysis.lines[static_cast<std::size_t>(index)];
    return line.usable ? &line : nullptr;
}

double targetMidi(const PitchLine& line, const Options& options) noexcept
{
    switch (options.target)
    {
        case Target::HighPoint: return line.highMidi;
        case Target::Centre:    return line.centreMidi;
        case Target::LowPoint:  return line.lowMidi;
        case Target::Start:     return line.startMidi;
        case Target::Note:      return options.noteMidi;
    }
    return line.highMidi;
}

double editAt(const std::vector<CurvePoint>& edits, double seconds) noexcept
{
    if (edits.empty())
        return 0.0;
    constexpr double ease = 0.05;
    if (seconds <= edits.front().seconds)
    {
        const double d = edits.front().seconds - seconds;
        return d >= ease ? 0.0 : edits.front().semitones * (1.0 - d / ease);
    }
    if (seconds >= edits.back().seconds)
    {
        const double d = seconds - edits.back().seconds;
        return d >= ease ? 0.0 : edits.back().semitones * (1.0 - d / ease);
    }
    const auto next = std::upper_bound(edits.begin(), edits.end(), seconds, [](double v, const CurvePoint& p) { return v < p.seconds; });
    const auto& b = *next;
    const auto& a = *(next - 1);
    return a.semitones + (seconds - a.seconds) / std::max(1.0e-9, b.seconds - a.seconds) * (b.semitones - a.semitones);
}

//==============================================================================
double Flattened::outputSampleFor(double sourceSample) const noexcept
{
    if (outputAtSource.empty())
        return 0.0;
    const double x = std::clamp(sourceSample / kMapStep, 0.0, static_cast<double>(outputAtSource.size() - 1));
    const auto i = static_cast<std::size_t>(x);
    if (i + 1 >= outputAtSource.size())
        return outputAtSource.back();
    return outputAtSource[i] + (x - static_cast<double>(i)) * (outputAtSource[i + 1] - outputAtSource[i]);
}

double Flattened::sourceSampleFor(double outputSample) const noexcept
{
    if (outputAtSource.size() < 2)
        return 0.0;
    const auto next = std::upper_bound(outputAtSource.begin(), outputAtSource.end(), outputSample);
    if (next == outputAtSource.begin())
        return 0.0;
    if (next == outputAtSource.end())
        return static_cast<double>(outputAtSource.size() - 1) * kMapStep;
    const auto i = static_cast<std::size_t>(next - outputAtSource.begin()) - 1;
    const double span = outputAtSource[i + 1] - outputAtSource[i];
    return (static_cast<double>(i) + (span > 0.0 ? (outputSample - outputAtSource[i]) / span : 0.0)) * kMapStep;
}

Flattened flatten(const Channels& sound, double sampleRate, const Analysis& analysis, const Options& options, const Cancel& cancelled)
{
    Flattened out;
    const auto* line = lineFor(analysis, options);
    if (line == nullptr || sound.empty() || sound[0].empty() || line->midi.empty())
        return out;

    out.sampleRate = sampleRate;
    out.targetMidi = targetMidi(*line, options);
    out.channels.assign(sound.size(), {});
    const auto length = static_cast<double>(sound[0].size());
    const double amount = std::clamp(options.amount, 0.0, 1.0);
    const double frameSamples = line->hopSeconds * sampleRate;
    const auto lastFrame = static_cast<double>(line->midi.size() - 1);

    // The line (with any hand corrections) at a moment of the sound.
    const auto midiAt = [&](double sample)
    {
        const double x = std::clamp(sample / frameSamples - 0.5, 0.0, lastFrame);
        const auto i = static_cast<std::size_t>(x);
        const double frac = x - static_cast<double>(i);
        const double a = line->midi[i];
        const double b = i + 1 < line->midi.size() ? line->midi[i + 1] : a;
        return a + frac * (b - a) + editAt(options.edits, sample / sampleRate);
    };

    // Reading the sound at a changing speed. Each output sample is read between the sound's
    // samples with a windowed sinc; reading faster than 1 would fold high frequencies back, so
    // the sinc is widened to filter them out first.
    constexpr int kHalf = 24;
    for (auto& channel : out.channels)
        channel.reserve(sound[0].size());
    double nextMapped = 0.0, speed = 1.0;
    float peak = 0.0f;
    std::size_t written = 0;
    for (double p = 0.0; p < length;)
    {
        if ((written & 0xffff) == 0 && stop(cancelled))
            return Flattened {};
        speed = std::clamp(std::exp2(amount * (out.targetMidi - midiAt(p)) / 12.0), 0.125, 8.0);
        while (nextMapped <= p)
        {
            out.outputAtSource.push_back(static_cast<double>(written) - (p - nextMapped) / speed);
            nextMapped += Flattened::kMapStep;
        }

        const double cutoff = std::min(1.0, 1.0 / speed);
        const auto centre = static_cast<long>(std::floor(p));
        const int reach = static_cast<int>(std::ceil(kHalf / cutoff));
        const long from = std::max(0L, centre - reach + 1), to = std::min(static_cast<long>(sound[0].size()) - 1, centre + reach);
        for (std::size_t c = 0; c < sound.size(); ++c)
        {
            const auto& source = sound[c];
            double sum = 0.0;
            for (long k = from; k <= to; ++k)
            {
                const double x = (static_cast<double>(k) - p) * cutoff;
                if (std::abs(x) >= kHalf)
                    continue;
                const double window = 0.5 + 0.5 * std::cos(kPi * x / kHalf);
                const double sinc = std::abs(x) < 1.0e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                sum += source[static_cast<std::size_t>(k)] * sinc * window * cutoff;
            }
            const auto v = static_cast<float>(sum);
            out.channels[c].push_back(v);
            peak = std::max(peak, std::abs(v));
        }
        ++written;
        p += speed;
    }
    // The map's last entry is the end of the sound.
    while (nextMapped < length + Flattened::kMapStep)
    {
        out.outputAtSource.push_back(static_cast<double>(written));
        nextMapped += Flattened::kMapStep;
    }

    // Reading between samples can raise a peak a little: the whole result is turned down by
    // just enough to stay under full scale.
    constexpr float kCeiling = 0.98855309f;   // -0.1 dBFS
    if (peak > kCeiling)
    {
        out.gain = kCeiling / peak;
        for (auto& channel : out.channels)
            for (auto& v : channel)
                v *= out.gain;
    }
    out.valid = true;
    return out;
}

} // namespace pitchflattener::core
