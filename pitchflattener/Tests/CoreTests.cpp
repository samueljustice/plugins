// Pitch Flattener's core against sounds whose pitch is known: no host, no JUCE.
//
// Each case makes a sound whose pitch moves in a known way, flattens it, and measures the
// result's pitch with a meter that shares nothing with the trackers (the period, from the
// autocorrelation of short stretches).

#include "core/Flatten.h"
#include "core/HarmonicTracker.h"
#include "core/KeepLength.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace pitchflattener::core;

namespace
{
int g_checks = 0, g_failed = 0;

void check(bool ok, const std::string& what)
{
    ++g_checks;
    if (! ok)
    {
        ++g_failed;
        std::printf("    FAILED: %s\n", what.c_str());
    }
}

void beginCase(const char* name) { std::printf("%s\n", name); }

constexpr double kRate = 48000.0;
constexpr double kTwoPi = 6.283185307179586;

// A sound whose fundamental follows hzAt(seconds), with the given harmonic amplitudes, plus noise.
std::vector<float> make(double seconds, const std::function<double(double)>& hzAt, const std::vector<double>& harmonics, double noise = 0.0,
                        const std::function<double(double)>& levelAt = {})
{
    const auto n = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> out(n);
    double phase = 0.0;
    std::uint32_t seed = 2463534242u;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / kRate;
        phase += kTwoPi * hzAt(t) / kRate;
        double v = 0.0;
        for (std::size_t h = 0; h < harmonics.size(); ++h)
            v += harmonics[h] * std::sin(phase * static_cast<double>(h + 1));
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        v += noise * (static_cast<double>(seed) / 2147483648.0 - 1.0);
        out[i] = static_cast<float>(0.3 * v * (levelAt ? levelAt(t) : 1.0));
    }
    return out;
}

// The pitch (MIDI) of a stretch of sound from its period, or 0 if it has none. For the clean
// test sounds only.
double meter(const std::vector<float>& x, std::size_t start, std::size_t length, double loHz, double hiHz)
{
    if (start + length > x.size())
        return 0.0;
    const int minLag = std::max(2, static_cast<int>(kRate / hiHz)), maxLag = std::min(static_cast<int>(length / 2), static_cast<int>(kRate / loHz));
    double energy = 0.0;
    for (std::size_t i = 0; i < length; ++i)
        energy += static_cast<double>(x[start + i]) * x[start + i];
    if (energy < 1.0e-9)
        return 0.0;
    std::vector<double> r(static_cast<std::size_t>(maxLag) + 2, 0.0);
    double best = 0.0;
    for (int lag = minLag - 1; lag <= maxLag + 1; ++lag)
    {
        double sum = 0.0, a = 0.0, b = 0.0;
        for (std::size_t i = 0; i + static_cast<std::size_t>(lag) < length; ++i)
        {
            const double u = x[start + i], v = x[start + i + static_cast<std::size_t>(lag)];
            sum += u * v; a += u * u; b += v * v;
        }
        r[static_cast<std::size_t>(lag)] = sum / std::sqrt(a * b + 1.0e-12);
        best = std::max(best, r[static_cast<std::size_t>(lag)]);
    }
    if (best < 0.5)
        return 0.0;
    // The shortest lag that is nearly as good as the best: the period, not a multiple of it.
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        const auto l = static_cast<std::size_t>(lag);
        if (r[l] >= 0.92 * best && r[l] >= r[l - 1] && r[l] >= r[l + 1])
        {
            const double d = 0.5 * (r[l - 1] - r[l + 1]) / (r[l - 1] - 2.0 * r[l] + r[l + 1] - 1.0e-12);
            return hzToMidi(kRate / (static_cast<double>(lag) + std::clamp(d, -0.5, 0.5)));
        }
    }
    return 0.0;
}

struct Travel
{
    double low = 0.0, high = 0.0, median = 0.0;
    int readings = 0;
    double span() const { return high - low; }
};

// How far the pitch of a sound travels: readings every 20 ms over 40 ms stretches, the outer 5% dropped.
Travel travelOf(const std::vector<float>& x, double loHz, double hiHz, double fromSeconds = 0.0, double toSeconds = 1.0e9)
{
    std::vector<double> midi;
    const auto step = static_cast<std::size_t>(0.02 * kRate), length = static_cast<std::size_t>(0.04 * kRate);
    for (std::size_t at = static_cast<std::size_t>(fromSeconds * kRate); at + length <= x.size() && static_cast<double>(at) / kRate < toSeconds; at += step)
        if (const double m = meter(x, at, length, loHz, hiHz); m > 0.0)
            midi.push_back(m);
    Travel t;
    t.readings = static_cast<int>(midi.size());
    if (midi.empty())
        return t;
    std::sort(midi.begin(), midi.end());
    t.low = midi[midi.size() / 20];
    t.high = midi[midi.size() - 1 - midi.size() / 20];
    t.median = midi[midi.size() / 2];
    return t;
}

