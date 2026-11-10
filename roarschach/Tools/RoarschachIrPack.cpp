// RoarschachIrPack: the build step that turns CHAMBER's room captures into the encrypted blob the
// plug-in embeds (CMakeLists.txt runs it when -DROARSCHACH_IR_DIR and the ROARSCHACH_IR_KEY
// environment variable are set).
//
//   RoarschachIrPack --dir <captures> --key-file <file with 64 hex digits> --out <blob>
//                    [--masked-key <generated RoarschachIrKey.h>]
//
// The captures are never committed to this repository and never written anywhere by this tool
// except, processed and encrypted, into <blob> in the build folder. Each room goes through
// irpack::processRoom (onset trim, polarity, one resample to 48 kHz, fade by -60 dB within 0.75 s,
// loudness match); the payload is sealed with ChaCha20-Poly1305 under the build's key and a fresh
// random nonce. The key is read from a file so it never appears on a command line or in a log.
//
// Which file is which room: <dir>/manifest.txt (or <dir>/irs/manifest.txt) if present, one line
// per room in the plug-in's order, "<index> <file name>"; otherwise each room is looked for under
// its own name (e.g. "Steel Box.wav", "steel-box.wav", "steel_box.wav") and then by the content
// fingerprint of the capture it was voiced with (so no file names are needed here).
//
// Before writing, the blob is opened and parsed again with the same key, and (with --masked-key)
// the key the plug-in will rebuild from the generated header is checked against it, so a build
// whose embedded rooms would not decode fails here instead of quietly shipping the modelled rooms.
//
// Plain C++17, no JUCE (it is built and run before the plug-in).

