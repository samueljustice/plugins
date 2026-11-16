#include "SoundCodec.h"

#include <JuceHeader.h>

#include <cmath>
#include <cstring>

namespace pitchflattener::codec
{
namespace
{
    constexpr char c_magic[4] = { 'P', 'F', 'S', '1' };
    constexpr std::size_t c_headerSize = 4 + 8 + 4 + 4 + 8;
    constexpr int c_bits = 24;
    constexpr int c_quality = 5;   // FLAC's default: nearly the smallest, and quick
    constexpr int c_chunk = 65536;
}

std::vector<char> encode (const core::Channels& sound, double sampleRate)
{
    if (sound.empty() || sound.size() > 2 || sound[0].empty() || ! (sampleRate > 0.0))
        return {};
    const auto frames = sound[0].size();

    float peak = 0.0f;
    for (const auto& channel : sound)
        for (const float v : channel)
            peak = std::max (peak, std::abs (v));
    // Just under full scale, so the largest sample does not clip in 24 bits.
    const float scale = peak > 0.999f ? peak / 0.999f : 1.0f;

    juce::MemoryOutputStream header;
    header.write (c_magic, 4);
    header.writeDouble (sampleRate);
    header.writeFloat (scale);
    header.writeInt ((int) sound.size());
    header.writeInt64 ((juce::int64) frames);

    // The writer owns (and deletes) its stream, and only finishes the FLAC (its final frame and the
    // stream header) when it is deleted: the bytes go into a block that outlives both.
    juce::MemoryBlock flacBytes;
    auto* stream = new juce::MemoryOutputStream (flacBytes, false);
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatWriter> writer (flac.createWriterFor (stream, juce::jmax (1.0, std::round (sampleRate)),
                                                                           (unsigned int) sound.size(), c_bits, {}, c_quality));
    if (writer == nullptr)
    {
        delete stream;
        return {};
    }

    juce::AudioBuffer<float> chunk ((int) sound.size(), c_chunk);
    for (std::size_t start = 0; start < frames; start += (std::size_t) c_chunk)
    {
        const int n = (int) std::min<std::size_t> ((std::size_t) c_chunk, frames - start);
        for (int c = 0; c < (int) sound.size(); ++c)
            for (int i = 0; i < n; ++i)
                chunk.setSample (c, i, sound[(std::size_t) c][start + (std::size_t) i] / scale);
        if (! writer->writeFromAudioSampleBuffer (chunk, 0, n))
            return {};
    }
    writer.reset();   // finishes the FLAC and deletes the stream
    std::vector<char> out (c_headerSize + flacBytes.getSize());
    std::memcpy (out.data(), header.getData(), c_headerSize);
    std::memcpy (out.data() + c_headerSize, flacBytes.getData(), flacBytes.getSize());
    return out;
}

bool decode (const std::vector<char>& bytes, core::Channels& sound, double& sampleRate)
{
    if (bytes.size() <= c_headerSize || std::memcmp (bytes.data(), c_magic, 4) != 0)
        return false;
    juce::MemoryInputStream header (bytes.data(), c_headerSize, false);
    header.skipNextBytes (4);
    const double rate = header.readDouble();
    const float scale = header.readFloat();
    const int channels = header.readInt();
    const auto frames = header.readInt64();
    if (! (rate > 0.0) || ! std::isfinite (scale) || channels < 1 || channels > 2 || frames <= 0)
        return false;

    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader (flac.createReaderFor (
        new juce::MemoryInputStream (bytes.data() + c_headerSize, bytes.size() - c_headerSize, false), true));
    if (reader == nullptr || (int) reader->numChannels != channels || reader->lengthInSamples < frames)
        return false;

    sound.assign ((std::size_t) channels, std::vector<float> ((std::size_t) frames));
    juce::AudioBuffer<float> chunk (channels, c_chunk);
    for (juce::int64 start = 0; start < frames; start += c_chunk)
    {
        const int n = (int) std::min<juce::int64> (c_chunk, frames - start);
        if (! reader->read (&chunk, 0, n, start, true, channels > 1))
            return false;
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
                sound[(std::size_t) c][(std::size_t) (start + i)] = chunk.getSample (c, i) * scale;
    }
    sampleRate = rate;
    return true;
}

} // namespace pitchflattener::codec
