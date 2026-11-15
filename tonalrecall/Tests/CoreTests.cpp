// Tests of the analysis core and Stamp on synthetic material with known answers.
// No JUCE: links only the core sources and the stretcher library.

#include "core/Analysis.h"
#include "core/HindsightTracker.h"
#include "core/PeakLimiter.h"
#include "core/Stamper.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <vector>

using namespace tonalrecall;
using namespace tonalrecall::core;

namespace
{
int g_checks = 0, g_failed = 0;
constexpr double kRate = 48000.0;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

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

// Adds a note with 1/h harmonics; midi may move over time through pitchAt (semitone offset).
template <typename PitchFn>
void addNote(std::vector<float>& x, double midi, double amp, int harmonics, std::int64_t from, std::int64_t to, PitchFn&& offsetAt)
{
    std::vector<double> phase(static_cast<std::size_t>(harmonics), 0.0);
    to = std::min<std::int64_t>(to, static_cast<std::int64_t>(x.size()));
    for (auto i = from; i < to; ++i)
    {
        const double hz = midiToHz(midi + offsetAt(static_cast<double>(i - from) / kRate));
        // 10 ms fades so notes do not click.
        const double fade = std::min({ 1.0, static_cast<double>(i - from) / 480.0, static_cast<double>(to - 1 - i) / 480.0 });
        double s = 0.0;
        for (int h = 1; h <= harmonics; ++h)
        {
            if (hz * h > 0.45 * kRate)
                break;
            auto& p = phase[static_cast<std::size_t>(h - 1)];
            p += kTwoPi * hz * h / kRate;
            s += std::sin(p) / h;
        }
        x[static_cast<std::size_t>(i)] += static_cast<float>(amp * fade * s);
    }
}

void addNote(std::vector<float>& x, double midi, double amp, int harmonics, std::int64_t from, std::int64_t to)
{
    addNote(x, midi, amp, harmonics, from, to, [](double) { return 0.0; });
}

void addNoise(std::vector<float>& x, double amp, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& v : x)
        v += static_cast<float>(amp) * dist(rng);
}

std::vector<float> seconds(double s) { return std::vector<float>(static_cast<std::size_t>(s * kRate), 0.0f); }

bool hasNote(const MusicAnalysis& m, int pitchClass)
{
    return std::any_of(m.notes.begin(), m.notes.end(), [&](const auto& n) { return n.pitchClass == pitchClass; });
}

void describe(const MusicAnalysis& m)
{
    std::printf("    -> %s, confidence %.2f, tuning %+.1f c, root %s, notes:", m.keyName().c_str(), static_cast<double>(m.keyConfidence),
                static_cast<double>(m.tuningCents), noteName(m.rootMidi).c_str());
    for (const auto& n : m.notes)
        std::printf(" %s(%.2f)", pitchClassName(n.pitchClass), static_cast<double>(n.strength));
    std::printf("\n");
}

std::vector<float> dMinorPad(double detuneSemitones = 0.0)
{
    auto x = seconds(4.0);
    const auto n = static_cast<std::int64_t>(x.size());
    for (double midi : { 38.0, 45.0, 50.0, 53.0, 57.0 })   // D2 A2 D3 F3 A3
        addNote(x, midi + detuneSemitones, 0.12, 6, 0, n);
    addNoise(x, 0.003, 1);
    return x;
}

// The pitch of a stretch of audio, as the median of the tracker's frames (MIDI).
double measuredMidi(const std::vector<float>& x, std::size_t from, std::size_t to, double* spreadOut = nullptr)
{
    hindsight::Settings settings;
    settings.method = hindsight::Method::Tonal;
    const auto a = hindsight::analyse(x.data() + from, static_cast<int>(to - from), kRate, settings);
    std::vector<double> midi;
    for (const auto& f : a.line.frames)
        if (f.hz > 0.0f && ! f.bridged)
            midi.push_back(hzToMidi(f.hz));
    if (midi.empty())
        return 0.0;
    std::sort(midi.begin(), midi.end());
    if (spreadOut != nullptr)
        *spreadOut = midi[static_cast<std::size_t>(0.95 * static_cast<double>(midi.size() - 1))] - midi[static_cast<std::size_t>(0.05 * static_cast<double>(midi.size() - 1))];
    return midi[midi.size() / 2];
}

// Runs a mono signal through a stereo Stamper in blocks and returns the left output with the
// latency removed (so output sample i corresponds to input sample i).
std::vector<float> stamp(Stamper& stamper, const std::vector<float>& in, int block = 1024)
{
    const int latency = stamper.latencySamples();
    std::vector<float> l(in), r(in);
    l.resize(in.size() + static_cast<std::size_t>(latency), 0.0f);
    r.resize(l.size(), 0.0f);
    for (std::size_t at = 0; at < l.size(); at += static_cast<std::size_t>(block))
    {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), l.size() - at));
        float* ch[2] { l.data() + at, r.data() + at };
        stamper.process(ch, n);
    }
    return std::vector<float>(l.begin() + latency, l.end());
}

// How far a hit moved, in ms: the lag that best lines up the high-frequency energy envelopes of
// input and output around the hit (+-20 ms searched, +-25 ms compared). Robust against the tonal
// bed and against the hit changing shape.
double hitLagMs(const std::vector<float>& in, const std::vector<float>& out, std::size_t centre)
{
    const auto envelope = [](const std::vector<float>& s, std::size_t from, std::size_t count)
    {
        std::vector<double> e(count, 0.0);
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::size_t at = from + i;
            const double d = static_cast<double>(s[at]) - 2.0 * s[at - 1] + s[at - 2];   // second difference: a steep high-pass
            e[i] = d * d;
        }
        // 0.5 ms smoothing.
        std::vector<double> smooth(count, 0.0);
        const int r = 12;
        double acc = 0.0;
        for (std::size_t i = 0; i < count; ++i)
        {
            acc += e[i];
            if (i >= static_cast<std::size_t>(2 * r + 1))
                acc -= e[i - static_cast<std::size_t>(2 * r + 1)];
            if (i >= static_cast<std::size_t>(r))
                smooth[i - static_cast<std::size_t>(r)] = acc;
        }
        return smooth;
    };
    const std::size_t half = 2160, search = 960;   // 45 ms, 20 ms
    const auto a = envelope(in, centre - half, 2 * half);
    const auto b = envelope(out, centre - half, 2 * half);
    double best = -1.0;
    int bestLag = 0;
    for (int lag = -static_cast<int>(search); lag <= static_cast<int>(search); ++lag)
    {
        double sum = 0.0;
        for (std::size_t i = search; i + search < 2 * half; ++i)
            sum += a[i] * b[static_cast<std::size_t>(static_cast<int>(i) + lag)];
        if (sum > best)
        {
            best = sum;
            bestLag = lag;
        }
    }
    return 1000.0 * bestLag / kRate;
}

