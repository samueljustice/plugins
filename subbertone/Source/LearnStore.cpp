#include "LearnStore.h"

#include <JuceHeader.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <utility>

namespace subbertone::learn
{
    namespace
    {
        using RegionList = std::vector<std::shared_ptr<const Region>>;

        //==============================================================================
        // Saved format, version 3
        //
        //   data = base64(zlib(header + body)). Every number is little-endian, floats as their IEEE bit patterns.
        //
        //   header, 16 bytes: "SBLN", u16 format version (3), u16 0, u32 body bytes, u32 CRC-32 of the body
        //   body: u32 regions, then per region (in timeline order; ids are not saved, a load gives new ones)
        //       f64 learned rate, u32 hop samples, i64 first hop, u32 hops,
        //       f64 first frame seconds, u32 frames,
        //       u8 candidates, u8 requested method, u8 used method, u8 flags (1 = uncertain),
        //       f32 line ratio, f32 gap fill seconds, f64 alignment seconds, f32 silence level (dBFS),
        //       u32 learn order (0 for the region learned first, then 1, 2 ...: each number once),
        //       per candidate: f32 ratio, f32 score, f32 max Hz,
        //       the frames, one plane per field: flags, pitch (low bytes, then high bytes), confidence, presence,
        //       the prints, one plane per field: flags, level, band 0 ... band 5.
        //
        //   Pitch is counted in quarter cents above 10 Hz and stored as the change from the previous frame, 16 bits,
        //   zigzag-coded (0, -1, 1, -2 ... as 0, 1, 2, 3 ...) so that small changes either way leave the high bytes
        //   at zero; a frame without a pitch repeats the previous value (change 0). Confidence (in 1/255), level and
        //   bands are stored as the change from the previous frame or hop, 8 bits. Changes wrap around. Slowly moving
        //   values stored as changes, with each field in a plane of its own, compress about three times better than
        //   the raw structs (measured: 30-35 kB per learned minute after zlib).
        //
        // The decompressor reports no errors (a damaged or cut-off stream just yields less), so the header carries
        // the body's length and checksum, and the decoder accepts nothing it would not have written itself.
        //
        // A load numbers the regions afresh in the order they were learned, so the oldest are still forgotten first.
        //
        // Version 2 is the same without the learn order, and version 1 without the silence level as well. Both are
        // still read: their regions count as learned in timeline order, and version 1's get c_unknownSilenceDbfs, so
        // they play as they always did (their silent frames stay silent whatever the Threshold).
        constexpr std::uint8_t c_magic[4] { 'S', 'B', 'L', 'N' };
        constexpr std::uint16_t c_formatVersion = 3;
        constexpr std::uint16_t c_oldestFormatVersion = 1;
        constexpr std::size_t c_headerBytes = 16;
        constexpr std::uint8_t c_regionUncertain = 1;
        constexpr std::size_t c_fixedRegionBytes = 64;   // the fields before the candidates
        constexpr std::size_t c_candidateBytes = 12;
        constexpr std::size_t c_frameBytes = 5;          // flags, pitch (2), confidence, presence
        constexpr std::size_t c_printBytes = 8;

        // Every limit at its maximum gives a body of about 1.1 MB (64 regions; 10 minutes as ~120 000 frames and
        // ~60 000 hops), so these caps leave room while bounding what a damaged state can make us allocate. Base64
        // holds 3 bytes in 4 characters, and zlib may grow incompressible data slightly.
        constexpr std::size_t c_maxPayloadBytes = std::size_t { 2 } << 20;
        constexpr std::size_t c_maxTextBytes = std::size_t { 4 } << 20;
        constexpr std::uint32_t c_maxFramesPerRegion = 12100;   // a guard before allocating; findProblem() is exact
        constexpr std::uint32_t c_maxRegionId = 0x7fffffff;

        // Pitch in quarter cents above 10 Hz: 0.125 cent at most from the analysed value, 16 bits up to 128 kHz.
        constexpr double c_logTenHz = 2.302585092994045684;                  // ln 10
        constexpr double c_logQuarterCent = 0.6931471805599453094 / 4800.0;  // ln 2 / (1200 * 4)
        constexpr int c_maxPitchCode = 52636;                                // c_maxLineHz: 4800 * log2(2000), rounded
        constexpr double c_logSemitone = 0.6931471805599453094 / 12.0;

        constexpr double c_timeEpsilon = 1.0e-9;   // seconds: positions this close are the same position
        constexpr double c_gridEpsilon = 1.0e-6;   // hops or frames: the rounding of a time onto a grid
        constexpr double c_learnedSlackSeconds = c_maxRegions * c_hopSeconds;  // a part-hop at the edges of each region

        //==============================================================================
        // Quarter cents above 10 Hz, or -1 outside c_minLineHz .. c_maxLineHz (NaN and infinities included).
        int pitchCode(float logHz) noexcept
        {
            const double code = std::round((static_cast<double>(logHz) - c_logTenHz) / c_logQuarterCent);
            return code >= 0.0 && code <= static_cast<double>(c_maxPitchCode) ? static_cast<int>(code) : -1;
        }

        float pitchFromCode(int code) noexcept
        {
            return static_cast<float>(c_logTenHz + c_logQuarterCent * static_cast<double>(code));
        }

        // 1/255 steps, or -1 outside 0..1 (NaN included).
        int confidenceCode(float confidence) noexcept
        {
            return confidence >= 0.0f && confidence <= 1.0f ? static_cast<int>(std::lround(confidence * 255.0f)) : -1;
        }

        float confidenceFromCode(int code) noexcept
        {
            return static_cast<float>(code) / 255.0f;
        }

        // A change of pitch code, modulo 2^16, with the sign moved to the lowest bit.
        std::uint16_t zigzag(int change) noexcept
        {
            const auto bits = static_cast<std::uint16_t>(change);
            const auto sign = static_cast<std::uint16_t>((bits & 0x8000u) != 0 ? 0xffffu : 0u);
            return static_cast<std::uint16_t>((bits << 1) ^ sign);
        }

        int unzigzag(std::uint16_t coded) noexcept
        {
            const int magnitude = coded >> 1;
            return (coded & 1u) != 0 ? -magnitude - 1 : magnitude;
        }

        bool sameBits(float a, float b) noexcept
        {
            return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
        }

        bool sameBits(double a, double b) noexcept
        {
            return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
        }

