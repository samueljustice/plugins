// PITCH and the voice stream: timing of the pitch the followers read, the tail of a run of marks,
// rough voice in Keep, consonant-safe splices, the onset detector, and the small fixes that ride
// along (silence after a start or reset, block-size independence, VOCODER's reset, IN's clamp,
// SHIMMER at every rate). Included by RoarschachTests.cpp inside its anonymous namespace, after
// its helpers. Every signal is synthetic.
//
// Groups: "silence", "blocks" (the extra chains, called from testBlockSizes), "reset", "in",
// "pitchstream", "pitchsplice", "shimmer".

namespace pitchbatch
{
    constexpr std::array<double, 6> c_allRates { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

    /** Renders in blocks taken in turn from `blocks` (each at most the prepared size). */
    inline Stereo renderBlocks(RoarschachAudioProcessor& p, const Stereo& in, const std::vector<int>& blocks)
    {
        const int numIns = p.getTotalNumInputChannels();
        const int numOuts = p.getTotalNumOutputChannels();
        const int numCh = std::max(numIns, numOuts);
        const int n = in.size();
        Stereo out;
        out.l.assign(static_cast<size_t>(n), 0.0f);
        out.r.assign(static_cast<size_t>(n), 0.0f);
        int largest = 1;
        for (int b : blocks)
            largest = std::max(largest, b);
        juce::AudioBuffer<float> buffer(numCh, largest);
        juce::MidiBuffer midi;
        size_t next = 0;
        for (int start = 0; start < n;)
        {
            const int len = std::min(blocks[next], n - start);
            next = (next + 1) % blocks.size();
            buffer.setSize(numCh, len, false, false, true);
            buffer.clear();
            for (int i = 0; i < len; ++i)
            {
                buffer.setSample(0, i, in.l[static_cast<size_t>(start + i)]);
                if (numIns > 1)
                    buffer.setSample(1, i, in.r[static_cast<size_t>(start + i)]);
            }
            p.processBlock(buffer, midi);
            for (int i = 0; i < len; ++i)
            {
                out.l[static_cast<size_t>(start + i)] = buffer.getSample(0, i);
                out.r[static_cast<size_t>(start + i)] = numOuts > 1 ? buffer.getSample(1, i) : buffer.getSample(0, i);
            }
            start += len;
        }
        return out;
    }

    inline double maxDifference(const Stereo& a, const Stereo& b, int from = 0)
    {
        double err = 0.0;
        for (int i = from; i < std::min(a.size(), b.size()); ++i)
            err = std::max({ err, std::abs(static_cast<double>(a.l[static_cast<size_t>(i)]) - b.l[static_cast<size_t>(i)]),
                             std::abs(static_cast<double>(a.r[static_cast<size_t>(i)]) - b.r[static_cast<size_t>(i)]) });
        return err;
    }

    // ------------------------------------------------------------------------------------ T10
    /** Digital silence through every factory preset, raw output from sample 0: exactly zero,
        also after a host reset in the middle. */
    inline void testSilence()
    {
        std::printf("[silence] digital silence in, every factory preset, raw output from sample 0 (and after a reset)\n");
        const auto& presets = PresetManager::getFactoryPresets();
        int bad = 0;
        for (const double sr : { 44100.0, 48000.0, 96000.0 })
        {
            const int half = static_cast<int>(sr);
            const Stereo silence { Signal(static_cast<size_t>(half), 0.0f), Signal(static_cast<size_t>(half), 0.0f) };
            for (size_t i = 0; i < presets.size(); ++i)
            {
                auto p = makeProcessor(sr, 512);
                p->setCurrentProgram(static_cast<int>(i));
                restart(*p, sr, 512);
                const auto a = render(*p, silence, 512);
                p->reset();
                const auto b = render(*p, silence, 512);
                double peak = 0.0;
                int first = -1;
                for (const auto* s : { &a, &b })
                    for (int k = 0; k < s->size(); ++k)
                    {
                        const double v = std::max(std::abs(static_cast<double>(s->l[static_cast<size_t>(k)])), std::abs(static_cast<double>(s->r[static_cast<size_t>(k)])));
                        if (v > 0.0 && first < 0)
                            first = k + (s == &b ? half : 0);
                        peak = std::max(peak, v);
                    }
                if (peak != 0.0)
                {
                    ++bad;
                    if (bad <= 12)
                        std::printf("  %-20s %.1f kHz: peak %.1f dBFS, first non-zero sample %d\n", presets[i].name, sr / 1000.0, toDb(peak), first);
                }
            }
        }
        std::printf("  %d preset x rate renders not silent\n", bad);
        check(bad == 0, juce::String(bad) + " preset renders give sound for digital silence in");
    }

    // ------------------------------------------------------------------------------------ T7
    struct BlockChain
    {
        const char* name;
        std::function<void(RoarschachAudioProcessor&)> setUp;
    };

    inline std::vector<BlockChain> blockChains()
    {
        std::vector<BlockChain> chains;
        chains.push_back({ "Init, PITCH Snap, VOCODER right behind it", [](RoarschachAudioProcessor& p)
        {
            setOrder(p, orderFromCodes("PI VO CL DS DT RM SH GR SW RO EN TA SU OV"));
            setValues(p, { { Params::pitchMode, 1.0f }, { Params::pitch, -5.0f }, { Params::vocoderOn, 1.0f }, { Params::gravel, 60.0f } });
        } });
        chains.push_back({ "Init, PITCH Mono, RING Follow right behind it", [](RoarschachAudioProcessor& p)
        {
            setOrder(p, orderFromCodes("PI RM CL DS DT VO SH GR SW RO EN TA SU OV"));
            setValues(p, { { Params::pitchMode, 2.0f }, { Params::ringOn, 1.0f }, { Params::ring, 70.0f }, { Params::ringTrack, 1.0f } });
        } });
        chains.push_back({ "Init, VOCODER first, PITCH second", [](RoarschachAudioProcessor& p)
        {
            setOrder(p, orderFromCodes("VO PI CL DS DT RM SH GR SW RO EN TA SU OV"));
            setValues(p, { { Params::vocoderOn, 1.0f }, { Params::gravel, 60.0f } });
        } });
        chains.push_back({ "Init, RING Follow first, PITCH second", [](RoarschachAudioProcessor& p)
        {
            setOrder(p, orderFromCodes("RM PI CL DS DT VO SH GR SW RO EN TA SU OV"));
            setValues(p, { { Params::ringOn, 1.0f }, { Params::ring, 70.0f }, { Params::ringTrack, 1.0f } });
        } });
        // The factory presets that put RING ahead of PITCH.
        const auto& presets = PresetManager::getFactoryPresets();
        for (size_t i = 0; i < presets.size(); ++i)
        {
            const auto& v = presets[i].values;
            const auto pos = [&](const char* id) { const auto it = v.find(id); return it == v.end() ? -1.0f : it->second; };
            Params::Order positions {};
            for (int m = 0; m < Params::numModules; ++m)
                positions[static_cast<size_t>(m)] = juce::roundToInt(pos(Params::modules[static_cast<size_t>(m)].posId));
            const auto order = Params::resolveOrder(positions);
            if (slotOf(order, Params::ModuleId::ring) < slotOf(order, Params::ModuleId::pitch))
            {
                const int index = static_cast<int>(i);
                chains.push_back({ presets[i].name, [index](RoarschachAudioProcessor& p) { p.setCurrentProgram(index); } });
            }
        }
        return chains;
    }

    /** The chains beyond Init: identical output at every host block size (the second part of "blocks"). */
    inline void testBlockChains()
    {
        std::printf("[blocks] PITCH with VOCODER or RING Follow, either order, and the presets with RING ahead of PITCH\n");
        const double sr = 48000.0;
        const auto in = dual(makeVoice(sr, 1.0));
        std::vector<int> randomBlocks;
        {
            std::mt19937 rng(17);
            std::uniform_int_distribution<int> pick(1, 700);
            for (int k = 0; k < 400; ++k)
                randomBlocks.push_back(pick(rng));
        }
        int chainsChecked = 0;
        for (const auto& chain : blockChains())
        {
            ++chainsChecked;
            const auto make = [&](int prepared)
            {
                auto p = makeProcessor(sr, prepared);
                chain.setUp(*p);
                restart(*p, sr, prepared);
                return p;
            };
            auto ref = make(512);
            const auto a = render(*ref, in, 512);
            std::printf("  %s:", chain.name);
            for (int block : { 1, 7, 16, 33, 64, 441, 4096, -1 })
            {
                auto p = make(block < 0 ? 700 : block);
                const auto b = block < 0 ? renderBlocks(*p, in, randomBlocks) : render(*p, in, block);
                const double err = maxDifference(a, b);
                std::printf(" %s %.0f dB;", block < 0 ? "random" : juce::String(block).toRawUTF8(), toDb(err));
                check(err < 1.0e-5, juce::String(chain.name) + ": block " + (block < 0 ? juce::String("random") : juce::String(block))
                                        + " changes the output by " + juce::String(toDb(err), 1) + " dBFS");
            }
            std::printf("\n");
        }
        check(chainsChecked >= 6, "the block test covers the four PITCH / follower orders and the presets with RING ahead of PITCH");
    }

    // ------------------------------------------------------------------------------------ T14
    /** A host reset gives exactly what a new instance gives on the same input. */
    inline void testResetEqualsFresh()
    {
        std::printf("[reset] after a host reset each module renders as a new instance does\n");
        const double sr = 48000.0;
        const auto voice = makeVoice(sr, 4.0);
        const int cut = static_cast<int>(2.0 * sr);
        const Stereo first { Signal(voice.begin(), voice.begin() + cut), Signal(voice.begin(), voice.begin() + cut) };
        const Stereo second { Signal(voice.begin() + cut, voice.end()), Signal(voice.begin() + cut, voice.end()) };

        struct Case { const char* name; Values values; const char* order; };
        const std::vector<Case> cases {
            { "PITCH Follow -5", { { Params::pitchOn, 1.0f }, { Params::pitch, -5.0f }, { Params::pitchDrive, 25.0f } }, nullptr },
            { "PITCH Keep -5, VOCODER behind it", { { Params::pitchOn, 1.0f }, { Params::pitch, -5.0f }, { Params::pitchFormant, 1.0f },
                                                    { Params::vocoderOn, 1.0f }, { Params::gravel, 60.0f } }, "PI VO" },
            { "RING Follow", { { Params::ringOn, 1.0f }, { Params::ring, 70.0f }, { Params::ringTrack, 1.0f } }, nullptr },
            { "SHIMMER Up+Down", { { Params::shimmerOn, 1.0f }, { Params::shimmer, 92.0f }, { Params::shimmerMode, 2.0f } }, nullptr },
            { "VOCODER Gravel 60 %", { { Params::vocoderOn, 1.0f }, { Params::gravel, 60.0f } }, nullptr },
        };
        for (const auto& c : cases)
        {
            const auto make = [&]
            {
                auto p = makeProcessor(sr, 512);
                p->getChainForTests().setOutputTestOptions(true, false, false);
                setAllModules(*p, false);
                setValues(*p, { { Params::inAuto, 0.0f } });
                setValues(*p, c.values);
                if (c.order != nullptr)
                    setOrder(*p, orderFromCodes(c.order));
                restart(*p, sr, 512);
                return p;
            };
            auto fresh = make();
            const auto a = render(*fresh, second, 512);
            auto used = make();
            (void) render(*used, first, 512);
            used->reset();
            const auto b = render(*used, second, 512);
            const double db = nullDb(a.l, b.l, 0, 0);
            std::printf("  %-34s reset against a new instance: %.0f dB\n", c.name, db);
            check(db < -100.0, juce::String(c.name) + ": a reset differs from a new instance by " + juce::String(db, 1) + " dB");
        }
    }
    // ------------------------------------------------------------------------------------ T11
    /** One absurd sample (1e30) cannot reach the modules: IN clamps to +-32. */
    inline void testInClamp()
    {
        std::printf("[in] one sample of 1e30: finite, under the ceiling, TAME back at rest within 2.5 s\n");
        const double sr = 48000.0;
        auto voice = makeVoice(sr, 2.0);
        const int at = static_cast<int>(1.0 * sr);
        {
            auto spiked = voice;
            spiked[static_cast<size_t>(at)] = 1.0e30f;
            auto p = makeProcessor(sr, 512);
            setAllModules(*p, false);
            setValues(*p, { { Params::tameOn, 1.0f }, { Params::tame, 60.0f } });
            restart(*p, sr, 512);
            const auto out = render(*p, dual(spiked), 512);
            const auto st = stats(out);
            std::printf("  whole plug-in, TAME alone: peak %.2f dBFS, finite %d\n", toDb(st.peak), st.finite ? 1 : 0);
            check(st.finite && st.peak <= ceilingGain(-1.0f) * 1.0001f, "a 1e30 sample gives a non-finite or over-ceiling output");
        }
        // IN then TAME, as in the chain: how long TAME's reduction stays more than 1 dB away from a
        // run without the sample.
        auto longVoice = makeVoice(sr, 8.0);
        const auto grTrace = [&](bool spike)
        {
            roar::InModule inModule;
            roar::TameModule tame;
            inModule.prepare(sr, 64);
            tame.prepare(sr, 64);
            roar::InSettings is;
            is.autoLevel = false;
            inModule.setParams(is);
            roar::TameSettings ts;
            ts.amount = 0.6f;
            tame.setParams(ts);
            inModule.reset();
            tame.reset();
            auto x = longVoice;
            if (spike)
                x[static_cast<size_t>(at)] = 1.0e30f;
            Signal r = x;
            std::vector<float> gr;
            for (int start = 0; start < static_cast<int>(x.size()); start += 64)
            {
                const int n = std::min(64, static_cast<int>(x.size()) - start);
                inModule.process(x.data() + start, r.data() + start, n, false, roar::VoiceFrameView {});
                tame.process(x.data() + start, r.data() + start, n, false, inModule.getVoiceView());
                gr.push_back(tame.getGrDb());
            }
            return gr;
        };
        const auto rest = grTrace(false), spiked = grTrace(true);
        int lastOff = -1;
        for (size_t k = 0; k < rest.size(); ++k)
            if (std::abs(spiked[k] - rest[k]) > 1.0f)
                lastOff = static_cast<int>(k);
        const double recoverMs = lastOff < 0 ? 0.0 : 1000.0 * ((lastOff + 1) * 64 - at) / sr;
        const size_t at100 = static_cast<size_t>((at + static_cast<int>(0.1 * sr)) / 64);
        std::printf("  IN then TAME: reduction 100 ms after the sample %.2f dB (without it %.2f dB); within 1 dB of rest for good %.0f ms after it\n",
                    static_cast<double>(spiked[at100]), static_cast<double>(rest[at100]), recoverMs);
        // A +30 dBFS sample is still a large event for TAME: with the clamp it lets go in about 2 s
        // (5.6 s without it). The clamp's job is to keep 1e30 out, not to make +30 dBFS harmless.
        check(recoverMs <= 2500.0, "TAME stays more than 1 dB from rest for " + juce::String(recoverMs, 0) + " ms after a 1e30 sample (<= 2500)");
    }
    // ------------------------------------------------------------------------------------ helpers
    /** A vowel on f0 (harmonics at 1 / k through three formant resonances), peak `peak`, from
        `phase` (radians of the fundamental) on; f0 may move: f0At (seconds) gives it. */
    template <typename F0At>
    Signal vowelOn(double fs, double seconds, double peak, F0At&& f0At)
    {
        const int n = static_cast<int>(fs * seconds);
        Signal s(static_cast<size_t>(n), 0.0f);
        const double formants[3][3] { { 700.0, 110.0, 1.0 }, { 1220.0, 120.0, 0.5 }, { 2600.0, 160.0, 0.25 } };
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double f0 = f0At(i / fs);
            phase += 2.0 * c_pi * f0 / fs;
            double v = 0.0;
            for (int k = 1; k <= 40 && k * f0 < std::min(0.4 * fs, 9000.0); ++k)
            {
                const std::complex<double> z = std::polar(1.0, -2.0 * c_pi * k * f0 / fs);
                std::complex<double> h = 0.0;
                for (const auto& f : formants)
                {
                    const double rad = std::exp(-c_pi * f[1] / fs);
                    h += f[2] * (1.0 - rad) / (1.0 - 2.0 * rad * std::cos(2.0 * c_pi * f[0] / fs) * z + rad * rad * z * z);
                }
                v += std::abs(h) / k * std::cos(k * phase + std::arg(h));
            }
            s[static_cast<size_t>(i)] = static_cast<float>(v);
        }
        normalisePeak(s, static_cast<float>(peak));
        return s;
    }

