// Session test: runs the real plug-in offline and checks what happens around the tape and the session.
//
//   - Stop: the sources let go and the echoes and Space ring out, into the output and into the tape, then rest.
//   - Skip Silence on the real rig, and a continuous take with it off.
//   - LAST 60s is written in the tape's format.
//   - The settings (shared file, Skip Silence on unless switched off), the wander shape names.
//   - The app's session autosave: saves when something changed, off the calling thread; a copy left behind is
//     recovered; how long a save takes with about 20 MB of audio in the session.
//
// Usage: SwooshbucklerSession [scratch folder] (a folder in the temp directory if none is given). Nothing is read
// from or written to the user's settings.

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "MorphPad.h"
#include "SessionAutosave.h"

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

    void writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio, double rate)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(file);
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream, rate, static_cast<unsigned int>(audio.getNumChannels()), 24, {}, 0));
        writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    }

    // Three sources: a growl with gaps, a steady wind, and a row of hits.
    void makeSources(const juce::File& folder)
    {
        juce::Random r(3);
        juce::AudioBuffer<float> growl(1, static_cast<int>(c_fs * 6.0)), wind(1, static_cast<int>(c_fs * 8.0)), hits(1, static_cast<int>(c_fs * 6.0));
        growl.clear();
        hits.clear();
        double ph = 0.0;
        float y = 0.0f;
        for (int i = 0; i < growl.getNumSamples(); ++i)
        {
            const double t = i / c_fs, lt = std::fmod(t, 1.5);
            if (lt < 0.6)
            {
                ph += (110.0 + 40.0 * std::sin(6.0 * t)) / c_fs;
                float v = 0.0f;
                for (int h = 1; h < 8; ++h)
                    v += static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * ph * h)) / static_cast<float>(h);
                growl.setSample(0, i, 0.4f * static_cast<float>(std::sin(juce::MathConstants<double>::pi * lt / 0.6)) * std::tanh(2.0f * v));
            }
        }
        for (int i = 0; i < wind.getNumSamples(); ++i)
        {
            y += 0.06f * ((r.nextFloat() * 2.0f - 1.0f) - y);
            wind.setSample(0, i, 2.5f * y * static_cast<float>(0.75 + 0.25 * std::sin(1.3 * i / c_fs)));
        }
        for (int k = 0; k < 6; ++k)
            for (int i = 0; i < static_cast<int>(0.25 * c_fs); ++i)
                hits.setSample(0, static_cast<int>((0.2 + k) * c_fs) + i,
                               0.8f * std::exp(-static_cast<float>(i) / (0.04f * static_cast<float>(c_fs))) * (r.nextFloat() * 2.0f - 1.0f));
        writeWav(folder.getChildFile("growl.wav"), growl, c_fs);
        writeWav(folder.getChildFile("wind.wav"), wind, c_fs);
        writeWav(folder.getChildFile("hits.wav"), hits, c_fs);
    }

    juce::RangedAudioParameter* param(juce::AudioProcessor& p, const juce::String& id)
    {
        for (auto* prm : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(prm))
                if (ranged->getParameterID() == id)
                    return ranged;
        return nullptr;
    }

    void setNorm(juce::AudioProcessor& p, const juce::String& id, float norm)
    {
        if (auto* prm = param(p, id))
            prm->setValueNotifyingHost(norm);
        else
            check(false, "parameter " + id + " exists");
    }

    struct Rig
    {
        std::unique_ptr<juce::AudioProcessor> base { createPluginFilter() };
        SwooshbucklerAudioProcessor& proc { *dynamic_cast<SwooshbucklerAudioProcessor*>(base.get()) };

        Rig(const juce::File& sources, int recordRate)
        {
            proc.setPlayConfigDetails(0, 2, c_fs, c_block);
            proc.overrideRecordRate(recordRate);
            const char* names[] = { "growl", "wind", "hits" };
            const CargoRole roles[] = { CargoRole::Tonal, CargoRole::Bed, CargoRole::Impact };
            for (int t = 0; t < 3; ++t)
            {
                juce::String error;
                if (! proc.getCargo().loadFile(t, sources.getChildFile(juce::String(names[t]) + ".wav"), error))
                    check(false, "loads " + juce::String(names[t]) + ": " + error);
                proc.getCargo().setRoleOverride(t, static_cast<int>(roles[t]));
            }
        }

        /** Renders `seconds` and returns the output (stereo). */
        juce::AudioBuffer<float> run(double seconds)
        {
            const int total = static_cast<int>(seconds * c_fs) / c_block * c_block;
            juce::AudioBuffer<float> out(2, total);
            juce::MidiBuffer midi;
            for (int pos = 0; pos < total; pos += c_block)
            {
                juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, pos, c_block);
                view.clear();
                proc.processBlock(view, midi);
                if ((pos / c_block) % 32 == 31)
                    juce::Thread::sleep(1); // offline is far faster than real time: let the tape's writer keep up
            }
            return out;
        }
    };

    float peak(const juce::AudioBuffer<float>& b, double fromSeconds, double toSeconds, double rate = c_fs)
    {
        const int from = juce::jlimit(0, b.getNumSamples(), static_cast<int>(fromSeconds * rate));
        const int to = juce::jlimit(from, b.getNumSamples(), static_cast<int>(toSeconds * rate));
        return to > from ? b.getMagnitude(from, to - from) : 0.0f;
    }

    /** Seconds from the start of b to its last sample that is not exactly zero. */
    double lastSound(const juce::AudioBuffer<float>& b)
    {
        for (int i = b.getNumSamples() - 1; i >= 0; --i)
            if (! juce::exactlyEqual(b.getSample(0, i), 0.0f) || ! juce::exactlyEqual(b.getSample(1, i), 0.0f))
                return (i + 1) / c_fs;
        return 0.0;
    }

    double readFile(const juce::File& f, juce::AudioBuffer<float>& into, int* bits = nullptr)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(f));
        if (r == nullptr)
        {
            into.setSize(2, 0);
            return 0.0;
        }
        into.setSize(static_cast<int>(r->numChannels), static_cast<int>(r->lengthInSamples));
        r->read(&into, 0, static_cast<int>(r->lengthInSamples), 0, true, true);
        if (bits != nullptr)
            *bits = static_cast<int>(r->bitsPerSample);
        return r->sampleRate;
    }

    // Stop with a tail: what the output and the tape do. Returns false if the take could not be made.
    void stopTest(const juce::File& sources, int recordRate, float space, float fill, const juce::String& label)
    {
        Rig rig(sources, recordRate);
        auto& p = rig.proc;
        setNorm(p, ParamIDs::space, space);
        setNorm(p, ParamIDs::fill, fill);
        p.prepareToPlay(c_fs, c_block);
        p.setSkipSilence(false);
        juce::String error;
        check(p.arm(error), label + ": the tape arms " + error);
        p.setPlaying(true);
        const auto playing = rig.run(4.0);
        p.setPlaying(false);
        const auto after = rig.run(8.0);
        const double taped = p.getRecordedSeconds();
        p.stopRecording();

        const float before = peak(playing, 3.0, 4.0);
        const float ringing = peak(after, 0.15, 0.35);
        const double rest = lastSound(after);
        const bool wet = space > 0.0f || fill > 0.0f;
        std::cout << "         " << label << ": playing " << juce::String(db(before), 1) << " dB, 150-350 ms after Stop "
                  << juce::String(db(ringing), 1) << " dB, at rest after " << juce::String(rest, 2) << " s, tape "
                  << juce::String(taped, 2) << " s" << std::endl;
        if (wet)
            check(ringing > juce::Decibels::decibelsToGain(-60.0f), label + ": the tail is still ringing 150 ms after Stop");
        else
            check(rest < 1.0, label + ": with no echoes or Space the rig rests within a second of Stop");
        check(rest > 0.1 && rest < SwooshbucklerAudioProcessor::c_tailMaxSeconds + 0.5,
              label + ": the rig comes to rest (" + juce::String(rest, 2) + " s after Stop)");
        const float lastBit = peak(after, rest - 0.004, rest);
        check(lastBit < juce::Decibels::decibelsToGain(-55.0f), label + ": the output ends quietly, not cut off ("
                                                                    + juce::String(db(lastBit), 1) + " dB in its last 4 ms)");

        juce::AudioBuffer<float> file;
        const double rate = readFile(p.getLastRecording(), file);
        const double length = rate > 0.0 ? file.getNumSamples() / rate : 0.0;
        if (wet)
            check(length > 4.0 + 0.3, label + ": the tape keeps the tail (" + juce::String(length, 2) + " s for 4 s of playing)");
        const float fileEnd = peak(file, length - 0.004, length, rate);
        check(length > 3.5 && fileEnd < juce::Decibels::decibelsToGain(-55.0f),
              label + ": the take ends quietly (" + juce::String(db(fileEnd), 1) + " dB in its last 4 ms)");

        // Play again: the sources come back.
        p.setPlaying(true);
        const auto again = rig.run(3.0);
        check(peak(again, 1.0, 3.0) > 0.25f * before, label + ": Play brings the sources back (" + juce::String(db(peak(again, 1.0, 3.0)), 1) + " dB)");
        p.setPlaying(false);
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SwooshbucklerSessionTest");
    folder.createDirectory();
    const auto sources = folder.getChildFile("sources");
    sources.createDirectory();
    makeSources(sources);
    const auto tapeFolder = folder.getChildFile("tape");
    tapeFolder.deleteRecursively();
    tapeFolder.createDirectory();

    // A scratch settings file, shared with every processor made below (the user's own is never touched).
    juce::PropertiesFile::Options options;
    options.applicationName = "SwooshbucklerSessionTest";
    options.filenameSuffix = ".settings";
    const auto settingsFile = folder.getChildFile("test.settings");
    settingsFile.deleteFile();
    juce::PropertiesFile settings(settingsFile, options);
    settings.setValue("recordFolder", tapeFolder.getFullPathName());
    SwooshbucklerAudioProcessor::useSettings(&settings);

    // Settings
    {
        Rig rig(sources, 0);
        check(rig.proc.getRecordFolder() == tapeFolder, "settings: the processor uses the settings file it is given");
        check(rig.proc.getSkipSilence(), "settings: Skip Silence is on unless switched off");
        rig.proc.setSkipSilence(false);
        juce::PropertiesFile reread(settingsFile, options);
        check(! reread.getBoolValue("skipSilence", true), "settings: Skip Silence is saved");
        rig.proc.setSkipSilence(true);

        auto* shape = dynamic_cast<juce::AudioParameterChoice*>(param(rig.proc, ParamIDs::morphShape));
        check(shape != nullptr && shape->choices.size() == 5 && shape->choices[1] == "Cherries" && shape->choices[0] == "Lorenz"
                  && MorphPad::shapeNames()[1] == "Cherries" && static_cast<int>(MorphPad::Cherries) == 1,
              "morph: the second wander shape is Cherries (the others keep their places)");
    }

    // Stop: the tail rings out
    stopTest(sources, 0, 0.7f, 0.7f, "stop, Space and Fill up");
    stopTest(sources, 0, 0.0f, 0.0f, "stop, dry");
    stopTest(sources, 96000, 0.7f, 0.7f, "stop, rendering at 96 kHz");

    // Stop with Skip Silence on: the gate doesn't cut the tail short, and stops rolling once the rig rests.
    {
        Rig rig(sources, 0);
        auto& p = rig.proc;
        setNorm(p, ParamIDs::space, 0.7f);
        setNorm(p, ParamIDs::fill, 0.7f);
        setNorm(p, ParamIDs::energy, 0.0f); // steady: nothing for the gate to skip while playing
        p.prepareToPlay(c_fs, c_block);
        p.setSkipSilence(true);
        juce::String error;
        check(p.arm(error), "stop, Skip Silence: the tape arms " + error);
        p.setPlaying(true);
        rig.run(4.0);
        const double atStop = p.getRecordedSeconds();
        p.setPlaying(false);
        const auto after = rig.run(10.0);
        const double rest = lastSound(after);
        const double taped = p.getRecordedSeconds();
        const bool armedAtRest = p.isRecording(), capturingAtRest = p.isCapturing();
        // Play again: the gate opens with the new sound (nothing left over from before the rest in front of it).
        p.setPlaying(true);
        rig.run(1.0);
        const bool capturingAgain = p.isCapturing();
        p.setPlaying(false);
        p.stopRecording();
        check(armedAtRest && ! capturingAtRest, "stop, Skip Silence: at rest the tape is armed but no longer capturing");
        check(capturingAgain, "stop, Skip Silence: Play starts the tape capturing again");
        check(taped - atStop > rest - 0.3 && taped - atStop < rest + 0.1,
              "stop, Skip Silence: the take keeps the whole tail (" + juce::String(taped - atStop, 2) + " s on tape, the rig rang for "
                  + juce::String(rest, 2) + " s)");
    }

    // Skip Silence on the real rig: deep swells (Energy at full) leave gaps for the tape to skip; with it off the
    // take is everything.
    {
        double kept[2] {};
        for (int pass = 0; pass < 2; ++pass)
        {
            Rig rig(sources, 0);
            auto& p = rig.proc;
            setNorm(p, ParamIDs::energy, 1.0f);
            setNorm(p, ParamIDs::space, 0.0f);
            p.prepareToPlay(c_fs, c_block);
            p.setSkipSilence(pass == 0);
            juce::String error;
            check(p.arm(error), "skip silence: the tape arms " + error);
            p.setPlaying(true);
            rig.run(60.0);
            kept[pass] = p.getRecordedSeconds();
            check(p.getRecordingDroppedSeconds() < 1.0e-9, "skip silence: nothing dropped");
            p.stopRecording();
        }
        std::cout << "         60 s of the rig at full Energy: " << juce::String(kept[0], 1) << " s on tape with Skip Silence, "
                  << juce::String(kept[1], 1) << " s without" << std::endl;
        check(kept[1] > 59.5, "skip silence: off, the take is continuous (" + juce::String(kept[1], 1) + " s of 60 s)");
        check(kept[0] > 5.0 && kept[0] < kept[1] - 0.5, "skip silence: on, the quiet stretches are left out ("
                                                           + juce::String(kept[0], 1) + " s of 60 s)");
        settings.setValue("skipSilence", true);
    }

    // LAST 60s in the tape's format
    {
        Rig rig(sources, 0);
        auto& p = rig.proc;
        p.prepareToPlay(c_fs, c_block);
        p.setPlaying(true);
        rig.run(4.0);
        p.setFileType(2);
        p.setBitDepth(16);
        p.setMono(true);
        juce::File written;
        juce::String error;
        check(p.saveRetro(written, error), "LAST 60s: saves " + error);
        juce::AudioBuffer<float> audio;
        int bits = 0;
        readFile(written, audio, &bits);
        check(written.hasFileExtension("flac") && bits == 16 && audio.getNumChannels() == 1,
              "LAST 60s: written as the tape is set (FLAC, 16-bit, mono): " + written.getFileName());
        p.setFileType(0);
        p.setBitDepth(24);
        p.setMono(false);
    }

    // Session autosave
    {
        Rig rig(sources, 0);
        auto& p = rig.proc;
        // About 20 MB of audio in the session: six tracks of noise (FLAC can't shrink noise), 24 s each.
        {
            juce::Random r(9);
            juce::AudioBuffer<float> noise(1, static_cast<int>(24.0 * c_fs));
            for (int t = 0; t < 6; ++t)
            {
                for (int i = 0; i < noise.getNumSamples(); ++i)
                    noise.setSample(0, i, 0.5f * (r.nextFloat() * 2.0f - 1.0f));
                const auto f = sources.getChildFile("noise" + juce::String(t) + ".wav");
                writeWav(f, noise, c_fs);
                juce::String error;
                check(p.getCargo().loadFile(t, f, error), "autosave: loads a long track " + error);
            }
        }
        const auto file = folder.getChildFile("Swooshbuckler.autosave");
        file.deleteFile();
        SessionAutosave::Timing timing;
        timing.intervalMs = 400;
        timing.afterTrackMs = 100;
        auto autosave = std::make_unique<SessionAutosave>(p, &p.getCargo(), [&p] { return p.getMods().changeCount(); }, file, timing,
                                                          [&p] (int index) { return p.isUserEdit(index); });

        auto waitForSave = [&](int count, int timeoutMs)
        {
            const auto start = juce::Time::getMillisecondCounter();
            while (autosave->getSaveCount() < count && juce::Time::getMillisecondCounter() - start < static_cast<juce::uint32>(timeoutMs))
            {
                autosave->tick();
                juce::Thread::sleep(20);
            }
            return autosave->getSaveCount() >= count;
        };

        autosave->tick();
        juce::Thread::sleep(100);
        check(autosave->getSaveCount() == 0 && ! file.existsAsFile(), "autosave: nothing is written while nothing changes");

        // A parameter moves: one save, done on another thread (tick returns at once).
        setNorm(p, ParamIDs::energy, 0.8f);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        autosave->tick();
        const double tickMs = juce::Time::getMillisecondCounterHiRes() - t0;
        check(waitForSave(1, 60000) && file.existsAsFile(), "autosave: a changed parameter is saved");
        const double firstMs = autosave->getLastSaveMs();
        check(tickMs < 100.0, "autosave: the save does not run on the calling thread (the check took " + juce::String(tickMs, 2) + " ms)");

        // A wire, then a track change: each saved in turn, now with the audio already encoded.
        check(p.getMods().connect(0, ParamIDs::energy, 0.3f) >= 0, "autosave: a wire connects");
        check(waitForSave(2, 60000), "autosave: a changed wire is saved");
        const double secondMs = autosave->getLastSaveMs();
        p.getCargo().clear(5);
        autosave->noteTracksChanged();
        check(waitForSave(3, 60000), "autosave: a removed track is saved");
        std::cout << "         session file " << juce::String(file.getSize() / 1048576.0, 1) << " MB: the first save took "
                  << juce::String(firstMs, 0) << " ms (encoding the audio), later saves " << juce::String(secondMs, 0)
                  << " ms and " << juce::String(autosave->getLastSaveMs(), 0) << " ms, all off the interface thread" << std::endl;
        juce::Thread::sleep(100);
        autosave->tick();
        juce::Thread::sleep(100);
        check(autosave->getSaveCount() == 3, "autosave: no more saves once everything is on disk");

        // Only edits count. Wander moves the morph dot by itself for as long as it is on: switching it on is saved
        // once, the moving dot is not saved over and over. A new audio device (a latency change) is no edit either.
        {
            auto idle = [&](int ms, const std::function<void()>& during)
            {
                const auto start = juce::Time::getMillisecondCounter();
                while (juce::Time::getMillisecondCounter() - start < static_cast<juce::uint32>(ms))
                {
                    if (during) during();
                    autosave->tick();
                    juce::Thread::sleep(20);
                }
            };
            auto moveDot = [&](float x)
            {
                for (const char* id : { ParamIDs::morphX, ParamIDs::morphY })
                    if (auto* prm = param(p, id))
                    {
                        prm->beginChangeGesture();
                        prm->setValueNotifyingHost(x);
                        prm->endChangeGesture();
                    }
            };
            setNorm(p, ParamIDs::morphWander, 1.0f);
            check(waitForSave(4, 60000), "autosave: switching Wander on is saved");
            idle(500, {});
            float x = 0.1f;
            const int before = autosave->getSaveCount();
            idle(1500, [&] { x = x > 0.9f ? 0.1f : x + 0.01f; moveDot(x); });
            check(autosave->getSaveCount() == before, "autosave: the dot wandering by itself is not saved again and again ("
                                                          + juce::String(autosave->getSaveCount() - before) + " saves in 1.5 s, one was allowed every 0.4 s)");
            p.setLatencySamples(p.getLatencySamples() + 16);
            p.setLatencySamples(p.getLatencySamples() - 16);
            idle(700, {});
            check(autosave->getSaveCount() == before, "autosave: a latency change is not an edit");
            setNorm(p, ParamIDs::morphWander, 0.0f);
            check(waitForSave(before + 1, 60000), "autosave: switching Wander off is saved");
            idle(500, {});
            moveDot(0.3f);
            check(waitForSave(before + 2, 60000), "autosave: the dot moved by hand is saved");
            // A wire's depth dragged: saved (the wire itself was there already).
            idle(1200, {}); // everything so far is on disk
            const int settled = autosave->getSaveCount();
            idle(600, {});
            check(autosave->getSaveCount() == settled, "autosave: nothing more to save");
            const auto routes = p.getMods().getRoutes();
            check(! routes.empty() && p.getMods().setDepth(routes.front().slot, -0.6f), "autosave: a wire's depth changes");
            check(waitForSave(settled + 1, 5000), "autosave: a dragged wire depth is saved");
            idle(600, {});
            check(! routes.empty() && p.getMods().setDepth(routes.front().slot, -0.6f) && (idle(600, {}), autosave->getSaveCount() == settled + 1),
                  "autosave: the same depth again is not a change");
        }

        // A run that did not end cleanly leaves the file behind: the next run loads it.
        autosave.reset();
        check(file.existsAsFile(), "autosave: the copy stays if the app does not quit cleanly");
        {
            Rig next(sources, 0);
            next.proc.getCargo().clear(0);
            setNorm(next.proc, ParamIDs::energy, 0.1f);
            check(SessionAutosave::recover(next.proc, file), "autosave: the copy is recovered");
            const auto* energy = param(next.proc, ParamIDs::energy);
            check(energy != nullptr && std::abs(energy->getValue() - 0.8f) < 0.01f, "autosave: the recovered session has the parameters");
            check(next.proc.getCargo().getLive(0) != nullptr && next.proc.getCargo().getLive(4) != nullptr
                      && next.proc.getCargo().getLive(5) == nullptr && next.proc.getMods().getRoutes().size() == 1,
                  "autosave: the recovered session has the tracks and the wire");
            check(! file.existsAsFile(), "autosave: a recovered copy is not loaded twice");
        }

        // A clean quit removes the copy.
        {
            SessionAutosave clean(p, nullptr, {}, file, timing);
            clean.markChanged();
            const auto start = juce::Time::getMillisecondCounter();
            while (clean.getSaveCount() < 1 && juce::Time::getMillisecondCounter() - start < 60000u)
            {
                clean.tick();
                juce::Thread::sleep(20);
            }
            check(file.existsAsFile(), "autosave: saves again");
            clean.finish();
            check(! file.existsAsFile(), "autosave: a clean quit removes the copy");
        }
    }

    SwooshbucklerAudioProcessor::useSettings(nullptr);
    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
