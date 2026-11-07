// Offline test runner for Reversinator.
//
//   ReversinatorTests render <in.wav> <out.wav> [--mode 0|1|2] [--time s] [--feedback %] [--wet %] [--dry %] [--sr hz]
//                            [--reverser 0|1] [--block n] [--random-blocks]
//   ReversinatorTests screenshot <out.png> [--input in.wav] [--seconds s] [--scale f] [--width px] [--about] [params...]
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
                about->show();

        const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, static_cast<float>(o.number("scale", 2.0)));
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

        // 8. A host reset clears everything that was buffered.
        {
            const double sr = 44100.0;
            auto audio = makeNoise(2, static_cast<int>(sr * 3), 0.5f, 17);
            ReversinatorAudioProcessor p;
            setParam(p, ParamIDs::feedback, 75.0f);
            render(p, audio, sr, 512, false);
            prepare(p, 2, sr, 512);
            p.reset();
            juce::AudioBuffer<float> silence(2, static_cast<int>(sr * 2));
            silence.clear();
            juce::MidiBuffer midi;
            for (int pos = 0; pos < silence.getNumSamples(); pos += 512)
            {
                juce::AudioBuffer<float> block(silence.getArrayOfWritePointers(), 2, pos, 512);
                p.processBlock(block, midi);
            }
            check(silence.getMagnitude(0, silence.getNumSamples()) < 1.0e-6f, "reset() clears buffered audio");
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
