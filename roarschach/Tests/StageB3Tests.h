// Top end and dynamics: DE-ESS's shelf and its guard, TAME's guard, VOCODER's noise-band trim, ROAR's
// learnt makeup (and the audition law), DETAIL's period hold, and CLEAN's gate pinned. Included by
// RoarschachTests.cpp inside its anonymous namespace, after its helpers (check, Signal, runModuleMono,
// spectrum ...). Module level: each module is driven directly. Every signal is synthetic.
//
// Group: "stageb3".

namespace stageb3
{
    constexpr double c_fs = 48000.0;

    // ROARSCHACH_ROAR_LAW=B selects ROAR's audition law for this process (renders through --render).
    [[maybe_unused]] const bool c_roarLawFromEnvironment = []
    {
        const char* v = std::getenv("ROARSCHACH_ROAR_LAW");
        roar::RoarModule::setLawForTests(v != nullptr && (v[0] == 'B' || v[0] == 'b') ? 1 : 0);
        return true;
    }();

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
    Signal voiceNoise(double sr, double seconds, double levelDb = -20.0, unsigned seed = 41u, bool bandLimited = false)
    {
        auto w = gaussian(static_cast<int>(seconds * sr), seed);
        runBiquad(w, roar::Biquad::lowpass1(sr, 500.0));
        if (bandLimited)   // the same signal at every rate (nothing above 10 kHz)
        {
            runBiquad(w, roar::Biquad::lowpass(sr, 10000.0, roar::Biquad::c_bw4Q1));
            runBiquad(w, roar::Biquad::lowpass(sr, 10000.0, roar::Biquad::c_bw4Q2));
        }
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

    // ============================================================================== VOCODER (D21)

    void testVocoder()
    {
        const double sr = c_fs;
        const auto speech = speechLike(sr);
        // The stream as IN's detector gives it: voiced at 150 Hz on the vowels, unvoiced on the esses.
        std::vector<char> voiced(speech.x.size(), 0);
        for (const auto& [a, b] : speech.vowels)
            std::fill(voiced.begin() + a, voiced.begin() + b, 1);
        const auto frameAt = [&voiced](int i)
        {
            return voiced[static_cast<size_t>(i)] != 0 ? roar::VoiceFrame { 150.0f, 0.95f, true } : roar::VoiceFrame { 150.0f, 0.1f, false };
        };
        const auto wetOf = [&](double gravel, bool trim)
        {
            roar::VocoderModule m;
            m.prepare(sr, 512);
            roar::VocoderSettings s;
            s.on = true;
            s.gravel = static_cast<float>(gravel);
            m.setParams(s);
            m.setNoiseTrimForTests(trim);
            m.reset();
            const auto y = runModuleMono(m, speech.x, 512, frameAt, [](int) {});
            const double g = 0.15 + 0.85 * gravel, mix = 0.85 * g;
            const double norm = std::sqrt((1.0 - mix) * (1.0 - mix) + mix * mix);
            const double dryGain = (1.0 - mix) / norm, wetGain = mix / norm;
            Signal wet(y.size());
            for (size_t i = 0; i < y.size(); ++i)
                wet[i] = static_cast<float>((static_cast<double>(y[i]) - dryGain * speech.x[i]) / wetGain);
            return wet;
        };
        std::vector<std::pair<int, int>> esses, vowelsBefore;
        for (int k = 3; k <= 8; ++k)
        {
            const auto& e = speech.esses[static_cast<size_t>(k - 1)];
            esses.emplace_back(e.first + static_cast<int>(0.030 * sr), e.second - static_cast<int>(0.010 * sr));
            vowelsBefore.emplace_back(e.first - static_cast<int>(0.220 * sr), e.first);
        }
        for (const int pct : { 50, 100 })
        {
            double d[2] {}, vowelLevel[2] {};
            for (const bool trim : { false, true })
            {
                const auto wet = wetOf(pct / 100.0, trim);
                const double ess = bandChangeDb(speech.x, wet, sr, esses, 100.0, 16000.0);
                const double vow = bandChangeDb(speech.x, wet, sr, vowelsBefore, 100.0, 16000.0);
                d[trim ? 1 : 0] = ess - vow;
                vowelLevel[trim ? 1 : 0] = vow;
            }
            std::printf("  S13 VOCODER Gravel %d %%: esses against vowels in the wet %+.2f dB with the trim (%+.2f without); vowels %+.2f / %+.2f dB\n",
                        pct, d[1], d[0], vowelLevel[1], vowelLevel[0]);
            check(std::abs(d[1]) <= 1.5, "S13: esses through the vocoder stand " + db1(d[1]) + " against the vowels at Gravel " + juce::String(pct) + " %");
            check(d[0] - d[1] >= (pct == 50 ? 1.5 : 2.0), "S13: the trim takes only " + db1(d[0] - d[1]) + " off the esses at Gravel " + juce::String(pct) + " %");
            check(std::abs(vowelLevel[1] - vowelLevel[0]) <= 0.1, "S13: the trim moves the vowels by " + db1(vowelLevel[1] - vowelLevel[0]));
        }
    }

    // ============================================================================== ROAR level (D18)

    /** One ROAR instance kept across passes (its learnt makeup lives in the module). */
    struct RoarRig
    {
        roar::RoarModule m;
        double sr;
        int block;

        RoarRig(double rate = c_fs, int blockSize = 512, double drive = 0.8, double tone = 0.0, bool follow = true)
            : sr(rate), block(blockSize)
        {
            m.prepare(sr, block);
            set(drive, tone);
            m.setLevelFollowForTests(follow);
            m.reset();
            m.forgetLevel();
        }

        void set(double drive, double tone = 0.0, bool mic = false)
        {
            roar::RoarSettings s;
            s.drive = static_cast<float>(drive);
            s.tone = static_cast<float>(tone);
            s.voicing = mic ? Params::RoarVoicing::mic : Params::RoarVoicing::line;
            m.setParams(s);
        }

        /** Runs x; perBlock (start sample) before each block. With traces the blocks are 32 samples
            (one control block) and the correction / evidence are recorded after each. */
        Signal run(const Signal& x, std::function<void(int)> perBlock = {}, std::vector<double>* corr = nullptr, std::vector<double>* evid = nullptr)
        {
            const int b = (corr != nullptr || evid != nullptr) ? roar::RoarModule::c_controlBlock : block;
            Signal y = x, right(static_cast<size_t>(b), 0.0f);
            const roar::VoiceFrameView none;
            for (int pos = 0; pos < static_cast<int>(x.size()); pos += b)
            {
                const int n = std::min(b, static_cast<int>(x.size()) - pos);
                if (perBlock)
                    perBlock(pos);
                m.process(y.data() + pos, right.data(), n, false, none);
                if (corr != nullptr)
                    corr->push_back(m.getCorrectionDb());
                if (evid != nullptr)
                    evid->push_back(m.getLearning().evidence);
            }
            return y;
        }
    };

    Signal slice(const Signal& x, double from, double to, double sr)
    {
        const auto a = static_cast<size_t>(std::clamp(from * sr, 0.0, static_cast<double>(x.size())));
        const auto b = static_cast<size_t>(std::clamp(to * sr, 0.0, static_cast<double>(x.size())));
        return Signal(x.begin() + static_cast<std::ptrdiff_t>(a), x.begin() + static_cast<std::ptrdiff_t>(b));
    }

    /** Weighted level (IN's weighting) of out against in over [from, to) seconds. */
    double weightedChangeDb(const Signal& in, const Signal& out, double from, double to, double sr)
    {
        return levels::weightedRmsDb(slice(out, from, to, sr), sr) - levels::weightedRmsDb(slice(in, from, to, sr), sr);
    }

    void testRoarLevel()
    {
        const double sr = c_fs;
        const auto noise = voiceNoise(sr, 8.0);
        const double blockSec = roar::RoarModule::c_controlBlock / sr;
        const auto tIndex = [&](double t) { return static_cast<size_t>(std::max(0.0, t / blockSec - 1.0)); };

        // ---- S14: the start
        {
            RoarRig rig;
            std::vector<double> corr;
            rig.run(noise, {}, &corr);
            double early = 0.0;
            for (size_t k = 0; k <= tIndex(0.19); ++k)
                early = std::max(early, std::abs(corr[k]));
            const double at06 = corr[tIndex(0.6)], at3 = corr[tIndex(3.0)];
            std::printf("  S14 ROAR start: |correction| over the first 0.19 s %.4f dB; %.2f dB at 0.5 s, %.2f at 0.6 s, %.2f at 3 s\n",
                        early, corr[tIndex(0.5)], at06, at3);
            check(early == 0.0, "S14: the correction moves in the first 0.19 s (" + db1(early) + ")");
            check(std::abs(at06 - at3) <= 0.5, "S14: the correction at 0.6 s " + db1(at06) + " is not within 0.5 dB of its 3 s value " + db1(at3));
        }

        // ---- S15a: the static table (follower off); S15b: the learnt level
        {
            const std::vector<int> drives { 0, 20, 45, 62, 80, 100 };
            const std::vector<double> proto { -0.14, -0.28, -0.11, 0.48, 1.19, 0.66 };
            juce::String line;
            for (size_t k = 0; k < drives.size(); ++k)
            {
                RoarRig rig(sr, 512, drives[k] / 100.0, 0.0, false);
                const auto y = rig.run(noise);
                const double d = rmsDbOf(y, static_cast<int>(3.0 * sr), static_cast<int>(8.0 * sr)) - rmsDbOf(noise, static_cast<int>(3.0 * sr), static_cast<int>(8.0 * sr));
                line << drives[k] << " % " << juce::String(d, 2) << "  ";
                check(std::abs(d) <= 1.5 && std::abs(d - proto[k]) <= 0.3,
                      "S15a: ROAR's static makeup at Drive " + juce::String(drives[k]) + " % leaves " + db1(d) + " (prototype " + db1(proto[k]) + ")");
            }
            std::printf("  S15a static makeup, out - in (follower off): %s\n", line.toRawUTF8());
            line.clear();
            double worst = 0.0;
            for (const int drive : drives)
                for (const double tone : { -1.0, 0.0, 1.0 })
                {
                    RoarRig rig(sr, 512, drive / 100.0, tone);
                    const double d = weightedChangeDb(noise, rig.run(noise), 3.0, 8.0, sr);
                    worst = std::abs(d) > std::abs(worst) ? d : worst;
                    check(std::abs(d) <= 0.5, "S15b: learnt level at Drive " + juce::String(drive) + " %, Tone " + juce::String(tone * 100.0, 0) + " is " + db1(d));
                }
            std::printf("  S15b learnt weighted level, seconds 3-8: worst %+.3f dB over 18 settings\n", worst);
        }

        // ---- S15c: held; S15d: the schedule
        {
            RoarRig rig;
            std::vector<double> corr, evid;
            rig.run(voiceNoise(sr, 12.0), {}, &corr, &evid);
            double lo = 1e9, hi = -1e9;
            for (size_t k = tIndex(7.0); k < corr.size(); ++k)
            {
                lo = std::min(lo, corr[k]);
                hi = std::max(hi, corr[k]);
            }
            std::printf("  S15c held: correction %.3f / %.3f / %.3f dB at 1 / 3 / 7 s; 7-12 s travel %.5f dB\n", corr[tIndex(1.0)], corr[tIndex(3.0)], corr[tIndex(7.0)], hi - lo);
            check(hi - lo <= 0.001, "S15c: the correction moves " + db1(hi - lo) + " once the evidence has passed 6 s");
            const size_t w = static_cast<size_t>(std::lround(0.1 / blockSec));
            bool ok = true;
            double worstRate = 0.0;
            for (size_t k = 0; k + w < corr.size(); ++k)
            {
                const double rate = std::abs(corr[k + w] - corr[k]) / (static_cast<double>(w) * blockSec);
                const double e = evid[k];
                const double limit = e >= 6.0 ? 0.0 : e >= 2.0 ? 0.5 : e >= 0.5 ? 1.5 : 6.0;
                if (rate > limit * 1.01 + 1.0e-9)
                    ok = false;
                worstRate = std::max(worstRate, rate);
            }
            std::printf("  S15d schedule: fastest move %.2f dB/s\n", worstRate);
            check(ok, "S15d: the correction moves faster than its schedule allows");
        }

        // ---- S16: the bound
        {
            for (const double level : { -10.0, -50.0 })
            {
                RoarRig rig(sr, 512, 1.0);
                std::vector<double> corr;
                const auto y = rig.run(voiceNoise(sr, 8.0, level), {}, &corr);
                const auto [mn, mx] = std::minmax_element(corr.begin(), corr.end());
                bool finite = true;
                for (float v : y)
                    finite = finite && std::isfinite(v);
                std::printf("  S16 noise at %.0f dBFS, Drive 100 %%: correction %.2f dB at the end (range %.2f .. %.2f)\n", level, corr.back(), *mn, *mx);
                check(*mn >= -6.0 && *mx <= 6.0 && finite, "S16: the correction leaves +-6 dB or the output is not finite at " + juce::String(level, 0) + " dBFS");
            }
        }

        // ---- S17: a pause
        for (const bool quietNoise : { false, true })
        {
            Signal gap(static_cast<size_t>(3.0 * sr), 0.0f);
            if (quietNoise)
                gap = voiceNoise(sr, 3.0, -75.0, 5u);
            const auto x = join({ voiceNoise(sr, 3.0), gap, voiceNoise(sr, 3.0, -20.0, 7u) });
            RoarRig rig;
            std::vector<double> corr;
            rig.run(x, {}, &corr);
            const double a = corr[tIndex(3.0)], b = corr[tIndex(6.0)];
            std::printf("  S17 pause (%s): correction %.4f dB at its start, %.4f at its end\n", quietNoise ? "-75 dBFS noise" : "silence", a, b);
            check(std::abs(a - b) <= 0.01, "S17: the correction moves over a pause by " + db1(b - a));
        }

        // ---- S18a / S18c: a Drive step; S18b: automation; S18d: a small move
        const auto stepRun = [&](double rate, int blockSize, std::vector<double>& corr, Signal& in, Signal& out)
        {
            in = voiceNoise(rate, 8.0, -20.0, 41u, true);
            RoarRig rig(rate, blockSize, 0.2);
            const int stepAt = static_cast<int>(4.0 * rate);
            out = rig.run(in, [&](int pos) { rig.set(pos >= stepAt ? 0.9 : 0.2); }, &corr);
        };
        {
            std::vector<double> corr;
            Signal in, out;
            stepRun(sr, 512, corr, in, out);
            // The glide (30 ms per control block) is within 0.005 of the target after 0.148 s.
            const double glideEnd = 4.0 + 0.149;
            double moved = 0.0;
            for (size_t k = tIndex(4.0); k <= tIndex(glideEnd + 0.15); ++k)
                moved = std::max(moved, std::abs(corr[k] - corr[tIndex(4.0)]));
            size_t first = tIndex(4.0);
            while (first < corr.size() && std::abs(corr[first] - corr[tIndex(4.0)]) < 1.0e-12)
                ++first;
            const double level = weightedChangeDb(in, out, 6.0, 7.0, sr);
            RoarRig fixedLow(sr, 512, 0.2), fixedHigh(sr, 512, 0.9);
            const double fixedStep = std::max(maxStep(fixedLow.run(in), static_cast<int>(3.9 * sr), static_cast<int>(5.0 * sr)),
                                              maxStep(fixedHigh.run(in), static_cast<int>(3.9 * sr), static_cast<int>(5.0 * sr)));
            const double stepStep = maxStep(out, static_cast<int>(3.9 * sr), static_cast<int>(5.0 * sr));
            std::printf("  S18a Drive 20 -> 90 %% at 4 s: first move of the correction %.3f s after the step; level 2-3 s later %+.2f dB; largest step %.4f (fixed %.4f)\n",
                        (static_cast<double>(first + 1) * blockSec) - 4.0, level, stepStep, fixedStep);
            check(moved == 0.0, "S18a: the correction moves before the glide has ended and 0.15 s more has passed");
            check(std::abs(level) <= 1.5, "S18a: 2 s after the step the level is " + db1(level));
            check(stepStep <= 1.1 * fixedStep, "S18a: the Drive step makes a sample step of " + juce::String(stepStep, 4));

            std::vector<std::vector<double>> runs;
            for (const auto& [rate, b] : std::vector<std::pair<double, int>> { { 48000.0, 64 }, { 48000.0, 512 }, { 192000.0, 64 }, { 192000.0, 512 } })
            {
                std::vector<double> c;
                Signal i2, o2;
                stepRun(rate, b, c, i2, o2);
                const double bs = roar::RoarModule::c_controlBlock / rate;
                runs.push_back({ c[static_cast<size_t>(4.5 / bs) - 1], c[static_cast<size_t>(5.0 / bs) - 1], c[static_cast<size_t>(6.0 / bs) - 1] });
            }
            double spread = 0.0;
            for (size_t t = 0; t < 3; ++t)
                for (const auto& r : runs)
                    spread = std::max(spread, std::abs(r[t] - runs[0][t]));
            std::printf("  S18c the same at 48 / 192 kHz, blocks 64 / 512: largest difference %.3f dB\n", spread);
            check(spread <= 0.3, "S18c: the correction after a Drive step depends on the rate or block (" + db1(spread) + ")");
        }
        {
            // (the ramp starts once the learn at 20 % is held, so only the automation can move it)
            RoarRig rig(sr, 512, 0.2);
            std::vector<double> corr;
            const int from = static_cast<int>(7.0 * sr), len = static_cast<int>(3.0 * sr);
            rig.run(voiceNoise(sr, 11.0), [&](int pos) { rig.set(0.2 + 0.7 * std::clamp(static_cast<double>(pos - from) / len, 0.0, 1.0)); }, &corr);
            double travel = 0.0;
            for (size_t k = tIndex(7.0); k <= tIndex(10.0); ++k)
                travel = std::max(travel, std::abs(corr[k] - corr[tIndex(7.0)]));
            std::printf("  S18b Drive ramped 20 -> 90 %% over 3 s: the correction moves %.5f dB\n", travel);
            check(travel <= 0.001, "S18b: the correction moves " + db1(travel) + " during automation");
        }
        {
            RoarRig rig(sr, 512, 0.8);
            std::vector<double> evid;
            const int at = static_cast<int>(4.0 * sr);
            rig.run(voiceNoise(sr, 6.0), [&](int pos) { rig.set(pos >= at ? 0.81 : 0.8); }, nullptr, &evid);
            bool fell = false;
            for (size_t k = 1; k < evid.size(); ++k)
                fell = fell || evid[k] < evid[k - 1] - 1.0e-9;
            check(! fell, "S18d: Drive 80 -> 81 % starts a new learn");
        }

        // ---- S19: the dynamics are kept
        for (const double drive : { 0.8, 1.0 })
        {
            const auto x = join({ voiceNoise(sr, 5.0, -14.0, 1u), voiceNoise(sr, 10.0, -34.0, 2u), voiceNoise(sr, 5.0, -14.0, 3u) });
            RoarRig on(sr, 512, drive), off(sr, 512, drive, 0.0, false);
            std::vector<double> corr;
            const auto yOn = on.run(x, {}, &corr), yOff = off.run(x);
            const auto [mn, mx] = std::minmax_element(corr.begin() + static_cast<std::ptrdiff_t>(tIndex(5.0)), corr.end());
            const auto gap = [&](const Signal& y) { return rmsDbOf(y, static_cast<int>(16.0 * sr), static_cast<int>(20.0 * sr)) - rmsDbOf(y, static_cast<int>(7.0 * sr), static_cast<int>(15.0 * sr)); };
            const double gOn = gap(yOn), gOff = gap(yOff);
            std::printf("  S19 Drive %.0f %%, -14 / -34 / -14 dBFS: correction travel 5-20 s %.3f dB; loud - quiet %.2f dB (follower off %.2f)\n",
                        drive * 100.0, *mx - *mn, gOn, gOff);
            check(*mx - *mn <= 0.6, "S19: the correction travels " + db1(*mx - *mn) + " across a quiet section");
            check(std::abs(gOn - gOff) <= 0.6, "S19: the follower changes the loud - quiet difference by " + db1(gOn - gOff));
        }

        // ---- S19b: passes; S19c: the start position
        {
            RoarRig rig;
            std::vector<double> c1, c2, c3;
            rig.run(noise, {}, &c1);
            rig.m.reset();
            const auto y2 = rig.run(noise, {}, &c2);
            rig.m.reset();
            const auto y3 = rig.run(noise, {}, &c3);
            std::printf("  S19b passes: pass 1 ends at %.4f dB, pass 2 starts at %.4f and ends at %.4f; pass 3 differs by %.2g\n", c1.back(), c2.front(), c2.back(), maxAbsDiff(y2, y3));
            check(std::abs(c2.front() - c1.back()) <= 0.001, "S19b: pass 2 does not start with pass 1's correction");
            check(maxAbsDiff(y2, y3) <= 1.0e-6, "S19b: passes 2 and 3 differ");

            const auto half = slice(noise, 4.0, 8.0, sr);
            const auto compare = [&](bool follow)
            {
                RoarRig r(sr, 512, 0.8, 0.0, follow);
                r.run(noise);
                r.m.reset();
                const auto full = r.run(noise);
                r.m.reset();
                const auto part = r.run(half);
                std::array<double, 3> d {};
                for (int k = 0; k < 3; ++k)
                    d[static_cast<size_t>(k)] = rmsDbOf(part, static_cast<int>(k * sr), static_cast<int>((k + 1) * sr))
                                              - rmsDbOf(full, static_cast<int>((4.0 + k) * sr), static_cast<int>((5.0 + k) * sr));
                return d;
            };
            const auto dOn = compare(true), dOff = compare(false);
            std::printf("  S19c second half alone against pass 2: %+.3f / %+.3f / %+.3f dB (follower off %+.3f / %+.3f / %+.3f)\n",
                        dOn[0], dOn[1], dOn[2], dOff[0], dOff[1], dOff[2]);
            bool ok = true;
            for (size_t k = 0; k < 3; ++k)
                ok = ok && std::abs(dOn[k] - dOff[k]) <= 0.05;
            check(ok, "S19c: the same audio renders at another level when the pass starts later");
        }

        // ---- S19d: louder sound re-opens the learn
        {
            const auto x = join({ voiceNoise(sr, 8.0, -30.0, 11u), voiceNoise(sr, 4.0, -14.0, 12u) });
            RoarRig rig(sr, 512, 1.0);
            std::vector<double> corr, evid;
            rig.run(x, {}, &corr, &evid);
            const size_t change = tIndex(8.0);
            const size_t firstFrame = change + static_cast<size_t>(std::lround(0.1 / blockSec)) + 1;
            bool monotonic = true;
            for (size_t k = change + 1; k <= tIndex(11.9); ++k)
                monotonic = monotonic && corr[k] >= corr[k - 1] - 1.0e-9;
            const double rise = corr[tIndex(11.9)] - corr[change];
            std::printf("  S19d quiet then loud: correction %.2f dB before, %.2f / %.2f / %.2f at 0.5 / 1 / 3.9 s after; evidence %.2f s a frame after the change\n",
                        corr[change], corr[tIndex(8.5)], corr[tIndex(9.0)], corr[tIndex(11.9)], evid[firstFrame]);
            check(evid[firstFrame] < 2.0, "S19d: louder sound does not re-open the learn (evidence " + juce::String(evid[firstFrame], 2) + " s)");
            check(monotonic && rise >= 2.5, "S19d: the correction rises " + db1(rise) + (monotonic ? "" : ", not monotonically"));

            const auto y = join({ voiceNoise(sr, 8.0, -14.0, 12u), voiceNoise(sr, 4.0, -30.0, 11u) });
            RoarRig rev(sr, 512, 1.0);
            std::vector<double> c2;
            rev.run(y, {}, &c2);
            const auto [mn, mx] = std::minmax_element(c2.begin() + static_cast<std::ptrdiff_t>(tIndex(8.0)), c2.end());
            std::printf("  S19d loud then quiet: the correction moves %.3f dB in the quiet part\n", *mx - *mn);
            check(*mx - *mn < 0.6, "S19d: quieter sound moves the correction by " + db1(*mx - *mn));
        }

        // ---- S20: rates and blocks; S20b: learning carried; S20c: relearn
        {
            juce::String line;
            for (const double rate : { 44100.0, 96000.0, 192000.0 })
            {
                const auto x = voiceNoise(rate, 8.0);
                double worst = 0.0;
                for (const int drive : { 0, 45, 100 })
                {
                    RoarRig rig(rate, 512, drive / 100.0);
                    const double d = weightedChangeDb(x, rig.run(x), 3.0, 8.0, rate);
                    worst = std::abs(d) > std::abs(worst) ? d : worst;
                }
                line << juce::String(rate / 1000.0, 1) << " kHz " << juce::String(worst, 3) << "  ";
                check(std::abs(worst) <= 0.5, "S20: learnt level at " + juce::String(rate / 1000.0, 1) + " kHz is " + db1(worst));
            }
            RoarRig ref(sr, 512);
            const auto y512 = ref.run(noise);
            double blocks = 0.0;
            for (const int b : { 16, 4096 })
            {
                RoarRig rig(sr, b);
                blocks = std::max(blocks, maxAbsDiff(y512, rig.run(noise)));
            }
            std::printf("  S20 learnt level (worst of Drive 0 / 45 / 100 %%): %s; blocks 16 / 4096 against 512: %.2g\n", line.toRawUTF8(), blocks);
            check(blocks <= 1.0e-6, "S20: ROAR's learnt makeup depends on the block size (" + juce::String(blocks) + ")");
        }
        {
            RoarRig a;
            const auto first = a.run(noise);
            const auto learnt = a.m.getLearning();
            a.m.reset();
            a.run(noise);
            a.m.reset();
            const auto third = a.run(noise);
            RoarRig b;
            b.m.restoreLearning(learnt);
            const auto restored = b.run(noise);
            b.m.forgetLevel();
            b.m.reset();
            const auto again = b.run(noise);
            std::printf("  S20b restored learning against a third pass: %.2g; after forgetLevel against a first pass: %.2g\n",
                        maxAbsDiff(restored, third), maxAbsDiff(again, first));
            check(maxAbsDiff(restored, third) <= 1.0e-6, "S20b: a restored learning does not render as a later pass");
            check(maxAbsDiff(again, first) <= 1.0e-6, "S20b: forgetLevel does not give a first pass again");
        }
        {
            // A copy (the twin chain's reorder) continues exactly as the original.
            RoarRig a, b;
            const auto head = slice(noise, 0.0, 1.0, sr), tail = slice(noise, 1.0, 1.5, sr);
            a.run(head);
            b.m.copyStateFrom(a.m);
            const double d = maxAbsDiff(a.run(tail), b.run(tail));
            std::printf("  ROAR copy continues: %.2g\n", d);
            check(d <= 1.0e-7, "ROAR: a copied instance does not continue as the original (" + juce::String(d) + ")");
        }
        {
            RoarRig rig;
            std::vector<double> corr, evid;
            rig.run(noise);
            const double before = rig.m.getCorrectionDb();
            rig.m.relearnLevel();
            const double after = rig.m.getCorrectionDb(), e0 = rig.m.getLearning().evidence;
            rig.run(slice(noise, 0.0, 1.0, sr), {}, &corr, &evid);
            std::printf("  S20c relearn: correction %.4f -> %.4f dB, evidence %.2f, then %.2f s after 1 s\n", before, after, e0, evid.back());
            check(std::abs(after - before) < 1.0e-9 && e0 == 0.0 && evid.back() > 0.5 && evid.back() < 1.2, "S20c: relearnLevel does not start the schedule again from the held correction");
        }
    }

    // ============================================================================== ROAR law B (finding 14)

    double harmonicDbc(const Signal& y, double sr, double f0, int k, int from, int to)
    {
        return toDb(toneAmplitude(y, sr, k * f0, from, to) / std::max(1.0e-30, toneAmplitude(y, sr, f0, from, to)));
    }

    double thdDbOf(const Signal& y, double sr, double f0, int from, int to)
    {
        double h = 0.0;
        for (int k = 2; k <= 10 && k * f0 < 0.45 * sr; ++k)
            h += std::pow(toneAmplitude(y, sr, k * f0, from, to), 2.0);
        return 10.0 * std::log10(h / std::pow(std::max(1.0e-30, toneAmplitude(y, sr, f0, from, to)), 2.0) + 1.0e-30);
    }

    /** Energy (dB re the whole) of bins further than 30 Hz from every harmonic of f0, up to 20 kHz. */
    double inharmonicDb(const Signal& y, double sr, double f0, int from)
    {
        constexpr int order = 15;
        const auto p = spectrum(y, from, order);
        const double binHz = sr / (1 << order);
        double all = 0.0, off = 0.0;
        for (size_t k = 1; k < p.size(); ++k)
        {
            const double f = static_cast<double>(k) * binHz;
            if (f > 20000.0)
                break;
            all += p[k];
            const double nearest = std::round(f / f0) * f0;
            if (std::abs(f - nearest) > 30.0)
                off += p[k];
        }
        return 10.0 * std::log10((off + 1.0e-30) / (all + 1.0e-30));
    }

    void testRoarLaw()
    {
        const double sr = c_fs;
        const int previous = roar::RoarModule::getLawForTests();
        const auto toneRun = [&](int law, double drive, double hz, double rmsDb, double seconds = 1.0)
        {
            roar::RoarModule::setLawForTests(law);
            RoarRig rig(sr, 512, drive, 0.0, false);
            return rig.run(makeSine(sr, seconds, hz, std::sqrt(2.0) * std::pow(10.0, rmsDb / 20.0)));
        };
        const int a = static_cast<int>(0.5 * sr), b = static_cast<int>(1.0 * sr);

        // H3 at 1 kHz, -20 dBFS RMS
        {
            juce::String line;
            const std::vector<std::tuple<int, double, double>> bounds { { 30, -45.0, -32.0 }, { 50, -27.0, -18.0 }, { 80, -20.0, -12.0 } };
            for (const auto& [pct, lo, hi] : bounds)
            {
                const double h3b = harmonicDbc(toneRun(1, pct / 100.0, 1000.0, -20.0), sr, 1000.0, 3, a, b);
                const double h3a = harmonicDbc(toneRun(0, pct / 100.0, 1000.0, -20.0), sr, 1000.0, 3, a, b);
                line << pct << " % B " << juce::String(h3b, 1) << " (A " << juce::String(h3a, 1) << ")  ";
                check(h3b >= lo && h3b <= hi, "law B: H3 at " + juce::String(pct) + " % is " + juce::String(h3b, 1) + " dBc");
            }
            std::printf("  law B H3 at 1 kHz, -20 dBFS RMS: %s\n", line.toRawUTF8());
        }
        // THD rises with Drive (law B)
        for (const double hz : { 200.0, 1000.0 })
        {
            double prev = -400.0, peak = -400.0;
            bool rising = true, risingTo80 = true;
            juce::String line;
            for (int pct = 0; pct <= 100; pct += 10)
            {
                const double t = thdDbOf(toneRun(1, pct / 100.0, hz, -20.0), sr, hz, a, b);
                rising = rising && t > prev;
                if (pct <= 80)
                    risingTo80 = rising;
                peak = std::max(peak, t);
                prev = t;
                line << juce::String(t, 1) << " ";
            }
            std::printf("  law B THD at %.0f Hz, Drive 0..100 %%: %s\n", hz, line.toRawUTF8());
            // (At 1 kHz the THD of law B as specified peaks near 80 % and falls about 0.5 dB to 100 %:
            // the output low-pass closes on the upper harmonics. Rising to 80 % and within 1 dB above.)
            if (hz < 500.0)
                check(rising, "law B: THD at " + juce::String(hz, 0) + " Hz does not rise with Drive over the whole travel");
            else
                check(risingTo80 && prev >= peak - 1.0, "law B: THD at " + juce::String(hz, 0) + " Hz does not rise to 80 % and hold within 1 dB");
        }
        // Aliasing: 2.7 kHz at -6 dBFS RMS
        {
            juce::String line;
            double b100 = 0.0, a100 = 0.0;
            for (const int pct : { 45, 80, 100 })
            {
                const double ib = inharmonicDb(toneRun(1, pct / 100.0, 2700.0, -6.0), sr, 2700.0, a / 2);
                const double ia = inharmonicDb(toneRun(0, pct / 100.0, 2700.0, -6.0), sr, 2700.0, a / 2);
                line << pct << " % B " << juce::String(ib, 1) << " (A " << juce::String(ia, 1) << ")  ";
                if (pct < 100)
                    check(ib < -50.0, "law B: energy off the harmonics of a hot 2.7 kHz tone at " + juce::String(pct) + " % is " + juce::String(ib, 1) + " dB");
                else
                {
                    b100 = ib;
                    a100 = ia;
                }
            }
            std::printf("  inharmonic energy, 2.7 kHz at -6 dBFS RMS: %s\n", line.toRawUTF8());
            check(b100 <= a100 + 1.0, "law B: more aliasing than law A at Drive 100 % (" + juce::String(b100, 1) + " against " + juce::String(a100, 1) + " dB)");
        }
        // A full-scale input at the top of B's travel stays bounded
        {
            const auto y = toneRun(1, 1.0, 200.0, -3.0);
            double peak = 0.0;
            bool finite = true;
            for (float v : y)
            {
                finite = finite && std::isfinite(v);
                peak = std::max(peak, static_cast<double>(std::abs(v)));
            }
            std::printf("  law B, 0 dBFS peak 200 Hz at Drive 100 %%: output peak %.2f\n", peak);
            check(finite && peak < 4.0, "law B: a full-scale input at Drive 100 % gives a peak of " + juce::String(peak, 2));
        }
        // S15a / S15b with law B
        {
            roar::RoarModule::setLawForTests(1);
            const auto noise = voiceNoise(sr, 8.0);
            const std::vector<int> drives { 0, 20, 45, 62, 80, 100 };
            const std::vector<double> firstRun { -0.14, 0.80, 1.34, 1.43, 1.21, 0.56 };   // recorded from the first run (pins B's table and trim)
            juce::String line;
            double worst = 0.0;
            for (size_t k = 0; k < drives.size(); ++k)
            {
                const int drive = drives[k];
                RoarRig off(sr, 512, drive / 100.0, 0.0, false);
                const auto y = off.run(noise);
                const double d = rmsDbOf(y, static_cast<int>(3.0 * sr), static_cast<int>(8.0 * sr)) - rmsDbOf(noise, static_cast<int>(3.0 * sr), static_cast<int>(8.0 * sr));
                line << drive << " % " << juce::String(d, 2) << "  ";
                check(std::abs(d - firstRun[k]) <= 0.3, "law B S15a: the static makeup at Drive " + juce::String(drive) + " % leaves " + db1(d));
                for (const double tone : { -1.0, 0.0, 1.0 })
                {
                    RoarRig on(sr, 512, drive / 100.0, tone);
                    const double w = weightedChangeDb(noise, on.run(noise), 3.0, 8.0, sr);
                    worst = std::abs(w) > std::abs(worst) ? w : worst;
                    check(std::abs(w) <= 0.5, "law B S15b: learnt level at Drive " + juce::String(drive) + " %, Tone " + juce::String(tone * 100.0, 0) + " is " + db1(w));
                }
            }
            std::printf("  law B S15a static makeup (follower off): %s; S15b learnt, worst %+.3f dB\n", line.toRawUTF8(), worst);
        }
        roar::RoarModule::setLawForTests(previous);
    }

    // ============================================================================== DETAIL (D17)

    struct DetailRun
    {
        Signal y;
        std::vector<float> gain;   // getGainDb () per sample (traced runs)
        int latency = 0;
    };

    DetailRun runDetail(const Signal& x, double sr, double low, double high, std::function<roar::VoiceFrame(int)> frameAt,
                        int block = 512, bool snappy = false, bool trace = false)
    {
        roar::DetailModule m;
        m.prepare(sr, block);
        roar::DetailSettings s;
        s.low = static_cast<float>(low);
        s.high = static_cast<float>(high);
        s.speed = snappy ? Params::DetailSpeed::snappy : Params::DetailSpeed::natural;
        m.setParams(s);
        m.reset();
        DetailRun r;
        r.latency = m.getLatencySamples();
        if (trace)
            r.y = runModuleMono(m, x, 1, frameAt, [&](int) { r.gain.push_back(m.getGainDb()); });
        else
            r.y = runModuleMono(m, x, block, frameAt, [](int) {});
        return r;
    }

    /**
     * Distortion of y against x (dB): y over [from, to) less its best fit by a constant gain on x
     * delayed by `latency`, against that fit. A level stage that holds its gain steady over a tone
     * scores at the float floor; one that follows each cycle scores the wobble it makes.
     */
    double distortionDb(const Signal& x, const Signal& y, int latency, int from, int to)
    {
        double xy = 0.0, xx = 0.0;
        for (int i = from; i < to; ++i)
        {
            const double xi = x[static_cast<size_t>(i - latency)], yi = y[static_cast<size_t>(i)];
            xy += xi * yi;
            xx += xi * xi;
        }
        const double g = xy / std::max(xx, 1.0e-30);
        double e = 0.0, f = 0.0;
        for (int i = from; i < to; ++i)
        {
            const double fit = g * x[static_cast<size_t>(i - latency)];
            const double d = static_cast<double>(y[static_cast<size_t>(i)]) - fit;
            e += d * d;
            f += fit * fit;
        }
        return 10.0 * std::log10((e + 1.0e-300) / std::max(f, 1.0e-300));
    }

    /** Harmonics 2-11 against the fundamental (dB), Hann-windowed over [from, to). */
    double thdDb(const Signal& y, int from, int to, double hz, double sr)
    {
        const int n = to - from;
        double harm = 0.0, fund = 0.0;
        for (int k = 1; k <= 11; ++k)
        {
            double re = 0.0, im = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double w = 0.5 - 0.5 * std::cos(2.0 * c_pi * i / n);
                const double ph = 2.0 * c_pi * k * hz * static_cast<double>(i) / sr;
                const double v = w * y[static_cast<size_t>(from + i)];
                re += v * std::cos(ph);
                im += v * std::sin(ph);
            }
            (k == 1 ? fund : harm) += re * re + im * im;
        }
        return 10.0 * std::log10((harm + 1.0e-300) / std::max(fund, 1.0e-300));
    }

