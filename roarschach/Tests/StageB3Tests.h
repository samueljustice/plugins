// Top end and dynamics: DE-ESS's shelf and its guard, TAME's guard, VOCODER's noise-band trim, ROAR's
// learnt makeup (and the audition law), DETAIL's period hold, and CLEAN's gate pinned. Included by
// RoarschachTests.cpp inside its anonymous namespace, after its helpers (check, Signal, runModuleMono,
// spectrum ...). Module level: each module is driven directly. Every signal is synthetic.
//
// Group: "stageb3".

namespace stageb3
{
    constexpr double c_fs = 48000.0;

    // ------------------------------------------------------------------------------ signals

    double rmsOf(const Signal& x, int from = 0, int to = -1)
    {
        to = to < 0 ? static_cast<int>(x.size()) : std::min(to, static_cast<int>(x.size()));
        double s = 0.0;
        for (int i = std::max(0, from); i < to; ++i)
            s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
        return std::sqrt(s / std::max(1, to - from));
    }

    double rmsDbOf(const Signal& x, int from = 0, int to = -1) { return 20.0 * std::log10(rmsOf(x, from, to) + 1.0e-30); }

    void scaleToRmsDb(std::vector<double>& x, double db)
    {
        double s = 0.0;
        for (double v : x)
            s += v * v;
        const double g = std::pow(10.0, db / 20.0) / std::sqrt(s / std::max<size_t>(1, x.size()) + 1.0e-30);
        for (auto& v : x)
            v *= g;
    }

    Signal toSignal(const std::vector<double>& x)
    {
        Signal s(x.size());
        for (size_t i = 0; i < x.size(); ++i)
            s[i] = static_cast<float>(x[i]);
        return s;
    }

    Signal join(std::initializer_list<Signal> parts)
    {
        Signal out;
        for (const auto& p : parts)
            out.insert(out.end(), p.begin(), p.end());
        return out;
    }

    std::vector<double> gaussian(int n, unsigned seed)
    {
        std::mt19937 rng(seed);
        std::normal_distribution<double> g(0.0, 1.0);
        std::vector<double> w(static_cast<size_t>(n));
        for (auto& v : w)
            v = g(rng);
        return w;
    }

    void runBiquad(std::vector<double>& x, const roar::Biquad::Coeffs& c)
    {
        roar::Biquad f;
        f.setCoeffs(c);
        for (auto& v : x)
            v = f.process(v);
    }

    /** Butterworth band-pass of 2 x `order` poles (an order-`order` high-pass, then low-pass); order 2 or 4. */
    void bandLimit(std::vector<double>& x, double sr, double lo, double hi, int order)
    {
        hi = std::min(hi, 0.45 * sr);
        if (order == 4)
        {
            runBiquad(x, roar::Biquad::highpass(sr, lo, roar::Biquad::c_bw4Q1));
            runBiquad(x, roar::Biquad::highpass(sr, lo, roar::Biquad::c_bw4Q2));
            runBiquad(x, roar::Biquad::lowpass(sr, hi, roar::Biquad::c_bw4Q1));
            runBiquad(x, roar::Biquad::lowpass(sr, hi, roar::Biquad::c_bw4Q2));
        }
        else
        {
            runBiquad(x, roar::Biquad::highpass(sr, lo, roar::Biquad::c_butterworthQ));
            runBiquad(x, roar::Biquad::lowpass(sr, hi, roar::Biquad::c_butterworthQ));
        }
    }

    /** The vowel: harmonics k f0 below 3 kHz at 1 / k, scaled to levelDb RMS. */
    Signal vowel(double sr, double seconds, double f0 = 150.0, double levelDb = -20.0)
    {
        std::vector<double> x(static_cast<size_t>(seconds * sr), 0.0);
        const int top = static_cast<int>(std::ceil(3000.0 / f0)) - 1;
        for (size_t i = 0; i < x.size(); ++i)
            for (int k = 1; k <= top; ++k)
                x[i] += std::sin(2.0 * c_pi * k * f0 * static_cast<double>(i) / sr) / k;
        scaleToRmsDb(x, levelDb);
        return toSignal(x);
    }

