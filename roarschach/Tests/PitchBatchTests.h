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
}

