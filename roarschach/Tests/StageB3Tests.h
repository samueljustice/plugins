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
        const double g = std::pow(10.0, db / 20.0) / std::sqrt(s / static_cast<double>(std::max<size_t>(1, x.size())) + 1.0e-30);
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

    // ============================================================================== DE-ESS (D5)

    struct DeessRun
    {
        Signal y;
        std::vector<float> gr;   // getGrDb () after each sample (when traced)
    };

    /** A fresh DE-ESS over x; amount may move per block through `knob` (block start sample -> settings). */
    DeessRun runDeess(const Signal& x, double u, Params::DeessMode mode, double sr = c_fs, int block = 512, bool trace = false,
                      std::function<void(roar::DeessModule&, int)> perBlock = {})
    {
        roar::DeessModule m;
        m.prepare(sr, block);
        roar::DeessSettings s;
        s.amount = static_cast<float>(u);
        s.mode = mode;
        m.setParams(s);
        m.reset();
        DeessRun r;
        r.y = x;
        Signal right(static_cast<size_t>(block), 0.0f);
        if (trace)
            r.gr.resize(x.size());
        const roar::VoiceFrameView none;
        for (int pos = 0; pos < static_cast<int>(x.size()); pos += block)
        {
            const int n = std::min(block, static_cast<int>(x.size()) - pos);
            if (perBlock)
                perBlock(m, pos);
            if (trace)
                for (int i = 0; i < n; ++i)
                {
                    m.process(r.y.data() + pos + i, right.data(), 1, false, none);
                    r.gr[static_cast<size_t>(pos + i)] = m.getGrDb();
                }
            else
                m.process(r.y.data() + pos, right.data(), n, false, none);
        }
        return r;
    }

    /** The measuring windows of esses (1-based list), 30 ms in to 10 ms before the end. */
    std::vector<std::pair<int, int>> essWindows(const Marked& m, std::initializer_list<int> which, double sr, int offset = 0)
    {
        std::vector<std::pair<int, int>> w;
        for (int k : which)
        {
            const auto& e = m.esses[static_cast<size_t>(k - 1)];
            w.emplace_back(offset + e.first + static_cast<int>(0.030 * sr), offset + e.second - static_cast<int>(0.010 * sr));
        }
        return w;
    }

    double essDepth(const Marked& m, const Signal& y, double sr, std::initializer_list<int> which = { 3, 4, 5, 6, 7, 8 })
    {
        return bandChangeDb(m.x, y, sr, essWindows(m, which, sr), 5000.0, 8000.0);
    }

    double essDepthOne(const Signal& in, const Signal& y, const Marked& m, int k, double sr, int offset)
    {
        return bandChangeDb(in, y, sr, essWindows(m, { k }, sr, offset), 5000.0, 8000.0);
    }

    double toneLevelDb(const Signal& in, const Signal& out, double sr, double hz, int from, int to)
    {
        return toDb(toneAmplitude(out, sr, hz, from, to) / std::max(1.0e-30, toneAmplitude(in, sr, hz, from, to)));
    }

    void testDeess()
    {
        using Mode = Params::DeessMode;
        const double sr = c_fs;
        const auto speech = speechLike(sr);

        // ---- S1: depth, recovery, the vowel after each ess
        double s1 = 0.0;
        {
            const auto r49 = runDeess(speech.x, 0.49, Mode::split, sr, 512, true);
            s1 = essDepth(speech, r49.y, sr);
            const double d25 = essDepth(speech, runDeess(speech.x, 0.25, Mode::split).y, sr);
            const double d75 = essDepth(speech, runDeess(speech.x, 0.75, Mode::split).y, sr);
            const double d100 = essDepth(speech, runDeess(speech.x, 1.0, Mode::split).y, sr);
            std::vector<double> recovery;
            for (const auto& e : speech.esses)
            {
                int k = e.second;
                while (k < static_cast<int>(r49.gr.size()) && r49.gr[static_cast<size_t>(k)] <= -1.0f)
                    ++k;
                recovery.push_back(1000.0 * (k - e.second) / sr);
            }
            std::sort(recovery.begin(), recovery.end());
            const double recMedian = recovery[recovery.size() / 2];
            double vowelWorst = 0.0;
            for (size_t k = 0; k + 1 < speech.esses.size(); ++k)
            {
                const int a = speech.esses[k].second + static_cast<int>(0.050 * sr), b = speech.esses[k].second + static_cast<int>(0.250 * sr);
                const double d = rmsDbOf(r49.y, a, b) - rmsDbOf(speech.x, a, b);
                if (std::abs(d) > std::abs(vowelWorst))
                    vowelWorst = d;
            }
            std::printf("  S1 DE-ESS esses 5-8 kHz: 25 %% %.2f, 49 %% %.2f, 75 %% %.2f, 100 %% %.2f dB; recovery %.1f ms (median); vowel after %.3f dB\n",
                        d25, s1, d75, d100, recMedian, vowelWorst);
            check(s1 <= -8.5 && s1 >= -10.5, "S1: esses at 49 % reduced by " + db1(s1) + " (expected -8.5 .. -10.5)");
            check(d25 <= -4.0 && d25 >= -5.5, "S1: esses at 25 % reduced by " + db1(d25) + " (expected -4.0 .. -5.5)");
            check(d100 <= d75 - 3.0, "S1: 100 % takes " + db1(d100) + ", not 3 dB more than 75 % (" + db1(d75) + ")");
            check(recMedian <= 25.0, "S1: recovery to -1 dB after an ess takes " + juce::String(recMedian, 1) + " ms at 49 %");
            check(std::abs(vowelWorst) <= 0.1, "S1: the vowel after an ess moves by " + db1(vowelWorst));
        }

        // ---- S2: steady rasp (the balanced recipe)
        {
            const auto rasp = raspLike(sr);
            const int from = static_cast<int>(2.0 * sr), end = static_cast<int>(rasp.size()), half = static_cast<int>(0.5 * sr);
            juce::String line;
            for (const int pct : { 25, 49, 75, 100 })
                for (const auto mode : { Mode::split, Mode::wide })
                {
                    const double u = pct / 100.0;
                    const auto y = runDeess(rasp, u, mode).y;
                    const double d = bandChangeDb(rasp, y, sr, from, end, 5000.0, 16000.0);
                    const double first = bandChangeDb(rasp, y, sr, 0, half, 5000.0, 16000.0);
                    line << juce::String(u * 100.0, 0) << (mode == Mode::split ? " % Split " : " % Wide ") << juce::String(d, 2) << " (first 0.5 s "
                         << juce::String(first, 2) << ")  ";
                    const juce::String what = "S2: rasp at " + juce::String(u * 100.0, 0) + (mode == Mode::split ? " % Split" : " % Wide");
                    if (pct == 49)
                    {
                        check(d > -1.0, what + " loses " + db1(d) + " at 5-16 kHz");
                        check(first > -1.5, what + " loses " + db1(first) + " over its first 0.5 s");
                        if (mode == Mode::wide)
                        {
                            const double body = bandChangeDb(rasp, y, sr, from, end, 300.0, 2000.0);
                            line << "(300 Hz-2 kHz " << juce::String(body, 2) << ")  ";
                            check(body > -1.0, what + " ducks 300 Hz-2 kHz by " + db1(body));
                        }
                    }
                    if (pct == 75)
                        check(d > -3.0, what + " loses " + db1(d));
                    if (pct == 100 && mode == Mode::split)
                        check(d <= -5.0 && d >= -12.0, what + ": " + db1(d) + " (expected -5 .. -12: the knob reaches a rasp)");
                }
            std::printf("  S2 rasp-like 5-16 kHz: %s\n", line.toRawUTF8());
        }

        // ---- S2b: speech, then the rasp, no reset
        {
            const auto rasp = raspLike(sr);
            const auto x = join({ speech.x, rasp });
            const int at = static_cast<int>(speech.x.size());
            const auto r = runDeess(x, 0.49, Mode::split, sr, 512, true);
            const auto grAt = [&](double t) { return static_cast<double>(r.gr[static_cast<size_t>(at + static_cast<int>(t * sr))]); };
            const double g6 = grAt(0.6), g75 = grAt(0.75), g10 = grAt(1.0);
            const double first = bandChangeDb(x, r.y, sr, at, at + static_cast<int>(0.5 * sr), 5000.0, 16000.0);
            const double later = bandChangeDb(x, r.y, sr, at + static_cast<int>(0.75 * sr), static_cast<int>(x.size()), 5000.0, 16000.0);
            std::printf("  S2b rasp after speech: GR %.2f / %.2f / %.2f dB at 0.6 / 0.75 / 1.0 s; 5-16 kHz first 0.5 s %.2f, from 0.75 s %.2f dB\n",
                        g6, g75, g10, first, later);
            check(g6 >= -1.0 && g75 >= -1.0 && g10 >= -1.0, "S2b: a rasp after speech is still reduced at 0.6-1 s");
            check(later > -1.0, "S2b: the rasp after speech loses " + db1(later) + " from 0.75 s");
            check(first > -4.0, "S2b: the rasp after speech loses " + db1(first) + " over its first 0.5 s");
        }

        // ---- S2c: an ess after a rasp (bright and balanced), no reset
        for (const bool bright : { true, false })
        {
            const auto rasp = bright ? brightRaspLike(sr) : raspLike(sr);
            const auto x = join({ rasp, speech.x });
            const int at = static_cast<int>(rasp.size());
            const auto y = runDeess(x, 0.49, Mode::split).y;
            juce::String line;
            double worst = -100.0, eight = 0.0;
            for (const int k : { 1, 2, 3, 4, 8 })
            {
                const double d = essDepthOne(x, y, speech, k, sr, at);
                line << "ess " << k << " " << juce::String(d, 2) << "  ";
                worst = std::max(worst, d);
                if (k == 8)
                    eight = d;
            }
            std::printf("  S2c esses after the %s rasp-like: %s\n", bright ? "bright" : "balanced", line.toRawUTF8());
            check(worst <= -6.5, juce::String("S2c: an ess after the ") + (bright ? "bright" : "balanced") + " rasp is reduced by only " + db1(worst));
            check(std::abs(eight - s1) <= 2.0, "S2c: ess 8 after a rasp " + db1(eight) + " against " + db1(s1));
        }

        // ---- S2d: the ceiling
        {
            const auto bright = brightRaspLike(sr);
            const double d = bandChangeDb(bright, runDeess(bright, 0.49, Mode::split).y, sr, static_cast<int>(2.0 * sr), static_cast<int>(bright.size()), 5000.0, 16000.0);
            const auto hiss = noiseBand(sr, 4.0, 4000.0, 9000.0, -26.0, 5u);
            const double h = bandChangeDb(hiss, runDeess(hiss, 0.49, Mode::split).y, sr, static_cast<int>(2.0 * sr), static_cast<int>(hiss.size()), 5000.0, 8000.0);
            std::printf("  S2d ceiling: bright rasp-like 5-16 kHz %.2f dB; ess noise alone 5-8 kHz %.2f dB\n", d, h);
            check(d <= -4.5 && d >= -8.0, "S2d: the bright rasp-like at 49 % " + db1(d) + " (expected -4.5 .. -8)");
            check(h <= -6.0 && h >= -9.0, "S2d: steady ess noise at 49 % " + db1(h) + " (expected -6 .. -9)");
        }

        // ---- S3: Wide's hold and depth
        {
            const auto v = faded(vowel(sr, 1.0), sr);
            const auto hiss = faded(noiseBand(sr, 3.0, 4000.0, 9000.0, -26.0, 5u), sr);
            const auto x = join({ v, hiss });
            const int at = static_cast<int>(v.size());
            const auto r = runDeess(x, 0.49, Mode::wide, sr, 512, true);
            const double g01 = r.gr[static_cast<size_t>(at + static_cast<int>(0.1 * sr))];
            double after = -100.0;
            for (int i = at + static_cast<int>(1.0 * sr); i < static_cast<int>(x.size()); ++i)
                after = std::max(after, -static_cast<double>(r.gr[static_cast<size_t>(i)]));
            after = -after;   // the deepest GR from 1 s on
            const auto w = runDeess(speech.x, 0.49, Mode::wide).y;
            double pi = 0.0, po = 0.0;
            for (const auto& [a, b] : essWindows(speech, { 3, 4, 5, 6, 7, 8 }, sr))
            {
                pi += std::pow(rmsOf(speech.x, a, b), 2.0) * (b - a);
                po += std::pow(rmsOf(w, a, b), 2.0) * (b - a);
            }
            const double depth = 10.0 * std::log10(po / pi);
            std::printf("  S3 Wide: GR %.2f dB 0.1 s into a hiss, at most %.2f dB from 1 s on; esses broadband %.2f dB\n", g01, after, depth);
            check(g01 <= -7.0, "S3: Wide at 0.1 s into a hiss " + db1(g01));
            check(after >= -2.0, "S3: Wide still holds " + db1(after) + " 1 s into a hiss");
            check(depth <= -7.0 && depth >= -9.0, "S3: Wide takes " + db1(depth) + " off the esses (expected -7 .. -9)");
        }

        // ---- S4: null; S4b: the shelf's response
        {
            const auto sine = makeSine(sr, 1.0, 200.0, std::pow(10.0, -12.0 / 20.0));
            bool exact = true;
            for (const double u : { 0.49, 1.0 })
                for (const auto mode : { Mode::split, Mode::wide })
                    exact = exact && runDeess(sine, u, mode).y == sine;
            check(exact, "S4: a 200 Hz sine is not passed bit for bit at 49 % / 100 %");
            check(runDeess(speech.x, 0.0, Mode::split).y == speech.x, "S4: De-ess 0 % is not bit-exact on speech");
        }
        const auto shelfCheck = [](double rate, const juce::String& tag)
        {
            const double topHz = rate < 46000.0 ? 14000.0 : 16000.0;
            const std::vector<double> tones { 200.0, 500.0, 1000.0, 2800.0, 5000.0, 10000.0, topHz };
            const auto forced = [rate](const Signal& x, Params::DeessMode mode, double db)
            {
                return runDeess(x, 0.49, mode, rate, 512, false, [db](roar::DeessModule& m, int) { m.setForcedGainForTest(true, db); }).y;
            };
            juce::String line;
            double prev = 1.0;
            bool ok = true, monotonic = true, wideOk = true, zeroExact = true;
            for (size_t j = 0; j < tones.size(); ++j)
            {
                const double hz = tones[j];
                const auto x = makeSine(rate, 0.5, hz, 0.1);
                const int a = static_cast<int>(0.2 * rate), b = static_cast<int>(0.5 * rate);
                const double d = toneLevelDb(x, forced(x, Params::DeessMode::split, -12.0), rate, hz, a, b);
                const double dw = toneLevelDb(x, forced(x, Params::DeessMode::wide, -12.0), rate, hz, a, b);
                zeroExact = zeroExact && forced(x, Params::DeessMode::split, 0.0) == x;
                line << juce::String(hz / 1000.0, 1) << "k " << juce::String(d, 2) << "  ";
                monotonic = monotonic && d <= prev + 0.01;
                prev = d;
                wideOk = wideOk && std::abs(dw + 9.6) <= 0.1;
                if (j <= 1)
                    ok = ok && std::abs(d) <= 0.1;
                else if (j == 2)
                    ok = ok && std::abs(d) <= 0.3;
                else if (j == 3)
                    ok = ok && std::abs(d + 4.1) <= 0.5;
                else if (j == 4)
                    ok = ok && d <= -9.0 && d >= -11.0;
                else
                    ok = ok && std::abs(d + 12.0) <= 0.5;
            }
            std::printf("  S4b shelf at -12 dB, %s: %s\n", tag.toRawUTF8(), line.toRawUTF8());
            check(ok, "S4b: the shelf's response at " + tag + " is outside its bounds");
            check(monotonic, "S4b: the shelf's response rises with frequency at " + tag);
            check(wideOk, "S4b: Wide forced to -12 dB is not -9.6 dB at every frequency at " + tag);
            check(zeroExact, "S4b: forced 0 dB is not bit-exact at " + tag);
        };
        shelfCheck(sr, "48 kHz");

        // ---- S5: a vowel at the top of the travel
        {
            const auto v = vowel(sr, 3.0);
            for (const double u : { 0.49, 1.0 })
            {
                const auto y = runDeess(v, u, Mode::split).y;
                const double diff = maxAbsDiff(v, y), lev = rmsDbOf(y) - rmsDbOf(v);
                std::printf("  S5 vowel at %.0f %%: max |y - x| %.4f, level %+.3f dB\n", u * 100.0, diff, lev);
                check(diff <= 0.02 && std::abs(lev) <= 0.05, "S5: the vowel at " + juce::String(u * 100.0, 0) + " % moves by " + juce::String(diff, 4));
            }
        }

        // ---- S6: rates and blocks
        for (const double rate : { 44100.0, 96000.0, 192000.0 })
        {
            const auto sp = speechLike(rate);
            const double d = essDepth(sp, runDeess(sp.x, 0.49, Mode::split, rate).y, rate);
            std::printf("  S6 esses at %.1f kHz: %.2f dB\n", rate / 1000.0, d);
            check(std::abs(d - s1) <= 0.5, "S6: ess depth at " + juce::String(rate / 1000.0, 1) + " kHz " + db1(d) + " against " + db1(s1));
            shelfCheck(rate, juce::String(rate / 1000.0, 1) + " kHz");
        }
        {
            const auto ref = runDeess(speech.x, 0.49, Mode::split, sr, 512).y;
            double worst = 0.0;
            for (const int block : { 16, 480, 4096 })
                worst = std::max(worst, maxAbsDiff(ref, runDeess(speech.x, 0.49, Mode::split, sr, block).y));
            check(worst <= 1.0e-6, "S6: DE-ESS depends on the block size (" + juce::String(worst) + ")");
        }

        // ---- S7: the first ess after a reset
        {
            const auto first = speechLike(sr, 8, false);
            const auto y = runDeess(first.x, 0.49, Mode::split).y;
            const double e1 = essDepthOne(first.x, y, first, 1, sr, 0), e2 = essDepthOne(first.x, y, first, 2, sr, 0), e3 = essDepthOne(first.x, y, first, 3, sr, 0);
            std::printf("  S7 starting on an ess: %.2f / %.2f / %.2f dB\n", e1, e2, e3);
            check(e1 <= -6.5, "S7: the first ess after a reset is reduced by only " + db1(e1));
            check(std::abs(e2 - s1) <= 1.5, "S7: the second ess " + db1(e2) + " against " + db1(s1));
        }

        // ---- S8: room tone between lines
        {
            const auto gapped = speechLike(sr, 8, true, 1.0, -70.0);
            const double d = essDepth(gapped, runDeess(gapped.x, 0.49, Mode::split).y, sr);
            std::printf("  S8 with -70 dBFS room between repeats: %.2f dB\n", d);
            check(std::abs(d - s1) <= 0.5, "S8: room tone between lines moves the ess depth to " + db1(d));
        }

        // ---- S9: knob moves
        {
            const auto rasp = raspLike(sr);
            const int from = static_cast<int>(2.0 * sr), len = static_cast<int>(0.5 * sr);
            const auto y = runDeess(rasp, 0.0, Mode::split, sr, 32, false, [&](roar::DeessModule& m, int pos)
            {
                const double t = std::clamp(static_cast<double>(pos - from) / len, 0.0, 1.0);
                roar::DeessSettings s;
                s.amount = static_cast<float>(t < 0.5 ? 2.0 * t : 2.0 - 2.0 * t);
                m.setParams(s);
            }).y;
            const double out = maxStep(y, from, from + len + static_cast<int>(0.1 * sr)), in = maxStep(rasp, from, from + len + static_cast<int>(0.1 * sr));
            const int switchAt = speech.esses[3].first + static_cast<int>(0.07 * sr);
            const auto ys = runDeess(speech.x, 0.49, Mode::split, sr, 32, false, [&](roar::DeessModule& m, int pos)
            {
                roar::DeessSettings s;
                s.amount = 0.49f;
                s.mode = (pos >= switchAt && pos < switchAt + static_cast<int>(0.03 * sr)) ? Mode::wide : Mode::split;
                m.setParams(s);
            }).y;
            const int a = speech.esses[3].first, b = speech.esses[3].second + static_cast<int>(0.05 * sr);
            const double outS = maxStep(ys, a, b), inS = maxStep(speech.x, a, b);
            std::printf("  S9 knob moves: De-ess 0-100-0 %% largest step %.4f (input %.4f); Split-Wide-Split in an ess %.4f (input %.4f)\n", out, in, outS, inS);
            check(out <= in * 1.05, "S9: a De-ess sweep makes a step of " + juce::String(out, 4) + " against the rasp's " + juce::String(in, 4));
            check(outS <= inS * 1.05, "S9: a mode switch in an ess makes a step of " + juce::String(outS, 4) + " against " + juce::String(inS, 4));
        }

        // ---- S9b: the conversion of first-release ranges
        {
            namespace cv = roar::laws::convert;
            const auto lerpTable = [](const std::vector<double>& xs, const std::vector<double>& ys, double x)
            {
                x = std::clamp(x, xs.front(), xs.back());
                size_t i = 0;
                while (i + 2 < xs.size() && x > xs[i + 1])
                    ++i;
                return ys[i] + (x - xs[i]) / (xs[i + 1] - xs[i]) * (ys[i + 1] - ys[i]);
            };
            const std::vector<double> basePct { 0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60 };
            const std::vector<double> shelfPct { 0, 5.0, 9.3, 14.1, 18.9, 23.8, 28.9, 34.0, 39.2, 45.5, 52.2, 59.1, 65.5 };
            bool ok = true, monotonic = true;
            double prev = -1.0;
            juce::String line;
            for (const double r : { -3.0, -6.0, -9.0, -12.0, -15.0 })
            {
                const double v = cv::deessForOldRange(r), want = lerpTable(basePct, shelfPct, cv::deessBasePositionForOldRange(r));
                ok = ok && std::abs(v - want) <= 0.05;
                monotonic = monotonic && v > prev;
                prev = v;
                line << juce::String(r, 0) << " dB -> " << juce::String(v, 1) << " %  ";
            }
            std::printf("  S9b first-release ranges: %s\n", line.toRawUTF8());
            check(ok && monotonic, "S9b: the De-ess conversion is not the measured table over the base position, or not monotonic");
        }
    }

    // ============================================================================== TAME (finding 12)

    Signal runTame(const Signal& x, double u, double sr = c_fs, int block = 512, bool guard = true, bool forcedZero = false)
    {
        roar::TameModule m;
        m.prepare(sr, block);
        roar::TameSettings s;
        s.amount = static_cast<float>(u);
        m.setParams(s);
        m.setGuardForTests(guard);
        if (forcedZero)
            m.setForcedGainsForTest(true, 0.0, 0.0);
        m.reset();
        return runModuleMono(m, x, block, unvoicedFrame, [](int) {});
    }

    /** 8 s of the bright rasp-like with +10 dB for 0.4 s at 2, 3, .. 7 s (the step through a 10 ms pole). */
    struct Swells
    {
        Signal x;
        std::vector<std::pair<int, int>> in, between;
    };

    Swells makeSwells(double sr)
    {
        Swells w;
        w.x = brightRaspLike(sr, 8.0);
        const double k = 1.0 - std::exp(-1.0 / (0.010 * sr));
        double g = 0.0;
        int start = -1;
        for (int i = 0; i < static_cast<int>(w.x.size()); ++i)
        {
            const double t = i / sr;
            const double target = (t >= 2.0 && t < 7.4 && std::fmod(t - 2.0, 1.0) < 0.4) ? 10.0 : 0.0;
            g += k * (target - g);
            w.x[static_cast<size_t>(i)] *= static_cast<float>(std::pow(10.0, g / 20.0));
            if (g > 8.0 && start < 0)
                start = i;
            if (g <= 8.0 && start >= 0)
            {
                w.in.emplace_back(start, i);
                start = -1;
            }
        }
        for (int s0 = 2; s0 <= 7; ++s0)
            w.between.emplace_back(static_cast<int>((s0 + 0.5) * sr), static_cast<int>((s0 + 1.0) * sr));
        return w;
    }

    void testTame()
    {
        const double sr = c_fs;

        // ---- S10: the bright rasp-like across the knob
        {
            const auto rasp = brightRaspLike(sr);
            const int from = static_cast<int>(2.0 * sr), end = static_cast<int>(rasp.size());
            juce::String line;
            double prev = 1.0;
            bool deeper = true;
            std::map<int, double> at;
            for (const int pct : { 6, 12, 25, 50, 100 })
            {
                const auto y = runTame(rasp, pct / 100.0);
                const double d = bandChangeDb(rasp, y, sr, from, end, 5000.0, 16000.0);
                at[pct] = d;
                deeper = deeper && d < prev;
                prev = d;
                line << pct << " % " << juce::String(d, 2) << "  ";
                if (pct == 6)
                {
                    const double first = bandChangeDb(rasp, y, sr, 0, static_cast<int>(0.5 * sr), 5000.0, 16000.0);
                    line << "(first 0.5 s " << juce::String(first, 2) << ")  ";
                    check(first > -1.5, "S10: TAME 6 % takes " + db1(first) + " off the first 0.5 s of a bright rasp");
                }
            }
            std::printf("  S10 TAME on the bright rasp-like, 5-16 kHz: %s\n", line.toRawUTF8());
            check(at[6] > -1.0, "S10: TAME 6 % takes " + db1(at[6]) + " off a bright rasp");
            check(at[12] > -2.5, "S10: TAME 12 % takes " + db1(at[12]));
            check(at[25] <= -3.0 && at[25] >= -7.0, "S10: TAME 25 % takes " + db1(at[25]) + " (expected 3 .. 7 dB)");
            check(deeper, "S10: TAME does not take more at every step of the knob");
        }

        // ---- S10b / S12b: swells
        std::map<int, double> swell48;
        const auto swellRun = [](double rate, double u, double& inSwells, double& between)
        {
            const auto w = makeSwells(rate);
            const auto y = runTame(w.x, u, rate);
            inSwells = bandChangeDb(w.x, y, rate, w.in, 5000.0, 16000.0);
            between = bandChangeDb(w.x, y, rate, w.between, 5000.0, 16000.0);
        };
        {
            juce::String line;
            for (const int pct : { 6, 12, 25, 50 })
            {
                double a = 0.0, b = 0.0;
                swellRun(sr, pct / 100.0, a, b);
                swell48[pct] = a;
                line << pct << " % " << juce::String(a, 2) << " / " << juce::String(b, 2) << "  ";
                if (pct == 6)
                    check(a > -1.5 && b > -1.5, "S10b: TAME 6 % takes " + db1(a) + " / " + db1(b) + " off a bright voice's swells / between them");
                if (pct == 25)
                    check(a <= b - 3.5 && a <= -6.0, "S10b: TAME 25 % in the swells " + db1(a) + ", between " + db1(b));
                if (pct == 50)
                    check(a <= b - 4.0, "S10b: TAME 50 % in the swells " + db1(a) + ", between " + db1(b));
            }
            std::printf("  S10b swells, 5-16 kHz in / between: %s\n", line.toRawUTF8());
        }
        for (const double rate : { 44100.0, 96000.0, 192000.0 })
        {
            double a = 0.0, b = 0.0;
            swellRun(rate, 0.25, a, b);
            std::printf("  S12b swells at %.1f kHz, Tame 25 %%: %.2f / %.2f dB\n", rate / 1000.0, a, b);
            check(std::abs(a - swell48[25]) <= 0.5, "S12b: swells at " + juce::String(rate / 1000.0, 1) + " kHz " + db1(a) + " against " + db1(swell48[25]));
        }

        // ---- S11: esses with and without the guard
        {
            const auto speech = speechLike(sr);
            juce::String line;
            for (const int pct : { 6, 25, 50, 100 })
            {
                const double on = essDepth(speech, runTame(speech.x, pct / 100.0), sr);
                const double off = essDepth(speech, runTame(speech.x, pct / 100.0, sr, 512, false), sr);
                line << pct << " % " << juce::String(on, 2) << " (fixed " << juce::String(off, 2) << ")  ";
                check(std::abs(on - off) <= 0.5, "S11: TAME " + juce::String(pct) + " % on esses " + db1(on) + " against the fixed threshold's " + db1(off));
            }
            std::printf("  S11 esses 5-8 kHz: %s\n", line.toRawUTF8());
        }

        // ---- S12: null and blocks
        {
            const auto speech = speechLike(sr);
            const double nullDiff = maxAbsDiff(runTame(speech.x, 0.0), runTame(speech.x, 0.25, sr, 512, true, true));
            const auto rasp = brightRaspLike(sr, 3.0);
            const auto ref = runTame(rasp, 0.25);
            const double blocks = std::max(maxAbsDiff(ref, runTame(rasp, 0.25, sr, 16)), maxAbsDiff(ref, runTame(rasp, 0.25, sr, 4096)));
            std::printf("  S12 Tame 0 %% against forced 0 dB: %.2g; blocks 16 / 4096 against 512: %.2g\n", nullDiff, blocks);
            check(nullDiff <= 1.0e-6, "S12: Tame 0 % is not the allpass (" + juce::String(nullDiff) + ")");
            check(blocks <= 1.0e-6, "S12: TAME depends on the block size (" + juce::String(blocks) + ")");
        }
    }

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
    stageb3::testDeess();
    stageb3::testTame();
    stageb3::testCleanGate();
}
