// Command-line look at what Tonal Recall makes of real files:
//   TonalRecallAnalyse music.wav effect.wav [result.wav]
// Prints the music's notes, the effect's pitch line, the match, and (with a third file) how a
// rendered result compares with the effect in pitch and timing. PCM 16/24/32-bit or float WAV.

#include "core/Analysis.h"
#include "core/HindsightTracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace tonalrecall;
using namespace tonalrecall::core;

namespace
{
struct Wav
{
    double rate = 0.0;
    int channels = 0;
    std::vector<float> mono;   // the sum of the channels, as Learn keeps it
};

bool load(const char* path, Wav& out)
{
    std::ifstream f(path, std::ios::binary);
    if (! f)
        return false;
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
        return false;
    const auto u16 = [&](std::size_t at) { return static_cast<unsigned>(d[at] | (d[at + 1] << 8)); };
    const auto u32 = [&](std::size_t at) { return static_cast<std::uint32_t>(d[at]) | (static_cast<std::uint32_t>(d[at + 1]) << 8) | (static_cast<std::uint32_t>(d[at + 2]) << 16) | (static_cast<std::uint32_t>(d[at + 3]) << 24); };
    unsigned format = 0, bits = 0;
    std::size_t at = 12;
    while (at + 8 <= d.size())
    {
        const std::size_t size = u32(at + 4), body = at + 8;
        if (std::memcmp(d.data() + at, "fmt ", 4) == 0)
        {
            format = u16(body);
            out.channels = static_cast<int>(u16(body + 2));
            out.rate = u32(body + 4);
            bits = u16(body + 14);
            if (format == 0xfffe && size >= 26)
                format = u16(body + 24);
        }
        else if (std::memcmp(d.data() + at, "data", 4) == 0 && out.channels > 0 && bits > 0)
        {
            const std::size_t bytes = bits / 8, frame = bytes * static_cast<std::size_t>(out.channels);
            const std::size_t frames = std::min(size, d.size() - body) / frame;
            out.mono.assign(frames, 0.0f);
            for (std::size_t i = 0; i < frames; ++i)
                for (int c = 0; c < out.channels; ++c)
                {
                    const unsigned char* p = d.data() + body + i * frame + static_cast<std::size_t>(c) * bytes;
                    float v = 0.0f;
                    if (format == 3 && bits == 32) { std::memcpy(&v, p, 4); }
                    else if (bits == 16) v = static_cast<float>(static_cast<std::int16_t>(p[0] | (p[1] << 8))) / 32768.0f;
                    else if (bits == 24) v = static_cast<float>((static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16)) << 8) >> 8) / 8388608.0f;
                    else if (bits == 32) v = static_cast<float>(static_cast<std::int32_t>(u32(body + i * frame + static_cast<std::size_t>(c) * bytes))) / 2147483648.0f;
                    out.mono[i] += v;
                }
            return true;
        }
        at = body + size + (size & 1);
    }
    return false;
}

