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