//==============================================================================
void testMusic()
{
    beginCase("music: a D minor pad");
    {
        const auto x = dMinorPad();
        const auto m = analyseMusic(x.data(), static_cast<int>(x.size()), kRate);
        describe(m);
        check(m.valid, "valid");
        check(m.rootPitchClass == 2, "root is D");
        check(m.minor, "minor");
        check(std::abs(m.tuningCents) < 5.0f, "tuning near 0");
        check(hasNote(m, 2) && hasNote(m, 9) && hasNote(m, 5), "D, A and F are offered");
        check(std::abs(m.rootMidi - std::round(m.rootMidi)) < 0.06 && static_cast<int>(std::lround(m.rootMidi)) % 12 == 2, "root pitch on D");
        check(m.laneFrames > 0 && static_cast<int>(m.lane.size()) == m.laneFrames * m.lanePitches, "lane size");
    }

    beginCase("music: the same pad 30 cents sharp");
    {
        const auto x = dMinorPad(0.30);
        const auto m = analyseMusic(x.data(), static_cast<int>(x.size()), kRate);
        describe(m);
        check(m.rootPitchClass == 2 && m.minor, "still D minor");
        check(std::abs(m.tuningCents - 30.0f) < 5.0f, "tuning near +30 cents");
        check(std::abs(m.pitchNear(2, 50.0) - 50.30) < 0.06, "the D it offers is the music's D, not the piano's");
    }

    beginCase("music: a single low drone (A1)");
    {
        auto x = seconds(3.0);
        addNote(x, 33.0, 0.3, 12, 0, static_cast<std::int64_t>(x.size()));
        addNoise(x, 0.003, 2);
        const auto m = analyseMusic(x.data(), static_cast<int>(x.size()), kRate);
        describe(m);
        check(m.valid && m.rootPitchClass == 9, "root is A");
        check(static_cast<int>(std::lround(m.rootMidi)) == 33, "in the right octave: " + noteName(m.rootMidi));
    }

    beginCase("music: a braam (E power chord, bright)");
    {
        auto x = seconds(3.0);
        const auto n = static_cast<std::int64_t>(x.size());
        addNote(x, 28.0, 0.25, 20, 0, n);   // E1
        addNote(x, 35.0, 0.20, 20, 0, n);   // B1
        addNote(x, 40.0, 0.20, 20, 0, n);   // E2
        addNoise(x, 0.01, 3);
        const auto m = analyseMusic(x.data(), static_cast<int>(x.size()), kRate);
        describe(m);
        check(m.rootPitchClass == 4, "root is E");
        check(hasNote(m, 11), "the fifth (B) is offered");
    }

    beginCase("music: a progression in D minor (Dm, Bb, C, Dm)");
    {
        auto x = seconds(6.0);
        const auto bar = static_cast<std::int64_t>(1.5 * kRate);
        const double chords[4][4] { { 38, 50, 53, 57 }, { 34, 50, 53, 58 }, { 36, 48, 52, 55 }, { 38, 50, 53, 57 } };
        for (int c = 0; c < 4; ++c)
            for (double midi : chords[c])
                addNote(x, midi, 0.12, 6, c * bar, (c + 1) * bar);
        addNoise(x, 0.003, 4);
        const auto m = analyseMusic(x.data(), static_cast<int>(x.size()), kRate);
        describe(m);
        check(m.rootPitchClass == 2 && m.minor, "D minor");
    }

    beginCase("music: a riser (the root glides up a semitone)");
    {
        auto x = seconds(4.0);
        const auto n = static_cast<std::int64_t>(x.size());
        addNote(x, 38.0, 0.25, 8, 0, n, [](double t) { return t / 4.0; });
        const auto m = analyseMusic(x.data(), static_cast<int>(x.size()), kRate);
        const float first = m.rootDriftCents[m.rootDriftCents.size() / 8];
        const float last = m.rootDriftCents[m.rootDriftCents.size() * 7 / 8];
        std::printf("    -> root drift %.0f c near the start, %.0f c near the end\n", static_cast<double>(first), static_cast<double>(last));
        check(last - first > 55.0f && last - first < 95.0f, "drift rises by about three quarters of the glide between those points");
    }

    beginCase("music: silence and very short buffers are refused");
    {
        const auto x = seconds(1.0);
        check(! analyseMusic(x.data(), static_cast<int>(x.size()), kRate).valid, "silence");
        check(! analyseMusic(x.data(), 100, kRate).valid, "too short");
    }
}

void testSfxAndMatch()
{
    const auto music = dMinorPad();
    const auto m = analyseMusic(music.data(), static_cast<int>(music.size()), kRate);

    beginCase("effect: a tone on B2 +38 cents with vibrato, matched to the root");
    {
        auto x = seconds(2.0);
        addNote(x, 47.38, 0.3, 8, 0, static_cast<std::int64_t>(x.size()), [](double t) { return 0.15 * std::sin(kTwoPi * 5.0 * t); });
        const auto s = analyseSfx(x.data(), static_cast<int>(x.size()), kRate);
        std::printf("    -> centre %.2f (%s), drift %.2f st, tonalness %.2f\n", s.centreMidi, noteName(s.centreMidi).c_str(),
                    static_cast<double>(s.driftSemitones), static_cast<double>(s.tonalness));
        check(s.valid && s.pitched, "pitched");
        check(std::abs(s.centreMidi - 47.38) < 0.06, "centre within 6 cents");

        const auto r = match(m, s, {});
        std::printf("    -> shift %+.2f st to %s, speed %.1f%%\n", r.semitones, noteName(r.targetMidi).c_str(), r.speedRatio * 100.0);
        check(r.valid && r.targetPitchClass == 2, "target D");
        check(std::abs(r.semitones - 2.62) < 0.08, "shift about +2.62 semitones");
        check(std::abs(r.speedRatio - std::exp2(r.semitones / 12.0)) < 1.0e-9, "speed ratio");

        MatchOptions fifth;
        fifth.target = Target::Fifth;
        const auto rf = match(m, s, fifth);
        check(rf.targetPitchClass == 9 && std::abs(rf.semitones + 2.38) < 0.08, "fifth: down to A2");

        MatchOptions nearest;
        nearest.target = Target::Nearest;
        const auto rn = match(m, s, nearest);
        check(std::abs(rn.semitones) <= std::abs(r.semitones) + 1.0e-6 && std::abs(rn.semitones) <= std::abs(rf.semitones) + 1.0e-6, "nearest is no further than root or fifth");

        MatchOptions up;
        up.octaveShift = 1;
        check(std::abs(match(m, s, up).semitones - (r.semitones + 12.0)) < 1.0e-6, "octave up adds 12");
        check(std::abs(r.semitones) <= 6.0 + 1.0e-6, "the plain match is the smallest move");
    }

    beginCase("effect: a 3 semitone glide, flattened");
    {
        auto x = seconds(2.0);
        addNote(x, 45.0, 0.3, 8, 0, static_cast<std::int64_t>(x.size()), [](double t) { return 1.5 * t; });
        const auto s = analyseSfx(x.data(), static_cast<int>(x.size()), kRate);
        check(s.pitched && std::abs(s.driftSemitones - 2.7f) < 0.4f, "drift about 2.7 st (5th..95th percentile of 3)");

        MatchOptions flat;
        flat.bend = -1.0;
        const auto r = match(m, s, flat);
        float lo = 1.0e9f, hi = -1.0e9f;
        for (std::size_t i = 10; i + 10 < r.afterHz.size(); ++i)
            if (r.afterHz[i] > 0.0f)
            {
                const auto midi = static_cast<float>(hzToMidi(r.afterHz[i]));
                lo = std::min(lo, midi);
                hi = std::max(hi, midi);
            }
        std::printf("    -> after: %.2f .. %.2f (target %.2f)\n", static_cast<double>(lo), static_cast<double>(hi), r.targetMidi);
        check(hi - lo < 0.12f, "flat to within 12 cents");
        check(std::abs(0.5 * (hi + lo) - r.targetMidi) < 0.08, "and on the target");

        MatchOptions half;
        half.bend = -0.5;
        const auto rh = match(m, s, half);
        const float span = rh.curve.front() - rh.curve.back();
        check(std::abs(span - 1.5f) < 0.3f, "half flatten takes out half the glide");
    }

    beginCase("effect: noise with a ring at 400 Hz (no pitch to track)");
    {
        auto x = seconds(1.5);
        // White noise through a two-pole resonator.
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        const double w = kTwoPi * 400.0 / kRate, rad = 0.995;
        double y1 = 0.0, y2 = 0.0;
        for (auto& v : x)
        {
            const double in = dist(rng);
            const double y = in + 2.0 * rad * std::cos(w) * y1 - rad * rad * y2;
            y2 = y1;
            y1 = y;
            v = static_cast<float>(0.002 * y + 0.02 * in);
        }
        const auto s = analyseSfx(x.data(), static_cast<int>(x.size()), kRate, SfxMethod::Resonance);
        std::printf("    -> %.1f Hz, tonalness %.2f\n", s.pitched ? midiToHz(s.centreMidi) : 0.0, static_cast<double>(s.tonalness));
        check(s.valid && s.usedMethod == SfxMethod::Resonance && s.pitched, "resonance found");
        check(std::abs(midiToHz(s.centreMidi) - 400.0) < 6.0, "at 400 Hz");
        check(match(m, s, {}).valid, "and it can be matched");
    }

    beginCase("effect: plain white noise has nothing to match");
    {
        auto x = seconds(1.0);
        addNoise(x, 0.2, 9);
        const auto s = analyseSfx(x.data(), static_cast<int>(x.size()), kRate);
        std::printf("    -> method %d, pitched %d, tonalness %.2f\n", static_cast<int>(s.usedMethod), s.pitched ? 1 : 0, static_cast<double>(s.tonalness));
        check(s.valid && (! s.pitched || s.tonalness < 0.2f), "not confidently pitched");
    }

    beginCase("follow: the effect takes on the music's rise");
    {
        auto riser = seconds(4.0);
        addNote(riser, 38.0, 0.25, 8, 0, static_cast<std::int64_t>(riser.size()), [](double t) { return t / 4.0; });
        const auto mr = analyseMusic(riser.data(), static_cast<int>(riser.size()), kRate);
        auto x = seconds(4.0);
        addNote(x, 47.0, 0.3, 8, 0, static_cast<std::int64_t>(x.size()));
        const auto s = analyseSfx(x.data(), static_cast<int>(x.size()), kRate);
        MatchOptions follow;
        follow.bend = 1.0;
        follow.offsetKnown = true;
        const auto r = match(mr, s, follow);
        const float rise = r.curve[r.curve.size() * 7 / 8] - r.curve[r.curve.size() / 8];
        std::printf("    -> curve rises %.2f st between 1/8 and 7/8\n", static_cast<double>(rise));
        check(rise > 0.55f && rise < 0.95f, "about three quarters of a semitone");
    }
}

