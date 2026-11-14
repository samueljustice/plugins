// Stamps one effect against a batch of music files, without a host, for listening tests:
//   TonalRecallStamp effect.wav outFolder music1.wav [music2.wav ...]
// For each music file it makes outFolder/<music name>/ holding the three things to compare:
//   1 source - <music>.wav            the music, as given
//   2 before - <effect>.wav           the effect, as given
//   3 after - <effect> on <music>.wav the effect on that music, as the plug-in does it by
//                                      default: gliding gently through the notes the music rests on
//   4 after, locked to one note - ...  the effect given one fixed shift onto a strong note
//   5 after, more movement - ...       as 3, taking on far more of the music's own movement
// and adds what it heard and did to outFolder/Results.txt (also printed). The match is the
// plug-in's default: follow the music's lead line note by note, or where the music holds one
// note, the smallest move onto a strong note of it. The two files are taken to start together.
// PCM 16/24/32-bit or float WAV in; mono or stereo.

#include "core/Analysis.h"
#include "core/Stamper.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace tonalrecall::core;

namespace
{
struct Wav
{
    double rate = 0.0;
    std::vector<std::vector<float>> channels;

    std::vector<float> sum() const
    {
        std::vector<float> out(channels.empty() ? 0 : channels[0].size(), 0.0f);
        for (const auto& c : channels)
            for (std::size_t i = 0; i < out.size(); ++i)
                out[i] += c[i];
        return out;
    }
};

bool load(const std::string& path, Wav& out)
{
    std::ifstream f(path, std::ios::binary);
    if (! f)
        return false;
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
        return false;
    const auto u16 = [&](std::size_t at) { return static_cast<unsigned>(d[at] | (d[at + 1] << 8)); };
    const auto u32 = [&](std::size_t at)
    {
        return static_cast<std::uint32_t>(d[at]) | (static_cast<std::uint32_t>(d[at + 1]) << 8)
             | (static_cast<std::uint32_t>(d[at + 2]) << 16) | (static_cast<std::uint32_t>(d[at + 3]) << 24);
    };
    unsigned format = 0, bits = 0, channels = 0;
    std::size_t at = 12;
    while (at + 8 <= d.size())
    {
        const std::size_t size = u32(at + 4), body = at + 8;
        if (std::memcmp(d.data() + at, "fmt ", 4) == 0)
        {
            format = u16(body);
            channels = u16(body + 2);
            out.rate = u32(body + 4);
            bits = u16(body + 14);
            if (format == 0xfffe && size >= 26)
                format = u16(body + 24);
        }
        else if (std::memcmp(d.data() + at, "data", 4) == 0 && channels > 0 && bits > 0)
        {
            const std::size_t bytes = bits / 8, frame = bytes * channels;
            const std::size_t frames = std::min(size, d.size() - body) / frame;
            out.channels.assign(std::min<std::size_t>(channels, 2), std::vector<float>(frames));
            for (std::size_t i = 0; i < frames; ++i)
                for (std::size_t c = 0; c < out.channels.size(); ++c)
                {
                    const std::size_t pos = body + i * frame + c * bytes;
                    const unsigned char* p = d.data() + pos;
                    float v = 0.0f;
                    if (format == 3 && bits == 32) std::memcpy(&v, p, 4);
                    else if (bits == 16) v = static_cast<float>(static_cast<std::int16_t>(p[0] | (p[1] << 8))) / 32768.0f;
                    else if (bits == 24) v = static_cast<float>((static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16)) << 8) >> 8) / 8388608.0f;
                    else if (bits == 32) v = static_cast<float>(static_cast<std::int32_t>(u32(pos))) / 2147483648.0f;
                    out.channels[c][i] = v;
                }
            return true;
        }
        at = body + size + (size & 1);
    }
    return false;
}