    Signal tone(double sr, double seconds, double hz, double peakDb, double vibrato = 0.0, double tremolo = 0.0, double decayDbPerSecond = 0.0)
    {
        Signal s(static_cast<size_t>(seconds * sr));
        double ph = 0.0;
        const double a0 = std::pow(10.0, peakDb / 20.0);
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double t = static_cast<double>(i) / sr;
            const double f = hz * (1.0 + vibrato * std::sin(2.0 * c_pi * 5.0 * t));
            const double amp = a0 * (1.0 + tremolo * std::sin(2.0 * c_pi * 4.0 * t)) * std::pow(10.0, -decayDbPerSecond * t / 20.0);
            s[i] = static_cast<float>(amp * std::sin(ph));
            ph += 2.0 * c_pi * f / sr;
        }
        return s;
    }

    void testDetail()
    {
        const auto voicedAt = [](double f0, float confidence = 1.0f) { return [=](int) { return roar::VoiceFrame { static_cast<float>(f0), confidence, true }; }; };
        const auto unvoiced = [](double f0) { return [=](int) { return roar::VoiceFrame { static_cast<float>(f0), 0.0f, false }; }; };
        struct Setting { double low, high; const char* name; };
        const std::vector<Setting> settings { { 0.60, 0.30, "Low 60 / High 30" }, { 0.25, 0.19, "defaults" } };

        // ---- S20d / S20e / S20f: steady tones
        struct Case { double hz, sr, vibrato; double bound; bool relative; const char* tag; };
        const std::vector<Case> cases {
            { 61.7, 48000.0, 0.0, -100.0, true, "S20d" }, { 97.3, 48000.0, 0.0, -100.0, true, "S20d" }, { 143.7, 48000.0, 0.0, -100.0, true, "S20d" },
            { 200.0, 44100.0, 0.0, -100.0, true, "S20d" },
            { 60.0, 48000.0, 0.0, -120.0, false, "S20e" }, { 100.0, 48000.0, 0.0, -120.0, false, "S20e" },
            { 100.0, 48000.0, 0.03, -100.0, false, "S20f" },
        };
        for (const auto& st : settings)
        {
            juce::String line;
            for (const auto& c : cases)
            {
                const auto x = tone(c.sr, 3.0, c.hz, -17.0, c.vibrato);
                const int from = static_cast<int>(1.0 * c.sr), to = static_cast<int>(3.0 * c.sr);
                const auto on = runDetail(x, c.sr, st.low, st.high, voicedAt(c.vibrato > 0.0 ? 100.0 : c.hz));
                const auto off = runDetail(x, c.sr, st.low, st.high, unvoiced(c.hz));
                const double dOn = distortionDb(x, on.y, on.latency, from, to), dOff = distortionDb(x, off.y, off.latency, from, to);
                line << juce::String(c.hz, 1) << (c.vibrato > 0.0 ? " vib" : "") << " " << juce::String(dOn, 0) << " (" << juce::String(dOff, 0) << ")  ";
                const juce::String what = juce::String(c.tag) + ": DETAIL " + st.name + " on " + juce::String(c.hz, 1) + " Hz" + (c.vibrato > 0.0 ? " with vibrato" : "");
                check(dOn < c.bound, what + " distorts at " + juce::String(dOn, 1) + " dB");
                if (c.relative)
                    check(dOn <= dOff - 40.0, what + ": " + juce::String(dOn, 1) + " dB, not 40 dB under the unvoiced run's " + juce::String(dOff, 1));
            }
            std::printf("  S21 DETAIL %s, distortion dB voiced (unvoiced): %s\n", st.name, line.toRawUTF8());
        }

        // ---- S21d: amplitude changes still pass
        {
            const double sr = 48000.0;
            const auto trem = tone(sr, 3.0, 100.0, -17.0, 0.0, 0.17);
            const auto y = runDetail(trem, sr, 0.60, 0.30, voicedAt(100.0)).y;
            double hi = -400.0, lo = 400.0;
            const int w = static_cast<int>(0.020 * sr);
            for (int i = static_cast<int>(1.0 * sr); i + w <= static_cast<int>(3.0 * sr); i += w / 2)
            {
                const double r = rmsDbOf(y, i, i + w);
                hi = std::max(hi, r);
                lo = std::min(lo, r);
            }
            const auto decay = tone(sr, 3.0, 97.3, -17.0, 0.0, 0.0, 6.0);
            const auto dOn = runDetail(decay, sr, 0.60, 0.30, voicedAt(97.3)), dOff = runDetail(decay, sr, 0.60, 0.30, unvoiced(97.3));
            // THD here, not the fit residual: a level stage follows a decaying tone's level by design.
            const double a = thdDb(dOn.y, static_cast<int>(1.0 * sr), static_cast<int>(3.0 * sr), 97.3, sr);
            const double b = thdDb(dOff.y, static_cast<int>(1.0 * sr), static_cast<int>(3.0 * sr), 97.3, sr);
            std::printf("  S21d 4 Hz tremolo of 17 %%: output depth %.2f dB; 97.3 Hz decaying 6 dB/s: %.1f dB (unvoiced %.1f)\n", hi - lo, a, b);
            check(hi - lo >= 1.5 && hi - lo <= 3.0, "S21d: a 4 Hz tremolo comes out " + db1(hi - lo) + " deep");
            check(std::abs(a - b) <= 3.0, "S21d: a decaying tone " + juce::String(a, 1) + " dB against the unvoiced run's " + juce::String(b, 1));
        }

        // ---- S22: a stream below the confidence line is today's behaviour
        {
            const double sr = 48000.0;
            const auto x = tone(sr, 1.5, 97.3, -17.0);
            const auto a = runDetail(x, sr, 0.60, 0.30, voicedAt(97.3, 0.5)).y, b = runDetail(x, sr, 0.60, 0.30, unvoiced(97.3)).y;
            check(a == b, "S22: a stream at confidence 0.5 changes DETAIL");
        }

        // ---- S23: noise after a vowel comes up as before
        {
            const double sr = 48000.0;
            const auto v = vowel(sr, 0.5, 150.0, -20.0);
            auto nz = gaussian(static_cast<int>(0.5 * sr), 23u);
            scaleToRmsDb(nz, -45.0);
            const auto x = join({ v, toSignal(nz) });
            const int at = static_cast<int>(v.size());
            const auto frames = [at](int i) { return i < at ? roar::VoiceFrame { 150.0f, 1.0f, true } : roar::VoiceFrame { 150.0f, 0.0f, false }; };
            const auto reach = [&](const DetailRun& r)
            {
                const double settled = r.gain.back();
                int k = at;
                while (k < static_cast<int>(r.gain.size()) && std::abs(r.gain[static_cast<size_t>(k)] - settled) > 1.0)
                    ++k;
                return 1000.0 * (k - at) / sr;
            };
            const double tOn = reach(runDetail(x, sr, 0.60, 0.30, frames, 512, true, true));
            const double tOff = reach(runDetail(x, sr, 0.60, 0.30, unvoiced(150.0), 512, true, true));
            std::printf("  S23 noise after a vowel, Snappy: within 1 dB of its boost after %.1f ms (unvoiced stream %.1f ms)\n", tOn, tOff);
            check(tOn <= tOff + 12.0, "S23: the boost on noise after a vowel arrives " + juce::String(tOn - tOff, 1) + " ms later");
        }

        // ---- S24: the hold's extremes
        {
            roar::DetailModule m;
            m.prepare(48000.0, 64);
            m.setParams(roar::DetailSettings {});
            m.reset();
            Signal x(64, 0.1f), right(64, 0.0f);
            const auto holdFor = [&](float f0)
            {
                roar::VoiceStream stream;
                stream.prepare(64);
                stream.reset();
                stream.beginChunk(64);
                stream.fill(roar::VoiceFrame { f0, 1.0f, true });
                m.process(x.data(), right.data(), 64, false, stream.view());
                return m.getHoldSamples();
            };
            const int h20 = holdFor(20.0f), h0 = holdFor(0.0f), hNan = holdFor(std::numeric_limits<float>::quiet_NaN()), hInf = holdFor(std::numeric_limits<float>::infinity());
            std::printf("  S24 hold at f0 20 Hz: %d samples; f0 0 / NaN / inf: %d / %d / %d\n", h20, h0, hNan, hInf);
            check(h20 == 1320 && h0 == 0 && hNan == 0 && hInf == 0, "S24: the hold is not 27.5 ms at 20 Hz, or not 0 without a finite pitch");
        }

        // ---- S25: block sizes with a stream that moves every frame
        {
            const double sr = 48000.0;
            const auto x = tone(sr, 2.0, 90.0, -17.0, 0.04);
            const auto frames = [](int i)
            {
                const int f = i / roar::c_voiceFrameSize;
                return roar::VoiceFrame { 70.0f + static_cast<float>((f * 7) % 41), (f % 9) == 0 ? 0.4f : 0.9f, (f % 13) != 0 };
            };
            const auto ref = runDetail(x, sr, 0.60, 0.30, frames, 512).y;
            double worst = 0.0;
            for (const int b : { 16, 480, 4096 })
                worst = std::max(worst, maxAbsDiff(ref, runDetail(x, sr, 0.60, 0.30, frames, b).y));
            std::printf("  S25 blocks 16 / 480 / 4096 against 512: %.2g\n", worst);
            check(worst <= 1.0e-6, "S25: DETAIL depends on the block size (" + juce::String(worst) + ")");
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

        // S27 pins the gate's ceiling at the default: a room 12 dB louder than S26's sits above
        // what Gate 47 % reaches and stays (nearly) open, while the -50 dBFS room of the same
        // signal is gated deep. Both halves in one check, so it cannot pass with a gate that does
        // nothing.
        gateRun(-38.0, gaps, bursts);
        const double gapMean = std::accumulate(gaps.begin(), gaps.end(), 0.0) / static_cast<double>(gaps.size());
        std::printf("  S27 CLEAN gate 47 %%, room -38 dBFS: gaps %.2f dB (mean); room -50 dBFS: gaps at most %.2f dB\n", gapMean, gapMax);
        check(gapMean >= -2.0 && gapMean <= 0.0 && gapMax <= -12.0,
              "S27: the gate's ceiling moved: a -38 dBFS room is gated by " + db1(gapMean) + " (expected -2 .. 0) with a -50 dBFS room at "
                  + db1(gapMax) + " (expected <= -12)");
    }

    void testRoarFeed()
    {
        // S20d-f: ROAR's learnt makeup in the whole plug-in (spec 4.9): it learns again when what
        // feeds it changes, and it travels with the session.
        const double sr = 48000.0;
        const int block = 512;
        Signal quiet = makeVoice(sr, 6.0);
        for (auto& v : quiet)
            v *= 0.25f;   // -12 dB: IN's Auto Level climbs at the start of the take
        const auto setup = [&](bool autoLevel, float trimDb)
        {
            auto p = makeProcessor(sr, block);
            setAllModules(*p, false);
            setParam(*p, Params::roarOn, 1.0f);
            setParam(*p, Params::roar, 80.0f);
            setParam(*p, Params::outMatch, 0.0f);
            setParam(*p, Params::inAuto, autoLevel ? 1.0f : 0.0f);
            setParam(*p, Params::inTrim, trimDb);
            restart(*p, sr, block);
            return p;
        };
        const auto roarOf = [](RoarschachAudioProcessor& p) { return p.getChain().getLevels().roar; };
        const auto oneBlock = [&](RoarschachAudioProcessor& p, const Signal& x, int from)
        {
            (void) render(p, dual(Signal(x.begin() + from, x.begin() + from + block)), block);
        };

        // S20d: a take at the start of Auto Level's learn. ROAR learns again once IN's gain has
        // moved, so its makeup is the one a take already at that level gets.
        {
            auto p = setup(true, 0.0f);
            (void) render(*p, dual(quiet), block);
            const double gainDb = p->getChain().getIn().getAutoGainDb();
            const auto climbing = roarOf(*p);
            auto q = setup(false, static_cast<float>(gainDb));
            (void) render(*q, dual(quiet), block);
            const auto settled = roarOf(*q);
            std::printf("  S20d ROAR after IN's climb (+%.1f dB): correction %+.2f dB over %.2f s against %+.2f dB over %.2f s with the gain set\n",
                        gainDb, climbing.corrDb, climbing.seconds, settled.corrDb, settled.seconds);
            check(gainDb > 6.0, "S20d: IN did not climb (" + db1(gainDb) + ")");
            check(climbing.seconds < settled.seconds - 0.1, "S20d: ROAR did not learn again while IN's gain moved");
            check(std::abs(climbing.corrDb - settled.corrDb) <= 0.5,
                  "S20d: ROAR's makeup after IN's climb is " + db1(climbing.corrDb) + " against " + db1(settled.corrDb));
        }

        // S20e: a module switched in before ROAR starts a new learn (the correction held); one
        // after it does not.
        {
            auto p = setup(false, 12.0f);
            (void) render(*p, dual(quiet), block);
            const auto before = roarOf(*p);
            setParam(*p, Params::tameOn, 1.0f);   // after ROAR in the default order
            oneBlock(*p, quiet, 0);
            const auto afterTame = roarOf(*p);
            setParam(*p, Params::cleanOn, 1.0f);  // before ROAR
            oneBlock(*p, quiet, block);
            const auto afterClean = roarOf(*p);
            std::printf("  S20e evidence %.3f s; TAME switched in after ROAR: %.3f s; CLEAN before it: %.3f s (correction %+.3f -> %+.3f dB)\n",
                        before.evidence, afterTame.evidence, afterClean.evidence, afterTame.corrDb, afterClean.corrDb);
            check(before.evidence > 0.2 && afterTame.evidence >= before.evidence - 1.0e-9, "S20e: a module after ROAR started a new learn");
            check(afterClean.evidence == 0.0 && std::abs(afterClean.corrDb - afterTame.corrDb) < 1.0e-9,
                  "S20e: CLEAN switched in before ROAR did not start a new learn from the held correction");
        }

        // S20f: the learning is saved with the session and restored exactly; a state without it
        // (or with a malformed one) loads as a new learn.
        {
            auto p = setup(true, 0.0f);
            (void) render(*p, dual(quiet), block);
            p->reset();
            (void) render(*p, dual(quiet), block);
            const auto saved = roarOf(*p);
            juce::MemoryBlock state;
            p->getStateInformation(state);
            auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
            check(xml != nullptr && xml->hasAttribute("levelRoar"), "S20f: the state carries no ROAR learning");

            auto q = makeProcessor(sr, block);
            q->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            restart(*q, sr, block);
            (void) render(*q, dual(Signal(static_cast<size_t>(block), 0.0f)), block);   // the restore lands at a block start
            const auto restored = roarOf(*q);
            const double dS1 = std::abs(restored.s1 - saved.s1) / std::max(1.0e-30, saved.s1);
            std::printf("  S20f restored: correction %+.6f / %+.6f dB, evidence %.6f / %.6f s, s1 within %.1e\n",
                        restored.corrDb, saved.corrDb, restored.evidence, saved.evidence, dS1);
            check(std::abs(restored.corrDb - saved.corrDb) < 1.0e-9 && std::abs(restored.evidence - saved.evidence) < 1.0e-9 && dS1 < 1.0e-12
                      && restored.anchored == saved.anchored && std::abs(restored.anchorA - saved.anchorA) < 1.0e-12,
                  "S20f: ROAR's learning did not survive the state");

            // Finite but impossible learnings (a damaged or hand-edited state) are treated as missing too.
            const auto asText = [](const roar::RoarModule::LevelLearning& l)
            {
                juce::String t;
                for (const double v : l.toValues())
                {
                    juce::int64 bits = 0;
                    std::memcpy(&bits, &v, sizeof bits);
                    t << (t.isEmpty() ? "" : " ") << juce::String::toHexString(bits).paddedLeft('0', 16);
                }
                return t;
            };
            std::vector<juce::String> texts { "", "1 2 three 4 5 6 7 8 9 10", "1 2 3" };
            {
                auto huge = saved;
                huge.s1 *= 1.0e20;
                huge.s2 *= 1.0e40;   // consistent with s1, but far louder than anything ROAR is fed
                auto moreEvidence = saved;
                moreEvidence.evidence = saved.seconds * 2.0;
                auto ages = saved;
                ages.seconds = 1.0e12;
                auto anchor = saved;
                anchor.anchorA = 5.0;
                auto sumsWithoutSeconds = saved;
                sumsWithoutSeconds.seconds = sumsWithoutSeconds.evidence = 0.0;
                check(roar::RoarModule::isPlausible(saved) && roar::RoarModule::isPlausible(roar::RoarModule::LevelLearning {}),
                      "S20f: a real learning or a new one is taken as damaged");
                for (const auto& l : { huge, moreEvidence, ages, anchor, sumsWithoutSeconds })
                    texts.push_back(asText(l));
            }
            int absurdNew = 0;
            for (const auto& text : texts)
            {
                auto edited = std::make_unique<juce::XmlElement>(*xml);
                if (juce::String(text).isEmpty())
                    edited->removeAttribute("levelRoar");
                else
                    edited->setAttribute("levelRoar", text);
                juce::MemoryBlock other;
                juce::AudioProcessor::copyXmlToBinary(*edited, other);
                p->setStateInformation(other.getData(), static_cast<int>(other.getSize()));
                (void) render(*p, dual(Signal(static_cast<size_t>(block), 0.0f)), block);
                const auto fresh = roarOf(*p);
                const bool isNew = fresh.corrDb == 0.0 && fresh.seconds == 0.0 && fresh.s1 == 0.0;
                absurdNew += isNew && text.length() > 40 ? 1 : 0;
                check(isNew, juce::String("S20f: a state with levelRoar \"") + text + "\" did not load as a new learn");
            }
            std::printf("  S20f damaged learnings (too loud, evidence over the seconds, 1e12 s, Drive anchor 5, sums without seconds): %d of 5 load as a new learn\n", absurdNew);
        }
    }
}

void testStageB3()
{
    std::printf("[stageb3] DE-ESS, TAME, VOCODER, ROAR level, DETAIL hold, CLEAN gate\n");
    stageb3::testDeess();
    stageb3::testTame();
    stageb3::testVocoder();
    stageb3::testRoarLevel();
    stageb3::testRoarLaw();
    stageb3::testDetail();
    stageb3::testCleanGate();
    stageb3::testRoarFeed();
}