void testTailAndEdits()
{
    // Music: 4 s whose root rises a semitone. Effect: 8 s, starting with the music.
    auto riser = seconds(4.0);
    addNote(riser, 38.0, 0.25, 8, 0, static_cast<std::int64_t>(riser.size()), [](double t) { return t / 4.0; });
    const auto m = analyseMusic(riser.data(), static_cast<int>(riser.size()), kRate);
    auto x = seconds(8.0);
    addNote(x, 47.0, 0.3, 8, 0, static_cast<std::int64_t>(x.size()));
    const auto s = analyseSfx(x.data(), static_cast<int>(x.size()), kRate);

    beginCase("tail: an effect longer than the music is marked, and a follow bend eases on past the music");
    {
        MatchOptions follow;
        follow.bend = 1.0;
        follow.offsetKnown = true;
        const auto r = match(m, s, follow);
        const auto n = static_cast<int>(r.curve.size());
        std::printf("    -> covered frames %d..%d of %d\n", r.coveredFrom, r.coveredTo, n);
        check(r.coveredFrom == 0 && std::abs(r.coveredTo - n / 2) <= 2, "the first half is covered");
        const float atEnd = r.curve[static_cast<std::size_t>(r.coveredTo - 20)];
        const float later = r.curve[static_cast<std::size_t>(r.coveredTo + 200)];    // 1 s past the music
        const float final = r.curve.back();
        const float almostFinal = r.curve[r.curve.size() - 200];
        std::printf("    -> shift at the music's end %.2f, 1 s later %.2f, at the effect's end %.2f\n",
                    static_cast<double>(atEnd), static_cast<double>(later), static_cast<double>(final));
        check(later > atEnd + 0.05f, "the rise carries on past the music");
        check(final - atEnd < 0.45f, "but does not run away (under a 1.5 s worth of the music's pace)");
        check(std::abs(final - almostFinal) < 0.04f, "and has all but settled by the end");
        float worstStep = 0.0f;
        for (std::size_t i = 1; i < r.curve.size(); ++i)
            worstStep = std::max(worstStep, std::abs(r.curve[i] - r.curve[i - 1]));
        check(worstStep < 0.02f, "with no jump anywhere");

        MatchOptions late = follow;
        late.sfxOffsetInMusicSeconds = -2.0;   // the effect starts 2 s before the music
        const auto rl = match(m, s, late);
        check(std::abs(rl.coveredFrom - 400) <= 2 && std::abs(rl.coveredTo - 1200) <= 2, "an effect that starts before the music: covered in the middle");
    }

    beginCase("edits: hand adjustments add to the shift");
    {
        MatchOptions plain;
        const auto base = match(m, s, plain);
        MatchOptions edited;
        edited.edits = { { 6.0, -1.0 } };
        const auto r = match(m, s, edited);
        const auto at = [&](const MatchResult& res, double sec) { return res.curve[static_cast<std::size_t>(sec / res.hopSeconds)]; };
        check(std::abs(at(r, 6.0) - at(base, 6.0) + 1.0f) < 0.03f, "a full semitone down at the point");
        check(std::abs(at(r, 3.0) - at(base, 3.0) + 0.5f) < 0.03f, "half way there half way along");
        check(std::abs(at(r, 0.0) - at(base, 0.0)) < 0.03f && std::abs(r.curve.back() - base.curve.back()) < 0.03f, "nothing at the two ends");
        check(std::abs(core::midiToHz(r.afterMidi[1200]) - core::midiToHz(base.afterMidi[1200] - 1.0f)) < 0.5, "the after-pitch shows it");

        check(std::abs(editOffsetAt({ { 6.0, -1.0 } }, 3.0, 8.0) + 0.5) < 1.0e-9 && std::abs(editOffsetAt({ { 6.0, -1.0 } }, 7.0, 8.0) + 0.5) < 1.0e-9,
              "the adjustment at a moment can be asked for directly");
        check(std::abs(editOffsetAt({ { 7.0, 1.0 }, { 2.0, -1.0 } }, 4.5, 8.0)) < 1.0e-9, "whatever order the points are in");

        // Snapping to the music (D minor here, in tune): notes, and notes of the scale.
        const auto pad = dMinorPad();
        const auto dm = analyseMusic(pad.data(), static_cast<int>(pad.size()), kRate);
        const double tuning = dm.tuningCents / 100.0;
        check(std::abs(snapToMusic(dm, 51.4, false) - (51.0 + tuning)) < 1.0e-6, "51.4 snaps to the semitone 51");
        check(std::abs(snapToMusic(dm, 51.4, true) - (52.0 + tuning)) < 1.0e-6, "but to E (52) in D minor: E flat is not in the scale");
        check(std::abs(snapToMusic(dm, 50.2, true) - (50.0 + tuning)) < 1.0e-6, "a scale note stays");

        edited.edits = { { 4.0, 0.0 }, { 8.0, 2.0 } };   // bend only the tail, up to the very end
        const auto tail = match(m, s, edited);
        check(std::abs(at(tail, 2.0) - at(base, 2.0)) < 0.01f, "before the first point: untouched");
        check(std::abs(tail.curve.back() - base.curve.back() - 2.0f) < 0.06f, "a point at the end holds its value there");
    }
}

