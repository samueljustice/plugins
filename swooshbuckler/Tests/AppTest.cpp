// App test: the standalone app's Listening setting and its audio device, without hardware.
//
//   - The setting: Stereo on a fresh install, remembered between runs (a rig made later reads it back from the
//     settings file on disk), and the app's rig takes that width and no other. In a host the track decides:
//     the setting is not used and every width is taken.
//   - AppOutput (what JUCE's player plays in the app): the rig is prepared again at each of the seven widths from
//     the setting; with enough outputs every speaker goes to its own output, bit for bit, in the order a room is
//     wired (typed here by hand: the tops last, so 7.1.4's rears stay on 7 and 8 where 7.1 has them), and the
//     outputs past the room are silent (Mono plays its one channel on the first two). With fewer outputs the room is folded down
//     (to stereo, or to mono on a one-output device), equal to room::fold of what the rig played when it is quiet,
//     and held under the Ceiling when it is loud (the unlimited fold goes over).
//   - A change of width while playing: out in 10 ms, the rig prepared again, in over 10 ms, and no click (the
//     largest change of step against the peak, as the other click tests measure it).
//   - Which outputs are switched on for a room (AppAudio::outputsFor), and the status texts word for word.
//   - The whole app chain with a pretend audio device (tools/FakeAudioDevice.h) through JUCE's device manager and
//     player: the room's outputs are switched on when the device has them, a change of Listening fades and
//     re-opens the device wider, a two-output device plays the fold and the status says so, outputs switched off
//     by hand stay off, and the tape still records the whole 7.1.4 room while you hear the fold.
//
// What this cannot show: a real multichannel interface or aggregate device (its driver, its own channel order,
// what reaches the speakers), and the dialog's clicks (the UI shots draw it).
//
// Usage: SwooshbucklerApp [scratch folder]. Run it from a scratch folder; nothing is read from or written to the
// user's settings.

