#include "DemoSounds.h"

#include <cmath>

namespace
{
    constexpr double c_fs = DemoSounds::c_sampleRate;
    constexpr double c_pi = 3.14159265358979323846;
    constexpr double c_twoPi = 2.0 * c_pi;

    int samples(double seconds) { return static_cast<int>(std::lround(seconds * c_fs)); }

    /** White noise, -1..1, the same every time for a seed. */
    struct Noise
    {
        explicit Noise(std::uint32_t seed) : m_state(seed * 2654435761u + 0x9e3779b9u) {}
        float operator()()
        {
            m_state ^= m_state << 13;
            m_state ^= m_state >> 17;
            m_state ^= m_state << 5;
            return static_cast<float>(m_state) * (2.0f / 4294967295.0f) - 1.0f;
        }
        /** 0..1 */
        float unit() { return 0.5f * ((*this)() + 1.0f); }
    private:
        std::uint32_t m_state;
    };

    struct OnePole
    {
        float z = 0.0f;
        float low(float x, double cutoffHz)
        {
            const float a = static_cast<float>(1.0 - std::exp(-c_twoPi * cutoffHz / c_fs));
            z += a * (x - z);
            return z;
        }
    };

    /** A state-variable filter whose frequency can move every sample (the band-pass and low-pass outputs). */
    struct Svf
    {
        float ic1 = 0.0f, ic2 = 0.0f, band = 0.0f, low = 0.0f;
        void run(float x, double hz, double q)
        {
            const double g = std::tan(c_pi * std::min(hz, 0.45 * c_fs) / c_fs);
            const double k = 1.0 / q;
            const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
            const double v3 = x - ic2;
            const double v1 = a1 * ic1 + a2 * v3;
            const double v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = static_cast<float>(2.0 * v1 - ic1);
            ic2 = static_cast<float>(2.0 * v2 - ic2);
            band = static_cast<float>(v1);
            low = static_cast<float>(v2);
        }
    };

    float smoothstep(float t)
    {
        t = juce::jlimit(0.0f, 1.0f, t);
        return t * t * (3.0f - 2.0f * t);
    }

    /** Brings the loudest sample to `peak`. */
    void normalise(juce::AudioBuffer<float>& b, float peak)
    {
        const float mag = b.getMagnitude(0, b.getNumSamples());
        if (mag > 1.0e-6f)
            b.applyGain(peak / mag);
    }
}