bool save24(const std::string& path, const Wav& w)
{
    const auto channels = static_cast<std::uint32_t>(w.channels.size());
    const auto frames = static_cast<std::uint32_t>(w.channels[0].size());
    const std::uint32_t dataBytes = frames * channels * 3;
    std::ofstream f(path, std::ios::binary);
    if (! f)
        return false;
    const auto put32 = [&](std::uint32_t v) { f.put(static_cast<char>(v & 255)); f.put(static_cast<char>((v >> 8) & 255)); f.put(static_cast<char>((v >> 16) & 255)); f.put(static_cast<char>((v >> 24) & 255)); };
    const auto put16 = [&](std::uint32_t v) { f.put(static_cast<char>(v & 255)); f.put(static_cast<char>((v >> 8) & 255)); };
    f.write("RIFF", 4); put32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); put32(16); put16(1); put16(channels); put32(static_cast<std::uint32_t>(w.rate));
    put32(static_cast<std::uint32_t>(w.rate) * channels * 3); put16(channels * 3); put16(24);
    f.write("data", 4); put32(dataBytes);
    for (std::uint32_t i = 0; i < frames; ++i)
        for (std::uint32_t c = 0; c < channels; ++c)
        {
            const auto v = static_cast<std::int32_t>(std::lround(std::clamp(w.channels[c][i], -1.0f, 1.0f) * 8388607.0f));
            f.put(static_cast<char>(v & 255)); f.put(static_cast<char>((v >> 8) & 255)); f.put(static_cast<char>((v >> 16) & 255));
        }
    return static_cast<bool>(f);
}