std::string st(double v) { char b[32]; std::snprintf(b, sizeof(b), "%.2f st", v); return b; }

struct Run
{
    Analysis analysis;
    Flattened result;
};

Run run(const std::vector<float>& sound, Options options = {}, bool score = true)
{
    Run r;
    const Channels channels { sound };
    r.analysis = analyse(channels, kRate, score);
    r.result = flatten(channels, kRate, r.analysis, options);
    return r;
}

const std::vector<double> kRich { 1.0, 0.6, 0.45, 0.3, 0.25, 0.2, 0.15, 0.12 };

//==============================================================================
void testPassBy()
{
    beginCase("a pass-by: a rich tone falling 7 semitones through the middle");
    const auto hz = [](double t) { return 220.0 * std::exp2((3.5 - 7.0 / (1.0 + std::exp(-(t - 2.0) * 4.0))) / 12.0); };
    const auto sound = make(4.0, hz, kRich, 0.02);
    const auto before = travelOf(sound, 100.0, 500.0);
    const auto r = run(sound);
    check(r.analysis.valid && r.analysis.pick >= 0, "a line was found");
    check(r.result.valid, "it was flattened");
    if (! r.result.valid) return;
    const auto after = travelOf(r.result.channels[0], 100.0, 500.0);
    std::printf("    -> %s line; travels %s before, %s after; length %.2f s -> %.2f s\n", r.analysis.pick >= 0 ? trackerName(static_cast<Tracker>(r.analysis.pick)) : "no", st(before.span()).c_str(),
                st(after.span()).c_str(), sound.size() / kRate, r.result.channels[0].size() / kRate);
    check(before.span() > 6.0, "the meter reads the fall in the source: " + st(before.span()));
    check(after.span() < 0.25, "flattened, the pitch travels under a quarter of a semitone: " + st(after.span()));
    check(std::abs(after.median - hzToMidi(220.0 * std::exp2(3.5 / 12.0))) < 0.3, "and sits on the high point: " + st(after.median));
    check(r.result.channels[0].size() <= sound.size() + 480, "flattening to the high point never makes the sound longer");

    Options low;
    low.target = Target::LowPoint;
    const auto toLow = run(sound, low, false);
    check(toLow.result.valid && toLow.result.channels[0].size() > sound.size(), "flattening to the low point makes it longer");
    const auto lowAfter = travelOf(toLow.result.channels[0], 100.0, 500.0);
    check(lowAfter.span() < 0.25 && std::abs(lowAfter.median - hzToMidi(220.0 * std::exp2(-3.5 / 12.0))) < 0.3, "and flat on the low point: " + st(lowAfter.median));

    Options half;
    half.amount = 0.5;
    const auto halved = run(sound, half, false);
    const auto halfAfter = travelOf(halved.result.channels[0], 100.0, 500.0);
    check(std::abs(halfAfter.span() - before.span() * 0.5) < 0.5, "at half amount, half the movement is left: " + st(halfAfter.span()));

    Options note;
    note.target = Target::Note;
    note.noteMidi = 57.0;
    const auto toNote = run(sound, note, false);
    const auto noteAfter = travelOf(toNote.result.channels[0], 100.0, 500.0);
    check(std::abs(noteAfter.median - 57.0) < 0.2 && noteAfter.span() < 0.25, "flattened to a chosen note it lands on it: " + st(noteAfter.median));
}

void testSiren()
{
    beginCase("a siren: a tone wailing between 700 and 1700 Hz");
    const auto hz = [](double t) { return 700.0 * std::exp2(std::log2(1700.0 / 700.0) * (0.5 - 0.5 * std::cos(kTwoPi * t / 4.0))); };
    const auto sound = make(8.0, hz, { 1.0, 0.25, 0.1 }, 0.03);
    const auto before = travelOf(sound, 500.0, 2500.0);
    const auto r = run(sound);
    check(r.result.valid, "it was flattened");
    if (! r.result.valid) return;
    const auto after = travelOf(r.result.channels[0], 500.0, 2500.0);
    std::printf("    -> %s line; travels %s before, %s after\n", r.analysis.pick >= 0 ? trackerName(static_cast<Tracker>(r.analysis.pick)) : "no", st(before.span()).c_str(), st(after.span()).c_str());
    check(before.span() > 13.0, "the meter reads the wail: " + st(before.span()));
    check(after.span() < 0.4, "flattened, it travels under 0.4 st: " + st(after.span()));
    check(r.analysis.lines[2].usable, "the tone line reads a sound above the voice and engine trackers' reach");
}