void testFollow()
{
    // Music: three notes in a row, E4 for 0.8 s, C4 for 0.7 s, A3 to the end (as brass would play
    // them: each with its octave doubled). Effect: a steady tone on B3, starting with the music.
    auto line = seconds(3.0);
    const auto at = [](double s) { return static_cast<std::int64_t>(s * kRate); };
    for (double octave : { 0.0, 12.0 })
    {
        addNote(line, 64.0 + octave, 0.15, 6, at(0.0), at(0.8));
        addNote(line, 60.0 + octave, 0.15, 6, at(0.8), at(1.5));
        addNote(line, 57.0 + octave, 0.15, 6, at(1.5), at(3.0));
    }
    addNoise(line, 0.002, 21);
    const auto m = analyseMusic(line.data(), static_cast<int>(line.size()), kRate);

    beginCase("follow: the music's lead line is found, note by note, in time");
    {
        std::printf("    -> lead line:");
        for (const auto& note : m.leadNotes)
            std::printf(" %s at %.2f s,", noteName(note.midi).c_str(), note.startSeconds);
        std::printf("\n");
        check(m.leadNotes.size() == 3, "three notes");
        if (m.leadNotes.size() == 3)
        {
            const auto pc = [](double midi) { return (static_cast<int>(std::lround(midi)) % 12 + 12) % 12; };
            check(pc(m.leadNotes[0].midi) == 4 && pc(m.leadNotes[1].midi) == 0 && pc(m.leadNotes[2].midi) == 9, "E, then C, then A");
            check(std::abs(m.leadNotes[1].startSeconds - 0.8) < 0.06 && std::abs(m.leadNotes[2].startSeconds - 1.5) < 0.06,
                  "each within 60 ms of when it starts");
        }
    }

    beginCase("follow: a held chord is not a melody (its notes trade places in loudness without stopping)");
    {
        auto chord = seconds(3.0);
        const auto n = static_cast<std::int64_t>(chord.size());
        const double notes[] { 50.0, 57.0, 65.0 };
        for (int k = 0; k < 3; ++k)
            for (std::int64_t i = 0; i < n; ++i)
            {
                // Each note swells and fades (by 4.4 dB, peak to trough) at its own rate, never stopping.
                const double swell = 1.0 + 0.25 * std::sin(kTwoPi * (0.7 + 0.45 * k) * static_cast<double>(i) / kRate + k);
                double s = 0.0;
                for (int hmn = 1; hmn <= 6; ++hmn)
                    s += std::sin(kTwoPi * midiToHz(notes[k]) * hmn * static_cast<double>(i) / kRate) / hmn;
                chord[static_cast<std::size_t>(i)] += static_cast<float>(0.1 * swell * s);
            }
        const auto held = analyseMusic(chord.data(), static_cast<int>(chord.size()), kRate);
        std::printf("    -> %zu lead note(s)\n", held.leadNotes.size());
        check(held.leadNotes.size() <= 1, "no line to follow");
    }

    auto x = seconds(2.4);
    addNote(x, 59.35, 0.3, 8, 0, static_cast<std::int64_t>(x.size()));
    const auto s = analyseSfx(x.data(), static_cast<int>(x.size()), kRate);

    beginCase("follow: the effect steps through the notes with the music, and Stamp renders the steps");
    {
        MatchOptions follow;
        follow.target = Target::Follow;
        follow.offsetKnown = true;
        const auto r = match(m, s, follow);
        const auto shiftAt = [&](double sec) { return r.curve[static_cast<std::size_t>(sec / r.hopSeconds)]; };
        std::printf("    -> follows %d notes; shift %+.2f at 0.4 s, %+.2f at 1.15 s, %+.2f at 2.0 s\n", r.notesFollowed,
                    static_cast<double>(shiftAt(0.4)), static_cast<double>(shiftAt(1.15)), static_cast<double>(shiftAt(2.0)));
        check(r.valid && r.notesFollowed == 3, "follows three notes");
        // From B3 +35 c: E4 is +4.65, C4 +0.65, A3 -2.35.
        check(std::abs(shiftAt(0.4) - 4.65f) < 0.1f && std::abs(shiftAt(1.15) - 0.65f) < 0.1f && std::abs(shiftAt(2.0) + 2.35f) < 0.1f,
              "onto E4, then C4, then A3");
        check(std::abs(r.semitones - 4.65) < 0.1, "the headline shift is the first note's");

        Stamper::Setup setup;
        setup.sampleRate = kRate;
        setup.staticSemitones = r.semitones;
        setup.curveHopSeconds = r.hopSeconds;
        setup.curve = r.curve;
        auto both = std::make_shared<std::vector<float>>(x.size());
        for (std::size_t i = 0; i < x.size(); ++i)
            (*both)[i] = x[i] + x[i];
        setup.learned = both;
        Stamper stamper(setup);
        const auto y = stamp(stamper, x);
        const double first = measuredMidi(y, 4800, 33600), second = measuredMidi(y, 43200, 67200), third = measuredMidi(y, 76800, 110400);
        std::printf("    -> stamped: %.2f, then %.2f, then %.2f\n", first, second, third);
        check(std::abs(first - 64.0) < 0.12 && std::abs(second - 60.0) < 0.12 && std::abs(third - 57.0) < 0.12, "the rendered effect plays E4, C4, A3");

        MatchOptions late = follow;
        late.sfxOffsetInMusicSeconds = 1.0;   // the effect starts a second into the music
        const auto rl = match(m, s, late);
        check(rl.notesFollowed == 2 && std::abs(rl.curve[40] - 0.65f) < 0.1f && std::abs(rl.curve[200] + 2.35f) < 0.1f,
              "an effect that starts later picks the line up where the music is");

        MatchOptions nearest;
        nearest.target = Target::Nearest;
        check(match(m, s, nearest).notesFollowed == 0, "Nearest does not follow");

        // Passing notes are passed over: with a second between "notes", only E and A are long
        // enough here (C lasts 0.7 s), and the E carries on until the A.
        MatchOptions resting = follow;
        resting.shortestNoteSeconds = 0.75;
        const auto rs = match(m, s, resting);
        check(rs.notesFollowed == 2 && std::abs(rs.curve[static_cast<std::size_t>(1.15 / rs.hopSeconds)] - 4.65f) < 0.1f
                  && std::abs(rs.curve[static_cast<std::size_t>(2.0 / rs.hopSeconds)] + 2.35f) < 0.1f,
              "a short note is passed over and the note before carries on");

        // Glide: the steps open into ramps. At full glide the line runs straight from the middle
        // of one note to the middle of the next.
        MatchOptions ramp = follow;
        ramp.glide = 1.0;
        const auto rr = match(m, s, ramp);
        const auto rampAt = [&](double sec) { return rr.curve[static_cast<std::size_t>(sec / rr.hopSeconds)]; };
        // E4 is 0..0.8 s (middle 0.4), C4 0.8..1.5 (middle 1.15): the change at 0.8 is half way.
        std::printf("    -> full glide: shift %+.2f at 0.4 s, %+.2f at the change (0.8 s), %+.2f at 1.15 s\n",
                    static_cast<double>(rampAt(0.42)), static_cast<double>(rampAt(0.8)), static_cast<double>(rampAt(1.15)));
        check(std::abs(rampAt(0.8) - 2.65f) < 0.25f, "half way between the two notes at the change");
        check(rampAt(0.6) < rampAt(0.5) && rampAt(0.7) < rampAt(0.6) && rampAt(1.0) < rampAt(0.9), "a steady slide, not a step");
        float worst = 0.0f;
        for (std::size_t i = 1; i < rr.curve.size(); ++i)
            worst = std::max(worst, std::abs(rr.curve[i] - rr.curve[i - 1]));
        check(worst < 0.1f, "no jump anywhere along the ramp");

        ramp.glide = 0.4;
        const auto rg = match(m, s, ramp);
        check(std::abs(rg.curve[static_cast<std::size_t>(0.4 / rg.hopSeconds)] - 4.65f) < 0.15f
                  && std::abs(rg.curve[static_cast<std::size_t>(1.15 / rg.hopSeconds)] - 0.65f) < 0.15f,
              "a partial glide still holds each note in its middle");
    }
}