//==============================================================================
juce::AudioBuffer<float> DemoSounds::growl()
{
    // Five phrases of a creature's voice: a buzzing pulse through three formants that open and close like a mouth,
    // and some breath on top. Every other pulse is weaker, which is what makes a growl rough and drops what you
    // hear an octave under the pulse rate (so the pitch heard is about 62 to 176 Hz).
    struct Phrase
    {
        double seconds, gap;
        double f0Start, f0Peak, f0End;        // pulses a second: they rise to the peak a third of the way in, then fall
        double mouthStart, mouthEnd;          // 0 = closed ("oo"), 1 = open ("aa")
        double rough;                         // how much weaker every other pulse is
        float level;
    };
    const Phrase phrases[] = {
        { 1.10, 0.45, 164.0, 208.0, 140.0, 0.25, 0.85, 0.45, 0.90f },
        { 0.70, 0.40, 236.0, 300.0, 192.0, 0.70, 0.30, 0.30, 0.75f },
        { 1.60, 0.50, 136.0, 184.0, 124.0, 0.15, 1.00, 0.60, 1.00f },
        { 0.55, 0.35, 280.0, 352.0, 240.0, 0.90, 0.50, 0.25, 0.70f },
        { 1.30, 0.45, 180.0, 252.0, 128.0, 0.50, 0.95, 0.50, 0.95f },
    };

    double total = 0.2;
    for (const auto& p : phrases)
        total += p.seconds + p.gap;
    juce::AudioBuffer<float> b(1, samples(total));
    b.clear();
    float* out = b.getWritePointer(0);

    Noise noise(7);
    int at = samples(0.2);
    for (const auto& p : phrases)
    {
        const int len = samples(p.seconds);
        Svf f1, f2, f3, breath;
        OnePole body, wobble;
        double phase = 0.0;
        bool weak = false;
        for (int i = 0; i < len; ++i)
        {
            const double t = static_cast<double>(i) / len;
            const double rise = t < 0.33 ? t / 0.33 : 1.0 - (t - 0.33) / 0.67;
            const double centre = t < 0.33 ? p.f0Start + (p.f0Peak - p.f0Start) * std::sin(0.5 * c_pi * rise)
                                           : p.f0End + (p.f0Peak - p.f0End) * rise * rise;
            // A slow, uneven wobble of the pitch (under 1%): a throat, not an oscillator.
            const double drift = 1.0 + 0.008 * wobble.low(noise(), 9.0) * 6.0;
            phase += centre * drift / c_fs;
            if (phase >= 1.0)
            {
                phase -= 1.0;
                weak = ! weak;
            }
            // The pulse: a sharp closing edge (bright) after a rounded opening.
            const double open = std::pow(std::sin(c_pi * std::min(1.0, phase / 0.62)), 2.0);
            const double pulse = (phase < 0.62 ? open : 0.0) - 0.28;
            const double amount = weak ? 1.0 - p.rough : 1.0;
            const auto source = static_cast<float>(pulse * amount);

            const double mouth = p.mouthStart + (p.mouthEnd - p.mouthStart) * smoothstep(static_cast<float>(t * 1.2));
            f1.run(source, 320.0 + 430.0 * mouth, 5.0);
            f2.run(source, 900.0 + 500.0 * mouth, 7.0);
            f3.run(source, 2300.0 + 500.0 * mouth, 8.0);
            breath.run(noise(), 1800.0 + 1200.0 * mouth, 1.2);

            const float attack = smoothstep(static_cast<float>(i) / static_cast<float>(samples(0.07)));
            const float release = smoothstep(static_cast<float>(len - i) / static_cast<float>(samples(0.16)));
            const float swell = 0.75f + 0.25f * static_cast<float>(std::sin(c_pi * t));
            const float env = attack * release * swell;

            // The voice is its formants; only a little of the raw pulse (the chest) under them.
            float v = 0.12f * body.low(source, 300.0) + 0.55f * f1.band + 0.5f * f2.band + 0.3f * f3.band;
            v = std::tanh(1.3f * v);                                   // a little grit, and a level that holds
            v += 0.05f * breath.band * static_cast<float>(0.4 + 0.6 * mouth);
            out[at + i] = p.level * env * v;
        }
        at += len + samples(p.gap);
    }
    normalise(b, 0.8f);
    return b;
}