void testRicochet()
{
    beginCase("a ricochet: a whine falling an octave and a half in 0.15 s, in noise, then ringing on");
    const auto hz = [](double t)
    {
        if (t < 0.02) return 800.0;
        if (t < 0.17) return 800.0 * std::exp2(-17.0 * (t - 0.02) / 0.15 / 12.0);
        return 800.0 * std::exp2(-17.0 / 12.0) * std::exp2(2.0 * (t - 0.17) / 12.0);
    };
    const auto level = [](double t) { return t < 0.01 ? t / 0.01 : std::exp(-(t - 0.01) * 2.5); };
    const auto sound = make(1.2, hz, { 1.0, 0.3 }, 0.25, level);
    const auto r = run(sound);
    check(r.result.valid, "it was flattened");
    if (! r.result.valid) return;
    const auto* line = r.analysis.picked();
    const double fall = line->midi[4] - line->midi[static_cast<std::size_t>(0.2 / line->hopSeconds)];
    std::printf("    -> %s line; reads a fall of %s in the first 0.2 s\n", r.analysis.pick >= 0 ? trackerName(static_cast<Tracker>(r.analysis.pick)) : "no", st(fall).c_str());
    check(fall > 13.0 && fall < 20.0, "the line follows the fall (17 st in the sound): " + st(fall));
    // Read the result where the whine is: the fall is 0.15 s of the source.
    const double whineEnds = r.result.outputSampleFor(0.6 * kRate) / kRate;
    const auto after = travelOf(r.result.channels[0], 200.0, 1200.0, 0.0, whineEnds);
    check(after.readings >= 5 && after.span() < 1.5, "flattened, the whine travels under 1.5 st: " + st(after.span()));
}

void testRunUp()
{
    beginCase("an engine run-up: a harmonic series climbing from 60 to 200 Hz");
    const auto hz = [](double t) { return 60.0 * std::exp2(std::log2(200.0 / 60.0) * std::min(1.0, t / 5.0)); };
    std::vector<double> harmonics;
    for (int h = 1; h <= 14; ++h)
        harmonics.push_back(h == 1 ? 0.3 : 1.0 / std::sqrt(static_cast<double>(h)));
    const auto sound = make(6.0, hz, harmonics, 0.05);
    const auto before = travelOf(sound, 40.0, 300.0);
    const auto r = run(sound);
    check(r.result.valid, "it was flattened");
    if (! r.result.valid) return;
    const auto after = travelOf(r.result.channels[0], 40.0, 300.0);
    std::printf("    -> %s line; travels %s before, %s after\n", r.analysis.pick >= 0 ? trackerName(static_cast<Tracker>(r.analysis.pick)) : "no", st(before.span()).c_str(), st(after.span()).c_str());
    check(before.span() > 18.0, "the meter reads the climb: " + st(before.span()));
    check(after.span() < 0.6, "flattened, it travels under 0.6 st: " + st(after.span()));
}

void testVibrato()
{
    beginCase("vibrato: a voice-like tone wobbling a semitone at 6 Hz");
    const auto hz = [](double t) { return 330.0 * std::exp2(0.5 * std::sin(kTwoPi * 6.0 * t) / 12.0); };
    const auto sound = make(3.0, hz, kRich, 0.02);
    const auto before = travelOf(sound, 150.0, 700.0);
    Options centre;
    centre.target = Target::Centre;
    const auto r = run(sound, centre);
    check(r.result.valid, "it was flattened");
    if (! r.result.valid) return;
    const auto after = travelOf(r.result.channels[0], 150.0, 700.0);
    std::printf("    -> travels %s before, %s after\n", st(before.span()).c_str(), st(after.span()).c_str());
    check(before.span() > 0.8 && after.span() < 0.3, "the wobble is taken out: " + st(after.span()));
    check(std::abs(static_cast<double>(r.result.channels[0].size()) - static_cast<double>(sound.size())) < 0.02 * static_cast<double>(sound.size()),
          "flattened to its centre it keeps its length within 2%");
}