    /** White noise through a Butterworth band-pass (4th order each side), scaled to levelDb RMS. */
    Signal noiseBand(double sr, double seconds, double lo, double hi, double levelDb, unsigned seed)
    {
        auto w = gaussian(static_cast<int>(seconds * sr), seed);
        bandLimit(w, sr, lo, hi, 4);
        scaleToRmsDb(w, levelDb);
        return toSignal(w);
    }

    /** Raised-cosine fades of `ms` at both ends. */
    Signal faded(Signal x, double sr, double ms = 5.0)
    {
        const int k = std::min(static_cast<int>(ms * sr / 1000.0), static_cast<int>(x.size()) / 2);
        for (int i = 0; i < k; ++i)
        {
            const auto r = static_cast<float>(0.5 - 0.5 * std::cos(c_pi * i / k));
            x[static_cast<size_t>(i)] *= r;
            x[x.size() - 1 - static_cast<size_t>(i)] *= r;
        }
        return x;
    }

    struct Marked
    {
        Signal x;
        std::vector<std::pair<int, int>> esses;    // [start, end) samples
        std::vector<std::pair<int, int>> vowels;
    };

    /** repeats x [0.30 s vowel, 0.15 s ess]; withFirstVowel = false drops the first vowel (starts on an ess). */
    Marked speechLike(double sr, int repeats = 8, bool withFirstVowel = true, double gapSeconds = 0.0, double gapDb = -70.0)
    {
        Marked m;
        for (int r = 0; r < repeats; ++r)
        {
            if (r > 0 && gapSeconds > 0.0)
            {
                auto g = gaussian(static_cast<int>(gapSeconds * sr), 900u + static_cast<unsigned>(r));
                scaleToRmsDb(g, gapDb);
                const auto gs = toSignal(g);
                m.x.insert(m.x.end(), gs.begin(), gs.end());
            }
            if (r > 0 || withFirstVowel)
            {
                const auto v = faded(vowel(sr, 0.30), sr);
                m.vowels.emplace_back(static_cast<int>(m.x.size()), static_cast<int>(m.x.size() + v.size()));
                m.x.insert(m.x.end(), v.begin(), v.end());
            }
            const auto e = faded(noiseBand(sr, 0.15, 4000.0, 9000.0, -26.0, 10u + static_cast<unsigned>(r)), sr);
            m.esses.emplace_back(static_cast<int>(m.x.size()), static_cast<int>(m.x.size() + e.size()));
            m.x.insert(m.x.end(), e.begin(), e.end());
        }
        return m;
    }

    /** 6 Hz tremor of 0.17 and the level. */
    Signal tremorAndLevel(std::vector<double> w, double sr, double levelDb)
    {
        for (size_t i = 0; i < w.size(); ++i)
            w[i] *= 1.0 + 0.17 * std::sin(2.0 * c_pi * 6.0 * static_cast<double>(i) / sr);
        scaleToRmsDb(w, levelDb);
        return toSignal(w);
    }

    /** A real rasp's balance (4-12 kHz about 8 dB under the whole): LP1 1.5 kHz, BP 300 Hz-12 kHz. */
    Signal raspLike(double sr, double seconds = 6.0, double levelDb = -24.0)
    {
        auto w = gaussian(static_cast<int>(seconds * sr), 3u);
        runBiquad(w, roar::Biquad::lowpass1(sr, 1500.0));
        bandLimit(w, sr, 300.0, 12000.0, 2);
        return tremorAndLevel(std::move(w), sr, levelDb);
    }