#include "Params.h"
#include "dsp/common/IrPack.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    int fail (const std::string& message)
    {
        std::fprintf (stderr, "RoarschachIrPack: %s\n", message.c_str());
        return 1;
    }

    std::string lower (std::string s)
    {
        for (auto& c : s)
            c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
        return s;
    }

    /** "Steel Box" -> "steel box"; separators (space, '-', '_') compare equal. */
    std::string normalise (const std::string& s)
    {
        std::string out;
        for (const char c : lower (s))
            out.push_back (c == '-' || c == '_' ? ' ' : c);
        return out;
    }

    // FNV-1a 64 of each room's capture file (the bytes the factory presets were voiced with), in
    // the plug-in's room order: identifies the captures whatever their file names are.
    constexpr std::array<std::uint64_t, 8> c_captureFingerprints {
        0x594d506c7477f76eull, 0xad1d528ebe2f7607ull, 0x40b3f0091e9822c3ull, 0x4176bf686d66c646ull,
        0x6affd4d495cdc361ull, 0xa3a6a86e6ca50e87ull, 0x72b0df9e2c6a0d3bull, 0x4c70791ab4258092ull,
    };

    std::uint64_t fingerprint (const fs::path& path)
    {
        std::ifstream in (path, std::ios::binary);
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (std::istreambuf_iterator<char> it (in), end; it != end; ++it)
            h = (h ^ static_cast<unsigned char> (*it)) * 0x100000001b3ull;
        return h;
    }

    struct Wav
    {
        double rate = 0.0;
        std::vector<float> left, right;
    };

    std::uint32_t le32 (const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t> (p[3]) << 24); }
    std::uint16_t le16 (const unsigned char* p) { return static_cast<std::uint16_t> (p[0] | (p[1] << 8)); }

    bool readWav (const fs::path& path, Wav& wav, std::string& error)
    {
        std::ifstream in (path, std::ios::binary);
        std::vector<unsigned char> d ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
        if (d.size() < 12 || std::string (d.begin(), d.begin() + 4) != "RIFF" || std::string (d.begin() + 8, d.begin() + 12) != "WAVE")
            return (error = "not a WAV file"), false;

        int format = 0, channels = 0, bits = 0;
        const unsigned char* data = nullptr;
        size_t dataSize = 0;
        for (size_t at = 12; at + 8 <= d.size();)
        {
            const std::string id (d.begin() + static_cast<std::ptrdiff_t> (at), d.begin() + static_cast<std::ptrdiff_t> (at + 4));
            const size_t size = le32 (&d[at + 4]);
            const size_t body = at + 8;
            if (body + size > d.size() && id != "data")
                break;
            if (id == "fmt " && size >= 16)
            {
                format = le16 (&d[body]);
                channels = le16 (&d[body + 2]);
                wav.rate = le32 (&d[body + 4]);
                bits = le16 (&d[body + 14]);
                if (format == 0xfffe && size >= 26)
                    format = le16 (&d[body + 24]);   // WAVE_FORMAT_EXTENSIBLE: the sub-format
            }
            else if (id == "data")
            {
                data = &d[body];
                dataSize = std::min (size, d.size() - body);
            }
            at = body + size + (size & 1);
        }
        if (data == nullptr || channels < 1 || wav.rate < 8000.0)
            return (error = "no audio data"), false;
        if (! ((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)))
            return (error = "unsupported sample format (16/24/32-bit PCM or 32-bit float)"), false;

        const size_t frameBytes = static_cast<size_t> (channels) * static_cast<size_t> (bits / 8);
        const size_t frames = dataSize / frameBytes;
        wav.left.resize (frames);
        wav.right.resize (frames);
        for (size_t i = 0; i < frames; ++i)
            for (int c = 0; c < std::min (channels, 2); ++c)
            {
                const unsigned char* p = data + i * frameBytes + static_cast<size_t> (c) * static_cast<size_t> (bits / 8);
                float v = 0.0f;
                if (format == 3)
                {
                    const std::uint32_t u = le32 (p);
                    std::memcpy (&v, &u, 4);
                }
                else if (bits == 16)
                    v = static_cast<float> (static_cast<std::int16_t> (le16 (p))) / 32768.0f;
                else if (bits == 24)
                    v = static_cast<float> (static_cast<std::int32_t> ((p[0] << 8) | (p[1] << 16) | (static_cast<std::uint32_t> (p[2]) << 24)) >> 8) / 8388608.0f;
                else
                    v = static_cast<float> (static_cast<double> (static_cast<std::int32_t> (le32 (p))) / 2147483648.0);
                (c == 0 ? wav.left : wav.right)[i] = std::isfinite (v) ? v : 0.0f;
            }
        if (channels == 1)
            wav.right = wav.left;
        return true;
    }

    std::vector<fs::path> wavsIn (const fs::path& dir)
    {
        std::vector<fs::path> files;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator (dir, ec))
            if (e.is_regular_file() && lower (e.path().extension().string()) == ".wav")
                files.push_back (e.path());
        return files;
    }

    /** The capture for each room, in the plug-in's order (empty paths where none was found). */
    std::array<fs::path, 8> findRooms (const fs::path& base, std::string& error)
    {
        std::array<fs::path, 8> rooms {};
        fs::path dir = base;
        if (wavsIn (dir).empty() && fs::is_directory (base / "irs"))
            dir = base / "irs";

        for (const auto& manifest : { dir / "manifest.txt", base / "manifest.txt" })
        {
            std::ifstream in (manifest);
            if (! in)
                continue;
            std::string line;
            while (std::getline (in, line))
            {
                std::istringstream ls (line);
                int index = -1;
                if (! (ls >> index) || index < 0 || index >= 8)
                    continue;
                std::string name;
                std::getline (ls >> std::ws, name);
                while (! name.empty() && std::isspace (static_cast<unsigned char> (name.back())))
                    name.pop_back();
                rooms[static_cast<size_t> (index)] = dir / name;
            }
            return rooms;
        }

        const auto files = wavsIn (dir);
        for (size_t r = 0; r < rooms.size(); ++r)
            for (const auto& f : files)
                if (normalise (f.stem().string()) == normalise (Params::chamberRoomNames[r]))
                    rooms[r] = f;
        for (const auto& f : files)
        {
            const auto h = fingerprint (f);
            for (size_t r = 0; r < rooms.size(); ++r)
                if (rooms[r].empty() && h == c_captureFingerprints[r])
                    rooms[r] = f;
        }
        (void) error;
        return rooms;
    }
}

