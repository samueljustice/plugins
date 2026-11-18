// Measurements on real mixes that the generated test signals cannot stand in for. Each takes a
// stereo mix (and, but for "diff", the dialogue stem it was made with) and prints one line per
// case. Nothing here passes or fails: the figures are for reading, and the ones quoted in SPEC.md
// come from it.
//
//   dehonk_realmix key mix.wav dx.wav [other_dx.wav]
//       the dialogue sidechain with the key as it should be and as it should not: late, early,
//       hot, quiet, the whole mix, the mix without its dialogue, upside down, and (with a fourth
//       file) another mix's stem. Per case: the cut left in the bands the dialogue has where a
//       cut was open without the key, the cut kept in the bands it has not, the gain the key was
//       taken at, how surely it was found in the mix, and how much of the time it was flagged
//       as most of the mix.
//   dehonk_realmix surround mix.wav dx.wav
//       the same stems laid out as 5.1 (the dialogue in the centre, the rest in the fronts and,
//       quieter, the surrounds): what Linked, Grouped and Unlinked leave of each channel's cut.
//   dehonk_realmix snap mix.wav dx.wav
//       Snap with and without the key: how often and how far it cuts in 20 ms windows from 1.8 to
//       3.5 kHz that are mostly dialogue, and in those that have none.
//   dehonk_realmix diff mix.wav
//       Diff's level match: Diff against the treated output in 3 s and 0.4 s windows, and how
//       fast the gain Diff applies moves.
//
// tools/realmix_library.sh runs one of these over the sets in corpus/library/sets.txt.
#include "Audio.h"
#include "Render.h"
#include "Wav.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dh;


static std::vector<float> band (const std::vector<float>& x, double sr)
{
    std::vector<float> y (x.size());
    const double kl = 1.0 - std::exp (-2.0 * M_PI * 900.0 / sr), kh = 1.0 - std::exp (-2.0 * M_PI * 120.0 / sr);
    double l1 = 0, l2 = 0, h1 = 0, h2 = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        l1 += kl * (x[i] - l1); l2 += kl * (l1 - l2);
        h1 += kh * (l2 - h1); h2 += kh * (h1 - h2);
        y[i] = (float) (l2 - 2 * h1 + h2); // (1-LP)^2 highpass on lowpassed
    }
    return y;
}
struct Tally { double a = 0, b = 0; size_t n = 0; void add (double x, double y) { a += x; b += y; ++n; } };

struct Scores { double dlgOpenA, dlgOpenB, othOpenA, othOpenB, trimMed, matchMed, overShare, dxLossA, dxLossB, restLossA, restLossB; size_t nd, no; bool identical; };

static Audio shifted (const Audio& a, long samples) // positive = key late
{
    Audio o = a;
    for (auto& c : o.ch)
    {
        std::vector<float> t (c.size(), 0.0f);
        for (long i = 0; i < (long) c.size(); ++i) { long j = i - samples; if (j >= 0 && j < (long) c.size()) t[(size_t) i] = c[(size_t) j]; }
        c = t;
    }
    return o;
}

// fraction of signal `ref` (band-limited) removed: sum<r,ref>/sum<ref,ref>, in dB of remaining gain
static double lossDb (const Audio& in, const Audio& out, const Audio& ref)
{
    double num = 0, den = 0;
    for (int c = 0; c < in.numChannels(); ++c)
    {
        std::vector<float> r (in.ch[c].size());
        for (size_t i = 0; i < r.size(); ++i) r[i] = in.ch[c][i] - out.ch[c][i];
        auto rb = band (r, in.sampleRate), db = band (ref.ch[(size_t) std::min (c, ref.numChannels() - 1)], in.sampleRate);
        for (size_t i = 48000; i < r.size(); ++i) { num += (double) rb[i] * db[i]; den += (double) db[i] * db[i]; }
    }
    return den > 0 ? 20.0 * std::log10 (std::max (1e-6, 1.0 - num / den)) : 0.0;
}

