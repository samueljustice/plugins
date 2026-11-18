#include "Wav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace dh
{
namespace
{
constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

std::uint16_t u16 (const std::uint8_t* p) { return (std::uint16_t) (p[0] | (p[1] << 8)); }
std::uint32_t u32 (const std::uint8_t* p)
{
    return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8) | ((std::uint32_t) p[2] << 16) | ((std::uint32_t) p[3] << 24);
}

void put16 (std::vector<std::uint8_t>& v, std::uint32_t x)
{
    v.push_back ((std::uint8_t) (x & 0xff));
    v.push_back ((std::uint8_t) ((x >> 8) & 0xff));
}
void put32 (std::vector<std::uint8_t>& v, std::uint32_t x)
{
    put16 (v, x & 0xffff);
    put16 (v, x >> 16);
}
void putId (std::vector<std::uint8_t>& v, const char* id) { v.insert (v.end(), id, id + 4); }

float readSample (const std::uint8_t* p, int bits, bool isFloat)
{
    if (isFloat)
    {
        if (bits == 32)
        {
            std::uint32_t u = u32 (p);
            float f;
            std::memcpy (&f, &u, 4);
            return f;
        }
        std::uint64_t u = (std::uint64_t) u32 (p) | ((std::uint64_t) u32 (p + 4) << 32);
        double d;
        std::memcpy (&d, &u, 8);
        return (float) d;
    }
    switch (bits)
    {
        case 8: return ((float) p[0] - 128.0f) / 128.0f;
        case 16: return (float) (std::int16_t) u16 (p) / 32768.0f;
        case 24:
        {
            std::int32_t v = (std::int32_t) ((std::uint32_t) p[0] << 8 | (std::uint32_t) p[1] << 16 | (std::uint32_t) p[2] << 24) >> 8;
            return (float) v / 8388608.0f;
        }
        case 32: return (float) ((double) (std::int32_t) u32 (p) / 2147483648.0);
        default: return 0.0f;
    }
}
} // namespace

