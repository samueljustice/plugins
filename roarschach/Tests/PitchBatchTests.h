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
    // ------------------------------------------------------------------------------------ PITCH alone
    /** A pulse vowel: impulses one period apart (period jittered by `jitter`, seeded) through three
        formant resonances and a gentle pre-emphasis, peak `amp`. */
    inline Signal pulseVowel(double fs, double f0, double seconds, double jitter = 0.0, unsigned seed = 0, double amp = 0.25)
    {
        const int n = static_cast<int>(seconds * fs);
        std::vector<double> x(static_cast<size_t>(n) + 2, 0.0);
        std::mt19937 rng(seed);
        std::normal_distribution<double> gauss(0.0, 1.0);
        for (double t = 0.0; t < n - 1;)
        {
            const auto i = static_cast<size_t>(t);
            const double fr = t - static_cast<double>(i);
            x[i] += 1.0 - fr;
            x[i + 1] += fr;
            t += std::max(8.0, fs / f0 * (1.0 + jitter * gauss(rng)));
        }
        std::vector<double> y(static_cast<size_t>(n), 0.0);
        const double formants[3][2] { { 700.0, 90.0 }, { 1200.0, 110.0 }, { 2600.0, 160.0 } };
        for (const auto& f : formants)
        {
            const double r = std::exp(-c_pi * f[1] / fs), a1 = -2.0 * r * std::cos(2.0 * c_pi * f[0] / fs), a2 = r * r;
            double y1 = 0.0, y2 = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double v = (1.0 - r) * x[static_cast<size_t>(i)] - a1 * y1 - a2 * y2;
                y2 = y1;
                y1 = v;
                y[static_cast<size_t>(i)] += v;
            }
        }
        Signal s(static_cast<size_t>(n));
        double px = 0.0, py = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double v = y[static_cast<size_t>(i)] - px + 0.995 * py;   // (1 - z^-1) / (1 - 0.995 z^-1)
            px = y[static_cast<size_t>(i)];
            py = v;
            s[static_cast<size_t>(i)] = static_cast<float>(v);
        }
        normalisePeak(s, static_cast<float>(amp));
        return s;
    }

    /** 22 noise bursts (12 ms, the last 3 ms a linear decay, 150..400 ms apart from 0.3 s, seeded):
        the signal and the burst starts. */
    inline std::pair<Signal, std::vector<int>> noiseBursts(double fs, double amp = 0.3, unsigned seed = 11)
    {
        Signal x(static_cast<size_t>(8.5 * fs), 0.0f);
        std::mt19937 rng(seed);
        std::normal_distribution<double> gauss(0.0, 1.0);
        std::uniform_real_distribution<double> gap(0.15, 0.4);
        std::vector<int> starts;
        int t = static_cast<int>(0.3 * fs);
        const int len = static_cast<int>(0.012 * fs), decay = static_cast<int>(0.003 * fs);
        for (int k = 0; k < 22; ++k)
        {
            for (int i = 0; i < len; ++i)
            {
                const double env = i < len - decay ? 1.0 : static_cast<double>(len - i) / decay;
                x[static_cast<size_t>(t + i)] += static_cast<float>(amp * gauss(rng) * env);
            }
            starts.push_back(t);
            t += static_cast<int>(gap(rng) * fs);
        }
        return { x, starts };
    }

    struct PitchRun
    {
        Signal out;                                          // latency removed
        std::vector<std::pair<std::int64_t, std::int64_t>> onsets;   // every onset found (input positions)
        std::vector<bool> voiced;                            // isVoiced() after every 32-sample block
    };

    /** PITCH alone (mono), in 32-sample blocks, its latency removed. */
    inline PitchRun runPitch(double fs, const Signal& in, roar::PitchSettings ps)
    {
        roar::PitchModule m;
        m.setParams(ps);
        m.prepare(fs, 32);
        m.setParams(ps);
        m.reset();
        const int latency = m.getLatencySamples();
        Signal x = in;
        x.resize(in.size() + static_cast<size_t>(latency), 0.0f);
        Signal r(32, 0.0f);
        PitchRun run;
        std::map<std::int64_t, std::int64_t> found;
        for (int start = 0; start < static_cast<int>(x.size()); start += 32)
        {
            const int n = std::min(32, static_cast<int>(x.size()) - start);
            m.process(x.data() + start, r.data(), n, false, roar::VoiceFrameView {});
            run.voiced.push_back(m.isVoiced());
            for (int i = 0; i < m.getNumOnsetsKept(); ++i)
            {
                const auto o = m.getOnset(i);
                found[o.first] = o.second;
            }
        }
        run.out.assign(x.begin() + latency, x.end());
        for (const auto& o : found)
            run.onsets.push_back(o);
        return run;
    }

    inline roar::PitchSettings pitchSettings(float semitones, Params::PitchFormant formant, float size = 0.0f)
    {
        roar::PitchSettings ps;
        ps.pitchSemitones = semitones;
        ps.formant = formant;
        ps.size = size;
        ps.drive = 0.0f;
        return ps;
    }

    inline double meanSquare(const Signal& x, int from, int to)
    {
        double s = 0.0;
        from = std::max(0, from);
        to = std::min(to, static_cast<int>(x.size()));
        for (int i = from; i < to; ++i)
            s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
        return to > from ? s / (to - from) : 0.0;
    }

    // ------------------------------------------------------------------------------------ T3, T17
    inline void testPitchTail()
    {
        std::printf("[pitch] the tail of a run of marks: no repeated last cycle after a dead stop; grains kept inside a rough voice\n");
        const double fs = 48000.0;
        constexpr auto keep = Params::PitchFormant::keep;
        constexpr auto follow = Params::PitchFormant::follow;

        // T3 (a): 10 to 20 ms after a dead stop, at least 40 dB under the voice.
        for (const double f0 : { 70.0, 90.0, 140.0, 220.0 })
        {
            auto v = pulseVowel(fs, f0, 1.4);
            const int stop = static_cast<int>(1.0 * fs);
            std::fill(v.begin() + stop, v.end(), 0.0f);
            const double voice = meanSquare(v, stop - static_cast<int>(0.2 * fs), stop);
            std::printf("  %3.0f Hz, stopped dead, 10..20 ms after the stop:", f0);
            for (const auto& [name, st, formant] : { std::tuple { "Keep -5", -5.0f, keep }, std::tuple { "Keep -12", -12.0f, keep }, std::tuple { "Follow -5", -5.0f, follow } })
            {
                const auto run = runPitch(fs, v, pitchSettings(st, formant));
                const double after = meanSquare(run.out, stop + static_cast<int>(0.010 * fs), stop + static_cast<int>(0.020 * fs));
                const double db = 10.0 * std::log10(std::max(after, 1e-30) / voice);
                std::printf(" %s %.1f dB;", name, db);
                check(db <= -40.0, juce::String(name) + " at " + juce::String(f0, 0) + " Hz: " + juce::String(db, 1) + " dB 10..20 ms after a dead stop (<= -40)");
            }
            // T3 (b), a ceiling the branch passes too: Follow -12 and -24, energy after the stop
            // at most 1.25 x (0.75 + 1 / phi) periods' worth of the voice.
            for (const float st : { -12.0f, -24.0f })
            {
                const auto run = runPitch(fs, v, pitchSettings(st, follow));
                const double phi = std::exp2(st / 12.0), T = fs / f0;
                const double periods = meanSquare(run.out, stop, stop + static_cast<int>(0.2 * fs)) * 0.2 * fs / (voice * T);
                const double ceiling = 1.25 * (0.75 + 1.0 / phi);
                std::printf(" Follow %.0f %.2f T (ceiling %.2f);", static_cast<double>(st), periods, ceiling);
                check(periods <= ceiling, "Follow " + juce::String(st, 0) + " at " + juce::String(f0, 0) + " Hz: " + juce::String(periods, 2)
                                              + " periods' worth after a dead stop (ceiling " + juce::String(ceiling, 2) + ", a guard the branch passes too)");
            }
            std::printf("\n");
        }

        // T17: grain time on jittered low voices, against the figures this same body gave on the
        // shifter as it was before the tail rule (built from the tree before this batch): at least
        // that minus 6 points (minus 10 at Keep -12).
        {
            struct Voice { double f0, jitter; unsigned seed; };
            const std::array<Voice, 6> voices { { { 55, 0.04, 1 }, { 60, 0.08, 2 }, { 70, 0.04, 3 }, { 70, 0.10, 4 }, { 90, 0.08, 5 }, { 120, 0.10, 6 } } };
            struct Setting { const char* name; float st; Params::PitchFormant formant; double margin; };
            const std::array<Setting, 4> settings { { { "Keep -5", -5.0f, keep, 6.0 }, { "Follow -5", -5.0f, follow, 6.0 },
                                                      { "Follow +7", 7.0f, follow, 6.0 }, { "Keep -12", -12.0f, keep, 10.0 } } };
            // [voice][setting], per cent of 32-sample frames with isVoiced(), 0.5..2.8 s.
            static constexpr double c_branch[6][4] { { 87.5, 47.3, 72.7, 91.7 }, { 83.7, 31.6, 73.7, 85.3 }, { 96.1, 83.8, 90.2, 95.7 },
                                                     { 90.3, 37.8, 74.9, 90.3 }, { 58.9, 44.9, 53.6, 58.3 }, { 42.2, 23.8, 37.9, 41.1 } };
            for (size_t vi = 0; vi < voices.size(); ++vi)
            {
                const auto& vc = voices[vi];
                const auto v = pulseVowel(fs, vc.f0, 3.0, vc.jitter, vc.seed);
                std::printf("  %3.0f Hz, jitter %2.0f %%, grain time (branch):", vc.f0, 100.0 * vc.jitter);
                for (size_t si = 0; si < settings.size(); ++si)
                {
                    const auto& st = settings[si];
                    const auto run = runPitch(fs, v, pitchSettings(st.st, st.formant));
                    int on = 0, all = 0;
                    const int latencyFrames = roar::PitchModule::latencyFor(fs) / 32;
                    for (int k = static_cast<int>(0.5 * fs) / 32; k < static_cast<int>(2.8 * fs) / 32; ++k)
                    {
                        const auto at = static_cast<size_t>(k + latencyFrames);
                        if (at < run.voiced.size())
                        {
                            on += run.voiced[at] ? 1 : 0;
                            ++all;
                        }
                    }
                    const double share = 100.0 * on / std::max(1, all);
                    std::printf(" %s %.1f (%.1f);", st.name, share, c_branch[vi][si]);
                    check(share >= c_branch[vi][si] - st.margin, juce::String(st.name) + " on " + juce::String(vc.f0, 0) + " Hz jitter " + juce::String(100.0 * vc.jitter, 0)
                                                                 + " %: grain time " + juce::String(share, 1) + " % against the branch's " + juce::String(c_branch[vi][si], 1));
                }
                std::printf("\n");
            }
        }
    }

    // ------------------------------------------------------------------------------------ T8
    inline void testOnsetDetector()
    {
        std::printf("[pitchsplice] onset detector at six rates: pulse trains, noise, bursts, the tilt\n");
        const auto onsetsOf = [](double fs, const Signal& x) { return runPitch(fs, x, pitchSettings(-5.0f, Params::PitchFormant::follow)).onsets; };
        int count48d = -1;
        for (const double fs : { 48000.0, 44100.0, 88200.0, 96000.0, 176400.0, 192000.0 })
        {
            // (a) jittered pulse trains, 2 s after 0.1 s of silence.
            juce::String trains;
            for (const double f : { 20.0, 25.0, 30.0, 36.0, 40.0, 44.0, 48.0, 52.0, 55.0, 60.0, 70.0, 100.0, 200.0 })
            {
                Signal x(static_cast<size_t>(0.1 * fs), 0.0f);
                const auto v = pulseVowel(fs, f, 2.0, 0.06, 3);
                x.insert(x.end(), v.begin(), v.end());
                const int n = static_cast<int>(onsetsOf(fs, x).size());
                const int most = f < 58.0 ? 6 : 2;
                trains << juce::String(f, 0) << ":" << n << " ";
                check(n <= most, juce::String(fs / 1000.0, 1) + " kHz: a " + juce::String(f, 0) + " Hz pulse train gives " + juce::String(n) + " onsets (<= " + juce::String(most) + ")");
            }
            // (b) white noise at -20 dBFS rms: nothing after the first 10 ms.
            int noiseOnsets = 0;
            {
                std::mt19937 rng(1);
                std::normal_distribution<float> gauss(0.0f, 0.1f);
                Signal x(static_cast<size_t>(5.0 * fs));
                for (auto& v : x)
                    v = gauss(rng);
                for (const auto& o : onsetsOf(fs, x))
                    noiseOnsets += o.first > static_cast<std::int64_t>(0.010 * fs) ? 1 : 0;
                check(noiseOnsets == 0, juce::String(fs / 1000.0, 1) + " kHz: " + juce::String(noiseOnsets) + " onsets in steady white noise");
            }
            // (c) the 22 bursts at -10 and -60 dBFS: each found within 3 ms, ending 4 to 16 ms later.
            juce::String burstText;
            for (const double gain : { 1.0, std::pow(10.0, -50.0 / 20.0) })
            {
                auto [x, starts] = noiseBursts(fs, 0.3 * gain);
                const auto found = onsetsOf(fs, x);
                int hit = 0, goodEnd = 0;
                for (const int t : starts)
                    for (const auto& o : found)
                        if (std::abs(static_cast<double>(o.first - t)) <= 0.003 * fs)
                        {
                            ++hit;
                            const double len = (o.second - o.first) / fs;
                            goodEnd += o.second >= 0 && len >= 0.004 - 1.0e-9 && len <= 0.016 + 0.002 ? 1 : 0;
                            break;
                        }
                burstText << (gain == 1.0 ? "-10 dBFS " : "-60 dBFS ") << static_cast<int>(found.size()) << " found, " << hit << " within 3 ms; ";
                check(static_cast<int>(found.size()) == 22 && hit == 22 && goodEnd == 22, juce::String(fs / 1000.0, 1) + " kHz bursts: " + juce::String(static_cast<int>(found.size()))
                                                                                       + " onsets, " + juce::String(hit) + " within 3 ms, " + juce::String(goodEnd) + " ending 4..16 ms later");
            }
            // (d) the tilt: band-passed bursts (3..12 kHz) on a steady 120 Hz vowel.
            int foundD = 0;
            {
                auto x = pulseVowel(fs, 120.0, 8.5, 0.0, 0, 0.2);
                auto [noise, starts] = noiseBursts(fs, 1.0, 21);
                roar::Biquad hp1, hp2, lp1, lp2;
                hp1.setCoeffs(roar::Biquad::highpass(fs, 3000.0, roar::Biquad::c_bw4Q1));
                hp2.setCoeffs(roar::Biquad::highpass(fs, 3000.0, roar::Biquad::c_bw4Q2));
                lp1.setCoeffs(roar::Biquad::lowpass(fs, 12000.0, roar::Biquad::c_bw4Q1));
                lp2.setCoeffs(roar::Biquad::lowpass(fs, 12000.0, roar::Biquad::c_bw4Q2));
                Signal band(noise.size());
                for (size_t i = 0; i < noise.size(); ++i)
                    band[i] = static_cast<float>(lp2.process(lp1.process(hp2.process(hp1.process(static_cast<double>(noise[i]))))));
                const int first3 = static_cast<int>(0.003 * fs);
                for (const int t : starts)
                {
                    const double rms = std::sqrt(meanSquare(band, t, t + first3));
                    const double g = rms > 0.0 ? 0.063 / rms : 0.0;
                    for (int i = t; i < t + static_cast<int>(0.016 * fs) && i < static_cast<int>(x.size()); ++i)
                        x[static_cast<size_t>(i)] += static_cast<float>(g * band[static_cast<size_t>(i)]);
                }
                const auto found = onsetsOf(fs, x);
                for (const int t : starts)
                    for (const auto& o : found)
                        if (std::abs(static_cast<double>(o.first - t)) <= 0.004 * fs)
                        {
                            ++foundD;
                            break;
                        }
                if (fs == 48000.0)
                    count48d = foundD;
            }
            std::printf("  %5.1f kHz: trains %s| noise %d | bursts %s| tilt %d of 22\n", fs / 1000.0, trains.toRawUTF8(), noiseOnsets, burstText.toRawUTF8(), foundD);
            if (count48d >= 0)
                check(std::abs(foundD - count48d) <= 5, juce::String(fs / 1000.0, 1) + " kHz finds " + juce::String(foundD) + " band-limited bursts on a vowel against "
                                                            + juce::String(count48d) + " at 48 kHz (within 5)");
        }
    }
    // ------------------------------------------------------------------------------------ measures
    /** Clicks per second (spec 8.3): samples whose squared second difference is 20 dB above the
        median of it over the 10 ms around them and above -66 dBFS, clustered within 2 ms. */
    inline double clickRate(const Signal& x, double fs, int from = 0, int to = -1)
    {
        to = to < 0 ? static_cast<int>(x.size()) : std::min(to, static_cast<int>(x.size()));
        const int n = to - from - 2;
        if (n <= 8)
            return 0.0;
        std::vector<double> p(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            const double d = static_cast<double>(x[static_cast<size_t>(from + i + 2)]) - 2.0 * x[static_cast<size_t>(from + i + 1)] + x[static_cast<size_t>(from + i)];
            p[static_cast<size_t>(i)] = d * d;
        }
        // Median over 10 ms on a 4x decimated copy (the max of each 4).
        const int m = n / 4;
        std::vector<double> q(static_cast<size_t>(m));
        for (int k = 0; k < m; ++k)
            q[static_cast<size_t>(k)] = std::max({ p[static_cast<size_t>(4 * k)], p[static_cast<size_t>(4 * k + 1)], p[static_cast<size_t>(4 * k + 2)], p[static_cast<size_t>(4 * k + 3)] });
        const int half = (static_cast<int>(0.010 * fs) / 4) / 2;
        std::vector<double> win;
        int clicks = 0, last = -1000000;
        const double ratio = std::pow(10.0, 2.0), floor = std::pow(10.0, -6.6);
        for (int k = 0; k < m; ++k)
        {
            win.assign(q.begin() + std::max(0, k - half), q.begin() + std::min(m, k + half + 1));
            std::nth_element(win.begin(), win.begin() + static_cast<long>(win.size() / 2), win.end());
            const double med = win[win.size() / 2];
            for (int j = 4 * k; j < 4 * k + 4; ++j)
            {
                const double v = p[static_cast<size_t>(j)];
                if (v > med * ratio && v > floor)
                {
                    if (j - last > static_cast<int>(0.002 * fs))
                        ++clicks;
                    last = j;
                }
            }
        }
        return clicks / (n / fs);
    }

    /** Log-frequency centroid (semitones re 1 Hz) of the averaged spectrum of x[from, to), 80 Hz..6 kHz. */
    inline double logCentroidSt(const Signal& x, double fs, int from, int to)
    {
        constexpr int order = 13;
        constexpr int size = 1 << order;
        std::vector<double> sum(size / 2 + 1, 0.0);
        for (int start = from; start + size <= to; start += size / 2)
        {
            const auto pw = spectrum(x, start, order);
            for (size_t k = 0; k < sum.size(); ++k)
                sum[k] += pw[k];
        }
        double w = 0.0, acc = 0.0;
        for (size_t k = 1; k < sum.size(); ++k)
        {
            const double f = static_cast<double>(k) * fs / size;
            if (f < 80.0 || f > 6000.0)
                continue;
            w += sum[k];
            acc += sum[k] * 12.0 * std::log2(f);
        }
        return w > 0.0 ? acc / w : 0.0;
    }

    inline Signal filtered(const Signal& x, double fs, double hpHz, double lpHz)
    {
        roar::Biquad h1, h2, l1, l2;
        if (hpHz > 0.0)
        {
            h1.setCoeffs(roar::Biquad::highpass(fs, hpHz, roar::Biquad::c_bw4Q1));
            h2.setCoeffs(roar::Biquad::highpass(fs, hpHz, roar::Biquad::c_bw4Q2));
        }
        if (lpHz > 0.0)
        {
            l1.setCoeffs(roar::Biquad::lowpass(fs, lpHz, roar::Biquad::c_bw4Q1));
            l2.setCoeffs(roar::Biquad::lowpass(fs, lpHz, roar::Biquad::c_bw4Q2));
        }
        Signal y(x.size());
        for (size_t i = 0; i < x.size(); ++i)
        {
            double v = static_cast<double>(x[i]);
            if (hpHz > 0.0)
                v = h2.process(h1.process(v));
            if (lpHz > 0.0)
                v = l2.process(l1.process(v));
            y[i] = static_cast<float>(v);
        }
        return y;
    }

    inline void scaleToRmsDb(Signal& x, double db)
    {
        const double rms = std::sqrt(meanSquare(x, 0, static_cast<int>(x.size())));
        const auto g = static_cast<float>(std::pow(10.0, db / 20.0) / std::max(rms, 1e-30));
        for (auto& v : x)
            v *= g;
    }

    /** PITCH alone with the air decision forced (-1 as measured), 512-sample blocks, latency removed. */
    inline Signal runPitchAir(double fs, const Signal& in, roar::PitchSettings ps, int airMode)
    {
        roar::PitchModule m;
        m.setParams(ps);
        m.prepare(fs, 512);
        m.setParams(ps);
        m.reset();
        m.setAirForTests(airMode);
        const int latency = m.getLatencySamples();
        Signal x = in;
        x.resize(in.size() + static_cast<size_t>(latency), 0.0f);
        Signal r(512, 0.0f);
        for (int start = 0; start < static_cast<int>(x.size()); start += 512)
            m.process(x.data() + start, r.data(), std::min(512, static_cast<int>(x.size()) - start), false, roar::VoiceFrameView {});
        return Signal(x.begin() + latency, x.end());
    }

    // ------------------------------------------------------------------------------------ T5
    inline void testKeepHandOver()
    {
        std::printf("[pitch] Keep: unmarked sound moves with the pitch, air stays, the hand-over is clean, Follow has one path\n");
        const double fs = 48000.0;
        constexpr auto keep = Params::PitchFormant::keep;
        constexpr auto follow = Params::PitchFormant::follow;
        const int from = static_cast<int>(1.0 * fs), to = static_cast<int>(5.0 * fs);

        // (a) band-passed pink noise (400..1500 Hz) at -20 dBFS: moved by the shift, level kept.
        auto mid = filtered(makePink(fs, 6.0, 0.5f, 5), fs, 400.0, 1500.0);
        scaleToRmsDb(mid, -20.0);
        const double inCentroid = logCentroidSt(mid, fs, from, to);
        for (const float st : { -5.0f, -12.0f, 4.0f })
        {
            const auto out = runPitchAir(fs, mid, pitchSettings(st, keep), -1);
            const double moved = logCentroidSt(out, fs, from, to) - inCentroid;
            const double level = 10.0 * std::log10(meanSquare(out, from, to) / meanSquare(mid, from, to));
            std::printf("  band-passed noise, Keep %+.0f st: centroid moved %+.2f st, level %+.2f dB\n", static_cast<double>(st), moved, level);
            check(std::abs(moved - st) <= 0.5, "Keep " + juce::String(st, 0) + " moves unmarked noise by " + juce::String(moved, 2) + " st");
            check(std::abs(level) <= 0.5, "Keep " + juce::String(st, 0) + " changes the level of unmarked noise by " + juce::String(level, 2) + " dB");
        }

        // (b) pink noise above 5 kHz is air: Keep -5 leaves it as the delayed input.
        auto air = filtered(makePink(fs, 6.0, 0.5f, 6), fs, 5000.0, 0.0);
        scaleToRmsDb(air, -20.0);
        {
            const auto out = runPitchAir(fs, air, pitchSettings(-5.0f, keep), -1);
            double err = 0.0;
            for (int i = static_cast<int>(0.02 * fs); i < static_cast<int>(air.size()); ++i)
                err += std::pow(static_cast<double>(out[static_cast<size_t>(i)]) - air[static_cast<size_t>(i)], 2.0);
            const double db = 10.0 * std::log10(err / (static_cast<double>(air.size()) - 0.02 * fs) / meanSquare(air, 0, static_cast<int>(air.size())) + 1e-30);
            std::printf("  noise above 5 kHz, Keep -5: output against the input %.1f dB\n", db);
            check(db <= -60.0, "Keep -5 changes air (noise above 5 kHz): " + juce::String(db, 1) + " dB against the input (<= -60)");
        }

        // (c) the hand-over itself: on the band-passed noise, the air decision forced to alternate
        // every 200 ms, so the output switches between the voice read and the air read on sound
        // whose own clicks would not hide a step (noise above 5 kHz, as the plan's signal has,
        // hides any step from this measure).
        {
            roar::PitchModule m;
            const auto ps = pitchSettings(-5.0f, keep);
            m.setParams(ps);
            m.prepare(fs, 32);
            m.setParams(ps);
            m.reset();
            const int latency = m.getLatencySamples();
            Signal x = mid;
            x.resize(mid.size() + static_cast<size_t>(latency), 0.0f);
            Signal r(32, 0.0f);
            const int period = static_cast<int>(0.2 * fs);
            for (int start = 0; start < static_cast<int>(x.size()); start += 32)
            {
                m.setAirForTests(((start + latency) / period) % 2);
                m.process(x.data() + start, r.data(), std::min(32, static_cast<int>(x.size()) - start), false, roar::VoiceFrameView {});
            }
            const Signal out(x.begin() + latency, x.end());
            const double inRate = clickRate(mid, fs, from, to), outRate = clickRate(out, fs, from, to);
            std::printf("  band-passed noise, Keep -5, voice read and air read swapped every 200 ms: %.2f clicks per second (input %.2f)\n", outRate, inRate);
            check(outRate <= inRate + 0.1, "the hand-over between the voice read and the air read clicks: " + juce::String(outRate, 2) + " per second (input " + juce::String(inRate, 2) + ")");
        }

        // (d) Follow has one path: the air decision changes nothing.
        {
            const auto voice = makeVoice(fs, 2.0);
            struct S { float st, size; };
            int differ = 0;
            for (const auto& c : { S { 12, 0 }, S { 7, 0 }, S { 3, 0 }, S { -2, 0 }, S { -5, 0 }, S { -12, 0 }, S { -24, 0 }, S { 0, 0.25f }, S { 0, 0.5f }, S { 0, 1.0f } })
            {
                const auto a = runPitchAir(fs, voice, pitchSettings(c.st, follow, c.size), 0);
                const auto b = runPitchAir(fs, voice, pitchSettings(c.st, follow, c.size), 1);
                double err = 0.0;
                for (size_t i = 0; i < a.size(); ++i)
                    err = std::max(err, std::abs(static_cast<double>(a[i]) - b[i]));
                differ += err > 0.0 ? 1 : 0;
                check(err == 0.0, "Follow " + juce::String(c.st, 0) + " st, Size " + juce::String(100 * c.size, 0) + " %: the air decision changes the output by " + juce::String(toDb(err), 1) + " dBFS");
            }
            std::printf("  Follow, ten settings: %d change with the air decision\n", differ);
        }
    }
}