static int keyMain (int argc, char** argv)
{
    if (argc < 3) return 2;
    Audio mix, dx; std::string err;
    if (! readWav (argv[1], mix, err) || ! readWav (argv[2], dx, err)) { std::fprintf (stderr, "%s\n", err.c_str()); return 1; }
    const char* mode = std::strcmp (argv[0], "surround") == 0 ? "surround" : "stereo";
    if (mix.numChannels() > 2) mix.ch.resize (2);
    if (dx.numChannels() > 2) dx.ch.resize (2);
    while (dx.numChannels() < mix.numChannels()) dx.ch.push_back (dx.ch[0]);
    size_t n = std::min (mix.length(), dx.length());
    n = std::min (n, (size_t) (mix.sampleRate * 100.0));
    for (auto& c : mix.ch) c.resize (n);
    for (auto& c : dx.ch) c.resize (n);
    Audio rest = mix;
    for (int c = 0; c < 2 && c < mix.numChannels(); ++c) for (size_t i = 0; i < n; ++i) rest.ch[c][i] -= dx.ch[c][i];
    std::string name = argv[1]; name = name.substr (name.find_last_of ('/') + 1); name = name.substr (0, 28);

    dehonk::Params params;
    RenderSpec spec; spec.params = params; spec.blockSize = 480; spec.collectFrames = true;

    if (std::strcmp (mode, "stereo") == 0)
    {
        const auto plain = render (mix, spec);
        RenderSpec listen = spec; listen.params.depth = 0.0f;
        const auto stem = render (dx, listen);
        double stemTop = -200;
        for (auto& f : stem.frames) for (int k = 0; k < plain.numBands; ++k) stemTop = std::max (stemTop, (double) f.levelDb[(size_t) k]);

        auto score = [&] (const Audio& key, bool on, float amount = 1.0f) -> Scores
        {
            RenderSpec ks = spec; ks.params.dialogueOn = on; ks.params.dialogueAmount = amount; ks.key = &key;
            const auto wk = render (mix, ks);
            Tally d, o; std::vector<float> trims, matches; size_t over = 0;
            size_t frames = std::min ({ plain.frames.size(), wk.frames.size(), stem.frames.size() });
            for (size_t f = 0; f < frames; ++f)
            {
                const auto& a = plain.frames[f]; const auto& b = wk.frames[f]; const auto& s = stem.frames[f];
                if (a.timeSec < 1.0) continue;
                bool speaking = false;
                for (int k = 0; k < plain.numBands; ++k)
                {
                    const int m = moduleAt ((double) plain.bandHz[(size_t) k], params);
                    if (m < 0 || ! params.moduleOn[m]) continue;
                    const double mixDb = a.levelDb[(size_t) k], dxDb = s.levelDb[(size_t) k], ca = a.cutDb[(size_t) k], cb = b.cutDb[(size_t) k];
                    const bool audible = dxDb > std::max (-70.0, stemTop - 45.0);
                    speaking = speaking || audible;
                    if (audible && dxDb >= mixDb - 3.0) { if (ca >= 0.5) d.add (ca, cb); }
                    else if (dxDb <= mixDb - 12.0) { if (ca >= 0.5) o.add (ca, cb); }
                }
                if (speaking) { trims.push_back (b.dialogueTrimDb); matches.push_back (b.keyMatch); over += b.keyOver ? 1 : 0; }
            }
            std::sort (trims.begin(), trims.end()); std::sort (matches.begin(), matches.end());
            Scores s {};
            s.dlgOpenA = d.n ? d.a / (double) d.n : 0; s.dlgOpenB = d.n ? d.b / (double) d.n : 0; s.othOpenA = o.n ? o.a / (double) o.n : 0; s.othOpenB = o.n ? o.b / (double) o.n : 0;
            s.nd = d.n; s.no = o.n; s.trimMed = trims.empty() ? 0 : trims[trims.size() / 2]; s.matchMed = matches.empty() ? 0 : matches[matches.size() / 2]; s.overShare = trims.empty() ? 0 : 100.0 * (double) over / (double) trims.size();
            s.dxLossA = lossDb (mix, plain.out, dx); s.dxLossB = lossDb (mix, wk.out, dx);
            s.restLossA = lossDb (mix, plain.out, rest); s.restLossB = lossDb (mix, wk.out, rest);
            s.identical = true;
            for (int c = 0; c < mix.numChannels(); ++c) if (std::memcmp (plain.out.ch[c].data(), wk.out.ch[c].data(), sizeof (float) * n) != 0) s.identical = false;
            return s;
        };
        auto gained = [] (Audio a, double db) { const float g = (float) dbToAmp (db); for (auto& c : a.ch) for (auto& s : c) s *= g; return a; };
        auto mono = [] (Audio a) { for (size_t i = 0; i < a.ch[0].size(); ++i) a.ch[0][i] += a.ch[1][i]; a.ch.resize (1); return a; };
        auto print = [&] (const char* what, const Scores& s)
        {
            std::printf ("%s|%-22s| dlg-open cut %.2f -> %.3f dB (%zu) | other-open %.2f -> %.2f dB (%zu) | key gain med %+.1f dB, found %.2f, flagged %.0f%% | DX loss(audio) %.3f -> %.3f dB | rest loss %.3f -> %.3f dB%s\n",
                         name.c_str(), what, s.dlgOpenA, s.dlgOpenB, s.nd, s.othOpenA, s.othOpenB, s.no, s.trimMed, s.matchMed, s.overShare, s.dxLossA, s.dxLossB, s.restLossA, s.restLossB, s.identical ? " | BIT-IDENTICAL to no key" : "");
            std::fflush (stdout);
        };
        const long ms = (long) (mix.sampleRate / 1000.0);
        print ("unity", score (dx, true));
        print ("key on, Dialogue OFF", score (dx, false));
        Audio silent = dx; for (auto& c : silent.ch) std::fill (c.begin(), c.end(), 0.0f);
        print ("silent key", score (silent, true));
        print ("amount 0", score (dx, true, 0.0f));
        print ("key late 1 ms", score (shifted (dx, 1 * ms), true));
        print ("key late 5 ms", score (shifted (dx, 5 * ms), true));
        print ("key late 20 ms", score (shifted (dx, 20 * ms), true));
        print ("key late 100 ms", score (shifted (dx, 100 * ms), true));
        print ("key early 100 ms", score (shifted (dx, -100 * ms), true));
        print ("key late 500 ms", score (shifted (dx, 500 * ms), true));
        print ("key late 2 s", score (shifted (dx, 2000 * ms), true));
        print ("key +12 dB", score (gained (dx, 12), true));
        print ("key +24 dB", score (gained (dx, 24), true));
        print ("mono sum +18 dB", score (gained (mono (dx), 18), true));
        print ("key -6 dB", score (gained (dx, -6), true));
        print ("key -12 dB", score (gained (dx, -12), true));
        if (argc > 3)
        {
            Audio other;
            if (readWav (argv[3], other, err))
            {
                if (other.numChannels() > 2) other.ch.resize (2);
                for (auto& c : other.ch) c.resize (n, 0.0f);
                print ("key = another mix's stem", score (other, true));
            }
        }
        print ("key = whole mix", score (mix, true));
        print ("key = mix minus DX", score (rest, true));
        { Audio inv = gained (dx, 0); for (auto& c : inv.ch) for (auto& s : c) s = -s; print ("key polarity flipped", score (inv, true)); }
        return 0;
    }

    // surround: real stems laid out as 5.1: L/R = mix minus DX, C = DX (mono), Ls/Rs = 0.4 x (mix minus DX), LFE silent.
    Audio s51 = Audio::make (mix.sampleRate, 6, n);
    Audio keyC = Audio::make (mix.sampleRate, 1, n);
    for (size_t i = 0; i < n; ++i)
    {
        s51.ch[0][i] = rest.ch[0][i]; s51.ch[1][i] = rest.ch[1][i];
        s51.ch[2][i] = 0.7071f * (dx.ch[0][i] + dx.ch[1][i]);
        s51.ch[4][i] = 0.4f * rest.ch[0][i]; s51.ch[5][i] = 0.4f * rest.ch[1][i];
        keyC.ch[0][i] = s51.ch[2][i];
    }
    dehonk::ChannelLayout lay;
    lay.position[0] = dehonk::Position::leftFront; lay.position[1] = dehonk::Position::rightFront; lay.position[2] = dehonk::Position::centre;
    lay.position[3] = dehonk::Position::lfe; lay.position[4] = dehonk::Position::leftOther; lay.position[5] = dehonk::Position::rightOther;
    spec.useLayout = true; spec.layout = lay; spec.collectFrames = false;
    auto chLoss = [&] (const RenderResult& r, int c)
    {
        auto ib = band (s51.ch[(size_t) c], mix.sampleRate), ob = band (r.out.ch[(size_t) c], mix.sampleRate);
        double a = 0, b = 0, d = 0; for (size_t i = 48000; i < n; ++i) { a += (double) ib[i] * ib[i]; b += (double) ob[i] * ob[i]; d += (double) (ib[i] - ob[i]) * (ib[i] - ob[i]); }
        return std::pair<double,double> (10 * std::log10 ((b + 1e-20) / (a + 1e-20)), 10 * std::log10 ((d + 1e-20) / (a + 1e-20)));
    };
    const char* modes[] = { "Linked", "Grouped", "Unlinked" };
    for (int lm = 0; lm < 3; ++lm)
    {
        RenderSpec a = spec; a.params.linkMode = (dehonk::LinkMode) lm;
        const auto plain = render (s51, a);
        RenderSpec b = a; b.params.dialogueOn = true; b.key = &keyC;
        const auto wk = render (s51, b);
        const int chans[] = { 2, 0, 4 }; const char* cn[] = { "C (all dialogue)", "L (no dialogue)", "Ls (no dialogue)" };
        for (int i = 0; i < 3; ++i)
        {
            auto p = chLoss (plain, chans[i]), k = chLoss (wk, chans[i]);
            std::printf ("%s|5.1 %-8s| %-17s| 120-900 Hz level change %.3f -> %.3f dB | removed/in %.1f -> %.1f dB\n", name.c_str(), modes[lm], cn[i], p.first, k.first, p.second, k.second);
        }
        std::fflush (stdout);
    }
    return 0;
}

