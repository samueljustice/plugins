#pragma once

#include "SweepFilter.h"
#include "SwirlFx.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>

/**
 * WHAT A BUS'S FILTER AND SWIRL ARE DOING, FOR THE WINDOW (the plate over a bus while its Filter or Swirl layer is
 * open: BusViewPlate). JUCE-free; nothing here allocates or locks.
 *
 * The audio thread (RigEngine::renderBus) writes, once per chunk, a bus's:
 *   - FilterLook: where its filter is (Hz, placed for its type, its smoothing included), its Q, type and slope, how
 *     much of it is heard (the wet mix law and trim()), and the ends of the travel it has with the front wheel where
 *     it is now (from the whoosh at nothing and the rhythm at its lowest to the whoosh at its peak and the rhythm at
 *     its highest, each placed for the type);
 *   - SwirlLook: the Swirl type in use and the state each type is in (the phaser's all-pass coefficient, the
 *     flanger's delay (Phaser + Flanger and Flanger), the Tape Flanger's offset of the copy against the dry, the Doubler's delay; mixes, feedback
 *     and the level match);
 *   - two Traces: the copy's place or the delay over the last few seconds (by type), and how near the bus's
 *     nearest pass is;
 *   - a Tap: the bus's mono sum just after its filter, for the spectrum behind the filter's curve.
 * Every value is its own relaxed atomic, so a reader may see one field a chunk newer than another (a picture, not a
 * calculation). tick counts the chunks, so the window can tell a running engine from one at rest.
 *
 * The maths the plate draws (filterTransfer, swirlTransfer) is the DSP's own, linear: for the filter it is exactly
 * what SweepFilter does once a place has been held (the TPT state-variable filter is its analog prototype at
 * tan(pi f / fs) / tan(pi fc / fs)); SweepFilterTest measures the filter against it with sine tones.
 */
namespace busview
{
    using Complex = std::complex<double>;

    /** The bus filter's transfer at `hz` for a filter held at `cutoffHz` (already placed for its type) and `q`,
        as SweepFilter::setTarget sets it up (cutoff within 10 Hz and 0.45 fs, k = 1 / max(0.05, q)). */
    inline Complex filterTransfer(int type, bool steep, double cutoffHz, double q, double hz, double fs)
    {
        const double cutoff = std::clamp(cutoffHz, 10.0, 0.45 * fs);
        const double w = std::tan(3.14159265358979323846 * std::clamp(hz, 0.0, 0.4999 * fs) / fs)
                         / std::tan(3.14159265358979323846 * cutoff / fs);
        const Complex s(0.0, w);
        const double k = 1.0 / std::max(0.05, q);
        auto stage = [&](double damping)
        {
            const Complex den = s * s + s * damping + 1.0;
            switch (type)
            {
                case SweepFilter::HighPass: return Complex(s * s / den);
                case SweepFilter::BandPass: return Complex(s / den / static_cast<double>(SweepFilter::c_bandQ));
                case SweepFilter::Notch:    return Complex((s * s + 1.0) / den);
                default:                    return Complex(1.0 / den);
            }
        };
        if (! steep)
            return stage(k);
        if (type == SweepFilter::BandPass)
            return stage(1.0 / static_cast<double>(SweepFilter::c_bandQ)) * stage(k);
        if (type == SweepFilter::Notch)
            return stage(k) * stage(k);
        return stage(1.0 / static_cast<double>(SweepFilter::c_steepFirstQ)) * stage(k / static_cast<double>(SweepFilter::c_steepSecondQ));
    }

    /** How much of the filter a bus hears for its front Filter wheel f (0..1): the engine's wet mix law, all of it from
        80% up. */
    inline float filterMix(float f)
    {
        return f <= 0.8f ? (1.0f - std::pow(10.0f, -1.875f * f)) / (1.0f - std::pow(10.0f, -1.5f)) : 1.0f;
    }

