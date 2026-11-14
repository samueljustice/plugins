#include "SpaceReverb.h"

namespace
{
    bool isPrime(int n)
    {
        if (n < 2) return false;
        for (int d = 2; d * d <= n; ++d)
            if (n % d == 0) return false;
        return true;
    }

    /** The first prime at or above `ms` of samples: distinct primes share no factor, so the lines never line up. */
    int primeLength(float ms, float sampleRate)
    {
        int n = std::max(3, static_cast<int>(ms * 0.001f * sampleRate));
        while (! isPrime(n)) ++n;
        return n;
    }

    // Line lengths: 23 to 83 ms, spread unevenly (about 400 ms in all, dense enough to have no ring of its own).
    constexpr std::array<float, SpaceReverb::c_lines> c_lineMs { 23.1f, 29.3f, 36.7f, 43.9f, 52.1f, 61.3f, 71.9f, 83.3f };
    // How fast each line's length breathes (Hz): slow, and all different.
    constexpr std::array<float, SpaceReverb::c_lines> c_lfoHz { 0.31f, 0.47f, 0.23f, 0.59f, 0.37f, 0.71f, 0.19f, 0.53f };
    // The two output taps. They are at right angles (their products sum to 0), so mid and side are uncorrelated.
    constexpr std::array<float, SpaceReverb::c_lines> c_tapMid  { 1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f };
    constexpr std::array<float, SpaceReverb::c_lines> c_tapSide { 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f };

    // The diffusers: how long each round's longest delay is (ms), where in its own eighth of that each channel
    // sits, which channel gets which eighth, and which channels are flipped.
    constexpr std::array<float, 3> c_diffMs { 9.0f, 17.0f, 31.0f };
    constexpr std::array<std::array<float, SpaceReverb::c_lines>, 3> c_diffSpot {{
        { 0.42f, 0.77f, 0.18f, 0.63f, 0.91f, 0.29f, 0.54f, 0.83f },
        { 0.71f, 0.23f, 0.88f, 0.36f, 0.59f, 0.14f, 0.95f, 0.47f },
        { 0.33f, 0.92f, 0.61f, 0.12f, 0.79f, 0.45f, 0.26f, 0.68f } }};
    constexpr std::array<std::array<int, SpaceReverb::c_lines>, 3> c_diffSlot {{
        { 3, 6, 0, 5, 2, 7, 1, 4 },
        { 5, 1, 7, 2, 4, 0, 6, 3 },
        { 2, 7, 4, 0, 6, 3, 5, 1 } }};
    constexpr std::array<std::array<float, SpaceReverb::c_lines>, 3> c_diffSign {{
        { 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f },
        { -1.0f, -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, 1.0f, -1.0f },
        { 1.0f, 1.0f, -1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f } }};
    constexpr float c_early = 0.6f;       // how much of the diffused send is heard directly, ahead of the tail
    constexpr float c_norm = 0.35355339f; // 1 / sqrt(8): makes the Hadamard mix lossless
    constexpr float c_dampHz = 5000.0f;   // above this the tail falls faster
    constexpr float c_highDecayRatio = 0.5f;
    constexpr float c_side = 0.8f;        // side tap against mid: wide, with the mono sum intact
}

void SpaceReverb::prepare(double sampleRate)
{
    m_fs = static_cast<float>(sampleRate);

    // A gentle slope from the nominal frequency down, and a second one further down to keep rumble out of the tail.
    m_hpL1.set(c_sendHighPassHz, m_fs);
    m_hpR1.set(c_sendHighPassHz, m_fs);
    m_hpL2.set(c_sendHighPassHz * 0.3f, m_fs);
    m_hpR2.set(c_sendHighPassHz * 0.3f, m_fs);
    m_lpL.set(c_sendLowPassHz, m_fs);
    m_lpR.set(c_sendLowPassHz, m_fs);

    m_preDelay = std::max(1, static_cast<int>(c_preDelayMs * 0.001f * m_fs));
    m_preL.prepare(m_preDelay + 1);
    m_preR.prepare(m_preDelay + 1);

    for (size_t d = 0; d < c_diffusers; ++d)
        for (size_t i = 0; i < c_lines; ++i)
        {
            const float ms = c_diffMs[d] * (static_cast<float>(c_diffSlot[d][i]) + c_diffSpot[d][i]) / static_cast<float>(c_lines);
            m_diffDelay[d][i] = std::max(1, static_cast<int>(ms * 0.001f * m_fs));
            m_diff[d][i].prepare(m_diffDelay[d][i] + 1);
        }

    m_modDepth = 4.0f * m_fs / 48000.0f;
    for (size_t i = 0; i < c_lines; ++i)
    {
        const int n = primeLength(c_lineMs[i], m_fs);
        m_length[i] = static_cast<float>(n);
        m_line[i].prepare(n + static_cast<int>(m_modDepth) + 4);
        m_damp[i].set(c_dampHz, m_fs);
        m_lfoInc[i] = 2.0f * c_lfoHz[i] / m_fs;
    }

    m_decay = -1.0f;
    setDecay(decaySecondsFor(0.3f));
    reset();
}