#include <JuceHeader.h>
#include "AppOutput.h"
#include "FakeAudioDevice.h"
#include "Parameters.h"
#include "PluginProcessor.h"
#include "Room.h"
#include "TakeFile.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
    constexpr double c_fs = 48000.0;
    constexpr int c_block = 256;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok)
            ++g_failures;
    }

    float db(float gain) { return juce::Decibels::gainToDecibels(gain, -200.0f); }
    juce::String num(double v, int places = 2) { return juce::String(v, places); }

    using W = room::Width;
    constexpr W c_widths[] = { W::Mono, W::Stereo, W::Quad, W::Surround51, W::Surround71, W::Surround712, W::Surround714 };
    juce::String nameOf(W w) { return room::layout(w).name; }
    int channelsOf(W w) { return room::layout(w).numChannels; }

    using Proc = SwooshbucklerAudioProcessor;

    juce::AudioProcessor::BusesLayout layoutFor(const juce::AudioChannelSet& out)
    {
        juce::AudioProcessor::BusesLayout layout;
        layout.outputBuses.add(out);
        return layout;
    }

    /** A rig as the app makes it (or as a host does, app = false). */
    std::unique_ptr<Proc> makeRig(bool app)
    {
        if (app)
            juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
        std::unique_ptr<juce::AudioProcessor> base { createPluginFilter() };
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        return std::unique_ptr<Proc>(dynamic_cast<Proc*>(base.release()));
    }

    void setPlain(Proc& p, const char* id, float plain)
    {
        auto* param = p.getState().getParameter(id);
        param->setValueNotifyingHost(param->convertTo0to1(plain));
    }

    // One steady source: a low tone with a few harmonics, sounding all the time (smooth, so a click shows).
    void makeSource(const juce::File& file)
    {
        juce::AudioBuffer<float> tone(1, static_cast<int>(c_fs * 6.0));
        for (int i = 0; i < tone.getNumSamples(); ++i)
        {
            const double t = i / c_fs;
            float v = 0.0f;
            for (int h = 1; h < 5; ++h)
                v += static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 110.0 * h * t)) / static_cast<float>(h * h);
            tone.setSample(0, i, 0.45f * v);
        }
        file.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(file);
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream, c_fs, 1, 24, {}, 0));
        writer->writeFromAudioSampleBuffer(tone, 0, tone.getNumSamples());
    }

    /** The source on its track and the room settings that make every main speaker sound (as WideTest has them). */
    void load(Proc& p, const juce::File& source)
    {
        juce::String error;
        if (! p.getCargo().loadFile(0, source, error))
            check(false, "loads the source: " + error);
        p.getCargo().setRoleOverride(0, static_cast<int>(CargoRole::Tonal));
        setPlain(p, ParamIDs::flightPath[0], static_cast<float>(room::Path::Overhead));
        setPlain(p, ParamIDs::roomSpread, 0.3f);
        setPlain(p, ParamIDs::roomCentre, 1.0f);
    }

    /** AppOutput as the player drives it: a device with `outputs` outputs. */
    void prepareFor(AppOutput& out, int outputs)
    {
        out.releaseResources();
        out.setPlayConfigDetails(0, outputs, c_fs, c_block);
        out.prepareToPlay(c_fs, c_block);
    }

    /** Plays `blocks` blocks; the device's outputs and the room the rig played, side by side. */
    struct Played
    {
        juce::AudioBuffer<float> device, room;
    };
    Played play(AppOutput& out, int outputs, int blocks)
    {
        const int roomChannels = channelsOf(out.getRoom());
        Played p;
        p.device.setSize(outputs, blocks * c_block);
        p.room.setSize(roomChannels, blocks * c_block);
        juce::AudioBuffer<float> block(outputs, c_block);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b)
        {
            block.clear();
            out.processBlock(block, midi);
            for (int ch = 0; ch < outputs; ++ch)
                p.device.copyFrom(ch, b * c_block, block, ch, 0, c_block);
            for (int ch = 0; ch < roomChannels; ++ch)
                p.room.copyFrom(ch, b * c_block, out.lastRoom(), ch, 0, c_block);
        }
        return p;
    }

    float peakOf(const juce::AudioBuffer<float>& a, int ch, int from = 0)
    {
        return a.getMagnitude(ch, from, a.getNumSamples() - from);
    }

    /** The largest difference between two channels from `from` on, b delayed by `lag` samples. */
    float diffOf(const float* a, const float* b, int n, int from, int lag = 0)
    {
        float d = 0.0f;
        for (int i = from; i < n; ++i)
            d = std::max(d, std::abs(a[i] - (i - lag >= 0 ? b[i - lag] : 0.0f)));
        return d;
    }

    /** The largest change of step (second difference) in x[from, to), against the peak of x. */
    float stepChange(const float* x, int from, int to, float peak)
    {
        float m = 0.0f;
        for (int i = juce::jmax(2, from); i < to; ++i)
            m = std::max(m, std::abs(x[i] - 2.0f * x[i - 1] + x[i - 2]));
        return peak > 0.0f ? m / peak : 0.0f;
    }

    /** The order a room is wired in, output by output, typed by hand (not read from the code under test): the
        ear-level speakers and the LFE as 5.1, 7.1 and Dolby Atmos monitoring have them, then the tops. */
    using S = room::Speaker;
    std::vector<S> wiring(W w)
    {
        switch (w)
        {
            case W::Mono:        return { S::C };
            case W::Stereo:      return { S::L, S::R };
            case W::Quad:        return { S::L, S::R, S::Ls, S::Rs };
            case W::Surround51:  return { S::L, S::R, S::C, S::Lfe, S::Ls, S::Rs };
            case W::Surround71:  return { S::L, S::R, S::C, S::Lfe, S::Lss, S::Rss, S::Lsr, S::Rsr };
            case W::Surround712: return { S::L, S::R, S::C, S::Lfe, S::Lss, S::Rss, S::Lsr, S::Rsr, S::Ltm, S::Rtm };
            case W::Surround714: return { S::L, S::R, S::C, S::Lfe, S::Lss, S::Rss, S::Lsr, S::Rsr, S::Ltf, S::Rtf, S::Ltr, S::Rtr };
        }
        return {};
    }
    juce::String wiringText(W w)
    {
        juce::String s;
        for (auto speaker : wiring(w))
            s << (s.isEmpty() ? "" : " ") << room::info(speaker).name;
        return s;
    }

    /** The room the rig played, folded down to `to` with room::fold. */
    juce::AudioBuffer<float> fold(const juce::AudioBuffer<float>& roomAudio, W from, W to)
    {
        const auto& m = room::fold(from, to);
        juce::AudioBuffer<float> out(m.numOut, roomAudio.getNumSamples());
        room::applyFold(m, roomAudio.getArrayOfReadPointers(), out.getArrayOfWritePointers(), roomAudio.getNumSamples());
        return out;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("SwooshbucklerAppTest");
    folder.createDirectory();
    const auto source = folder.getChildFile("tone.wav");
    makeSource(source);
    const auto tapeFolder = folder.getChildFile("tape");
    tapeFolder.deleteRecursively();
    tapeFolder.createDirectory();

    juce::PropertiesFile::Options options;
    options.applicationName = "SwooshbucklerAppTest";
    options.filenameSuffix = ".settings";
    const auto settingsFile = folder.getChildFile("test.settings");
    settingsFile.deleteFile();
    juce::PropertiesFile settings(settingsFile, options);
    settings.setValue("recordFolder", tapeFolder.getFullPathName());
    Proc::useSettings(&settings);

    //==========================================================================
    std::cout << "The Listening setting" << std::endl;
    {
        auto app = makeRig(true);
        auto host = makeRig(false);
        check(app->isApp() && ! host->isApp(), "the app's rig is the app; a host's is not");
        check(app->getListening() == W::Stereo, "a fresh install listens in Stereo");
        int told = 0;
        app->onListeningChanged = [&told] { ++told; };
        app->setListening(W::Surround714);
        check(app->getListening() == W::Surround714 && told == 1, "choosing 7.1.4 is taken and the app is told once");
        {
            juce::PropertiesFile onDisk(settingsFile, options); // what the next run reads
            check(onDisk.getIntValue("listening", -1) == static_cast<int>(W::Surround714), "it is written to the settings file");
        }
        auto later = makeRig(true);
        check(later->getListening() == W::Surround714, "a rig made later (the next run) listens in 7.1.4");

        bool onlyItsOwn = true;
        for (auto w : c_widths)
        {
            app->setListening(w);
            for (auto other : c_widths)
                onlyItsOwn = onlyItsOwn && app->checkBusesLayoutSupported(layoutFor(room::channelSet(other))) == (other == w);
        }
        check(onlyItsOwn, "the app's rig takes its Listening width and no other, for each of the seven");

        host->setListening(W::Surround51);
        bool hostTakesAll = true;
        for (auto w : c_widths)
            hostTakesAll = hostTakesAll && host->checkBusesLayoutSupported(layoutFor(room::channelSet(w)));
        check(host->getListening() == W::Stereo && hostTakesAll && host->listeningStatus().isEmpty(),
              "in a host the setting is not used: the track decides, every width is taken, no status line");
        app->setListening(W::Stereo);
        app->onListeningChanged = nullptr;
    }

    //==========================================================================
    std::cout << "Enough outputs: the rig prepared again at each width, every speaker on its own output" << std::endl;
    {
        auto rig = makeRig(true);
        load(*rig, source);
        AppOutput out(*rig);
        rig->setPlaying(true);
        for (auto w : c_widths)
        {
            rig->setListening(w);
            prepareFor(out, 12);
            const int n = channelsOf(w);
            const auto played = play(out, 12, static_cast<int>(1.5 * c_fs / c_block));
            const int from = 4 * c_block; // after the look-ahead and the 10 ms fade-in
            float routed = 0.0f, past = 0.0f;
            int sounding = 0;
            bool distinct = true; // every speaker of the room is on one output and no two outputs share one
            std::vector<int> used;
            for (int ch = 0; ch < 12; ++ch)
            {
                const bool carries = ch < n || (w == W::Mono && ch == 1);
                // The speaker this output must play, from the order typed above, as the rig's own channel.
                const int roomCh = ! carries ? -1 : w == W::Mono ? 0 : room::layout(w).indexOf(wiring(w)[static_cast<size_t>(ch)]);
                if (carries)
                {
                    distinct = distinct && roomCh >= 0 && (w == W::Mono || std::find(used.begin(), used.end(), roomCh) == used.end());
                    used.push_back(roomCh);
                    if (roomCh < 0)
                        continue;
                    routed = std::max(routed, diffOf(played.device.getReadPointer(ch), played.room.getReadPointer(roomCh), played.device.getNumSamples(), from));
                    if (peakOf(played.device, ch, from) > 1.0e-3f)
                        ++sounding;
                }
                else
                    past = std::max(past, peakOf(played.device, ch));
            }
            const int mains = (w == W::Mono ? 2 : n) - (room::hasLfe(w) ? 1 : 0); // LFE Off: the LFE is silent
            check(rig->getRoomWidth() == w && rig->getTotalNumOutputChannels() == n && out.getMonitor() == AppOutput::Monitor::Direct
                      && routed == 0.0f && past == 0.0f && sounding == mains && distinct,
                  nameOf(w) + ": the rig is " + nameOf(rig->getRoomWidth()) + " with " + juce::String(rig->getTotalNumOutputChannels())
                      + " channels; each speaker on its own output in the order " + wiringText(w) + ", bit for bit (largest difference " + juce::String(routed)
                      + "), " + juce::String(sounding) + " outputs sounding, the rest silent (" + juce::String(past) + ")"
                      + (w == W::Mono ? ", the one channel on the first two" : ""));
        }
    }

    //==========================================================================
    std::cout << "Fewer outputs: a fold-down, held under the Ceiling" << std::endl;
    {
        struct Case { W room; int outputs; W heard; };
        const Case cases[] = { { W::Surround714, 2, W::Stereo }, { W::Surround714, 6, W::Stereo }, { W::Surround51, 2, W::Stereo },
                               { W::Quad, 2, W::Stereo }, { W::Stereo, 1, W::Mono }, { W::Surround714, 1, W::Mono } };
        for (const auto& c : cases)
        {
            auto rig = makeRig(true);
            load(*rig, source);
            AppOutput out(*rig);
            rig->setPlaying(true);
            rig->setListening(c.room);
            setPlain(*rig, ParamIDs::masterTrim, -20.0f); // quiet: the fold's limiter has nothing to do
            prepareFor(out, c.outputs);
            const auto expected = c.heard == W::Stereo ? AppOutput::Monitor::Stereo : AppOutput::Monitor::Mono;
            const auto played = play(out, c.outputs, static_cast<int>(2.0 * c_fs / c_block));
            const auto folded = fold(played.room, c.room, c.heard);
            const int lag = out.getLatencySamples() - rig->getLatencySamples(); // the fold limiter's look-ahead
            float d = 0.0f, past = 0.0f;
            for (int ch = 0; ch < c.outputs; ++ch)
                if (ch < folded.getNumChannels())
                    d = std::max(d, diffOf(played.device.getReadPointer(ch), folded.getReadPointer(ch), played.device.getNumSamples(), 4 * c_block, lag));
                else
                    past = std::max(past, peakOf(played.device, ch));
            check(out.getMonitor() == expected && rig->getRoomWidth() == c.room && d < 1.0e-6f && past == 0.0f
                      && peakOf(played.device, 0, c_block) > 1.0e-3f,
                  nameOf(c.room) + " on " + juce::String(c.outputs) + (c.outputs == 1 ? " output" : " outputs") + ": heard as " + nameOf(c.heard)
                      + ", equal to room::fold of the " + nameOf(c.room) + " the rig played (" + juce::String(lag) + " samples later, largest difference "
                      + juce::String(d) + ")" + (c.outputs > folded.getNumChannels() ? ", the other outputs silent" : ""));
        }

        // Loud: a 7.1.4 room pushed into its limiter, everything spread, folded to stereo. Each speaker is under the
        // Ceiling, the fold of them is not, and what is heard is held there again.
        {
            auto rig = makeRig(true);
            load(*rig, source);
            AppOutput out(*rig);
            rig->setPlaying(true);
            rig->setListening(W::Surround714);
            setPlain(*rig, ParamIDs::push, 12.0f);
            setPlain(*rig, ParamIDs::roomSpread, 1.0f);
            prepareFor(out, 2);
            const auto played = play(out, 2, static_cast<int>(3.0 * c_fs / c_block));
            const auto folded = fold(played.room, W::Surround714, W::Stereo);
            const float ceiling = rig->getState().getRawParameterValue(ParamIDs::ceiling)->load();
            float roomPeak = 0.0f;
            for (int ch = 0; ch < played.room.getNumChannels(); ++ch)
                roomPeak = std::max(roomPeak, peakOf(played.room, ch));
            const float unlimited = std::max(peakOf(folded, 0), peakOf(folded, 1));
            const float heard = std::max(peakOf(played.device, 0), peakOf(played.device, 1));
            std::cout << "         loudest speaker " << num(db(roomPeak)) << " dB, the plain fold " << num(db(unlimited)) << " dB, heard "
                      << num(db(heard)) << " dB (Ceiling " << num(ceiling) << " dB), fold limiter " << num(out.getFoldGainReductionDb()) << " dB now" << std::endl;
            check(db(unlimited) > ceiling + 1.0f, "the plain fold of a loud 7.1.4 room goes over the Ceiling (" + num(db(unlimited)) + " dB)");
            check(db(heard) <= ceiling + 0.001f, "what is heard is held under the Ceiling (" + num(db(heard), 3) + " dB)");
        }
    }

    //==========================================================================
    std::cout << "A change of width while playing" << std::endl;
    {
        auto rig = makeRig(true);
        load(*rig, source);
        AppOutput out(*rig);
        rig->setPlaying(true);
        rig->setListening(W::Surround51);
        prepareFor(out, 12);
        const auto steady = play(out, 12, static_cast<int>(2.0 * c_fs / c_block));
        // Out: how long until silent, and the samples that went.
        out.fadeOut();
        juce::AudioBuffer<float> tail(12, 8 * c_block);
        juce::AudioBuffer<float> block(12, c_block);
        juce::MidiBuffer midi;
        int blocks = 0;
        while (! out.isFadedOut() && blocks < 8)
        {
            block.clear();
            out.processBlock(block, midi);
            tail.copyFrom(0, blocks * c_block, block, 0, 0, c_block);
            ++blocks;
        }
        const int fadeOutSamples = blocks * c_block;
        rig->setListening(W::Surround714);
        prepareFor(out, 12); // what the app does next: JUCE's player prepares AppOutput again
        const auto after = play(out, 12, static_cast<int>(1.0 * c_fs / c_block));
        check(out.isFadedOut() == false && rig->getRoomWidth() == W::Surround714 && out.getMonitor() == AppOutput::Monitor::Direct,
              "the rig is prepared again at 7.1.4 and plays");

        // L, which both rooms have, across the change: the steady sound, the fade out, then the new room fading in.
        const int n1 = steady.device.getNumSamples(), n3 = after.device.getNumSamples();
        std::vector<float> l(static_cast<size_t>(n1 + fadeOutSamples + n3));
        std::copy_n(steady.device.getReadPointer(0), n1, l.begin());
        std::copy_n(tail.getReadPointer(0), fadeOutSamples, l.begin() + n1);
        std::copy_n(after.device.getReadPointer(0), n3, l.begin() + n1 + fadeOutSamples);
        float peak = 0.0f;
        for (float v : l)
            peak = std::max(peak, std::abs(v));
        const float steadyStep = stepChange(l.data(), n1 / 2, n1, peak);
        const float changeStep = stepChange(l.data(), n1 - 8, n1 + fadeOutSamples + 2 * c_block, peak);
        float lastOut = 0.0f;
        for (int i = fadeOutSamples - c_block; i < fadeOutSamples; ++i)
            lastOut = std::max(lastOut, std::abs(tail.getSample(0, i)));
        std::cout << "         faded out in " << fadeOutSamples << " samples; change of step: steady " << num(100.0 * steadyStep, 3)
                  << "% of the peak, across the change " << num(100.0 * changeStep, 3) << "%" << std::endl;
        check(fadeOutSamples <= static_cast<int>(AppOutput::c_fadeSeconds * c_fs) + c_block, "silent within 10 ms and a block");
        check(std::abs(after.device.getSample(0, 0)) <= peak * 0.01f, "the new room starts from silence");
        check(changeStep < 0.005f, "no click across the change: largest change of step " + num(100.0 * changeStep, 3) + "% of the peak (limit 0.5%)");
        check(lastOut < 0.5f * peak || lastOut == 0.0f, "the fade really is a fade (the last block before silence is quiet)");
    }

    //==========================================================================
    std::cout << "Which outputs a room switches on" << std::endl;
    {
        auto bits = [](std::initializer_list<int> list)
        {
            juce::BigInteger b;
            for (int i : list)
                b.setBit(i);
            return b;
        };
        auto range = [](int from, int count)
        {
            juce::BigInteger b;
            b.setRange(from, count, true);
            return b;
        };
        check(AppAudio::outputsFor(bits({ 0, 1 }), 16, 6) == range(0, 6), "the first pair on a 16-output device: outputs 1 to 6 for 5.1");
        check(AppAudio::outputsFor(bits({ 4, 5 }), 16, 6) == range(4, 6), "outputs 5 and 6 picked: 5 to 10, the pair stays where it is");
        check(AppAudio::outputsFor(bits({ 14, 15 }), 16, 6) == (range(0, 4) | bits({ 14, 15 })), "the last pair: the rest from the first output");
        check(AppAudio::outputsFor(range(0, 12), 16, 6) == range(0, 12), "more already on: left as they are");
        check(AppAudio::outputsFor(bits({ 0, 1 }), 2, 12) == bits({ 0, 1 }), "a stereo device: nothing to add");
        check(AppAudio::outputsFor({}, 8, 6) == range(0, 6), "none on: the first six");
        check(AppAudio::outputsFor(bits({ 0, 1, 20 }), 16, 2) == bits({ 0, 1 }), "an output the device does not have is dropped");
    }

    //==========================================================================
    std::cout << "The status texts" << std::endl;
    {
        check(Proc::listeningStatus(W::Surround714, 2, 2) == "Playing 7.1.4 folded to stereo: this audio device has 2 outputs.", "7.1.4 on a stereo device");
        check(Proc::listeningStatus(W::Surround714, 2, 16) == "Playing 7.1.4 folded to stereo: 2 of this audio device's 16 outputs are switched on.",
              "7.1.4 with two of sixteen outputs switched on");
        check(Proc::listeningStatus(W::Stereo, 1, 1) == "Playing Stereo folded to mono: this audio device has 1 output.", "Stereo on a one-output device");
        check(Proc::listeningStatus(W::Surround51, 6, 6).isEmpty() && Proc::listeningStatus(W::Mono, 1, 1).isEmpty()
                  && Proc::listeningStatus(W::Surround714, 0, 0).isEmpty(),
              "nothing to say with enough outputs, for Mono on one, or with no device");
        check(Proc::listeningLine(W::Surround51, 6, 6) == "Plays on the first 6 outputs switched on, in this order: L R C LFE Ls Rs.", "5.1's line spells out the order");
        check(Proc::listeningLine(W::Surround714, 12, 12)
                  == "Plays on the first 12 outputs switched on, in this order: L R C LFE Lss Rss Lsr Rsr Ltf Rtf Ltr Rtr.",
              "7.1.4's line: the rears on 7 and 8, the tops last");
        check(Proc::listeningLine(W::Surround71, 8, 8) == "Plays on the first 8 outputs switched on, in this order: L R C LFE Lss Rss Lsr Rsr."
                  && Proc::listeningLine(W::Surround712, 10, 10) == "Plays on the first 10 outputs switched on, in this order: L R C LFE Lss Rss Lsr Rsr Ltm Rtm.",
              "7.1 and 7.1.2 keep the same eight, 7.1.2 adds its two tops");
        {
            // The dialog: each speaker beside the number of the output it really plays on (a number and its speaker
            // are held together on one line by a no-break space, shown here as a plain one).
            auto first = [](int count) { juce::BigInteger b; b.setRange(0, count, true); return b; };
            auto text = [](W w, const juce::BigInteger& active, int available)
            {
                return AppAudio::dialogText(w, active, available).replaceCharacter(juce::juce_wchar(0x00a0), ' ');
            };
            check(text(W::Surround714, first(12), 12)
                      == "Listening: 7.1.4, 12 outputs. The speakers play on these outputs: 1 L, 2 R, 3 C, 4 LFE, 5 Lss, 6 Rss, 7 Lsr, 8 Rsr, 9 Ltf, 10 Rtf, 11 Ltr, 12 Rtr.",
                  "the dialog numbers 7.1.4's outputs in the same order");
            juce::BigInteger picked; // outputs 3, 4 and 9 to 12 of sixteen, switched on by hand
            picked.setRange(2, 2, true);
            picked.setRange(8, 4, true);
            check(text(W::Surround51, picked, 16) == "Listening: 5.1, 6 outputs. The speakers play on these outputs: 3 L, 4 R, 9 C, 10 LFE, 11 Ls, 12 Rs.",
                  "outputs picked by hand: the dialog names the outputs the speakers really play on");
            check(text(W::Surround51, first(2), 2)
                      == "Listening: 5.1, 6 outputs. The speakers go out in this order, lowest output first: L R C LFE Ls Rs.\n"
                         "Playing 5.1 folded to stereo: this audio device has 2 outputs. Switch on more outputs below, or choose a device that has them.",
                  "too few outputs: the dialog gives the order and says it is folded");
            check(text(W::Mono, first(2), 2) == "Listening: Mono, one channel, played on the first two outputs switched on.", "the dialog for Mono");
        }
        check(Proc::listeningLine(W::Surround714, 2, 2)
                  == "Playing 7.1.4 folded to stereo: this audio device has 2 outputs. The tape and Render Variations still get all 12 channels.",
              "folded: the line says the tape still gets the room");
        check(Proc::listeningLine(W::Mono, 2, 2) == "The one channel plays on the first two outputs switched on.", "Mono's line");
        check(Proc::listeningLine(W::Stereo, 0, 0) == "No audio device is playing. Choose one with Audio Device.", "no device");
    }

    //==========================================================================
    std::cout << "The whole app with a pretend audio device" << std::endl;
    {
        settings.setValue("listening", static_cast<int>(W::Surround51));
        juce::AudioDeviceManager devices;
        devices.addAudioDeviceType(std::make_unique<FakeAudio::Type>());
        devices.initialise(0, 2, nullptr, true);
        FakeAudio::choose(devices, "Fake 16", 2);
        auto rig = makeRig(true);
        load(*rig, source);
        juce::AudioProcessorPlayer player;
        devices.addAudioCallback(&player);
        {
            AppAudio audio(*rig, devices, player);
            audio.start();
            auto* device = FakeAudio::current(devices);
            juce::BigInteger six;
            six.setRange(0, 6, true);
            check(device != nullptr && device->getActiveOutputChannels() == six && rig->getRoomWidth() == W::Surround51
                      && audio.output().getMonitor() == AppOutput::Monitor::Direct && rig->getDeviceOutputs() == 6
                      && rig->getDeviceOutputsAvailable() == 16 && rig->listeningStatus().isEmpty(),
                  "5.1 on a 16-output device that had two on: outputs 1 to 6 switched on, the rig at 5.1, played direct");

            auto pull = [&](double seconds)
            {
                juce::AudioBuffer<float> all, block;
                const int blocks = static_cast<int>(seconds * FakeAudio::c_rate / FakeAudio::c_buffer);
                for (int b = 0; b < blocks; ++b)
                {
                    auto* d = FakeAudio::current(devices);
                    if (d == nullptr)
                        break;
                    d->pull(block);
                    if (b == 0)
                        all.setSize(block.getNumChannels(), blocks * FakeAudio::c_buffer);
                    for (int ch = 0; ch < block.getNumChannels() && ch < all.getNumChannels(); ++ch)
                        all.copyFrom(ch, b * FakeAudio::c_buffer, block, ch, 0, FakeAudio::c_buffer);
                }
                return all;
            };
            auto sounding = [](const juce::AudioBuffer<float>& a)
            {
                int s = 0;
                for (int ch = 0; ch < a.getNumChannels(); ++ch)
                    if (a.getMagnitude(ch, a.getNumSamples() / 2, a.getNumSamples() / 2) > 1.0e-3f)
                        ++s;
                return s;
            };
            rig->setPlaying(true);
            auto heard = pull(1.5);
            check(heard.getNumChannels() == 6 && sounding(heard) == 5, "the device plays five main speakers (the LFE is Off)");

            // 7.1.4: fades, then the device is opened with twelve outputs and the rig prepared at 7.1.4.
            const int opened = device->opened;
            rig->setListening(W::Surround714);
            int waited = 0;
            while (! audio.output().isFadedOut() && waited < 20)
            {
                pull(FakeAudio::c_buffer / FakeAudio::c_rate);
                ++waited;
            }
            check(audio.output().isFadedOut() && waited <= 3, "a change of Listening fades the sound out first (" + juce::String(waited) + " blocks)");
            check(audio.finishChange(), "then the change goes ahead");
            device = FakeAudio::current(devices);
            juce::BigInteger twelve;
            twelve.setRange(0, 12, true);
            check(device != nullptr && device->opened > opened && device->getActiveOutputChannels() == twelve && rig->getRoomWidth() == W::Surround714
                      && audio.output().getRoom() == W::Surround714 && audio.output().getMonitor() == AppOutput::Monitor::Direct && rig->getDeviceOutputs() == 12,
                  "7.1.4: the device opened again with outputs 1 to 12, the rig at 7.1.4, played direct");
            heard = pull(1.5);
            check(heard.getNumChannels() == 12 && sounding(heard) == 11, "eleven main speakers sound");

            // A stereo device: the fold, and the status line.
            FakeAudio::choose(devices, "Fake 2", 2);
            audio.deviceChanged();
            rig->getState().getParameter(ParamIDs::push)->setValueNotifyingHost(rig->getState().getParameter(ParamIDs::push)->convertTo0to1(12.0f));
            heard = pull(2.0);
            const float ceiling = rig->getState().getRawParameterValue(ParamIDs::ceiling)->load();
            const float loudest = juce::jmax(heard.getMagnitude(0, 0, heard.getNumSamples()), heard.getMagnitude(1, 0, heard.getNumSamples()));
            check(audio.output().getMonitor() == AppOutput::Monitor::Stereo && rig->getRoomWidth() == W::Surround714 && heard.getNumChannels() == 2
                      && sounding(heard) == 2 && db(loudest) <= ceiling + 0.001f,
                  "a two-output device: the 7.1.4 room folded to stereo, under the Ceiling (" + num(db(loudest), 3) + " dB), the rig still 7.1.4");
            check(rig->isListeningFolded() && rig->listeningStatus() == "Playing 7.1.4 folded to stereo: this audio device has 2 outputs.",
                  "the status line: " + rig->listeningStatus());

            // The tape records the whole room meanwhile.
            {
                rig->setSkipSilence(false);
                juce::String error;
                check(rig->arm(error), "the tape arms while the fold plays " + error);
                pull(2.0);
                rig->stopRecording();
                for (int wait = 0; wait < 200 && rig->isRecording(); ++wait)
                    juce::Thread::sleep(10);
                const auto r = takefile::openReader(rig->getLastRecording());
                int channels = 0, tapeSounding = 0;
                if (r != nullptr)
                {
                    juce::AudioBuffer<float> file(static_cast<int>(r->numChannels), static_cast<int>(r->lengthInSamples));
                    r->read(&file, 0, file.getNumSamples(), 0, true, true);
                    channels = file.getNumChannels();
                    tapeSounding = sounding(file);
                }
                check(channels == 12 && tapeSounding == 11, "the tape records all 12 channels of the room (" + juce::String(channels) + ", "
                                                              + juce::String(tapeSounding) + " sounding) while you hear it folded");
            }

            // While the tape records, Listening stays as it is: a take is written at the room's width.
            {
                juce::String error;
                check(rig->arm(error), "the tape arms again " + error);
                pull(0.5);
                const bool refused = ! rig->setListening(W::Surround51);
                check(refused && rig->getListening() == W::Surround714 && ! audio.output().isFadedOut() && rig->getRoomWidth() == W::Surround714,
                      "Listening cannot change while the tape records: refused, nothing fades, the rig stays 7.1.4");
                rig->stopRecording();
                for (int wait = 0; wait < 200 && rig->isRecording(); ++wait)
                    juce::Thread::sleep(10);
            }

            // Back to the big device: a device chosen anew gets the room's outputs; outputs switched off by hand on
            // the same device stay off, and the status line says how many are on.
            FakeAudio::choose(devices, "Fake 16", 2);
            audio.deviceChanged();
            check(FakeAudio::current(devices)->getActiveOutputChannels() == twelve && audio.output().getMonitor() == AppOutput::Monitor::Direct,
                  "another device chosen: the room's twelve outputs are switched on");
            FakeAudio::choose(devices, "Fake 16", 2);
            audio.deviceChanged();
            check(FakeAudio::current(devices)->getActiveOutputChannels().countNumberOfSetBits() == 2 && audio.output().getMonitor() == AppOutput::Monitor::Stereo
                      && rig->listeningStatus() == "Playing 7.1.4 folded to stereo: 2 of this audio device's 16 outputs are switched on.",
                  "two outputs switched on by hand stay that way; the status: " + rig->listeningStatus());

            // Mono: no new outputs needed, the rig prepared again here; the one channel on both outputs.
            rig->setListening(W::Mono);
            for (int i = 0; i < 4 && ! audio.output().isFadedOut(); ++i)
                pull(FakeAudio::c_buffer / FakeAudio::c_rate);
            check(audio.finishChange(), "Mono goes ahead after its fade");
            heard = pull(1.0);
            check(rig->getRoomWidth() == W::Mono && audio.output().getMonitor() == AppOutput::Monitor::Direct && heard.getNumChannels() == 2
                      && sounding(heard) == 2 && diffOf(heard.getReadPointer(0), heard.getReadPointer(1), heard.getNumSamples(), 0) == 0.0f
                      && rig->listeningStatus().isEmpty(),
                  "Mono: the rig is Mono, its one channel on both outputs, no status line");
        }
        check(player.getCurrentProcessor() == nullptr, "closing the app's audio lets go of what the player plays");
        devices.removeAudioCallback(&player);
        devices.closeAudioDevice();
    }

    Proc::useSettings(nullptr);
    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