//==============================================================================
juce::AudioBuffer<float> DemoSounds::windBed()
{
    // Wind: noise with most of its weight low down, through two bands that drift, in gusts that both sides share
    // (so it holds together in mono) while the hiss itself is different left and right (so it is wide).
    const double seconds = 8.0, seam = 0.6;
    const int len = samples(seconds), extra = samples(seam);
    juce::AudioBuffer<float> made(2, len + extra);
    for (int ch = 0; ch < 2; ++ch)
    {
        Noise shared(20), own(ch == 0 ? 21u : 22u); // the same hiss on both sides, and some of each side's own
        OnePole pink1, pink2, low;
        Svf bandA, bandB;
        float* out = made.getWritePointer(ch);
        for (int i = 0; i < len + extra; ++i)
        {
            const double t = i / c_fs;
            const float white = 0.8f * shared() + 0.6f * own();
            // Roughly pink: white with two low-passed copies of itself laid over it.
            const float pink = 0.35f * white + 1.6f * pink1.low(white, 900.0) + 3.5f * pink2.low(white, 140.0);
            // Whole numbers of cycles in the loop, so the gusts join up at the seam.
            const double gust = 0.72 + 0.16 * std::sin(c_twoPi * 1.0 * t / seconds + 0.4)
                                     + 0.09 * std::sin(c_twoPi * 3.0 * t / seconds + 1.9)
                                     + 0.05 * std::sin(c_twoPi * 5.0 * t / seconds + (ch == 0 ? 0.3 : 1.1));
            bandA.run(pink, 420.0 * std::pow(2.0, 0.7 * std::sin(c_twoPi * 2.0 * t / seconds + 0.8)), 0.9);
            bandB.run(white, 2400.0 * std::pow(2.0, 0.5 * std::sin(c_twoPi * 3.0 * t / seconds + 2.6)), 1.4);
            const float v = 0.9f * bandA.band + 0.35f * bandB.band * static_cast<float>(gust) + 0.5f * low.low(pink, 110.0);
            out[i] = static_cast<float>(gust) * v;
        }
    }

    // The loop: the stretch after the end is faded in over the beginning, so the end runs into the start.
    juce::AudioBuffer<float> b(2, len);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* in = made.getReadPointer(ch);
        float* out = b.getWritePointer(ch);
        for (int i = 0; i < len; ++i)
        {
            if (i < extra)
            {
                const double x = static_cast<double>(i) / extra;
                out[i] = static_cast<float>(std::sin(0.5 * c_pi * x)) * in[i] + static_cast<float>(std::cos(0.5 * c_pi * x)) * in[len + i];
            }
            else
                out[i] = in[i];
        }
    }
    normalise(b, 0.7f);
    return b;
}

//==============================================================================
juce::AudioBuffer<float> DemoSounds::swish()
{
    // Air swishes, as from a stick or a blade: hiss through a band that sweeps up as the swing speeds up and back
    // down as it slows, loudest a little past the middle.
    struct Swing
    {
        double seconds, gap;
        double lowHz, highHz;   // where the band starts and ends, and how high it gets
        double q;
        float level;
    };
    const Swing swings[] = {
        { 0.55, 0.45,  700.0, 5200.0, 1.5, 0.95f },
        { 0.90, 0.50,  450.0, 3200.0, 1.1, 0.85f },
        { 0.38, 0.40, 1100.0, 7000.0, 1.9, 0.90f },
        { 1.20, 0.55,  350.0, 2600.0, 1.0, 0.80f },
        { 0.65, 0.45,  800.0, 4400.0, 1.4, 1.00f },
        { 0.45, 0.45,  950.0, 6000.0, 1.7, 0.85f },
    };

    double total = 0.25;
    for (const auto& s : swings)
        total += s.seconds + s.gap;
    juce::AudioBuffer<float> b(1, samples(total));
    b.clear();
    float* out = b.getWritePointer(0);

    Noise noise(33);
    int at = samples(0.25);
    for (const auto& s : swings)
    {
        const int len = samples(s.seconds);
        Svf band, air;
        for (int i = 0; i < len; ++i)
        {
            const double t = static_cast<double>(i) / len;
            const double shaped = t < 0.58 ? 0.5 * t / 0.58 : 0.5 + 0.5 * (t - 0.58) / 0.42; // the peak at 58%
            const double swell = std::pow(std::sin(c_pi * shaped), 2.0);
            const float white = noise();
            band.run(white, s.lowHz * std::pow(s.highHz / s.lowHz, swell), s.q);
            air.run(white, 6500.0, 0.8);
            out[at + i] = s.level * static_cast<float>(swell) * (band.band + 0.2f * static_cast<float>(swell) * (white - air.low));
        }
        at += len + samples(s.gap);
    }
    normalise(b, 0.75f);
    return b;
}

