// Tests for the listening engine on audio made here: short noise bursts on known beat times.
// Prints one line per test and a summary; the exit code is the number of failed tests.

#include "../Source/core/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace
{

using tt::Pin;

// Exact comparison of two values that are meant to be the very same number.
template <typename T>
bool same (T a, T b)    { return std::equal_to<T>() (a, b); }

//==============================================================================
struct Failure
{
    std::string what;
};

void expect (bool condition, const std::string& what)
{
    if (! condition)
        throw Failure { what };
}

std::string text (const char* format, double a = 0.0, double b = 0.0, double c = 0.0)
{
    char buffer[256];
    std::snprintf (buffer, sizeof (buffer), format, a, b, c);
    return buffer;
}

int run (const char* name, const std::function<std::string()>& test)
{
    try
    {
        const auto detail = test();
        std::printf ("PASS  %s  %s\n", name, detail.c_str());
        return 0;
    }
    catch (const Failure& f)
    {
        std::printf ("FAIL  %s  %s\n", name, f.what.c_str());
        return 1;
    }
}

//==============================================================================
// A cue: a quiet noise floor with a 50 ms decaying noise burst starting on each given time, each at its own level.
std::vector<float> makeCueWithLevels (const std::vector<std::pair<double, double>>& hits, double seconds, double sampleRate)
{
    std::vector<float> out ((size_t) (seconds * sampleRate), 0.0f);
    std::uint32_t state = 12345u;
    auto noise = [&]
    {
        state = state * 1664525u + 1013904223u;
        return (float) ((state >> 8) & 0xffffff) / 8388608.0f - 1.0f;
    };
    for (auto& s : out)
        s = 0.0003f * noise();
    for (const auto& [t, level] : hits)
    {
        const auto start = (long long) std::ceil (t * sampleRate);
        const auto length = (long long) (0.05 * sampleRate);
        for (long long i = 0; i < length && start + i < (long long) out.size(); ++i)
            out[(size_t) (start + i)] += (float) level * noise() * (float) std::exp (-(double) i / (0.012 * sampleRate));
    }
    return out;
}

// The same with every burst at one level.
std::vector<float> makeCue (const std::vector<double>& hitTimes, double seconds, double sampleRate)
{
    std::vector<std::pair<double, double>> hits;
    for (double t : hitTimes)
        hits.push_back ({ t, 0.6 });
    return makeCueWithLevels (hits, seconds, sampleRate);
}

std::vector<double> steadyBeats (double first, double bpm, int count)
{
    std::vector<double> out;
    for (int i = 0; i < count; ++i)
        out.push_back (first + i * 60.0 / bpm);
    return out;
}

double nearestOnsetError (const std::vector<tt::Onset>& onsets, double t)
{
    double best = 1.0e9;
    for (const auto& o : onsets)
        best = std::min (best, std::abs (o.time - t));
    return best;
}

void expectPinsInOrder (const std::vector<Pin>& pins)
{
    for (size_t i = 1; i < pins.size(); ++i)
    {
        expect (pins[i].time > pins[i - 1].time, text ("pins not sorted by time at %g s", pins[i].time));
        expect (pins[i].beat > pins[i - 1].beat, text ("pin beats not increasing at %g s", pins[i].time));
    }
}

bool hasPin (const std::vector<Pin>& pins, const Pin& wanted)
{
    for (const auto& p : pins)
        if (same (p.time, wanted.time) && same (p.beat, wanted.beat) && p.user)
            return true;
    return false;
}

// The largest difference between the grid and the true beat times, in milliseconds.
double worstBeatError (const tt::BeatGrid& grid, double firstBeat, const std::vector<double>& truth)
{
    double worst = 0.0;
    for (size_t i = 0; i < truth.size(); ++i)
        worst = std::max (worst, std::abs (grid.timeAtBeat (firstBeat + (double) i) - truth[i]));
    return worst * 1000.0;
}

constexpr double sr48 = 48000.0;

int countChecks (const tt::MapResult& map, tt::Check::Kind kind, const char* containing = "")
{
    int n = 0;
    for (const auto& c : map.checks)
        n += c.kind == kind && c.note.find (containing) != std::string::npos;
    return n;
}

std::string allChecks (const tt::MapResult& map)
{
    std::string out = text ("%g checks:", (double) map.checks.size());
    for (const auto& c : map.checks)
        out += " \"" + c.note + "\"";
    return out;
}

// Hits given directly, each of strength 0.8.
std::vector<tt::Onset> hitsAt (const std::vector<double>& times)
{
    std::vector<tt::Onset> out;
    for (double t : times)
        out.push_back ({ t, 0.8f });
    return out;
}

// 32 hits half a second apart from 0.5 s: 120 BPM, the last at 16 s.
std::vector<tt::Onset> steadyHits()    { return hitsAt (steadyBeats (0.5, 120.0, 32)); }

tt::Hint hintAt (double bpm)
{
    tt::Hint hint;
    hint.bpm = bpm;
    return hint;
}

// The cue of tests ii, iv and v: 32 beats at 128 BPM, 2.9 s with nothing, 32 more beats.
struct RestartCue
{
    double bpm = 128.0;
    std::vector<double> before = steadyBeats (0.4, bpm, 32);
    double restart = before.back() + 2.9;
    std::vector<double> after = steadyBeats (restart, bpm, 32);
    double seconds = after.back() + 1.0;
    std::vector<tt::Onset> onsets;

    RestartCue()
    {
        auto all = before;
        all.insert (all.end(), after.begin(), after.end());
        const auto audio = makeCue (all, seconds, sr48);
        onsets = tt::findOnsets (audio.data(), audio.size(), sr48);
    }
};

//==============================================================================
std::string steadyTempo (double sampleRate)
{
    const auto truth = steadyBeats (0.5, 120.0, 32);
    const auto audio = makeCue (truth, 17.5, sampleRate);
    const auto onsets = tt::findOnsets (audio.data(), audio.size(), sampleRate);

    double worstOnset = 0.0;
    for (double t : truth)
        worstOnset = std::max (worstOnset, nearestOnsetError (onsets, t));
    expect (worstOnset <= 0.0015, text ("an onset is %.2f ms from its hit", worstOnset * 1000.0));

    tt::Hint hint;
    hint.bpm = 120.0;
    const auto map = tt::mapCue (onsets, 17.5, hint);
    const tt::BeatGrid grid (map.pins, map.bpm);
    const double worst = worstBeatError (grid, 0.0, truth);
    expect (worst <= 2.0, text ("a mapped beat is %.2f ms out", worst));
    expect (std::abs (map.bpm - 120.0) <= 0.05, text ("tempo %.3f", map.bpm));
    expect (std::abs (map.firstHitTime - 0.5) <= 0.0015, text ("first hit at %.4f s", map.firstHitTime));
    expect (map.pins.size() >= 2 && map.pins.size() <= 3, text ("%g pins", (double) map.pins.size()));
    expect (map.sections.size() == 1 && map.checks.empty(), "expected one section and no checks");
    expectPinsInOrder (map.pins);
    return text ("onsets within %.2f ms, beats within %.2f ms, %.3f BPM", worstOnset * 1000.0, worst, map.bpm)
           + text (", %g pins", (double) map.pins.size());
}

std::string gapRestart()
{
    const RestartCue cue;
    tt::Hint hint;
    hint.bpm = cue.bpm;
    const auto map = tt::mapCue (cue.onsets, cue.seconds, hint);

    expect (map.sections.size() == 2, text ("%g sections", (double) map.sections.size()));
    const auto& second = map.sections[1];
    expect (second.kind == tt::Section::Kind::gap, "the second section is not a gap restart");
    expect (std::abs (second.startTime - cue.restart) <= 0.002, text ("restart found at %.4f s, hit at %.4f s", second.startTime, cue.restart));
    // 2.9 s is 6.19 beats: the restart is given the whole beat 31 + 6 = 37, bar 10 beat 2.
    expect (second.startBeat == 37.0 && ! second.confirmed, text ("restart beat %g", second.startBeat));

    int restartChecks = 0, otherChecks = 0;
    std::string note;
    for (const auto& c : map.checks)
    {
        if (c.kind == tt::Check::Kind::restart)
        {
            ++restartChecks;
            note = c.note;
        }
        else
            ++otherChecks;
    }
    expect (restartChecks == 1 && otherChecks == 0, text ("%g restart checks, %g others", restartChecks, otherChecks));
    expect (note == "Beat restarts. Bar 10 beat 2?", "check says: " + note);

    int restartPins = 0;
    for (const auto& p : map.pins)
        restartPins += p.restart;
    expect (restartPins == 1, text ("%g restart pins", restartPins));
    expectPinsInOrder (map.pins);

    const tt::BeatGrid grid (map.pins, map.bpm);
    const double worstBefore = worstBeatError (grid, 0.0, cue.before);
    const double worstAfter = worstBeatError (grid, 37.0, cue.after);
    expect (worstBefore <= 3.0 && worstAfter <= 3.0, text ("beats out by %.2f ms before, %.2f ms after", worstBefore, worstAfter));
    return text ("restart at %.3f s, beats within %.2f / %.2f ms, \"", second.startTime, worstBefore, worstAfter) + note + "\"";
}

std::string longGapAndHalfBeat()
{
    // 16 beats at 120, then 6.2 s (12.4 beats) with nothing.
    const auto before = steadyBeats (0.5, 120.0, 16);
    const double restart = before.back() + 6.2;
    auto all = before;
    const auto after = steadyBeats (restart, 120.0, 16);
    all.insert (all.end(), after.begin(), after.end());
    const double seconds = after.back() + 1.0;
    const auto audio = makeCue (all, seconds, sr48);
    const auto onsets = tt::findOnsets (audio.data(), audio.size(), sr48);

    tt::Hint hint;
    hint.bpm = 120.0;
    tt::Options options;
    options.bridge = tt::Options::Bridge::halfBeat;
    const auto map = tt::mapCue (onsets, seconds, hint, {}, options);
    expect (map.sections.size() == 2, text ("%g sections", (double) map.sections.size()));
    expect (map.sections[1].startBeat == 27.5, text ("restart beat %g", map.sections[1].startBeat));
    std::string restartNote, noHitsNote;
    for (const auto& c : map.checks)
        (c.kind == tt::Check::Kind::restart ? restartNote : noHitsNote) = c.note;
    expect (map.checks.size() == 2, text ("%g checks", (double) map.checks.size()));
    expect (restartNote == "Beat restarts. Bar 7 beat 4.5?", "check says: " + restartNote);
    expect (noHitsNote == "No hits for 6.2 s", "check says: " + noHitsNote);
    return "\"" + restartNote + "\", \"" + noHitsNote + "\"";
}

std::string phaseJump()
{
    // Bar 9 beat 1 (beat 32) and everything after it arrive 90 ms late; nothing stops.
    auto truth = steadyBeats (0.5, 120.0, 64);
    for (size_t i = 32; i < truth.size(); ++i)
        truth[i] += 0.090;
    const auto audio = makeCue (truth, 33.5, sr48);
    const auto onsets = tt::findOnsets (audio.data(), audio.size(), sr48);

    tt::Hint hint;
    hint.bpm = 120.0;
    const auto map = tt::mapCue (onsets, 33.5, hint);
    expect (map.sections.size() == 2, text ("%g sections", (double) map.sections.size()));
    expect (map.sections[1].kind == tt::Section::Kind::jump, "the second section is not a jump");
    const double off = std::abs (map.sections[1].startTime - truth[32]);
    expect (off <= 0.003, text ("jump found %.3f s from bar 9", off));
    expect (same (map.sections[1].startBeat, 32.0), text ("the jump is given beat %g", map.sections[1].startBeat));
    expectPinsInOrder (map.pins);
    const tt::BeatGrid grid (map.pins, map.bpm);
    const std::vector<double> before (truth.begin(), truth.begin() + 32), after (truth.begin() + 32, truth.end());
    const double worstBefore = worstBeatError (grid, 0.0, before), worstAfter = worstBeatError (grid, 32.0, after);
    expect (worstBefore <= 3.0 && worstAfter <= 3.0, text ("beats out by %.2f ms before the jump, %.2f ms after", worstBefore, worstAfter));

    tt::Options noJumps;
    noJumps.detectJumps = false;
    expect (tt::mapCue (onsets, 33.5, hint, {}, noJumps).sections.size() == 1, "a jump was reported with jump detection off");
    return text ("jump at %.3f s, bar 9 at %.3f s, beats within %.2f ms after it", map.sections[1].startTime, truth[32], worstAfter);
}

std::string roughHint()
{
    const RestartCue cue;
    tt::Hint hint;
    hint.bpm = cue.bpm;
    const auto exact = tt::mapCue (cue.onsets, cue.seconds, hint);

    std::string detail;
    for (double bpm : { cue.bpm - 3.0, cue.bpm + 3.0, 0.0 })
    {
        hint.bpm = bpm;
        const auto map = tt::mapCue (cue.onsets, cue.seconds, hint);
        expect (map.pins.size() == exact.pins.size(), text ("hint %g: %g pins, exact hint gives %g", bpm, (double) map.pins.size(), (double) exact.pins.size()));
        double worst = 0.0;
        for (size_t i = 0; i < map.pins.size(); ++i)
        {
            expect (same (map.pins[i].beat, exact.pins[i].beat), text ("hint %g: pin %g has another beat", bpm, (double) i));
            worst = std::max (worst, std::abs (map.pins[i].time - exact.pins[i].time));
        }
        expect (worst <= 0.002, text ("hint %g: a pin is %.2f ms from the exact hint's", bpm, worst * 1000.0));
        expect (std::abs (map.bpm - cue.bpm) <= 0.05, text ("hint %g: tempo %.3f", bpm, map.bpm));
        detail += text ("hint %g -> %.3f BPM  ", bpm, map.bpm);
    }
    const double estimate = tt::estimateTempo (cue.onsets, cue.seconds);
    expect (std::abs (estimate - cue.bpm) <= 2.0, text ("estimateTempo gives %.2f", estimate));
    return detail + text ("estimate %.2f", estimate);
}

std::string userPins()
{
    const RestartCue cue;
    tt::Hint hint;
    hint.bpm = cue.bpm;

    // The restart confirmed as bar 11 beat 1 (beat 40), on a time 4 ms from the detected hit.
    const Pin confirm { 40.0, cue.restart + 0.004, true, false };
    // A beat moved by hand: beat 10 placed 12 ms after its hit.
    const Pin anchor { 10.0, cue.before[10] + 0.012, true, false };
    tt::Options tight;   // so the beats after a pin placed off its hit are pinned back onto the hits
    tight.pinToleranceMs = 2.0;
    auto map = tt::mapCue (cue.onsets, cue.seconds, hint, { confirm, anchor }, tight);

    expect (map.sections.size() == 2, text ("%g sections", (double) map.sections.size()));
    expect (map.sections[1].confirmed && same (map.sections[1].startBeat, 40.0), "the confirmed beat was not used");
    expect (same (map.sections[1].startTime, confirm.time), "the confirmed section does not start on the user's pin");
    for (const auto& c : map.checks)
        expect (c.kind != tt::Check::Kind::restart, "a confirmed restart still has a check");
    expect (hasPin (map.pins, confirm) && hasPin (map.pins, anchor), "a user pin is missing or changed");
    expectPinsInOrder (map.pins);

    const tt::BeatGrid grid (map.pins, map.bpm);
    expect (std::abs (grid.timeAtBeat (10.0) - anchor.time) < 1.0e-9, "the grid does not pass through the user's pin");
    expect (std::abs (grid.timeAtBeat (40.0) - confirm.time) < 1.0e-9, "the grid does not pass through the confirmed restart");
    std::vector<double> afterAnchor (cue.before.begin() + 11, cue.before.end());
    const double worstBefore = worstBeatError (grid, 11.0, afterAnchor);
    std::vector<double> afterRestart (cue.after.begin() + 1, cue.after.end());
    const double worstAfter = worstBeatError (grid, 41.0, afterRestart);
    expect (worstBefore <= 3.0 && worstAfter <= 3.0, text ("beats out by %.2f ms after the anchor, %.2f ms after the restart", worstBefore, worstAfter));

    // A restart the engine has no reason to see: the user says beat 16's hit is bar 26 beat 1 (beat 100).
    const Pin restart { 100.0, cue.before[16], true, true };
    map = tt::mapCue (cue.onsets, cue.seconds, hint, { restart });
    expect (map.sections.size() == 3, text ("user restart: %g sections", (double) map.sections.size()));
    expect (map.sections[1].kind == tt::Section::Kind::user && map.sections[1].confirmed
                && same (map.sections[1].startTime, restart.time) && same (map.sections[1].startBeat, 100.0),
            "the user's restart is not a section of its own");
    expect (hasPin (map.pins, restart), "the user's restart pin is missing or changed");
    expectPinsInOrder (map.pins);
    const tt::BeatGrid grid2 (map.pins, map.bpm);
    std::vector<double> rest (cue.before.begin() + 17, cue.before.end());
    const double worstRest = worstBeatError (grid2, 101.0, rest);
    expect (worstRest <= 3.0, text ("beats after the user's restart out by %.2f ms", worstRest));

    // A user pin whose beat is lower than engine pins before it: the user pin stays, those go.
    const Pin low { 5.0, cue.restart, true, false };
    map = tt::mapCue (cue.onsets, cue.seconds, hint, { low });
    expect (hasPin (map.pins, low), "the low user pin is missing or changed");
    expectPinsInOrder (map.pins);
    int enginePinsBefore = 0;
    for (const auto& p : map.pins)
        if (! p.user)
        {
            expect ((p.time < low.time) == (p.beat < low.beat), text ("an engine pin at %g s, beat %g, is on the wrong side of the low user pin", p.time, p.beat));
            enginePinsBefore += p.time < low.time;
        }
    expect (enginePinsBefore == 1 && same (map.pins.front().beat, 0.0), text ("%g engine pins before the low user pin", enginePinsBefore));
    expect (std::abs (tt::BeatGrid (map.pins, map.bpm).timeAtBeat (6.0) - cue.after[1]) <= 0.003, "the beats after the low user pin are not counted from it");
    expect (countChecks (map, tt::Check::Kind::unsure, "bar is not after") == 1, "no check says the pin's bar runs backwards: " + allChecks (map));
    return text ("anchor and confirmation kept exactly; beats within %.2f / %.2f / %.2f ms", worstBefore, worstAfter, worstRest);
}

std::string gridAndPositions()
{
    const std::vector<Pin> pins { { 0.0, 1.0, false, false }, { 8.0, 5.0, false, false }, { 16.0, 8.0, false, true } };
    const tt::BeatGrid grid (pins, 100.0);
    auto near = [] (double a, double b) { return std::abs (a - b) < 1.0e-9; };
    expect (near (grid.timeAtBeat (4.0), 3.0) && near (grid.timeAtBeat (12.0), 6.5), "timeAtBeat between pins");
    // The last segment ends on a restart, so it is not carried on: after it the grid runs at the first segment's 120.
    expect (near (grid.timeAtBeat (-2.0), 0.0) && near (grid.timeAtBeat (20.0), 10.0), "timeAtBeat outside the pins");
    expect (near (grid.beatAtTime (3.0), 4.0) && near (grid.beatAtTime (0.0), -2.0) && near (grid.beatAtTime (10.0), 20.0), "beatAtTime");
    expect (near (grid.bpmAtTime (2.0), 120.0) && near (grid.bpmAtTime (7.0), 160.0) && near (grid.bpmAtTime (30.0), 120.0) && near (grid.bpmAtTime (-5.0), 120.0), "bpmAtTime");

    // A lone first hit, a restart five beats on, then 15 beats at 120: the lead-in runs at 120, not at the bridge's tempo.
    const tt::BeatGrid bridged ({ { 0.0, 0.2, false, false }, { 5.0, 2.85, false, true }, { 20.0, 10.35, false, false } }, 100.0);
    expect (near (bridged.bpmAtTime (0.0), 120.0) && near (bridged.timeAtBeat (-2.0), -0.8), text ("lead-in before a bridge runs at %.2f", bridged.bpmAtTime (0.0)));
    // Two pins half a millisecond apart at the end: what follows runs at the tempo before them.
    const tt::BeatGrid close ({ { 0.0, 0.5, false, false }, { 31.0, 16.0, false, false }, { 40.0, 16.0005, true, false } }, 100.0);
    expect (near (close.bpmAtTime (20.0), 120.0) && near (close.timeAtBeat (42.0), 17.0005), text ("after two close pins the grid runs at %.2f", close.bpmAtTime (20.0)));
    // With no segment that can be carried on, the fallback tempo is used.
    const tt::BeatGrid onlyClose ({ { 0.0, 1.0, false, false }, { 9.0, 1.0005, false, false } }, 100.0);
    expect (near (onlyClose.bpmAtTime (5.0), 100.0) && near (onlyClose.bpmAtTime (0.0), 100.0), "two close pins alone do not set the tempo outside them");
    for (double t = -3.0; t < 14.0; t += 0.37)
        expect (near (grid.timeAtBeat (grid.beatAtTime (t)), t), text ("round trip at %g s", t));

    const tt::BeatGrid one ({ { 8.0, 2.0, false, false } }, 120.0);
    expect (near (one.timeAtBeat (10.0), 3.0) && near (one.beatAtTime (1.0), 6.0) && near (one.bpmAtTime (0.0), 120.0), "one pin uses the fallback tempo");
    const tt::BeatGrid none ({}, 60.0);
    expect (none.empty() && near (none.timeAtBeat (3.0), 3.0) && near (none.beatAtTime (2.0), 2.0), "no pins uses the fallback tempo");

    auto position = [] (double beat, int perBar, const char* wanted)
    {
        const auto got = tt::formatBarBeat (beat, perBar);
        expect (got == wanted, text ("formatBarBeat (%g, %g) gives ", beat, perBar) + got + ", expected " + wanted);
    };
    position (0.0, 4, "1|1|000");
    position (156.0, 4, "40|1|000");
    position (2.5, 4, "1|3|480");
    position (1.0 + 1.0 / 960.0, 4, "1|2|001");
    position (0.9996, 4, "1|2|000");      // 959.6 ticks rounds up into the next beat
    position (3.9999, 4, "2|1|000");      // and into the next bar
    position (3.9990, 4, "1|4|959");
    position (7.0, 3, "3|2|000");
    // Before bar 1 the bars count down through 0, each beat with its own text.
    position (-0.25, 4, "0|4|720");
    position (-1.0, 4, "0|4|000");
    position (-3.0, 4, "0|2|000");
    position (-4.0, 4, "0|1|000");
    position (-4.5, 4, "-1|4|480");
    return "grid values, round trips and thirteen positions";
}

std::string click()
{
    // Beat 0 at 5.00001 s (a fractional sample), 120 BPM, 4/4, 48 kHz.
    const double first = 5.00001;
    const tt::BeatGrid grid ({ { 0.0, first, false, false }, { 8.0, first + 4.0, false, false } }, 120.0);
    const auto out = tt::renderClick (grid, 4, sr48, (size_t) (12.0 * sr48));
    expect (out.size() == (size_t) (12.0 * sr48), "wrong length");

    auto firstSound = [&] (double from, double to)
    {
        for (auto i = (size_t) (from * sr48); i < (size_t) (to * sr48) && i < out.size(); ++i)
            if (out[i] != 0.0f)
                return (long long) i;
        return -1LL;
    };
    auto peak = [&] (double from, double to)
    {
        float p = 0.0f;
        for (auto i = (size_t) (from * sr48); i < (size_t) (to * sr48) && i < out.size(); ++i)
            p = std::max (p, std::abs (out[i]));
        return (double) p;
    };
    auto crossings = [&] (double from, double to)
    {
        int n = 0;
        for (auto i = (size_t) (from * sr48) + 1; i < (size_t) (to * sr48); ++i)
            n += (out[i - 1] < 0.0f) != (out[i] < 0.0f);
        return n;
    };

    // Nothing more than a bar before the first pin; the first click is on beat -4 at 3.00001 s.
    const long long lead = firstSound (0.0, 12.0);
    expect (lead == (long long) std::ceil ((first - 2.0) * sr48), text ("first sound at sample %g", (double) lead));
    // Beat 0 starts on the first sample after 5.00001 s and is silent before it.
    const auto beat0 = (long long) std::ceil (first * sr48);
    expect (firstSound (first - 0.4, first + 0.1) == beat0, text ("beat 0 starts at sample %g, expected %g", (double) firstSound (first - 0.4, first + 0.1), (double) beat0));
    // The burst is over after 12 ms.
    expect (peak (first + 0.0125, first + 0.49) == 0.0, "sound between clicks");

    const double accent = peak (first, first + 0.012), plain = peak (first + 0.5, first + 0.512);
    expect (accent > 0.45 && accent < 0.52, text ("beat 1 peaks at %.3f", accent));
    expect (plain > 0.28 && plain < 0.33, text ("other beats peak at %.3f", plain));
    const int accentCrossings = crossings (first + 0.0005, first + 0.0055), plainCrossings = crossings (first + 0.5005, first + 0.5055);
    expect (accentCrossings >= 19 && accentCrossings <= 21, text ("beat 1 crosses zero %g times in 5 ms", accentCrossings));
    expect (plainCrossings >= 9 && plainCrossings <= 11, text ("other beats cross zero %g times in 5 ms", plainCrossings));
    // Beat 4 is the next beat 1.
    expect (std::abs (peak (first + 2.0, first + 2.012) - accent) < 0.02, "beat 1 of the second bar is not accented");
    expect (tt::renderClick (tt::BeatGrid(), 4, sr48, 1000) == std::vector<float> (1000, 0.0f), "an empty grid should be silent");
    return text ("beat 1 peak %.3f at 2 kHz, others %.3f at 1 kHz, start sample exact", accent, plain);
}


std::string sparseHits()
{
    // 16 hits on every beat, 8 on every second beat, 16 on every beat again: one section throughout.
    auto times = steadyBeats (0.5, 120.0, 16);
    for (double t : steadyBeats (8.5, 60.0, 8))
        times.push_back (t);
    for (double t : steadyBeats (16.0, 120.0, 16))
        times.push_back (t);
    auto map = tt::mapCue (hitsAt (times), 24.5, hintAt (120.0));
    expect (map.sections.size() == 1, text ("half-time middle: %g sections, ", (double) map.sections.size()) + allChecks (map));
    expect (map.checks.empty(), "half-time middle: " + allChecks (map));
    expectPinsInOrder (map.pins);
    tt::BeatGrid grid (map.pins, map.bpm);
    double worst = 0.0;
    for (double t : times)
        worst = std::max (worst, std::abs (grid.timeAtBeat (std::round (grid.beatAtTime (t))) - t));
    expect (worst <= 0.002 && std::abs (grid.beatAtTime (23.5) - 46.0) < 0.01, text ("half-time middle: a hit is %.2f ms from a beat, the last hit is beat %.2f", worst * 1000.0, grid.beatAtTime (23.5)));

    // A hit on every second beat only.
    const auto everyOther = hitsAt (steadyBeats (1.0, 60.0, 19));
    map = tt::mapCue (everyOther, 20.0, hintAt (120.0));
    expect (map.sections.size() == 1 && map.pins.size() >= 2 && map.checks.empty(), text ("every second beat: %g pins, ", (double) map.pins.size()) + allChecks (map));
    grid = tt::BeatGrid (map.pins, map.bpm);
    expect (std::abs (grid.timeAtBeat (36.0) - 19.0) <= 0.002 && std::abs (map.bpm - 120.0) <= 0.05, text ("every second beat: beat 36 at %.4f s, %.3f BPM", grid.timeAtBeat (36.0), map.bpm));

    // The same hits against a tempo they do not fit at all: the stretch is reported, not left silent.
    map = tt::mapCue (steadyHits(), 17.0, hintAt (400.0));
    expect (countChecks (map, tt::Check::Kind::unsure, "Beat not followed") == 1, "400 BPM hint: " + allChecks (map));

    // Hits between two sections that the beat was not followed through are not called "no hits".
    times = steadyBeats (0.5, 120.0, 16);
    for (double t : { 9.13, 9.57, 10.21, 10.94, 11.33, 12.07 })
        times.push_back (t);
    for (double t : steadyBeats (14.0, 120.0, 16))
        times.push_back (t);
    map = tt::mapCue (hitsAt (times), 22.5, hintAt (120.0));
    expect (countChecks (map, tt::Check::Kind::noHits) == 0 && countChecks (map, tt::Check::Kind::unsure, "Beat not followed") == 1,
            "loose hits between sections: " + allChecks (map));
    return "half-time stretches are followed; hits left unmapped are reported";
}

std::string wrongTempo()
{
    // Hints outside the range the tempo is refined over lock onto a ratio of the real tempo.
    std::string detail;
    for (double bpm : { 100.0, 110.0, 130.0 })
    {
        const auto map = tt::mapCue (steadyHits(), 17.0, hintAt (bpm));
        expect (countChecks (map, tt::Check::Kind::unsure) >= 1, text ("hint %g settled on %.3f with no check", bpm, map.bpm));
        detail += text ("%g -> %.2f, checked  ", bpm, map.bpm);
    }
    // A hit on every beat and a weaker one between: at four fifths of the tempo every second beat
    // still lands on a hit, but few land on a strong one.
    std::vector<tt::Onset> withOffbeats;
    for (double t : steadyBeats (0.5, 120.0, 48))
    {
        withOffbeats.push_back ({ t, 0.8f });
        withOffbeats.push_back ({ t + 0.25, 0.2f });
    }
    const auto slow = tt::mapCue (withOffbeats, 25.0, hintAt (96.0));
    expect (countChecks (slow, tt::Check::Kind::unsure, "Few beats") >= 1, text ("off-beats, hint 96: settled on %.3f, ", slow.bpm) + allChecks (slow));
    const auto right = tt::mapCue (withOffbeats, 25.0, hintAt (120.0));
    expect (right.checks.empty() && std::abs (right.bpm - 120.0) <= 0.05, text ("off-beats, hint 120: %.3f BPM, ", right.bpm) + allChecks (right));

    for (double bpm : { 112.0, 120.0, 128.0, 0.0 })
    {
        const auto map = tt::mapCue (steadyHits(), 17.0, hintAt (bpm));
        expect (map.checks.empty() && std::abs (map.bpm - 120.0) <= 0.05, text ("hint %g: %.3f BPM, ", bpm, map.bpm) + allChecks (map));
    }
    return detail;
}

std::string awkwardInput()
{
    const auto hits = steadyHits();
    const auto plain = tt::mapCue (hits, 17.0, hintAt (120.0));

    // A first hit placed a quarter of a second before the hits: it is beat 0 exactly, the first hit
    // after it is beat 1, and the hinted tempo is not pulled away.
    auto hint = hintAt (120.0);
    hint.firstHitTime = 0.25;
    auto map = tt::mapCue (hits, 17.0, hint);
    expect (! map.pins.empty() && same (map.pins.front().time, 0.25) && same (map.pins.front().beat, 0.0), "first hit off the hits: the first pin is not on the given time");
    expect (std::abs (map.bpm - 120.0) <= 0.05, text ("first hit off the hits: tempo %.3f", map.bpm));
    tt::BeatGrid grid (map.pins, map.bpm);
    expect (std::abs (grid.timeAtBeat (1.0) - 0.5) <= 0.001 && std::abs (grid.timeAtBeat (32.0) - 16.0) <= 0.001,
            text ("first hit off the hits: beat 1 at %.4f s, beat 32 at %.4f s", grid.timeAtBeat (1.0), grid.timeAtBeat (32.0)));
    expectPinsInOrder (map.pins);

    // A first hit after the end of the music, or not a number, counts as not given.
    for (double first : { 500.0, std::nan (""), std::numeric_limits<double>::infinity() })
    {
        hint.firstHitTime = first;
        map = tt::mapCue (hits, 17.0, hint);
        expect (same (map.firstHitTime, 0.5) && map.pins.size() == plain.pins.size(), text ("first hit %g: mapped from %.3f s", first, map.firstHitTime));
    }

    // A tempo that is not a number, or far outside music, counts as not given.
    for (double bpm : { std::nan (""), std::numeric_limits<double>::infinity(), -5.0, 1.0e9, 1.0e-6 })
    {
        map = tt::mapCue (hits, 17.0, hintAt (bpm));
        expect (std::abs (map.bpm - 120.0) <= 0.05 && map.pins.size() == plain.pins.size() && map.checks.empty(), text ("tempo hint %g gives %.3f BPM", bpm, map.bpm));
    }
    map = tt::mapCue (hits, std::nan (""), hintAt (120.0));
    expect (map.pins.size() == plain.pins.size(), "a length that is not a number");
    map = tt::mapCue ({}, 17.0, hintAt (120.0));
    expectPinsInOrder (map.pins);
    expect (map.checks.empty() && map.onsets.empty(), "no hits at all");
    return "first hit off the hits, beyond the end or not a number; tempo and length not numbers; no hits";
}

std::string userPinTempo()
{
    // One user pin that is off the engine's count does not change the tempo reported for the section.
    std::string detail;
    const std::vector<Pin> cases { { 41.0, 20.5, true, false }, { 300.0, 100.0, true, false }, { 10.0, 10.5, true, false } };
    for (const auto& pin : cases)
    {
        const auto map = tt::mapCue (steadyHits(), 17.0, hintAt (120.0), { pin });
        expect (hasPin (map.pins, pin), text ("the user pin at %g s is missing", pin.time));
        expect (std::abs (map.bpm - 120.0) <= 0.05 && std::abs (map.sections.front().bpm - 120.0) <= 0.05,
                text ("user pin at %g s: tempo %.3f, section %.3f", pin.time, map.bpm, map.sections.front().bpm));
        expect (map.sections.back().endTime <= 16.001, text ("user pin at %g s: the section ends at %.3f s", pin.time, map.sections.back().endTime));
        expectPinsInOrder (map.pins);
        detail += text ("pin at %g s -> %.3f BPM  ", pin.time, map.bpm);
    }
    return detail;
}

std::string conflictingUserPins()
{
    // Two user pins that do not move forward in both time and beat: the one given later is kept,
    // the other is left out with a check, and the pins stay in order.
    struct Case { Pin first, second; bool check; };
    const std::vector<Case> cases {
        { { 8.0, 4.5, true, false }, { 12.0, 4.5, true, false }, true },     // the same time
        { { 8.0, 4.5, true, false }, { 8.0, 4.5, true, false }, false },     // the same pin twice
        { { 20.0, 4.5, true, false }, { 12.0, 8.5, true, false }, true },    // later in time, earlier in beats
        { { 12.0, 8.5, true, false }, { 20.0, 4.5, true, false }, true },    // the same two, given the other way round
    };
    for (const auto& c : cases)
    {
        const auto map = tt::mapCue (steadyHits(), 17.0, hintAt (120.0), { c.first, c.second });
        expectPinsInOrder (map.pins);
        expect (hasPin (map.pins, c.second), text ("the pin given later (beat %g at %g s) is missing", c.second.beat, c.second.time));
        int userPins = 0;
        for (const auto& p : map.pins)
            userPins += p.user;
        expect (userPins == 1, text ("%g user pins in the map", userPins));
        expect (countChecks (map, tt::Check::Kind::unsure, "does not fit") == (c.check ? 1 : 0), allChecks (map));
        const tt::BeatGrid grid (map.pins, map.bpm);
        for (double b = -4.0; b < 40.0; b += 0.5)
            expect (grid.timeAtBeat (b + 0.5) > grid.timeAtBeat (b), text ("the grid runs backwards at beat %g", b));
    }
    // Pins that do move forward are all kept, whatever order they are given in.
    const Pin a { 8.0, 4.52, true, false }, b { 12.0, 6.49, true, false }, c { 20.0, 10.51, true, false };
    const auto map = tt::mapCue (steadyHits(), 17.0, hintAt (120.0), { c, a, b });
    expect (hasPin (map.pins, a) && hasPin (map.pins, b) && hasPin (map.pins, c) && countChecks (map, tt::Check::Kind::unsure, "does not fit") == 0, "pins in order were not all kept");
    expectPinsInOrder (map.pins);

    // A pin whose time or beat is not a number is left out.
    const auto odd = tt::mapCue (steadyHits(), 17.0, hintAt (120.0), { { std::nan (""), 4.5, true, false }, { 8.0, std::nan (""), true, false } });
    for (const auto& p : odd.pins)
        expect (! p.user, "a user pin that is not a number is in the map");
    return "the later-given pin is kept, the other is left out with a check";
}

std::string userRestartOffHit()
{
    // 32 beats, a gap, 24 beats from 20 s, then everything 90 ms late for 24 more beats.
    auto times = steadyBeats (0.5, 120.0, 32);
    for (double t : steadyBeats (20.0, 120.0, 24))
        times.push_back (t);
    const double jump = 32.0 + 0.090;
    const auto tail = steadyBeats (jump, 120.0, 24);
    times.insert (times.end(), tail.begin(), tail.end());
    const auto hits = hitsAt (times);
    const double seconds = tail.back() + 1.0;

    // The restart moved by hand to 100 ms after its hit: the beats after it are still followed
    // on the hits, and the jump later on is still found.
    const Pin restart { 40.0, 20.1, true, true };
    const auto map = tt::mapCue (hits, seconds, hintAt (120.0), { restart });
    expect (hasPin (map.pins, restart), "the user's restart is missing or changed");
    expect (map.sections.size() == 3 && map.sections[2].kind == tt::Section::Kind::jump && std::abs (map.sections[2].startTime - jump) <= 0.002,
            text ("%g sections", (double) map.sections.size()));
    expectPinsInOrder (map.pins);
    const tt::BeatGrid grid (map.pins, map.bpm);
    expect (std::abs (grid.timeAtBeat (41.0) - 20.5) <= 0.002 && std::abs (grid.timeAtBeat (63.0) - 31.5) <= 0.002,
            text ("after the moved restart: beat 41 at %.4f s, beat 63 at %.4f s", grid.timeAtBeat (41.0), grid.timeAtBeat (63.0)));
    expect (map.pins.back().time >= tail.back() - 0.002, text ("the last pin is at %.3f s", map.pins.back().time));
    return text ("moved restart kept, jump found at %.3f s, last pin at %.3f s", map.sections[2].startTime, map.pins.back().time);
}

std::string onsetsOnAwkwardAudio()
{
    // Audio with no hits in it gives no hits.
    const size_t n = (size_t) (10.0 * sr48);
    std::vector<float> audio (n, 0.0f);
    expect (tt::findOnsets (audio.data(), n, sr48).empty(), "hits found in silence");
    expect (tt::findOnsets (nullptr, 0, sr48).empty() && tt::findOnsets (audio.data(), 10, sr48).empty() && tt::findOnsets (audio.data(), n, 0.0).empty(), "hits found in nothing");
    for (size_t i = 0; i < n; ++i)
        audio[i] = 0.3f * (float) std::sin (2.0 * 3.14159265358979323846 * 220.0 * (double) i / sr48);
    const auto inTone = tt::findOnsets (audio.data(), n, sr48);
    expect (inTone.size() <= 1, text ("%g hits found in a steady tone", (double) inTone.size()));
    const auto floorOnly = makeCue ({}, 10.0, sr48);
    const auto inFloor = tt::findOnsets (floorOnly.data(), floorOnly.size(), sr48);
    expect (inFloor.empty(), text ("%g hits found in a noise floor at -70 dB", (double) inFloor.size()));

    // Detection on a stem, placed on the mix: the stem's hits are 4 ms late against the mix's.
    const auto truth = steadyBeats (0.5, 120.0, 16);
    auto late = truth;
    for (auto& t : late)
        t += 0.004;
    const auto mix = makeCue (truth, 9.0, sr48), stem = makeCue (late, 9.0, sr48);
    const auto onStem = tt::findOnsets (stem.data(), stem.size(), sr48);
    const auto placed = tt::findOnsets (stem.data(), stem.size(), sr48, mix.data());
    auto strongOnes = [] (const std::vector<tt::Onset>& all)
    {
        std::vector<tt::Onset> out;
        for (const auto& o : all)
            if (o.strength >= 0.3f)
                out.push_back (o);
        return out;
    };
    const auto strongStem = strongOnes (onStem), strongPlaced = strongOnes (placed);
    expect (strongStem.size() == 16 && strongPlaced.size() == 16 && placed.size() <= onStem.size(),
            text ("%g strong hits on the stem, %g placed on the mix", (double) strongStem.size(), (double) strongPlaced.size()));
    double worstStem = 0.0, worstPlaced = 0.0;
    for (size_t i = 0; i < 16; ++i)
    {
        worstStem = std::max (worstStem, std::abs (strongStem[i].time - late[i]));
        worstPlaced = std::max (worstPlaced, std::abs (strongPlaced[i].time - truth[i]));
        expect (same (strongPlaced[i].strength, strongStem[i].strength), "placing a hit on the mix changed its strength");
    }
    expect (worstStem <= 0.0015 && worstPlaced <= 0.0015, text ("stem hits within %.2f ms, placed hits within %.2f ms", worstStem * 1000.0, worstPlaced * 1000.0));
    return text ("nothing found in silence, a tone or a noise floor; stem hits placed on the mix within %.2f ms", worstPlaced * 1000.0);
}

std::string clickAcrossRestart()
{
    // 8 beats at 120 from 1 s, then the beat restarts at 7.3 s as beat 20 (bar 6 beat 1).
    const tt::BeatGrid grid ({ { 0.0, 1.0, false, false }, { 8.0, 5.0, false, false }, { 20.0, 7.3, false, true }, { 28.0, 11.3, false, false } }, 120.0);
    const auto out = tt::renderClick (grid, 4, sr48, (size_t) (13.0 * sr48));
    auto startsAt = [&] (double t)
    {
        const auto i = (size_t) std::ceil (t * sr48 - 1.0e-6);
        return i > 0 && i + 1 < out.size() && out[i - 1] == 0.0f && (out[i] != 0.0f || out[i + 1] != 0.0f);
    };
    expect (startsAt (5.0) && startsAt (7.3) && startsAt (7.8) && startsAt (12.3), "a click is missing at a pin or after the restart");
    float accent = 0.0f;
    for (auto i = (size_t) (7.3 * sr48); i < (size_t) (7.312 * sr48); ++i)
        accent = std::max (accent, std::abs (out[i]));
    expect (accent > 0.45f, text ("the restart on beat 1 of a bar peaks at %.3f", accent));

    // Two pins half a millisecond apart do not fill the rest of the file with clicks.
    const tt::BeatGrid close ({ { 0.0, 0.5, false, false }, { 31.0, 16.0, false, false }, { 40.0, 16.0005, true, false } }, 120.0);
    const auto tail = tt::renderClick (close, 4, sr48, (size_t) (180.0 * sr48));
    int clicks = 0;
    size_t silentFor = 1000;   // samples since the last sound
    for (auto i = (size_t) (16.9 * sr48); i < tail.size(); ++i)
    {
        if (tail[i] != 0.0f)
        {
            clicks += silentFor > 48;   // a new click after a millisecond of silence
            silentFor = 0;
        }
        else
            ++silentFor;
    }
    // Beats 42 to 367 fall between 17 s and the end: 326 clicks at 120 BPM.
    expect (clicks == 326, text ("%g clicks in the 163 s after two close pins", clicks));
    return text ("clicks follow the grid across a restart; %g clicks in 163 s after two close pins", clicks);
}

//==============================================================================
// The largest difference between the tempo of a stretch between two pins and `bpm`, as a share of it.
double worstSegmentTempo (const std::vector<Pin>& pins, double bpm)
{
    double worst = 0.0;
    for (size_t i = 1; i < pins.size(); ++i)
        if (! pins[i].restart)
            worst = std::max (worst, std::abs (60.0 * (pins[i].beat - pins[i - 1].beat) / (pins[i].time - pins[i - 1].time) / bpm - 1.0));
    return worst;
}

// A minute at a constant 122 BPM with a busy pattern, two bars long, starting half a second in.
// Loud hits on the beat, except beat 2 of every second bar. Hits as loud on the "and" of beats 2
// and 4, quieter ones a sixteenth before beats 1 and 3 and after beat 3, and quiet ghost notes off
// the grid, one of them 40 ms after the beat that has no hit of its own.
// From beat 40 there is a fill: ten beats with no hit on the beat and one hit each instead, every
// one 24.6 ms later against the beat than the one before, so that the last is on the "and".
// Followed from hit to hit, the fill leads the count half a beat off the grid.
struct BusySteadyCue
{
    double bpm = 122.0, first = 0.5, seconds = 61.0;
    int beats = 122;
    std::vector<double> truth = steadyBeats (first, bpm, beats);
    std::vector<tt::Onset> onsets;

    BusySteadyCue()
    {
        const double beat = 60.0 / bpm;
        std::vector<std::pair<double, double>> hits;
        for (int k = 0; k < beats; ++k)
        {
            const double t = truth[(size_t) k];
            const int inBar = k % 4, inPattern = k % 8;
            if (k >= 40 && k < 50)
            {
                hits.push_back ({ t + 0.0246 * (k - 39), 0.5 });
                continue;
            }
            if (inPattern != 5)
                hits.push_back ({ t, inBar == 0 ? 0.6 : 0.5 });
            else
                hits.push_back ({ t + 0.040, 0.14 });
            if (inBar == 1 || inBar == 3)
                hits.push_back ({ t + 0.5 * beat, 0.6 });
            if (inBar == 3 || inBar == 1)
                hits.push_back ({ t + 0.75 * beat, 0.35 });
            if (inBar == 2)
                hits.push_back ({ t + 0.25 * beat, 0.4 });
            if (inPattern == 2 || inPattern == 7)
                hits.push_back ({ t + 0.5 * beat - 0.065, 0.12 });
            if (inPattern == 0 || inPattern == 6)
                hits.push_back ({ t + 0.62 * beat, 0.1 });
        }
        const auto audio = makeCueWithLevels (hits, seconds, sr48);
        onsets = tt::findOnsets (audio.data(), audio.size(), sr48);
    }
};

std::string steadyWithSyncopation()
{
    const BusySteadyCue cue;
    tt::Hint hint;
    hint.bpm = 122.0;
    const auto map = tt::mapCue (cue.onsets, cue.seconds, hint);
    expect (map.sections.size() == 1 && map.checks.empty(), text ("%g sections, ", (double) map.sections.size()) + allChecks (map));
    expect (std::abs (map.bpm - cue.bpm) <= 0.02 && std::abs (map.sections.front().bpm - cue.bpm) <= 0.02, text ("tempo %.4f, section %.4f", map.bpm, map.sections.front().bpm));
    expectPinsInOrder (map.pins);
    const tt::BeatGrid grid (map.pins, map.bpm);
    const double worst = worstBeatError (grid, 0.0, cue.truth);
    expect (worst <= 2.0, text ("a mapped beat is %.2f ms out", worst));
    const double tempoError = worstSegmentTempo (map.pins, cue.bpm);
    expect (tempoError <= 0.001, text ("a stretch between two pins runs %.3f percent from 122", tempoError * 100.0));

    // A pin about every four bars, each on a hit, each but the last on beat 1 of a bar, none placed by hand.
    // A bar is passed over only where its beat 1 has no hit: bars 11 to 13, in the fill.
    expect (map.pins.size() >= 8 && map.pins.size() <= 10, text ("%g pins", (double) map.pins.size()));
    for (size_t i = 0; i < map.pins.size(); ++i)
    {
        const auto& pin = map.pins[i];
        expect (! pin.user, text ("the pin at %g s is marked as placed by hand", pin.time));
        expect (nearestOnsetError (map.onsets, pin.time) <= 1.0e-9, text ("the pin at %g s is not on a hit", pin.time));
        if (i + 1 < map.pins.size())
        {
            expect (same (std::fmod (pin.beat, 4.0), 0.0), text ("the pin at beat %g is not on beat 1 of a bar", pin.beat));
            for (double due = pin.beat + 16.0; due < map.pins[i + 1].beat; due += 4.0)
                expect (due >= 40.0 && due < 50.0, text ("no pin on beat %g, %g beats after the pin before it", due, due - pin.beat));
        }
    }

    // With no regular pins: only the pins the tempo needs, and the same grid within the tolerance.
    tt::Options needed;
    needed.pinEveryBars = 0;
    const auto bare = tt::mapCue (cue.onsets, cue.seconds, hint, {}, needed);
    expect (bare.pins.size() >= 2 && bare.pins.size() <= 3, text ("%g pins with regular pins off", (double) bare.pins.size()));
    expect (std::abs (bare.bpm - cue.bpm) <= 0.02 && worstSegmentTempo (bare.pins, cue.bpm) <= 0.001, text ("regular pins off: tempo %.4f", bare.bpm));
    const tt::BeatGrid bareGrid (bare.pins, bare.bpm);
    double moved = 0.0;
    for (double b = 0.0; b < (double) cue.beats; b += 1.0)
        moved = std::max (moved, std::abs (grid.timeAtBeat (b) - bareGrid.timeAtBeat (b)));
    expect (moved <= 0.010, text ("the regular pins move the grid by %.2f ms", moved * 1000.0));

    // Regular pins every two bars.
    tt::Options often;
    often.pinEveryBars = 2;
    const auto dense = tt::mapCue (cue.onsets, cue.seconds, hint, {}, often);
    for (size_t i = 1; i < dense.pins.size(); ++i)
        for (double due = dense.pins[i - 1].beat + 8.0; due < dense.pins[i].beat; due += 4.0)
            expect (due >= 40.0 && due < 50.0, text ("every two bars: no pin on beat %g, %g beats after the pin before it", due, due - dense.pins[i - 1].beat));
    expect (dense.pins.size() > map.pins.size(), text ("every two bars: %g pins", (double) dense.pins.size()));
    return text ("%.4f BPM, %g pins (%g with regular pins off)", map.bpm, (double) map.pins.size(), (double) bare.pins.size())
           + text (", beats within %.2f ms, stretches within %.3f percent of 122", worst, tempoError * 100.0);
}

std::string realDrift()
{
    // 8 bars at 122 BPM, 16 bars speeding up evenly to 124, 8 bars at 124. A hit on every beat and a quieter one between.
    std::vector<double> truth;
    double t = 0.5;
    for (int k = 0; k < 128; ++k)
    {
        truth.push_back (t);
        const double along = std::clamp ((k - 32) / 64.0, 0.0, 1.0);
        t += 60.0 / 122.0 + along * (60.0 / 124.0 - 60.0 / 122.0);
    }
    std::vector<std::pair<double, double>> hits;
    for (size_t k = 0; k + 1 < truth.size(); ++k)
    {
        hits.push_back ({ truth[k], 0.6 });
        hits.push_back ({ 0.5 * (truth[k] + truth[k + 1]), 0.25 });
    }
    hits.push_back ({ truth.back(), 0.6 });
    const double seconds = truth.back() + 1.0;
    const auto audio = makeCueWithLevels (hits, seconds, sr48);
    const auto onsets = tt::findOnsets (audio.data(), audio.size(), sr48);

    std::string detail;
    for (int every : { 4, 0 })
    {
        tt::Hint hint;
        hint.bpm = 122.0;
        tt::Options options;
        options.pinEveryBars = every;
        const auto map = tt::mapCue (onsets, seconds, hint, {}, options);
        expect (map.sections.size() == 1, text ("%g sections, ", (double) map.sections.size()) + allChecks (map));
        expectPinsInOrder (map.pins);
        const tt::BeatGrid grid (map.pins, map.bpm);
        const double worst = worstBeatError (grid, 0.0, truth);
        expect (worst <= 5.0, text ("regular pins every %g bars: a mapped beat is %.2f ms out", (double) every, worst));
        expect (std::abs (grid.bpmAtTime (2.0) - 122.0) <= 0.1 && std::abs (grid.bpmAtTime (truth[120]) - 124.0) <= 0.1,
                text ("%.3f BPM at the start, %.3f at the end", grid.bpmAtTime (2.0), grid.bpmAtTime (truth[120])));
        detail += text ("pins every %g bars: %g pins, beats within %.2f ms  ", (double) every, (double) map.pins.size(), worst);
    }
    return detail;
}

std::string oneDisplacedHit()
{
    // 64 beats at 122 BPM with the hit of beat 30 played 14 ms late: the grid stays where it was.
    auto times = steadyBeats (0.5, 122.0, 64);
    const auto truth = times;
    times[30] += 0.014;
    const auto audio = makeCue (times, truth.back() + 1.0, sr48);
    const auto onsets = tt::findOnsets (audio.data(), audio.size(), sr48);
    tt::Hint hint;
    hint.bpm = 122.0;
    std::string detail;
    for (int every : { 4, 0 })
    {
        tt::Options options;
        options.pinEveryBars = every;
        const auto map = tt::mapCue (onsets, truth.back() + 1.0, hint, {}, options);
        expect (map.sections.size() == 1 && map.checks.empty(), text ("%g sections, ", (double) map.sections.size()) + allChecks (map));
        for (const auto& pin : map.pins)
            expect (std::abs (pin.beat - 30.0) > 0.5, text ("a pin was placed on the late hit, at %g s", pin.time));
        if (every == 0)
            expect (map.pins.size() >= 2 && map.pins.size() <= 3, text ("%g pins with regular pins off", (double) map.pins.size()));
        const tt::BeatGrid grid (map.pins, map.bpm);
        const double worst = worstBeatError (grid, 0.0, truth);
        expect (worst <= 2.0 && worstSegmentTempo (map.pins, 122.0) <= 0.001 && std::abs (map.bpm - 122.0) <= 0.02,
                text ("a mapped beat is %.2f ms out, tempo %.4f", worst, map.bpm));
        detail += text ("pins every %g bars: %g pins, beats within %.2f ms  ", (double) every, (double) map.pins.size(), worst);
    }
    return detail;
}

} // namespace