bool readWav (const std::string& path, Audio& out, std::string& err, WavFormat* formatOut)
{
    std::ifstream f (path, std::ios::binary);
    if (! f)
    {
        err = "cannot open " + path;
        return false;
    }
    std::vector<std::uint8_t> d ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp (d.data(), "RIFF", 4) != 0 || std::memcmp (d.data() + 8, "WAVE", 4) != 0)
    {
        err = path + " is not a RIFF/WAVE file";
        return false;
    }

    bool haveFmt = false;
    std::uint16_t format = 0, channels = 0, bits = 0, blockAlign = 0;
    std::uint32_t rate = 0;
    bool extensible = false;
    const std::uint8_t* data = nullptr;
    std::size_t dataBytes = 0;

    std::size_t pos = 12;
    while (pos + 8 <= d.size())
    {
        const std::uint8_t* id = d.data() + pos;
        std::size_t size = u32 (id + 4);
        const std::size_t body = pos + 8;
        const std::size_t avail = d.size() - body;
        if (std::memcmp (id, "fmt ", 4) == 0)
        {
            if (size < 16 || avail < 16)
            {
                err = "fmt chunk too short";
                return false;
            }
            const std::uint8_t* p = d.data() + body;
            format = u16 (p);
            channels = u16 (p + 2);
            rate = u32 (p + 4);
            blockAlign = u16 (p + 12);
            bits = u16 (p + 14);
            if (format == kFormatExtensible)
            {
                if (size < 40 || avail < 40)
                {
                    err = "WAVE_FORMAT_EXTENSIBLE fmt chunk too short";
                    return false;
                }
                extensible = true;
                format = u16 (p + 24); // first two bytes of the SubFormat GUID
            }
            haveFmt = true;
        }
        else if (std::memcmp (id, "data", 4) == 0)
        {
            data = d.data() + body;
            // Streaming writers leave 0 or 0xFFFFFFFF here; take what is in the file.
            dataBytes = (size == 0 || size == 0xFFFFFFFFu || size > avail) ? avail : size;
        }
        if (size > avail)
            break;
        pos = body + size + (size & 1);
    }

    if (! haveFmt || data == nullptr)
    {
        err = "missing fmt or data chunk";
        return false;
    }
    const bool isFloat = format == kFormatFloat;
    if (format != kFormatPcm && ! isFloat)
    {
        err = fmt ("unsupported WAV format tag %u (only PCM and IEEE float)", (unsigned) format);
        return false;
    }
    const bool okBits = isFloat ? (bits == 32 || bits == 64) : (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (! okBits || channels == 0 || rate == 0)
    {
        err = fmt ("unsupported WAV layout: %u channels, %u bits, %u Hz", (unsigned) channels, (unsigned) bits, (unsigned) rate);
        return false;
    }
    const std::size_t bytesPerSample = (std::size_t) bits / 8;
    const std::size_t frameBytes = std::max<std::size_t> (blockAlign, bytesPerSample * channels);
    const std::size_t frames = dataBytes / frameBytes;

    out = Audio::make ((double) rate, channels, frames);
    for (std::size_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            out.ch[(std::size_t) c][i] = readSample (data + i * frameBytes + (std::size_t) c * bytesPerSample, bits, isFloat);

    if (formatOut != nullptr)
        *formatOut = WavFormat { bits, isFloat, extensible };
    return true;
}

bool writeWav (const std::string& path, const Audio& a, const WavFormat& format, std::string& err)
{
    const int bits = format.bitsPerSample;
    if (format.isFloat ? bits != 32 : (bits != 16 && bits != 24 && bits != 32))
    {
        err = "writeWav: unsupported bit depth";
        return false;
    }
    const int nch = a.numChannels();
    const std::size_t frames = a.length();
    const std::size_t bps = (std::size_t) bits / 8;
    const std::uint64_t dataBytes64 = (std::uint64_t) frames * (std::uint64_t) nch * bps;
    if (nch < 1 || dataBytes64 > 0xFFFFFFF0ull)
    {
        err = "writeWav: no channels, or more than 4 GB of data";
        return false;
    }
    const std::uint32_t dataBytes = (std::uint32_t) dataBytes64;
    const std::uint16_t tag = format.extensible ? kFormatExtensible : (format.isFloat ? kFormatFloat : kFormatPcm);

    std::vector<std::uint8_t> h;
    putId (h, "RIFF");
    put32 (h, 0); // patched below
    putId (h, "WAVE");
    putId (h, "fmt ");
    put32 (h, format.extensible ? 40u : (format.isFloat ? 18u : 16u));
    put16 (h, tag);
    put16 (h, (std::uint32_t) nch);
    put32 (h, (std::uint32_t) std::lround (a.sampleRate));
    put32 (h, (std::uint32_t) (std::lround (a.sampleRate) * nch * (long) bps));
    put16 (h, (std::uint32_t) (nch * (int) bps));
    put16 (h, (std::uint32_t) bits);
    if (format.extensible)
    {
        put16 (h, 22);
        put16 (h, (std::uint32_t) bits);
        put32 (h, nch == 1 ? 0x4u : 0x3u);
        static const std::uint8_t guidTail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
        put16 (h, format.isFloat ? kFormatFloat : kFormatPcm);
        h.insert (h.end(), guidTail, guidTail + 14);
    }
    else if (format.isFloat)
    {
        put16 (h, 0);
    }
    if (format.isFloat || format.extensible)
    {
        putId (h, "fact");
        put32 (h, 4);
        put32 (h, (std::uint32_t) frames);
    }
    putId (h, "data");
    put32 (h, dataBytes);

    std::vector<std::uint8_t> body;
    body.reserve (dataBytes + 1);
    for (std::size_t i = 0; i < frames; ++i)
    {
        for (int c = 0; c < nch; ++c)
        {
            const float x = a.ch[(std::size_t) c][i];
            if (format.isFloat)
            {
                std::uint32_t u;
                std::memcpy (&u, &x, 4);
                put32 (body, u);
                continue;
            }
            const double clipped = std::clamp ((double) x, -1.0, 1.0);
            if (bits == 16)
                put16 (body, (std::uint32_t) (std::int32_t) std::clamp (std::lround (clipped * 32768.0), -32768L, 32767L) & 0xffff);
            else if (bits == 24)
            {
                const auto v = (std::uint32_t) (std::int32_t) std::clamp (std::lround (clipped * 8388608.0), -8388608L, 8388607L);
                body.push_back ((std::uint8_t) (v & 0xff));
                body.push_back ((std::uint8_t) ((v >> 8) & 0xff));
                body.push_back ((std::uint8_t) ((v >> 16) & 0xff));
            }
            else
                put32 (body, (std::uint32_t) (std::int32_t) std::clamp (std::llround (clipped * 2147483648.0), -2147483648LL, 2147483647LL));
        }
    }
    if (body.size() & 1)
        body.push_back (0);

    const std::uint32_t riffSize = (std::uint32_t) (h.size() - 8 + body.size());
    h[4] = (std::uint8_t) (riffSize & 0xff);
    h[5] = (std::uint8_t) ((riffSize >> 8) & 0xff);
    h[6] = (std::uint8_t) ((riffSize >> 16) & 0xff);
    h[7] = (std::uint8_t) ((riffSize >> 24) & 0xff);

    std::ofstream f (path, std::ios::binary | std::ios::trunc);
    if (! f)
    {
        err = "cannot write " + path;
        return false;
    }
    f.write ((const char*) h.data(), (std::streamsize) h.size());
    f.write ((const char*) body.data(), (std::streamsize) body.size());
    if (! f)
    {
        err = "write failed: " + path;
        return false;
    }
    return true;
}

} // namespace dh