static std::vector<float> bp (const std::vector<float>& x, double sr)
{
    std::vector<float> y (x.size()); const double kl = 1.0 - std::exp (-2.0 * M_PI * 3500.0 / sr), kh = 1.0 - std::exp (-2.0 * M_PI * 1800.0 / sr); double l1 = 0, l2 = 0, h1 = 0, h2 = 0;
    for (size_t i = 0; i < x.size(); ++i) { l1 += kl * (x[i] - l1); l2 += kl * (l1 - l2); h1 += kh * (l2 - h1); h2 += kh * (h1 - h2); y[i] = (float) (l2 - 2 * h1 + h2); }
    return y;
}
static int snapMain (int argc, char** argv)
{
    Audio mix, dx; std::string err;
    if (argc < 3 || ! readWav (argv[1], mix, err) || ! readWav (argv[2], dx, err)) return 1;
    mix.ch.resize (1); dx.ch.resize (1);
    size_t n = std::min ({ mix.length(), dx.length(), (size_t) (mix.sampleRate * 100.0) });
    mix.ch[0].resize (n); dx.ch[0].resize (n);
    std::string name = argv[1]; name = name.substr (name.find_last_of ('/') + 1); name = name.substr (0, 24);
    const double sr = mix.sampleRate;
    for (int keyed = 0; keyed < 2; ++keyed)
    {
        RenderSpec s; s.blockSize = 480; s.collectFrames = true; s.frameEveryTick = true; s.params.snapOn = true;
        if (keyed) { s.params.dialogueOn = true; s.key = &dx; }
        const auto r = render (mix, s);
        auto mb = bp (mix.ch[0], sr), db = bp (dx.ch[0], sr);
        const size_t w = (size_t) (0.02 * sr);
        std::vector<float> cutAt (n / w + 1, 0.0f);
        for (auto& f : r.frames) { size_t i = (size_t) (f.timeSec * sr) / w; if (i < cutAt.size()) cutAt[i] = std::max (cutAt[i], f.snapCutDb); }
        double dTop = 0; std::vector<double> dp (n / w), mp (n / w);
        for (size_t i = 0; i < n / w; ++i) { double a = 0, b = 0; for (size_t j = i * w; j < (i + 1) * w; ++j) { a += (double) db[j] * db[j]; b += (double) mb[j] * mb[j]; } dp[i] = a; mp[i] = b; dTop = std::max (dTop, a); }
        int dlg = 0, dlgCut1 = 0, dlgCut3 = 0, oth = 0, othCut1 = 0; double dlgSum = 0, othSum = 0, deepest = 0;
        for (size_t i = 50; i < n / w; ++i)
        {
            const bool isDlg = dp[i] > 1e-3 * dTop && dp[i] >= 0.5 * mp[i];
            // a 20 ms window and the one after (Snap's cut is reported after the event)
            const float c = std::max (cutAt[i], i + 1 < cutAt.size() ? cutAt[i + 1] : 0.0f);
            if (isDlg) { ++dlg; dlgSum += c; dlgCut1 += c >= 1.0f; dlgCut3 += c >= 3.0f; deepest = std::max (deepest, (double) c); }
            else if (dp[i] < 0.06 * mp[i]) { ++oth; othSum += c; othCut1 += c >= 1.0f; }
        }
        std::printf ("%s|%s| 1.8-3.5 kHz dialogue-dominant 20 ms windows: %d, Snap cut >= 1 dB in %d (%.1f%%), >= 3 dB in %d, deepest %.1f dB, mean %.3f dB | non-dialogue windows: %d, >= 1 dB in %d (%.1f%%), mean %.3f dB\n", name.c_str(), keyed ? "keyed " : "no key",
                     dlg, dlgCut1, dlg ? 100.0 * dlgCut1 / dlg : 0.0, dlgCut3, deepest, dlg ? dlgSum / dlg : 0.0, oth, othCut1, oth ? 100.0 * othCut1 / oth : 0.0, oth ? othSum / oth : 0.0);
    }
    return 0;
}