        // Exactly what decoding gives back for this frame.
        bool isCanonical(const Frame& frame) noexcept
        {
            const int confidence = confidenceCode(frame.confidence);

            if (confidence < 0 || ! sameBits(confidenceFromCode(confidence), frame.confidence))
                return false;

            if (! frame.isVoiced())
                return sameBits(frame.logLineHz, 0.0f);

            const int pitch = pitchCode(frame.logLineHz);
            return pitch >= 0 && sameBits(pitchFromCode(pitch), frame.logLineHz);
        }

        //==============================================================================
        constexpr std::array<std::uint32_t, 256> makeCrcTable() noexcept
        {
            std::array<std::uint32_t, 256> table {};

            for (std::uint32_t i = 0; i < 256; ++i)
            {
                std::uint32_t c = i;

                for (int bit = 0; bit < 8; ++bit)
                    c = (c & 1u) != 0 ? 0xedb88320u ^ (c >> 1) : c >> 1;

                table[i] = c;
            }

            return table;
        }

        constexpr auto c_crcTable = makeCrcTable();

        // The CRC-32 of zip and PNG.
        std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept
        {
            std::uint32_t c = 0xffffffffu;

            for (std::size_t i = 0; i < size; ++i)
                c = c_crcTable[(c ^ data[i]) & 0xffu] ^ (c >> 8);

            return c ^ 0xffffffffu;
        }

        //==============================================================================
        class ByteWriter
        {
        public:
            explicit ByteWriter(std::vector<std::uint8_t>& bytes) : m_bytes(bytes) {}

            void u8(std::uint8_t value) { m_bytes.push_back(value); }
            void u16(std::uint16_t value) { put(value, 2); }
            void u32(std::uint32_t value) { put(value, 4); }
            void u64(std::uint64_t value) { put(value, 8); }
            void i64(std::int64_t value) { put(static_cast<std::uint64_t>(value), 8); }
            void f32(float value) { put(std::bit_cast<std::uint32_t>(value), 4); }
            void f64(double value) { put(std::bit_cast<std::uint64_t>(value), 8); }

        private:
            void put(std::uint64_t value, int bytes)
            {
                for (int i = 0; i < bytes; ++i)
                    m_bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
            }

            std::vector<std::uint8_t>& m_bytes;
        };

        // Reads past the end give 0 and fail the reader for good, so a parser checks ok() once per section instead
        // of after every field.
        class ByteReader
        {
        public:
            ByteReader(const std::uint8_t* data, std::size_t size) noexcept : m_data(data), m_size(size) {}

            bool ok() const noexcept { return m_ok; }
            std::size_t remaining() const noexcept { return m_size - m_position; }

            const std::uint8_t* take(std::size_t bytes) noexcept
            {
                if (! m_ok || bytes > remaining())
                {
                    m_ok = false;
                    return nullptr;
                }

                const std::uint8_t* start = m_data + m_position;
                m_position += bytes;
                return start;
            }

            std::uint8_t u8() noexcept { return static_cast<std::uint8_t>(get(1)); }
            std::uint16_t u16() noexcept { return static_cast<std::uint16_t>(get(2)); }
            std::uint32_t u32() noexcept { return static_cast<std::uint32_t>(get(4)); }
            std::int64_t i64() noexcept { return static_cast<std::int64_t>(get(8)); }
            float f32() noexcept { return std::bit_cast<float>(u32()); }
            double f64() noexcept { return std::bit_cast<double>(get(8)); }

        private:
            std::uint64_t get(int bytes) noexcept
            {
                const std::uint8_t* start = take(static_cast<std::size_t>(bytes));

                if (start == nullptr)
                    return 0;

                std::uint64_t value = 0;

                for (int i = 0; i < bytes; ++i)
                    value |= static_cast<std::uint64_t>(start[i]) << (8 * i);

                return value;
            }

            const std::uint8_t* m_data;
            std::size_t m_size;
            std::size_t m_position = 0;
            bool m_ok = true;
        };

        //==============================================================================
        // `learnRank`: the region's place in the order the regions were learned (0 = first).
        void writeRegion(ByteWriter& out, const Region& region, std::uint32_t learnRank)
        {
            const auto numFrames = region.frames.size();

            out.f64(region.learnedRate);
            out.u32(static_cast<std::uint32_t>(region.hopSamples));
            out.i64(region.firstHop);
            out.u32(static_cast<std::uint32_t>(region.prints.size()));
            out.f64(region.firstFrameSeconds);
            out.u32(static_cast<std::uint32_t>(numFrames));
            out.u8(static_cast<std::uint8_t>(region.candidates.size()));
            out.u8(static_cast<std::uint8_t>(region.requested));
            out.u8(static_cast<std::uint8_t>(region.used));
            out.u8(region.uncertain ? c_regionUncertain : std::uint8_t { 0 });
            out.f32(region.lineRatio);
            out.f32(region.gapFillSeconds);
            out.f64(region.alignmentSeconds);
            out.f32(region.silenceDbfs);
            out.u32(learnRank);

            for (const auto& candidate : region.candidates)
            {
                out.f32(candidate.ratio);
                out.f32(candidate.score);
                out.f32(candidate.maxHz);
            }

            for (const auto& frame : region.frames)
                out.u8(frame.flags);

            std::vector<std::uint16_t> pitchChanges(numFrames);
            int previousPitch = 0;

            for (std::size_t i = 0; i < numFrames; ++i)
            {
                const auto& frame = region.frames[i];
                const int pitch = frame.isVoiced() ? std::max(0, pitchCode(frame.logLineHz)) : previousPitch;
                pitchChanges[i] = zigzag(pitch - previousPitch);
                previousPitch = pitch;
            }

            for (const auto change : pitchChanges)
                out.u8(static_cast<std::uint8_t>(change & 0xffu));

            for (const auto change : pitchChanges)
                out.u8(static_cast<std::uint8_t>(change >> 8));

            int previousConfidence = 0;

            for (const auto& frame : region.frames)
            {
                const int confidence = std::max(0, confidenceCode(frame.confidence));
                out.u8(static_cast<std::uint8_t>(confidence - previousConfidence));   // modulo 2^8
                previousConfidence = confidence;
            }

            for (std::size_t i = 0; i < numFrames; ++i)
                out.u8(i < region.presence.size() ? region.presence[i] : std::uint8_t { 0 });

            for (const auto& print : region.prints)
                out.u8(print.flags);

            int previousLevel = 0;

            for (const auto& print : region.prints)
            {
                out.u8(static_cast<std::uint8_t>(print.level - previousLevel));
                previousLevel = print.level;
            }

            for (std::size_t band = 0; band < static_cast<std::size_t>(Fingerprint::numBands); ++band)
            {
                int previousBand = 0;

                for (const auto& print : region.prints)
                {
                    out.u8(static_cast<std::uint8_t>(print.band[band] - previousBand));
                    previousBand = print.band[band];
                }
            }
        }