    /** A bus's filter as the window draws it. */
    struct FilterLook
    {
        int type = SweepFilter::LowPass;
        bool steep = false;
        float hz = 1000.0f, q = 0.7f;
        float mix = 0.0f;          // how much of the filter is heard (0: mixed out)
        float trim = 1.0f;         // SweepFilter::trim(q), on the filtered stream before it is mixed in
        float lowHz = 20.0f, highHz = 20000.0f; // the travel's ends, placed for the type (lowHz <= highHz)
    };
    /** The filter's gain at `hz` (linear). */
    inline double filterGain(const FilterLook& f, double hz, double fs)
    {
        return std::abs(filterTransfer(f.type, f.steep, f.hz, f.q, hz, fs));
    }
    /** What the bus hears at `hz` (linear): the dry and the trimmed filter mixed by the wet mix law, for a sound
        small enough that the engine's soft clip is straight. */
    inline double heardGain(const FilterLook& f, double hz, double fs)
    {
        const double mix = std::clamp(static_cast<double>(f.mix), 0.0, 1.0);
        return std::abs((1.0 - mix) + mix * static_cast<double>(f.trim) * filterTransfer(f.type, f.steep, f.hz, f.q, hz, fs));
    }

    /** Swirl's type (RigEngine::SwirlType, in the same order). */
    enum SwirlKind { SwirlBoth = 0, SwirlPhaser, SwirlFlanger, SwirlTape, SwirlDoubler, c_swirlKinds };

    /** A bus's Swirl as the window draws it (its first lane: left, or a room's first pass). */
    struct SwirlLook
    {
        int type = SwirlBoth;
        float knob = 0.0f;               // the front Swirl wheel with its modulation (0..1)
        bool on = false;                 // the type's sound is in (Phaser + Flanger, Phaser and Flanger switch off at 0)
        // Phaser + Flanger, Phaser, Flanger.
        float phaserCoef = 0.0f, phaserFeedback = 0.0f, phaserMix = 0.0f, phaserHz = 1000.0f;
        // (phaserHz: where the all-passes turn, a quarter turn each: the middle of the phaser's three notches.)
        float flangerMs = 0.3f, flangerFeedback = 0.0f, flangerMix = 0.0f;
        float level = 1.0f;              // the level match after it (all but the Tape Flanger)
        // Tape Flanger (offsetMs: the copy against the dry, + later) and Doubler (offsetMs: its delay).
        float offsetMs = 0.0f, dryGain = 1.0f, wetGain = 0.0f, loopGain = 0.0f, loopMs = 0.25f;
    };

