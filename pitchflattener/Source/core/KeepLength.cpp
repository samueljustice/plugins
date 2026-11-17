#include "KeepLength.h"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace pitchflattener::core
{

std::vector<std::size_t> findHits(const Channels& sound, double sampleRate)
{
    // A hit is a sudden rise in level: the level over 3 ms, weighted towards its high
    // frequencies (the sample-to-sample change), against the average of the 40 ms before it.
    std::vector<std::size_t> hits;
    if (sound.empty() || sound[0].size() < 64)
        return hits;
    const auto frame = std::max<std::size_t>(8, static_cast<std::size_t>(0.003 * sampleRate));
    const std::size_t frames = sound[0].size() / frame;
    std::vector<double> level(frames, 0.0);
    double loudest = 0.0;
    for (std::size_t f = 0; f < frames; ++f)
    {
        double e = 0.0;
        for (const auto& channel : sound)
            for (std::size_t i = f * frame + 1; i < (f + 1) * frame; ++i)
            {
                const double d = static_cast<double>(channel[i]) - channel[i - 1];
                e += d * d + 0.05 * static_cast<double>(channel[i]) * channel[i];
            }
        level[f] = e;
        loudest = std::max(loudest, e);
    }
    const auto history = std::max<std::size_t>(2, static_cast<std::size_t>(0.04 * sampleRate / static_cast<double>(frame)));
    const auto spacing = std::max<std::size_t>(1, static_cast<std::size_t>(0.06 * sampleRate / static_cast<double>(frame)));
    std::size_t last = 0;
    bool any = false;
    for (std::size_t f = 1; f + 1 < frames; ++f)
    {
        if (level[f] < loudest * 1.0e-3)
            continue;   // too quiet to matter
        double before = 0.0;
        const std::size_t from = f > history ? f - history : 0;
        for (std::size_t k = from; k < f; ++k)
            before += level[k];
        before /= static_cast<double>(std::max<std::size_t>(1, f - from));
        // Eight times the level before it (9 dB), and the top of its rise.
        if (level[f] > 8.0 * before + loudest * 1.0e-5 && level[f] >= level[f - 1] && level[f + 1] <= level[f] * 4.0)
        {
            if (any && f - last < spacing)
                continue;
            hits.push_back(f * frame);
            last = f;
            any = true;
        }
    }
    return hits;
}

Channels restoreTiming(const Channels& flattened, double sampleRate, const std::vector<double>& outputAtSource, int mapStep,
                       std::size_t sourceLength, Character character, const std::vector<std::size_t>& hitsInSource, const Cancel& cancelled)
{
    using Stretcher = RubberBand::RubberBandStretcher;
    if (flattened.empty() || flattened[0].empty() || outputAtSource.size() < 2 || sourceLength == 0 || mapStep <= 0)
        return {};

    const std::size_t channels = flattened.size();
    const auto length = static_cast<std::int64_t>(flattened[0].size());

    // Where a sample of the flattened sound has to land in the result: the time map read the
    // other way (flattened position -> the original's position), straight lines between its
    // entries.
    const auto landsAt = [&](double flattenedSample)
    {
        const auto next = std::upper_bound(outputAtSource.begin(), outputAtSource.end(), flattenedSample);
        if (next == outputAtSource.begin())
            return 0.0;
        if (next == outputAtSource.end())
            return static_cast<double>(sourceLength);
        const auto i = static_cast<std::size_t>(next - outputAtSource.begin()) - 1;
        const double span = outputAtSource[i + 1] - outputAtSource[i];
        const double at = (static_cast<double>(i) + (span > 0.0 ? (flattenedSample - outputAtSource[i]) / span : 0.0)) * mapStep;
        return std::min(at, static_cast<double>(sourceLength));
    };

    // Transients: around each hit the sound is not stretched at all. From 4 ms before a hit to
    // 30 ms after it the stretch is exactly 1, placed so the hit itself lands where the map says;
    // the time lost or gained is made up over the 30 ms before and the 120 ms after. The map is
    // sampled on a fine grid and those stretches of it rewritten.
    constexpr int kGrid = 32;
    std::vector<double> carried;   // landsAt on the grid, hits carried through; empty unless Transients
    std::vector<std::pair<std::size_t, std::int64_t>> hitPlaces;   // each hit: its sample in the flattened sound, and where it lands
    if (character == Character::Transients)
    {
        const auto points = static_cast<std::size_t>(length / kGrid) + 2;
        carried.resize(points);
        for (std::size_t i = 0; i < points; ++i)
            carried[i] = landsAt(static_cast<double>(i) * kGrid);
        const double lead = 0.004 * sampleRate, hold = 0.030 * sampleRate, easeIn = 0.030 * sampleRate, easeOut = 0.120 * sampleRate;
        for (const auto hitInSource : hitsInSource)
        {
            // Where the hit is in the flattened sound (the time map, read forwards), and where it
            // must land: where it was.
            const double x = std::clamp(static_cast<double>(hitInSource) / mapStep, 0.0, static_cast<double>(outputAtSource.size() - 1));
            const auto xi = static_cast<std::size_t>(x);
            const double h = xi + 1 < outputAtSource.size() ? outputAtSource[xi] + (x - static_cast<double>(xi)) * (outputAtSource[xi + 1] - outputAtSource[xi])
                                                            : outputAtSource.back();
            const double anchor = static_cast<double>(hitInSource);
            if (h < 0.0 || h >= static_cast<double>(length))
                continue;
            hitPlaces.push_back({ static_cast<std::size_t>(std::llround(h)), static_cast<std::int64_t>(hitInSource) });
            const auto index = [&](double q) { return static_cast<std::size_t>(std::clamp(q / kGrid, 0.0, static_cast<double>(points - 1))); };
            const std::size_t a = index(h - lead), b = index(h + hold);
            const double errorIn = (anchor + (static_cast<double>(a) * kGrid - h)) - carried[a];
            const double errorOut = (anchor + (static_cast<double>(b) * kGrid - h)) - carried[b];
            for (std::size_t i = a; i <= b; ++i)
                carried[i] = anchor + (static_cast<double>(i) * kGrid - h);
            const std::size_t in0 = index(h - lead - easeIn), out1 = index(h + hold + easeOut);
            for (std::size_t i = in0; i < a; ++i)
                carried[i] += errorIn * static_cast<double>(i - in0) / static_cast<double>(std::max<std::size_t>(1, a - in0));
            for (std::size_t i = b + 1; i <= out1; ++i)
                carried[i] += errorOut * (1.0 - static_cast<double>(i - b) / static_cast<double>(std::max<std::size_t>(1, out1 - b)));
        }
        // Time never runs backwards.
        for (std::size_t i = 1; i < points; ++i)
            carried[i] = std::max(carried[i], carried[i - 1] + 0.125 * kGrid);
    }
    const auto place = [&](double flattenedSample)
    {
        if (carried.empty())
            return landsAt(flattenedSample);
        const double x = std::clamp(flattenedSample / kGrid, 0.0, static_cast<double>(carried.size() - 1));
        const auto i = static_cast<std::size_t>(x);
        return i + 1 < carried.size() ? carried[i] + (x - static_cast<double>(i)) * (carried[i + 1] - carried[i]) : carried.back();
    };

    // The stretcher runs as a stream, so the stretch can be set block by block from the map's
    // slope. A stretch set with a block acts on audio that went in a little earlier (its
    // windows look back), so each block asks for the slope at the place it will act on.
    constexpr int kBlock = 256;
    const auto slopeAt = [&](double flattenedSample)
    {
        const double a = std::clamp(flattenedSample - kBlock / 2.0, 0.0, static_cast<double>(length));
        const double b = std::clamp(flattenedSample + kBlock / 2.0, 0.0, static_cast<double>(length));
        return b > a ? std::clamp((place(b) - place(a)) / (b - a), 0.125, 8.0) : 1.0;
    };

    Stretcher stretcher(static_cast<std::size_t>(std::lround(sampleRate)), channels,
                        Stretcher::OptionProcessRealTime | Stretcher::OptionEngineFiner | Stretcher::OptionChannelsTogether
                            | (character == Character::Voice ? Stretcher::OptionFormantPreserved : Stretcher::OptionFormantShifted),
                        slopeAt(0.0), 1.0);
    stretcher.setMaxProcessSize(kBlock);
    // How far back: measured with clicks on a sound whose stretch changes (Tests/CoreTests.cpp),
    // the clicks land within 2 ms when a block's stretch is taken from 0.94 of the stretcher's
    // start delay behind it; at none of it they run 20 ms late, at twice 22 ms early.
    const double align = 0.94 * static_cast<double>(stretcher.getStartDelay());

    Channels out(channels);
    for (auto& channel : out)
        channel.reserve(sourceLength + 16384);
    std::vector<std::vector<float>> inBlock(channels, std::vector<float>(kBlock, 0.0f)), outBlock(channels, std::vector<float>(8192));
    std::vector<const float*> in(channels);
    std::vector<float*> outPointers(channels);
    for (std::size_t c = 0; c < channels; ++c)
        outPointers[c] = outBlock[c].data();
    const auto drain = [&]
    {
        for (;;)
        {
            const int available = stretcher.available();
            if (available <= 0)
                return;
            const auto got = stretcher.retrieve(outPointers.data(), std::min<std::size_t>(8192, static_cast<std::size_t>(available)));
            for (std::size_t c = 0; c < channels; ++c)
                out[c].insert(out[c].end(), outBlock[c].begin(), outBlock[c].begin() + static_cast<std::ptrdiff_t>(got));
        }
    };

    // Silence first, so the sound's first samples come out clean; the delay is cut off after.
    for (auto pad = static_cast<std::int64_t>(stretcher.getPreferredStartPad()); pad > 0; pad -= kBlock)
    {
        for (std::size_t c = 0; c < channels; ++c)
            in[c] = inBlock[c].data();
        stretcher.process(in.data(), static_cast<std::size_t>(std::min<std::int64_t>(pad, kBlock)), false);
        drain();
    }
    const auto delay = static_cast<std::size_t>(stretcher.getStartDelay());

    // The sound, then silence until all of the result has come out.
    const std::int64_t wanted = static_cast<std::int64_t>(sourceLength + delay);
    for (std::int64_t at = 0; static_cast<std::int64_t>(out[0].size()) < wanted && at < length + 64 * 8192; at += kBlock)
    {
        if ((at & 0xffff) == 0 && cancelled && cancelled())
            return {};
        const auto n = static_cast<std::size_t>(std::clamp<std::int64_t>(length - at, 0, kBlock));
        for (std::size_t c = 0; c < channels; ++c)
        {
            if (n > 0)
                std::copy_n(flattened[c].begin() + at, n, inBlock[c].begin());
            std::fill(inBlock[c].begin() + static_cast<std::ptrdiff_t>(n), inBlock[c].end(), 0.0f);
            in[c] = inBlock[c].data();
        }
        const double stretch = slopeAt(static_cast<double>(at) + kBlock / 2.0 - align);
        stretcher.setTimeRatio(stretch);
        // Voice: where the sound was slowed by some amount its formants came down by the same
        // amount, and the stretch that undoes the slowing is that same amount: shifting the
        // envelope by it puts the formants back where the voice had them.
        if (character == Character::Voice)
        {
            const double speed = std::clamp((landsAt(static_cast<double>(at) + kBlock - align) - landsAt(static_cast<double>(at) - align)) / kBlock, 0.125, 8.0);
            stretcher.setFormantScale(1.0 / std::clamp(speed, 0.25, 4.0));
        }
        stretcher.process(in.data(), kBlock, false);
        drain();
    }

    for (auto& channel : out)
    {
        channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(std::min(delay, channel.size())));
        channel.resize(sourceLength, 0.0f);
    }

    // Transients: even at a stretch of exactly 1 the stretcher softens a hit a little (it still
    // takes the sound apart and puts it back). So the hit itself is laid in untouched, from
    // 4 ms before it to 30 ms after, faded into the stretched sound over 2 ms at each end. The
    // stretch there was 1 and placed on the same spot, so the two line up.
    if (! hitPlaces.empty())
    {
        const auto lead = static_cast<std::int64_t>(0.004 * sampleRate), hold = static_cast<std::int64_t>(0.030 * sampleRate);
        const auto fade = std::max<std::int64_t>(1, static_cast<std::int64_t>(0.002 * sampleRate));
        for (const auto& [from, to] : hitPlaces)
            for (std::int64_t k = -lead; k < hold; ++k)
            {
                const std::int64_t source = static_cast<std::int64_t>(from) + k, target = to + k;
                if (source < 0 || source >= length || target < 0 || target >= static_cast<std::int64_t>(sourceLength))
                    continue;
                const double edge = static_cast<double>(std::min(k + lead, hold - 1 - k)) + 0.5;
                const auto w = static_cast<float>(std::clamp(edge / static_cast<double>(fade), 0.0, 1.0));
                for (std::size_t c = 0; c < channels; ++c)
                {
                    float& v = out[c][static_cast<std::size_t>(target)];
                    v += w * (flattened[c][static_cast<std::size_t>(source)] - v);
                }
            }
    }
    return out;
}

} // namespace pitchflattener::core
