// Wide test: the plug-in carries every room width end to end.
//
//   - Layouts: the output accepts the seven widths as JUCE's own channel sets (Mono, Stereo, Quad, 5.1, the film
//     7.1, 7.1.2, 7.1.4) and nothing else; the app takes its Listening width only (Stereo when new). In AAX each width gets its own plug-in
//     ID, pinned here (Mono and Stereo keep the IDs they always had).
//   - Loudness weights: ITU-R BS.1770-4's, from where each speaker stands.
//   - The output stage (RoomMaster): the same latency on every channel at every width; the Ceiling held on every
//     channel; the main channels share one gain; the LFE has a limiter of its own (a loud LFE leaves the room
//     alone, and the other way round). Its CPU at 12 channels against 2.
//   - The whole plug-in at each width: no crash, the room's width and meters follow the bus, Mono is the Stereo
//     render folded, and from Quad up (where every pass is an object on the speakers) every main speaker sounds,
//     the LFE is silent, the Ceiling holds on every speaker, the room is about as loud as the Stereo render, the
//     latency is the same at every width, rendering above the device rate too, and the tape records the room at
//     its width: every channel of a 7.1.4 take, read back in the room's order, is the output's. What the whole
//     plug-in costs at 7.1.4 against Stereo, rendering at 48, 96 and 192 kHz.
//   - Block sizes: the same samples whatever sizes of block the host asks for (1, 7, 32, 37, 64, 441, a first block
//     of 7, random sizes), in Stereo, at 7.1.4 and rendering above the device rate; small blocks cost no more.
//   - Direction on a wire: a square LFO wired to a bus's Direction sets each pass's way as the pass starts.
//   - The Output EQ in the plug-in: with +12 dB on all four gain bands the Ceiling still holds on every speaker at
//     every width (the EQ is before the limiters), the latency is what it was, and the mains and the LFE get the same
//     curve: through a 24 dB an octave Low Cut at 80 Hz the LFE stays in step with the mains. What every band costs
//     in the whole plug-in at 7.1.4, rendering at 192 kHz.
//
// Usage: SwooshbucklerWide [scratch folder]. Run it from a scratch folder; nothing is read from or written to the
// user's settings.

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "Room.h"
#include "RoomMaster.h"
#include "TakeFile.h"
#include "TestClock.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <functional>
#include <iostream>
#include <vector>

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
            // In a room: bus A's passes fly Overhead (so the tops sound where there are any), everything bleeds a
            // little into every speaker of its layer, and the centre speaker is in use: every main speaker sounds.
            setPlain(ParamIDs::flightPath[0], 2.0f);
            setPlain(ParamIDs::roomSpread, 0.3f);
            setPlain(ParamIDs::roomCentre, 1.0f);
            proc.prepareToPlay(c_fs, c_block);
        }

        void setPlain(const char* id, float plain)
        {
            auto* param = proc.getState().getParameter(id);
            param->setValueNotifyingHost(param->convertTo0to1(plain));
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

        // The app: its Listening width only, Stereo on a fresh install (AppTest goes through the others).
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
        std::unique_ptr<juce::AudioProcessor> app { createPluginFilter() };
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        bool wide = false;
        for (auto w : c_widths)
            if (w != room::Width::Mono && w != room::Width::Stereo)
                wide = wide || app->checkBusesLayoutSupported(layoutFor(room::channelSet(w)));
        check(app->wrapperType == juce::AudioProcessor::wrapperType_Standalone && ! wide
                  && app->checkBusesLayoutSupported(layoutFor(juce::AudioChannelSet::stereo()))
                  && ! app->checkBusesLayoutSupported(layoutFor(juce::AudioChannelSet::mono())),
              "the app takes its Listening width only: Stereo, as a fresh install has it");

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
        auto nearly = [](float a, float b) { return std::abs(a - b) < 1.0e-6f; };
        check(nearly(room::loudnessWeight(S::L), 1.0f) && nearly(room::loudnessWeight(S::R), 1.0f) && nearly(room::loudnessWeight(S::C), 1.0f),
              "BS.1770: the fronts count 1");
        check(nearly(room::loudnessWeight(S::Ls), 1.41f) && nearly(room::loudnessWeight(S::Rs), 1.41f) && nearly(room::loudnessWeight(S::Lss), 1.41f)
                  && nearly(room::loudnessWeight(S::Rss), 1.41f),
              "BS.1770: the surrounds between 60 and 120 degrees (Ls / Rs at 110, the sides at 90) count +1.5 dB");
        check(nearly(room::loudnessWeight(S::Lsr), 1.0f) && nearly(room::loudnessWeight(S::Rsr), 1.0f),
              "BS.1770: the rear surrounds at 135 degrees count 1");
        bool tops = true;
        for (auto s : { S::Ltf, S::Rtf, S::Ltr, S::Rtr, S::Lts, S::Rts })
            tops = tops && nearly(room::loudnessWeight(s), 1.0f);
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
            check(finite && shaped, nameOf(w) + at + ": renders, and the room width and the meters follow the bus");
            check(p.getLatencySamples() == refLatency, nameOf(w) + at + ": the same latency as Stereo");
            if (w == room::Width::Mono)
            {
                // Mono is the stereo engine, folded.
                std::vector<float> folded(static_cast<size_t>(len));
                for (int i = 0; i < len; ++i)
                    folded[static_cast<size_t>(i)] = 0.5f * (reference.getSample(0, i) + reference.getSample(1, i));
                const float apart = maxDiff(out.getReadPointer(0), folded.data(), len);
                bool meters = p.getChannelPeak(0) > 0.0f && p.takeChannelPeak(0) > 0.0f;
                for (int ch = 1; ch < room::c_maxChannels; ++ch)
                    meters = meters && p.takeChannelPeak(ch) == 0.0f && p.getChannelPeak(ch) == 0.0f;
                std::cout << "         Mono" << at << ": against the Stereo render folded " << juce::String(db(apart), 1) << " dB" << std::endl;
                check(apart < 1.0e-5f, "Mono" + at + ": the one channel is the Stereo render folded");
                check(meters, "Mono" + at + ": the one speaker meter shows the sound");
                continue;
            }
            // A room: every pass and hit is an object on the speakers (the Surround test says where). Here: every
            // main speaker sounds, the LFE is silent, the Ceiling holds on every channel after the two limiters,
            // the meters show it, and the room is about as loud as the Stereo render of the same session.
            float quietest = 1.0f, loudest = 0.0f, lfe = 0.0f;
            double power = 0.0, refPower = 0.0;
            bool meters = true;
            for (int ch = 0; ch < room::c_maxChannels; ++ch)
            {
                const float meterNow = p.getChannelPeak(ch), meterHeld = p.takeChannelPeak(ch);
                if (ch >= lay.numChannels)
                {
                    meters = meters && meterNow == 0.0f && meterHeld == 0.0f;
                    continue;
                }
                const float peak = maxAbs(out.getReadPointer(ch), len);
                if (ch == lay.lfe)
                {
                    lfe = peak;
                    meters = meters && meterNow == 0.0f && meterHeld == 0.0f;
                    continue;
                }
                quietest = std::min(quietest, peak);
                loudest = std::max(loudest, peak);
                meters = meters && meterHeld > 0.0f;
                for (int i = 0; i < len; ++i)
                    power += static_cast<double>(out.getSample(ch, i)) * out.getSample(ch, i);
            }
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < len; ++i)
                    refPower += static_cast<double>(reference.getSample(ch, i)) * reference.getSample(ch, i);
            const float against = static_cast<float>(10.0 * std::log10(power / refPower));
            std::cout << "         " << nameOf(w) << at << ": " << out.getNumChannels() << " channels, the loudest speaker peaks at "
                      << juce::String(db(loudest), 2) << " dB, the quietest at " << juce::String(db(quietest), 1) << " dB, the LFE at " << juce::String(db(lfe), 1)
                      << " dB; all speakers together " << juce::String(against, 1) << " dB against the Stereo render; latency " << p.getLatencySamples() << std::endl;
            check(quietest > 1.0e-3f, nameOf(w) + at + ": every main speaker sounds");
            check(lfe == 0.0f, nameOf(w) + at + ": the LFE is silent");
            check(loudest <= juce::Decibels::decibelsToGain(-0.3f) * 1.0001f, nameOf(w) + at + ": the Ceiling holds on every speaker");
            check(std::abs(against) < 3.0f, nameOf(w) + at + ": the room is about as loud as the Stereo render (within 3 dB)");
            check(meters, nameOf(w) + at + ": the speaker meters show every main speaker sounding, and nothing on the LFE or past the width");
        }
    }

    //==========================================================================
    std::cout << "The tape" << std::endl;
    {
        // The tape records the room at its width: a 7.1.4 take has twelve channels, and read back (in the room's
        // order, whatever order the file has them in) each is the output's channel as it was played.
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
            for (int wait = 0; wait < 200 && p.isRecording(); ++wait)
                juce::Thread::sleep(10);
            const auto r = takefile::openReader(p.getLastRecording());
            if (r != nullptr)
            {
                file.setSize(static_cast<int>(r->numChannels), static_cast<int>(r->lengthInSamples));
                r->read(&file, 0, file.getNumSamples(), 0, true, true);
            }
        }
        // Line the file up with the output (the take starts where the tape was armed, near the start).
        float best = 1.0f;
        if (file.getNumChannels() == 12 && out.getNumChannels() == 12 && file.getNumSamples() > 48000)
        {
            const int n = 48000;
            for (int lag = -4096; lag <= 4096; ++lag)
            {
                const int o0 = 48000 + lag; // output samples from 1 s
                const int f0 = 48000;
                if (o0 < 0 || o0 + n > out.getNumSamples() || f0 + n > file.getNumSamples())
                    continue;
                float d = 0.0f;
                for (int ch = 0; ch < 12; ++ch)
                    d = std::max(d, maxDiff(out.getReadPointer(ch) + o0, file.getReadPointer(ch) + f0, n));
                best = std::min(best, d);
            }
        }
        int sounding = 0;
        for (int ch = 0; ch < file.getNumChannels(); ++ch)
            if (maxAbs(file.getReadPointer(ch), file.getNumSamples()) > 1.0e-3f)
                ++sounding;
        std::cout << "         take: " << file.getNumChannels() << " channels (" << sounding << " sounding), " << file.getNumSamples() << " samples, against the output "
                  << juce::String(db(best), 1) << " dB" << std::endl;
        check(file.getNumChannels() == 12 && file.getNumSamples() > 48000 * 2 && best < 1.0e-5f && sounding == 11,
              "7.1.4: the tape records all twelve channels, each as it is played (eleven sounding, the LFE silent)");
    }

    //==========================================================================
    std::cout << "Block sizes: the sound does not depend on how the host cuts it up" << std::endl;
    {
        // The rig playing, a modulator at work on it (so the parameters move), a whoosh thrown at the start: the
        // same three seconds whatever the sizes of the blocks the host asks for.
        const auto render = [&](room::Width width, int recordRate, const std::function<int(int)>& sizeOf, double* cpuPercent = nullptr)
        {
            Plug plug(sources, width, recordRate);
            plug.proc.getMods().connect(0, "aWidth", 0.8f);
            plug.proc.getMods().connect(2, ParamIDs::space, 0.5f);
            plug.proc.setPlaying(true);
            plug.proc.throwWhoosh();
            const int channels = plug.proc.getTotalNumOutputChannels();
            const int total = static_cast<int>(3.0 * c_fs);
            juce::AudioBuffer<float> out(channels, total);
            juce::AudioBuffer<float> block(channels, 1024);
            juce::MidiBuffer midi;
            const double t0 = TestClock::cpuMs();
            for (int pos = 0, b = 0; pos < total; ++b)
            {
                const int n = juce::jlimit(1, juce::jmin(1024, total - pos), sizeOf(b));
                juce::AudioBuffer<float> part(block.getArrayOfWritePointers(), channels, n);
                part.clear();
                plug.proc.processBlock(part, midi);
                for (int ch = 0; ch < channels; ++ch)
                    out.copyFrom(ch, pos, part, ch, 0, n);
                pos += n;
            }
            if (cpuPercent != nullptr)
                *cpuPercent = 100.0 * (TestClock::cpuMs() - t0) / 3000.0;
            return out;
        };
        struct Pattern { const char* name; std::function<int(int)> sizeOf; };
        juce::Random pick(77);
        std::vector<int> randomSizes;
        for (int i = 0; i < 100000; ++i)
            randomSizes.push_back(1 + pick.nextInt(600));
        const std::vector<Pattern> patterns {
            { "1", [](int) { return 1; } }, { "7", [](int) { return 7; } }, { "32", [](int) { return 32; } }, { "37", [](int) { return 37; } },
            { "64", [](int) { return 64; } }, { "441", [](int) { return 441; } }, { "one of 7 then 512s", [](int b) { return b == 0 ? 7 : 512; } },
            { "random, 1 to 600", [&randomSizes](int b) { return randomSizes[static_cast<size_t>(b) % randomSizes.size()]; } },
        };
        struct Case { room::Width width; int recordRate; };
        for (const auto& c : { Case { room::Width::Stereo, 0 }, Case { room::Width::Surround714, 0 }, Case { room::Width::Surround51, 96000 } })
        {
            const auto reference = render(c.width, c.recordRate, [](int) { return 512; });
            float worst = 0.0f, peak = 0.0f;
            juce::String worstWhere;
            for (int ch = 0; ch < reference.getNumChannels(); ++ch)
                peak = std::max(peak, maxAbs(reference.getReadPointer(ch), reference.getNumSamples()));
            for (const auto& pattern : patterns)
            {
                const auto out = render(c.width, c.recordRate, pattern.sizeOf);
                for (int ch = 0; ch < reference.getNumChannels(); ++ch)
                    if (const float d = maxDiff(out.getReadPointer(ch), reference.getReadPointer(ch), reference.getNumSamples()); d > worst)
                    {
                        worst = d;
                        worstWhere = pattern.name;
                    }
            }
            check(worst == 0.0f && peak > 0.05f,
                  nameOf(c.width) + (c.recordRate > 0 ? " rendering at 96 kHz" : "") + ": blocks of 1, 7, 32, 37, 64, 441, a first block of 7 and random sizes give the samples blocks of 512 give"
                      + (worst > 0.0f ? " (apart by " + juce::String(db(worst), 1) + " dB with blocks of " + worstWhere + ")" : juce::String(" (exactly)")));
        }
        // The host's transport starting a rig that is already playing by hand (Follow Host): the ride starts again
        // from its seed at the first sample of that block, whatever was left over of the block before it.
        {
            struct Transport final : juce::AudioPlayHead
            {
                bool playing = false;
                juce::Optional<PositionInfo> getPosition() const override
                {
                    PositionInfo info;
                    info.setIsPlaying(playing);
                    return info;
                }
            };
            const auto started = [&](int blockBefore)
            {
                Plug plug(sources, room::Width::Stereo, 0);
                Transport transport;
                plug.proc.setPlayHead(&transport);
                plug.proc.setPlaying(true);
                juce::AudioBuffer<float> block(2, 1024);
                juce::MidiBuffer midi;
                for (int k = 0; k < 41; ++k) // most of half a second by hand, in blocks of this size
                {
                    juce::AudioBuffer<float> part(block.getArrayOfWritePointers(), 2, blockBefore);
                    part.clear();
                    plug.proc.processBlock(part, midi);
                }
                transport.playing = true;
                const int total = static_cast<int>(c_fs);
                juce::AudioBuffer<float> out(2, total);
                for (int pos = 0; pos < total;)
                {
                    const int n = juce::jmin(512, total - pos);
                    juce::AudioBuffer<float> part(out.getArrayOfWritePointers(), 2, pos, n);
                    part.clear();
                    plug.proc.processBlock(part, midi);
                    pos += n;
                }
                plug.proc.setPlayHead(nullptr);
                return out;
            };
            // Where the ride's first sound is: the same sample after blocks of 512 (nothing left over) and after
            // blocks of 500 (the last one leaves 12 samples of a quantum behind).
            const auto firstSound = [](const juce::AudioBuffer<float>& b)
            {
                for (int i = 0; i < b.getNumSamples(); ++i)
                    if (std::abs(b.getSample(0, i)) > 1.0e-3f || std::abs(b.getSample(1, i)) > 1.0e-3f)
                        return i;
                return -1;
            };
            const auto even = started(512), odd = started(500);
            // The best lag between the two rides over their first half second.
            int bestLag = 99;
            double bestDiff = 1.0e30;
            const int from = 4800, n = 24000;
            for (int lag = -40; lag <= 40; ++lag)
            {
                double d = 0.0;
                for (int i = from; i < from + n; ++i)
                {
                    const double x = static_cast<double>(even.getSample(0, i)) - odd.getSample(0, i + lag);
                    d += x * x;
                }
                if (d < bestDiff)
                {
                    bestDiff = d;
                    bestLag = lag;
                }
            }
            std::cout << "         the transport starts a rig playing by hand: first sound at sample " << firstSound(even) << " after blocks of 512, "
                      << firstSound(odd) << " after blocks of 500; the two rides line up " << bestLag << " samples apart" << std::endl;
            check(bestLag == 0 && firstSound(even) >= 0,
                  "the host's transport starts the ride at its own first sample, whatever was left of the block before (the two starts " + juce::String(bestLag) + " samples apart)");
        }

        // And small blocks cost no more: the rig is rendered 32 samples at a time whatever the host asks for.
        double cost[4] = { 0.0, 0.0, 0.0, 0.0 };
        const int sizes[4] = { 512, 64, 32, 16 };
        for (int k = 0; k < 4; ++k)
        {
            double best = 1.0e9;
            for (int rep = 0; rep < 3; ++rep)
            {
                double percent = 0.0;
                const int size = sizes[k];
                render(room::Width::Surround714, 0, [size](int) { return size; }, &percent);
                best = std::min(best, percent);
            }
            cost[k] = best;
        }
        std::cout << "         7.1.4, of a core: blocks of 512 " << juce::String(cost[0], 2) << "%, 64 " << juce::String(cost[1], 2) << "%, 32 " << juce::String(cost[2], 2) << "%, 16 "
                  << juce::String(cost[3], 2) << "%" << std::endl;
        check(cost[3] < 1.25 * cost[0] + 0.5, "blocks of 16 cost about what blocks of 512 do (no more than a quarter over)");
    }

    //==========================================================================
    std::cout << "Direction on a wire: a modulator decides which way each pass flies, as the pass starts" << std::endl;
    {
        // A square LFO wired to bus A's Direction, in Stereo (where Direction is left to right or right to left):
        // each pass flies the way the LFO stood when it started, forwards over its middle and backwards under it.
        Plug plug(sources, room::Width::Stereo, 0);
        plug.setPlain(ParamIDs::modType[0], 0.0f);  // LFO
        plug.setPlain(ParamIDs::modShape[0], 3.0f); // Square
        plug.setPlain(ParamIDs::modKnob[0], 0.12f); // slow: a few passes to each half
        plug.setPlain(ParamIDs::direction[0], 2.0f); // (the wire wins over the menu)
        plug.setPlain(ParamIDs::passMode[0], 1.0f);
        plug.proc.getMods().connect(0, ParamIDs::direction[0], 1.0f);
        plug.proc.setPlaying(true);
        juce::AudioBuffer<float> block(2, c_block);
        juce::MidiBuffer midi;
        int passes = 0, right = 0, forwards = 0, backwards = 0;
        float lastAge[2] = { 1.0f, 1.0f }, lfoBefore = 0.0f;
        int started[2] = { 0, 0 }; // how the LFO stood when the head's pass started (+1 / -1), 0 once it has been read
        for (int b = 0; b < static_cast<int>(90.0 * c_fs / c_block); ++b)
        {
            block.clear();
            plug.proc.processBlock(block, midi);
            const float lfo = plug.proc.getMods().getSourceValue(0);
            for (int h = 0; h < 2; ++h)
            {
                const auto view = plug.proc.getEngine().getFlightView(h);
                if (view.age < lastAge[h] - 0.5f) // a new pass (only counted if the LFO did not turn over just then)
                    started[h] = lfo * lfoBefore > 0.0f ? (lfo > 0.0f ? 1 : -1) : 0;
                if (started[h] != 0 && view.age > 0.15f && view.age < 0.35f)
                {
                    // On its way in a pass that flies left to right is on the left.
                    const int way = view.x < 0.0f ? 1 : -1;
                    ++passes;
                    right += way == started[h] ? 1 : 0;
                    (way > 0 ? forwards : backwards) += 1;
                    started[h] = 0;
                }
                lastAge[h] = view.age;
            }
            lfoBefore = lfo;
        }
        check(passes >= 12 && right == passes && forwards >= 3 && backwards >= 3,
              "a square LFO wired to Direction: " + juce::String(right) + " of " + juce::String(passes) + " passes fly the way it stood as they started (" + juce::String(forwards)
                  + " left to right, " + juce::String(backwards) + " right to left)");
        check(ModMatrix::targetIndex(ParamIDs::direction[0]) >= 0 && ModMatrix::targetIndex(ParamIDs::direction[1]) >= 0 && ModMatrix::targetIndex(ParamIDs::stowDirection) >= 0
                  && ModMatrix::targetIndex(ParamIDs::side[0]) >= 0 && ModMatrix::targetIndex(ParamIDs::side[1]) >= 0 && ModMatrix::targetIndex(ParamIDs::stowSide) >= 0
                  && ModMatrix::targetIndex(ParamIDs::roomHeight) < ModMatrix::targetIndex(ParamIDs::direction[0]),
              "Direction and Side of bus A, bus B and Stowaway are modulation targets (added at the end of the list)");
    }

    //==========================================================================
    std::cout << "The Output EQ in the plug-in" << std::endl;
    {
        auto boostAll = [](Plug& plug) {
            for (const char* id : { ParamIDs::eqLowShelfGain, ParamIDs::eqLowMidGain, ParamIDs::eqHighMidGain, ParamIDs::eqHighShelfGain })
                plug.setPlain(id, 12.0f);
        };
        // +12 dB on every gain band at every width: the Ceiling holds on every speaker (the LFE too, playing the low
        // end of the mix), and the latency is what it is with the EQ flat.
        const float ceiling = juce::Decibels::decibelsToGain(-0.3f) * 1.0001f;
        for (int recordRate : { 0, 96000 })
            for (auto w : c_widths)
            {
                if (recordRate > 0 && w != room::Width::Stereo && w != room::Width::Surround714)
                    continue;
                const juce::String at = nameOf(w) + (recordRate > 0 ? " (rendered at 96 kHz)" : "");
                Plug flat(sources, w, recordRate), boosted(sources, w, recordRate);
                for (auto* plug : { &flat, &boosted })
                    plug->setPlain(ParamIDs::roomLfeMode, 2.0f); // Mixdown (it does nothing under 5.1)
                boostAll(boosted);
                flat.proc.setPlaying(true);
                boosted.proc.setPlaying(true);
                const auto a = flat.run(4.0), b = boosted.run(4.0);
                float loudest = 0.0f, loudestFlat = 0.0f, lfe = 0.0f, apart = 0.0f;
                bool finite = true;
                const int lfeChannel = room::layout(w).lfe;
                for (int ch = 0; ch < b.getNumChannels(); ++ch)
                {
                    const float peak = maxAbs(b.getReadPointer(ch), b.getNumSamples());
                    finite = finite && std::isfinite(peak);
                    loudest = std::max(loudest, peak);
                    loudestFlat = std::max(loudestFlat, maxAbs(a.getReadPointer(ch), a.getNumSamples()));
                    if (ch == lfeChannel)
                        lfe = peak;
                    apart = std::max(apart, maxDiff(a.getReadPointer(ch), b.getReadPointer(ch), b.getNumSamples()));
                }
                std::cout << "         " << at << ", +12 dB on four bands: the loudest speaker peaks at " << juce::String(db(loudest), 2) << " dB (flat "
                          << juce::String(db(loudestFlat), 2) << " dB)" << (lfeChannel >= 0 ? ", the LFE at " + juce::String(db(lfe), 2) + " dB" : juce::String())
                          << "; latency " << boosted.proc.getLatencySamples() << " (flat " << flat.proc.getLatencySamples() << ")" << std::endl;
                check(finite && apart > 0.01f, at + ": the EQ is heard");
                check(loudest <= ceiling, at + ": the Ceiling holds on every speaker with +12 dB on four bands");
                check(boosted.proc.getLatencySamples() == flat.proc.getLatencySamples(), at + ": the EQ adds no latency");
                check(boosted.proc.isOutputEqActive() && ! flat.proc.isOutputEqActive(), at + ": the plug-in says whether the EQ is doing anything");
            }

        // The mains and the LFE through a 24 dB an octave Low Cut at 80 Hz, at 5.1 and 7.1.4 with the LFE on Mixdown
        // (the low end of the mains, under 120 Hz). Around its corner such a cut turns the phase by half a turn; one
        // curve on every channel turns the mains and the LFE alike, so the LFE against the mains is where it was
        // with the EQ flat. (The limiters work between here and the EQ, so this is held in angle, not to the sample.)
        for (auto w : { room::Width::Surround51, room::Width::Surround714 })
        {
            auto render = [&](bool cut) {
                Plug plug(sources, w, 0);
                plug.setPlain(ParamIDs::roomLfeMode, 2.0f);
                if (cut)
                {
                    plug.setPlain(ParamIDs::eqLowCut, 80.0f);
                    plug.setPlain(ParamIDs::eqLowCutSlope, 1.0f);
                }
                plug.proc.setPlaying(true);
                return plug.run(8.0);
            };
            const auto flat = render(false), cut = render(true);
            const int lfeChannel = room::layout(w).lfe, len = flat.getNumSamples();
            // The LFE against the mains summed, at one frequency: the cross-spectrum over half-second Hann frames.
            auto against = [&](const juce::AudioBuffer<float>& out, double hz, double& lfePower, double& mainsPower) {
                const int frame = 24000;
                std::complex<double> cross = 0.0;
                lfePower = mainsPower = 0.0;
                for (int from = 24000; from + frame <= len; from += frame / 2) // (past the start, where the EQ glides in)
                {
                    std::complex<double> l = 0.0, m = 0.0;
                    for (int i = 0; i < frame; ++i)
                    {
                        const double window = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * (i + 0.5) / frame);
                        const auto turn = std::polar(window, -juce::MathConstants<double>::twoPi * hz * i / c_fs);
                        double mains = 0.0;
                        for (int ch = 0; ch < out.getNumChannels(); ++ch)
                            if (ch != lfeChannel)
                                mains += out.getSample(ch, from + i);
                        l += static_cast<double>(out.getSample(lfeChannel, from + i)) * turn;
                        m += mains * turn;
                    }
                    cross += l * std::conj(m);
                    lfePower += std::norm(l);
                    mainsPower += std::norm(m);
                }
                return cross;
            };
            double worstAngle = 0.0, worstLevel = 0.0;
            juce::String line;
            for (double hz : { 60.0, 80.0, 100.0 })
            {
                double lf = 0.0, mf = 0.0, lc = 0.0, mc = 0.0;
                const auto flatCross = against(flat, hz, lf, mf), cutCross = against(cut, hz, lc, mc);
                const double angle = std::abs(std::arg(cutCross * std::conj(flatCross))) * 180.0 / juce::MathConstants<double>::pi;
                // How far the cut took the LFE down against how far it took the mains down.
                const double level = 10.0 * std::log10((lc / lf) / (mc / mf));
                const double cutDb = 10.0 * std::log10(mc / mf);
                worstAngle = std::max(worstAngle, angle);
                worstLevel = std::max(worstLevel, std::abs(level));
                line << " " << juce::String(hz, 0) << " Hz: mains " << juce::String(cutDb, 1) << " dB, the LFE " << juce::String(level, 2) << " dB from that and "
                     << juce::String(angle, 1) << " degrees from where it was;";
            }
            std::cout << "         " << nameOf(w) << ", Low Cut 80 Hz 24 dB:" << line << std::endl;
            check(worstAngle < 10.0, nameOf(w) + ": the LFE stays in step with the mains through the Low Cut (within 10 degrees at 60, 80 and 100 Hz)");
            check(worstLevel < 1.5, nameOf(w) + ": the Low Cut takes the LFE down as far as the mains (within 1.5 dB)");
        }
    }

    //==========================================================================
    std::cout << "Speed: the whole plug-in at 7.1.4 against Stereo" << std::endl;
    {
        // Engine, output stage and, rendering above the device's 48 kHz, the downsampler and the monitor limiter.
        for (int recordRate : { 0, 96000, 192000 })
        {
            double percent[2] = { 0.0, 0.0 };
            int which = 0;
            for (auto w : { room::Width::Stereo, room::Width::Surround714 })
            {
                Plug plug(sources, w, recordRate);
                plug.proc.setPlaying(true);
                plug.run(1.0);
                const double t0 = TestClock::cpuMs();
                plug.run(8.0);
                percent[which++] = 100.0 * (TestClock::cpuMs() - t0) / 8000.0;
            }
            std::cout << "         rendering at " << (recordRate > 0 ? recordRate / 1000 : 48) << " kHz: Stereo " << juce::String(percent[0], 1) << "% of a core, 7.1.4 "
                      << juce::String(percent[1], 1) << "%" << std::endl;
            check(percent[1] < 100.0, "7.1.4 runs in real time rendering at " + juce::String(recordRate > 0 ? recordRate / 1000 : 48) + " kHz");
        }
    }

    {
        // The Output EQ's share: 7.1.4 rendering at 192 kHz, flat against every band in (seven filter sections).
        double percent[2] = { 0.0, 0.0 };
        for (int which = 0; which < 2; ++which)
        {
            Plug plug(sources, room::Width::Surround714, 192000);
            if (which == 1)
            {
                plug.setPlain(ParamIDs::eqLowCut, 80.0f);
                plug.setPlain(ParamIDs::eqLowCutSlope, 1.0f);
                plug.setPlain(ParamIDs::eqHighCut, 12000.0f);
                plug.setPlain(ParamIDs::eqLowShelfGain, 6.0f);
                plug.setPlain(ParamIDs::eqLowMidGain, -9.0f);
                plug.setPlain(ParamIDs::eqHighMidGain, 12.0f);
                plug.setPlain(ParamIDs::eqHighShelfGain, -6.0f);
            }
            plug.proc.setPlaying(true);
            plug.run(1.0);
            double best = 1.0e9;
            for (int pass = 0; pass < 3; ++pass)
            {
                const double t0 = TestClock::cpuMs();
                plug.run(4.0);
                best = std::min(best, 100.0 * (TestClock::cpuMs() - t0) / 4000.0);
            }
            percent[which] = best;
        }
        std::cout << "         7.1.4 rendering at 192 kHz: " << juce::String(percent[0], 1) << "% of a core with the Output EQ flat, " << juce::String(percent[1], 1)
                  << "% with every band in (" << juce::String(percent[1] - percent[0], 2) << "% more; the EQ timed on its own is in the Output EQ test)" << std::endl;
        // (A shared build machine a third as fast measured 4.28% on 59.1%: so 3% of a core, or a tenth of the plug-in's own cost there, whichever is more.)
        check(percent[1] - percent[0] < std::max(3.0, 0.1 * percent[0]) || !TestClock::holdsCostLimits(), "the Output EQ with every band in adds under 3% of a core, or a tenth of the plug-in, at 7.1.4 and 192 kHz");
    }

    Proc::useSettings(nullptr);
    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
