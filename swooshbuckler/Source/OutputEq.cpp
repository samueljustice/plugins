#include "OutputEq.h"

#include <algorithm>
#include <cmath>
#include <complex>

// See OutputEq.h for the design. The sections here, in order: 0 and 1 the Low Cut, 2 the Low Shelf, 3 the Low Mid,
// 4 the High Mid, 5 the High Shelf, 6 the High Cut.

namespace
{
    constexpr double c_pi = 3.14159265358979323846;
    constexpr double c_qFlat = 0.70710678118654752;  // a 12 dB an octave cut, and the shelves
    constexpr double c_qFirst = 0.54119610014619698; // the two sections of a 24 dB an octave cut
    constexpr double c_qSecond = 1.30656296487637653;
    constexpr double c_topOfBand = 0.49;             // of the sample rate: no section is tuned above it
    constexpr double c_maxG = 32.0;                  // (tan at 0.49 is 31.8; a boosted high shelf may ask for more)
    constexpr double c_tiny = 1.0e-60, c_huge = 1.0e30;

    using Coefs = OutputEq::Coefs;

    double gFor(double octave, double sampleRate) { return std::tan(c_pi * std::exp2(octave) / sampleRate); }
    double ampFor(double db) { return std::pow(10.0, db / 40.0); }

    Coefs bellCoefs(double octave, double db, double q, double sampleRate)
    {
        const double a = ampFor(db);
        Coefs c;
        c.g = gFor(octave, sampleRate);
        c.k = 1.0 / (q * a);
        c.m0 = 1.0;
        c.m1 = c.k * (a * a - 1.0);
        c.m2 = 0.0;
        return c;
    }

    Coefs lowShelfCoefs(double octave, double db, double sampleRate)
    {
        const double a = ampFor(db);
        Coefs c;
        c.g = gFor(octave, sampleRate) / std::sqrt(a);
        c.k = 1.0 / c_qFlat;
        c.m0 = 1.0;
        c.m1 = c.k * (a - 1.0);
        c.m2 = a * a - 1.0;
        return c;
    }

    Coefs highShelfCoefs(double octave, double db, double sampleRate)
    {
        const double a = ampFor(db);
        Coefs c;
        c.g = std::min(c_maxG, gFor(octave, sampleRate) * std::sqrt(a));
        c.k = 1.0 / c_qFlat;
        c.m0 = a * a;
        c.m1 = c.k * (1.0 - a) * a;
        c.m2 = 1.0 - a * a;
        return c;
    }

    /** A section's output blended with its input: mix 1 is the section, 0 leaves the sound as it is. */
    Coefs mixed(double g, double k, double m0, double m1, double m2, double mix)
    {
        Coefs c;
        c.g = g;
        c.k = k;
        c.m0 = 1.0 + mix * (m0 - 1.0);
        c.m1 = mix * m1;
        c.m2 = mix * m2;
        return c;
    }

    /** The Low Cut's sections: slope 0 is 12 dB an octave (the first alone at Q 0.7071), 1 is 24 (both). */
    Coefs lowCutSection(int section, double octave, double mix, double slope, double sampleRate)
    {
        const double g = gFor(octave, sampleRate);
        if (section == 0)
        {
            const double k = 1.0 / (c_qFlat * std::pow(c_qFirst / c_qFlat, slope));
            return mixed(g, k, 1.0, -k, -1.0, mix);
        }
        const double k = 1.0 / c_qSecond;
        return mixed(g, k, 1.0, -k, -1.0, mix * slope);
    }

    Coefs highCutSection(double octave, double mix, double sampleRate)
    {
        return mixed(gFor(octave, sampleRate), 1.0 / c_qFlat, 0.0, 0.0, 1.0, mix);
    }

    double octaveOf(double hz, double sampleRate)
    {
        return std::log2(std::clamp(hz, 0.5, c_topOfBand * sampleRate));
    }

    double clampDb(double db) { return std::clamp(db, -OutputEq::c_maxDb, OutputEq::c_maxDb); }
    double clampQ(double q) { return std::clamp(q, 0.1, 20.0); }