        // One region of a body in format `version`, without an id, or nothing if anything about it is off. Its place in
        // the order the regions were learned goes to `learnRank` (version 3 saves it; before, it is left as it is).
        // Sizes are checked against the limits and against what is left of the body before anything is allocated.
        std::optional<Region> readRegion(ByteReader& in, std::uint16_t version, std::uint32_t& learnRank)
        {
            Region region;
            region.learnedRate = in.f64();
            const std::uint32_t hopSamples = in.u32();
            region.firstHop = in.i64();
            const std::uint32_t numHops = in.u32();
            region.firstFrameSeconds = in.f64();
            const std::uint32_t numFrames = in.u32();
            const std::uint8_t numCandidates = in.u8();
            const std::uint8_t requested = in.u8();
            const std::uint8_t used = in.u8();
            const std::uint8_t flags = in.u8();
            region.lineRatio = in.f32();
            region.gapFillSeconds = in.f32();
            region.alignmentSeconds = in.f64();
            region.silenceDbfs = version >= 2 ? in.f32() : c_unknownSilenceDbfs;

            if (version >= 3)
                learnRank = in.u32();

            if (! in.ok() || ! (region.learnedRate >= c_minSampleRate && region.learnedRate <= c_maxSampleRate))
                return std::nullopt;

            if (static_cast<std::int64_t>(hopSamples) != hopSamplesFor(region.learnedRate)
                || numHops == 0 || static_cast<std::int64_t>(numHops) > maxHopsFor(region.learnedRate)
                || numFrames == 0 || numFrames > c_maxFramesPerRegion || numCandidates > c_maxCandidates
                || requested > static_cast<std::uint8_t>(Method::Tonal)
                || (used != static_cast<std::uint8_t>(Method::Engine) && used != static_cast<std::uint8_t>(Method::Tonal))
                || (flags & ~c_regionUncertain) != 0)
                return std::nullopt;

            if (in.remaining() < numCandidates * c_candidateBytes + numFrames * c_frameBytes + numHops * c_printBytes)
                return std::nullopt;

            region.hopSamples = static_cast<int>(hopSamples);
            region.requested = static_cast<Method>(requested);
            region.used = static_cast<Method>(used);
            region.uncertain = (flags & c_regionUncertain) != 0;

            region.candidates.resize(numCandidates);

            for (auto& candidate : region.candidates)
            {
                candidate.ratio = in.f32();
                candidate.score = in.f32();
                candidate.maxHz = in.f32();
            }

            const std::uint8_t* frameFlags = in.take(numFrames);
            const std::uint8_t* pitchLow = in.take(numFrames);
            const std::uint8_t* pitchHigh = in.take(numFrames);
            const std::uint8_t* confidenceChanges = in.take(numFrames);
            const std::uint8_t* presence = in.take(numFrames);
            const std::uint8_t* printFlags = in.take(numHops);
            const std::uint8_t* levelChanges = in.take(numHops);
            std::array<const std::uint8_t*, Fingerprint::numBands> bands {};

            for (auto& band : bands)
                band = in.take(numHops);

            if (! in.ok())
                return std::nullopt;

            region.frames.resize(numFrames);
            int pitch = 0;
            int confidence = 0;

            for (std::uint32_t i = 0; i < numFrames; ++i)
            {
                const int change = unzigzag(static_cast<std::uint16_t>(pitchLow[i] | (pitchHigh[i] << 8)));
                pitch = (pitch + change) & 0xffff;
                confidence = (confidence + confidenceChanges[i]) & 0xff;

                auto& frame = region.frames[i];
                frame.flags = frameFlags[i];
                frame.confidence = confidenceFromCode(confidence);

                if (frame.isVoiced())
                {
                    if (pitch > c_maxPitchCode)
                        return std::nullopt;

                    frame.logLineHz = pitchFromCode(pitch);
                }
                else if (change != 0)
                {
                    return std::nullopt;   // never written: a frame without a pitch repeats the previous one
                }
            }

            region.presence.assign(presence, presence + numFrames);
            region.prints.resize(numHops);
            int level = 0;
            std::array<int, Fingerprint::numBands> band {};

            for (std::uint32_t i = 0; i < numHops; ++i)
            {
                auto& print = region.prints[i];
                level = (level + levelChanges[i]) & 0xff;
                print.level = static_cast<std::uint8_t>(level);
                print.flags = printFlags[i];

                for (std::size_t b = 0; b < band.size(); ++b)
                {
                    band[b] = (band[b] + bands[b][i]) & 0xff;
                    print.band[b] = static_cast<std::uint8_t>(band[b]);
                }
            }

            canonicalise(region);

            if (findProblem(region) != nullptr)
                return std::nullopt;

            return region;
        }

        //==============================================================================
        constexpr char c_base64Digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string toBase64(const std::uint8_t* data, std::size_t size)
        {
            std::string text;
            text.reserve((size + 2) / 3 * 4);

            for (std::size_t i = 0; i < size; i += 3)
            {
                const std::size_t count = std::min<std::size_t>(3, size - i);
                std::uint32_t bits = static_cast<std::uint32_t>(data[i]) << 16;

                if (count > 1)
                    bits |= static_cast<std::uint32_t>(data[i + 1]) << 8;

                if (count > 2)
                    bits |= static_cast<std::uint32_t>(data[i + 2]);

                text += c_base64Digits[(bits >> 18) & 63u];
                text += c_base64Digits[(bits >> 12) & 63u];
                text += count > 1 ? c_base64Digits[(bits >> 6) & 63u] : '=';
                text += count > 2 ? c_base64Digits[bits & 63u] : '=';
            }

            return text;
        }

        int base64Digit(char c) noexcept
        {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        }