int main (int argc, char** argv)
{
    std::string dirArg, keyFile, outArg, maskedKeyArg;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string a (argv[i]);
        if (a == "--dir")
            dirArg = argv[i + 1];
        else if (a == "--key-file")
            keyFile = argv[i + 1];
        else if (a == "--out")
            outArg = argv[i + 1];
        else if (a == "--masked-key")
            maskedKeyArg = argv[i + 1];
        else
            return fail ("unknown option " + a);
    }
    if (dirArg.empty() || keyFile.empty() || outArg.empty())
        return fail ("usage: RoarschachIrPack --dir <captures> --key-file <file> --out <blob>");

    roar::crypto::Key key {};
    {
        std::ifstream in (keyFile);
        std::string hex ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
        if (! roar::irpack::parseHexKey (hex, key))
            return fail ("the key file does not hold 64 hex digits");
    }

    std::string error;
    const auto paths = findRooms (fs::path (dirArg), error);
    std::vector<roar::irpack::Ir> rooms;
    for (size_t r = 0; r < paths.size(); ++r)
    {
        if (paths[r].empty() || ! fs::is_regular_file (paths[r]))
            return fail (std::string ("no capture found for room ") + std::to_string (r) + " (" + Params::chamberRoomNames[r]
                         + "): add <dir>/manifest.txt (\"<index> <file name>\" per line) or name the file after the room");
        Wav wav;
        if (! readWav (paths[r], wav, error))
            return fail (paths[r].filename().string() + ": " + error);
        rooms.push_back (roar::irpack::processRoom (std::move (wav.left), std::move (wav.right), wav.rate));
        const auto& ir = rooms.back();
        std::printf ("RoarschachIrPack: %-17s %5.0f ms at 48 kHz, RT60 %.2f s\n", Params::chamberRoomNames[r],
                     1000.0 * static_cast<double> (ir.left.size()) / ir.rate, static_cast<double> (ir.rt60));
    }

    roar::crypto::Nonce nonce {};
    std::random_device rd;
    for (auto& b : nonce)
        b = static_cast<std::uint8_t> (rd() & 0xffu);

    const auto blob = roar::irpack::seal (roar::irpack::serialise (rooms), key, nonce);

    // Check the blob the way the plug-in will read it: same key (rebuilt from the masked header
    // when given), open, parse, eight rooms of the lengths just packed.
    {
        roar::crypto::Key readKey = key;
        if (! maskedKeyArg.empty())
        {
            std::ifstream in (maskedKeyArg);
            std::string text ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
            const auto brace = text.find ('{');
            std::istringstream bytes (brace == std::string::npos ? std::string() : text.substr (brace + 1));
            size_t n = 0;
            for (std::string tok; n < readKey.size() && std::getline (bytes, tok, ',');)
            {
                const auto v = std::strtoul (tok.c_str(), nullptr, 0);
                readKey[n] = static_cast<std::uint8_t> ((v & 0xffu) ^ roar::irpack::keyMask (n));
                ++n;
            }
            if (n != readKey.size())
                return fail ("could not read the masked key header");
        }
        std::vector<std::uint8_t> payload;
        std::vector<roar::irpack::Ir> back;
        bool ok = readKey == key && roar::irpack::open (blob.data(), blob.size(), readKey, payload)
                  && roar::irpack::parse (payload.data(), payload.size(), back) && back.size() == rooms.size();
        for (size_t r = 0; ok && r < back.size(); ++r)
            ok = back[r].left.size() == rooms[r].left.size() && back[r].right.size() == rooms[r].right.size();
        std::fill (payload.begin(), payload.end(), std::uint8_t { 0 });
        std::fill (readKey.begin(), readKey.end(), std::uint8_t { 0 });
        if (! ok)
        {
            std::fill (key.begin(), key.end(), std::uint8_t { 0 });
            return fail ("the packed rooms do not decode with the plug-in's key: not writing the blob");
        }
    }
    std::fill (key.begin(), key.end(), std::uint8_t { 0 });

    const fs::path out (outArg);
    const fs::path tmp = out.string() + ".tmp";
    {
        std::ofstream o (tmp, std::ios::binary | std::ios::trunc);
        o.write (reinterpret_cast<const char*> (blob.data()), static_cast<std::streamsize> (blob.size()));
        if (! o)
            return fail ("could not write " + tmp.string());
    }
    std::error_code ec;
    fs::rename (tmp, out, ec);
    if (ec)
        return fail ("could not write " + out.string());
    std::printf ("RoarschachIrPack: %zu rooms, %zu bytes (encrypted)\n", rooms.size(), blob.size());
    return 0;
}
