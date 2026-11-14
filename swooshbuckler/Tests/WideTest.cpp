// Wide test: the plug-in carries every room width end to end.
//
//   - Layouts: the output accepts the seven widths as JUCE's own channel sets (Mono, Stereo, Quad, 5.1, the film
//     7.1, 7.1.2, 7.1.4) and nothing else; the app stays Stereo or Mono. In AAX each width gets its own plug-in
//     ID, pinned here (Mono and Stereo keep the IDs they always had).
//   - Loudness weights: ITU-R BS.1770-4's, from where each speaker stands.
//   - The output stage (RoomMaster): the same latency on every channel at every width; the Ceiling held on every
//     channel; the main channels share one gain; the LFE has a limiter of its own (a loud LFE leaves the room
//     alone, and the other way round). Its CPU at 12 channels against 2.
//   - The whole plug-in at each width: no crash, the room's width and meters follow the bus, L and R carry the
//     same sound as the Stereo render (Mono: its fold), every other channel is silent, the latency is the same
//     at every width, rendering above the device rate too, and the tape (still stereo) gets the room folded to
//     Stereo.
//
// Usage: SwooshbucklerWide [scratch folder]. Run it from a scratch folder; nothing is read from or written to the
// user's settings.

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "Room.h"
#include "RoomMaster.h"

#include <chrono>
#include <cmath>
#include <iostream>

