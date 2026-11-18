// dehonk_render: renders a WAV file through the De-Honk DSP core.
//   dehonk_render in.wav out.wav [options]
// See dehonk/Tests/README.md.

#include "Analysis.h"
#include "Audio.h"
#include "Measures.h"
#include "Render.h"
#include "TuningFields.h"
#include "Wav.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace dh;

namespace
{
void usage()
{
    const dehonk::Params d;
    std::printf (
        "Usage: dehonk_render in.wav out.wav [options]\n"
        "\n"
        "Renders a WAV of 1 to 12 channels (16/24/32-bit PCM or 32/64-bit float) through the De-Honk\n"
        "core at the file's own sample rate. The output is 32-bit float, lined up sample for\n"
        "sample with the input (the core's latency is removed).\n"
        "\n"
        "Parameters (defaults are the plug-in's):\n"
        "  --depth X         0..1      (%g)\n"
        "  --sharpness X     0..1      (%g)  0 broad, 1 narrow\n"
        "  --speed X         0..1      (%g)  0 slow, 1 fast: how quickly a notch goes in\n"
        "  --release MS      50..1500  (%g)  how slowly a notch lets go\n"
        "  --max-cut DB      3..12     (%g)  no notch goes deeper\n"
        "  --impact-guard X  0..1      (%g)  how far the cuts are kept out of a broadband impact while it sounds\n"
        "  --woof 0|1                  (%d)  module switches\n"
        "  --tub 0|1                   (%d)\n"
        "  --fog 0|1                   (%d)\n"
        "  --bark 0|1                  (%d)\n"
        "  --snap 0|1                 (%d)  the Snap module (sharp pops around 2.5 kHz)\n"
        "  --edges A,B,C,D,E  Hz       (%g,%g,%g,%g,%g)  module m spans edge m to edge m+1\n"
        "                    limits    %g-%g, %g-%g, %g-%g, %g-%g, %g-%g\n"
        "  --threshold DB    -60..0    (%g)  acts only while the programme is louder; -60 = always\n"
        "  --link X          0..1      (%g)  how far a channel follows what it shares (no control reaches anything but 1)\n"
        "  --link-mode M     linked, grouped or unlinked (linked)\n"
        "  --detection D     fast or steady (fast): steady compares each band's level over about a second\n"
        "  --range-timing R=A,REL      unlock range R (woof, tub, fog or bark) with attack A ms (2..100)\n"
        "                    and release REL ms (50..1500); repeatable. Without it a range follows Speed and Release\n"
        "  --key FILE.wav    a sidechain, in time with the input (its first two channels are used)\n"
        "  --dialogue 0|1    (0)  the sidechain is the dialogue stem: bands it accounts for are left alone\n"
        "  --dialogue-amount X  0..1   (1)\n"
        "  --lookahead MS    0, 5, 10 or 20 (%g)\n"
        "  --mix X           0..1      (%g)\n"
        "  --trim DB         -12..12   (%g)\n"
        "  --solo-honk       output only what is being removed\n"
        "  --diff            output the untreated signal (analysis keeps running)\n"
        "  --bypass          output the untreated signal\n"
        "\n"
        "Harness options:\n"
        "  --layout NAME     the speakers of a WAV with more than two channels, in WAV order:\n"
        "                    5.1 (L R C LFE Ls Rs), 7.1 (L R C LFE Lrs Rrs Lss Rss),\n"
        "                    5.1.4 (5.1 then Ltf Rtf Ltr Rtr), 7.1.4 (7.1 then Ltf Rtf Ltr Rtr).\n"
        "                    Without it, no channel is an LFE and channels 0 and 1 are left and right\n"
        "  --lfe N           channel N (from 0) is the LFE, the rest as without --layout\n"
        "  --block N         block size (512)\n"
        "  --set NAME=VALUE  override a dehonk::Tuning field; repeatable\n"
        "  --metrics FILE    CSV: one row per band (levels in and out from the harness's own FFT\n"
        "                    analysis, the band's module, cut statistics), one row per module and\n"
        "                    one for Snap (its band's levels, its cut over time, how many events)\n"
        "  --frames FILE     CSV of the display frames over time, with each module's deepest notch\n"
        "                    and Snap's cut. With --snap 1 every frame the core writes is read\n"
        "                    (blocks are processed in pieces of %d samples), since Snap's figure\n"
        "                    covers only the time since the core's previous frame\n"
        "  --frame-levels    also write each band's input level (dBFS) in the --frames file\n"
        "  --notch-log FILE  CSV, one row per notch: when it opened (cut above 1 dB) and closed (under\n"
        "                    0.5 dB for 100 ms), the band, its note and cents from equal temperament\n"
        "                    (A = 440), its deepest cut, and the rank of its note in the core's key\n"
        "                    profile when it opened (1 = strongest; empty before Tuning::keySeconds).\n"
        "                    Every frame the core writes is read for it (as with --snap 1)\n"
        "  --labels FILE     CSV of marked moments: time_seconds,kind with kind one of impact, tonal,\n"
        "                    dialogue_start, dialogue_end, music_start, music_end. With --metrics,\n"
        "                    adds a row per impact (Body, Tail, Front, SPEC 13.2) and per dialogue or\n"
        "                    music stretch (cut and level change from 110 to 400 Hz)\n"
        "  --no-ftz          do not set flush-to-zero (the plug-in runs with it set)\n"
        "  --print-tuning    print the tuning in use\n"
        "\n"
        "Tuning fields: %s\n",
        (double) d.depth, (double) d.sharpness, (double) d.speed, (double) d.releaseMs, (double) d.maxCutDb, (double) d.impactGuard,
        (int) d.moduleOn[dehonk::Woof], (int) d.moduleOn[dehonk::Tub],
        (int) d.moduleOn[dehonk::Fog], (int) d.moduleOn[dehonk::Bark], (int) d.snapOn, (double) d.edgeHz[0], (double) d.edgeHz[1], (double) d.edgeHz[2],
        (double) d.edgeHz[3], (double) d.edgeHz[4], (double) dehonk::kEdgeMinHz[0], (double) dehonk::kEdgeMaxHz[0],
        (double) dehonk::kEdgeMinHz[1], (double) dehonk::kEdgeMaxHz[1], (double) dehonk::kEdgeMinHz[2], (double) dehonk::kEdgeMaxHz[2],
        (double) dehonk::kEdgeMinHz[3], (double) dehonk::kEdgeMaxHz[3], (double) dehonk::kEdgeMinHz[4], (double) dehonk::kEdgeMaxHz[4],
        (double) d.thresholdDb, (double) d.link, (double) d.lookaheadMs, (double) d.mix, (double) d.trimDb, kFrameSubBlock, tuningFieldNames().c_str());
}

bool parseFloat (const std::string& s, float& out)
{
    char* end = nullptr;
    out = std::strtof (s.c_str(), &end);
    return end != s.c_str() && *end == '\0' && std::isfinite (out);
}

struct RangeCheck
{
    const char* name;
    float value, lo, hi;
};

struct Label
{
    double sec = 0.0;
    std::string kind;
};

std::string trim (const std::string& x)
{
    const auto a = x.find_first_not_of (" \t\r\n\"");
    if (a == std::string::npos)
        return {};
    const auto b = x.find_last_not_of (" \t\r\n\"");
    return x.substr (a, b - a + 1);
}

/** time_seconds,kind per line; a header line, blank lines and lines starting with # are skipped. */
bool readLabels (const std::string& path, std::vector<Label>& out, std::string& err)
{
    std::ifstream f (path);
    if (! f)
    {
        err = "cannot read " + path;
        return false;
    }
    static const char* const kinds[] = { "impact", "tonal", "dialogue_start", "dialogue_end", "music_start", "music_end" };
    std::string line;
    int lineNo = 0;
    while (std::getline (f, line))
    {
        ++lineNo;
        const std::string l = trim (line);
        if (l.empty() || l[0] == '#')
            continue;
        const auto comma = l.find (',');
        if (comma == std::string::npos)
        {
            err = fmt ("%s line %d: expected time_seconds,kind", path.c_str(), lineNo);
            return false;
        }
        const std::string t = trim (l.substr (0, comma));
        std::string kind = trim (l.substr (comma + 1));
        const auto comma2 = kind.find (',');
        if (comma2 != std::string::npos)
            kind = trim (kind.substr (0, comma2));   // anything after a second comma is a note for people
        char* end = nullptr;
        const double v = std::strtod (t.c_str(), &end);
        if (end == t.c_str() || *end != '\0' || ! std::isfinite (v))
        {
            if (lineNo == 1)
                continue;   // header
            err = fmt ("%s line %d: not a time: %s", path.c_str(), lineNo, t.c_str());
            return false;
        }
        bool known = false;
        for (const char* k : kinds)
            known = known || kind == k;
        if (! known)
        {
            err = fmt ("%s line %d: unknown kind %s (impact, tonal, dialogue_start, dialogue_end, music_start, music_end)", path.c_str(), lineNo,
                       kind.c_str());
            return false;
        }
        out.push_back ({ v, kind });
    }
    std::stable_sort (out.begin(), out.end(), [] (const Label& a, const Label& b) { return a.sec < b.sec; });
    return true;
}

/** Pairs of kind_start and kind_end, in time order. A start with no end runs to endSec. */
std::vector<std::pair<double, double>> stretches (const std::vector<Label>& labels, const std::string& kind, double endSec)
{
    std::vector<std::pair<double, double>> v;
    double start = -1.0;
    for (const auto& l : labels)
    {
        if (l.kind == kind + "_start" && start < 0.0)
            start = l.sec;
        else if (l.kind == kind + "_end" && start >= 0.0)
        {
            v.emplace_back (start, l.sec);
            start = -1.0;
        }
    }
    if (start >= 0.0)
        v.emplace_back (start, endSec);
    return v;
}
} // namespace