int main()
{
    int failed = 0, total = 0;
    auto test = [&] (const char* name, const std::function<std::string()>& body)
    {
        ++total;
        failed += run (name, body);
    };
    test ("steady 120 BPM at 44.1 kHz", [] { return steadyTempo (44100.0); });
    test ("steady 120 BPM at 48 kHz", [] { return steadyTempo (48000.0); });
    test ("steady 120 BPM at 96 kHz", [] { return steadyTempo (96000.0); });
    test ("stop for 2.9 s and restart", gapRestart);
    test ("long gap, half-beat bridge", longGapAndHalfBeat);
    test ("90 ms phase jump at bar 9", phaseJump);
    test ("tempo hint 3 BPM out, and none", roughHint);
    test ("user pins", userPins);
    test ("BeatGrid and formatBarBeat", gridAndPositions);
    test ("renderClick", click);
    test ("renderClick across a restart", clickAcrossRestart);
    test ("hits on every second beat", sparseHits);
    test ("tempo hint far out", wrongTempo);
    test ("awkward first hit, tempo and length", awkwardInput);
    test ("tempo with a user pin off the count", userPinTempo);
    test ("user pins that conflict", conflictingUserPins);
    test ("user restart placed off its hit", userRestartOffHit);
    test ("findOnsets on audio with no hits, and on a stem", onsetsOnAwkwardAudio);
    test ("steady 122 BPM with syncopation and ghost notes", steadyWithSyncopation);
    test ("tempo drifting from 122 to 124 BPM", realDrift);
    test ("one hit played late", oneDisplacedHit);
    std::printf ("%d of %d tests passed\n", total - failed, total);
    return failed;
}