        // Accepts exactly what toBase64() writes: no line breaks or spaces, padding only at the end, unused bits 0.
        bool fromBase64(std::string_view text, std::vector<std::uint8_t>& bytes)
        {
            bytes.clear();

            if (text.size() % 4 != 0)
                return false;

            bytes.reserve(text.size() / 4 * 3);

            for (std::size_t i = 0; i < text.size(); i += 4)
            {
                const bool last = i + 4 == text.size();
                const int padding = ! last ? 0 : (text[i + 3] != '=' ? 0 : (text[i + 2] != '=' ? 1 : 2));
                std::uint32_t bits = 0;

                for (std::size_t k = 0; k < 4; ++k)
                {
                    const int digit = k >= 4 - static_cast<std::size_t>(padding) ? 0 : base64Digit(text[i + k]);

                    if (digit < 0)
                        return false;

                    bits = (bits << 6) | static_cast<std::uint32_t>(digit);
                }

                if ((padding == 1 && (bits & 0xffu) != 0) || (padding == 2 && (bits & 0xffffu) != 0))
                    return false;

                bytes.push_back(static_cast<std::uint8_t>(bits >> 16));

                if (padding < 2)
                    bytes.push_back(static_cast<std::uint8_t>(bits >> 8));

                if (padding < 1)
                    bytes.push_back(static_cast<std::uint8_t>(bits));
            }

            return true;
        }

        //==============================================================================
        std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& payload)
        {
            juce::MemoryOutputStream compressed;

            {
                juce::GZIPCompressorOutputStream zlib(compressed, 9);   // zlib format (the default window bits)
                zlib.write(payload.data(), payload.size());
            }   // the zlib stream is finished when the compressor goes

            const auto* bytes = static_cast<const std::uint8_t*>(compressed.getData());
            return std::vector<std::uint8_t>(bytes, bytes + compressed.getDataSize());
        }

        // One payload of at most c_maxPayloadBytes, nothing after it. The body length comes from the header, so a
        // stream that claims more is refused before it is unpacked.
        bool decompress(const std::vector<std::uint8_t>& compressed, std::vector<std::uint8_t>& payload)
        {
            juce::MemoryInputStream source(compressed.data(), compressed.size(), false);
            juce::GZIPDecompressorInputStream zlib(source);

            payload.assign(c_headerBytes, 0);

            if (zlib.read(payload.data(), static_cast<int>(c_headerBytes)) != static_cast<int>(c_headerBytes))
                return false;

            ByteReader header(payload.data() + 8, 4);
            const std::uint32_t bodyBytes = header.u32();

            if (bodyBytes > c_maxPayloadBytes - c_headerBytes)
                return false;

            payload.resize(c_headerBytes + bodyBytes);
            const int bodySize = static_cast<int>(bodyBytes);

            if (bodySize > 0 && zlib.read(payload.data() + c_headerBytes, bodySize) != bodySize)
                return false;

            std::uint8_t more = 0;
            return zlib.read(&more, 1) == 0;
        }

        //==============================================================================
        double learnedSeconds(const RegionList& regions) noexcept
        {
            double total = 0.0;

            for (const auto& region : regions)
                total += region->durationSeconds();

            return total;
        }

        bool fitsLimits(const RegionList& regions) noexcept
        {
            return regions.size() <= static_cast<std::size_t>(c_maxRegions)
                && learnedSeconds(regions) <= c_maxLearnedSeconds + c_learnedSlackSeconds;
        }

        bool overlap(const Region& a, const Region& b) noexcept
        {
            return a.startSeconds() < b.endSeconds() - c_timeEpsilon && b.startSeconds() < a.endSeconds() - c_timeEpsilon;
        }

        void sortByStart(RegionList& regions)
        {
            std::stable_sort(regions.begin(), regions.end(), [](const auto& a, const auto& b)
            {
                return a->startSeconds() < b->startSeconds();
            });
        }