    /** The same to the bit (the window: has anything moved since the last frame?). */
    inline bool sameBits(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
    inline bool same(const FilterLook& a, const FilterLook& b)
    {
        return a.type == b.type && a.steep == b.steep && sameBits(a.hz, b.hz) && sameBits(a.q, b.q) && sameBits(a.mix, b.mix) && sameBits(a.trim, b.trim)
               && sameBits(a.lowHz, b.lowHz) && sameBits(a.highHz, b.highHz);
    }
    inline bool same(const SwirlLook& a, const SwirlLook& b)
    {
        return a.type == b.type && a.on == b.on && sameBits(a.knob, b.knob) && sameBits(a.phaserCoef, b.phaserCoef) && sameBits(a.phaserFeedback, b.phaserFeedback)
               && sameBits(a.phaserMix, b.phaserMix) && sameBits(a.phaserHz, b.phaserHz) && sameBits(a.flangerMs, b.flangerMs)
               && sameBits(a.flangerFeedback, b.flangerFeedback) && sameBits(a.flangerMix, b.flangerMix) && sameBits(a.level, b.level)
               && sameBits(a.offsetMs, b.offsetMs) && sameBits(a.dryGain, b.dryGain) && sameBits(a.wetGain, b.wetGain) && sameBits(a.loopGain, b.loopGain)
               && sameBits(a.loopMs, b.loopMs);
    }

    constexpr int c_phaserStages = 6;
    constexpr double c_pi = 3.14159265358979323846;

    /** sj::DelayLine::read's transfer at angular frequency `w` (radians a sample) for a read `delay` samples behind
        the newest sample (either sign; the line clamps nothing here): the four-point cubic it is, which loses at the
        top as the fraction nears a half. A whole number of samples is a plain delay. */
    inline Complex cubicRead(double delay, double w)
    {
        const double whole = std::floor(delay);
        const double frac = delay - whole;
        if (frac <= 0.0)
            return std::polar(1.0, -w * delay);
        // The read point is (whole + 1) - t behind the newest sample, t = 1 - frac: x0 at whole + 1, x1 at whole,
        // xm1 at whole + 2, x2 at whole - 1 (DelayLine::locate).
        const double t = 1.0 - frac, t2 = t * t, t3 = t2 * t;
        const double wm1 = -0.5 * t + t2 - 0.5 * t3;
        const double w0 = 1.0 - 2.5 * t2 + 1.5 * t3;
        const double w1 = 0.5 * t + 2.0 * t2 - 1.5 * t3;
        const double w2 = -0.5 * t2 + 0.5 * t3;
        return wm1 * std::polar(1.0, -w * (whole + 2.0)) + w0 * std::polar(1.0, -w * (whole + 1.0))
             + w1 * std::polar(1.0, -w * whole) + w2 * std::polar(1.0, -w * (whole - 1.0));
    }

    /** sj::OnePole::lowpass's transfer at `w` for a corner at `hz`. */
    inline Complex onePole(double hz, double w, double fs)
    {
        const double a = 1.0 - std::exp(-2.0 * c_pi * std::clamp(hz, 1.0, 0.49 * fs) / fs);
        return a / (1.0 - (1.0 - a) * std::polar(1.0, -w));
    }

    /** Swirl's transfer at `hz` (its first lane, linear, for a held state), with every read and filter the engine
        has in it (SwirlFxTest and SwirlWireTest measure it against the sound):
          - Phaser + Flanger, Phaser, Flanger: the six all-passes in their feedback loop, then the flanger's comb (its
            delay read with the line's cubic, in its loop and out of it), each mixed as the engine mixes them, and the
            level match after them;
          - Tape Flanger: the copy against its dry, read with the sinc under SwirlFx::c_sincBelowHz (a plain delay
            here) and with the cubic over it, over the recirculating comb, whose loop reads with the cubic through
            the two 19 kHz one-poles.
        It is the response to a quiet sound: the soft clips in the loops are taken as straight (on a loud sound the
        phaser's flattens its tallest peaks by a dB or so). The Doubler's comb is not given (its notches are tens of
        hertz apart: no picture of them reads), nor the delay every lane has in common. */
    inline Complex swirlTransfer(const SwirlLook& s, double hz, double fs)
    {
        const double w = 2.0 * c_pi * std::clamp(hz, 0.0, 0.5 * fs) / fs;
        const Complex z1 = std::polar(1.0, -w); // z^-1
        if (s.type == SwirlTape)
        {
            const double d = static_cast<double>(s.offsetMs) * 0.001 * fs;
            const double loop = std::max(static_cast<double>(s.loopMs) * 0.001 * fs, std::abs(d));
            // The loop reads one sample short before the new sample goes in: z^-1 x the cubic at loop - 1.
            const Complex lp = onePole(SwirlFx::c_lowPassHz, w, fs);
            const Complex back = static_cast<double>(s.loopGain) * lp * lp * z1 * cubicRead(loop - 1.0, w);
            const Complex read = fs < SwirlFx::c_sincBelowHz ? std::polar(1.0, -w * d) : cubicRead(d, w);
            return static_cast<double>(s.dryGain) + static_cast<double>(s.wetGain) * read / (1.0 - back);
        }
        if (s.type == SwirlDoubler)
            return { static_cast<double>(s.level), 0.0 };
        Complex out(1.0, 0.0);
        if (s.on)
        {
            if (s.type != SwirlFlanger && s.phaserMix > 0.0f)
            {
                const double a = static_cast<double>(s.phaserCoef);
                const Complex ap = (-a + z1) / (1.0 - a * z1);
                Complex chain(1.0, 0.0);
                for (int i = 0; i < c_phaserStages; ++i)
                    chain *= ap;
                const Complex wet = chain / (1.0 - static_cast<double>(s.phaserFeedback) * z1 * chain);
                out = 1.0 + static_cast<double>(s.phaserMix) * (wet - 1.0);
            }
            if (s.type != SwirlPhaser && s.flangerMix > 0.0f)
            {
                // The line is read after the new sample goes in; what it read goes back in with the next one.
                const Complex read = cubicRead(static_cast<double>(s.flangerMs) * 0.001 * fs, w);
                const Complex comb = read / (1.0 - static_cast<double>(s.flangerFeedback) * read * z1);
                out *= 1.0 + static_cast<double>(s.flangerMix) * (comb - 1.0);
            }
        }
        return out * static_cast<double>(s.level);
    }

    /** The phaser's part of swirlTransfer alone (the six all-passes in their loop, mixed in), as a gain. */
    inline double phaserGain(const SwirlLook& s, double hz, double fs)
    {
        const Complex z1 = std::polar(1.0, -2.0 * c_pi * std::clamp(hz, 0.0, 0.5 * fs) / fs);
        const double a = static_cast<double>(s.phaserCoef);
        const Complex ap = (-a + z1) / (1.0 - a * z1);
        Complex chain(1.0, 0.0);
        for (int i = 0; i < c_phaserStages; ++i)
            chain *= ap;
        return std::abs(1.0 + static_cast<double>(s.phaserMix) * (chain / (1.0 - static_cast<double>(s.phaserFeedback) * z1 * chain) - 1.0));
    }

    /** The phaser's lowest notch, in Hz: the lowest dip of phaserGain. Without feedback the three notches are where
        each of the six stages has turned 30, 90 and 150 degrees; Feedback moves the lowest by up to a quarter either
        way, so it is looked for from there (0.6 to 1.6 times it, a hundredth at a step, then the three lowest
        points' parabola). 0 when there is no dip in that span under half the rate (Feedback right up can fill it in). */
    inline double phaserFirstNotchHz(const SwirlLook& s, double fs)
    {
        const double t = std::tan(c_pi * std::clamp(static_cast<double>(s.phaserHz), 1.0, 0.4999 * fs) / fs);
        const double plain = std::atan(std::tan(c_pi / 12.0) * t) * fs / c_pi;
        constexpr int c_steps = 100;
        const double ratio = std::pow(1.6 / 0.6, 1.0 / c_steps);
        double hz = 0.6 * plain, before = phaserGain(s, hz, fs), at = phaserGain(s, hz * ratio, fs);
        for (int i = 1; i < c_steps && hz * ratio * ratio < 0.5 * fs; ++i)
        {
            const double after = phaserGain(s, hz * ratio * ratio, fs);
            if (at < before && at <= after)
            {
                // The parabola through the three (in dB, over the steps): its lowest point.
                const double b = std::log(std::max(before, 1.0e-9)), m = std::log(std::max(at, 1.0e-9)), e = std::log(std::max(after, 1.0e-9));
                const double bend = b - 2.0 * m + e;
                const double shift = bend > 1.0e-12 ? std::clamp(0.5 * (b - e) / bend, -1.0, 1.0) : 0.0;
                return hz * std::pow(ratio, 1.0 + shift);
            }
            hz *= ratio;
            before = at;
            at = after;
        }
        return 0.0;
    }

    /**
     * A few seconds of one value, written a chunk at a time by the audio thread and read by the window: every bucket
     * of samples becomes one point (its lowest and highest value) and the kind it was written in (a Swirl type, so
     * the window draws only the points of the type in use; a point is of one kind throughout). Lock-free; a torn
     * read draws one point slightly wrong.
     */
    class Trace
    {
    public:
        static constexpr int c_points = 240;
        static constexpr double c_seconds = 4.0;

        /** Audio thread (prepare). */
        void prepare(double sampleRate)
        {
            m_bucket = std::max(1, static_cast<int>(sampleRate * c_seconds / c_points));
            m_count = 0;
            m_lo = 1.0e30f;
            m_hi = -1.0e30f;
        }
        /** Audio thread: `value` held for `frames` samples. */
        void push(float value, int frames, int kind)
        {
            if (! std::isfinite(value))
                value = 0.0f;
            if (kind != m_kind)
            {
                // Another kind of value (Swirl's type changed): the point in hand starts again, so none mixes two.
                m_kind = kind;
                m_count = 0;
                m_lo = 1.0e30f;
                m_hi = -1.0e30f;
            }
            m_lo = std::min(m_lo, value);
            m_hi = std::max(m_hi, value);
            m_count += frames;
            if (m_count < m_bucket)
                return;
            const auto w = m_written.load(std::memory_order_relaxed);
            auto& p = m_points[static_cast<size_t>(w % c_points)];
            p.lo.store(m_lo, std::memory_order_relaxed);
            p.hi.store(m_hi, std::memory_order_relaxed);
            p.kind.store(kind, std::memory_order_relaxed);
            m_written.store(w + 1, std::memory_order_release);
            m_count -= m_bucket;
            m_lo = 1.0e30f;
            m_hi = -1.0e30f;
        }
        /** Any thread: how many points have ever been written (the newest is written - 1). */
        std::uint32_t written() const { return m_written.load(std::memory_order_acquire); }
        /** Any thread: the point `age` back from the newest (0 the newest, up to c_points - 1). */
        void point(int age, float& lo, float& hi, int& kind) const
        {
            const auto w = m_written.load(std::memory_order_acquire);
            const auto& p = m_points[static_cast<size_t>((w + static_cast<std::uint32_t>(c_points) - 1u - static_cast<std::uint32_t>(std::clamp(age, 0, c_points - 1))) % c_points)];
            lo = p.lo.load(std::memory_order_relaxed);
            hi = p.hi.load(std::memory_order_relaxed);
            kind = p.kind.load(std::memory_order_relaxed);
        }

    private:
        struct Point
        {
            std::atomic<float> lo { 0.0f }, hi { 0.0f };
            std::atomic<int> kind { -1 };
        };
        std::array<Point, c_points> m_points;
        std::atomic<std::uint32_t> m_written { 0 };
        int m_bucket = 800, m_count = 0, m_kind = -1;
        float m_lo = 1.0e30f, m_hi = -1.0e30f;
    };

    /** The newest c_size samples of a stream, written by the audio thread and copied out by the window. */
    class Tap
    {
    public:
        static constexpr int c_size = 2048;
        /** Audio thread. */
        void push(float x)
        {
            const auto w = m_write.load(std::memory_order_relaxed);
            m_data[static_cast<size_t>(w & (c_size - 1))].store(std::isfinite(x) ? x : 0.0f, std::memory_order_relaxed);
            m_write.store(w + 1, std::memory_order_release);
        }
        /** Any thread: the newest `count` samples (up to c_size), oldest first. */
        void copy(float* dest, int count) const
        {
            count = std::clamp(count, 0, c_size);
            const auto w = m_write.load(std::memory_order_acquire);
            for (int i = 0; i < count; ++i)
                dest[i] = m_data[static_cast<size_t>((w - static_cast<std::uint32_t>(count) + static_cast<std::uint32_t>(i)) & (c_size - 1))].load(std::memory_order_relaxed);
        }

    private:
        std::array<std::atomic<float>, c_size> m_data {};
        std::atomic<std::uint32_t> m_write { 0 };
    };

    /** One bus's view: what the audio thread publishes and the window reads. */
    struct BusView
    {
        // FilterLook, field by field.
        std::atomic<int> filterType { 0 };
        std::atomic<bool> filterSteep { false };
        std::atomic<float> filterHz { 1000.0f }, filterQ { 0.7f }, filterMix { 0.0f }, filterTrim { 1.0f }, filterLow { 20.0f }, filterHigh { 20000.0f };
        // SwirlLook, field by field.
        std::atomic<int> swirlType { 0 };
        std::atomic<bool> swirlOn { false };
        std::atomic<float> swirlKnob { 0.0f }, phaserCoef { 0.0f }, phaserFeedback { 0.0f }, phaserMix { 0.0f }, phaserHz { 1000.0f };
        std::atomic<float> flangerMs { 0.3f }, flangerFeedback { 0.0f }, flangerMix { 0.0f }, swirlLevel { 1.0f };
        std::atomic<float> offsetMs { 0.0f }, dryGain { 1.0f }, wetGain { 0.0f }, loopGain { 0.0f }, loopMs { 0.25f };
        std::atomic<std::uint32_t> tick { 0 };
        Trace swirlTrace, passTrace;
        Tap tap;

        /** Audio thread. */
        void publish(const FilterLook& f, const SwirlLook& s)
        {
            constexpr auto r = std::memory_order_relaxed;
            filterType.store(f.type, r);
            filterSteep.store(f.steep, r);
            filterHz.store(f.hz, r);
            filterQ.store(f.q, r);
            filterMix.store(f.mix, r);
            filterTrim.store(f.trim, r);
            filterLow.store(f.lowHz, r);
            filterHigh.store(f.highHz, r);
            swirlType.store(s.type, r);
            swirlOn.store(s.on, r);
            swirlKnob.store(s.knob, r);
            phaserCoef.store(s.phaserCoef, r);
            phaserFeedback.store(s.phaserFeedback, r);
            phaserMix.store(s.phaserMix, r);
            phaserHz.store(s.phaserHz, r);
            flangerMs.store(s.flangerMs, r);
            flangerFeedback.store(s.flangerFeedback, r);
            flangerMix.store(s.flangerMix, r);
            swirlLevel.store(s.level, r);
            offsetMs.store(s.offsetMs, r);
            dryGain.store(s.dryGain, r);
            wetGain.store(s.wetGain, r);
            loopGain.store(s.loopGain, r);
            loopMs.store(s.loopMs, r);
            tick.store(tick.load(r) + 1u, std::memory_order_release);
        }
        /** Any thread. */
        FilterLook filter() const
        {
            constexpr auto r = std::memory_order_relaxed;
            FilterLook f;
            f.type = filterType.load(r);
            f.steep = filterSteep.load(r);
            f.hz = filterHz.load(r);
            f.q = filterQ.load(r);
            f.mix = filterMix.load(r);
            f.trim = filterTrim.load(r);
            f.lowHz = filterLow.load(r);
            f.highHz = filterHigh.load(r);
            return f;
        }
        /** Any thread. */
        SwirlLook swirl() const
        {
            constexpr auto r = std::memory_order_relaxed;
            SwirlLook s;
            s.type = swirlType.load(r);
            s.on = swirlOn.load(r);
            s.knob = swirlKnob.load(r);
            s.phaserCoef = phaserCoef.load(r);
            s.phaserFeedback = phaserFeedback.load(r);
            s.phaserMix = phaserMix.load(r);
            s.phaserHz = phaserHz.load(r);
            s.flangerMs = flangerMs.load(r);
            s.flangerFeedback = flangerFeedback.load(r);
            s.flangerMix = flangerMix.load(r);
            s.level = swirlLevel.load(r);
            s.offsetMs = offsetMs.load(r);
            s.dryGain = dryGain.load(r);
            s.wetGain = wetGain.load(r);
            s.loopGain = loopGain.load(r);
            s.loopMs = loopMs.load(r);
            return s;
        }
    };
}
