// Offline test runner for Reversinator.
//
//   ReversinatorTests render <in.wav> <out.wav> [--mode 0|1|2] [--time s] [--feedback %] [--wet %] [--dry %] [--sr hz]
//                            [--reverser 0|1] [--block n] [--random-blocks]
//   ReversinatorTests screenshot <out.png> [--input in.wav] [--seconds s] [--scale f] [--width px] [--about [--select]]
//                                [--edit] [--menu edit|demo] [params...]
//   ReversinatorTests selftest
//   ReversinatorTests hostcheck <plugin.vst3|plugin.component>

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SnippetDisplay.h"
#include "AboutPanel.h"

namespace
{
    struct Options
    {
        juce::StringArray positional;
        juce::StringPairArray named;
        juce::StringArray flags;

        Options(const juce::StringArray& args, int first)
        {
            for (int i = first; i < args.size(); ++i)
            {
                if (args[i].startsWith("--"))
                {
                    const auto key = args[i].substring(2);
                    if (i + 1 < args.size() && ! args[i + 1].startsWith("--") && key != "random-blocks")
                        named.set(key, args[++i]);
                    else
                        flags.add(key);
                }
                else
                {
                    positional.add(args[i]);
                }
            }
        }

        bool has(const juce::String& key) const { return named.containsKey(key) || flags.contains(key); }
        double number(const juce::String& key, double fallback) const
        {
            return named.containsKey(key) ? named[key].getDoubleValue() : fallback;
        }
    };

    void setParam(ReversinatorAudioProcessor& p, const juce::String& id, float value)
    {
        if (auto* param = p.getParameters().getParameter(id))
            param->setValueNotifyingHost(param->convertTo0to1(value));
    }

    void applyOptions(ReversinatorAudioProcessor& p, const Options& o)
    {
        if (o.has("mode"))     setParam(p, ParamIDs::mode, static_cast<float>(o.number("mode", 0)));
        if (o.has("time"))     setParam(p, ParamIDs::time, static_cast<float>(o.number("time", 2.0)));
        if (o.has("feedback")) setParam(p, ParamIDs::feedback, static_cast<float>(o.number("feedback", 0.0)));
        if (o.has("wet"))      setParam(p, ParamIDs::wetMix, static_cast<float>(o.number("wet", 100.0)));
        if (o.has("dry"))      setParam(p, ParamIDs::dryMix, static_cast<float>(o.number("dry", 0.0)));
        if (o.has("reverser")) setParam(p, ParamIDs::reverser, static_cast<float>(o.number("reverser", 1.0)));
    }