//==============================================================================
juce::AudioBuffer<float> DemoSounds::hit()
{
    // Four impacts with a millisecond attack: a deep thump, a dry crack, a clang and a boom with a rumbling tail.
    const double spacing = 1.0;
    juce::AudioBuffer<float> b(1, samples(0.1 + 4.0 * spacing + 0.2));
    b.clear();
    float* out = b.getWritePointer(0);
    Noise noise(55);

    auto attack = [](double t) { return static_cast<float>(std::min(1.0, t / 0.001)); };

    for (int k = 0; k < 4; ++k)
    {
        const int at = samples(0.1 + k * spacing);
        const int len = samples(0.8);
        Svf crack, tail;
        OnePole dull, debris;
        double phase = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double t = i / c_fs;
            const float white = noise();
            float v = 0.0f;
            switch (k)
            {
                case 0: // thump: a sine that drops from 220 to 66 Hz, with a knock of noise on the front
                {
                    phase += (66.0 + 156.0 * std::exp(-t / 0.05)) / c_fs;
                    crack.run(white, 2600.0, 0.9);
                    v = static_cast<float>(0.6 * std::exp(-t / 0.085) * std::sin(c_twoPi * phase))
                        + 1.2f * static_cast<float>(std::exp(-t / 0.022)) * crack.band
                        + 0.5f * static_cast<float>(std::exp(-t / 0.005)) * white;
                    break;
                }
                case 1: // crack: a burst of noise around 1.9 kHz with a short knock under it
                {
                    crack.run(white, 1900.0, 1.1);
                    phase += 95.0 / c_fs;
                    v = 1.3f * static_cast<float>(std::exp(-t / 0.045)) * crack.band
                        + 0.5f * static_cast<float>(std::exp(-t / 0.012)) * white
                        + 0.3f * static_cast<float>(std::exp(-t / 0.05) * std::sin(c_twoPi * phase));
                    break;
                }
                case 2: // clang: partials that are not in tune with each other, each dying at its own speed
                {
                    const double partials[] = { 212.0, 587.0, 1148.0, 1893.0, 2811.0 };
                    const double decays[] = { 0.22, 0.17, 0.12, 0.09, 0.06 };
                    for (int h = 0; h < 5; ++h)
                        v += static_cast<float>((0.5 / (1.0 + 0.5 * h)) * std::exp(-t / decays[h]) * std::sin(c_twoPi * partials[h] * t + 0.7 * h));
                    v += 0.6f * static_cast<float>(std::exp(-t / 0.004)) * white;
                    break;
                }
                default: // boom: a lower, longer thump with a tail of dull noise
                {
                    phase += (50.0 + 118.0 * std::exp(-t / 0.07)) / c_fs;
                    tail.run(white, 700.0, 0.7);
                    v = static_cast<float>(0.6 * std::exp(-t / 0.13) * std::sin(c_twoPi * phase))
                        + 1.4f * static_cast<float>(std::exp(-t / 0.1)) * dull.low(tail.band, 1800.0)
                        + 0.6f * static_cast<float>(std::exp(-t / 0.008)) * white;
                    break;
                }
            }
            // Every impact throws up some debris: broadband noise that outlasts the body, so none ends as a pure note.
            v += 0.5f * static_cast<float>(std::exp(-t / 0.16)) * debris.low(white, 3500.0);
            const float end = smoothstep(static_cast<float>(len - i) / static_cast<float>(samples(0.05)));
            out[at + i] = attack(t) * end * std::tanh(1.4f * v);
        }
    }
    normalise(b, 0.9f);
    return b;
}

//==============================================================================
std::vector<DemoSounds::Sound> DemoSounds::all()
{
    std::vector<Sound> sounds;
    sounds.push_back({ "Demo Growl", growl() });
    sounds.push_back({ "Demo Wind Bed", windBed() });
    sounds.push_back({ "Demo Swish", swish() });
    sounds.push_back({ "Demo Hit", hit() });
    return sounds;
}