void testMovement()
{
    auto x = seconds(3.0);
    addNote(x, 59.35, 0.3, 8, 0, static_cast<std::int64_t>(x.size()));
    const auto s = analyseSfx(x.data(), static_cast<int>(x.size()), kRate);

    beginCase("movement: a voice that scoops up into its note; the effect leans the same way, gently");
    {
        // A sung note: starts two semitones under and slides up over 0.5 s, then holds.
        auto voice = seconds(3.0);
        addNote(voice, 64.0, 0.25, 8, 0, static_cast<std::int64_t>(voice.size()), [](double t) { return t < 0.5 ? -2.0 * (1.0 - t / 0.5) : 0.0; });
        const auto m = analyseMusic(voice.data(), static_cast<int>(voice.size()), kRate);
        check(m.contourMidi.size() > 20, "the voice's movement was followed");
        const auto contourAt = [&](double sec) { return m.contourMidi[static_cast<std::size_t>((sec - m.contourStartSeconds) / m.contourHopSeconds)]; };
        std::printf("    -> voice at 0.1 s %.2f, at 0.3 s %.2f, at 1.5 s %.2f\n", static_cast<double>(contourAt(0.1)),
                    static_cast<double>(contourAt(0.3)), static_cast<double>(contourAt(1.5)));
        check(std::abs(contourAt(0.3) - 63.2f) < 0.35f && std::abs(contourAt(1.5) - 64.0f) < 0.1f, "the slide is seen as a slide, not as two notes");

        MatchOptions hold, some, full;
        hold.offsetKnown = some.offsetKnown = full.offsetKnown = true;
        some.bend = 0.35;
        full.bend = 1.0;
        const auto rh = match(m, s, hold), rs = match(m, s, some), rf = match(m, s, full);
        const auto rise = [](const MatchResult& r) { return r.curve[static_cast<std::size_t>(1.5 / r.hopSeconds)] - r.curve[static_cast<std::size_t>(0.1 / r.hopSeconds)]; };
        std::printf("    -> the effect rises by %.2f st held, %.2f at 35%%, %.2f in full\n", static_cast<double>(rise(rh)),
                    static_cast<double>(rise(rs)), static_cast<double>(rise(rf)));
        check(std::abs(rise(rh)) < 0.02f, "held: no movement");
        check(rise(rs) > 0.3f && rise(rs) < 0.8f, "at 35%: a subtle lean up into the note");
        check(rise(rf) > 1.3f && rise(rf) < 2.1f, "in full: most of the voice's slide");
        float worst = 0.0f;
        for (std::size_t i = 1; i < rf.curve.size(); ++i)
            worst = std::max(worst, std::abs(rf.curve[i] - rf.curve[i - 1]));
        check(worst < 0.08f, "as a curve, with no jump");
    }

    beginCase("movement: short detached notes that climb overall; following them, the effect climbs as one glide");
    {
        // Eight staccato notes, 0.15 s on and 0.2 s off, each a semitone above the last.
        auto staccato = seconds(3.0);
        for (int k = 0; k < 8; ++k)
        {
            const auto from = static_cast<std::int64_t>((0.1 + 0.35 * k) * kRate);
            addNote(staccato, 60.0 + k, 0.25, 8, from, from + static_cast<std::int64_t>(0.15 * kRate));
        }
        addNoise(staccato, 0.002, 31);
        const auto m = analyseMusic(staccato.data(), static_cast<int>(staccato.size()), kRate);
        MatchOptions some;
        some.offsetKnown = true;
        some.target = Target::Follow;
        some.glide = 1.0;
        some.bend = 0.35;
        const auto r = match(m, s, some);
        const float climb = r.curve[static_cast<std::size_t>(2.7 / r.hopSeconds)] - r.curve[static_cast<std::size_t>(0.2 / r.hopSeconds)];
        float worst = 0.0f;
        for (std::size_t i = 1; i < r.curve.size(); ++i)
            worst = std::max(worst, std::abs(r.curve[i] - r.curve[i - 1]));
        std::printf("    -> the effect rests on %d notes and climbs %.2f st over the passage; largest step between frames %.3f st\n",
                    r.notesFollowed, static_cast<double>(climb), static_cast<double>(worst));
        check(climb > 1.5f && climb < 7.5f, "it climbs with the notes");
        check(worst < 0.1f, "as a glide, not as steps");

        MatchOptions lockedOptions;
        lockedOptions.target = Target::Nearest;
        const auto locked = match(m, s, lockedOptions);
        const auto [lo, hi] = std::minmax_element(locked.curve.begin(), locked.curve.end());
        check(*hi - *lo < 1.0e-4f, "locked to one note, it does not move at all");
    }

    beginCase("movement never pulls the effect off the music's notes: while the music holds a note, so does the effect");
    {
        // A voice holds D4 for 1.5 s, then G4, over a low D drone. The effect is B2 +30 c.
        auto music = seconds(3.0);
        const auto n = static_cast<std::int64_t>(music.size());
        addNote(music, 38.0, 0.2, 8, 0, n);
        addNote(music, 62.0, 0.2, 6, 0, n / 2);
        addNote(music, 67.0, 0.2, 6, n / 2, n);
        const auto m = analyseMusic(music.data(), static_cast<int>(music.size()), kRate);
        auto effect = seconds(3.0);
        addNote(effect, 47.3, 0.3, 8, 0, static_cast<std::int64_t>(effect.size()));
        const auto e = analyseSfx(effect.data(), static_cast<int>(effect.size()), kRate);

        const auto offNote = [&](const MatchResult& r, double sec)
        {
            // How far the effect's pitch is from the nearest note of the music's tuning, in cents.
            const double pitch = r.afterMidi[static_cast<std::size_t>(sec / r.hopSeconds)] - static_cast<double>(m.tuningCents) / 100.0;
            return 100.0 * std::abs(pitch - std::round(pitch));
        };
        for (double bend : { 0.35, 0.8 })
        {
            MatchOptions style;
            style.offsetKnown = true;
            style.target = Target::Follow;
            style.glide = 1.0;
            style.bend = bend;
            const auto r = match(m, e, style);
            std::printf("    -> bend %.0f%%: %.1f c off a note at 0.75 s, %.1f c at 2.5 s (pitch %.2f, then %.2f)\n", bend * 100.0, offNote(r, 0.75),
                        offNote(r, 2.5), static_cast<double>(r.afterMidi[150]), static_cast<double>(r.afterMidi[500]));
            check(offNote(r, 0.75) < 8.0 && offNote(r, 2.5) < 8.0, "on a note of the music while the music holds one");
        }
    }

    beginCase("following: a semitone step in the music is a semitone step for the effect, not a leap across the octave");
    {
        // F4, then F sharp. The effect sits near C4, almost a tritone from both.
        auto music = seconds(3.0);
        const auto n = static_cast<std::int64_t>(music.size());
        addNote(music, 65.0, 0.25, 6, 0, n / 2);
        addNote(music, 66.0, 0.25, 6, n / 2, n);
        const auto m = analyseMusic(music.data(), static_cast<int>(music.size()), kRate);
        auto effect = seconds(3.0);
        addNote(effect, 59.8, 0.3, 8, 0, static_cast<std::int64_t>(effect.size()));
        const auto e = analyseSfx(effect.data(), static_cast<int>(effect.size()), kRate);
        MatchOptions follow;
        follow.offsetKnown = true;
        follow.target = Target::Follow;
        const auto r = match(m, e, follow);
        const float before = r.afterMidi[static_cast<std::size_t>(0.75 / r.hopSeconds)], after = r.afterMidi[static_cast<std::size_t>(2.5 / r.hopSeconds)];
        std::printf("    -> the effect goes from %.2f to %.2f\n", static_cast<double>(before), static_cast<double>(after));
        check(r.notesFollowed == 2 && std::abs(std::abs(after - before) - 1.0f) < 0.1f, "one semitone");
    }

    beginCase("hand edits come out as drawn, whatever Smooth is set to");
    {
        const auto music = dMinorPad();
        const auto m = analyseMusic(music.data(), static_cast<int>(music.size()), kRate);
        // A quick bump: nothing at 1.4 s, two semitones up at 1.5 s, nothing again at 1.6 s.
        for (double glide : { 0.0, 0.4, 1.0 })
        {
            MatchOptions plain, bumped;
            plain.glide = bumped.glide = glide;
            bumped.edits = { { 1.4, 0.0 }, { 1.5, 2.0 }, { 1.6, 0.0 } };
            const auto base = match(m, s, plain), r = match(m, s, bumped);
            const auto at = static_cast<std::size_t>(1.5 / r.hopSeconds);
            std::printf("    -> smooth %.0f%%: the bump reads %+.2f st at its peak\n", glide * 100.0, static_cast<double>(r.curve[at] - base.curve[at]));
            check(r.curve[at] - base.curve[at] > 1.7f, "the bump keeps its height");
        }
    }

    beginCase("a pitch line that slips an octave for a few frames only is put back, and stays put back");
    {
        for (int length : { 3, 5, 6, 7, 10, 40 })
        {
            std::vector<float> hz(200, 200.0f), level(200, 0.1f);
            level[20] = 0.2f;   // the loudest frame is well before the slip
            for (int i = 100; i < 100 + length; ++i)
                hz[static_cast<std::size_t>(i)] = 100.0f;
            detail::repairHarmonicSlips(hz, level);
            int wrong = 0;
            for (float v : hz)
                wrong += std::abs(v - 200.0f) > 2.0f ? 1 : 0;
            check(wrong <= 6, "a slip of " + std::to_string(length) + " frames: " + std::to_string(wrong) + " frames left off the line");
        }
    }
}