static double rmsDb (const std::vector<float>& x, size_t a, size_t b) { double s = 0; for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i]; return 10 * std::log10 (s / (double) (b - a) + 1e-30); }
static int diffMain (int argc, char** argv)
{
    Audio mix; std::string err;
    if (argc < 2 || ! readWav (argv[1], mix, err)) return 1;
    if (mix.numChannels() > 2) mix.ch.resize (2);
    size_t n = std::min (mix.length(), (size_t) (mix.sampleRate * 90.0));
    for (auto& c : mix.ch) c.resize (n);
    std::string name = argv[1]; name = name.substr (name.find_last_of ('/') + 1); name = name.substr (0, 24);
    const double sr = mix.sampleRate;
    {
        for (float secs : { 1.5f, 4.0f, 8.0f })
        {
            RenderSpec s; s.blockSize = 480; s.collectFrames = true; s.useTuning = true; s.tuning.diffMatchSeconds = secs;
            for (auto& m : s.params.moduleOn) m = true; s.params.depth = 0.8f;
            const auto wet = render (mix, s);
            RenderSpec d = s; d.params.diff = true;
            const auto dif = render (mix, d);
            // level match per 3 s window, and per 400 ms (momentary) window
            std::vector<double> e3, e04;
            const size_t w3 = (size_t) (3 * sr), w04 = (size_t) (0.4 * sr);
            for (size_t a = (size_t) (5 * sr); a + w3 <= n; a += w3) { if (rmsDb (mix.ch[0], a, a + w3) < -40) continue; e3.push_back (rmsDb (dif.out.ch[0], a, a + w3) - rmsDb (wet.out.ch[0], a, a + w3)); }
            for (size_t a = (size_t) (5 * sr); a + w04 <= n; a += w04) { if (rmsDb (mix.ch[0], a, a + w04) < -40) continue; e04.push_back (rmsDb (dif.out.ch[0], a, a + w04) - rmsDb (wet.out.ch[0], a, a + w04)); }
            auto stats = [] (std::vector<double> v) { double worst = 0, mean = 0; for (double x : v) { worst = std::abs (x) > std::abs (worst) ? x : worst; mean += std::abs (x); } return std::pair<double,double> (worst, v.empty() ? 0 : mean / (double) v.size()); };
            // offset movement
            double minO = 0, maxSlope = 0; float prev = 0; double prevT = 0; bool first = true; double sumAbsSlope = 0; int ns = 0;
            for (auto& f : dif.frames) { if (f.timeSec < 5) continue; minO = std::min (minO, (double) f.diffOffsetDb); if (! first) { double sl = (f.diffOffsetDb - prev) / (f.timeSec - prevT); maxSlope = std::max (maxSlope, std::abs (sl)); sumAbsSlope += std::abs (sl); ++ns; } prev = f.diffOffsetDb; prevT = f.timeSec; first = false; }
            auto a = stats (e3), b = stats (e04);
            std::printf ("%s|diffMatchSeconds %.1f| Diff vs treated level: 3 s windows worst %+.2f dB, mean |%.2f| ; 0.4 s windows worst %+.2f dB, mean |%.2f| ; offset down to %.2f dB, moves up to %.2f dB/s (mean %.2f dB/s)\n", name.c_str(), secs, a.first, a.second, b.first, b.second, minO, maxSlope, ns ? sumAbsSlope / ns : 0.0);
        }
        return 0;
    }
    return 2;
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: dehonk_realmix key|surround|snap|diff mix.wav [dx.wav [other_dx.wav]]\n");
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "diff")
        return diffMain (argc - 1, argv + 1);
    if (mode == "snap")
        return snapMain (argc - 1, argv + 1);
    return keyMain (argc - 1, argv + 1);
}