        // Hops [from, to) of `region` (absolute indices on its own grid) as a region of its own, without an id but as
        // old as `region` (its learnOrder); nothing if that would be shorter than c_minRegionSeconds.
        std::optional<Region> slice(const Region& region, std::int64_t from, std::int64_t to)
        {
            from = std::max(from, region.firstHop);
            to = std::min(to, region.firstHop + region.numHops());

            if (to <= from || static_cast<double>((to - from) * region.hopSamples) < c_minRegionSeconds * region.learnedRate)
                return std::nullopt;

            Region part;
            part.learnOrder = region.learnOrder;
            part.learnedRate = region.learnedRate;
            part.hopSamples = region.hopSamples;
            part.firstHop = from;
            part.prints.assign(region.prints.begin() + (from - region.firstHop),
                               region.prints.begin() + (to - region.firstHop));

            // From the last frame at or before the new start to the first one at or after the new end, so the curve
            // inside interpolates exactly as before.
            const auto lastFrame = static_cast<std::int64_t>(region.frames.size()) - 1;
            const double startFrame = (part.startSeconds() - region.firstFrameSeconds) / c_frameSeconds;
            const double endFrame = (part.endSeconds() - region.firstFrameSeconds) / c_frameSeconds;
            const auto first = std::clamp(static_cast<std::int64_t>(std::floor(startFrame + c_gridEpsilon)),
                                          std::int64_t { 0 }, lastFrame);
            const auto last = std::clamp(static_cast<std::int64_t>(std::ceil(endFrame - c_gridEpsilon)), first, lastFrame);

            part.firstFrameSeconds = region.frameTime(static_cast<std::size_t>(first));
            part.frames.assign(region.frames.begin() + first, region.frames.begin() + last + 1);
            part.presence.assign(region.presence.begin() + first, region.presence.begin() + last + 1);
            part.candidates = region.candidates;
            part.requested = region.requested;
            part.used = region.used;
            part.uncertain = region.uncertain;
            part.lineRatio = region.lineRatio;
            part.gapFillSeconds = region.gapFillSeconds;
            part.alignmentSeconds = region.alignmentSeconds;
            part.silenceDbfs = region.silenceDbfs;

            canonicalise(part);   // the quality of what is left, and playback status of its own

            if (findProblem(part) != nullptr)
                return std::nullopt;

            return part;
        }
    } // namespace

    //==============================================================================
    void canonicalise(Region& region)
    {
        for (auto& frame : region.frames)
        {
            if (! frame.isVoiced())
                frame.logLineHz = 0.0f;
            else if (const int pitch = pitchCode(frame.logLineHz); pitch >= 0)
                frame.logLineHz = pitchFromCode(pitch);

            if (const int confidence = confidenceCode(frame.confidence); confidence >= 0)
                frame.confidence = confidenceFromCode(confidence);
        }

        region.lineLogRatio = region.lineRatio > 0.0f && std::isfinite(region.lineRatio) ? std::log(region.lineRatio) : 0.0f;
        region.quality = measure(region);

        if (region.live == nullptr)
            region.live = std::make_shared<RegionLive>();
    }

    Quality measure(const Region& region) noexcept
    {
        Quality quality;
        int audible = 0;
        int voiced = 0;
        int measured = 0;
        int bridged = 0;
        double confidenceSum = 0.0;

        // Steps compare measured frames only: gap fill's frames are skipped (a note change it spans is one step, whatever
        // GAP FILL is), as are audible frames without a pitch; silence starts afresh. Moves on neighbouring measured
        // frames make one step.
        const Frame* lastMeasured = nullptr;
        bool moving = false;

        for (const auto& frame : region.frames)
        {
            if ((frame.flags & Frame::silent) == 0)
                ++audible;
            else
                lastMeasured = nullptr;

            if (frame.isVoiced())
            {
                ++voiced;

                if ((frame.flags & Frame::bridged) != 0)
                {
                    ++bridged;
                }
                else
                {
                    ++measured;
                    confidenceSum += frame.confidence;

                    const bool moved = lastMeasured != nullptr
                                    && std::abs(static_cast<double>(frame.logLineHz - lastMeasured->logLineHz)) > c_logSemitone;

                    if (moved && ! moving)
                        ++quality.jumps;

                    moving = moved;
                    lastMeasured = &frame;
                }
            }
        }

        const auto heard = std::count_if(region.prints.begin(), region.prints.end(),
                                         [](const Fingerprint& print) { return print.isHeard(); });

        if (! region.prints.empty())
            quality.heardShare = static_cast<float>(heard) / static_cast<float>(region.prints.size());

        quality.voicedShare = audible > 0 ? static_cast<float>(voiced) / static_cast<float>(audible) : 0.0f;
        quality.meanConfidence = measured > 0 ? static_cast<float>(confidenceSum / measured) : 0.0f;
        quality.bridgedSeconds = static_cast<float>(bridged * c_frameSeconds);
        return quality;
    }

    const char* findProblem(const Region& region) noexcept
    {
        const double rate = region.learnedRate;

        if (! (rate >= c_minSampleRate && rate <= c_maxSampleRate))
            return "sample rate out of range";

        if (region.hopSamples != hopSamplesFor(rate))
            return "hop does not match the sample rate";

        if (region.numHops() < 1 || region.numHops() > maxHopsFor(rate))
            return "length out of range";

        // In floating point first: an absurd firstHop would overflow start().
        if (! (std::abs(static_cast<double>(region.firstHop) * region.hopSamples / rate) <= c_maxTimelineSeconds))
            return "position out of range";

        const std::size_t numFrames = region.frames.size();

        if (numFrames == 0 || static_cast<double>(numFrames) > region.durationSeconds() / c_frameSeconds + 5.0)
            return "frame count does not fit the length";

        if (! (std::abs(region.firstFrameSeconds - region.startSeconds()) <= 2.0 * c_frameSeconds + c_timeEpsilon))
            return "frames do not start with the region";

        if (region.frameTime(numFrames - 1) < region.endSeconds() - 4.0 * c_frameSeconds - c_timeEpsilon)
            return "frames end before the region";

        constexpr std::uint8_t frameFlags = Frame::voiced | Frame::bridged | Frame::silent;

        for (const auto& frame : region.frames)
        {
            if ((frame.flags & ~frameFlags) != 0)
                return "unknown frame flags";

            if (frame.isVoiced() ? (frame.flags & Frame::silent) != 0 : (frame.flags & Frame::bridged) != 0)
                return "frame flags contradict each other";

            if (! isCanonical(frame))
                return "frame not rounded as saved, or out of range";
        }

        if (region.presence.size() != numFrames)
            return "presence does not match the frames";

        if (region.candidates.size() > static_cast<std::size_t>(c_maxCandidates))
            return "too many candidates";

        const unsigned present = (1u << region.candidates.size()) - 1u;

        for (const auto bits : region.presence)
            if ((static_cast<unsigned>(bits) & ~present) != 0u)
                return "presence of a candidate that does not exist";

        for (const auto& candidate : region.candidates)
        {
            if (! (candidate.ratio >= c_minLineRatio && candidate.ratio <= c_maxLineRatio)
                || ! (candidate.score >= 0.0f && candidate.score <= 1.0f)
                || ! (candidate.maxHz >= 0.0f && candidate.maxHz <= c_maxCandidateHz))
                return "candidate out of range";
        }

        constexpr std::uint8_t printFlags = Fingerprint::complete | Fingerprint::partial | Fingerprint::clipped;
        constexpr std::uint8_t heardFlags = Fingerprint::complete | Fingerprint::partial;

        for (const auto& print : region.prints)
            if ((print.flags & ~printFlags) != 0 || (print.flags & heardFlags) == heardFlags)
                return "unknown fingerprint flags";

        if (static_cast<std::uint8_t>(region.requested) > static_cast<std::uint8_t>(Method::Tonal)
            || (region.used != Method::Engine && region.used != Method::Tonal))
            return "unknown method";

        if (! (region.lineRatio >= c_minLineRatio && region.lineRatio <= c_maxLineRatio))
            return "line ratio out of range";

        if (! (region.gapFillSeconds >= 0.0f && region.gapFillSeconds <= c_maxGapFillSeconds))
            return "gap fill out of range";

        if (! (std::abs(region.alignmentSeconds) <= c_maxAlignmentSeconds))
            return "alignment out of range";

        if (! (region.silenceDbfs >= c_minSilenceDbfs && region.silenceDbfs <= c_maxSilenceDbfs))
            return "silence level out of range";

        return nullptr;
    }

    const char* findProblem(const RegionList& regions) noexcept
    {
        if (regions.size() > static_cast<std::size_t>(c_maxRegions))
            return "too many regions";

        for (std::size_t i = 0; i < regions.size(); ++i)
        {
            const auto& region = regions[i];

            if (region == nullptr)
                return "missing region";

            if (const char* problem = findProblem(*region))
                return problem;

            if (region->id == 0 || region->id > c_maxRegionId)
                return "region id out of range";

            if (region->live == nullptr)
                return "region without playback status";

            for (std::size_t j = 0; j < i; ++j)
                if (regions[j]->id == region->id)
                    return "two regions with one id";

            if (i > 0 && regions[i - 1]->endSeconds() > region->startSeconds() + c_timeEpsilon)
                return "regions out of order or overlapping";
        }

        if (! fitsLimits(regions))
            return "more learned than an instance holds";

        return nullptr;
    }

    //==============================================================================
    std::vector<std::uint8_t> encodeRegions(const RegionList& regions)
    {
        std::size_t expected = c_headerBytes + 4;

        for (const auto& region : regions)
            expected += c_fixedRegionBytes + region->candidates.size() * c_candidateBytes
                      + region->frames.size() * c_frameBytes + region->prints.size() * c_printBytes;

        // Each region's place in the order they were learned, which a load numbers them by.
        std::vector<std::size_t> learned(regions.size());
        std::iota(learned.begin(), learned.end(), std::size_t { 0 });
        std::stable_sort(learned.begin(), learned.end(), [&regions](std::size_t a, std::size_t b)
        {
            return regions[a]->learnedBefore(*regions[b]);
        });

        std::vector<std::uint32_t> learnRanks(regions.size());

        for (std::size_t rank = 0; rank < learned.size(); ++rank)
            learnRanks[learned[rank]] = static_cast<std::uint32_t>(rank);

        std::vector<std::uint8_t> blob(c_headerBytes, 0);
        blob.reserve(expected);
        ByteWriter body(blob);
        body.u32(static_cast<std::uint32_t>(regions.size()));

        for (std::size_t i = 0; i < regions.size(); ++i)
            writeRegion(body, *regions[i], learnRanks[i]);

        const std::size_t bodyBytes = blob.size() - c_headerBytes;
        std::vector<std::uint8_t> header;
        ByteWriter headerOut(header);

        for (const auto byte : c_magic)
            headerOut.u8(byte);

        headerOut.u16(c_formatVersion);
        headerOut.u16(0);
        headerOut.u32(static_cast<std::uint32_t>(bodyBytes));
        headerOut.u32(crc32(blob.data() + c_headerBytes, bodyBytes));
        std::copy(header.begin(), header.end(), blob.begin());
        return blob;
    }

    bool decodeRegions(const std::uint8_t* data, std::size_t size, RegionList& regions, std::uint32_t firstId)
    {
        regions.clear();

        if (data == nullptr || size < c_headerBytes || size > c_maxPayloadBytes || firstId == 0)
            return false;

        ByteReader header(data, c_headerBytes);

        for (const auto byte : c_magic)
            if (header.u8() != byte)
                return false;

        const std::uint16_t version = header.u16();
        const std::uint16_t reserved = header.u16();
        const std::uint32_t bodyBytes = header.u32();
        const std::uint32_t checksum = header.u32();

        if (version < c_oldestFormatVersion || version > c_formatVersion || reserved != 0 || bodyBytes != size - c_headerBytes
            || checksum != crc32(data + c_headerBytes, bodyBytes))
            return false;

        ByteReader body(data + c_headerBytes, bodyBytes);
        const std::uint32_t count = body.u32();

        if (! body.ok() || count > static_cast<std::uint32_t>(c_maxRegions))
            return false;

        // Ids in the order the regions were learned (before version 3: in timeline order), each place taken once.
        static_assert(c_maxRegions <= 64, "one bit per region");
        std::uint64_t ranksTaken = 0;
        RegionList decoded;
        decoded.reserve(count);

        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::uint32_t learnRank = i;
            auto region = readRegion(body, version, learnRank);

            if (! region || learnRank >= count || (ranksTaken & (std::uint64_t { 1 } << learnRank)) != 0)
                return false;

            ranksTaken |= std::uint64_t { 1 } << learnRank;
            region->id = firstId + learnRank;
            region->learnOrder = region->id;
            decoded.push_back(std::make_shared<const Region>(std::move(*region)));
        }

        if (body.remaining() != 0 || findProblem(decoded) != nullptr)
            return false;

        regions = std::move(decoded);
        return true;
    }

    std::string encodeStateData(const RegionList& regions)
    {
        const auto compressed = compress(encodeRegions(regions));
        return toBase64(compressed.data(), compressed.size());
    }

    bool decodeStateData(std::string_view text, RegionList& regions, std::uint32_t firstId)
    {
        regions.clear();

        if (text.size() > c_maxTextBytes)
            return false;

        std::vector<std::uint8_t> compressed;
        std::vector<std::uint8_t> payload;

        return fromBase64(text, compressed)
            && decompress(compressed, payload)
            && decodeRegions(payload.data(), payload.size(), regions, firstId);
    }

    //==============================================================================
    struct LearnStore::StateCache
    {
        std::mutex lock;
        bool valid = false;
        std::uint64_t regionsRevision = 0;
        juce::String data;

        // A LEARN child this build could not read in full (takeState()). It is written again as it came while the
        // regions published when it was read are still the ones on show; once they change it is never written again.
        std::unique_ptr<juce::XmlElement> kept;
        std::uint64_t keptRegionsRevision = 0;
        bool keptFollow = true;   // USE LEARNED as read from it: a change made here since is written into it
    };

    LearnStore::LearnStore()
        : m_stateCache(std::make_unique<StateCache>())
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        publishLocked({}, true, true);
    }

    LearnStore::~LearnStore() = default;

    std::shared_ptr<const Snapshot> LearnStore::getSnapshot() const
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        return m_owner;
    }

    LearnStore::AddResult LearnStore::add(Region region, std::optional<std::uint64_t> epoch)
    {
        AddResult result;
        region.live = std::make_shared<RegionLive>();   // a new region starts with its own playback status
        canonicalise(region);

        if (const char* problem = findProblem(region))
        {
            result.problem = problem;
            return result;
        }

        {
            const std::lock_guard<std::mutex> lock(m_lock);

            if (epoch.has_value() && *epoch != m_epoch.load(std::memory_order_relaxed))
            {
                result.problem = "everything learned was cleared or replaced while this was being learned";
                return result;
            }

            const double newStart = region.startSeconds();
            const double newEnd = region.endSeconds();
            RegionList regions;
            regions.reserve(m_owner->regions.size() + 2);

            for (const auto& old : m_owner->regions)
            {
                if (! overlap(*old, region))
                {
                    regions.push_back(old);
                    continue;
                }

                // What lies before and after the new region, on the old one's own hop grid, may stay.
                const double hopsPerSecond = old->learnedRate / old->hopSamples;
                const auto beforeEnd = static_cast<std::int64_t>(std::floor(newStart * hopsPerSecond + c_gridEpsilon));
                const auto afterStart = static_cast<std::int64_t>(std::ceil(newEnd * hopsPerSecond - c_gridEpsilon));
                auto before = slice(*old, old->firstHop, beforeEnd);
                auto after = slice(*old, afterStart, old->firstHop + old->numHops());

                if (! before && ! after)
                {
                    ++result.replaced;
                    continue;
                }

                ++result.shortened;

                // The part that remains (or the earlier of two) is still the same region to the view. Both parts stay
                // as old as it is (slice()).
                if (before)
                {
                    before->id = old->id;
                    regions.push_back(std::make_shared<const Region>(std::move(*before)));
                }

                if (after)
                {
                    after->id = before ? m_nextId++ : old->id;
                    regions.push_back(std::make_shared<const Region>(std::move(*after)));
                }
            }

            region.id = m_nextId++;
            region.learnOrder = region.id;
            result.id = region.id;
            regions.push_back(std::make_shared<const Region>(std::move(region)));
            sortByStart(regions);

            // Within the limits: forget the oldest of the others (the first learned) first. The new region alone
            // always fits, so this ends.
            while (! fitsLimits(regions))
            {
                auto oldest = regions.end();

                for (auto it = regions.begin(); it != regions.end(); ++it)
                    if ((*it)->id != result.id && (oldest == regions.end() || (*it)->learnedBefore(**oldest)))
                        oldest = it;

                if (oldest == regions.end())
                    break;

                regions.erase(oldest);
                ++result.forgotten;
            }

            if (const char* problem = findProblem(regions))
            {
                result = {};
                result.problem = problem;
                return result;
            }

            publishLocked(std::move(regions), m_owner->follow, true);
        }

        notifyChange();
        return result;
    }

    bool LearnStore::update(std::uint32_t id, const std::function<void(Region&)>& edit)
    {
        // The edit runs without the lock (it may take a while, and may call back in). If the region was replaced
        // meanwhile, edit the newer version instead of overwriting it.
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            std::shared_ptr<const Region> original;

            {
                // Held in a variable: a temporary in a range-for's initialiser is destroyed before the loop runs, and a
                // publish on another thread could then free the snapshot under it.
                const auto snapshot = getSnapshot();

                for (const auto& region : snapshot->regions)
                    if (region->id == id)
                        original = region;
            }

            if (original == nullptr)
                return false;

            Region edited = *original;

            if (edit)
                edit(edited);

            edited.id = id;
            edited.learnOrder = original->learnOrder;
            edited.live = original->live;   // still the same region: its age and playback status carry on

            if (! sameBits(edited.learnedRate, original->learnedRate) || edited.hopSamples != original->hopSamples
                || edited.firstHop != original->firstHop || edited.prints.size() != original->prints.size())
                return false;

            canonicalise(edited);

            if (findProblem(edited) != nullptr)
                return false;

            bool published = false;

            {
                const std::lock_guard<std::mutex> lock(m_lock);
                RegionList regions = m_owner->regions;
                const auto it = std::find(regions.begin(), regions.end(), original);

                if (it != regions.end())
                {
                    *it = std::make_shared<const Region>(std::move(edited));
                    publishLocked(std::move(regions), m_owner->follow, true);
                    published = true;
                }
            }

            if (published)
            {
                notifyChange();
                return true;
            }
        }

        return false;
    }

    bool LearnStore::forget(std::uint32_t id, Clock::time_point now)
    {
        {
            const std::lock_guard<std::mutex> lock(m_lock);
            RegionList regions = m_owner->regions;
            const auto it = std::find_if(regions.begin(), regions.end(),
                                         [id](const auto& region) { return region->id == id; });

            if (it == regions.end())
                return false;

            rememberForgottenLocked({ *it }, now);
            regions.erase(it);
            publishLocked(std::move(regions), m_owner->follow, true);
        }

        notifyChange();
        return true;
    }

    bool LearnStore::clear(Clock::time_point now)
    {
        // Forgetting everything also forgets a LEARN child kept from a state this build could not read in full. It
        // was never on show, so undo() does not bring it back.
        bool droppedKept = false;

        {
            const std::lock_guard<std::mutex> lock(m_stateCache->lock);
            droppedKept = m_stateCache->kept != nullptr;
            m_stateCache->kept.reset();
        }

        bool forgotRegions = false;

        {
            const std::lock_guard<std::mutex> lock(m_lock);
            m_epoch.fetch_add(1, std::memory_order_acq_rel);
            forgotRegions = ! m_owner->regions.empty();

            if (! forgotRegions && ! droppedKept)
                return false;

            if (forgotRegions)
                rememberForgottenLocked(m_owner->regions, now);

            // Published even if only the kept child went: what a save writes has changed, so the host must hear of it.
            publishLocked({}, m_owner->follow, true);
        }

        notifyChange();
        return forgotRegions;
    }

    bool LearnStore::canUndo(Clock::time_point now) const
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        return ! m_forgotten.empty() && now < m_undoDeadline;
    }

    bool LearnStore::undo(Clock::time_point now)
    {
        {
            const std::lock_guard<std::mutex> lock(m_lock);

            if (m_forgotten.empty() || now >= m_undoDeadline)
                return false;

            RegionList regions = m_owner->regions;
            int restored = 0;

            for (const auto& region : m_forgotten)
            {
                const bool overlapsNewer = std::any_of(regions.begin(), regions.end(),
                                                       [&](const auto& other) { return overlap(*other, *region); });
                RegionList with = regions;
                with.push_back(region);

                if (! overlapsNewer && fitsLimits(with))
                {
                    regions = std::move(with);
                    ++restored;
                }
            }

            m_forgotten.clear();

            if (restored == 0)
                return false;

            sortByStart(regions);
            publishLocked(std::move(regions), m_owner->follow, true);
        }

        notifyChange();
        return true;
    }

    void LearnStore::setFollow(bool follow)
    {
        {
            const std::lock_guard<std::mutex> lock(m_lock);

            if (m_owner->follow == follow)
                return;

            publishLocked(m_owner->regions, follow, false);
        }

        notifyChange();
    }

    void LearnStore::setChangeCallback(std::function<void()> callback)
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        m_changeCallback = std::move(callback);
    }

    void LearnStore::collectGarbage(Clock::time_point now)
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        reclaimLocked();

        if (now >= m_undoDeadline)
            m_forgotten.clear();
    }

    std::size_t LearnStore::getNumRetired() const
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        return m_retired.size();
    }

    //==============================================================================
    void LearnStore::writeState(juce::XmlElement& state) const
    {
        const auto snapshot = getSnapshot();
        std::unique_ptr<juce::XmlElement> child;
        juce::String data;

        {
            const std::lock_guard<std::mutex> lock(m_stateCache->lock);
            const auto& kept = m_stateCache->kept;

            if (kept != nullptr && m_stateCache->keptRegionsRevision == snapshot->regionsRevision)
            {
                // Nothing learned, edited, forgotten or loaded since a child this build could not read in full: that
                // child again (its attribute values are shared, not copied).
                child = std::make_unique<juce::XmlElement>(*kept);

                if (snapshot->follow != m_stateCache->keptFollow)
                    child->setAttribute("follow", snapshot->follow ? 1 : 0);
            }
            else
            {
                // Regions published since the kept child was read replace it for good. (A snapshot taken before it
                // was read is older, and leaves it alone.)
                if (kept != nullptr && snapshot->regionsRevision > m_stateCache->keptRegionsRevision)
                    m_stateCache->kept.reset();

                if (! m_stateCache->valid || m_stateCache->regionsRevision != snapshot->regionsRevision)
                {
                    const auto text = encodeStateData(snapshot->regions);
                    m_stateCache->data = juce::String::fromUTF8(text.data(), static_cast<int>(text.size()));
                    m_stateCache->regionsRevision = snapshot->regionsRevision;
                    m_stateCache->valid = true;
                }

                data = m_stateCache->data;   // shared, not copied
            }
        }

        if (child == nullptr)
        {
            child = std::make_unique<juce::XmlElement>(c_stateTag);
            child->setAttribute("version", c_stateVersion);
            child->setAttribute("follow", snapshot->follow ? 1 : 0);
            child->setAttribute("data", data);
        }

        state.deleteAllChildElementsWithTagName(c_stateTag);
        state.addChildElement(child.release());
    }

    LearnStore::StateResult LearnStore::takeState(juce::XmlElement& state)
    {
        auto* child = state.getChildByName(c_stateTag);

        if (child == nullptr)
            return StateResult::absent;

        const bool knownVersion = child->getStringAttribute("version") == juce::String(c_stateVersion);
        const bool follow = child->getStringAttribute("follow", "1") != "0";
        const juce::String data = child->getStringAttribute("data");

        // Anything this build does not write: an attribute a later version added, or child elements.
        bool knownContent = child->getNumChildElements() == 0;

        for (int i = 0; i < child->getNumAttributes(); ++i)
        {
            const auto& name = child->getAttributeName(i);
            knownContent = knownContent && (name == "version" || name == "follow" || name == "data");
        }

        // Taken out of the state as it is, in case it has to be saved again; any further LEARN children go too.
        state.removeChildElement(child, false);
        std::unique_ptr<juce::XmlElement> taken(child);
        state.deleteAllChildElementsWithTagName(c_stateTag);

        // Ids for whatever it holds, so they are never ones that an earlier region (or a job still holding one) had.
        std::uint32_t firstId = 0;

        {
            const std::lock_guard<std::mutex> lock(m_lock);
            firstId = m_nextId;
            m_nextId += static_cast<std::uint32_t>(c_maxRegions);
        }

        RegionList regions;
        const bool readable = knownVersion
                           && decodeStateData(std::string_view(data.toRawUTF8(), data.getNumBytesAsUTF8()), regions, firstId);
        std::uint64_t regionsRevision = 0;

        {
            const std::lock_guard<std::mutex> lock(m_lock);
            m_epoch.fetch_add(1, std::memory_order_acq_rel);

            // A state that leaves nothing learned (a host preset or default saved with nothing learned, or one that
            // could not be read) can be undone like clear(). One with regions of its own withdraws UNDO, so two
            // sessions' regions are never mixed. Over nothing learned, a pending UNDO stays as it is.
            if (! regions.empty())
                m_forgotten.clear();
            else if (! m_owner->regions.empty())
                rememberForgottenLocked(m_owner->regions, Clock::now());

            publishLocked(std::move(regions), follow, true);
            regionsRevision = m_owner->regionsRevision;
        }

        {
            const std::lock_guard<std::mutex> lock(m_stateCache->lock);

            // What was just read is what saving would write again.
            if (readable)
            {
                m_stateCache->data = data;
                m_stateCache->regionsRevision = regionsRevision;
                m_stateCache->valid = true;
            }

            // What this build could not read in full is saved again as it came, until the regions change: a session
            // opened and saved here keeps what a later version learned.
            if (! readable || ! knownContent)
            {
                m_stateCache->kept = std::move(taken);
                m_stateCache->keptRegionsRevision = regionsRevision;
                m_stateCache->keptFollow = follow;
            }
            else
            {
                m_stateCache->kept.reset();
            }
        }

        return readable ? StateResult::loaded : StateResult::unreadable;
    }

    //==============================================================================
    void LearnStore::publishLocked(RegionList regions, bool follow, bool regionsChanged)
    {
        auto next = std::make_shared<Snapshot>();
        next->regions = std::move(regions);
        next->follow = follow;
        next->revision = m_revision.load(std::memory_order_relaxed) + 1;
        next->regionsRevision = regionsChanged || m_owner == nullptr ? next->revision : m_owner->regionsRevision;

        m_current.store(next.get(), std::memory_order_seq_cst);
        m_revision.store(next->revision, std::memory_order_release);

        if (m_owner != nullptr)
            m_retired.push_back(std::move(m_owner));

        m_owner = std::move(next);
        reclaimLocked();
    }

    void LearnStore::reclaimLocked()
    {
        // m_current names none of the retired snapshots any more, and the audio thread checks m_current after it
        // announces a snapshot: so the one it announces now is the only one it can still be reading, and everything
        // else can go (here, on this thread).
        const Snapshot* inUse = m_hazard.load(std::memory_order_seq_cst);
        std::erase_if(m_retired, [inUse](const auto& snapshot) { return snapshot.get() != inUse; });
    }

    void LearnStore::rememberForgottenLocked(RegionList forgotten, Clock::time_point now)
    {
        m_forgotten = std::move(forgotten);
        m_undoDeadline = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(c_undoSeconds));
    }

    void LearnStore::notifyChange()
    {
        std::function<void()> callback;

        {
            const std::lock_guard<std::mutex> lock(m_lock);
            callback = m_changeCallback;
        }

        if (callback)
            callback();
    }
} // namespace subbertone::learn