void testStamp()
{
    beginCase("stamp: a fixed shift keeps the length and lands on pitch");
    {
        auto x = seconds(2.0);
        addNote(x, 57.0, 0.3, 6, 0, static_cast<std::int64_t>(x.size()));   // A3
        Stamper::Setup setup;
        setup.sampleRate = kRate;
        setup.staticSemitones = 2.62;
        Stamper stamper(setup);
        const auto y = stamp(stamper, x);
        std::printf("    -> latency %d samples (%.1f ms)\n", stamper.latencySamples(), 1000.0 * stamper.latencySamples() / kRate);
        check(y.size() == x.size(), "same length");
        const double midi = measuredMidi(y, 9600, y.size() - 9600);
        std::printf("    -> output pitch %.3f (want %.3f)\n", midi, 57.0 + 2.62);
        check(std::abs(midi - (57.0 + 2.62)) < 0.05, "pitch within 5 cents");
        float peak = 0.0f;
        for (float v : y)
            peak = std::max(peak, std::abs(v));
        check(peak > 0.1f && peak < 1.0f, "level sane");
    }

    beginCase("stamp: hits stay where they were");
    {
        // A tonal bed with a sharp hit every 250 ms.
        auto x = seconds(3.0);
        addNote(x, 45.0, 0.1, 6, 0, static_cast<std::int64_t>(x.size()));
        std::vector<std::size_t> hits;
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        for (std::size_t at = 12000; at + 12000 < x.size(); at += 12000)
        {
            hits.push_back(at);
            for (std::size_t i = 0; i < 480; ++i)
                x[at + i] += 0.7f * dist(rng) * std::exp(-static_cast<float>(i) / 80.0f);
        }
        // On average hits must not move (well under a millisecond). Single hits scatter around
        // that: within 1 ms up to a tritone either way (the smallest move to a note is never
        // more), and the stretcher itself scatters them further by an octave.
        const struct { double semitones, meanMs, worstMs; } cases[] {
            { 3.0, 0.5, 1.0 }, { -4.0, 0.5, 1.0 }, { 6.0, 0.5, 1.0 }, { -7.0, 0.5, 1.0 }, { 7.0, 0.5, 2.0 },
            { 12.0, 0.75, 2.0 }, { -12.0, 1.0, 3.5 } };
        for (const auto& c : cases)
        {
            Stamper::Setup setup;
            setup.sampleRate = kRate;
            setup.staticSemitones = c.semitones;
            Stamper stamper(setup);
            const auto y = stamp(stamper, x);
            double worst = 0.0, sum = 0.0;
            for (auto at : hits)
            {
                const double drift = hitLagMs(x, y, at);
                worst = std::abs(drift) > std::abs(worst) ? drift : worst;
                sum += drift;
            }
            const double mean = sum / static_cast<double>(hits.size());
            std::printf("    -> %+.0f st: onset drift mean %+.2f ms, worst %+.2f ms over %zu hits\n", c.semitones, mean, worst, hits.size());
            check(std::abs(mean) < c.meanMs, "mean drift within " + std::to_string(c.meanMs) + " ms");
            check(std::abs(worst) < c.worstMs, "every hit within " + std::to_string(c.worstMs) + " ms");
        }
    }

    beginCase("stamp: hits stay where they were while the shift moves (+1 to +4 semitones over the clip)");
    {
        auto x = seconds(3.0);
        addNote(x, 45.0, 0.1, 6, 0, static_cast<std::int64_t>(x.size()));
        std::vector<std::size_t> hits;
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        for (std::size_t at = 12000; at + 12000 < x.size(); at += 12000)
        {
            hits.push_back(at);
            for (std::size_t i = 0; i < 480; ++i)
                x[at + i] += 0.7f * dist(rng) * std::exp(-static_cast<float>(i) / 80.0f);
        }
        Stamper::Setup setup;
        setup.sampleRate = kRate;
        setup.curve.resize(600);
        for (std::size_t i = 0; i < setup.curve.size(); ++i)
            setup.curve[i] = 1.0f + 3.0f * static_cast<float>(i) / 599.0f;
        auto both = std::make_shared<std::vector<float>>(x.size());
        for (std::size_t i = 0; i < x.size(); ++i)
            (*both)[i] = x[i] + x[i];
        setup.learned = both;
        Stamper stamper(setup);
        const auto y = stamp(stamper, x);
        check(stamper.alignmentFound() && stamper.alignmentOffset() == 0, "aligned at the start");
        double worst = 0.0, sum = 0.0;
        for (auto at : hits)
        {
            const double drift = hitLagMs(x, y, at);
            worst = std::abs(drift) > std::abs(worst) ? drift : worst;
            sum += drift;
        }
        std::printf("    -> onset drift mean %+.2f ms, worst %+.2f ms over %zu hits\n", sum / static_cast<double>(hits.size()), worst, hits.size());
        check(std::abs(sum / static_cast<double>(hits.size())) < 0.5 && std::abs(worst) < 1.0, "every hit within 1 ms");
        const double early = measuredMidi(y, 30000, 42000);
        const double late = measuredMidi(y, 102000, 114000);
        std::printf("    -> pitch %.2f at 0.75 s (want %.2f), %.2f at 2.25 s (want %.2f)\n", early, 45.0 + 1.75, late, 45.0 + 3.25);
        check(std::abs(early - 46.75) < 0.12 && std::abs(late - 48.25) < 0.12, "pitch follows the curve");
    }

    beginCase("stamp: finds the learned effect between the render's handles and flattens it there");
    {
        // The learned effect: a 3 semitone glide. The render: 2 s of other audio, the effect, 2 s more.
        auto learned = seconds(2.0);
        addNote(learned, 45.0, 0.3, 8, 0, static_cast<std::int64_t>(learned.size()), [](double t) { return 1.5 * t; });
        const auto music = dMinorPad();
        const auto m = analyseMusic(music.data(), static_cast<int>(music.size()), kRate);
        const auto s = analyseSfx(learned.data(), static_cast<int>(learned.size()), kRate);
        MatchOptions flat;
        flat.bend = -1.0;
        const auto r = match(m, s, flat);

        auto render = seconds(6.0);
        addNote(render, 52.0, 0.2, 6, 0, 96000);
        addNote(render, 40.0, 0.2, 6, 192000, 288000);
        for (std::size_t i = 0; i < learned.size(); ++i)
            render[96000 + i] += learned[i];

        Stamper::Setup setup;
        setup.sampleRate = kRate;
        setup.staticSemitones = r.semitones;
        setup.curveHopSeconds = r.hopSeconds;
        setup.curve = r.curve;
        // Learn captures the sum of the channels; stamp() feeds the same signal to both.
        auto both = std::make_shared<std::vector<float>>(learned.size());
        for (std::size_t i = 0; i < learned.size(); ++i)
            (*both)[i] = learned[i] + learned[i];
        setup.learned = both;
        Stamper stamper(setup);
        const auto y = stamp(stamper, render, 1024);
        check(stamper.alignmentFound(), "alignment found");
        check(stamper.alignmentOffset() == 96000, "at the right sample: " + std::to_string(stamper.alignmentOffset()));

        double spread = 0.0;
        const double midi = measuredMidi(y, 96000 + 4800, 192000 - 4800, &spread);
        std::printf("    -> learned region after stamp: %.2f, spread %.2f st (target %.2f)\n", midi, spread, r.targetMidi);
        check(std::abs(midi - r.targetMidi) < 0.1, "on the target");
        check(spread < 0.25, "flat (was 2.7 st)");
    }

    beginCase("stamp: a looping preview starts the bend again each time round");
    {
        auto glide = seconds(1.0);
        addNote(glide, 45.0, 0.3, 8, 0, static_cast<std::int64_t>(glide.size()), [](double t) { return 3.0 * t; });
        auto twice = glide;
        twice.insert(twice.end(), glide.begin(), glide.end());
        Stamper::Setup setup;
        setup.sampleRate = kRate;
        setup.curve.resize(200);
        for (std::size_t i = 0; i < setup.curve.size(); ++i)
            setup.curve[i] = -3.0f * static_cast<float>(i) / 199.0f;   // cancels the glide
        auto both = std::make_shared<std::vector<float>>(glide.size());
        for (std::size_t i = 0; i < glide.size(); ++i)
            (*both)[i] = glide[i] + glide[i];
        setup.learned = both;
        setup.loopLength = static_cast<std::int64_t>(glide.size());
        Stamper stamper(setup);
        const auto y = stamp(stamper, twice);
        double spread1 = 0.0, spread2 = 0.0;
        const double first = measuredMidi(y, 4800, 43200, &spread1);
        const double second = measuredMidi(y, 48000 + 4800, 48000 + 43200, &spread2);
        std::printf("    -> first time round %.2f (spread %.2f), second %.2f (spread %.2f)\n", first, spread1, second, spread2);
        check(std::abs(first - 45.0) < 0.15 && spread1 < 0.3, "first pass flat on the starting note");
        check(std::abs(second - 45.0) < 0.15 && spread2 < 0.3, "second pass too");
    }

    beginCase("stamp: a render that is not the learned effect gets the fixed shift");
    {
        auto x = seconds(2.0);
        addNote(x, 57.0, 0.3, 6, 0, static_cast<std::int64_t>(x.size()));
        auto other = seconds(1.0);
        addNote(other, 50.0, 0.3, 6, 0, static_cast<std::int64_t>(other.size()));
        Stamper::Setup setup;
        setup.sampleRate = kRate;
        setup.staticSemitones = -2.0;
        setup.curve.assign(200, 5.0f);   // would be audible if it were (wrongly) applied
        setup.learned = std::make_shared<std::vector<float>>(other);
        Stamper stamper(setup);
        const auto y = stamp(stamper, x);
        check(! stamper.alignmentFound(), "no alignment");
        check(std::abs(measuredMidi(y, 9600, y.size() - 9600) - 55.0) < 0.05, "fixed shift applied");
    }
}
}

