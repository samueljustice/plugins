// Measures the dialogue sidechain on real material: a mix and the dialogue stem it was made with.
//
//   dehonk_dialogue MIX.wav DX.wav [options]
//
// The mix is rendered twice at the same settings, without a key and with the stem as the key and
// Dialogue on, and the stem once on its own, only to read its level per band through the core's
// own band bank. Every band of every frame inside a range that is switched on is then sorted by
// how the stem stands against the mix there (both from the unkeyed renders, so the sorting does
// not use anything the dialogue logic works out):
//
//   the dialogue's   the stem is within 3 dB of the mix (it is half the band's power or more)
//   mixed            between the two
//   not dialogue     the stem is 12 dB or more under the mix
//
// and the cut is averaged over each kind, with and without the key. With the key the cut in the
// bands that are the dialogue's should fall to near nothing and the cut in the bands that are
// not should stay.
//
// Options:
//   --threshold DB       (-16)   as the plug-in's Threshold; -60 = always
//   --all-ranges                 Woof, Tub, Fog and Bark on (default: the plug-in's defaults, Tub and Fog)
//   --depth X            (0.5)
//   --amount X           (1)     Params::dialogueAmount
//   --key-gain DB        (0)     the stem is sent to the sidechain this much up or down
//   --key-mono-sum               the stem's two channels are added into a mono sidechain (6 dB up for a
//                                voice in the middle), as a stereo stem sent to a mono key input arrives
//   --csv                        one line of figures, comma separated, after the name
//   --set NAME=VALUE             a dehonk::Tuning constant
#include "Audio.h"
#include "Render.h"
#include "TuningFields.h"
#include "Wav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace dh;

namespace
{
struct Tally
{
    double without = 0.0, with = 0.0;
    std::size_t n = 0;
    void add (double a, double b) { without += a; with += b; ++n; }
    double meanWithout() const { return n > 0 ? without / (double) n : 0.0; }
    double meanWith() const { return n > 0 ? with / (double) n : 0.0; }
};
}