void testVoiceBreak()
{
    beginCase("a voice that breaks into a growl: the reading leaps, the flattening must not");
    // A scream rising from 500 to 700 Hz, then a growl: the same tone, but pulsing at a
    // thirteenth of the pitch, which is what a tracker then reads.
    const double sr = kRate;
    const auto n = static_cast<std::size_t>(4.0 * sr);
    std::vector<float> sound(n);
    double phase = 0.0;
    std::uint32_t seed = 88172645u;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / sr;
        const bool growl = t >= 0.8;
        const double tone = t < 0.8 ? 500.0 * std::exp2(std::log2(700.0 / 500.0) * t / 0.8) : 700.0;
        const double hz = growl ? tone / 13.0 : tone;
        phase += kTwoPi * hz / sr;
        double v = 0.0;
        if (growl)
            for (int h = 1; h <= 40; ++h)   // a dense series with its weight around the old tone
                v += std::exp(-0.5 * std::pow((h - 13.0) / 5.0, 2.0)) * std::sin(phase * h + 0.7 * h * h);
        else
            for (int h = 1; h <= 6; ++h)
                v += std::sin(phase * h) / h;
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        sound[i] = static_cast<float>(0.2 * v + 0.01 * (static_cast<double>(seed) / 2147483648.0 - 1.0));
    }
    const auto r = run(sound);
    check(r.result.valid, "it was flattened");
    if (! r.result.valid) return;
    const auto* line = r.analysis.picked();
    float biggestStep = 0.0f, low = 1.0e9f, high = -1.0e9f, readLow = 1.0e9f, readHigh = -1.0e9f;
    for (std::size_t f = 0; f < line->midi.size(); ++f)
    {
        if (f > 0) biggestStep = std::max(biggestStep, std::abs(line->midi[f] - line->midi[f - 1]));
        low = std::min(low, line->midi[f]); high = std::max(high, line->midi[f]);
        if (line->trackedHz[f] > 0.0f)
        {
            const auto m = static_cast<float>(hzToMidi(line->trackedHz[f]));
            readLow = std::min(readLow, m); readHigh = std::max(readHigh, m);
        }
    }
    std::printf("    -> %s line; the reading spans %s, the line used spans %s with %d leaps taken out; length %.2f s -> %.2f s\n", r.analysis.pick >= 0 ? trackerName(static_cast<Tracker>(r.analysis.pick)) : "no",
                st(readHigh - readLow).c_str(), st(high - low).c_str(), line->leaps, sound.size() / kRate, r.result.channels[0].size() / kRate);
    check(biggestStep < 0.5f, "the line used never steps by half a semitone between frames: " + st(biggestStep));
    check(high - low < 9.0f, "and spans the scream's rise (5.8 st), not the leap: " + st(high - low));
    const double ratio = static_cast<double>(r.result.channels[0].size()) / static_cast<double>(sound.size());
    check(ratio > 0.6 && ratio <= 1.01, "so the flattened sound is not stretched or squeezed out of shape: " + std::to_string(ratio));
}

