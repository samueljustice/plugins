#pragma once

#include "Biquad.h"
#include "ChaCha20Poly1305.h"
#include "RealFft.h"
#include "SincResampler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace roar::irpack
{
    /**
     * CHAMBER's room impulse responses: the offline processing every room goes through (the
     * build-time packer for the bundled rooms, and the modelled rooms at load), and the encrypted
     * container the bundled rooms are stored in. Plain C++, no JUCE; allocates (never on the
     * audio thread).
     *
     * Container (all little endian):
     *   "RCIR" | u32 format (1) | 12-byte nonce | u32 payload size | payload (ChaCha20) | 16-byte tag
     *   The tag (Poly1305, RFC 8439 AEAD) covers the 24 header bytes as associated data and the
     *   encrypted payload, so a damaged or tampered blob is rejected before anything is decoded.
     * Payload:
     *   u32 count, then per room: u32 rate | u32 frames | f32 rt60 s | f32 scale | frames x (i16 L, i16 R)
     */
    constexpr double c_storeRate = 48000.0;
    constexpr double c_maxSeconds = 0.75;
    constexpr double c_preRollSeconds = 0.00025;
    constexpr double c_onsetSearchSeconds = 0.030;
    constexpr double c_fadeSeconds = 0.150;
    constexpr std::uint32_t c_format = 1;
    constexpr size_t c_headerSize = 24;

    /** CHAMBER's rooms (Params::chamberRoomNames, in that order). */
    constexpr int c_numRooms = 10;

    /**
     * What a room's capture needs beyond the common processing (processRoom), by room. The first
     * eight are designed impulse responses and need nothing more; the last two are recordings of
     * a single loud, impulsive excitation in a resonant concrete space, heard from the next room:
     *   - midSide: the file holds mid (channel 1) and side (channel 2), decoded to
     *     L = M + sideGain S, R = M - sideGain S (sideGain puts the side at the mid's level, as
     *     in the other rooms, so Width means the same in every room);
     *   - highPassHz: a 4th-order Butterworth high-pass on the capture (the excitation's thump
     *     and the building's rumble sit under the voice and would only add peaks);
     *   - fitDecay: the capture rings for 2-3 s, past c_maxSeconds (the length every room keeps to,
     *     for CPU); its late decay is steepened from c_fitFromSeconds so it reaches -60 dB by then
     *     instead of being cut off;
     *   - peakCeiling: the most the processed room's mid may reach on a full-scale click (its
     *     largest |sample|, after the loudness match), re the loudness-matched level; a room over
     *     it is turned down to it (the loudness match then gives way to the peak bound);
     *   - keepArrival: trim from the first sample above -40 dB (less the pre-roll) instead of from
     *     the onset, and fade in over the pre-roll (raised cosine). A recorded arrival through a
     *     wall builds up over a millisecond or more; trimming at the onset would drop it and start
     *     the room mid-waveform, a step that lets back the low end the high-pass removed.
     */
    struct Capture
    {
        bool midSide = false;
        double sideGain = 1.0;
        double highPassHz = 0.0;
        bool fitDecay = false;
        double peakCeiling = 0.0;   // 0 = no bound
        bool keepArrival = false;
    };

    constexpr double c_fitFromSeconds = 0.050;
    /** The largest |sample| of the loudest designed room's mid after the loudness match (-18 dB):
        the recorded rooms may not be more impulsive than that (they measure -35 to -37 dB, so the
        bound does not act on them today; it holds for any re-recording). */
    constexpr double c_designedPeak = 0.125;

    inline constexpr std::array<Capture, c_numRooms> c_captures { {
        {}, {}, {}, {}, {}, {}, {}, {},
        { false, 1.0, 90.0, true, c_designedPeak, true },    // Party Wall: a spaced pair
        { true, 0.86, 90.0, true, c_designedPeak, true },    // Tower Block: mid / side
    } };

    struct Ir
    {
        double rate = c_storeRate;
        std::vector<float> left, right;
        float rt60 = 0.0f;    // seconds (T20 extrapolated), after processing
    };

    // ---------------------------------------------------------------------------- measurement

    /** Schroeder energy decay (dB re the total) of x. */
    inline std::vector<double> energyDecayDb (const std::vector<float>& x)
    {
        std::vector<double> edc (x.size(), -200.0);
        double acc = 0.0;
        for (size_t i = x.size(); i-- > 0;)
        {
            acc += static_cast<double> (x[i]) * x[i];
            edc[i] = acc;
        }
        const double total = edc.empty() ? 0.0 : edc[0];
        for (auto& e : edc)
            e = total > 0.0 ? 10.0 * std::log10 (std::max (e / total, 1.0e-20)) : -200.0;
        return edc;
    }

    /** RT60 from the -5 .. -25 dB part of the energy decay (T20 x 3), seconds. */
    inline double measureRt60 (const std::vector<float>& x, double rate)
    {
        const auto edc = energyDecayDb (x);
        int i5 = -1, i25 = -1;
        for (size_t i = 0; i < edc.size(); ++i)
        {
            if (i5 < 0 && edc[i] <= -5.0)
                i5 = static_cast<int> (i);
            if (i25 < 0 && edc[i] <= -25.0)
            {
                i25 = static_cast<int> (i);
                break;
            }
        }
        if (i5 < 0 || i25 <= i5)
            return static_cast<double> (x.size()) / rate;
        return 3.0 * static_cast<double> (i25 - i5) / rate;
    }

    /**
     * The voice-weighted power gain of an impulse response: its power response averaged over a
     * pink spectrum band-limited to the voice (150 Hz - 5 kHz, smooth 4th-order edges).
     */
    inline double voicePowerGain (const std::vector<float>& x, double rate)
    {
        int n = 1;
        while (n < static_cast<int> (x.size()) * 2 || n < 4096)
            n <<= 1;
        RealFft fft;
        fft.prepare (n);
        std::vector<float> buffer (static_cast<size_t> (n), 0.0f), re (static_cast<size_t> (n / 2 + 1)), im (re.size());
        std::copy (x.begin(), x.end(), buffer.begin());
        fft.forward (buffer.data(), re.data(), im.data());
        double num = 0.0, den = 0.0;
        for (int k = 1; k <= n / 2; ++k)
        {
            const double f = rate * k / n;
            const double w = (1.0 / f) / (1.0 + std::pow (150.0 / f, 4.0)) / (1.0 + std::pow (f / 5000.0, 4.0));
            num += w * (static_cast<double> (re[static_cast<size_t> (k)]) * re[static_cast<size_t> (k)]
                        + static_cast<double> (im[static_cast<size_t> (k)]) * im[static_cast<size_t> (k)]);
            den += w;
        }
        return den > 0.0 ? num / den : 0.0;
    }

    // ---------------------------------------------------------------------------- processing

    /**
     * Turns a raw stereo capture into a CHAMBER room at c_storeRate:
     *   1. onset: the first sample within 6 dB of the peak of the 30 ms after the first sample
     *      above -40 dB (re the whole IR's peak), on the mid; trimmed to 0.25 ms before it (the
     *      captures' pre-delays differ from 0.6 to 87 ms, so every room starts together and
     *      CHAMBER adds no delay; a Capture with keepArrival trims from the -40 dB sample instead);
     *   2. polarity: the mid's strongest sample in the 5 ms after the onset is made positive (the
     *      wet then sums with the dry instead of cancelling);
     *   3. resampled once to c_storeRate (windowed sinc, level kept: gain in / out);
     *   4. faded to silence (raised cosine, 150 ms or 40 % of the length) by where the energy decay
     *      reaches -60 dB, and at the latest by c_maxSeconds;
     *   5. loudness-matched: voice-weighted power gain 1 (the wet sits at the dry's level), on the
     *      mid, the same gain on both channels.
     * A Capture adds, by room: a mid/side decode and a high-pass before step 1, a steeper late
     * decay before step 4, and a peak bound after step 5 (see Capture). The default does nothing.
     */
    inline Ir processRoom (std::vector<float> left, std::vector<float> right, double rate, const Capture& capture = {})
    {
        Ir out;
        const size_t n = std::min (left.size(), right.size());
        left.resize (n);
        right.resize (n);
        if (n == 0)
            return out;

        if (capture.midSide)
            for (size_t i = 0; i < n; ++i)
            {
                const float m = left[i], s = static_cast<float> (capture.sideGain * right[i]);
                left[i] = m + s;
                right[i] = m - s;
            }
        if (capture.highPassHz > 0.0)
            for (auto* channel : { &left, &right })
                for (const double q : { Biquad::c_bw4Q1, Biquad::c_bw4Q2 })
                {
                    Biquad f;
                    f.setCoeffs (Biquad::highpass (rate, capture.highPassHz, q));
                    f.process (channel->data(), static_cast<int> (n));
                }

        std::vector<float> mid (n);
        float peak = 0.0f;
        for (size_t i = 0; i < n; ++i)
        {
            mid[i] = 0.5f * (left[i] + right[i]);
            peak = std::max (peak, std::abs (mid[i]));
        }
        size_t first = 0;
        while (first < n && std::abs (mid[first]) < 0.01f * peak)
            ++first;
        const size_t window = std::min (n, first + static_cast<size_t> (c_onsetSearchSeconds * rate));
        float localPeak = 0.0f;
        for (size_t i = first; i < window; ++i)
            localPeak = std::max (localPeak, std::abs (mid[i]));
        size_t onset = first;
        while (onset < window && std::abs (mid[onset]) < 0.5f * localPeak)
            ++onset;
        const size_t preRoll = static_cast<size_t> (std::lround (c_preRollSeconds * rate));
        const size_t trimFrom = capture.keepArrival ? first : onset;
        const size_t start = trimFrom > preRoll ? trimFrom - preRoll : 0;

        size_t strongest = onset;
        const size_t polarityEnd = std::min (n, onset + static_cast<size_t> (0.005 * rate));
        for (size_t i = onset; i < polarityEnd; ++i)
            if (std::abs (mid[i]) > std::abs (mid[strongest]))
                strongest = i;
        const float sign = strongest < n && mid[strongest] < 0.0f ? -1.0f : 1.0f;

        std::vector<float> l (left.begin() + static_cast<std::ptrdiff_t> (start), left.end());
        std::vector<float> r (right.begin() + static_cast<std::ptrdiff_t> (start), right.end());
        for (auto& x : l) x *= sign;
        for (auto& x : r) x *= sign;
        if (capture.keepArrival)
        {
            const size_t fade = std::min (l.size(), trimFrom - start);
            for (size_t i = 0; i < fade; ++i)
            {
                const auto g = static_cast<float> (0.5 - 0.5 * std::cos (3.14159265358979 * static_cast<double> (i) / static_cast<double> (fade)));
                l[i] *= g;
                r[i] *= g;
            }
        }

        out.rate = c_storeRate;
        out.left = SincResampler::resample (l.data(), static_cast<int> (l.size()), rate, c_storeRate, rate / c_storeRate);
        out.right = SincResampler::resample (r.data(), static_cast<int> (r.size()), rate, c_storeRate, rate / c_storeRate);

        // Fade by -60 dB of the energy decay (or the length limit).
        std::vector<float> m (out.left.size());
        for (size_t i = 0; i < m.size(); ++i)
            m[i] = 0.5f * (out.left[i] + out.right[i]);
        const size_t limit = std::min (m.size(), static_cast<size_t> (c_maxSeconds * c_storeRate));
        if (capture.fitDecay && limit > 64)
        {
            // A ring longer than the limit: an extra exponential decay from c_fitFromSeconds, the
            // gentlest (bisection on its rate) that puts the energy decay at -60 dB by 95 % of the limit.
            const auto from = static_cast<size_t> (c_fitFromSeconds * c_storeRate);
            const auto target = static_cast<size_t> (0.95 * static_cast<double> (limit));
            const auto shaped = [&] (double rate60, std::vector<float>& y)   // rate60: dB per second
            {
                y.assign (m.begin(), m.begin() + static_cast<std::ptrdiff_t> (limit));
                for (size_t i = from; i < y.size(); ++i)
                    y[i] = static_cast<float> (y[i] * std::pow (10.0, -rate60 * static_cast<double> (i - from) / c_storeRate / 20.0));
                return energyDecayDb (y)[target];
            };
            std::vector<float> y;
            if (shaped (0.0, y) > -60.0)
            {
                double lo = 0.0, hi = 400.0;
                for (int it = 0; it < 40; ++it)
                {
                    const double trial = 0.5 * (lo + hi);
                    (shaped (trial, y) > -60.0 ? lo : hi) = trial;
                }
                for (size_t i = from; i < out.left.size(); ++i)
                {
                    const auto g = static_cast<float> (std::pow (10.0, -hi * static_cast<double> (i - from) / c_storeRate / 20.0));
                    out.left[i] *= g;
                    out.right[i] *= g;
                }
                for (size_t i = 0; i < m.size(); ++i)
                    m[i] = 0.5f * (out.left[i] + out.right[i]);
            }
        }
        const auto edc = energyDecayDb (m);
        size_t end = limit;
        for (size_t i = 0; i < end; ++i)
            if (edc[i] <= -60.0)
            {
                end = i;
                break;
            }
        end = std::max<size_t> (end, 64);
        end = std::min (end, m.size());
        out.left.resize (end);
        out.right.resize (end);
        const size_t fade = std::min (static_cast<size_t> (c_fadeSeconds * c_storeRate), static_cast<size_t> (0.4 * static_cast<double> (end)));
        for (size_t i = 0; i < fade; ++i)
        {
            const double t = static_cast<double> (i + 1) / static_cast<double> (fade);   // 0 .. 1 towards the end
            const auto w = static_cast<float> (0.5 * (1.0 + std::cos (3.14159265358979323846 * t)));
            out.left[end - fade + i] *= w;
            out.right[end - fade + i] *= w;
        }

        // Loudness match on the mid.
        m.resize (end);
        for (size_t i = 0; i < end; ++i)
            m[i] = 0.5f * (out.left[i] + out.right[i]);
        const double g = voicePowerGain (m, c_storeRate);
        double scale = g > 0.0 ? 1.0 / std::sqrt (g) : 1.0;
        if (capture.peakCeiling > 0.0)
        {
            float top = 0.0f;
            for (const float x : m)
                top = std::max (top, std::abs (x));
            if (top * scale > capture.peakCeiling)
                scale = capture.peakCeiling / top;
        }
        for (auto& x : out.left) x = static_cast<float> (x * scale);
        for (auto& x : out.right) x = static_cast<float> (x * scale);
        for (size_t i = 0; i < end; ++i)
            m[i] = 0.5f * (out.left[i] + out.right[i]);
        out.rt60 = static_cast<float> (measureRt60 (m, c_storeRate));
        return out;
    }

    // ---------------------------------------------------------------------------- payload

    namespace detail
    {
        inline void put32 (std::vector<std::uint8_t>& v, std::uint32_t x)
        {
            for (int i = 0; i < 4; ++i)
                v.push_back (static_cast<std::uint8_t> (x >> (8 * i)));
        }

        inline void putFloat (std::vector<std::uint8_t>& v, float f)
        {
            std::uint32_t x = 0;
            std::memcpy (&x, &f, 4);
            put32 (v, x);
        }

        inline bool get32 (const std::uint8_t*& p, const std::uint8_t* end, std::uint32_t& x)
        {
            if (end - p < 4)
                return false;
            x = crypto::detail::load32 (p);
            p += 4;
            return true;
        }

        inline bool getFloat (const std::uint8_t*& p, const std::uint8_t* end, float& f)
        {
            std::uint32_t x = 0;
            if (! get32 (p, end, x))
                return false;
            std::memcpy (&f, &x, 4);
            return std::isfinite (f);
        }
    }

    inline std::vector<std::uint8_t> serialise (const std::vector<Ir>& rooms)
    {
        std::vector<std::uint8_t> v;
        detail::put32 (v, static_cast<std::uint32_t> (rooms.size()));
        for (const auto& ir : rooms)
        {
            const size_t frames = std::min (ir.left.size(), ir.right.size());
            float peak = 0.0f;
            for (size_t i = 0; i < frames; ++i)
                peak = std::max ({ peak, std::abs (ir.left[i]), std::abs (ir.right[i]) });
            const float scale = peak > 0.0f ? peak / 32767.0f : 1.0f;
            detail::put32 (v, static_cast<std::uint32_t> (std::lround (ir.rate)));
            detail::put32 (v, static_cast<std::uint32_t> (frames));
            detail::putFloat (v, ir.rt60);
            detail::putFloat (v, scale);
            for (size_t i = 0; i < frames; ++i)
                for (const float x : { ir.left[i], ir.right[i] })
                {
                    const auto q = static_cast<std::int16_t> (std::clamp<long> (std::lround (x / scale), -32767, 32767));
                    v.push_back (static_cast<std::uint8_t> (static_cast<std::uint16_t> (q)));
                    v.push_back (static_cast<std::uint8_t> (static_cast<std::uint16_t> (q) >> 8));
                }
        }
        return v;
    }

    inline bool parse (const std::uint8_t* data, size_t size, std::vector<Ir>& rooms)
    {
        rooms.clear();
        const std::uint8_t* p = data;
        const std::uint8_t* end = data + size;
        std::uint32_t count = 0;
        if (! detail::get32 (p, end, count) || count > 64)
            return false;
        for (std::uint32_t r = 0; r < count; ++r)
        {
            std::uint32_t rate = 0, frames = 0;
            float rt60 = 0.0f, scale = 0.0f;
            if (! detail::get32 (p, end, rate) || ! detail::get32 (p, end, frames) || ! detail::getFloat (p, end, rt60)
                || ! detail::getFloat (p, end, scale))
                return false;
            if (rate < 8000 || rate > 384000 || frames > rate * 2u || static_cast<size_t> (end - p) < static_cast<size_t> (frames) * 4u)
                return false;
            Ir ir;
            ir.rate = rate;
            ir.rt60 = rt60;
            ir.left.resize (frames);
            ir.right.resize (frames);
            for (std::uint32_t i = 0; i < frames; ++i)
            {
                const auto l = static_cast<std::int16_t> (static_cast<std::uint16_t> (p[0] | (p[1] << 8)));
                const auto rr = static_cast<std::int16_t> (static_cast<std::uint16_t> (p[2] | (p[3] << 8)));
                ir.left[i] = static_cast<float> (l) * scale;
                ir.right[i] = static_cast<float> (rr) * scale;
                p += 4;
            }
            rooms.push_back (std::move (ir));
        }
        return p == end;
    }

    // ---------------------------------------------------------------------------- container

    inline std::vector<std::uint8_t> seal (std::vector<std::uint8_t> payload, const crypto::Key& key, const crypto::Nonce& nonce)
    {
        std::vector<std::uint8_t> blob { 'R', 'C', 'I', 'R' };
        detail::put32 (blob, c_format);
        blob.insert (blob.end(), nonce.begin(), nonce.end());
        detail::put32 (blob, static_cast<std::uint32_t> (payload.size()));
        const auto tag = crypto::aeadEncrypt (key, nonce, blob.data(), c_headerSize, payload.data(), payload.size());
        blob.insert (blob.end(), payload.begin(), payload.end());
        blob.insert (blob.end(), tag.begin(), tag.end());
        return blob;
    }

    /** Checks the header and the tag, then decrypts. False for anything damaged, foreign or keyed differently. */
    inline bool open (const std::uint8_t* blob, size_t size, const crypto::Key& key, std::vector<std::uint8_t>& payload)
    {
        payload.clear();
        if (size < c_headerSize + 16 || blob[0] != 'R' || blob[1] != 'C' || blob[2] != 'I' || blob[3] != 'R')
            return false;
        if (crypto::detail::load32 (blob + 4) != c_format)
            return false;
        crypto::Nonce nonce {};
        std::copy (blob + 8, blob + 20, nonce.begin());
        const size_t length = crypto::detail::load32 (blob + 20);
        if (length != size - c_headerSize - 16)
            return false;
        crypto::Tag tag {};
        std::copy (blob + c_headerSize + length, blob + c_headerSize + length + 16, tag.begin());
        payload.assign (blob + c_headerSize, blob + c_headerSize + length);
        if (! crypto::aeadDecrypt (key, nonce, blob, c_headerSize, payload.data(), payload.size(), tag))
        {
            std::fill (payload.begin(), payload.end(), std::uint8_t { 0 });
            payload.clear();
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------- keys

    /** 64 hex digits -> 32 bytes. */
    inline bool parseHexKey (const std::string& hex, crypto::Key& key)
    {
        std::string digits;
        for (const char c : hex)
            if (! std::isspace (static_cast<unsigned char> (c)))
                digits.push_back (c);
        if (digits.size() != 64)
            return false;
        const auto nibble = [] (char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < 32; ++i)
        {
            const int hi = nibble (digits[2 * i]), lo = nibble (digits[2 * i + 1]);
            if (hi < 0 || lo < 0)
                return false;
            key[i] = static_cast<std::uint8_t> (hi * 16 + lo);
        }
        return true;
    }

    /** The mask the build applies to the key it compiles in (CMakeLists.txt computes the same). */
    constexpr std::uint8_t keyMask (size_t i) { return static_cast<std::uint8_t> ((i * 167u + 91u) & 0xffu); }
}