int main (int argc, char** argv)
{
    std::string mixPath, dxPath;
    dehonk::Params params;
    dehonk::Tuning tuning;
    double keyGainDb = 0.0;
    bool monoSum = false, csv = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] () -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--threshold") params.thresholdDb = (float) std::atof (next().c_str());
        else if (a == "--depth") params.depth = (float) std::atof (next().c_str());
        else if (a == "--amount") params.dialogueAmount = (float) std::atof (next().c_str());
        else if (a == "--all-ranges") { for (auto& m : params.moduleOn) m = true; }
        else if (a == "--key-gain") keyGainDb = std::atof (next().c_str());
        else if (a == "--key-mono-sum") monoSum = true;
        else if (a == "--csv") csv = true;
        else if (a == "--set")
        {
            const std::string kv = next();
            const auto eq = kv.find ('=');
            std::string why;
            if (eq == std::string::npos || ! setTuningField (tuning, kv.substr (0, eq), kv.substr (eq + 1), why))
            {
                std::fprintf (stderr, "dehonk_dialogue: --set %s: %s\n", kv.c_str(), why.c_str());
                return 2;
            }
        }
        else if (mixPath.empty()) mixPath = a;
        else if (dxPath.empty()) dxPath = a;
        else { std::fprintf (stderr, "dehonk_dialogue: unexpected argument %s\n", a.c_str()); return 2; }
    }
    if (dxPath.empty())
    {
        std::fprintf (stderr, "Usage: dehonk_dialogue MIX.wav DX.wav [--threshold DB] [--all-ranges] [--depth X] [--amount X] [--key-gain DB] [--key-mono-sum] [--csv] [--set NAME=VALUE]\n");
        return 2;
    }

    Audio mix, dx;
    std::string err;
    if (! readWav (mixPath, mix, err) || ! readWav (dxPath, dx, err))
    {
        std::fprintf (stderr, "dehonk_dialogue: %s\n", err.c_str());
        return 1;
    }
    if (mix.sampleRate != dx.sampleRate || mix.numChannels() < 1 || dx.numChannels() < 1)
    {
        std::fprintf (stderr, "dehonk_dialogue: the mix and the stem must have the same sample rate\n");
        return 1;
    }
    if (mix.numChannels() > 2) mix.ch.resize (2);
    if (dx.numChannels() > 2) dx.ch.resize (2);
    while (dx.numChannels() < mix.numChannels()) dx.ch.push_back (dx.ch[0]);
    const std::size_t n = std::min (mix.length(), dx.length());
    for (auto& c : mix.ch) c.resize (n);
    for (auto& c : dx.ch) c.resize (n);

    // The key as it is sent.
    Audio key = dx;
    if (monoSum && key.numChannels() == 2)
    {
        for (std::size_t i = 0; i < n; ++i)
            key.ch[0][i] += key.ch[1][i];
        key.ch.resize (1);
    }
    const float keyGain = (float) dbToAmp (keyGainDb);
    for (auto& c : key.ch)
        for (auto& s : c)
            s *= keyGain;

    RenderSpec spec;
    spec.params = params;
    spec.useTuning = true;
    spec.tuning = tuning;
    spec.blockSize = 480;
    spec.collectFrames = true;
    const auto plain = render (mix, spec);
    RenderSpec keyed = spec;
    keyed.params.dialogueOn = true;
    keyed.key = &key;
    const auto withKey = render (mix, keyed);
    RenderSpec listen = spec;
    listen.params.depth = 0.0f;
    const auto stem = render (dx, listen);

    const std::size_t frames = std::min ({ plain.frames.size(), withKey.frames.size(), stem.frames.size() });
    Tally dialogue, mixed, other, dialogueOpen, otherOpen;
    std::vector<float> trims;
    std::size_t speakingFrames = 0;
    double deepestDialogueWithout = 0.0, deepestDialogueWith = 0.0;
    std::size_t deepestCount = 0;
    double stemTop = -200.0;
    for (std::size_t f = 0; f < frames; ++f)
        for (int k = 0; k < plain.numBands; ++k)
            stemTop = std::max (stemTop, (double) stem.frames[f].levelDb[(std::size_t) k]);

    for (std::size_t f = 0; f < frames; ++f)
    {
        const auto& a = plain.frames[f];
        const auto& b = withKey.frames[f];
        const auto& s = stem.frames[f];
        if (a.timeSec < 1.0)
            continue;
        bool speaking = false;
        double deepA = 0.0, deepB = 0.0;
        bool anyDialogueBand = false;
        for (int k = 0; k < plain.numBands; ++k)
        {
            const int m = moduleAt ((double) plain.bandHz[(std::size_t) k], params);
            if (m < 0 || ! params.moduleOn[m])
                continue;
            const double mixDb = a.levelDb[(std::size_t) k], dxDb = s.levelDb[(std::size_t) k];
            const double cutA = a.cutDb[(std::size_t) k], cutB = b.cutDb[(std::size_t) k];
            const bool audible = dxDb > std::max (-70.0, stemTop - 45.0);
            speaking = speaking || audible;
            if (audible && dxDb >= mixDb - 3.0)
            {
                dialogue.add (cutA, cutB);
                if (cutA >= 0.5) dialogueOpen.add (cutA, cutB);
                deepA = std::max (deepA, cutA);
                deepB = std::max (deepB, cutB);
                anyDialogueBand = true;
            }
            else if (dxDb <= mixDb - 12.0)
            {
                other.add (cutA, cutB);
                if (cutA >= 0.5) otherOpen.add (cutA, cutB);
            }
            else
                mixed.add (cutA, cutB);
        }
        if (anyDialogueBand)
        {
            deepestDialogueWithout += deepA;
            deepestDialogueWith += deepB;
            ++deepestCount;
        }
        if (speaking)
        {
            ++speakingFrames;
            trims.push_back (b.dialogueTrimDb);
        }
    }
    std::sort (trims.begin(), trims.end());
    const auto trimAt = [&trims] (double q) { return trims.empty() ? 0.0 : (double) trims[(std::size_t) (q * (double) (trims.size() - 1))]; };
    const double total = (double) (dialogue.n + mixed.n + other.n);
    const std::string name = mixPath.substr (mixPath.find_last_of ('/') == std::string::npos ? 0 : mixPath.find_last_of ('/') + 1);

    if (csv)
    {
        std::printf ("%s,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f\n", name.c_str(), mix.seconds(),
                     total > 0 ? (double) dialogue.n / total : 0.0, total > 0 ? (double) other.n / total : 0.0,
                     dialogue.meanWithout(), dialogue.meanWith(), dialogueOpen.meanWithout(), dialogueOpen.meanWith(),
                     mixed.meanWithout(), mixed.meanWith(), other.meanWithout(), other.meanWith(), otherOpen.meanWithout(), otherOpen.meanWith(),
                     deepestCount > 0 ? deepestDialogueWithout / (double) deepestCount : 0.0, deepestCount > 0 ? deepestDialogueWith / (double) deepestCount : 0.0,
                     trimAt (0.1), trimAt (0.5), trimAt (0.9));
        return 0;
    }

    std::printf ("%s (%.1f s), threshold %.0f dBFS, key %+.1f dB%s\n", name.c_str(), mix.seconds(), (double) params.thresholdDb, keyGainDb, monoSum ? ", summed to mono" : "");
    std::printf ("  bands of frames in the ranges that are on: %.1f%% the dialogue's, %.1f%% mixed, %.1f%% not dialogue; the stem is audible in %.1f%% of frames\n",
                 total > 0 ? 100.0 * (double) dialogue.n / total : 0.0, total > 0 ? 100.0 * (double) mixed.n / total : 0.0,
                 total > 0 ? 100.0 * (double) other.n / total : 0.0, frames > 0 ? 100.0 * (double) speakingFrames / (double) frames : 0.0);
    std::printf ("  mean cut, no key -> keyed (dB):\n");
    std::printf ("    the dialogue's   %.3f -> %.3f   (where a cut was open: %.2f -> %.2f, %zu bands of frames)\n", dialogue.meanWithout(), dialogue.meanWith(),
                 dialogueOpen.meanWithout(), dialogueOpen.meanWith(), dialogueOpen.n);
    std::printf ("    mixed            %.3f -> %.3f\n", mixed.meanWithout(), mixed.meanWith());
    std::printf ("    not dialogue     %.3f -> %.3f   (where a cut was open: %.2f -> %.2f, %zu bands of frames)\n", other.meanWithout(), other.meanWith(),
                 otherOpen.meanWithout(), otherOpen.meanWith(), otherOpen.n);
    std::printf ("  deepest cut in a band that is the dialogue's, mean over the frames that have one: %.2f -> %.2f dB\n",
                 deepestCount > 0 ? deepestDialogueWithout / (double) deepestCount : 0.0, deepestCount > 0 ? deepestDialogueWith / (double) deepestCount : 0.0);
    std::printf ("  the key's trim while the stem is audible: %.2f dB at the 10th percentile, %.2f median, %.2f at the 90th\n", trimAt (0.1), trimAt (0.5), trimAt (0.9));
    return 0;
}
