// Pitch Flattener's core against sounds whose pitch is known: no host, no JUCE.
//
// Each case makes a sound whose pitch moves in a known way, flattens it, and measures the
// result's pitch with a meter that shares nothing with the trackers (the period, from the
// autocorrelation of short stretches).

#include "core/Flatten.h"
#include "core/HarmonicTracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
    std::printf("    -> %s line; travels %s before, %s after; length %.2f s -> %.2f s\n", r.analysis.pick == 0 ? "harmonic" : "ridge", st(before.span()).c_str(),
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
    std::printf("    -> %s line; travels %s before, %s after\n", r.analysis.pick == 0 ? "harmonic" : "ridge", st(before.span()).c_str(), st(after.span()).c_str());
    check(before.span() > 13.0, "the meter reads the wail: " + st(before.span()));
    check(after.span() < 0.4, "flattened, it travels under 0.4 st: " + st(after.span()));
    check(r.analysis.lines[1].usable, "the ridge line reads a sound above the harmonic tracker's reach");
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
    std::printf("    -> %s line; reads a fall of %s in the first 0.2 s\n", r.analysis.pick == 0 ? "harmonic" : "ridge", st(fall).c_str());
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
    std::printf("    -> %s line; travels %s before, %s after\n", r.analysis.pick == 0 ? "harmonic" : "ridge", st(before.span()).c_str(), st(after.span()).c_str());
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

void testNothingToFlatten()
{
    beginCase("noise and silence: nothing to flatten");
    const auto noise = make(2.0, [](double) { return 100.0; }, {}, 1.0);
    const auto r = run(noise);
    check(r.analysis.valid, "noise is analysed");
    for (const auto& line : r.analysis.lines)
        std::printf("    -> %s line in noise: %s, found in %.0f%%, unsteadiness %.3f st\n", line.tracker == Tracker::Harmonic ? "harmonic" : "ridge",
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
    check(std::abs(editAt(edited.edits, 0.975) - 0.5) < 1.0e-9 && editAt(edited.edits, 0.9) == 0.0 && editAt(edited.edits, 1.5) == 1.0, "corrections ease out over 50 ms");

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
    testNothingToFlatten();
    testMapAndOptions();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