    /** As bright as an ess: BP 300 Hz-12 kHz plus twice its own HP1 at 3 kHz. */
    Signal brightRaspLike(double sr, double seconds = 6.0, double levelDb = -24.0)
    {
        auto w = gaussian(static_cast<int>(seconds * sr), 3u);
        bandLimit(w, sr, 300.0, 12000.0, 2);
        auto h = w;
        runBiquad(h, roar::Biquad::highpass1(sr, 3000.0));
        for (size_t i = 0; i < w.size(); ++i)
            w[i] += 2.0 * h[i];
        return tremorAndLevel(std::move(w), sr, levelDb);
    }

    /** White noise through LP1 at 500 Hz (ROAR's test input). */
    Signal voiceNoise(double sr, double seconds, double levelDb = -20.0, unsigned seed = 41u)
    {
        auto w = gaussian(static_cast<int>(seconds * sr), seed);
        runBiquad(w, roar::Biquad::lowpass1(sr, 500.0));
        scaleToRmsDb(w, levelDb);
        return toSignal(w);
    }

    // ------------------------------------------------------------------------------ measures

    /**
     * Band power of x over [from, to), lo..hi Hz: Hann frames of 1024 hopped by 256 that lie inside
     * the range, power summed over the band's bins. The same frames on two signals give a level
     * difference that does not depend on the window.
     */
    double bandPow(const Signal& x, double sr, int from, int to, double lo, double hi)
    {
        constexpr int order = 10, size = 1 << order, hop = 256;
        static juce::dsp::FFT fft(order);
        std::vector<float> data(static_cast<size_t>(2 * size));
        const double binHz = sr / size;
        double sum = 0.0;
        to = std::min(to, static_cast<int>(x.size()));
        for (int start = std::max(0, from); start + size <= to; start += hop)
        {
            std::fill(data.begin(), data.end(), 0.0f);
            for (int i = 0; i < size; ++i)
                data[static_cast<size_t>(i)] = x[static_cast<size_t>(start + i)] * static_cast<float>(0.5 - 0.5 * std::cos(2.0 * c_pi * i / size));
            fft.performFrequencyOnlyForwardTransform(data.data());
            for (int k = 0; k <= size / 2; ++k)
            {
                const double f = k * binHz;
                if (f >= lo && f < hi)
                    sum += static_cast<double>(data[static_cast<size_t>(k)]) * data[static_cast<size_t>(k)];
            }
        }
        return sum;
    }

    /** Level change (dB) out against in, lo..hi Hz, summed over the ranges. */
    double bandChangeDb(const Signal& in, const Signal& out, double sr, const std::vector<std::pair<int, int>>& ranges, double lo, double hi)
    {
        double pi = 0.0, po = 0.0;
        for (const auto& [a, b] : ranges)
        {
            pi += bandPow(in, sr, a, b, lo, hi);
            po += bandPow(out, sr, a, b, lo, hi);
        }
        return 10.0 * std::log10((po + 1.0e-30) / (pi + 1.0e-30));
    }

    double bandChangeDb(const Signal& in, const Signal& out, double sr, int from, int to, double lo, double hi)
    {
        return bandChangeDb(in, out, sr, std::vector<std::pair<int, int>> { { from, to } }, lo, hi);
    }

    /** Largest |x[i] - x[i-1]| over [from, to). */
    double maxStep(const Signal& x, int from = 1, int to = -1)
    {
        to = to < 0 ? static_cast<int>(x.size()) : to;
        double m = 0.0;
        for (int i = std::max(1, from); i < to; ++i)
            m = std::max(m, static_cast<double>(std::abs(x[static_cast<size_t>(i)] - x[static_cast<size_t>(i - 1)])));
        return m;
    }

    double maxAbsDiff(const Signal& a, const Signal& b)
    {
        double m = 0.0;
        for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
            m = std::max(m, static_cast<double>(std::abs(a[i] - b[i])));
        return m;
    }