void testSafety();
void testSafety()
{
    using core::PeakLimiter;
    const double sr = 48000.0;

    beginCase("safety: the limiter leaves quiet audio alone");
    {
        PeakLimiter limiter(sr, 2);
        const int n = 20000, delay = limiter.delaySamples();
        std::vector<float> left(n), right(n), l0, r0;
        for (int i = 0; i < n; ++i)
        {
            left[static_cast<std::size_t>(i)] = 0.6f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 220.0 * i / sr));
            right[static_cast<std::size_t>(i)] = 0.9f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 331.0 * i / sr));
        }
        l0 = left;
        r0 = right;
        float* ch[2] { left.data(), right.data() };
        for (int at = 0; at < n; at += 777)
        {
            float* part[2] { ch[0] + at, ch[1] + at };
            limiter.process(part, std::min(777, n - at));
        }
        const bool same = std::equal(left.begin() + delay, left.end(), l0.begin()) && std::equal(right.begin() + delay, right.end(), r0.begin());
        check(same, "below the ceiling the audio comes out bit for bit, " + std::to_string(delay) + " samples late");
        check(limiter.maxReductionDb() <= 0.0f, "and the limiter reports that it did nothing");
    }

    beginCase("safety: nothing gets past the ceiling");
    {
        PeakLimiter limiter(sr, 2);
        const int n = 96000;
        std::vector<float> left(n), right(n);
        std::uint32_t seed = 12345u;
        const auto noise = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<float>(static_cast<double>(seed >> 8) / 8388608.0 - 1.0); };
        for (int i = 0; i < n; ++i)
        {
            // Loud noise, a far louder burst, single-sample spikes, and a low tone well over full scale.
            const float burst = (i > 30000 && i < 31000) ? 8.0f : 1.0f;
            float v = 1.4f * burst * noise() + 1.6f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 45.0 * i / sr));
            if (i % 7919 == 0)
                v = 30.0f;
            left[static_cast<std::size_t>(i)] = v;
            right[static_cast<std::size_t>(i)] = 0.5f * v + 0.7f * noise();
        }
        float* ch[2] { left.data(), right.data() };
        for (int at = 0; at < n; at += 1024)
        {
            float* part[2] { ch[0] + at, ch[1] + at };
            limiter.process(part, std::min(1024, n - at));
        }
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
            peak = std::max({ peak, std::abs(left[static_cast<std::size_t>(i)]), std::abs(right[static_cast<std::size_t>(i)]) });
        check(peak <= PeakLimiter::kCeiling, "the highest sample is at or under -0.1 dBFS: " + std::to_string(peak));
        check(peak > 0.9f * PeakLimiter::kCeiling, "and it is not turned down more than it has to be: " + std::to_string(peak));
        check(limiter.maxReductionDb() > 20.0f, "the limiter reports what it did: " + std::to_string(limiter.maxReductionDb()) + " dB");
    }

    beginCase("safety: one peak is ramped down to, not stepped");
    {
        PeakLimiter limiter(sr, 1);
        const int n = 20000, delay = limiter.delaySamples();
        std::vector<float> x(n, 0.5f);
        x[5000] = 2.0f;   // the peak, on a steady level so the gain can be read off the output
        float* ch[1] { x.data() };
        limiter.process(ch, n);
        float biggestStep = 0.0f;
        for (int i = delay + 1; i < n; ++i)
            if (i != 5000 + delay && i != 5001 + delay)
                biggestStep = std::max(biggestStep, std::abs(x[static_cast<std::size_t>(i)] - x[static_cast<std::size_t>(i - 1)]));
        check(std::abs(x[static_cast<std::size_t>(5000 + delay)]) <= PeakLimiter::kCeiling, "the peak itself is held");
        check(biggestStep < 0.5f * 0.02f, "around it the level moves by under 2% a sample: " + std::to_string(biggestStep / 0.5f * 100.0f) + "%");
        check(std::abs(x[static_cast<std::size_t>(n - 1)] - 0.5f) < 0.01f, "and it has let go a third of a second later");
    }

    beginCase("safety: the trim is measured and does the limiter's work for it");
    {
        // A loud, rich tone close to full scale whose partials are lined up to keep its peaks as
        // low as they can be (as a normalised or limited recording's are): any shift raises them.
        const int n = static_cast<int>(sr * 2.0);
        std::vector<float> effect(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            double v = 0.0;
            for (int h = 1; h <= 16; ++h)
                v += std::sin(2.0 * 3.14159265358979 * 110.0 * h * i / sr - 3.14159265358979 * h * (h - 1) / 16.0);
            effect[static_cast<std::size_t>(i)] = static_cast<float>(v);
        }
        float peak = 0.0f;
        for (const float v : effect)
            peak = std::max(peak, std::abs(v));
        for (auto& v : effect)
            v *= 0.985f / peak;

        core::Stamper::Setup setup;
        setup.sampleRate = sr;
        setup.channels = 1;
        setup.staticSemitones = 3.0;

        const std::vector<std::vector<float>> channels { effect };
        const auto levels = core::measureStamp(setup, channels);
        check(levels.complete, "the stamp was measured");
        check(std::abs(levels.inputPeak - 0.985f) < 1.0e-4f, "the effect's own peak is reported: " + std::to_string(levels.inputPeak));
        std::printf("    -> shifted peak %.3f (%+.2f dB against the effect), trim %.2f dB\n", static_cast<double>(levels.outputPeak),
                    20.0 * std::log10(static_cast<double>(levels.outputPeak / levels.inputPeak)), static_cast<double>(levels.trimDb()));
        check(levels.outputPeak > PeakLimiter::kCeiling, "shifted, it would go over full scale: " + std::to_string(levels.outputPeak));
        check(levels.trim() < 1.0f && levels.trimDb() < 0.0f, "so a trim is asked for: " + std::to_string(levels.trimDb()) + " dB");

        const auto run = [&](float trim, float& outPeak, float& reduction)
        {
            auto s = setup;
            s.trim = trim;
            core::Stamper stamper(s);
            std::vector<float> out(effect);
            out.resize(effect.size() + static_cast<std::size_t>(stamper.latencySamples()), 0.0f);
            float* ch[1] { out.data() };
            stamper.process(ch, static_cast<int>(out.size()));
            outPeak = 0.0f;
            for (const float v : out)
                outPeak = std::max(outPeak, std::abs(v));
            reduction = stamper.limiterReductionDb();
        };

        float trimmedPeak = 0.0f, trimmedReduction = 0.0f, untrimmedPeak = 0.0f, untrimmedReduction = 0.0f;
        run(levels.trim(), trimmedPeak, trimmedReduction);
        run(1.0f, untrimmedPeak, untrimmedReduction);
        check(trimmedPeak <= PeakLimiter::kCeiling && trimmedPeak > 0.99f * PeakLimiter::kCeiling,
              "with the trim the peak lands on the ceiling: " + std::to_string(trimmedPeak));
        check(trimmedReduction < 0.01f, "and the limiter has nothing to do: " + std::to_string(trimmedReduction) + " dB");
        check(untrimmedPeak <= PeakLimiter::kCeiling, "without the trim the limiter still holds it: " + std::to_string(untrimmedPeak));
        check(untrimmedReduction > 0.1f, "and reports that it had to: " + std::to_string(untrimmedReduction) + " dB");

        int asked = 0;
        const auto stopped = core::measureStamp(setup, channels, [&] { return ++asked > 3; });
        check(! stopped.complete && stopped.trim() >= 1.0f, "a cancelled measurement says so and asks for no trim");

        // A quiet effect needs nothing.
        auto quiet = channels;
        for (auto& v : quiet[0])
            v *= 0.25f;
        const auto quietLevels = core::measureStamp(setup, quiet);
        check(quietLevels.complete && quietLevels.trim() >= 1.0f && quietLevels.trimDb() >= 0.0f, "a quiet effect is left at its level");
    }
}

// An effect learned with silence in front of it (the selection started before the sound did).
void testSilentLeadIn();
void testSilentLeadIn()
{
    const auto music = dMinorPad();
    const auto m = analyseMusic(music.data(), static_cast<int>(music.size()), kRate);

    const auto effectAlone = [&](double lengthSeconds)
    {
        auto x = seconds(lengthSeconds);
        addNote(x, 47.38, 0.4, 8, 0, static_cast<std::int64_t>(x.size()), [](double t) { return 0.15 * std::sin(kTwoPi * 1.5 * t); });
        return x;
    };

    for (const auto& [silence, sound] : { std::pair<double, double> { 2.0, 3.0 }, std::pair<double, double> { 4.0, 1.0 } })
    {
        const std::string name = "silent lead-in: " + std::to_string(static_cast<int>(silence)) + " s of silence, then " + std::to_string(static_cast<int>(sound)) + " s of effect";
        beginCase(name.c_str());

        const auto alone = effectAlone(sound);
        std::vector<float> led(static_cast<std::size_t>(silence * kRate), 0.0f);
        led.insert(led.end(), alone.begin(), alone.end());

        const auto a = analyseSfx(alone.data(), static_cast<int>(alone.size()), kRate);
        const auto b = analyseSfx(led.data(), static_cast<int>(led.size()), kRate);
        std::printf("    -> alone: %s, pitched %d, tonal %.2f; with the silence: %s, pitched %d, tonal %.2f\n", noteName(a.centreMidi).c_str(), a.pitched ? 1 : 0,
                    static_cast<double>(a.tonalness), noteName(b.centreMidi).c_str(), b.pitched ? 1 : 0, static_cast<double>(b.tonalness));
        check(b.valid && b.pitched, "the effect is still found to have a pitch");
        check(b.usedMethod == a.usedMethod, "by the same tracking as without the silence");
        check(std::abs(b.centreMidi - a.centreMidi) < 0.1, "it sits on the same note: " + std::to_string(b.centreMidi - a.centreMidi) + " st apart");

        // The pitch line starts where the sound does.
        const auto frames = static_cast<int>(b.hz.size());
        const int startFrame = static_cast<int>(silence / b.hopSeconds);
        int firstPitched = -1;
        for (int f = 0; f < frames && firstPitched < 0; ++f)
            if (b.hz[static_cast<std::size_t>(f)] > 0.0f)
                firstPitched = f;
        check(std::abs(b.durationSeconds - (silence + sound)) < 0.02, "its length is the whole selection");
        check(firstPitched >= startFrame - 4 && firstPitched <= startFrame + 30,
              "the pitch line starts where the sound starts: frame " + std::to_string(firstPitched) + " against " + std::to_string(startFrame));

        MatchOptions o;
        o.target = Target::Nearest;
        const auto ra = match(m, a, o), rb = match(m, b, o);
        check(rb.valid && std::abs(rb.semitones - ra.semitones) < 0.1, "the shift is the same: " + std::to_string(rb.semitones) + " against " + std::to_string(ra.semitones));

        // Stamped inside a render with handles: the silence stays silent and the sound stays where it was.
        const auto handle = static_cast<std::size_t>(1.0 * kRate);
        Stamper::Setup setup;
        setup.sampleRate = kRate;
        setup.channels = 1;
        setup.staticSemitones = rb.semitones;
        setup.curveHopSeconds = rb.hopSeconds;
        setup.curve = rb.curve;
        setup.learned = std::make_shared<const std::vector<float>>(led);
        Stamper stamper(setup);
        const auto latency = static_cast<std::size_t>(stamper.latencySamples());
        std::vector<float> stream(handle + led.size() + handle + latency, 0.0f);
        std::copy(led.begin(), led.end(), stream.begin() + static_cast<std::ptrdiff_t>(handle));
        float* ch[1] { stream.data() };
        stamper.process(ch, static_cast<int>(stream.size()));

        check(stamper.alignmentFound() && stamper.alignmentOffset() == static_cast<std::int64_t>(handle),
              "the learned effect is found in the render, silence included: offset " + std::to_string(stamper.alignmentOffset()));
        const auto soundAt = latency + handle + static_cast<std::size_t>(silence * kRate);
        float before = 0.0f, after = 0.0f;
        for (std::size_t i = 0; i + 2400 < soundAt; ++i)
            before = std::max(before, std::abs(stream[i]));
        for (std::size_t i = soundAt + 2400; i < soundAt + static_cast<std::size_t>(sound * kRate) - 2400; ++i)
            after = std::max(after, std::abs(stream[i]));
        check(before < 0.002f, "nothing sounds before the effect does: " + std::to_string(before));
        check(after > 0.2f, "and the effect is there after it: " + std::to_string(after));
        const double got = measuredMidi(stream, soundAt + 4800, soundAt + static_cast<std::size_t>(sound * kRate) - 4800);
        check(std::abs(got - rb.targetMidi) < 0.15, "stamped onto the target note: " + noteName(got) + " against " + noteName(rb.targetMidi));

        // The level is measured the same.
        const auto levels = measureStamp(setup, { led });
        check(levels.complete && levels.outputPeak > 0.2f, "and the stamp's level can be measured: peak " + std::to_string(levels.outputPeak));
    }
}