void testKeepLength()
{
    beginCase("keep length: flat pitch, every moment where it was");
    // The pass-by again, with a short click every half second to time against.
    const auto hz = [](double t) { return 220.0 * std::exp2((3.5 - 7.0 / (1.0 + std::exp(-(t - 2.0) * 4.0))) / 12.0); };
    auto sound = make(4.0, hz, kRich, 0.01);
    std::vector<double> clicks;
    std::uint32_t seed = 1234567u;
    for (double at = 0.5; at < 3.9; at += 0.5)
    {
        clicks.push_back(at);
        const auto start = static_cast<std::size_t>(at * kRate);
        for (std::size_t i = 0; i < 240; ++i)   // 5 ms of noise
        {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            sound[start + i] += static_cast<float>(0.5 * (static_cast<double>(seed) / 2147483648.0 - 1.0) * (1.0 - static_cast<double>(i) / 240.0));
        }
    }
    // Where the clicks are in a sound: the loudest 2 ms of its sample-to-sample change near each expected time.
    const auto findClick = [](const std::vector<float>& x, double near)
    {
        const auto from = static_cast<std::size_t>(std::max(0.0, near - 0.12) * kRate), to = std::min(x.size() - 97, static_cast<std::size_t>((near + 0.12) * kRate));
        double best = -1.0, at = near;
        for (std::size_t i = from; i < to; i += 8)
        {
            double e = 0.0;
            for (std::size_t k = 1; k < 96; ++k)
                e += std::pow(static_cast<double>(x[i + k]) - x[i + k - 1], 2.0);
            if (e > best) { best = e; at = static_cast<double>(i) / kRate; }
        }
        return at;
    };

    Options options;
    options.keepLength = true;
    const auto r = run(sound, options, false);
    check(r.result.valid, "it was flattened");
    if (! r.result.valid) return;
    const auto& out = r.result.channels[0];
    check(out.size() == sound.size(), "the result is exactly as long as the sound: " + std::to_string(out.size()) + " samples");
    const auto after = travelOf(out, 100.0, 500.0);
    check(after.span() < 0.4, "its pitch travels under 0.4 st: " + st(after.span()));
    check(std::abs(after.median - hzToMidi(220.0 * std::exp2(3.5 / 12.0))) < 0.3, "and sits on the high point: " + st(after.median));
    double worst = 0.0;
    for (const double at : clicks)
    {
        const double error = findClick(out, at) - findClick(sound, at);
        worst = std::max(worst, std::abs(error));
    }
    std::printf("    -> travels %s; clicks land within %.1f ms of where they were\n", st(after.span()).c_str(), worst * 1000.0);
    check(worst < 0.004, "every click is within 4 ms of where it was: " + std::to_string(worst * 1000.0) + " ms");
    check(std::abs(r.result.outputSampleFor(2.0 * kRate) - 2.0 * kRate) < 1.0, "and the time map is one to one");

    // The same without keep length moves them (that is the difference between the two modes).
    const auto v = run(sound, {}, false);
    const double moved = v.result.valid ? v.result.outputSampleFor(3.5 * kRate) / kRate : 3.5;
    check(v.result.valid && std::abs(moved - 3.5) > 0.2 && std::abs(findClick(v.result.channels[0], moved) - moved) < 0.01,
          "by speed alone, a late click moves by more than 0.2 s (to " + std::to_string(moved) + " s)");

    int asked = 0;
    const Channels channels { sound };
    const auto stopped = flatten(channels, kRate, r.analysis, options, [&] { return ++asked > 3; });   // lets the first stage finish, stops the second
    check(! stopped.valid, "a cancelled keep-length render says so");
}

// Where the weight of a stretch of sound's spectrum sits between 300 Hz and 4 kHz (Hz).
double spectralCentre(const std::vector<float>& x, double fromSeconds, double seconds)
{
    const auto start = static_cast<std::size_t>(fromSeconds * kRate), n = static_cast<std::size_t>(seconds * kRate);
    if (start + n > x.size())
        return 0.0;
    double weighted = 0.0, total = 0.0;
    for (double hz = 300.0; hz <= 4000.0; hz += 20.0)
    {
        double re = 0.0, im = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(n));
            const double ph = kTwoPi * hz * static_cast<double>(i) / kRate;
            re += w * x[start + i] * std::cos(ph);
            im += w * x[start + i] * std::sin(ph);
        }
        const double power = re * re + im * im;
        weighted += power * hz;
        total += power;
    }
    return total > 0.0 ? weighted / total : 0.0;
}