    roar::VoiceFrame unvoicedFrame(int) { return roar::VoiceFrame {}; }

    template <typename M>
    Signal runMono(M& module, const Signal& in, int block = 512)
    {
        return runModuleMono(module, in, block, unvoicedFrame, [](int) {});
    }

    juce::String db1(double v) { return juce::String(v, 2) + " dB"; }

    // ============================================================================== CLEAN (D20)

    void testCleanGate()
    {
        // S26 / S27: vowel bursts (0.4 s on, 0.6 s off) over a low-passed room, Gate 47 %, Contain 0,
        // Flat, no IN in front. The gate's gain per sample from its trace.
        const auto gateRun = [](double roomDb, std::vector<double>& gapDb, std::vector<double>& burstDb)
        {
            const double sr = c_fs;
            const double seconds = 12.0;
            auto room = gaussian(static_cast<int>(seconds * sr), 77u);
            runBiquad(room, roar::Biquad::lowpass1(sr, 800.0));
            scaleToRmsDb(room, roomDb);
            const auto burst = faded(vowel(sr, 0.4), sr);
            Signal x = toSignal(room);
            const int period = static_cast<int>(1.0 * sr);
            for (int start = 0; start + period <= static_cast<int>(x.size()); start += period)
                for (size_t i = 0; i < burst.size(); ++i)
                    x[static_cast<size_t>(start) + i] += burst[i];

            roar::CleanModule clean;
            clean.prepare(sr, 512);
            roar::CleanSettings s;
            s.contain = 0.0f;
            s.gate = 0.47f;
            s.tone = Params::CleanTone::flat;
            clean.setParams(s);
            clean.reset();
            Signal trace(x.size(), 0.0f);
            int pos = 0;
            runModuleMono(clean, x, 512, unvoicedFrame, [&](int end)
            {
                const float* t = clean.getGateTrace();
                for (int i = pos; i < end; ++i)
                    trace[static_cast<size_t>(i)] = t[i - pos];
                pos = end;
            });
            gapDb.clear();
            burstDb.clear();
            for (int start = static_cast<int>(4.0 * sr); start + period <= static_cast<int>(x.size()); start += period)
            {
                for (int i = start + static_cast<int>(0.7 * sr); i < start + period; ++i)        // 0.3 s into the gap
                    gapDb.push_back(trace[static_cast<size_t>(i)]);
                for (int i = start + static_cast<int>(0.02 * sr); i < start + static_cast<int>(0.4 * sr); ++i)
                    burstDb.push_back(trace[static_cast<size_t>(i)]);
            }
        };

        std::vector<double> gaps, bursts;
        gateRun(-50.0, gaps, bursts);
        const double gapMax = *std::max_element(gaps.begin(), gaps.end());
        const double burstMin = *std::min_element(bursts.begin(), bursts.end());
        std::printf("  S26 CLEAN gate 47 %%, room -50 dBFS: gaps at most %.2f dB, bursts at least %.2f dB\n", gapMax, burstMin);
        check(gapMax <= -12.0, "S26: the gate leaves a -50 dBFS room at " + db1(gapMax));
        check(burstMin >= -0.5, "S26: the gate cuts the bursts by " + db1(burstMin));

        gateRun(-38.0, gaps, bursts);
        const double gapMean = std::accumulate(gaps.begin(), gaps.end(), 0.0) / static_cast<double>(gaps.size());
        std::printf("  S27 CLEAN gate 47 %%, room -38 dBFS: gaps %.2f dB (mean)\n", gapMean);
        check(gapMean >= -4.0 && gapMean <= 0.0, "S27: a -38 dBFS room is gated by " + db1(gapMean) + " (expected -4 .. 0)");
    }
}

void testStageB3()
{
    std::printf("[stageb3] DE-ESS, TAME, VOCODER, ROAR level, DETAIL hold, CLEAN gate\n");
    stageb3::testCleanGate();
}