void SpaceReverb::reset()
{
    for (auto* hp : { &m_hpL1, &m_hpL2, &m_hpR1, &m_hpR2 })
        hp->reset();
    m_lpL.reset(); m_lpR.reset();
    m_preL.reset(); m_preR.reset();
    for (auto& round : m_diff)
        for (auto& line : round) line.reset();
    for (size_t i = 0; i < c_lines; ++i)
    {
        m_line[i].reset();
        m_damp[i].reset();
        m_lfoPhase[i] = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(c_lines); // spread, and the same every time
    }
}

void SpaceReverb::setDecay(float seconds)
{
    seconds = sj::clamp(seconds, 0.1f, 20.0f);
    if (std::abs(seconds - m_decay) < 1.0e-3f)
        return;
    m_decay = seconds;
    // A line of n samples is passed fs / n times a second: -60 dB after `seconds` means this gain per pass.
    for (size_t i = 0; i < c_lines; ++i)
    {
        const float passes = m_length[i] / m_fs;
        m_gainLow[i] = std::pow(10.0f, -3.0f * passes / seconds);
        m_gainHigh[i] = std::pow(10.0f, -3.0f * passes / (seconds * c_highDecayRatio));
    }
}

/** Hadamard mix in place (three rounds of sums and differences), not yet scaled: every channel feeds every other. */
void SpaceReverb::mix(std::array<float, c_lines>& v)
{
    for (size_t h = 1; h < c_lines; h <<= 1)
        for (size_t i = 0; i < c_lines; i += h << 1)
            for (size_t j = i; j < i + h; ++j)
            {
                const float a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
}

void SpaceReverb::process(float* left, float* right, int frames, float dry0, float dry1, float wet0, float wet1)
{
    const float step = 1.0f / static_cast<float>(std::max(1, frames));

    for (int n = 0; n < frames; ++n)
    {
        const float t = static_cast<float>(n + 1) * step;
        const float dry = dry0 + (dry1 - dry0) * t, wet = wet0 + (wet1 - wet0) * t;
        const float inL = left[n], inR = right[n];

        // The send: no low end, a softened top, a little late.
        m_preL.push(m_lpL.lowpass(m_hpL2.highpass(m_hpL1.highpass(inL))));
        m_preR.push(m_lpR.lowpass(m_hpR2.highpass(m_hpR1.highpass(inR))));
        const float sL = 0.5f * m_preL.buffer[static_cast<size_t>((m_preL.write - m_preDelay) & m_preL.mask)];
        const float sR = 0.5f * m_preR.buffer[static_cast<size_t>((m_preR.write - m_preDelay) & m_preR.mask)];

        // Spread it: left on four channels, right on the other four, then the diffusers.
        std::array<float, c_lines> in { sL, sR, sL, sR, sL, sR, sL, sR };
        for (size_t d = 0; d < c_diffusers; ++d)
        {
            for (size_t i = 0; i < c_lines; ++i)
            {
                auto& line = m_diff[d][i];
                const float x = in[i];
                in[i] = c_diffSign[d][i] * line.buffer[static_cast<size_t>((line.write - m_diffDelay[d][i]) & line.mask)];
                line.push(x);
            }
            mix(in);
            for (auto& x : in) x *= c_norm;
        }

        // The lines: read (each a few samples longer or shorter, slowly), tap, lose some level and more top.
        std::array<float, c_lines> v;
        float mid = 0.0f, side = 0.0f;
        for (size_t i = 0; i < c_lines; ++i)
        {
            float p = m_lfoPhase[i] + m_lfoInc[i];
            if (p >= 1.0f) p -= 2.0f;
            m_lfoPhase[i] = p;
            const float lfo = 4.0f * p * (1.0f - std::abs(p)); // a near-sine, -1..1
            const float x = m_line[i].read(static_cast<double>(m_length[i] + m_modDepth * lfo));
            const float heard = x + c_early * in[i];
            mid += c_tapMid[i] * heard;
            side += c_tapSide[i] * heard;
            v[i] = m_gainHigh[i] * x + (m_gainLow[i] - m_gainHigh[i]) * m_damp[i].lowpass(x);
        }

        mix(v);
        for (size_t i = 0; i < c_lines; ++i)
            m_line[i].push(c_norm * v[i] + in[i]);

        left[n] = dry * inL + wet * c_norm * (mid + c_side * side);
        right[n] = dry * inR + wet * c_norm * (mid - c_side * side);
    }
}

//==============================================================================
namespace
{
    using RoomLines = std::array<float, RoomReverb::c_lines>;

    // SpaceReverb's eight lengths and eight more between them (ms): the same span, about the same mean.
    constexpr RoomLines c_roomLineMs { 23.1f, 25.7f, 29.3f, 32.3f, 36.7f, 40.1f, 43.9f, 47.3f,
                                       52.1f, 56.9f, 61.3f, 65.9f, 71.9f, 76.3f, 83.3f, 88.1f };
    constexpr RoomLines c_roomLfoHz { 0.31f, 0.29f, 0.47f, 0.43f, 0.23f, 0.67f, 0.59f, 0.21f,
                                      0.37f, 0.61f, 0.71f, 0.41f, 0.19f, 0.79f, 0.53f, 0.17f };
    constexpr float c_roomNorm = 0.25f; // 1 / sqrt(16)

    /** A fixed little generator for the diffusers' places and signs, so every instance is the same. */
    struct Lcg
    {
        std::uint32_t s = 0x2545F491u;
        std::uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
        float unit() { return static_cast<float>(next() & 0xFFFF) / 65536.0f; }
    };
}

void RoomReverb::prepare(double sampleRate)
{
    m_fs = static_cast<float>(sampleRate);
    m_preDelay = std::max(1, static_cast<int>(SpaceReverb::c_preDelayMs * 0.001f * m_fs));
    for (auto& s : m_send)
    {
        s.hp1.set(SpaceReverb::c_sendHighPassHz, m_fs);
        s.hp2.set(SpaceReverb::c_sendHighPassHz * 0.3f, m_fs);
        s.lp.set(SpaceReverb::c_sendLowPassHz, m_fs);
        s.pre.prepare(m_preDelay + 1);
    }

    // The diffusers, as SpaceReverb's: in each round every line gets its own sixteenth of the round's longest
    // delay (in a shuffled order), at its own place within it, and about half the lines are flipped.
    Lcg rng;
    for (size_t d = 0; d < c_diffusers; ++d)
    {
        std::array<int, c_lines> slot {};
        for (int i = 0; i < c_lines; ++i)
            slot[static_cast<size_t>(i)] = i;
        for (int i = c_lines - 1; i > 0; --i)
            std::swap(slot[static_cast<size_t>(i)], slot[static_cast<size_t>(rng.next() % static_cast<std::uint32_t>(i + 1))]);
        for (size_t i = 0; i < c_lines; ++i)
        {
            const float spot = 0.1f + 0.85f * rng.unit();
            const float ms = c_diffMs[d] * (static_cast<float>(slot[i]) + spot) / static_cast<float>(c_lines);
            m_diffDelay[d][i] = std::max(1, static_cast<int>(ms * 0.001f * m_fs));
            m_diff[d][i].prepare(m_diffDelay[d][i] + 1);
            m_diffSign[d][i] = (rng.next() & 1u) != 0 ? 1.0f : -1.0f;
        }
    }

    m_modDepth = 4.0f * m_fs / 48000.0f;
    for (size_t i = 0; i < c_lines; ++i)
    {
        const int n = primeLength(c_roomLineMs[i], m_fs);
        m_length[i] = static_cast<float>(n);
        m_line[i].prepare(n + static_cast<int>(m_modDepth) + 4);
        m_damp[i].set(c_dampHz, m_fs);
        m_lfoInc[i] = 2.0f * c_roomLfoHz[i] / m_fs;
    }

    m_decay = -1.0f;
    setDecay(SpaceReverb::decaySecondsFor(0.3f));
    reset();
}

void RoomReverb::reset()
{
    for (auto& s : m_send)
    {
        s.hp1.reset(); s.hp2.reset(); s.lp.reset();
        s.pre.reset();
    }
    for (auto& round : m_diff)
        for (auto& line : round) line.reset();
    for (size_t i = 0; i < c_lines; ++i)
    {
        m_line[i].reset();
        m_damp[i].reset();
        m_lfoPhase[i] = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(c_lines);
    }
    for (auto& row : m_taps)
        row.fill(0.0f);
}

void RoomReverb::setDecay(float seconds)
{
    seconds = sj::clamp(seconds, 0.1f, 20.0f);
    if (std::abs(seconds - m_decay) < 1.0e-3f)
        return;
    m_decay = seconds;
    for (size_t i = 0; i < c_lines; ++i)
    {
        const float passes = m_length[i] / m_fs;
        m_gainLow[i] = std::pow(10.0f, -3.0f * passes / seconds);
        m_gainHigh[i] = std::pow(10.0f, -3.0f * passes / (seconds * c_highDecayRatio));
    }
}

/** Fast Hadamard transform in place, not scaled: row r of the result is (row r of the 16 x 16 Hadamard matrix) . v. */
void RoomReverb::mix(std::array<float, c_lines>& v)
{
    for (size_t h = 1; h < c_lines; h <<= 1)
        for (size_t i = 0; i < c_lines; i += h << 1)
            for (size_t j = i; j < i + h; ++j)
            {
                const float a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
}

void RoomReverb::process(float* const* channels, int numChannels, int frames, float dry0, float dry1, float wet0, float wet1, const Taps& taps)
{
    numChannels = std::min(numChannels, c_maxChannels);
    const float step = 1.0f / static_cast<float>(std::max(1, frames));

    // Which rows each channel reads this call (either end of its glide), so the return costs what is used.
    std::array<std::array<std::uint8_t, c_lines>, c_maxChannels> rows {};
    std::array<int, c_maxChannels> numRows {};
    for (int c = 0; c < numChannels; ++c)
    {
        const auto cc = static_cast<size_t>(c);
        for (int r = 0; r < c_lines; ++r)
            if (m_taps[cc][static_cast<size_t>(r)] != 0.0f || taps[cc][static_cast<size_t>(r)] != 0.0f)
                rows[cc][static_cast<size_t>(numRows[cc]++)] = static_cast<std::uint8_t>(r);
    }

    for (int n = 0; n < frames; ++n)
    {
        const float t = static_cast<float>(n + 1) * step;
        const float dry = dry0 + (dry1 - dry0) * t, wet = wet0 + (wet1 - wet0) * t;

        // The send: every channel on its own row.
        std::array<float, c_lines> in {};
        for (int c = 0; c < numChannels; ++c)
        {
            auto& s = m_send[static_cast<size_t>(c)];
            s.pre.push(s.lp.lowpass(s.hp2.highpass(s.hp1.highpass(channels[c][n]))));
            in[static_cast<size_t>(c)] = s.pre.buffer[static_cast<size_t>((s.pre.write - m_preDelay) & s.pre.mask)];
        }
        mix(in);
        for (auto& x : in) x *= c_roomNorm;

        for (size_t d = 0; d < c_diffusers; ++d)
        {
            for (size_t i = 0; i < c_lines; ++i)
            {
                auto& line = m_diff[d][i];
                const float x = in[i];
                in[i] = m_diffSign[d][i] * line.buffer[static_cast<size_t>((line.write - m_diffDelay[d][i]) & line.mask)];
                line.push(x);
            }
            mix(in);
            for (auto& x : in) x *= c_roomNorm;
        }

        std::array<float, c_lines> v, heard;
        for (size_t i = 0; i < c_lines; ++i)
        {
            float p = m_lfoPhase[i] + m_lfoInc[i];
            if (p >= 1.0f) p -= 2.0f;
            m_lfoPhase[i] = p;
            const float lfo = 4.0f * p * (1.0f - std::abs(p));
            const float x = m_line[i].read(static_cast<double>(m_length[i] + m_modDepth * lfo));
            heard[i] = x + c_early * in[i];
            v[i] = m_gainHigh[i] * x + (m_gainLow[i] - m_gainHigh[i]) * m_damp[i].lowpass(x);
        }

        mix(v);
        for (size_t i = 0; i < c_lines; ++i)
            m_line[i].push(c_roomNorm * v[i] + in[i]);

        // The returns: the rows of what is heard, each speaker its own mix of them.
        mix(heard);
        for (int c = 0; c < numChannels; ++c)
        {
            const auto cc = static_cast<size_t>(c);
            float out = 0.0f;
            for (int k = 0; k < numRows[cc]; ++k)
            {
                const size_t r = rows[cc][static_cast<size_t>(k)];
                out += (m_taps[cc][r] + (taps[cc][r] - m_taps[cc][r]) * t) * heard[r];
            }
            channels[c][n] = dry * channels[c][n] + wet * c_roomNorm * out;
        }
    }
    for (int c = 0; c < numChannels; ++c)
        m_taps[static_cast<size_t>(c)] = taps[static_cast<size_t>(c)];
}