void testCharacters()
{
    beginCase("keep length, voice: the voice keeps its size");
    {
        // A falling voice whose formant stays at 1200 Hz: harmonics of a moving pitch under a fixed bump.
        const auto pitchAt = [](double t) { return 220.0 * std::exp2((3.5 - 7.0 / (1.0 + std::exp(-(t - 2.0) * 4.0))) / 12.0); };
        const auto n = static_cast<std::size_t>(4.0 * kRate);
        std::vector<float> sound(n);
        double phase = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double f0 = pitchAt(static_cast<double>(i) / kRate);
            phase += kTwoPi * f0 / kRate;
            double v = 0.0;
            for (int h = 1; h * f0 < 5000.0; ++h)
                v += (0.03 + std::exp(-0.5 * std::pow((h * f0 - 1200.0) / 180.0, 2.0))) * std::sin(phase * h);
            sound[i] = static_cast<float>(0.2 * v);
        }
        const double before = spectralCentre(sound, 3.4, 0.2);
        Options options;
        options.keepLength = true;
        const auto plain = run(sound, options, false);
        options.character = Character::Voice;
        const auto voice = run(sound, options, false);
        check(plain.result.valid && voice.result.valid, "both were flattened");
        if (! plain.result.valid || ! voice.result.valid) return;
        const double asHarmonic = spectralCentre(plain.result.channels[0], 3.4, 0.2), asVoice = spectralCentre(voice.result.channels[0], 3.4, 0.2);
        const auto travel = travelOf(voice.result.channels[0], 100.0, 500.0);
        std::printf("    -> near the end the formant sits at %.0f Hz in the sound, %.0f Hz as Harmonic, %.0f Hz as Voice; pitch travels %s\n", before, asHarmonic, asVoice,
                    st(travel.span()).c_str());
        check(asHarmonic > before * 1.25, "by speed the formant rises with the pitch (Harmonic leaves it there): " + std::to_string(asHarmonic));
        check(std::abs(asVoice - before) < before * 0.1, "Voice puts it back within 10%: " + std::to_string(asVoice));
        check(travel.span() < 0.5 && voice.result.channels[0].size() == sound.size(), "with the pitch still flat and the length kept: " + st(travel.span()));
    }

    beginCase("keep length, transients: hits stay sharp and in place");
    {
        // A tone falling a long way with hard clicks on it.
        const double fall = 19.0;   // a big fall, so the stretch is large (up to three times): where a hit suffers
        const auto hz = [fall](double t) { return 220.0 * std::exp2((fall / 2.0 - fall / (1.0 + std::exp(-(t - 2.0) * 4.0))) / 12.0); };
        auto sound = make(4.0, hz, kRich, 0.01);
        std::vector<double> clicks;
        std::uint32_t seed = 7654321u;
        for (double at = 0.5; at < 3.9; at += 0.5)
        {
            clicks.push_back(at);
            const auto start = static_cast<std::size_t>(at * kRate);
            for (std::size_t i = 0; i < 144; ++i)   // 3 ms of noise
            {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                sound[start + i] += static_cast<float>(0.6 * (static_cast<double>(seed) / 2147483648.0 - 1.0) * (1.0 - static_cast<double>(i) / 144.0));
            }
        }
        const auto hits = findHits({ sound }, kRate);
        int found = 0;
        for (const double at : clicks)
            for (const auto h : hits)
                found += std::abs(static_cast<double>(h) / kRate - at) < 0.006 ? 1 : 0;
        check(found == static_cast<int>(clicks.size()) && hits.size() <= clicks.size() + 1,
              "every click is found as a hit, and nothing else: " + std::to_string(hits.size()) + " hits for " + std::to_string(clicks.size()) + " clicks");

        // How sharp a click is: the share of the energy within 40 ms of it that lies in its own 4 ms.
        const auto sharpness = [](const std::vector<float>& x, double at)
        {
            const auto centre = static_cast<std::size_t>(at * kRate);
            double inside = 0.0, around = 0.0;
            for (std::size_t i = centre - 1920; i < centre + 1920; ++i)
            {
                const double d = static_cast<double>(x[i]) - x[i - 1];   // high frequencies: the click, not the tone
                around += d * d;
                if (i >= centre - 48 && i < centre + 192) inside += d * d;
            }
            return inside / around;
        };
        Options options;
        options.keepLength = true;
        const auto plain = run(sound, options, false);
        options.character = Character::Transients;
        const auto sharp = run(sound, options, false);
        check(plain.result.valid && sharp.result.valid, "both were flattened");
        if (! plain.result.valid || ! sharp.result.valid) return;
        double source = 0.0, asHarmonic = 0.0, asTransients = 0.0;
        for (const double at : clicks)
        {
            source += sharpness(sound, at) / static_cast<double>(clicks.size());
            asHarmonic += sharpness(plain.result.channels[0], at) / static_cast<double>(clicks.size());
            asTransients += sharpness(sharp.result.channels[0], at) / static_cast<double>(clicks.size());
        }
        // The best any of this can do is what flattening by speed alone left of each click
        // (it is the same audio, before the stretch); and the stretch must not smear a click
        // backwards in time (the level just before it, against the click itself).
        const auto v = run(sound, {}, false);
        const auto levelBefore = [](const std::vector<float>& x, double at)
        {
            const auto c = static_cast<std::size_t>(at * kRate);
            double pre = 0.0, in = 0.0;
            for (std::size_t i = c - 576; i < c - 96; ++i) { const double d = static_cast<double>(x[i]) - x[i - 1]; pre += d * d; }
            for (std::size_t i = c - 48; i < c + 192; ++i) { const double d = static_cast<double>(x[i]) - x[i - 1]; in += d * d; }
            return 10.0 * std::log10(pre / in + 1.0e-12);
        };
        double bySpeed = 0.0, beforeSpeed = 0.0, beforeHarmonic = 0.0, beforeTransients = 0.0;
        const auto count = static_cast<double>(clicks.size());
        for (const double at : clicks)
        {
            const double moved = v.result.outputSampleFor(at * kRate) / kRate;
            bySpeed += sharpness(v.result.channels[0], moved) / count;
            beforeSpeed += levelBefore(v.result.channels[0], moved) / count;
            beforeHarmonic += levelBefore(plain.result.channels[0], at) / count;
            beforeTransients += levelBefore(sharp.result.channels[0], at) / count;
        }
        const auto travel = travelOf(sharp.result.channels[0], 100.0, 500.0);
        std::printf("    -> click sharpness: %.2f in the sound, %.2f by speed alone, %.2f as Harmonic, %.2f as Transients\n", source, bySpeed, asHarmonic, asTransients);
        std::printf("    -> level just before a click, against the click: %.1f dB by speed alone, %.1f dB as Harmonic, %.1f dB as Transients\n", beforeSpeed, beforeHarmonic,
                    beforeTransients);
        check(asTransients > asHarmonic + 0.05, "Transients keeps the clicks sharper than Harmonic: " + std::to_string(asTransients) + " against " + std::to_string(asHarmonic));
        check(asTransients > 0.95 * bySpeed, "as sharp as flattening by speed left them: " + std::to_string(asTransients) + " of " + std::to_string(bySpeed));
        check(beforeTransients < beforeSpeed + 1.0, "with nothing smeared ahead of a click: " + std::to_string(beforeTransients) + " dB");
        check(beforeHarmonic > beforeTransients + 2.0, "where Harmonic smears some: " + std::to_string(beforeHarmonic) + " dB");
        check(travel.span() < 0.5 && sharp.result.channels[0].size() == sound.size(), "with the pitch still flat and the length kept: " + st(travel.span()));
    }
}