// A long window: 20 seconds of music, and an effect learned over the same 20 seconds.
void testLongWindow();
void testLongWindow()
{
    beginCase("long window: 20 s of music, an effect that comes in 12 s into the same 20 s");
    const auto clock = [] { return std::chrono::steady_clock::now(); };
    const auto since = [&](std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(clock() - t).count(); };

    // A pad that changes chord every 5 s (D minor, B flat, F, A minor), then the effect: a
    // swelling tone with a slow slide, from 12 s to 19 s.
    auto music = seconds(20.0);
    const std::vector<std::vector<double>> chords { { 38, 45, 50, 53, 57 }, { 34, 41, 46, 50, 53 }, { 29, 41, 45, 48, 53 }, { 33, 40, 45, 48, 52 } };
    for (int c = 0; c < static_cast<int>(chords.size()); ++c)
        for (double midi : chords[static_cast<std::size_t>(c)])
            addNote(music, midi, 0.1, 6, static_cast<std::int64_t>(c * 5 * kRate), static_cast<std::int64_t>((c + 1) * 5 * kRate));
    addNoise(music, 0.003, 7);

    auto effect = seconds(20.0);
    const auto from = static_cast<std::int64_t>(12.0 * kRate), to = static_cast<std::int64_t>(19.0 * kRate);
    addNote(effect, 47.38, 0.45, 8, from, to, [](double t) { return 0.4 * t / 7.0; });

    auto t = clock();
    const auto m = analyseMusic(music.data(), static_cast<int>(music.size()), kRate);
    const double musicTime = since(t);
    t = clock();
    const auto e = analyseSfx(effect.data(), static_cast<int>(effect.size()), kRate);
    const double effectTime = since(t);
    describe(m);
    check(m.valid && std::abs(m.durationSeconds - 20.0) < 0.05, "the music is analysed over all 20 s");
    check(e.valid && e.pitched && std::abs(e.durationSeconds - 20.0) < 0.05, "the effect is pitched, and as long as the selection");
    int firstPitched = -1, lastPitched = -1;
    for (int f = 0; f < static_cast<int>(e.hz.size()); ++f)
        if (e.hz[static_cast<std::size_t>(f)] > 0.0f)
        {
            if (firstPitched < 0) firstPitched = f;
            lastPitched = f;
        }
    check(std::abs(firstPitched * e.hopSeconds - 12.0) < 0.2 && std::abs(lastPitched * e.hopSeconds - 19.0) < 0.2,
          "its pitch line runs from 12 s to 19 s: " + std::to_string(firstPitched * e.hopSeconds) + " to " + std::to_string(lastPitched * e.hopSeconds));

    for (const bool follow : { false, true })
    {
        MatchOptions o;
        o.target = follow ? Target::Follow : Target::Nearest;
        o.glide = 1.0;
        o.bend = follow ? 0.35 : 0.0;
        o.offsetKnown = true;
        o.sfxOffsetInMusicSeconds = 0.0;
        t = clock();
        const auto r = match(m, e, o);
        const double matchTime = since(t);
        check(r.valid && r.curve.size() == e.hz.size(), std::string(follow ? "following" : "locked") + ": a curve as long as the effect");
        float low = 1.0e9f, high = -1.0e9f;
        for (const float v : r.curve) { low = std::min(low, v); high = std::max(high, v); }
        check(std::isfinite(low) && std::isfinite(high) && high - low < 12.5f && std::abs(high) < 13.0f && std::abs(low) < 13.0f,
              "the shift stays within an octave: " + std::to_string(low) + " to " + std::to_string(high) + " st");

        Stamper::Setup setup;
        setup.sampleRate = kRate;
        setup.channels = 2;
        setup.staticSemitones = r.semitones;
        setup.curveHopSeconds = r.hopSeconds;
        setup.curve = r.curve;
        // What Learn keeps to find the effect by is the sum of its channels.
        auto sum = effect;
        for (auto& v : sum)
            v *= 2.0f;
        setup.learned = std::make_shared<const std::vector<float>>(std::move(sum));

        t = clock();
        const auto levels = measureStamp(setup, { effect, effect });
        const double measureTime = since(t);
        check(levels.complete && levels.outputPeak > 0.2f, "its level is measured: peak " + std::to_string(levels.outputPeak));

        // Rendered in stereo with 2 s handles either side.
        Stamper stamper(setup);
        const auto handle = static_cast<std::size_t>(2.0 * kRate);
        const auto latency = static_cast<std::size_t>(stamper.latencySamples());
        std::vector<float> left(handle + effect.size() + handle + latency, 0.0f);
        std::copy(effect.begin(), effect.end(), left.begin() + static_cast<std::ptrdiff_t>(handle));
        auto right = left;
        t = clock();
        for (std::size_t at = 0; at < left.size(); at += 32768)
        {
            float* ch[2] { left.data() + at, right.data() + at };
            stamper.process(ch, static_cast<int>(std::min<std::size_t>(32768, left.size() - at)));
        }
        const double renderTime = since(t);
        check(stamper.alignmentFound() && stamper.alignmentOffset() == static_cast<std::int64_t>(handle), "the effect is found in the render");
        const auto soundAt = latency + handle + static_cast<std::size_t>(from);
        float before = 0.0f, peak = 0.0f;
        for (std::size_t i = 0; i + 2400 < soundAt; ++i)
            before = std::max(before, std::abs(left[i]));
        for (const float v : left)
            peak = std::max(peak, std::abs(v));
        check(before < 0.002f, "the 12 s before the effect stay silent: " + std::to_string(before));
        check(peak > 0.2f && peak <= PeakLimiter::kCeiling, "the effect is stamped, under full scale: " + std::to_string(peak));
        const double got = measuredMidi(left, soundAt + 9600, soundAt + 48000);
        const double want = e.centreMidi - 0.2 + static_cast<double>(r.curve[static_cast<std::size_t>((12.6) / r.hopSeconds)]);
        check(std::abs(got - want) < 0.5, "and lands where the curve says in its first second: " + noteName(got) + " against " + noteName(want));
        std::printf("    -> %s: match %.3f s, measuring the level %.2f s, rendering 24 s in stereo %.2f s\n", follow ? "following" : "locked", matchTime, measureTime, renderTime);
        check(measureTime < 10.0 && renderTime < 10.0, "measuring and rendering 20 s each take well under 10 s");
    }
    std::printf("    -> analysing 20 s: music %.2f s, effect %.2f s\n", musicTime, effectTime);
    check(musicTime < 20.0 && effectTime < 20.0, "each analysis takes less time than the audio lasts");
}

int main()
{
    testMusic();
    testSfxAndMatch();
    testTailAndEdits();
    testFollow();
    testMovement();
    testStamp();
    testSafety();
    testSilentLeadIn();
    testLongWindow();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