void printLine(const char* label, const std::vector<float>& hz, double hop)
{
    std::printf("%s (every 25 ms, MIDI; . = no pitch):\n   ", label);
    const auto step = static_cast<std::size_t>(std::max(1.0, 0.025 / hop));
    int col = 0;
    for (std::size_t i = 0; i < hz.size(); i += step)
    {
        if (hz[i] > 0.0f) std::printf("%5.1f ", hzToMidi(hz[i])); else std::printf("    . ");
        if (++col % 12 == 0) std::printf("\n   ");
    }
    std::printf("\n");
}
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: %s music.wav effect.wav [result.wav]\n", argv[0]);
        return 2;
    }
    Wav music, effect;
    if (! load(argv[1], music) || ! load(argv[2], effect))
    {
        std::printf("could not read the files\n");
        return 1;
    }

    const auto m = analyseMusic(music.mono.data(), static_cast<int>(music.mono.size()), music.rate);
    std::printf("MUSIC %s: %.3f s, %d ch\n", argv[1], static_cast<double>(music.mono.size()) / music.rate, music.channels);
    std::printf("   valid %d, %s, confidence %.2f, tuning %+.1f c, root %s (%.1f Hz)\n", m.valid ? 1 : 0, m.keyName().c_str(),
                static_cast<double>(m.keyConfidence), static_cast<double>(m.tuningCents), noteName(m.rootMidi).c_str(), midiToHz(m.rootMidi));
    std::printf("   chroma:");
    for (int pc = 0; pc < 12; ++pc)
        std::printf(" %s %.2f", pitchClassName(pc), static_cast<double>(m.chroma[static_cast<std::size_t>(pc)]));
    std::printf("\n   notes:");
    for (const auto& n : m.notes)
        std::printf(" %s(%.2f at %s)", pitchClassName(n.pitchClass), static_cast<double>(n.strength), noteName(n.midi).c_str());
    std::printf("\n");

    std::printf("EFFECT %s: %.3f s, %d ch\n", argv[2], static_cast<double>(effect.mono.size()) / effect.rate, effect.channels);
    for (auto method : { SfxMethod::Auto, SfxMethod::Tonal, SfxMethod::Engine, SfxMethod::Resonance })
    {
        const auto s = analyseSfx(effect.mono.data(), static_cast<int>(effect.mono.size()), effect.rate, method);
        const char* names[] { "auto", "tonal", "engine", "resonance" };
        std::printf("   %-9s -> used %s, pitched %d, centre %.2f (%s), drift %.1f st, tonalness %.2f\n", names[static_cast<int>(method)],
                    names[static_cast<int>(s.usedMethod)], s.pitched ? 1 : 0, s.centreMidi, s.pitched ? noteName(s.centreMidi).c_str() : "-",
                    static_cast<double>(s.driftSemitones), static_cast<double>(s.tonalness));
        if (method == SfxMethod::Auto)
        {
            printLine("   line", s.hz, s.hopSeconds);
            const auto r = match(m, s, {});
            if (r.valid)
                std::printf("   MATCH: %+.2f st to %s\n", r.semitones, noteName(r.targetMidi).c_str());
        }
    }

    if (argc >= 4)
    {
        Wav result;
        if (! load(argv[3], result))
            return 1;
        const auto s = analyseSfx(result.mono.data(), static_cast<int>(result.mono.size()), result.rate, SfxMethod::Auto);
        std::printf("RESULT %s: %.3f s; centre %.2f (%s), drift %.1f st\n", argv[3], static_cast<double>(result.mono.size()) / result.rate,
                    s.centreMidi, s.pitched ? noteName(s.centreMidi).c_str() : "-", static_cast<double>(s.driftSemitones));
        printLine("   line", s.hz, s.hopSeconds);

        // Timing: the lag that best lines up the two level envelopes (5 ms steps, +-400 ms).
        const auto envelope = [](const Wav& w)
        {
            const auto hop = static_cast<std::size_t>(w.rate * 0.001);
            std::vector<double> e;
            for (std::size_t i = 0; i + hop <= w.mono.size(); i += hop)
            {
                double sum = 0.0;
                for (std::size_t k = 0; k < hop; ++k)
                    sum += static_cast<double>(w.mono[i + k]) * w.mono[i + k];
                e.push_back(std::sqrt(sum / static_cast<double>(hop)));
            }
            return e;
        };
        const auto a = envelope(effect), b = envelope(result);
        double best = -1.0;
        int bestLag = 0;
        for (int lag = -400; lag <= 400; ++lag)
        {
            double sum = 0.0;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                const auto j = static_cast<std::ptrdiff_t>(i) + lag;
                if (j >= 0 && j < static_cast<std::ptrdiff_t>(b.size()))
                    sum += a[i] * b[static_cast<std::size_t>(j)];
            }
            if (sum > best) { best = sum; bestLag = lag; }
        }
        std::printf("   timing: the result's level envelope lines up with the effect's at %+d ms\n", bestLag);
    }
    return 0;
}