    /** IN (Auto Level off) then PITCH and its VoicePass, run in 32-sample blocks on the frame grid:
        the stream behind PITCH, one entry per global frame. */
    struct StreamProbe
    {
        std::vector<roar::VoiceFrame> frames;   // index = global frame
        std::vector<int> skew;                  // skew of the view behind PITCH
        std::vector<float> ratioAtStart;        // PITCH's ratio before the frame was processed
        int latency = 0;
    };

    inline StreamProbe probeStream(double fs, const Signal& in, roar::PitchSettings ps,
                                   const std::function<void(int, roar::PitchSettings&)>& atFrame = {})
    {
        roar::InModule inModule;
        roar::PitchModule pitch;
        roar::VoicePass pass;
        constexpr int block = 32;
        inModule.prepare(fs, block);
        roar::InSettings is;
        is.autoLevel = false;
        inModule.setParams(is);
        inModule.reset();
        pitch.setParams(ps);
        pitch.prepare(fs, block);
        pitch.setParams(ps);
        pitch.reset();
        pass.prepare(pitch.getLatencySamples(), block);
        pass.reset();
        StreamProbe probe;
        probe.latency = pitch.getLatencySamples();
        Signal x = in, r(static_cast<size_t>(block), 0.0f);
        const int frames = static_cast<int>(x.size()) / block;
        probe.frames.resize(static_cast<size_t>(frames));
        probe.skew.resize(static_cast<size_t>(frames));
        probe.ratioAtStart.resize(static_cast<size_t>(frames));
        for (int g = 0; g < frames; ++g)
        {
            if (atFrame)
            {
                atFrame(g, ps);
                pitch.setParams(ps);
            }
            probe.ratioAtStart[static_cast<size_t>(g)] = pitch.getPitchRatio();
            float* d = x.data() + g * block;
            inModule.process(d, r.data(), block, false, roar::VoiceFrameView {});
            const auto view = inModule.getVoiceView();
            pitch.process(d, r.data(), block, false, view);
            const auto behind = pass.process(view, &pitch);
            probe.frames[static_cast<size_t>(g)] = behind.frame(0);
            probe.skew[static_cast<size_t>(g)] = behind.skewSamples();
        }
        return probe;
    }