// Printed, and kept in Results.txt.
std::ofstream g_results;
void say(const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
;
void say(const char* format, ...)
{
    char text[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fputs(text, stdout);
    if (g_results)
        g_results << text << std::flush;
}

std::string baseName(const std::string& path)
{
    const auto slash = path.find_last_of('/');
    auto name = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::printf("usage: %s effect.wav outFolder music1.wav [music2.wav ...]\n", argv[0]);
        return 2;
    }
    Wav effect;
    if (! load(argv[1], effect) || effect.channels.empty())
    {
        std::printf("could not read the effect %s\n", argv[1]);
        return 1;
    }
    std::error_code made;
    std::filesystem::create_directories(argv[2], made);
    g_results.open(std::string(argv[2]) + "/Results.txt", std::ios::app);

    const auto effectSum = std::make_shared<std::vector<float>>(effect.sum());
    const auto sfx = analyseSfx(effectSum->data(), static_cast<int>(effectSum->size()), effect.rate);
    say("EFFECT %s: %.2f s, %zu ch; pitch %s %+d c, moves %.1f st, method %s\n\n", baseName(argv[1]).c_str(),
                static_cast<double>(effectSum->size()) / effect.rate, effect.channels.size(), sfx.pitched ? noteName(sfx.centreMidi).c_str() : "none",
                sfx.pitched ? static_cast<int>(std::lround((sfx.centreMidi - std::round(sfx.centreMidi)) * 100.0)) : 0,
                static_cast<double>(sfx.driftSemitones),
                sfx.usedMethod == SfxMethod::Resonance ? "resonance" : sfx.usedMethod == SfxMethod::Engine ? "engine" : "tonal");

    int failures = 0;
    for (int i = 3; i < argc; ++i)
    {
        Wav music;
        if (! load(argv[i], music) || music.channels.empty())
        {
            say("%s: could not be read\n", argv[i]);
            ++failures;
            continue;
        }
        const auto musicSum = music.sum();
        const auto m = analyseMusic(musicSum.data(), static_cast<int>(musicSum.size()), music.rate);
        // Two answers to compare. The default: the effect glides gently through the notes the
        // music rests on. And the plain one: locked to a single note, the smallest move onto a
        // strong note of the music.
        MatchOptions gliding;
        gliding.target = Target::Follow;
        gliding.glide = 1.0;
        gliding.bend = 0.35;     // a little of the music's own movement, as the plug-in has it
        MatchOptions locked;
        locked.target = Target::Nearest;
        MatchOptions moving = gliding;
        moving.bend = 0.8;       // ... and a lot of it, to compare
        const auto r = match(m, sfx, gliding);
        const auto rLocked = match(m, sfx, locked);
        const auto rMoving = match(m, sfx, moving);

        const std::string musicName = baseName(argv[i]), effectName = baseName(argv[1]);
        const std::filesystem::path folder = std::filesystem::path(argv[2]) / musicName;
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        say("%s\n", musicName.c_str());
        if (! m.valid)
        {
            say("   no notes found in the music\n\n");
            ++failures;
            continue;
        }
        say("   key %s (lock %.0f%%), tuning %+.0f c; notes:", m.keyName().c_str(), static_cast<double>(m.keyConfidence) * 100.0,
            static_cast<double>(m.tuningCents));
        for (const auto& n : m.notes)
            say(" %s(%.0f%%)", noteName(n.midi).c_str(), static_cast<double>(n.strength) * 100.0);
        say("\n");
        if (! r.valid || ! rLocked.valid)
        {
            say("   no match (the effect has no pitch to move)\n\n");
            ++failures;
            continue;
        }
        const auto range = [&](const MatchResult& result, const char* label)
        {
            float lo = result.curve.front(), hi = result.curve.front();
            for (float v : result.curve)
            {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            say("   %s: rests on %d note%s, shift moves between %+.2f and %+.2f st (%.2f st in all)\n", label,
                std::max(1, result.notesFollowed), result.notesFollowed >= 2 ? "s" : "", static_cast<double>(lo), static_cast<double>(hi),
                static_cast<double>(hi - lo));
        };
        range(r, "glide");
        range(rMoving, "more movement");
        say("   locked: %+.2f st, onto %s\n", rLocked.semitones, noteName(rLocked.targetMidi).c_str());

        const auto render = [&](const MatchResult& result, const std::string& path)
        {
            Stamper::Setup setup;
            setup.sampleRate = effect.rate;
            setup.channels = static_cast<int>(effect.channels.size());
            setup.staticSemitones = result.semitones;
            setup.curveHopSeconds = result.hopSeconds;
            setup.curve = result.curve;
            setup.learned = effectSum;
            Stamper stamper(setup);
            const auto latency = static_cast<std::size_t>(stamper.latencySamples());
            Wav out;
            out.rate = effect.rate;
            out.channels = effect.channels;
            for (auto& c : out.channels)
                c.resize(c.size() + latency, 0.0f);
            const std::size_t total = out.channels[0].size();
            for (std::size_t at = 0; at < total; at += 1024)
            {
                const int n = static_cast<int>(std::min<std::size_t>(1024, total - at));
                float* ptrs[2] { out.channels[0].data() + at, out.channels.size() > 1 ? out.channels[1].data() + at : nullptr };
                stamper.process(ptrs, n);
            }
            for (auto& c : out.channels)
                c.erase(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(latency));
            return save24(path, out);
        };

        // The files to compare, side by side.
        const auto copyOptions = std::filesystem::copy_options::overwrite_existing;
        std::filesystem::copy_file(argv[i], folder / ("1 source - " + musicName + ".wav"), copyOptions, error);
        std::filesystem::copy_file(argv[1], folder / ("2 before - " + effectName + ".wav"), copyOptions, error);
        const bool wrote = render(r, (folder / ("3 after - " + effectName + " on " + musicName + ".wav")).string())
                           && render(rLocked, (folder / ("4 after, locked to one note - " + effectName + " on " + musicName + ".wav")).string())
                           && render(rMoving, (folder / ("5 after, more movement - " + effectName + " on " + musicName + ".wav")).string());
        if (wrote)
            say("   in %s\n\n", folder.string().c_str());
        else
        {
            say("   could not write into %s\n\n", folder.string().c_str());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
