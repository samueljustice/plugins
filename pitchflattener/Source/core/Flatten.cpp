#include "Flatten.h"
#include "HarmonicTracker.h"
#include "KeepLength.h"
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

    struct LineStats
    {
        bool any = false;
        double low = 0.0, centre = 0.0, high = 0.0, start = 0.0;
    };

    // Where the line sits over some of its frames, counted by level so a loud part outweighs a
    // long quiet one: the 5th, 50th and 95th percentile, and the first 0.3 s that has a pitch.
    LineStats statsOf(const PitchLine& line, int fromFrame, int toFrame)
    {
        LineStats stats;
        std::vector<std::pair<float, float>> points;
        std::vector<float> opening;
        double total = 0.0;
        const int frames = static_cast<int>(line.midi.size());
        for (int f = std::max(0, fromFrame); f < std::min(frames, toFrame); ++f)
        {
            const auto uf = static_cast<std::size_t>(f);
            if (line.trackedHz[uf] <= 0.0f)
                continue;
            const float weight = uf < line.level.size() ? line.level[uf] : 1.0f;
            points.push_back({ line.midi[uf], weight });
            total += weight;
            if (static_cast<double>(opening.size()) * line.hopSeconds < 0.3)
                opening.push_back(line.midi[uf]);
        }
        if (points.empty())
            return stats;
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
        stats.any = true;
        stats.low = percentile(0.05);
        stats.centre = percentile(0.5);
        stats.high = percentile(0.95);
        std::nth_element(opening.begin(), opening.begin() + static_cast<std::ptrdiff_t>(opening.size() / 2), opening.end());
        stats.start = opening[opening.size() / 2];
        return stats;
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
        // semitones a second) but it does not leap, and a leap in the reading is nearly always
        // the tracker changing its mind: stepping to another partial or another octave, or a
        // voice breaking into a growl whose period is many times longer while its tone stays
        // where it was. Flattened as read, a leap becomes a lurch in speed, which is the worst
        // thing this plug-in can do to a sound. So each leap is taken out and everything after it
        // moved by the same amount: the line carries the sound's movement with no steps.
        //
        // The one sound that really does leap is a sweep that snaps back and starts again (a
        // yelping siren). Taking its leaps out would stack every sweep on top of the last, and
        // the line would climb without end; that shows as a mended line that travels much further
        // than the reading did, and then the reading is kept as it is.
        {
            std::vector<float> mended(raw);
            float offset = 0.0f, before = 0.0f;
            float rawLow = 1.0e9f, rawHigh = -1.0e9f, mendedLow = 1.0e9f, mendedHigh = -1.0e9f;
            int at = -1, leaps = 0;
            for (int f = 0; f < frames; ++f)
            {
                const auto uf = static_cast<std::size_t>(f);
                if (raw[uf] == 0.0f)
                    continue;
                const float here = raw[uf];
                if (at >= 0)
                {
                    // Across a gap in the reading the pitch may have moved on a little, no more.
                    const float allowed = 2.5f + std::min(4.0f, 0.1f * static_cast<float>(f - at - 1));
                    if (std::abs(here - before) > allowed)
                    {
                        offset -= here - before;
                        ++leaps;
                    }
                }
                before = here;
                at = f;
                mended[uf] = here + offset;
                rawLow = std::min(rawLow, here);
                rawHigh = std::max(rawHigh, here);
                mendedLow = std::min(mendedLow, mended[uf]);
                mendedHigh = std::max(mendedHigh, mended[uf]);
            }
            if (leaps > 0 && mendedHigh - mendedLow <= (rawHigh - rawLow) + 6.0f)
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

        line.level = level;
        const auto whole = statsOf(line, 0, frames);
        line.lowMidi = whole.low;
        line.centreMidi = whole.centre;
        line.highMidi = whole.high;
        line.startMidi = whole.start;
        return line;
    }

    // The voice and engine readings (and, for reading a result back, whichever of them suits).
    PitchLine readHarmonic(const std::vector<float>& mono, double rate, SfxMethod method, Tracker tracker)
    {
        auto sfx = analyseSfx(mono.data(), static_cast<int>(mono.size()), rate, method);
        // One fixed ring says nothing about how the pitch moves.
        const bool moving = sfx.valid && sfx.pitched && sfx.usedMethod != SfxMethod::Resonance;
        auto line = buildLine(std::move(sfx.hz), sfx.hopSeconds, mono, rate, tracker);
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
        auto line = buildLine(std::move(ridge.hz), ridge.hop, mono, rate, Tracker::Tone);
        // The ridge tracker always finds something to follow, even in noise, where its line
        // wanders from frame to frame. Measured on real recordings, a ridge on a real partial
        // (sirens, ricochet whines) is unsteady by up to about 0.28 st and one wandering through
        // an ambience, a drone's upper partials or noise by 0.36 st and more.
        constexpr double kWandering = 0.32;
        line.usable = voiced * ridge.hop >= 0.08 && sustained && ! line.midi.empty() && line.coverage >= 0.3 && line.jitter < kWandering;
        return line;
    }

    // The separate sounds in a recording: stretches above -50 dB of its peak, with a quarter of a
    // second or more of quiet between them. Shorter gaps are part of one sound (the breath in a
    // call, the dip in a pass).
    std::vector<Analysis::Event> findEvents(const std::vector<float>& mono, double rate)
    {
        std::vector<Analysis::Event> events;
        const auto frame = std::max<std::size_t>(1, static_cast<std::size_t>(0.005 * rate));
        const std::size_t frames = mono.size() / frame;
        std::vector<float> level(frames, 0.0f);
        float peak = 0.0f;
        for (std::size_t f = 0; f < frames; ++f)
        {
            float top = 0.0f;
            for (std::size_t i = f * frame; i < (f + 1) * frame; ++i)
                top = std::max(top, std::abs(mono[i]));
            level[f] = top;
            peak = std::max(peak, top);
        }
        const double total = static_cast<double>(mono.size()) / rate;
        if (peak <= 0.0f)
            return { { 0.0, total } };
        const float floor = peak * 0.00316f;
        const auto gap = static_cast<std::size_t>(0.25 / 0.005);
        std::size_t start = frames, last = 0;
        for (std::size_t f = 0; f < frames; ++f)
        {
            if (level[f] <= floor)
                continue;
            if (start == frames)
                start = f;
            else if (f - last > gap)
            {
                events.push_back({ static_cast<double>(start) * 0.005, static_cast<double>(last + 1) * 0.005 });
                start = f;
            }
            last = f;
        }
        if (start != frames)
            events.push_back({ static_cast<double>(start) * 0.005, static_cast<double>(last + 1) * 0.005 });
        // A click of a few milliseconds is not a sound of its own to flatten.
        events.erase(std::remove_if(events.begin(), events.end(), [](const Analysis::Event& e) { return e.endSeconds - e.startSeconds < 0.03; }), events.end());
        if (events.empty())
            events.push_back({ 0.0, total });
        return events;
    }

    // How far a sound's pitch travels, as both trackers read it (an unreadable result counts as
    // an octave: it is not a flat sound).
    double travelOf(const Channels& sound, double rate)
    {
        const auto mono = mixToMono(sound);
        const auto a = readHarmonic(mono, rate, SfxMethod::Auto, Tracker::Voice), b = readRidge(mono, rate);
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
    analysis.events = findEvents(mono, sampleRate);
    analysis.lines[0] = readHarmonic(mono, sampleRate, SfxMethod::Tonal, Tracker::Voice);
    if (stop(cancelled))
        return analysis;
    analysis.lines[1] = readHarmonic(mono, sampleRate, SfxMethod::Engine, Tracker::Engine);
    // The engine reading follows how the whole pattern of partials shifts, and it always comes
    // back with a line, even from noise. It is only believed where the voice reading found the
    // sound periodic at all (it may have read it badly, hopping between partials: that is what
    // the engine reading is for).
    if (analysis.lines[0].coverage < 0.3)
        analysis.lines[1].usable = false;
    if (stop(cancelled))
        return analysis;
    analysis.lines[2] = readRidge(mono, sampleRate);
    if (stop(cancelled))
        return analysis;
    analysis.valid = true;

    int usable = 0;
    for (int k = kNumTrackers - 1; k >= 0; --k)
        if (analysis.lines[static_cast<std::size_t>(k)].usable)
        {
            analysis.pick = k;   // ends on the first usable one
            ++usable;
        }
    if (! score || usable < 2)
        return analysis;

    // More than one line is usable: flatten by each and read the results back. The flattest
    // result wins.
    double best = 1.0e18;
    for (int k = 0; k < kNumTrackers; ++k)
    {
        auto& line = analysis.lines[static_cast<std::size_t>(k)];
        if (! line.usable)
            continue;
        Options options;
        options.line = k;
        const auto result = flatten(sound, sampleRate, analysis, options, cancelled);
        if (! result.valid || stop(cancelled))
        {
            analysis.valid = false;
            return analysis;
        }
        line.residual = travelOf(result.channels, sampleRate);
        if (line.residual < best)
        {
            best = line.residual;
            analysis.pick = k;
        }
    }
    return analysis;
}

const char* trackerName(Tracker tracker) noexcept
{
    switch (tracker)
    {
        case Tracker::Voice:  return "VOICE";
        case Tracker::Engine: return "ENGINE";
        case Tracker::Tone:   return "TONE";
    }
    return "";
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

double targetMidi(const PitchLine& line, const Options& options, double fromSeconds, double toSeconds) noexcept
{
    if (options.target == Target::Note)
        return options.noteMidi;
    const auto stats = statsOf(line, static_cast<int>(std::floor(fromSeconds / line.hopSeconds)), static_cast<int>(std::ceil(toSeconds / line.hopSeconds)));
    if (! stats.any)
        return targetMidi(line, options);
    switch (options.target)
    {
        case Target::HighPoint: return stats.high;
        case Target::Centre:    return stats.centre;
        case Target::LowPoint:  return stats.low;
        case Target::Start:     return stats.start;
        case Target::Note:      break;
    }
    return stats.high;
}

double editAt(const std::vector<CurvePoint>& edits, double seconds) noexcept
{
    if (edits.empty())
        return 0.0;
    constexpr double ease = 0.25;   // one pulled point bends a quarter of a second either side of it
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
    // Each separate sound has its own target. Between two of them the target changes over at the
    // middle of the silence, where there is nothing to hear the speed change on.
    std::vector<double> changeAt;   // sample positions where the next sound's target takes over
    if (analysis.events.size() <= 1)
        out.eventTargets.push_back(targetMidi(*line, options));
    else
        for (std::size_t e = 0; e < analysis.events.size(); ++e)
        {
            out.eventTargets.push_back(targetMidi(*line, options, analysis.events[e].startSeconds, analysis.events[e].endSeconds));
            if (e + 1 < analysis.events.size())
                changeAt.push_back(0.5 * (analysis.events[e].endSeconds + analysis.events[e + 1].startSeconds) * sampleRate);
        }
    out.targetMidi = out.eventTargets.front();
    std::size_t current = 0;
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
        while (current < changeAt.size() && p >= changeAt[current])
            ++current;
        speed = std::clamp(std::exp2(amount * (out.eventTargets[current] - midiAt(p)) / 12.0), 0.125, 8.0);
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

    // Keep length: the flattened sound is brought back to the original's timing, and the time
    // map becomes one to one.
    if (options.keepLength)
    {
        const auto hits = options.character == Character::Transients ? findHits(sound, sampleRate) : std::vector<std::size_t> {};
        auto restored = restoreTiming(out.channels, sampleRate, out.outputAtSource, Flattened::kMapStep, sound[0].size(), options.character, hits, cancelled);
        if (restored.empty())
            return Flattened {};
        out.channels = std::move(restored);
        for (std::size_t k = 0; k < out.outputAtSource.size(); ++k)
            out.outputAtSource[k] = std::min(length, static_cast<double>(k) * Flattened::kMapStep);
        peak = 0.0f;
        for (const auto& channel : out.channels)
            for (const float v : channel)
                peak = std::max(peak, std::abs(v));
    }

    // Reading between samples (and stretching) can raise a peak a little: the whole result is
    // turned down by just enough to stay under full scale.
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