void testSeveralSounds()
{
    beginCase("several sounds in one recording: each is flattened to its own pitch");
    // A call falling from 330 Hz, half a second of silence, then one falling from 520 Hz.
    const auto first = make(1.5, [](double t) { return 330.0 * std::exp2(-5.0 * t / 1.5 / 12.0); }, kRich, 0.0);
    const auto second = make(1.5, [](double t) { return 520.0 * std::exp2(-4.0 * t / 1.5 / 12.0); }, kRich, 0.0);
    std::vector<float> sound(first);
    sound.insert(sound.end(), static_cast<std::size_t>(0.5 * kRate), 0.0f);
    sound.insert(sound.end(), second.begin(), second.end());
    const auto r = run(sound);
    check(r.analysis.events.size() == 2, "two sounds are found: " + std::to_string(r.analysis.events.size()));
    check(r.result.valid && r.result.eventTargets.size() == 2, "each has its own target");
    if (! r.result.valid || r.result.eventTargets.size() != 2) return;
    const double a0 = r.result.outputSampleFor(0.1 * kRate) / kRate, a1 = r.result.outputSampleFor(1.4 * kRate) / kRate;
    const double b0 = r.result.outputSampleFor(2.1 * kRate) / kRate, b1 = r.result.outputSampleFor(3.4 * kRate) / kRate;
    const auto one = travelOf(r.result.channels[0], 150.0, 900.0, a0, a1), two = travelOf(r.result.channels[0], 150.0, 900.0, b0, b1);
    std::printf("    -> first sound flat at %s (travels %s), second at %s (travels %s)\n", noteName(one.median).c_str(), st(one.span()).c_str(), noteName(two.median).c_str(),
                st(two.span()).c_str());
    check(one.span() < 0.3 && two.span() < 0.3, "both come out flat");
    check(std::abs(one.median - hzToMidi(330.0)) < 0.4, "the first on its own high point: " + st(one.median));
    check(std::abs(two.median - hzToMidi(520.0)) < 0.4, "the second on its own, not the first's: " + st(two.median));

    Options note;
    note.target = Target::Note;
    note.noteMidi = 60.0;
    const auto n = run(sound, note, false);
    check(n.result.valid && std::abs(n.result.eventTargets[0] - 60.0) < 1.0e-9 && std::abs(n.result.eventTargets[1] - 60.0) < 1.0e-9, "a chosen note is the target of every sound");
}