namespace
{
    constexpr double c_fs = 48000.0;
    constexpr int c_block = 512;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok)
            ++g_failures;
    }

    float db(float gain) { return juce::Decibels::gainToDecibels(gain, -200.0f); }

    constexpr room::Width c_widths[] = { room::Width::Mono, room::Width::Stereo, room::Width::Quad, room::Width::Surround51,
                                         room::Width::Surround71, room::Width::Surround712, room::Width::Surround714 };

    juce::String nameOf(room::Width w) { return room::layout(w).name; }

    void writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio, double rate)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(file);
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream, rate, static_cast<unsigned int>(audio.getNumChannels()), 24, {}, 0));
        writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    }

    // Two sources: a growl with gaps (bus A) and a row of hits.
    void makeSources(const juce::File& folder)
    {
        juce::Random r(3);
        juce::AudioBuffer<float> growl(1, static_cast<int>(c_fs * 6.0)), hits(1, static_cast<int>(c_fs * 6.0));
        growl.clear();
        hits.clear();
        double ph = 0.0;
        for (int i = 0; i < growl.getNumSamples(); ++i)
        {
            const double t = i / c_fs, lt = std::fmod(t, 1.5);
            if (lt < 0.6)
            {
                ph += (90.0 + 40.0 * std::sin(6.0 * t)) / c_fs;
                float v = 0.0f;
                for (int h = 1; h < 8; ++h)
                    v += static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * ph * h)) / static_cast<float>(h);
                growl.setSample(0, i, 0.5f * static_cast<float>(std::sin(juce::MathConstants<double>::pi * lt / 0.6)) * std::tanh(2.0f * v));
            }
        }
        for (int k = 0; k < 6; ++k)
            for (int i = 0; i < static_cast<int>(0.25 * c_fs); ++i)
                hits.setSample(0, static_cast<int>((0.2 + k) * c_fs) + i,
                               0.9f * std::exp(-static_cast<float>(i) / (0.04f * static_cast<float>(c_fs))) * (r.nextFloat() * 2.0f - 1.0f));
        writeWav(folder.getChildFile("growl.wav"), growl, c_fs);
        writeWav(folder.getChildFile("hits.wav"), hits, c_fs);
    }

    using Proc = SwooshbucklerAudioProcessor;

    juce::AudioProcessor::BusesLayout layoutFor(const juce::AudioChannelSet& out)
    {
        juce::AudioProcessor::BusesLayout layout;
        layout.outputBuses.add(out);
        return layout;
    }

    struct Plug
    {
        std::unique_ptr<juce::AudioProcessor> base { createPluginFilter() };
        Proc& proc { *dynamic_cast<Proc*>(base.get()) };

        Plug(const juce::File& sources, room::Width width, int recordRate)
        {
            const bool set = proc.setBusesLayout(layoutFor(room::channelSet(width)));
            if (! set)
                check(false, "the bus takes " + nameOf(width));
            proc.overrideRecordRate(recordRate);
            const char* names[] = { "growl", "hits" };
            const CargoRole roles[] = { CargoRole::Tonal, CargoRole::Impact };
            for (int t = 0; t < 2; ++t)
            {
                juce::String error;
                if (! proc.getCargo().loadFile(t, sources.getChildFile(juce::String(names[t]) + ".wav"), error))
                    check(false, "loads " + juce::String(names[t]) + ": " + error);
                proc.getCargo().setRoleOverride(t, static_cast<int>(roles[t]));
            }
            proc.prepareToPlay(c_fs, c_block);
        }

        /** Plays for `seconds`, the output in a buffer as wide as the bus. */
        juce::AudioBuffer<float> run(double seconds)
        {
            const int channels = proc.getTotalNumOutputChannels();
            const int blocks = static_cast<int>(seconds * c_fs / c_block);
            juce::AudioBuffer<float> out(channels, blocks * c_block), block(channels, c_block);
            juce::MidiBuffer midi;
            for (int b = 0; b < blocks; ++b)
            {
                block.clear();
                proc.processBlock(block, midi);
                for (int ch = 0; ch < channels; ++ch)
                    out.copyFrom(ch, b * c_block, block, ch, 0, c_block);
            }
            return out;
        }
    };

    float maxAbs(const float* x, int n)
    {
        float m = 0.0f;
        for (int i = 0; i < n; ++i)
            m = std::max(m, std::abs(x[i]));
        return m;
    }

    float maxDiff(const float* a, const float* b, int n)
    {
        float m = 0.0f;
        for (int i = 0; i < n; ++i)
            m = std::max(m, std::abs(a[i] - b[i]));
        return m;
    }

    juce::String fourCC(juce::int32 id)
    {
        juce::String s;
        for (int i = 3; i >= 0; --i)
            s << static_cast<char>((id >> (8 * i)) & 0xff);
        return s;
    }

    // RoomMaster with the plug-in's settings: limiter on, Ceiling -0.3 dB (Push and Inflate as given).
    MasterProcessor::Parameters masterParams(float pushDb = 0.0f, float inflate = 0.0f)
    {
        MasterProcessor::Parameters p;
        p.limiterOn = true;
        p.ceilingDb = -0.3f;
        p.pushDb = pushDb;
        p.inflateEffect = inflate;
        p.releaseMs = 20.0f;
        p.sustainReleaseMs = 1000.0f;
        return p;
    }

    void runMaster(RoomMaster& m, juce::AudioBuffer<float>& audio)
    {
        std::array<float*, room::c_maxChannels> ptrs {};
        for (int pos = 0; pos < audio.getNumSamples(); pos += c_block)
        {
            const int n = std::min(c_block, audio.getNumSamples() - pos);
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                ptrs[static_cast<size_t>(ch)] = audio.getWritePointer(ch) + pos;
            m.process(ptrs.data(), audio.getNumChannels(), n);
        }
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("SwooshbucklerWideTest");
    folder.createDirectory();
    const auto sources = folder.getChildFile("sources");
    sources.createDirectory();
    makeSources(sources);
    const auto tapeFolder = folder.getChildFile("tape");
    tapeFolder.deleteRecursively();
    tapeFolder.createDirectory();

    juce::PropertiesFile::Options options;
    options.applicationName = "SwooshbucklerWideTest";
    options.filenameSuffix = ".settings";
    const auto settingsFile = folder.getChildFile("test.settings");
    settingsFile.deleteFile();
    juce::PropertiesFile settings(settingsFile, options);
    settings.setValue("recordFolder", tapeFolder.getFullPathName());
    Proc::useSettings(&settings);

    //==========================================================================
    std::cout << "Layouts" << std::endl;
    {
        std::unique_ptr<juce::AudioProcessor> base { createPluginFilter() };
        bool all = true;
        for (auto w : c_widths)
            all = all && base->checkBusesLayoutSupported(layoutFor(room::channelSet(w)));
        check(all, "the output takes each of the seven widths as JUCE's own channel set");
        const juce::AudioChannelSet others[] = { juce::AudioChannelSet::createLCR(), juce::AudioChannelSet::create5point0(),
                                                 juce::AudioChannelSet::create7point1SDDS(), juce::AudioChannelSet::create7point0(),
                                                 juce::AudioChannelSet::create5point1point4(), juce::AudioChannelSet::discreteChannels(12),
                                                 juce::AudioChannelSet::discreteChannels(6), juce::AudioChannelSet::ambisonic(1),
                                                 juce::AudioChannelSet::disabled() };
        bool none = true;
        for (const auto& set : others)
            none = none && ! base->checkBusesLayoutSupported(layoutFor(set));
        check(none, "and nothing else: no 5.0, five-front 7.1, 5.1.4, discrete twelve, Ambisonics or no output");
        // A layout an audio input with it is refused (the plug-in has none to give).
        auto withInput = layoutFor(juce::AudioChannelSet::create5point1());
        withInput.inputBuses.add(juce::AudioChannelSet::stereo());
        check(! base->checkBusesLayoutSupported(withInput), "a layout with an audio input is refused");

        // The app: Stereo and Mono only (its device would otherwise be matched to a surround layout unasked).
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
        std::unique_ptr<juce::AudioProcessor> app { createPluginFilter() };
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        bool wide = false;
        for (auto w : c_widths)
            if (w != room::Width::Mono && w != room::Width::Stereo)
                wide = wide || app->checkBusesLayoutSupported(layoutFor(room::channelSet(w)));
        check(app->wrapperType == juce::AudioProcessor::wrapperType_Standalone && ! wide
                  && app->checkBusesLayoutSupported(layoutFor(juce::AudioChannelSet::stereo()))
                  && app->checkBusesLayoutSupported(layoutFor(juce::AudioChannelSet::mono())),
              "the app stays Stereo or Mono");

        // AAX: one plug-in ID per width, from JUCE's fixed table (the plug-in keeps the default extensions).
        // Pinned, so a change of JUCE that would move them (and lose Pro Tools sessions) is caught here.
        const auto& ext = base->getAAXClientExtensions();
        const char* native[] = { "jcab", "jcac", "jcaf", "jcah", "jcal", "jcap", "jcay" };
        const char* suite[] = { "jyab", "jyac", "jyaf", "jyah", "jyal", "jyap", "jyay" };
        juce::StringArray ids;
        bool pinned = true;
        for (size_t i = 0; i < std::size(c_widths); ++i)
        {
            const auto set = room::channelSet(c_widths[i]);
            const auto id = fourCC(ext.getPluginIDForMainBusConfig(juce::AudioChannelSet::disabled(), set, false));
            const auto as = fourCC(ext.getPluginIDForMainBusConfig(juce::AudioChannelSet::disabled(), set, true));
            std::cout << "         " << nameOf(c_widths[i]) << ": AAX " << id << ", AudioSuite " << as << std::endl;
            pinned = pinned && id == native[i] && as == suite[i];
            ids.addIfNotAlreadyThere(id);
            ids.addIfNotAlreadyThere(as);
        }
        check(ids.size() == 14, "AAX: every width has its own plug-in ID, native and AudioSuite");
        check(pinned, "AAX: the IDs are the pinned ones (Mono jcab and Stereo jcac as before)");
    }

    //==========================================================================
    std::cout << "Loudness weights" << std::endl;
    {
        using S = room::Speaker;
        auto near = [](float a, float b) { return std::abs(a - b) < 1.0e-6f; };
        check(near(room::loudnessWeight(S::L), 1.0f) && near(room::loudnessWeight(S::R), 1.0f) && near(room::loudnessWeight(S::C), 1.0f),
              "BS.1770: the fronts count 1");
        check(near(room::loudnessWeight(S::Ls), 1.41f) && near(room::loudnessWeight(S::Rs), 1.41f) && near(room::loudnessWeight(S::Lss), 1.41f)
                  && near(room::loudnessWeight(S::Rss), 1.41f),
              "BS.1770: the surrounds between 60 and 120 degrees (Ls / Rs at 110, the sides at 90) count +1.5 dB");
        check(near(room::loudnessWeight(S::Lsr), 1.0f) && near(room::loudnessWeight(S::Rsr), 1.0f),
              "BS.1770: the rear surrounds at 135 degrees count 1");
        bool tops = true;
        for (auto s : { S::Ltf, S::Rtf, S::Ltr, S::Rtr, S::Ltm, S::Rtm })
            tops = tops && near(room::loudnessWeight(s), 1.0f);
        check(tops, "BS.1770: the tops count 1");
        check(room::loudnessWeight(S::Lfe) == 0.0f, "BS.1770: the LFE is left out");
    }

    //==========================================================================
    std::cout << "The output stage" << std::endl;
    {
        // Latency: an impulse on one channel at a time comes out the same number of samples later on every channel.
        bool sameLatency = true, sameAcross = true;
        int firstLatency = -1;
        for (auto w : c_widths)
        {
            RoomMaster m;
            m.prepare(c_fs, w);
            m.setParameters(masterParams());
            const int channels = RoomMaster::pipeChannels(w);
            if (firstLatency < 0)
                firstLatency = m.getLatencySamples();
            sameAcross = sameAcross && m.getLatencySamples() == firstLatency && m.getNumChannels() == channels;
            for (int ch = 0; ch < channels; ++ch)
            {
                m.reset();
                juce::AudioBuffer<float> a(channels, 4096);
                a.clear();
                a.setSample(ch, 100, 0.5f);
                runMaster(m, a);
                int at = -1;
                float best = 0.0f;
                for (int i = 0; i < a.getNumSamples(); ++i)
                    if (std::abs(a.getSample(ch, i)) > best)
                    {
                        best = std::abs(a.getSample(ch, i));
                        at = i;
                    }
                sameLatency = sameLatency && at - 100 == m.getLatencySamples() && std::abs(best - 0.5f) < 1.0e-6f;
            }
        }
        std::cout << "         latency " << firstLatency << " samples at 48 kHz" << std::endl;
        check(sameLatency, "every channel of every width comes out exactly the stage's latency later, untouched below the Ceiling");
        check(sameAcross, "the latency is the same at every width (Mono carried as Stereo)");

        // The Ceiling on every channel: unrelated noise at +12 dB on all twelve, pushed 6 dB more.
        {
            RoomMaster m;
            m.prepare(c_fs, room::Width::Surround714);
            m.setParameters(masterParams(6.0f, 0.5f));
            juce::Random r(7);
            juce::AudioBuffer<float> a(12, static_cast<int>(c_fs * 2.0));
            for (int ch = 0; ch < 12; ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    a.setSample(ch, i, 4.0f * (r.nextFloat() * 2.0f - 1.0f));
            runMaster(m, a);
            const float ceiling = juce::Decibels::decibelsToGain(-0.3f);
            float worst = 0.0f;
            for (int ch = 0; ch < 12; ++ch)
                worst = std::max(worst, maxAbs(a.getReadPointer(ch), a.getNumSamples()));
            std::cout << "         loudest sample " << juce::String(db(worst), 3) << " dB, Ceiling -0.3 dB" << std::endl;
            check(worst <= ceiling * 1.00001f, "7.1.4: the Ceiling holds on every channel, the LFE too");
        }

        // The LFE's own limiter, at 5.1, 7.1 and 7.1.4.
        for (auto w : { room::Width::Surround51, room::Width::Surround71, room::Width::Surround714 })
        {
            const auto& lay = room::layout(w);
            const int channels = lay.numChannels, lfe = lay.lfe;
            const int len = static_cast<int>(c_fs * 1.5);
            auto sine = [&](int ch, float amp, double hz)
            {
                juce::AudioBuffer<float> a(channels, len);
                a.clear();
                for (int i = 0; i < len; ++i)
                    a.setSample(ch, i, amp * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * hz * i / c_fs)));
                return a;
            };
            // A full-scale-plus LFE with quiet mains: the mains come through untouched.
            {
                RoomMaster m;
                m.prepare(c_fs, w);
                m.setParameters(masterParams());
                auto in = sine(lfe, 4.0f, 50.0);
                for (int ch = 0; ch < channels; ++ch)
                    if (ch != lfe)
                        for (int i = 0; i < len; ++i)
                            in.setSample(ch, i, 0.1f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * (300.0 + 50.0 * ch) * i / c_fs)));
                auto out = in;
                runMaster(m, out);
                const int lat = m.getLatencySamples();
                float mainsChange = 0.0f;
                for (int ch = 0; ch < channels; ++ch)
                    if (ch != lfe)
                        mainsChange = std::max(mainsChange, maxDiff(out.getReadPointer(ch) + lat, in.getReadPointer(ch), len - lat));
                const float lfePeak = maxAbs(out.getReadPointer(lfe), len);
                check(mainsChange < 1.0e-6f && lfePeak <= juce::Decibels::decibelsToGain(-0.3f) * 1.00001f && m.getLfeGainReductionDb() < -6.0f,
                      nameOf(w) + ": a loud LFE is held on its own and leaves the room alone (mains changed by " + juce::String(mainsChange)
                          + ", LFE reduced " + juce::String(m.getLfeGainReductionDb(), 1) + " dB)");
            }
            // Loud mains with a quiet LFE: the LFE comes through untouched, and the mains share one gain.
            {
                RoomMaster m;
                m.prepare(c_fs, w);
                m.setParameters(masterParams());
                auto in = sine(0, 4.0f, 200.0); // L far over the Ceiling
                const int quiet = channels - 1; // the last speaker, quiet
                for (int i = 0; i < len; ++i)
                {
                    in.setSample(lfe, i, 0.2f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 40.0 * i / c_fs)));
                    in.setSample(quiet, i, 0.2f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 500.0 * i / c_fs)));
                }
                auto out = in;
                runMaster(m, out);
                const int lat = m.getLatencySamples();
                const float lfeChange = maxDiff(out.getReadPointer(lfe) + lat, in.getReadPointer(lfe), len - lat);
                // Linked: in the second half, the quiet speaker is turned down as much as L is.
                const int from = len / 2;
                const float lRatio = maxAbs(out.getReadPointer(0) + from + lat, len - from - lat) / maxAbs(in.getReadPointer(0) + from, len - from - lat);
                const float qRatio = maxAbs(out.getReadPointer(quiet) + from + lat, len - from - lat) / maxAbs(in.getReadPointer(quiet) + from, len - from - lat);
                check(lfeChange < 1.0e-6f && std::abs(db(lRatio) - db(qRatio)) < 0.5f && db(qRatio) < -10.0f,
                      nameOf(w) + ": loud mains leave the LFE alone (changed by " + juce::String(lfeChange) + ") and share one gain (L "
                          + juce::String(db(lRatio), 1) + " dB, " + room::info(lay.speakers[static_cast<size_t>(quiet)]).name + " "
                          + juce::String(db(qRatio), 1) + " dB)");
            }
        }

        // CPU: the stage at 2 channels against 12, Inflate on and the limiter working, 48 and 192 kHz.
        for (double rate : { 48000.0, 192000.0 })
        {
            double ms[2] {};
            for (int k = 0; k < 2; ++k)
            {
                const auto w = k == 0 ? room::Width::Stereo : room::Width::Surround714;
                const int channels = RoomMaster::pipeChannels(w);
                RoomMaster m;
                m.prepare(rate, w);
                m.setParameters(masterParams(6.0f, 0.5f));
                juce::Random r(11);
                juce::AudioBuffer<float> a(channels, static_cast<int>(rate * 10.0));
                for (int ch = 0; ch < channels; ++ch)
                    for (int i = 0; i < a.getNumSamples(); ++i)
                        a.setSample(ch, i, 0.8f * (r.nextFloat() * 2.0f - 1.0f));
                const auto t0 = std::chrono::steady_clock::now();
                runMaster(m, a);
                ms[k] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            std::cout << "         CPU, 10 s at " << rate / 1000.0 << " kHz: Stereo " << juce::String(ms[0], 1) << " ms ("
                      << juce::String(ms[0] / 100.0, 2) << "% of a core), 7.1.4 " << juce::String(ms[1], 1) << " ms ("
                      << juce::String(ms[1] / 100.0, 2) << "% of a core), " << juce::String(ms[1] / ms[0], 2) << "x" << std::endl;
            check(ms[1] < 10000.0, "the stage runs faster than real time at 12 channels, " + juce::String(rate / 1000.0) + " kHz");
        }
    }

    //==========================================================================
    std::cout << "The plug-in at each width" << std::endl;
    for (int recordRate : { 0, 96000 })
    {
        const juce::String at = recordRate > 0 ? " (rendered at 96 kHz)" : "";
        Plug stereoPlug(sources, room::Width::Stereo, recordRate);
        stereoPlug.proc.setPlaying(true);
        const auto reference = stereoPlug.run(4.0);
        const int len = reference.getNumSamples();
        const float refPeak = std::max(maxAbs(reference.getReadPointer(0), len), maxAbs(reference.getReadPointer(1), len));
        const int refLatency = stereoPlug.proc.getLatencySamples();
        std::cout << "         Stereo" << at << ": peak " << juce::String(db(refPeak), 1) << " dB, latency " << refLatency << std::endl;
        check(refPeak > 0.05f, "the Stereo reference plays" + at);

        for (auto w : c_widths)
        {
            if (w == room::Width::Stereo)
                continue;
            const auto& lay = room::layout(w);
            Plug plug(sources, w, recordRate);
            auto& p = plug.proc;
            p.setPlaying(true);
            const auto out = plug.run(4.0);
            bool finite = true;
            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                for (int i = 0; i < len; ++i)
                    finite = finite && std::isfinite(out.getSample(ch, i));
            const bool shaped = out.getNumChannels() == lay.numChannels && p.getRoomWidth() == w && p.getMeterChannels() == lay.numChannels;
            float lr = 0.0f, others = 0.0f;
            if (w == room::Width::Mono)
            {
                std::vector<float> folded(static_cast<size_t>(len));
                for (int i = 0; i < len; ++i)
                    folded[static_cast<size_t>(i)] = 0.5f * (reference.getSample(0, i) + reference.getSample(1, i));
                lr = maxDiff(out.getReadPointer(0), folded.data(), len);
            }
            else
            {
                lr = std::max(maxDiff(out.getReadPointer(0), reference.getReadPointer(0), len), maxDiff(out.getReadPointer(1), reference.getReadPointer(1), len));
                for (int ch = 2; ch < out.getNumChannels(); ++ch)
                    others = std::max(others, maxAbs(out.getReadPointer(ch), len));
            }
            // The speaker meters: L and R (Mono's one) show the sound, the rest nothing.
            bool meters = p.getChannelPeak(0) > 0.0f && p.takeChannelPeak(0) > 0.0f;
            for (int ch = 1; ch < room::c_maxChannels; ++ch)
            {
                const bool sounding = ch == 1 && w != room::Width::Mono;
                meters = meters && (sounding ? p.takeChannelPeak(ch) > 0.0f : p.takeChannelPeak(ch) == 0.0f && p.getChannelPeak(ch) == 0.0f);
            }
            std::cout << "         " << nameOf(w) << at << ": " << out.getNumChannels() << " channels, L / R against Stereo "
                      << juce::String(db(lr), 1) << " dB, the others " << juce::String(db(others), 1) << " dB, latency " << p.getLatencySamples() << std::endl;
            check(finite && shaped, nameOf(w) + at + ": renders, and the room width and the meters follow the bus");
            check(lr < 1.0e-5f, nameOf(w) + at + (w == room::Width::Mono ? ": the one channel is the Stereo render folded" : ": L and R carry the Stereo render"));
            check(others == 0.0f, nameOf(w) + at + ": every other channel is silent");
            check(p.getLatencySamples() == refLatency, nameOf(w) + at + ": the same latency as Stereo");
            check(meters, nameOf(w) + at + ": the speaker meters show L and R sounding and the rest silent");
        }
    }

    //==========================================================================
    std::cout << "The tape" << std::endl;
    {
        // The tape is stereo for now: a 7.1.4 room reaches it folded to Stereo, which (with the sound in L and R
        // alone) is L and R as they are.
        Plug plug(sources, room::Width::Surround714, 0);
        auto& p = plug.proc;
        p.setSkipSilence(false);
        p.setBitDepth(32);
        juce::String error;
        check(p.arm(error), "the tape arms at 7.1.4 " + error);
        p.setPlaying(true);
        const auto out = plug.run(3.0);
        p.stopRecording();
        juce::AudioBuffer<float> file;
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            for (int wait = 0; wait < 200 && p.isRecording(); ++wait)
                juce::Thread::sleep(10);
            std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(p.getLastRecording()));
            if (r != nullptr)
            {
                file.setSize(static_cast<int>(r->numChannels), static_cast<int>(r->lengthInSamples));
                r->read(&file, 0, file.getNumSamples(), 0, true, true);
            }
        }
        // Line the file up with the output (the take starts where the tape was armed, near the start).
        float best = 1.0f;
        if (file.getNumChannels() == 2 && file.getNumSamples() > 48000)
        {
            const int n = 48000;
            for (int lag = -4096; lag <= 4096; ++lag)
            {
                const int o0 = 48000 + lag; // output samples from 1 s
                const int f0 = 48000;
                if (o0 < 0 || o0 + n > out.getNumSamples() || f0 + n > file.getNumSamples())
                    continue;
                const float d = std::max(maxDiff(out.getReadPointer(0) + o0, file.getReadPointer(0) + f0, n),
                                         maxDiff(out.getReadPointer(1) + o0, file.getReadPointer(1) + f0, n));
                best = std::min(best, d);
            }
        }
        std::cout << "         take: " << file.getNumChannels() << " channels, " << file.getNumSamples() << " samples, against the output "
                  << juce::String(db(best), 1) << " dB" << std::endl;
        check(file.getNumChannels() == 2 && file.getNumSamples() > 48000 * 2 && best < 1.0e-5f,
              "7.1.4: the tape records the room folded to Stereo (L and R as they are played)");
    }

    Proc::useSettings(nullptr);
    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