    void setA(double g, double k, double& a1, double& a2, double& a3)
    {
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    constexpr int c_row = OutputEq::c_maxChannels; // the work buffer holds one row of lanes per sample

    // The two inner loops, for L lanes at a time (L is known when compiling, so the lanes run side by side).
    template <int L>
    void runSteady(double* buf, int n, const Coefs& c, double a1, double a2, double a3, double* state1, double* state2)
    {
        double ic1[L], ic2[L];
        for (int l = 0; l < L; ++l)
        {
            ic1[l] = state1[l];
            ic2[l] = state2[l];
        }
        const double m0 = c.m0, m1 = c.m1, m2 = c.m2;
        for (int i = 0; i < n; ++i)
        {
            double* x = buf + i * c_row;
            for (int l = 0; l < L; ++l)
            {
                const double v3 = x[l] - ic2[l];
                const double v1 = a1 * ic1[l] + a2 * v3;
                const double v2 = ic2[l] + a2 * ic1[l] + a3 * v3;
                ic1[l] = 2.0 * v1 - ic1[l];
                ic2[l] = 2.0 * v2 - ic2[l];
                x[l] = m0 * x[l] + m1 * v1 + m2 * v2;
            }
        }
        for (int l = 0; l < L; ++l)
        {
            state1[l] = ic1[l];
            state2[l] = ic2[l];
        }
    }

    template <int L>
    void runMoving(double* buf, int n, Coefs& now, const Coefs& step, double* state1, double* state2)
    {
        double ic1[L], ic2[L];
        for (int l = 0; l < L; ++l)
        {
            ic1[l] = state1[l];
            ic2[l] = state2[l];
        }
        Coefs c = now;
        for (int i = 0; i < n; ++i)
        {
            c.g += step.g;
            c.k += step.k;
            c.m0 += step.m0;
            c.m1 += step.m1;
            c.m2 += step.m2;
            double a1, a2, a3;
            setA(c.g, c.k, a1, a2, a3);
            double* x = buf + i * c_row;
            for (int l = 0; l < L; ++l)
            {
                const double v3 = x[l] - ic2[l];
                const double v1 = a1 * ic1[l] + a2 * v3;
                const double v2 = ic2[l] + a2 * ic1[l] + a3 * v3;
                ic1[l] = 2.0 * v1 - ic1[l];
                ic2[l] = 2.0 * v2 - ic2[l];
                x[l] = c.m0 * x[l] + c.m1 * v1 + c.m2 * v2;
            }
        }
        now = c;
        for (int l = 0; l < L; ++l)
        {
            state1[l] = ic1[l];
            state2[l] = ic2[l];
        }
    }

    template <int L>
    void runSection(double* buf, int n, bool moving, Coefs& now, const Coefs& step, double a1, double a2, double a3, double* s1, double* s2)
    {
        if (moving)
            runMoving<L>(buf, n, now, step, s1, s2);
        else
            runSteady<L>(buf, n, now, a1, a2, a3, s1, s2);
    }
}

//==============================================================================
bool OutputEq::isFlat(const Params& p)
{
    if (! p.on)
        return true;
    return p.lowCutHz <= 0.0f && p.highCutHz <= 0.0f && p.lowShelfDb == 0.0f && p.lowMidDb == 0.0f && p.highMidDb == 0.0f
        && p.highShelfDb == 0.0f;
}

OutputEq::BandCoefs OutputEq::coefsFor(const Params& p, int band, double sampleRate)
{
    BandCoefs out;
    if (! p.on || sampleRate <= 0.0)
        return out;
    auto one = [&out](const Coefs& c) { out.section[static_cast<size_t>(out.count++)] = c; };
    switch (band)
    {
        case LowCut:
            if (p.lowCutHz > 0.0f)
            {
                const double slope = p.lowCutSlope >= 24 ? 1.0 : 0.0;
                one(lowCutSection(0, octaveOf(p.lowCutHz, sampleRate), 1.0, slope, sampleRate));
                if (slope > 0.0)
                    one(lowCutSection(1, octaveOf(p.lowCutHz, sampleRate), 1.0, slope, sampleRate));
            }
            break;
        case LowShelf:
            if (p.lowShelfDb != 0.0f)
                one(lowShelfCoefs(octaveOf(p.lowShelfHz, sampleRate), clampDb(p.lowShelfDb), sampleRate));
            break;
        case LowMid:
            if (p.lowMidDb != 0.0f)
                one(bellCoefs(octaveOf(p.lowMidHz, sampleRate), clampDb(p.lowMidDb), clampQ(p.lowMidQ), sampleRate));
            break;
        case HighMid:
            if (p.highMidDb != 0.0f)
                one(bellCoefs(octaveOf(p.highMidHz, sampleRate), clampDb(p.highMidDb), clampQ(p.highMidQ), sampleRate));
            break;
        case HighShelf:
            if (p.highShelfDb != 0.0f)
                one(highShelfCoefs(octaveOf(p.highShelfHz, sampleRate), clampDb(p.highShelfDb), sampleRate));
            break;
        case HighCut:
            if (p.highCutHz > 0.0f)
                one(highCutSection(octaveOf(p.highCutHz, sampleRate), 1.0, sampleRate));
            break;
        default:
            break;
    }
    return out;
}

double OutputEq::responseDb(const Params& p, double hz, double sampleRate, int band)
{
    if (! p.on || sampleRate <= 0.0)
        return 0.0;
    // The trapezoidal section is its analogue shape with s = j tan(pi f / fs) / g.
    const double t = std::tan(c_pi * std::clamp(hz, 0.0, 0.4999 * sampleRate) / sampleRate);
    double power = 1.0;
    for (int b = (band < 0 ? 0 : band); b < (band < 0 ? static_cast<int>(c_bands) : band + 1) && b < c_bands; ++b)
    {
        const auto coefs = coefsFor(p, b, sampleRate);
        for (int i = 0; i < coefs.count; ++i)
        {
            const auto& c = coefs.section[static_cast<size_t>(i)];
            const std::complex<double> s(0.0, t / c.g);
            const auto h = c.m0 + (c.m1 * s + c.m2) / (s * s + c.k * s + 1.0);
            power *= std::norm(h);
        }
    }
    return 10.0 * std::log10(std::max(power, 1.0e-30));
}

//==============================================================================
void OutputEq::prepare(double sampleRate, int numChannels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels = numChannels < 1 ? 1 : (numChannels > c_maxChannels ? c_maxChannels : numChannels);
    m_lanes = (m_channels + 1) & ~1;
    m_glideChunks = std::max(1, static_cast<int>(std::ceil(c_glideSeconds * m_sampleRate / c_chunk)));
    m_fadeChunks = std::max(1, static_cast<int>(std::ceil(c_fadeSeconds * m_sampleRate / c_chunk)));
    reset();
}

void OutputEq::reset()
{
    m_phase = 0;
    startChunk(true);
}

double OutputEq::clampOctave(double hz) const { return octaveOf(hz, m_sampleRate); }

OutputEq::Coefs OutputEq::lowCutCoefs(int section) const
{
    return lowCutSection(section, m_lowCut.octave.cur, m_lowCut.mix.cur, m_lowCut.slope.cur, m_sampleRate);
}

OutputEq::Coefs OutputEq::highCutCoefs() const { return highCutSection(m_highCut.octave.cur, m_highCut.mix.cur, m_sampleRate); }

void OutputEq::start(Section& s, const Coefs& at)
{
    s.ic1.fill(0.0);
    s.ic2.fill(0.0);
    s.now = s.end = at;
    s.step = Coefs { 0.0, 0.0, 0.0, 0.0, 0.0 };
    s.moving = false;
    setA(at.g, at.k, s.a1, s.a2, s.a3);
    if (! s.running)
    {
        s.running = true;
        ++m_running;
    }
}

void OutputEq::stop(Section& s)
{
    s.moving = false;
    if (s.running)
    {
        s.running = false;
        --m_running;
    }
}

void OutputEq::aim(Section& s, const Coefs& end, bool moved)
{
    if (! moved)
    {
        s.moving = false;
        return;
    }
    const double per = 1.0 / c_chunk;
    s.end = end;
    s.step.g = (end.g - s.now.g) * per;
    s.step.k = (end.k - s.now.k) * per;
    s.step.m0 = (end.m0 - s.now.m0) * per;
    s.step.m1 = (end.m1 - s.now.m1) * per;
    s.step.m2 = (end.m2 - s.now.m2) * per;
    s.moving = true;
}

/** The start of each 32 samples: reads the settings, moves every glide one chunk on and says where each section
    must be by the chunk's end. With `snap`, everything jumps to the settings and the filters are emptied. */
void OutputEq::startChunk(bool snap)
{
    const Params& p = m_target;
    const double rate = m_sampleRate;

    // The four bands with a gain. A band is in only while its gain is, or is on its way to or from, something.
    struct Gained
    {
        int section;
        float hz, db, q;
    };
    const Gained gained[4] = { { 2, p.lowShelfHz, p.lowShelfDb, 1.0f },
                               { 3, p.lowMidHz, p.lowMidDb, p.lowMidQ },
                               { 4, p.highMidHz, p.highMidDb, p.highMidQ },
                               { 5, p.highShelfHz, p.highShelfDb, 1.0f } };
    for (int i = 0; i < 4; ++i)
    {
        Bell& b = m_bells[static_cast<size_t>(i)];
        Section& s = m_sections[static_cast<size_t>(gained[i].section)];
        const double octave = clampOctave(gained[i].hz), q = std::log2(clampQ(gained[i].q));
        const double db = p.on ? clampDb(gained[i].db) : 0.0;
        auto coefs = [&]() {
            return i == 0 ? lowShelfCoefs(b.octave.cur, b.db.cur, rate)
                 : i == 3 ? highShelfCoefs(b.octave.cur, b.db.cur, rate)
                          : bellCoefs(b.octave.cur, b.db.cur, std::exp2(b.logQ.cur), rate);
        };
        if (snap)
        {
            b.octave.snap(octave);
            b.logQ.snap(q);
            b.db.snap(db);
            b.in = db != 0.0;
            if (b.in)
                start(s, coefs());
            else
                stop(s);
            continue;
        }
        if (! b.in)
        {
            if (db == 0.0)
                continue;
            // Coming in: from 0 dB at the frequency and Q asked for, with its filter empty.
            b.in = true;
            b.octave.snap(octave);
            b.logQ.snap(q);
            b.db.snap(0.0);
            start(s, coefs());
        }
        b.octave.set(octave, m_glideChunks);
        b.logQ.set(q, m_glideChunks);
        b.db.set(db, m_glideChunks);
        bool moved = b.octave.advance();
        moved = b.logQ.advance() || moved;
        moved = b.db.advance() || moved;
        if (! moved && b.db.cur == 0.0)
        {
            b.in = false; // settled at 0 dB: the last chunk ended on the sound as it came in
            stop(s);
            continue;
        }
        aim(s, moved ? coefs() : s.now, moved);
    }

    // The cuts. Off is not a frequency: the cut glides to its end stop, fades out there and is skipped.
    for (int which = 0; which < 2; ++which)
    {
        const bool low = which == 0;
        Cut& c = low ? m_lowCut : m_highCut;
        Section& first = m_sections[low ? 0 : 6];
        Section* second = low ? &m_sections[1] : nullptr;
        const float hz = low ? p.lowCutHz : p.highCutHz;
        const bool want = p.on && hz > 0.0f;
        const double stopOctave = clampOctave(low ? c_lowStopHz : c_highStopHz);
        const double slope = low && p.lowCutSlope >= 24 ? 1.0 : 0.0;
        auto coefs = [&](int section) { return low ? lowCutCoefs(section) : highCutCoefs(); };
        if (snap)
        {
            c.in = want;
            c.octave.snap(want ? clampOctave(hz) : stopOctave);
            c.mix.snap(want ? 1.0 : 0.0);
            c.slope.snap(slope);
            if (want)
                start(first, coefs(0));
            else
                stop(first);
            if (second != nullptr)
            {
                if (want && slope > 0.0)
                    start(*second, coefs(1));
                else
                    stop(*second);
            }
            continue;
        }
        if (! c.in)
        {
            if (! want)
            {
                c.slope.snap(slope);
                continue;
            }
            c.in = true;
            c.octave.snap(stopOctave);
            c.mix.snap(0.0);
            c.slope.snap(slope);
            start(first, coefs(0));
        }
        // (Before the glides move: a second section coming in starts empty, at no mix.)
        if (second != nullptr && ! second->running && (c.slope.cur > 0.0 || slope > 0.0))
            start(*second, coefs(1));
        if (want)
        {
            c.octave.set(clampOctave(hz), m_glideChunks);
            c.mix.set(1.0, m_fadeChunks);
        }
        else
        {
            c.octave.set(stopOctave, m_glideChunks);
            if (! c.octave.moving())
                c.mix.set(0.0, m_fadeChunks);
        }
        c.slope.set(slope, m_glideChunks);
        bool moved = c.octave.advance();
        moved = c.mix.advance() || moved;
        moved = c.slope.advance() || moved;
        if (! moved && ! want && c.mix.cur == 0.0)
        {
            c.in = false;
            stop(first);
            if (second != nullptr)
                stop(*second);
            continue;
        }
        aim(first, moved ? coefs(0) : first.now, moved);
        if (second != nullptr && second->running)
        {
            if (! moved && c.slope.cur == 0.0)
                stop(*second); // 12 dB an octave, settled
            else
                aim(*second, moved ? coefs(1) : second->now, moved);
        }
    }
}

void OutputEq::process(float* const* channels, int numChannels, int numSamples)
{
    const int chans = std::min(numChannels, static_cast<int>(c_maxChannels));
    if (chans <= 0)
        return;
    const int lanes = (chans + 1) & ~1;
    alignas(64) double buf[c_chunk * c_maxChannels];

    for (int pos = 0; pos < numSamples;)
    {
        if (m_phase == 0)
            startChunk(false);
        const int n = std::min(c_chunk - m_phase, numSamples - pos);
        if (m_running > 0)
        {
            for (int ch = 0; ch < chans; ++ch)
            {
                const float* in = channels[ch] + pos;
                for (int i = 0; i < n; ++i)
                    buf[i * c_row + ch] = static_cast<double>(in[i]);
            }
            if (lanes > chans)
                for (int i = 0; i < n; ++i)
                    buf[i * c_row + chans] = 0.0;

            for (auto& s : m_sections)
            {
                if (! s.running)
                    continue;
                double* s1 = s.ic1.data();
                double* s2 = s.ic2.data();
                switch (lanes)
                {
                    case 2:  runSection<2>(buf, n, s.moving, s.now, s.step, s.a1, s.a2, s.a3, s1, s2); break;
                    case 4:  runSection<4>(buf, n, s.moving, s.now, s.step, s.a1, s.a2, s.a3, s1, s2); break;
                    case 6:  runSection<6>(buf, n, s.moving, s.now, s.step, s.a1, s.a2, s.a3, s1, s2); break;
                    case 8:  runSection<8>(buf, n, s.moving, s.now, s.step, s.a1, s.a2, s.a3, s1, s2); break;
                    case 10: runSection<10>(buf, n, s.moving, s.now, s.step, s.a1, s.a2, s.a3, s1, s2); break;
                    case 12: runSection<12>(buf, n, s.moving, s.now, s.step, s.a1, s.a2, s.a3, s1, s2); break;
                    case 14: runSection<14>(buf, n, s.moving, s.now, s.step, s.a1, s.a2, s.a3, s1, s2); break;
                    default: runSection<16>(buf, n, s.moving, s.now, s.step, s.a1, s.a2, s.a3, s1, s2); break;
                }
            }

            for (int ch = 0; ch < chans; ++ch)
            {
                float* out = channels[ch] + pos;
                for (int i = 0; i < n; ++i)
                    out[i] = static_cast<float>(buf[i * c_row + ch]);
            }
        }
        m_phase += n;
        pos += n;
        if (m_phase >= c_chunk)
        {
            // The chunk's end: a moving section lands exactly where it was aimed, and a state that has died away
            // (or was handed something that is not a number) is emptied.
            m_phase = 0;
            for (auto& s : m_sections)
            {
                if (! s.running)
                    continue;
                if (s.moving)
                {
                    s.now = s.end;
                    setA(s.now.g, s.now.k, s.a1, s.a2, s.a3);
                }
                for (int l = 0; l < lanes; ++l)
                {
                    const size_t i = static_cast<size_t>(l);
                    const double m1 = std::abs(s.ic1[i]), m2 = std::abs(s.ic2[i]);
                    if (! (m1 >= c_tiny && m1 < c_huge))
                        s.ic1[i] = 0.0;
                    if (! (m2 >= c_tiny && m2 < c_huge))
                        s.ic2[i] = 0.0;
                }
            }
        }
    }
}