void testNothingToFlatten()
{
    beginCase("noise and silence: nothing to flatten");
    const auto noise = make(2.0, [](double) { return 100.0; }, {}, 1.0);
    const auto r = run(noise);
    check(r.analysis.valid, "noise is analysed");
    for (const auto& line : r.analysis.lines)
        std::printf("    -> %s line in noise: %s, found in %.0f%%, unsteadiness %.3f st\n", trackerName(line.tracker),
                    line.usable ? "usable" : "not usable", line.coverage * 100.0, line.jitter);
    check(r.analysis.pick < 0 && ! r.result.valid, "no steady pitch is found in noise, and nothing is rendered");
    const std::vector<float> silence(48000, 0.0f);
    const auto s = run(silence);
    check(s.analysis.pick < 0 && ! s.result.valid, "nor in silence");
    const auto none = analyse({}, kRate);
    check(! none.valid, "no audio at all is not an analysis");
}

void testMapAndOptions()
{
    beginCase("the time map, the hand corrections, stereo and cancelling");
    const auto hz = [](double t) { return 300.0 * std::exp2(-6.0 * t / 3.0 / 12.0); };
    const auto sound = make(3.0, hz, kRich, 0.01);
    const auto r = run(sound, {}, false);
    check(r.result.valid, "it was flattened");
    if (! r.result.valid) return;
    const auto& map = r.result.outputAtSource;
    bool rising = true;
    for (std::size_t i = 1; i < map.size(); ++i)
        rising = rising && map[i] >= map[i - 1];
    check(rising && map.size() == sound.size() / Flattened::kMapStep + 2, "the map rises and covers the sound");
    check(std::abs(map.front()) < 1.0 && std::abs(map.back() - static_cast<double>(r.result.channels[0].size())) < 2.0, "it starts at the start and ends at the end");
    const double there = r.result.outputSampleFor(1.5 * kRate);
    check(std::abs(r.result.sourceSampleFor(there) - 1.5 * kRate) < 2.0, "there and back gives the same moment");
    // The high point is the start (it falls), so the start plays at its own speed and the end fastest.
    check(r.result.outputSampleFor(0.5 * kRate) > 0.45 * kRate && there < 1.5 * kRate, "moments move as the speed says");

    // A hand correction: say the line read a semitone flat over the second second.
    Options edited;
    edited.edits = { { 1.0, 1.0 }, { 2.0, 1.0 } };
    const auto e = run(sound, edited, false);
    const auto plain = travelOf(r.result.channels[0], 150.0, 500.0, 0.2, 0.8), moved = travelOf(e.result.channels[0], 150.0, 500.0, 1.15, 1.45);
    check(e.result.valid && std::abs((plain.median - moved.median) - 1.0) < 0.2, "a correction of +1 st lowers the result there by a semitone: " + st(plain.median - moved.median));
    check(std::abs(editAt(edited.edits, 0.875) - 0.5) < 1.0e-9 && std::abs(editAt(edited.edits, 0.7)) < 1.0e-12 && std::abs(editAt(edited.edits, 1.5) - 1.0) < 1.0e-12, "corrections ease out over a quarter of a second");

    // Stereo: both channels read at the same speed.
    Channels stereo { sound, sound };
    for (auto& v : stereo[1]) v *= 0.5f;
    const auto a = analyse(stereo, kRate, false);
    const auto f = flatten(stereo, kRate, a, {});
    bool together = f.valid && f.channels.size() == 2 && f.channels[0].size() == f.channels[1].size();
    for (std::size_t i = 0; together && i < f.channels[0].size(); i += 97)
        together = std::abs(f.channels[1][i] - 0.5f * f.channels[0][i]) < 1.0e-5f;
    check(together, "stereo channels stay in step");

    int asked = 0;
    const auto stopped = flatten({ sound }, kRate, r.analysis, {}, [&] { return ++asked > 1; });
    check(! stopped.valid && stopped.channels.empty(), "a cancelled render says so");
    const auto stoppedAnalysis = analyse({ sound }, kRate, true, [] { return true; });
    check(! stoppedAnalysis.valid, "and a cancelled analysis");

    // Loud material: the result stays under full scale.
    auto loud = sound;
    for (auto& v : loud) v *= 3.2f;
    const auto l = run(loud, {}, false);
    float peak = 0.0f;
    for (const float v : l.result.channels[0]) peak = std::max(peak, std::abs(v));
    check(l.result.valid && peak <= 0.9886f, "a loud sound is turned down to stay under full scale: " + std::to_string(peak));
}
}

int main()
{
    testPassBy();
    testSiren();
    testRicochet();
    testRunUp();
    testVibrato();
    testVoiceBreak();
    testKeepLength();
    testCharacters();
    testSeveralSounds();
    testNothingToFlatten();
    testMapAndOptions();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