    bool readWav(const juce::File& file, juce::AudioBuffer<float>& audio, double& sampleRate)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader == nullptr)
            return false;

        audio.setSize(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
        reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);
        sampleRate = reader->sampleRate;
        return true;
    }

    bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        file.deleteFile();
        auto stream = file.createOutputStream();
        if (stream == nullptr)
            return false;

        // 32-bit float WAV; the writer takes ownership of the stream on success.
        juce::WavAudioFormat wav;
        auto* rawStream = stream.release();
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wav.createWriterFor(rawStream, sampleRate, static_cast<unsigned int>(audio.getNumChannels()), 32, {}, 0));
        if (writer == nullptr)
        {
            delete rawStream;
            return false;
        }

        return writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    }

    void prepare(ReversinatorAudioProcessor& p, int numChannels, double sampleRate, int maxBlock)
    {
        juce::AudioProcessor::BusesLayout layout;
        const auto set = numChannels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
        layout.inputBuses.add(set);
        layout.outputBuses.add(set);
        p.setBusesLayout(layout);
        p.setRateAndBufferSizeDetails(sampleRate, maxBlock);
        p.prepareToPlay(sampleRate, maxBlock);
    }

    /** Processes `audio` in place in blocks; `automation(sample)` runs before each block. */
    void render(ReversinatorAudioProcessor& p, juce::AudioBuffer<float>& audio, double sampleRate, int blockSize,
                bool randomBlocks, const std::function<void(int)>& automation = {}, int seed = 1)
    {
        prepare(p, audio.getNumChannels(), sampleRate, blockSize);
        juce::Random random(seed);
        juce::MidiBuffer midi;

        for (int pos = 0; pos < audio.getNumSamples();)
        {
            int n = randomBlocks ? 1 + random.nextInt(blockSize) : blockSize;
            n = std::min(n, audio.getNumSamples() - pos);

            if (automation)
                automation(pos);

            juce::AudioBuffer<float> block(audio.getArrayOfWritePointers(), audio.getNumChannels(), pos, n);
            p.processBlock(block, midi);
            pos += n;
        }

        p.releaseResources();
    }

    juce::AudioBuffer<float> makeNoise(int channels, int samples, float gain, int seed)
    {
        juce::AudioBuffer<float> b(channels, samples);
        juce::Random r(seed);
        for (int c = 0; c < channels; ++c)
        {
            float lp = 0.0f;
            for (int i = 0; i < samples; ++i)
            {
                lp += 0.25f * ((r.nextFloat() * 2.0f - 1.0f) - lp);   // slightly darker than white
                b.setSample(c, i, gain * lp * 2.0f);
            }
        }
        return b;
    }

    juce::AudioBuffer<float> makeSine(int channels, int samples, double sampleRate, double hz, float gain)
    {
        juce::AudioBuffer<float> b(channels, samples);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < samples; ++i)
                b.setSample(c, i, gain * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * hz * i / sampleRate)));
        return b;
    }

    struct Stats
    {
        bool finite = true;
        float peak = 0.0f;
        float maxSecondDifference = 0.0f;
    };

    Stats analyse(const juce::AudioBuffer<float>& b, int from = 0)
    {
        Stats s;
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            const float* x = b.getReadPointer(c);
            for (int i = from; i < b.getNumSamples(); ++i)
            {
                if (! std::isfinite(x[i])) { s.finite = false; continue; }
                s.peak = std::max(s.peak, std::abs(x[i]));
                if (i >= from + 2)
                    s.maxSecondDifference = std::max(s.maxSecondDifference, std::abs(x[i] - 2.0f * x[i - 1] + x[i - 2]));
            }
        }
        return s;
    }

    int failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "PASS  " : "FAIL  ") << what << std::endl;
        if (! ok)
            ++failures;
    }

    //==========================================================================
    int runRender(const Options& o)
    {
        if (o.positional.size() < 2) { std::cerr << "render <in.wav> <out.wav> [options]\n"; return 2; }

        juce::AudioBuffer<float> audio;
        double sampleRate = 0.0;
        if (! readWav(juce::File::getCurrentWorkingDirectory().getChildFile(o.positional[0]), audio, sampleRate))
        { std::cerr << "cannot read " << o.positional[0] << "\n"; return 1; }

        // --sr processes the samples as if they were at another rate (the vendor's demo
        // renders were made at 44.1 kHz on 48 kHz files); the output keeps the file's rate.
        // --preroll n runs n samples of silence first (shifts the snippet clock against the
        // audio) and trims them from the output.
        const double processRate = o.number("sr", sampleRate);
        const int preroll = std::max(0, static_cast<int>(o.number("preroll", 0)));
        if (preroll > 0)
        {
            juce::AudioBuffer<float> padded(audio.getNumChannels(), audio.getNumSamples() + preroll);
            padded.clear();
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                padded.copyFrom(ch, preroll, audio, ch, 0, audio.getNumSamples());
            audio = std::move(padded);
        }

        ReversinatorAudioProcessor p;
        applyOptions(p, o);
        render(p, audio, processRate, static_cast<int>(o.number("block", 512)), o.has("random-blocks"));

        if (preroll > 0)
        {
            juce::AudioBuffer<float> trimmed(audio.getNumChannels(), audio.getNumSamples() - preroll);
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                trimmed.copyFrom(ch, 0, audio, ch, preroll, trimmed.getNumSamples());
            audio = std::move(trimmed);
        }

        if (! writeWav(juce::File::getCurrentWorkingDirectory().getChildFile(o.positional[1]), audio, sampleRate))
        { std::cerr << "cannot write " << o.positional[1] << "\n"; return 1; }

        return 0;
    }

    template <typename ComponentType>
    ComponentType* findComponent(juce::Component& c)
    {
        if (auto* found = dynamic_cast<ComponentType*>(&c))
            return found;
        for (auto* child : c.getChildren())
            if (auto* found = findComponent<ComponentType>(*child))
                return found;
        return nullptr;
    }

    SnippetDisplay* findDisplay(juce::Component& c) { return findComponent<SnippetDisplay>(c); }

    int runScreenshot(const Options& o)
    {
        if (o.positional.isEmpty()) { std::cerr << "screenshot <out.png> [options]\n"; return 2; }

        ReversinatorAudioProcessor p;
        applyOptions(p, o);

        // Feed some audio so the display has history and the read heads are mid-flight.
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
        if (! o.has("input") || ! readWav(juce::File::getCurrentWorkingDirectory().getChildFile(o.named["input"]), audio, sampleRate))
            audio = makeNoise(2, 44100 * 8, 0.3f, 7);

        const int length = std::min(audio.getNumSamples(), static_cast<int>(o.number("seconds", 7.3) * sampleRate));
        audio.setSize(audio.getNumChannels(), length, true);
        prepare(p, audio.getNumChannels(), sampleRate, 512);
        juce::MidiBuffer midi;
        for (int pos = 0; pos < length; pos += 512)
        {
            juce::AudioBuffer<float> block(audio.getArrayOfWritePointers(), audio.getNumChannels(), pos, std::min(512, length - pos));
            p.processBlock(block, midi);
        }

        std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
        const int width = static_cast<int>(o.number("width", ReversinatorAudioProcessorEditor::baseWidth));
        editor->setSize(width, juce::roundToInt(width * static_cast<double>(ReversinatorAudioProcessorEditor::baseHeight)
                                                / ReversinatorAudioProcessorEditor::baseWidth));
        if (auto* display = findDisplay(*editor))
            for (int i = 0; i < 60; ++i)
                display->update();   // let the span settle

        if (o.has("about"))
            if (auto* about = findComponent<AboutPanel>(*editor))
            {
                about->show();
                if (o.has("select"))   // part of the licence text selected
                    if (auto* licence = findComponent<juce::TextEditor>(*about))
                        licence->setHighlightedRegion({ 0, 120 });
            }

        // --edit: the first slider's value box while it is typed into, its text selected.
        auto* slider = findComponent<juce::Slider>(*editor);
        if (o.has("edit") && slider != nullptr)
        {
            slider->showTextBox();
            if (auto* label = findComponent<juce::Label>(*slider))
                if (auto* ed = label->getCurrentTextEditor())
                    ed->selectAll();
        }

        // --menu edit|demo: a popup menu open over the editor. "edit" is the value box's own
        // right-click menu; "demo" shows every menu element (header, tick, disabled, submenu).
        if (o.has("menu"))
        {
            juce::PopupMenu menu;
            int selected = 1;
            if (o.named["menu"] == "demo")
            {
                menu.addSectionHeader("SECTION");
                menu.addItem(1, "Highlighted item");
                menu.addItem(2, "Ticked item", true, true);
                menu.addItem(3, "Disabled item", false);
                menu.addSeparator();
                juce::PopupMenu sub;
                sub.addItem(10, "Sub item");
                menu.addSubMenu("Submenu", sub);
            }
            else
            {
                juce::TextEditor scratch;   // editable, like a value box being typed into
                scratch.setText("2.00 s");
                scratch.selectAll();
                scratch.addPopupMenuItems(menu, nullptr);
                selected = 2;
            }

            menu.setLookAndFeel(&editor->getLookAndFeel());
            auto anchor = slider != nullptr ? editor->getLocalArea(slider, slider->getLocalBounds()).removeFromTop(24)
                                            : editor->getLocalBounds().withSizeKeepingCentre(1, 1);
            menu.showMenuAsync(juce::PopupMenu::Options().withParentComponent(editor.get())
                                                       .withTargetScreenArea(anchor)
                                                       .withInitiallySelectedItem(selected));
        }

        const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, static_cast<float>(o.number("scale", 2.0)));
        juce::PopupMenu::dismissAllActiveMenus();

        juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(o.positional[0]);
        out.deleteFile();
        juce::FileOutputStream stream(out);
        juce::PNGImageFormat png;
        return png.writeImageToStream(image, stream) ? 0 : 1;
    }

    //==========================================================================
    int runSelfTest()
    {
        // 1. Stability under heavy automation, for every mode, rate, layout and block size.
        for (const double sr : { 44100.0, 48000.0, 96000.0 })
            for (const int channels : { 1, 2 })
                for (const int block : { 1, 31, 512, 4096 })
                    for (int mode = 0; mode < 3; ++mode)
                    {
                        auto audio = makeNoise(channels, static_cast<int>(sr * (block == 1 ? 3.0 : 9.0)), 0.5f, 3 + mode);
                        ReversinatorAudioProcessor p;
                        setParam(p, ParamIDs::mode, static_cast<float>(mode));
                        render(p, audio, sr, block, block > 1, [&](int pos)
                        {
                            const double t = pos / sr;
                            setParam(p, ParamIDs::time, static_cast<float>(2.25 + 1.75 * std::sin(t * 2.1)));
                            setParam(p, ParamIDs::feedback, t > 2.0 ? 75.0f : 0.0f);
                            setParam(p, ParamIDs::reverser, std::fmod(t, 1.7) < 1.2 ? 1.0f : 0.0f);
                            setParam(p, ParamIDs::dryMix, static_cast<float>(50.0 + 50.0 * std::sin(t * 5.0)));
                        });
                        const auto s = analyse(audio);
                        check(s.finite && s.peak < 6.0f,
                              juce::String::formatted("stable  mode %d  %.0f Hz  %dch  block %d  (peak %.2f)", mode, sr, channels, block, s.peak));
                    }

        // 2. Block-size independence with static parameters.
        for (int mode = 0; mode < 3; ++mode)
        {
            const auto source = makeNoise(2, 44100 * 7, 0.5f, 11);
            auto a = source, b = source, c = source;
            for (auto* buf : { &a, &b, &c })
            {
                ReversinatorAudioProcessor p;
                setParam(p, ParamIDs::mode, static_cast<float>(mode));
                setParam(p, ParamIDs::feedback, 40.0f);
                setParam(p, ParamIDs::dryMix, 30.0f);
                render(p, *buf, 44100.0, buf == &a ? 64 : (buf == &b ? 4096 : 1000), buf == &c);
            }
            float diff = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    diff = std::max({ diff, std::abs(a.getSample(ch, i) - b.getSample(ch, i)), std::abs(a.getSample(ch, i) - c.getSample(ch, i)) });
            check(diff < 1.0e-5f, juce::String::formatted("block-size independent  mode %d  (max diff %.2e)", mode, diff));
        }

        // 3. No clicks on a steady tone while the controls move.
        for (int mode = 0; mode < 3; ++mode)
        {
            const double sr = 44100.0;
            auto audio = makeSine(2, static_cast<int>(sr * 12), sr, 220.0, 0.5f);
            ReversinatorAudioProcessor p;
            setParam(p, ParamIDs::mode, static_cast<float>(mode));
            render(p, audio, sr, 256, false, [&](int pos)
            {
                const double t = pos / sr;
                setParam(p, ParamIDs::time, static_cast<float>(2.0 + 1.5 * std::sin(t * 1.3)));   // sweeping TIME
                setParam(p, ParamIDs::reverser, std::fmod(t, 3.0) < 2.0 ? 1.0f : 0.0f);            // toggling REVERSER
                setParam(p, ParamIDs::wetMix, std::fmod(t, 2.0) < 1.0 ? 100.0f : 20.0f);           // jumping WET
            });
            // A 220 Hz sine at 0.5 has a second difference of ~0.0005 per sample at most;
            // amplitude and crossfade envelopes add a little. A click is orders of magnitude larger.
            const auto s = analyse(audio, static_cast<int>(sr * 0.05));
            check(s.finite && s.maxSecondDifference < 0.02f,
                  juce::String::formatted("click-free automation  mode %d  (max 2nd diff %.4f)", mode, s.maxSecondDifference));
        }

        // 4. Mode switches are smooth too.
        {
            const double sr = 44100.0;
            auto audio = makeSine(2, static_cast<int>(sr * 10), sr, 330.0, 0.5f);
            ReversinatorAudioProcessor p;
            render(p, audio, sr, 512, false, [&](int pos)
            {
                setParam(p, ParamIDs::mode, static_cast<float>((pos / static_cast<int>(sr * 1.3)) % 3));
            });
            const auto s = analyse(audio, static_cast<int>(sr * 0.05));
            check(s.finite && s.maxSecondDifference < 0.02f,
                  juce::String::formatted("click-free mode switching  (max 2nd diff %.4f)", s.maxSecondDifference));
        }

        // 5. State round trip.
        {
            ReversinatorAudioProcessor a, b;
            setParam(a, ParamIDs::mode, 2.0f);
            setParam(a, ParamIDs::time, 3.21f);
            setParam(a, ParamIDs::feedback, 61.0f);
            setParam(a, ParamIDs::wetMix, 77.0f);
            setParam(a, ParamIDs::dryMix, 12.0f);
            setParam(a, ParamIDs::reverser, 0.0f);
            juce::MemoryBlock state;
            a.getStateInformation(state);
            b.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

            bool same = true;
            for (const auto* id : { ParamIDs::mode, ParamIDs::time, ParamIDs::feedback, ParamIDs::wetMix, ParamIDs::dryMix, ParamIDs::reverser })
                same = same && std::abs(a.getParameters().getRawParameterValue(id)->load()
                                        - b.getParameters().getRawParameterValue(id)->load()) < 1.0e-4f;
            check(same, "state round trip");
        }

        // 6. Reverser off = forwards input at the wet level (plus dry), as on the original.
        {
            const double sr = 44100.0;
            const auto source = makeNoise(2, static_cast<int>(sr * 2), 0.5f, 5);
            auto audio = source;
            ReversinatorAudioProcessor p;
            setParam(p, ParamIDs::reverser, 0.0f);
            setParam(p, ParamIDs::wetMix, 50.0f);
            setParam(p, ParamIDs::dryMix, 0.0f);
            render(p, audio, sr, 512, false);
            float err = 0.0f;
            for (int i = static_cast<int>(sr * 0.1); i < audio.getNumSamples(); ++i)
                err = std::max(err, std::abs(audio.getSample(0, i) - 0.25f * source.getSample(0, i)));
            check(err < 1.0e-4f, juce::String::formatted("reverser off passes input at wet^2  (max err %.2e)", err));
        }

        // 7. A non-finite input sample must not poison the output (even with feedback).
        {
            const double sr = 44100.0;
            auto audio = makeNoise(2, static_cast<int>(sr * 12), 0.5f, 13);
            audio.setSample(0, static_cast<int>(sr * 1.0), std::numeric_limits<float>::quiet_NaN());
            audio.setSample(1, static_cast<int>(sr * 1.5), std::numeric_limits<float>::infinity());
            ReversinatorAudioProcessor p;
            setParam(p, ParamIDs::feedback, 75.0f);
            setParam(p, ParamIDs::dryMix, 0.0f);
            render(p, audio, sr, 512, false);
            // The dry path is off; the reversed output must stay finite throughout.
            const auto s = analyse(audio, static_cast<int>(sr * 2.0));
            check(s.finite, "non-finite input does not latch");
        }

        // 8. A host reset clears everything that was buffered (without one, the tail is heard).
        {
            const double sr = 44100.0;
            const auto tailAfter = [sr](bool withReset)
            {
                ReversinatorAudioProcessor p;
                setParam(p, ParamIDs::feedback, 75.0f);
                prepare(p, 2, sr, 512);
                juce::MidiBuffer midi;
                auto noise = makeNoise(2, static_cast<int>(sr * 3), 0.5f, 17);
                for (int pos = 0; pos + 512 <= noise.getNumSamples(); pos += 512)
                {
                    juce::AudioBuffer<float> block(noise.getArrayOfWritePointers(), 2, pos, 512);
                    p.processBlock(block, midi);
                }
                if (withReset)
                    p.reset();
                juce::AudioBuffer<float> silence(2, static_cast<int>(sr * 2));
                silence.clear();
                for (int pos = 0; pos + 512 <= silence.getNumSamples(); pos += 512)
                {
                    juce::AudioBuffer<float> block(silence.getArrayOfWritePointers(), 2, pos, 512);
                    p.processBlock(block, midi);
                }
                return silence.getMagnitude(0, silence.getNumSamples());
            };
            const float tail = tailAfter(false);
            const float cleared = tailAfter(true);
            check(cleared < 1.0e-6f && tail > 1.0e-3f,
                  juce::String::formatted("reset() clears buffered audio  (tail %.3f without it, %.1e with it)", tail, cleared));
        }

        // 9. Host bypass keeps the engine fed (no stale replay) and crossfades both edges.
        {
            const double sr = 48000.0;
            const auto runBlocks = [](ReversinatorAudioProcessor& p, juce::AudioBuffer<float>& b, const std::function<bool(int)>& bypassedAt)
            {
                juce::MidiBuffer midi;
                for (int pos = 0; pos < b.getNumSamples(); pos += 512)
                {
                    juce::AudioBuffer<float> block(b.getArrayOfWritePointers(), b.getNumChannels(), pos, std::min(512, b.getNumSamples() - pos));
                    if (bypassedAt(pos)) p.processBlockBypassed(block, midi);
                    else                 p.processBlock(block, midi);
                }
            };

            ReversinatorAudioProcessor p;
            prepare(p, 2, sr, 512);
            auto tone = makeSine(2, static_cast<int>(sr * 4), sr, 440.0, 0.5f);
            runBlocks(p, tone, [](int) { return false; });
            juce::AudioBuffer<float> gap(2, static_cast<int>(sr * 6));
            gap.clear();
            runBlocks(p, gap, [](int) { return true; });
            juce::AudioBuffer<float> after(2, static_cast<int>(sr * 3));
            after.clear();
            runBlocks(p, after, [](int) { return false; });
            check(after.getMagnitude(0, after.getNumSamples()) < 1.0e-4f, "host bypass keeps the engine fed (no stale replay)");

            ReversinatorAudioProcessor q;
            prepare(q, 2, sr, 512);
            auto steady = makeSine(2, static_cast<int>(sr * 9), sr, 220.0, 0.5f);
            runBlocks(q, steady, [sr](int pos) { const double t = pos / sr; return t >= 3.0 && t < 6.0; });
            const auto s = analyse(steady, static_cast<int>(sr * 0.05));
            check(s.maxSecondDifference < 0.02f, juce::String::formatted("click-free host bypass  (max 2nd diff %.4f)", s.maxSecondDifference));
        }

        // 10. REVERSER's text round-trips (hosts' generic editors type it back in).
        {
            ReversinatorAudioProcessor p;
            auto* reverser = p.getParameters().getParameter(ParamIDs::reverser);
            check(reverser->getValueForText("Reverse") > 0.5f && reverser->getValueForText("Forward") < 0.5f
                      && reverser->getValueForText(reverser->getText(1.0f, 64)) > 0.5f
                      && reverser->getValueForText(reverser->getText(0.0f, 64)) < 0.5f,
                  "REVERSER text round trip");
        }

        // 11. The editor opens at 100%, then restores the size it was left at.
        {
            ReversinatorAudioProcessor a;
            std::unique_ptr<juce::AudioProcessorEditor> editor(a.createEditor());
            const int firstWidth = editor->getWidth();
            editor->setSize(ReversinatorAudioProcessorEditor::baseWidth * 3 / 2, ReversinatorAudioProcessorEditor::baseHeight * 3 / 2);
            juce::MemoryBlock state;
            a.getStateInformation(state);
            editor.reset();

            ReversinatorAudioProcessor b;
            b.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            std::unique_ptr<juce::AudioProcessorEditor> reopened(b.createEditor());
            check(firstWidth == ReversinatorAudioProcessorEditor::baseWidth
                      && reopened->getWidth() == ReversinatorAudioProcessorEditor::baseWidth * 3 / 2,
                  juce::String::formatted("editor size: first open %d, restored %d", firstWidth, reopened->getWidth()));

            // Every slider's thumb sits fully inside the slider at both ends of its range.
            bool inside = true;
            std::function<void(juce::Component&)> visit = [&](juce::Component& c)
            {
                if (auto* slider = dynamic_cast<juce::Slider*>(&c))
                {
                    const float atMin = slider->getPositionOfValue(slider->getMinimum());
                    const float atMax = slider->getPositionOfValue(slider->getMaximum());
                    inside = inside && atMin <= static_cast<float>(slider->getHeight()) - 6.0f && atMax >= 24.0f;
                }
                for (auto* child : c.getChildren())
                    visit(*child);
            };
            visit(*reopened);
            check(inside, "slider thumbs are drawn in full at both ends");
        }

        // 12. JUCE's AU wrapper calls reset() as host bypass engages. That must neither click nor
        //     empty the effect; a fully bypassed block is the input bit for bit; and once
        //     un-bypassed the output is identical to an instance that was never bypassed.
        {
            const double sr = 48000.0;
            const int blockSize = 512;
            const auto input = makeSine(2, static_cast<int>(sr * 6), sr, 220.0, 0.5f);
            const int bypassFrom = 280 * blockSize;               // ~3 s
            const int bypassTo = bypassFrom + 30 * blockSize;     // a short bypass: ~0.32 s
            const int fade = static_cast<int>(sr * 0.010) + 2;

            auto reference = input;   // never bypassed
            auto output = input;      // bypassed in between
            ReversinatorAudioProcessor a;
            juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_AudioUnit);
            ReversinatorAudioProcessor b;
            juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
            prepare(a, 2, sr, blockSize);
            prepare(b, 2, sr, blockSize);
            juce::MidiBuffer midi;

            for (int pos = 0; pos < input.getNumSamples(); pos += blockSize)
            {
                const int n = std::min(blockSize, input.getNumSamples() - pos);
                juce::AudioBuffer<float> ra(reference.getArrayOfWritePointers(), 2, pos, n);
                a.processBlock(ra, midi);

                juce::AudioBuffer<float> rb(output.getArrayOfWritePointers(), 2, pos, n);
                if (pos == bypassFrom)
                    b.reset();   // what the AU wrapper does when bypass is switched on
                if (pos >= bypassFrom && pos < bypassTo) b.processBlockBypassed(rb, midi);
                else                                     b.processBlock(rb, midi);
            }

            const auto s = analyse(output, static_cast<int>(sr * 0.05));
            bool exactBypass = true, live = true;
            for (int ch = 0; ch < 2; ++ch)
            {
                for (int i = bypassFrom + fade; i < bypassTo; ++i)
                    exactBypass = exactBypass && juce::exactlyEqual(output.getSample(ch, i), input.getSample(ch, i));
                for (int i = bypassTo + fade; i < input.getNumSamples(); ++i)
                    live = live && juce::exactlyEqual(output.getSample(ch, i), reference.getSample(ch, i));
            }
            check(s.maxSecondDifference < 0.02f,
                  juce::String::formatted("click-free AU-style bypass with reset()  (max 2nd diff %.4f)", s.maxSecondDifference));
            check(exactBypass, "fully bypassed output is the input, bit for bit");
            check(live, "un-bypass lands in the live effect (identical to never bypassed)");
        }

        // 13. Above 50 kHz the snippet clock is exact: each snippet lasts T * SR samples and the
        //     read head runs at exactly -1, so nothing is out of tune. Up to 50 kHz it keeps the
        //     original's float32 timing (88257.7 samples per snippet at 2 s and 44.1 kHz).
        {
            const auto measure = [](double sr, float t, double& period, double& worstCents)
            {
                ReverseEngine e;
                e.prepare(sr, 1, 1);
                e.setMode(ReverseEngine::Mode::ReversePlayback, true);
                e.setTime(t, true);

                const int total = static_cast<int>(2.5 * t * sr);
                std::vector<double> delay(static_cast<size_t>(total));
                float in = 0.0f, out = 0.0f;
                const float* inputs[] = { &in };
                float* outputs[] = { &out };
                int count = 0;
                for (int n = 0; n < total; ++n)
                {
                    e.process(inputs, outputs, 1, 1);
                    delay[static_cast<size_t>(n)] = e.getVoiceViews(count)[0].delaySeconds * sr;   // samples
                }

                std::vector<int> jumps;
                for (int n = 1; n < total; ++n)
                    if (delay[static_cast<size_t>(n)] < delay[static_cast<size_t>(n - 1)] - 0.5 * t * sr)
                        jumps.push_back(n);

                period = jumps.size() >= 2 ? static_cast<double>(jumps[1] - jumps[0]) : 0.0;
                worstCents = 0.0;
                if (jumps.size() >= 2)
                {
                    // Read speed in stretches across the snippet, away from the jumps.
                    const int span = static_cast<int>(period / 16);
                    for (int start = jumps[0] + 64; start + span < jumps[1] - 64; start += span)
                    {
                        const double slope = (delay[static_cast<size_t>(start + span)] - delay[static_cast<size_t>(start)]) / span;
                        worstCents = std::max(worstCents, std::abs(1200.0 * std::log2(slope - 1.0)));
                    }
                }
            };

            double period = 0.0, cents = 0.0;
            bool exact = true;
            juce::String detail;
            for (const auto& [sr, t] : { std::pair { 96000.0, 2.0f }, std::pair { 192000.0, 3.925f }, std::pair { 384000.0, 4.0f } })
            {
                measure(sr, t, period, cents);
                exact = exact && std::abs(period - static_cast<double>(t) * sr) <= 1.0 && cents < 0.1;
                detail << juce::String::formatted(" %gk: %.0f/%.0f, %.2f c;", sr / 1000.0, period, static_cast<double>(t) * sr, cents);
            }
            check(exact, "exact snippet clock above 50 kHz (" + detail.trim() + ")");

            measure(44100.0, 2.0f, period, cents);
            check(period >= 88257.0 && period <= 88258.0,
                  juce::String::formatted("44.1 kHz keeps the original's float32 clock  (%.0f samples per snippet)", period));
        }

        // 14. A host stop or locate while bypassed is still honoured (once the bypass fade is
        //     done), so un-bypassing never replays audio from before it. Outside AU, a reset that
        //     lands just before bypass engages is a real one too, and is honoured at once.
        {
            const double sr = 48000.0;
            constexpr int blockSize = 512;
            const auto heardAfterUnbypass = [sr](juce::AudioProcessor::WrapperType type, bool resetBeforeEngage)
            {
                juce::AudioProcessor::setTypeOfNextNewPlugin(type);
                ReversinatorAudioProcessor p;
                juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
                prepare(p, 2, sr, blockSize);
                juce::MidiBuffer midi;

                auto tone = makeSine(2, 375 * blockSize, sr, 220.0, 0.5f);   // ~4 s, played normally
                for (int pos = 0; pos < tone.getNumSamples(); pos += blockSize)
                {
                    juce::AudioBuffer<float> block(tone.getArrayOfWritePointers(), 2, pos, blockSize);
                    p.processBlock(block, midi);
                }

                if (resetBeforeEngage)
                {
                    p.reset();                           // the host stops, then bypass is switched on
                }
                else
                {
                    if (type == juce::AudioProcessor::wrapperType_AudioUnit)
                        p.reset();                       // the AU wrapper's reset as bypass engages
                    auto more = makeSine(2, 2 * blockSize, sr, 220.0, 0.5f);
                    for (int b = 0; b < 2; ++b)
                    {
                        juce::AudioBuffer<float> block(more.getArrayOfWritePointers(), 2, b * blockSize, blockSize);
                        p.processBlockBypassed(block, midi);
                    }
                    p.reset();                           // the host locates while bypassed
                }

                juce::AudioBuffer<float> silence(2, blockSize);
                for (int b = 0; b < 30; ++b)             // ~0.3 s of bypassed silence
                {
                    silence.clear();
                    p.processBlockBypassed(silence, midi);
                }

                float heard = 0.0f;
                for (int b = 0; b < 300; ++b)            // ~3 s after un-bypassing
                {
                    silence.clear();
                    p.processBlock(silence, midi);
                    heard = std::max(heard, silence.getMagnitude(0, blockSize));
                }
                return heard;
            };

            const float au = heardAfterUnbypass(juce::AudioProcessor::wrapperType_AudioUnit, false);
            const float vst3 = heardAfterUnbypass(juce::AudioProcessor::wrapperType_VST3, false);
            const float vst3Before = heardAfterUnbypass(juce::AudioProcessor::wrapperType_VST3, true);
            check(au < 1.0e-6f && vst3 < 1.0e-6f && vst3Before < 1.0e-6f,
                  juce::String::formatted("reset() while bypassed: no stale audio after un-bypass  (AU %.1e, VST3 %.1e, VST3 before engage %.1e)",
                                          au, vst3, vst3Before));
        }

        // 15. The update check finds the newest Reversinator release in GitHub's listing,
        //     skipping other plugins' tags and prereleases.
        {
            const auto listing = juce::JSON::parse(R"([
                { "tag_name": "subbertone-v1.1.0" },
                { "tag_name": "reversinator-v2.1.0-beta", "prerelease": true },
                { "tag_name": "reversinator-v2.0.9" },
                { "tag_name": "reversinator-v2.0.10" },
                { "tag_name": "reversinator-v1.0.0" } ])");
            check(Version::latestRelease(listing) == "2.0.10"
                      && Version::latestRelease(juce::JSON::parse("[]")).isEmpty()
                      && Version::latestRelease(juce::JSON::parse(R"({ "message": "API rate limit exceeded" })")).isEmpty()
                      && Version::toNumber("10.0.0") > Version::toNumber("9.9.9"),
                  "update check picks the newest release (other plugins and prereleases skipped)");
        }

        // 16. Nothing falls back to JUCE's default look: every colour a Reversinator component
        //     can show differs from LookAndFeel_V4's, menus use the plugin's font, and the resize
        //     grip is the plugin's own drawing.
        {
            juce::SharedResourcePointer<Theme::Fonts> fonts;
            Controls::LookAndFeel ours(*fonts);
            juce::LookAndFeel_V4 stock;

            const std::pair<int, const char*> colours[] = {
                { juce::ResizableWindow::backgroundColourId,      "window background" },
                { juce::Label::textColourId,                      "label text" },
                { juce::Label::textWhenEditingColourId,           "label editing text" },
                { juce::TextEditor::backgroundColourId,           "text editor background" },
                { juce::TextEditor::textColourId,                 "text editor text" },
                { juce::TextEditor::outlineColourId,              "text editor outline" },
                { juce::TextEditor::focusedOutlineColourId,       "text editor focused outline" },
                { juce::TextEditor::highlightColourId,            "text selection" },
                { juce::TextEditor::highlightedTextColourId,      "selected text" },
                { juce::CaretComponent::caretColourId,            "caret" },
                { juce::PopupMenu::backgroundColourId,            "menu background" },
                { juce::PopupMenu::textColourId,                  "menu text" },
                { juce::PopupMenu::headerTextColourId,            "menu section header" },
                { juce::PopupMenu::highlightedBackgroundColourId, "menu highlight" },
                { juce::PopupMenu::highlightedTextColourId,       "menu highlighted text" },
                { juce::ScrollBar::thumbColourId,                 "scrollbar thumb" },
                { juce::TooltipWindow::backgroundColourId,        "tooltip background" },
                { juce::TooltipWindow::textColourId,              "tooltip text" },
                { juce::TooltipWindow::outlineColourId,           "tooltip outline" },
                { juce::TextButton::buttonColourId,               "button" },
                { juce::TextButton::textColourOffId,              "button text" },
            };
            juce::StringArray stockColours;
            for (const auto& [id, name] : colours)
                if (ours.findColour(id) == stock.findColour(id))
                    stockColours.add(name);

            const auto grip = [](juce::LookAndFeel& lf)
            {
                juce::Image image(juce::Image::ARGB, 18, 18, true);
                juce::Graphics g(image);
                lf.drawCornerResizer(g, 18, 18, false, false);
                return image;
            };
            const auto ourGrip = grip(ours), stockGrip = grip(stock);
            bool gripDiffers = false;
            for (int y = 0; y < 18 && ! gripDiffers; ++y)
                for (int x = 0; x < 18 && ! gripDiffers; ++x)
                    gripDiffers = ourGrip.getPixelAt(x, y) != stockGrip.getPixelAt(x, y);

            const auto menuFace = ours.getPopupMenuFont().getTypefaceName();

            // Text boxes are rounded (JUCE fills and frames a square), and menus are framed in the
            // panels' outline colour (JUCE uses a light grey on Windows).
            juce::TextEditor box;
            juce::Image boxImage(juce::Image::ARGB, 60, 20, true);
            {
                juce::Graphics g(boxImage);
                ours.fillTextEditorBackground(g, 60, 20, box);
                ours.drawTextEditorOutline(g, 60, 20, box);
            }
            juce::Image menuImage(juce::Image::ARGB, 40, 40, true);
            {
                juce::Graphics g(menuImage);
                ours.drawPopupMenuBackgroundWithOptions(g, 40, 40, juce::PopupMenu::Options());
            }
            const auto close = [](juce::Colour a, juce::Colour b)
            {
                return std::abs(a.getRed() - b.getRed()) < 12 && std::abs(a.getGreen() - b.getGreen()) < 12
                    && std::abs(a.getBlue() - b.getBlue()) < 12 && a.getAlpha() == 255;
            };
            const bool roundedBox = boxImage.getPixelAt(0, 0).getAlpha() == 0 && boxImage.getPixelAt(30, 10).getAlpha() == 255;
            const bool menuFrame = menuImage.getPixelAt(0, 0).getAlpha() < 128 && close(menuImage.getPixelAt(0, 20), Theme::Colours::outline)
                                && close(menuImage.getPixelAt(20, 20), Theme::Colours::panelRaised);

            // A value box being typed into keeps its centred text (JUCE's editor starts at the left).
            juce::Slider slider(juce::Slider::LinearVertical, juce::Slider::TextBoxAbove);
            slider.setLookAndFeel(&ours);
            slider.setBounds(0, 0, 80, 200);
            slider.showTextBox();
            const auto* valueBox = findComponent<juce::Label>(slider);
            const auto* typing = valueBox != nullptr ? valueBox->getCurrentTextEditor() : nullptr;
            const bool centredEditing = typing != nullptr && typing->getJustificationType() == juce::Justification::centred;
            slider.hideTextBox(true);
            slider.setLookAndFeel(nullptr);

            check(stockColours.isEmpty() && gripDiffers && menuFace.containsIgnoreCase("Azeret") && roundedBox && menuFrame && centredEditing,
                  "no JUCE default styling (colours, menu font " + menuFace + ", menu frame, rounded text boxes, centred editing, resize grip)"
                      + (stockColours.isEmpty() ? juce::String() : "  stock: " + stockColours.joinIntoString(", "))
                      + (roundedBox ? "" : "  square text box") + (menuFrame ? "" : "  menu frame")
                      + (centredEditing ? "" : "  editing text not centred"));
        }

        std::cout << (failures == 0 ? "ALL PASSED" : juce::String(failures) + " FAILED") << std::endl;
        return failures == 0 ? 0 : 1;
    }

    //==========================================================================
    int runHostCheck(const Options& o)
    {
        if (o.positional.isEmpty()) { std::cerr << "hostcheck <plugin>\n"; return 2; }

        juce::AudioPluginFormatManager formats;
        formats.addDefaultFormats();

        juce::OwnedArray<juce::PluginDescription> found;
        const auto path = juce::File::getCurrentWorkingDirectory().getChildFile(o.positional[0]).getFullPathName();
        for (auto* format : formats.getFormats())
            if (format->fileMightContainThisPluginType(path))
                format->findAllTypesForFile(found, path);

        if (found.isEmpty()) { std::cerr << "no plugin found in " << path << "\n"; return 1; }

        const auto& desc = *found.getFirst();
        std::cout << desc.pluginFormatName << "  " << desc.name << "  " << desc.manufacturerName << "  v" << desc.version << std::endl;

        juce::String error;
        auto instance = formats.createPluginInstance(desc, 48000.0, 512, error);
        if (instance == nullptr) { std::cerr << "load failed: " << error << "\n"; return 1; }

        for (auto* param : instance->getParameters())
            std::cout << "  param  " << param->getName(64) << " = " << param->getCurrentValueAsText()
                      << "  (default " << param->getText(param->getDefaultValue(), 64) << ")" << std::endl;

        instance->setPlayConfigDetails(2, 2, 48000.0, 512);
        instance->prepareToPlay(48000.0, 512);
        auto audio = makeNoise(2, 48000 * 6, 0.5f, 9);
        juce::MidiBuffer midi;
        for (int pos = 0; pos < audio.getNumSamples(); pos += 512)
        {
            juce::AudioBuffer<float> block(audio.getArrayOfWritePointers(), 2, pos, std::min(512, audio.getNumSamples() - pos));
            instance->processBlock(block, midi);
        }
        const auto s = analyse(audio);
        check(s.finite && s.peak > 0.01f && s.peak < 6.0f, juce::String::formatted("hosted processing  (peak %.2f)", s.peak));

        juce::MemoryBlock state;
        instance->getStateInformation(state);
        instance->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        check(state.getSize() > 0, "hosted state save/restore");
        instance->releaseResources();

        return failures == 0 ? 0 : 1;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add(juce::CharPointer_UTF8(argv[i]));

    const auto command = args.isEmpty() ? juce::String() : args[0];
    const Options options(args, 1);

    if (command == "render")     return runRender(options);
    if (command == "screenshot") return runScreenshot(options);
    if (command == "selftest")   return runSelfTest();
    if (command == "hostcheck")  return runHostCheck(options);

    std::cerr << "usage: ReversinatorTests render|screenshot|selftest|hostcheck ...\n";
    return 2;
}