int main (int argc, char** argv)
{
    std::vector<std::string> args (argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help")
    {
        usage();
        return args.empty() ? 1 : 0;
    }

    std::vector<std::string> positional;
    dehonk::Params params;
    std::vector<std::pair<std::string, std::string>> tuningSets;
    int block = 512;
    std::string metricsPath, framesPath, notchLogPath, labelsPath;
    bool ftz = true, printTuning = false, frameLevels = false;
    std::string layoutName, keyPath;
    int lfeIndex = -1;

    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string& a = args[i];
        auto value = [&] (const char* what) -> std::string {
            if (i + 1 >= args.size())
            {
                std::fprintf (stderr, "dehonk_render: %s needs a value\n", what);
                std::exit (2);
            }
            return args[++i];
        };
        auto number = [&] (const char* what) -> float {
            const std::string v = value (what);
            float f;
            if (! parseFloat (v, f))
            {
                std::fprintf (stderr, "dehonk_render: %s: not a number: %s\n", what, v.c_str());
                std::exit (2);
            }
            return f;
        };
        auto onOff = [&] (const char* what) -> bool {
            const std::string v = value (what);
            if (v == "1" || v == "on")
                return true;
            if (v == "0" || v == "off")
                return false;
            std::fprintf (stderr, "dehonk_render: %s takes 0 or 1, got %s\n", what, v.c_str());
            std::exit (2);
        };

        if (a == "--depth") params.depth = number ("--depth");
        else if (a == "--sharpness") params.sharpness = number ("--sharpness");
        else if (a == "--speed") params.speed = number ("--speed");
        else if (a == "--release") params.releaseMs = number ("--release");
        else if (a == "--max-cut") params.maxCutDb = number ("--max-cut");
        else if (a == "--impact-guard") params.impactGuard = number ("--impact-guard");
        else if (a == "--woof") params.moduleOn[dehonk::Woof] = onOff ("--woof");
        else if (a == "--tub") params.moduleOn[dehonk::Tub] = onOff ("--tub");
        else if (a == "--fog") params.moduleOn[dehonk::Fog] = onOff ("--fog");
        else if (a == "--bark") params.moduleOn[dehonk::Bark] = onOff ("--bark");
        else if (a == "--snap") params.snapOn = onOff ("--snap");
        else if (a == "--edges")
        {
            const std::string v = value ("--edges");
            std::vector<std::string> parts;
            std::size_t p = 0;
            while (true)
            {
                const auto c = v.find (',', p);
                parts.push_back (v.substr (p, c == std::string::npos ? std::string::npos : c - p));
                if (c == std::string::npos)
                    break;
                p = c + 1;
            }
            if (parts.size() != (std::size_t) dehonk::kNumEdges)
            {
                std::fprintf (stderr, "dehonk_render: --edges takes %d comma-separated frequencies, got %s\n", dehonk::kNumEdges, v.c_str());
                return 2;
            }
            for (int e = 0; e < dehonk::kNumEdges; ++e)
                if (! parseFloat (parts[(std::size_t) e], params.edgeHz[e]))
                {
                    std::fprintf (stderr, "dehonk_render: --edges: not a number: %s\n", parts[(std::size_t) e].c_str());
                    return 2;
                }
        }
        else if (a == "--threshold") params.thresholdDb = number ("--threshold");
        else if (a == "--link") params.link = number ("--link");
        else if (a == "--link-mode")
        {
            const std::string v = value ("--link-mode");
            if (v == "linked") params.linkMode = dehonk::LinkMode::linked;
            else if (v == "grouped") params.linkMode = dehonk::LinkMode::grouped;
            else if (v == "unlinked") params.linkMode = dehonk::LinkMode::unlinked;
            else
            {
                std::fprintf (stderr, "dehonk_render: --link-mode takes linked, grouped or unlinked, got %s\n", v.c_str());
                return 2;
            }
        }
        else if (a == "--detection")
        {
            const std::string v = value ("--detection");
            if (v == "fast") params.detection = dehonk::Detection::fast;
            else if (v == "steady") params.detection = dehonk::Detection::steady;
            else
            {
                std::fprintf (stderr, "dehonk_render: --detection takes fast or steady, got %s\n", v.c_str());
                return 2;
            }
        }
        else if (a == "--range-timing")
        {
            const std::string v = value ("--range-timing");
            const auto eq = v.find ('='), comma = v.find (',');
            const char* names[dehonk::kNumModules] = { "woof", "tub", "fog", "bark" };
            int m = -1;
            for (int k = 0; k < dehonk::kNumModules; ++k)
                if (eq != std::string::npos && v.substr (0, eq) == names[k])
                    m = k;
            float attack = 0.0f, release = 0.0f;
            if (m < 0 || comma == std::string::npos || comma < eq || ! parseFloat (v.substr (eq + 1, comma - eq - 1), attack) || ! parseFloat (v.substr (comma + 1), release)
                || attack < 2.0f || attack > 100.0f || release < 50.0f || release > 1500.0f)
            {
                std::fprintf (stderr, "dehonk_render: --range-timing takes RANGE=ATTACK,RELEASE (woof, tub, fog or bark; 2..100 ms; 50..1500 ms), got %s\n", v.c_str());
                return 2;
            }
            params.rangeTimingLocked[m] = false;
            params.rangeAttackMs[m] = attack;
            params.rangeReleaseMs[m] = release;
        }
        else if (a == "--key") keyPath = value ("--key");
        else if (a == "--dialogue") params.dialogueOn = onOff ("--dialogue");
        else if (a == "--dialogue-amount") params.dialogueAmount = number ("--dialogue-amount");
        else if (a == "--lookahead") params.lookaheadMs = number ("--lookahead");
        else if (a == "--mix") params.mix = number ("--mix");
        else if (a == "--trim") params.trimDb = number ("--trim");
        else if (a == "--solo-honk") params.soloHonk = true;
        else if (a == "--diff") params.diff = true;
        else if (a == "--bypass") params.bypass = true;
        else if (a == "--block") block = (int) number ("--block");
        else if (a == "--layout") layoutName = value ("--layout");
        else if (a == "--lfe") lfeIndex = (int) number ("--lfe");
        else if (a == "--metrics") metricsPath = value ("--metrics");
        else if (a == "--frames") framesPath = value ("--frames");
        else if (a == "--frame-levels") frameLevels = true;
        else if (a == "--notch-log") notchLogPath = value ("--notch-log");
        else if (a == "--labels") labelsPath = value ("--labels");
        else if (a == "--no-ftz") ftz = false;
        else if (a == "--print-tuning") printTuning = true;
        else if (a == "--set")
        {
            const std::string kv = value ("--set");
            const auto eq = kv.find ('=');
            if (eq == std::string::npos)
            {
                std::fprintf (stderr, "dehonk_render: --set expects NAME=VALUE, got %s\n", kv.c_str());
                return 2;
            }
            tuningSets.emplace_back (kv.substr (0, eq), kv.substr (eq + 1));
        }
        else if (! a.empty() && a[0] == '-')
        {
            std::fprintf (stderr, "dehonk_render: unknown option %s (try --help)\n", a.c_str());
            return 2;
        }
        else
            positional.push_back (a);
    }

    if (positional.size() != 2)
    {
        std::fprintf (stderr, "dehonk_render: expected in.wav and out.wav (try --help)\n");
        return 2;
    }

    const RangeCheck ranges[] = {
        { "depth", params.depth, 0.0f, 1.0f },          { "sharpness", params.sharpness, 0.0f, 1.0f },
        { "speed", params.speed, 0.0f, 1.0f },          { "link", params.link, 0.0f, 1.0f },
        { "lookahead", params.lookaheadMs, 0.0f, dehonk::kMaxLookaheadMs }, { "mix", params.mix, 0.0f, 1.0f },
        { "trim", params.trimDb, -12.0f, 12.0f },       { "threshold", params.thresholdDb, -60.0f, 0.0f },
        { "release", params.releaseMs, 50.0f, 1500.0f }, { "max-cut", params.maxCutDb, 3.0f, 12.0f },
        { "impact-guard", params.impactGuard, 0.0f, 1.0f }, { "dialogue-amount", params.dialogueAmount, 0.0f, 1.0f },
    };
    for (const auto& r : ranges)
        if (r.value < r.lo || r.value > r.hi)
        {
            std::fprintf (stderr, "dehonk_render: --%s %g is outside %g..%g\n", r.name, (double) r.value, (double) r.lo, (double) r.hi);
            return 2;
        }
    for (int e = 0; e < dehonk::kNumEdges; ++e)
        if (params.edgeHz[e] < dehonk::kEdgeMinHz[e] || params.edgeHz[e] > dehonk::kEdgeMaxHz[e])
        {
            std::fprintf (stderr, "dehonk_render: --edges: edge %d (%g Hz) is outside %g..%g Hz\n", e + 1, (double) params.edgeHz[e],
                          (double) dehonk::kEdgeMinHz[e], (double) dehonk::kEdgeMaxHz[e]);
            return 2;
        }
    if (block < 1 || block > 1 << 20)
    {
        std::fprintf (stderr, "dehonk_render: --block must be 1..1048576\n");
        return 2;
    }

    if (ftz)
        enableFlushDenormals();

    std::string err;
    RenderSpec spec;
    spec.params = params;
    spec.blockSize = block;
    spec.collectFrames = true;
    // Snap's figure in a frame covers only the time since the core's previous frame, so read
    // every one; otherwise a block of 512 samples would show one tick in about eleven
    // (a tick is 48 samples, 1 ms, at 48 kHz).
    // The notch log is read from every frame too, so open and close times are to the tick (1 ms)
    // rather than the block, and a notch shorter than a block is not missed.
    spec.frameEveryTick = params.snapOn || ! notchLogPath.empty();
    if (! layoutName.empty() || lfeIndex >= 0)
    {
        using P = dehonk::Position;
        const P lf = P::leftFront, rf = P::rightFront, c = P::centre, lfe = P::lfe, lo = P::leftOther, ro = P::rightOther;
        std::vector<P> speakers;
        if (layoutName == "5.1") speakers = { lf, rf, c, lfe, lo, ro };
        else if (layoutName == "7.1") speakers = { lf, rf, c, lfe, lo, ro, lo, ro };
        else if (layoutName == "5.1.4") speakers = { lf, rf, c, lfe, lo, ro, lo, ro, lo, ro };
        else if (layoutName == "7.1.4") speakers = { lf, rf, c, lfe, lo, ro, lo, ro, lo, ro, lo, ro };
        else if (! layoutName.empty())
        {
            std::fprintf (stderr, "dehonk_render: --layout takes 5.1, 7.1, 5.1.4 or 7.1.4, got %s\n", layoutName.c_str());
            return 2;
        }
        spec.useLayout = true;
        spec.layout = dehonk::ChannelLayout::withLfe (lfeIndex);
        for (std::size_t k = 0; k < speakers.size(); ++k)
            spec.layout.position[k] = speakers[k];
        // The overhead speakers, for --link-mode grouped: channels 6 to 9 of 5.1.4 and 7.1.4 as the
        // plug-in is handed them (L R C LFE Ls Rs + four tops; L R C LFE Lss Rss + four tops + Lrs Rrs).
        if (layoutName == "5.1.4" || layoutName == "7.1.4")
            for (std::size_t k = 6; k < 10; ++k)
                spec.layout.group[k] = dehonk::Group::height;
    }
    {
        dehonk::Core probe;
        spec.tuning = probe.tuning();
    }
    for (const auto& [name, v] : tuningSets)
    {
        if (! setTuningField (spec.tuning, name, v, err))
        {
            std::fprintf (stderr, "dehonk_render: %s\nTuning fields: %s\n", err.c_str(), tuningFieldNames().c_str());
            return 2;
        }
        spec.useTuning = true;
    }
    if (printTuning)
        std::printf ("tuning: %s\n", describeTuning (spec.tuning).c_str());

    std::vector<Label> labels;
    if (! labelsPath.empty() && ! readLabels (labelsPath, labels, err))
    {
        std::fprintf (stderr, "dehonk_render: %s\n", err.c_str());
        return 2;
    }

    Audio in;
    WavFormat inFormat;
    if (! readWav (positional[0], in, err, &inFormat))
    {
        std::fprintf (stderr, "dehonk_render: %s\n", err.c_str());
        return 1;
    }
    if (in.numChannels() < 1 || in.numChannels() > dehonk::kMaxChannels)
    {
        std::fprintf (stderr, "dehonk_render: %s has %d channels; De-Honk takes 1 to %d\n", positional[0].c_str(), in.numChannels(), dehonk::kMaxChannels);
        return 1;
    }
    if (! layoutName.empty())
    {
        const int want = layoutName == "5.1" ? 6 : layoutName == "7.1" ? 8 : layoutName == "5.1.4" ? 10 : 12;
        if (in.numChannels() != want)
        {
            std::fprintf (stderr, "dehonk_render: --layout %s needs %d channels, %s has %d\n", layoutName.c_str(), want, positional[0].c_str(), in.numChannels());
            return 1;
        }
    }

    Audio key;
    if (! keyPath.empty())
    {
        if (! readWav (keyPath, key, err))
        {
            std::fprintf (stderr, "dehonk_render: %s\n", err.c_str());
            return 1;
        }
        if (key.sampleRate != in.sampleRate || key.numChannels() < 1)
        {
            std::fprintf (stderr, "dehonk_render: --key %s must have the input's sample rate (%.0f Hz)\n", keyPath.c_str(), in.sampleRate);
            return 1;
        }
        spec.key = &key;
    }

    const RenderResult r = render (in, spec);

    if (! allFinite (r.out))
        std::fprintf (stderr, "dehonk_render: warning: output contains NaN or infinity\n");

    if (! writeWavFloat (positional[1], r.out, err))
    {
        std::fprintf (stderr, "dehonk_render: %s\n", err.c_str());
        return 1;
    }

    // Independent level analysis of input and output.
    const Spectrum sIn = powerSpectrum (in, 0, in.length());
    const Spectrum sOut = powerSpectrum (r.out, 0, r.out.length());
    const double nyq = in.sampleRate * 0.5;
    auto change = [&] (double lo, double hi) { return sOut.bandDb (lo, hi) - sIn.bandDb (lo, hi); };

    // The span of the modules that are on, from the bottom of the lowest to the top of the highest.
    const auto edges = clampedEdges (params);
    int lowestOn = -1, highestOn = -1;
    for (int m = 0; m < dehonk::kNumModules; ++m)
        if (params.moduleOn[m])
        {
            lowestOn = lowestOn < 0 ? m : lowestOn;
            highestOn = m;
        }
    const double spanLo = lowestOn >= 0 ? (double) edges[(std::size_t) lowestOn] : nyq;
    const double spanHi = highestOn >= 0 ? (double) edges[(std::size_t) highestOn + 1] : nyq;

    const int nb = r.numBands;
    std::vector<double> meanCut ((std::size_t) nb, 0.0), maxCut ((std::size_t) nb, 0.0), over1 ((std::size_t) nb, 0.0);
    double modMean[dehonk::kNumModules] {}, modMax[dehonk::kNumModules] {}, modOver1[dehonk::kNumModules] {};
    double meanTotal = 0.0, maxTotal = 0.0;
    double clickMean = 0.0, clickMax = 0.0, clickOver1 = 0.0, clickOver3 = 0.0;
    int clickEvents = 0;
    bool inEvent = false;
    for (const auto& f : r.frames)
    {
        const double cc = f.snapCutDb;
        clickMean += cc;
        clickMax = std::max (clickMax, cc);
        clickOver1 += cc > 1.0 ? 1.0 : 0.0;
        clickOver3 += cc > 3.0 ? 1.0 : 0.0;
        clickEvents += (cc > 1.0 && ! inEvent) ? 1 : 0;   // an event is a run of frames above 1 dB
        inEvent = cc > 1.0;
        meanTotal += f.totalCutDb;
        maxTotal = std::max (maxTotal, (double) f.totalCutDb);
        for (int k = 0; k < nb && k < dehonk::kMaxBands; ++k)
        {
            const double c = f.cutDb[(std::size_t) k];
            meanCut[(std::size_t) k] += c;
            maxCut[(std::size_t) k] = std::max (maxCut[(std::size_t) k], c);
            over1[(std::size_t) k] += c > 1.0 ? 1.0 : 0.0;
        }
        for (int m = 0; m < dehonk::kNumModules; ++m)
        {
            const double c = f.moduleCutDb[(std::size_t) m];
            modMean[m] += c;
            modMax[m] = std::max (modMax[m], c);
            modOver1[m] += c > 1.0 ? 1.0 : 0.0;
        }
    }
    const double nf = std::max<double> (1.0, (double) r.frames.size());
    meanTotal /= nf;
    clickMean /= nf;

    if (! metricsPath.empty())
    {
        std::ofstream m (metricsPath);
        if (! m)
        {
            std::fprintf (stderr, "dehonk_render: cannot write %s\n", metricsPath.c_str());
            return 1;
        }
        // One header for both kinds of row, so files can be joined. A band row's module is the
        // module whose range holds the band's centre, on or off; a module row's levels are over
        // its whole range and its cut figures are its deepest notch over time.
        // The click row's levels are over Snap's band (Tuning::clickLowHz to clickHighHz), its cut
        // figures come from DisplayFrame::snapCutDb, and it alone fills the last two columns.
        // With --labels, impact, dialogue and music rows fill the last columns (blank in the others).
        m << "row,band_hz,module,module_on,in_db,out_db,change_db,mean_cut_db,max_cut_db,pct_time_cut_over_1db,pct_time_cut_over_3db,events"
          << ",time_s,end_s,body_db,tail_in_ms,tail_out_ms,tail_change_pct,tail_fall_db,front_db,mean_cut_110_400_db,max_cut_110_400_db,notches\n";
        const std::string noLabelCols = ",,,,,,,,,,,";
        for (int k = 0; k < nb && k < dehonk::kMaxBands; ++k)
        {
            const double hz = r.bandHz[(std::size_t) k];
            if (hz >= nyq)
                continue;
            const int mod = moduleAt (hz, params);
            const double li = sIn.centredDb (hz, 1.0 / 24.0);
            const double lo = sOut.centredDb (hz, 1.0 / 24.0);
            m << fmt ("band,%.2f,%s,%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,,%s\n", hz, moduleName (mod), mod < 0 ? "" : (params.moduleOn[mod] ? "1" : "0"),
                      li, lo, lo - li, meanCut[(std::size_t) k] / nf, maxCut[(std::size_t) k], 100.0 * over1[(std::size_t) k] / nf, noLabelCols.c_str());
        }
        for (int mod = 0; mod < dehonk::kNumModules; ++mod)
        {
            const double lo = edges[(std::size_t) mod], hi = std::min ((double) edges[(std::size_t) mod + 1], nyq);
            const double li = sIn.bandDb (lo, hi), lout = sOut.bandDb (lo, hi);
            m << fmt ("module,,%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,,%s\n", moduleName (mod), params.moduleOn[mod] ? 1 : 0, li, lout, lout - li,
                      modMean[mod] / nf, modMax[mod], 100.0 * modOver1[mod] / nf, noLabelCols.c_str());
        }
        {
            const double lo = spec.tuning.clickLowHz, hi = std::min ((double) spec.tuning.clickHighHz, nyq);
            const double li = sIn.bandDb (lo, hi), lout = sOut.bandDb (lo, hi);
            m << fmt ("click,,Snap,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%d%s\n", params.snapOn ? 1 : 0, li, lout, lout - li, clickMean, clickMax,
                      100.0 * clickOver1 / nf, 100.0 * clickOver3 / nf, clickEvents, noLabelCols.c_str());
        }

        if (! labels.empty())
        {
            // The deepest cut between 110 and 400 Hz in each frame, for the stretches.
            auto cut110to400 = [&] (const Frame& fr) {
                double c = 0.0;
                for (int k = 0; k < nb && k < dehonk::kMaxBands; ++k)
                    if (r.bandHz[(std::size_t) k] >= 110.0 && r.bandHz[(std::size_t) k] <= 400.0)
                        c = std::max (c, (double) fr.cutDb[(std::size_t) k]);
                return c;
            };
            const BandPair bp = bandPair (in, r.out, 110.0, 400.0);
            std::vector<std::size_t> onsets;
            for (const auto& l : labels)
                if (l.kind == "impact" && l.sec >= 0.0 && l.sec < in.seconds())
                    onsets.push_back ((std::size_t) std::llround (l.sec * in.sampleRate));
            for (std::size_t i = 0; i < onsets.size(); ++i)
            {
                const ImpactFigures f = impactFigures (bp, onsets[i], i + 1 < onsets.size() ? onsets[i + 1] : 0);
                const double t0 = (double) onsets[i] / in.sampleRate;
                // Notches open during the 1.5 s after the onset, and their deepest cut.
                std::vector<double> mx ((std::size_t) nb, 0.0);
                for (const auto& fr : r.frames)
                    if (fr.timeSec >= t0 && fr.timeSec < t0 + 1.5)
                        for (int k = 0; k < nb && k < dehonk::kMaxBands; ++k)
                            mx[(std::size_t) k] = std::max (mx[(std::size_t) k], (double) fr.cutDb[(std::size_t) k]);
                std::string notches;
                for (int k = 0; k < nb && k < dehonk::kMaxBands; ++k)
                    if (mx[(std::size_t) k] > 1.0)
                        notches += fmt ("%s%.0fHz %s %.1fdB", notches.empty() ? "" : " ", (double) r.bandHz[(std::size_t) k],
                                        noteOf (r.bandHz[(std::size_t) k]).name().c_str(), mx[(std::size_t) k]);
                m << fmt ("impact,,,,,,,,,,,,%.3f,%.3f,%.3f,%s,%s,%s,%.1f,%.3f,,,%s\n", t0, t0 + 1.5, f.bodyDb,
                          f.tailInMs > 0.0 ? fmt ("%.0f", f.tailInMs).c_str() : "",
                          f.tailOutMs > 0.0 ? fmt ("%.0f", f.tailOutMs).c_str() : "",
                          f.tailMeasurable ? fmt ("%.2f", f.tailChangePct).c_str() : "", std::max (0.0, f.fallDb), f.frontDb, notches.c_str());
            }
            for (const char* kind : { "dialogue", "music" })
                for (const auto& [a, b] : stretches (labels, kind, in.seconds()))
                {
                    double sum = 0.0, mxc = 0.0;
                    std::size_t cnt = 0;
                    for (const auto& fr : r.frames)
                        if (fr.timeSec >= a && fr.timeSec < b)
                        {
                            const double c = cut110to400 (fr);
                            sum += c;
                            mxc = std::max (mxc, c);
                            ++cnt;
                        }
                    const auto ia = (std::size_t) std::llround (std::max (0.0, a) * in.sampleRate);
                    const auto ib = std::min (in.length(), (std::size_t) std::llround (b * in.sampleRate));
                    double ei = 0.0, eo = 0.0;
                    for (int c = 0; c < in.numChannels(); ++c)
                        for (std::size_t i = ia; i < ib; ++i)
                        {
                            ei += bp.in[(std::size_t) c][i] * bp.in[(std::size_t) c][i];
                            eo += bp.out[(std::size_t) c][i] * bp.out[(std::size_t) c][i];
                        }
                    m << fmt ("%s,,,,%s,%s,%s,,,,,,%.3f,%.3f,,,,,,,%.3f,%.3f,\n", kind, ib > ia ? fmt ("%.3f", powerToDb (ei / (double) (ib - ia) / in.numChannels())).c_str() : "",
                              ib > ia ? fmt ("%.3f", powerToDb (eo / (double) (ib - ia) / in.numChannels())).c_str() : "",
                              ib > ia ? fmt ("%.3f", powerToDb (eo / std::max (1e-300, ei))).c_str() : "", a, b,
                              cnt > 0 ? sum / (double) cnt : 0.0, mxc);
                }
        }
    }

    if (! framesPath.empty())
    {
        std::ofstream f (framesPath);
        if (! f)
        {
            std::fprintf (stderr, "dehonk_render: cannot write %s\n", framesPath.c_str());
            return 1;
        }
        f << "time_s,total_cut_db,program_db,activity";
        for (int mod = 0; mod < dehonk::kNumModules; ++mod)
        {
            std::string name = moduleName (mod);
            for (auto& ch : name)
                ch = (char) std::tolower ((unsigned char) ch);
            f << ",cut_" << name;
        }
        f << ",cut_click,guard";
        for (int k = 0; k < nb; ++k)
            f << fmt (",cut_%.1fhz", (double) r.bandHz[(std::size_t) k]);
        if (frameLevels)
            for (int k = 0; k < nb; ++k)
                f << fmt (",level_%.1fhz", (double) r.bandHz[(std::size_t) k]);
        f << "\n";
        // When every tick was read only for the notch log, write one frame per block as without it,
        // so the file stays the size it would otherwise be.
        const bool thin = spec.frameEveryTick && ! params.snapOn;
        const double blockSec = (double) block / in.sampleRate;
        double lastWritten = -1.0e9;
        for (const auto& fr : r.frames)
        {
            if (thin && fr.timeSec < lastWritten + blockSec - 0.5 / in.sampleRate && &fr != &r.frames.back())
                continue;
            lastWritten = fr.timeSec;
            f << fmt ("%.5f,%.3f,%.2f,%.3f", fr.timeSec, (double) fr.totalCutDb, (double) fr.programDb, (double) fr.activity);
            for (int mod = 0; mod < dehonk::kNumModules; ++mod)
                f << fmt (",%.3f", (double) fr.moduleCutDb[(std::size_t) mod]);
            f << fmt (",%.3f,%.3f", (double) fr.snapCutDb, (double) fr.guard);
            for (int k = 0; k < nb && k < dehonk::kMaxBands; ++k)
                f << fmt (",%.3f", (double) fr.cutDb[(std::size_t) k]);
            if (frameLevels)
                for (int k = 0; k < nb && k < dehonk::kMaxBands; ++k)
                    f << fmt (",%.2f", (double) fr.levelDb[(std::size_t) k]);
            f << "\n";
        }
    }

    if (! notchLogPath.empty())
    {
        std::ofstream f (notchLogPath);
        if (! f)
        {
            std::fprintf (stderr, "dehonk_render: cannot write %s\n", notchLogPath.c_str());
            return 1;
        }
        // A notch opens when its band's cut rises above 1 dB and closes once the cut has stayed
        // under 0.5 dB for 100 ms. Its rank is left empty when it opened before the key profile
        // had Tuning::keySeconds to fill.
        NotchRule rule;
        rule.rankFromSec = (double) spec.tuning.keySeconds;
        f << "open_s,close_s,band_hz,note,cents,deepest_cut_db,pitch_class_rank,module\n";
        for (const auto& e : notchEvents (r, rule))
        {
            const Note note = noteOf (e.hz);
            f << fmt ("%.4f,%.4f,%.2f,%s,%.1f,%.2f,%s,%s\n", e.openSec, e.closeSec, e.hz, note.name().c_str(), std::abs (note.cents) < 0.05 ? 0.0 : note.cents, e.deepestDb,
                      e.pcRankAtOpen > 0 ? fmt ("%d", e.pcRankAtOpen).c_str() : "", moduleName (moduleAt (e.hz, params)));
        }
    }

    std::string modules;
    for (int mod = 0; mod < dehonk::kNumModules; ++mod)
        if (params.moduleOn[mod])
            modules += fmt ("%s%s %.0f-%.0f Hz cut mean %.2f max %.2f dB", modules.empty() ? "" : ", ", moduleName (mod),
                            (double) edges[(std::size_t) mod], (double) edges[(std::size_t) mod + 1], modMean[mod] / nf, modMax[mod]);
    if (params.snapOn)
        modules += fmt ("%sClick cut mean %.2f max %.2f dB, above 3 dB %.2f%% of the time, %d events", modules.empty() ? "" : ", ", clickMean,
                        clickMax, 100.0 * clickOver3 / nf, clickEvents);
    if (modules.empty())
        modules = "no module on";

    std::printf ("%s: %.0f Hz, %d ch, %.2f s, in %d-bit %s%s | latency %d | below %.0f Hz %s dB | %.0f-%.0f Hz %s dB | above %.0f Hz %s dB | deepest notch mean %.2f dB, max %.2f dB | %s\n",
                 positional[0].c_str(), in.sampleRate, in.numChannels(), in.seconds(), inFormat.bitsPerSample,
                 inFormat.isFloat ? "float" : "PCM", inFormat.extensible ? " (extensible)" : "", r.latency,
                 spanLo, dbStr (change (10.0, spanLo)).c_str(), spanLo, spanHi, dbStr (change (spanLo, spanHi)).c_str(), spanHi,
                 dbStr (change (spanHi, nyq)).c_str(), meanTotal, maxTotal, modules.c_str());
    return 0;
}