    // ------------------------------------------------------------------------------- T1, T2
    inline void testPitchStream()
    {
        std::printf("[pitchstream] the pitch behind PITCH against its audio: phrase starts and stops, the rewrite's pairing, Snap\n");

        // T1: twelve vowel phrases with hard starts and stops through IN and PITCH (-5 st).
        {
            const std::array<double, 12> pitches { 98, 110, 147, 165, 196, 220, 262, 330, 500, 700, 900, 1000 };
            const double ratio = std::exp2(-5.0 / 12.0);
            for (const double fs : c_rates)
            {
                const int phrase = static_cast<int>(0.5 * fs), gap = static_cast<int>(0.3 * fs);
                Signal in(static_cast<size_t>(gap + 12 * (phrase + gap)), 0.0f);
                for (size_t p = 0; p < pitches.size(); ++p)
                {
                    const auto v = vowelOn(fs, 0.5, 0.25, [&](double) { return pitches[p]; });
                    std::copy(v.begin(), v.end(), in.begin() + gap + static_cast<long>(p) * (phrase + gap));
                }
                roar::PitchSettings ps;
                ps.pitchSemitones = -5.0f;
                ps.drive = 0.0f;
                const auto probe = probeStream(fs, in, ps);
                double worstOn = -1e9, earliestOn = 1e9, worstOff = -1e9;
                int missing = 0;
                for (size_t p = 0; p < pitches.size(); ++p)
                {
                    const int onset = gap + static_cast<int>(p) * (phrase + gap) + probe.latency;   // at PITCH's output
                    const int stop = onset + phrase;
                    const double want = pitches[p] * ratio;
                    int on = -1, off = -1;
                    for (int g = std::max(0, (onset - static_cast<int>(0.05 * fs)) / 32); g < static_cast<int>(probe.frames.size()) && g * 32 < stop; ++g)
                    {
                        const auto& f = probe.frames[static_cast<size_t>(g)];
                        if (f.voiced && std::abs(f.f0Hz / want - 1.0) <= 0.03)
                        {
                            on = g * 32;
                            break;
                        }
                    }
                    for (int g = stop / 32; g < static_cast<int>(probe.frames.size()); ++g)
                        if (! probe.frames[static_cast<size_t>(g)].voiced)
                        {
                            off = g * 32;
                            break;
                        }
                    if (on < 0 || off < 0)
                    {
                        ++missing;
                        continue;
                    }
                    const double onMs = 1000.0 * (on - onset) / fs, offMs = 1000.0 * (off - stop) / fs;
                    worstOn = std::max(worstOn, onMs);
                    earliestOn = std::min(earliestOn, onMs);
                    worstOff = std::max(worstOff, offMs);
                }
                std::printf("  %.1f kHz: pitch on %+.1f to %+.1f ms after the phrase's audio behind PITCH, off at most %+.1f ms after the stop\n",
                            fs / 1000.0, earliestOn, worstOn, worstOff);
                check(missing == 0, juce::String(missing) + " phrases never show their shifted pitch behind PITCH at " + juce::String(fs / 1000.0, 1) + " kHz");
                // The bounds are set at 48 kHz. The detector itself lets go about 4 ms later at the
                // other rates (50 ms at 48 kHz, 54 to 55 ms at 44.1, 96 and 192 kHz without the
                // stream's lateness), so they get that much more.
                const bool ref = fs == 48000.0;
                const double onMax = ref ? 10.0 : 11.0, offMax = ref ? 21.0 : 25.0;
                check(worstOn <= onMax, "the pitch behind PITCH comes " + juce::String(worstOn, 1) + " ms after the phrase's audio (<= " + juce::String(onMax, 0) + ") at " + juce::String(fs / 1000.0, 1) + " kHz");
                check(earliestOn >= -6.0, "the pitch behind PITCH comes " + juce::String(-earliestOn, 1) + " ms before the phrase's audio (<= 6) at " + juce::String(fs / 1000.0, 1) + " kHz");
                check(worstOff <= offMax, "the voiced flag behind PITCH lets go " + juce::String(worstOff, 1) + " ms after the stop (<= " + juce::String(offMax, 0) + ") at " + juce::String(fs / 1000.0, 1) + " kHz");
            }
        }

        // T2a: Snap on a vowel gliding up 2 st per 100 ms: the stream behind PITCH is the snapped note.
        {
            const double fs = 48000.0;
            const double startHz = 150.0, stPerSecond = 20.0;
            const auto f0At = [&](double t) { return startHz * std::exp2(stPerSecond * std::clamp(t - 0.2, 0.0, 1.2) / 12.0); };
            const auto in = vowelOn(fs, 1.8, 0.25, f0At);
            roar::PitchSettings ps;
            ps.pitchSemitones = 0.0f;
            ps.drive = 0.0f;
            ps.mode = Params::PitchMode::snap;
            const auto probe = probeStream(fs, in, ps);
            int voiced = 0, wrong = 0;
            double worstCents = 0.0;
            for (int g = static_cast<int>(0.3 * fs) / 32; g < static_cast<int>(probe.frames.size()); ++g)
            {
                const auto& f = probe.frames[static_cast<size_t>(g)];
                const double inputTime = (g * 32.0 - probe.skew[static_cast<size_t>(g)] - probe.latency) / fs;   // the audio this frame describes, at the input
                if (! f.voiced || inputTime < 0.22 || inputTime > 1.4)
                    continue;
                ++voiced;
                const double mIn = 69.0 + 12.0 * std::log2(f0At(inputTime) / 440.0);
                const double m = 69.0 + 12.0 * std::log2(f.f0Hz / 440.0);
                // The snapped note, or its next one while the input is within the hysteresis of the boundary.
                const double near = std::round(mIn);
                double cents = 100.0 * std::abs(m - near);
                if (std::abs(mIn - near) > 0.5 - 0.3 - 0.05)
                    cents = std::min(cents, 100.0 * std::abs(m - (near + (mIn > near ? 1.0 : -1.0))));
                worstCents = std::max(worstCents, cents);
                wrong += cents > 20.0 ? 1 : 0;
            }
            std::printf("  Snap on a 2 st / 100 ms glide: %d of %d voiced frames behind PITCH more than 20 cents off the snapped note (worst %.0f cents)\n", wrong, voiced, worstCents);
            check(voiced > 1000 && wrong == 0, juce::String(wrong) + " frames behind PITCH Snap are off the snapped note of the audio they describe");
        }

        // T2b: the pairing of ratio and frame. A steady 200 Hz vowel while Pitch steps from -12 to +3 st
        // (not to 0: at 0 st the shifter fades to its dry input and the stream carries the faded
        // ratio, which getPitchRatio() does not show).
        {
            const double fs = 48000.0;
            const auto in = vowelOn(fs, 2.6, 0.25, [](double) { return 200.0; });
            roar::PitchDetector detector;
            detector.prepare(fs);
            const int lag = std::max(0, (detector.getReactionSamples() - roar::PitchModule::latencyFor(fs) + 16) >> 5);
            roar::PitchSettings ps;
            ps.pitchSemitones = -12.0f;
            ps.drive = 0.0f;
            const int stepAt = static_cast<int>(1.2 * fs) / 32;
            const auto probe = probeStream(fs, in, ps, [&](int g, roar::PitchSettings& s) { s.pitchSemitones = g < stepAt ? -12.0f : 3.0f; });
            double moved = 0.0;
            for (int g = 15; g < static_cast<int>(probe.ratioAtStart.size()); ++g)
                moved = std::max(moved, std::abs(12.0 * std::log2(probe.ratioAtStart[static_cast<size_t>(g)] / probe.ratioAtStart[static_cast<size_t>(g - 15)])));
            check(moved >= 3.0, "T2b cannot fail: the ratio moves only " + juce::String(moved, 2) + " st within 15 frames");
            int frames = 0, off = 0;
            double worst = 0.0;
            for (int g = static_cast<int>(0.5 * fs) / 32; g < static_cast<int>(2.5 * fs) / 32; ++g)
            {
                const auto& f = probe.frames[static_cast<size_t>(g)];
                if (! f.voiced || g - lag < 0)
                    continue;
                ++frames;
                const double want = 200.0 * probe.ratioAtStart[static_cast<size_t>(g - lag)];
                const double cents = std::abs(1200.0 * std::log2(f.f0Hz / want));
                worst = std::max(worst, cents);
                off += cents > 10.0 ? 1 : 0;
            }
            std::printf("  steady 200 Hz, Pitch -12 -> +3 st (lag %d frames): %d of %d voiced frames more than 10 cents from 200 Hz x the ratio %d frames earlier (worst %.0f cents)\n",
                        lag, off, frames, lag, worst);
            check(frames > 2000 && off == 0, juce::String(off) + " frames behind PITCH carry a ratio other than the one applied to their audio");
        }
    }
}

