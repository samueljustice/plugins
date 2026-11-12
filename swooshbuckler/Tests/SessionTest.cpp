// Session test: runs the real plug-in offline and checks what happens around the tape and the session.
//
//   - Stop: the sources let go and the echoes and Space ring out, into the output and into the tape, then rest.
//   - Skip Silence on the real rig, and a continuous take with it off.
//   - LAST 60s is written in the tape's format.
//   - The settings (shared file, Skip Silence on unless switched off), the wander shape names.
//   - Perform: Follow Host and the Play parameter, MIDI keys (Throw / Pitch / Swell), the morph wander in the processor.
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
        check(shape != nullptr && shape->choices.size() == 6 && shape->choices[1] == "Cherries" && shape->choices[0] == "Lorenz"
                  && shape->choices[4] == "Circle" && shape->choices[5] == "Butterfly"
                  && MorphPad::shapeNames()[1] == "Cherries" && static_cast<int>(MorphWander::Cherries) == 1
                  && MorphPad::shapeNames() == shape->choices && static_cast<int>(MorphWander::Butterfly) == 5,
              "morph: the second wander shape is Cherries, Butterfly is added last (the others keep their places)");
        // The last three host parameters are the newest, in this order (new ones only ever go on the end).
        const auto& all = rig.proc.getParameters();
        auto idAt = [&all](int fromEnd)
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(all[all.size() - fromEnd]);
            return ranged != nullptr ? ranged->getParameterID() : juce::String();
        };
        check(idAt(3) == ParamIDs::throwLength && idAt(2) == ParamIDs::play && idAt(1) == ParamIDs::keysMode && idAt(4) == ParamIDs::passMode[1],
              "parameters: Throw Length, Play and Keys are added after every older parameter");
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

    // Harvesting on the real plug-in: one file per whoosh with names, descriptions and markers; the takes; audition.
    {
        const auto harvest = folder.getChildFile("harvest");
        harvest.deleteRecursively();
        harvest.createDirectory();
        Rig rig(sources, 0);
        auto& p = rig.proc;
        check(! p.getOneFilePerWhoosh() && p.getTakeName().isEmpty() && p.getCreator().isEmpty() && p.getNameStyle() == 0 && p.getRetroMinutes() == 1,
              "harvest: One File Per Whoosh is off, names follow the preset with a counter, LAST 60s keeps a minute, unless set otherwise");
        p.setRecordFolder(harvest);
        p.setOneFilePerWhoosh(true);
        p.setTakeName("Fly By");
        p.setCreator("Anne Bonny");
        if (auto* length = param(p, ParamIDs::throwLength))
            length->setValueNotifyingHost(length->convertTo0to1(1.5f));
        p.prepareToPlay(c_fs, c_block);
        juce::String error;
        check(p.arm(error), "harvest: the tape arms " + error);
        // Three throws with the rig stopped, each left to ring out: three whooshes.
        for (int k = 0; k < 3; ++k)
        {
            p.throwWhoosh();
            rig.run(5.0);
        }
        check(p.isRecording() && ! p.isCapturing(), "harvest: the tape is still armed, waiting for the next whoosh");
        // The files of the first whooshes are closed and in the takes while the tape is still armed.
        for (int tries = 0; tries < 100 && p.getTakes().size() < 3; ++tries)
        {
            juce::Thread::sleep(20);
            p.refreshTakes(); // (the processor's own timer does this in the app; no message loop runs here)
        }
        const int whileArmed = static_cast<int>(p.getTakes().size());
        p.stopRecording();
        const auto takes = p.getTakes(); // newest first
        juce::StringArray names;
        bool cued = takes.size() == 3, described = takes.size() == 3;
        for (const auto& take : takes)
        {
            names.add(take.file.getFileName() + " (" + take.kind + ")");
            juce::AudioBuffer<float> audio;
            const double rate = readFile(take.file, audio);
            // One Throw marker (with the hit aimed at it), where the file is loudest.
            const auto cues = TapeRecorder::readCues(take.file);
            int loudest = 0;
            float best = 0.0f;
            const int win = static_cast<int>(0.05 * rate);
            for (int at = 0; at + win < audio.getNumSamples(); at += win / 4)
                if (const float v = audio.getRMSLevel(0, at, win) + audio.getRMSLevel(1, at, win); v > best)
                {
                    best = v;
                    loudest = at + win / 2;
                }
            int throws = 0;
            double throwAt = -1.0;
            for (const auto& cue : cues)
                if (cue.label.contains("Throw"))
                {
                    ++throws;
                    throwAt = static_cast<double>(cue.frame) / rate;
                }
            cued = cued && throws == 1 && take.markers == static_cast<int>(cues.size()) && std::abs(throwAt - loudest / rate) < 0.25
                   && std::abs(take.seconds - audio.getNumSamples() / rate) < 0.01 && take.seconds > 1.0 && take.seconds < 4.5;
            std::cout << "         " << take.file.getFileName() << ": " << juce::String(take.seconds, 2) << " s, loudest at "
                      << juce::String(loudest / rate, 2) << " s, cues:";
            for (const auto& cue : cues)
                std::cout << " [" << cue.label << " @ " << juce::String(static_cast<double>(cue.frame) / rate, 2) << "]";
            std::cout << std::endl;
            const auto description = TapeRecorder::readDescription(take.file);
            std::unique_ptr<juce::AudioFormatReader> r(juce::WavAudioFormat().createReaderFor(new juce::FileInputStream(take.file), true));
            described = described && description.startsWith("Swooshbuckler " + juce::String(JucePlugin_VersionString) + ". Preset: Default. Energy ")
                        && description.contains("Pass A Bed") && r != nullptr
                        && r->metadataValues[juce::WavAudioFormat::bwavOriginator] == "Anne Bonny";
            if (&take == &takes.front())
                std::cout << "         description: " << description << std::endl;
        }
        check(names.joinIntoString(", ") == "Fly By_0003.wav (Whoosh), Fly By_0002.wav (Whoosh), Fly By_0001.wav (Whoosh)",
              "harvest: three throws, three numbered whoosh files in the takes, newest first (" + names.joinIntoString(", ") + ")");
        check(whileArmed >= 2 && p.getFilesThisTake() == 3 && harvest.getNumberOfChildFiles(juce::File::findFiles) == 3,
              "harvest: they are listed as each whoosh ends, and nothing else is written (" + juce::String(whileArmed) + " listed while armed)");
        check(cued, "harvest: each file has its Throw marker at its loudest moment");
        check(described, "harvest: each file says what made it and who");

        // Audition: a take plays through the output with the rig at rest, and is not recorded.
        if (! takes.empty())
        {
            const auto silent = rig.run(1.0);
            check(peak(silent, 0.0, 1.0) == 0.0f && p.getAuditionTake() == -1, "audition: the rig is at rest and nothing is playing");
            check(p.arm(error), "audition: the tape arms again " + error);
            check(p.auditionTake(takes.front().id, error) && p.getAuditionTake() == takes.front().id, "audition: a take starts playing " + error);
            juce::AudioBuffer<float> heard(2, 0);
            double progressMid = 0.0;
            for (int k = 0; k < 12 && p.getAuditionTake() >= 0; ++k)
            {
                const auto part = rig.run(0.5);
                const int had = heard.getNumSamples();
                heard.setSize(2, had + part.getNumSamples(), true);
                for (int ch = 0; ch < 2; ++ch)
                    heard.copyFrom(ch, had, part, ch, 0, part.getNumSamples());
                if (k == 1)
                    progressMid = p.getAuditionProgress();
                juce::Thread::sleep(30); // the reader thread tops the playback up
            }
            juce::AudioBuffer<float> original;
            readFile(takes.front().file, original);
            const float heardPeak = heard.getMagnitude(0, heard.getNumSamples()), filePeak = original.getMagnitude(0, original.getNumSamples());
            const double heardSeconds = lastSound(heard);
            check(std::abs(heardPeak - filePeak) < 0.05f * filePeak && std::abs(heardSeconds - takes.front().seconds) < 0.3 && p.getAuditionTake() == -1,
                  "audition: the whole take is heard at its own level, then it stops (" + juce::String(heardSeconds, 2) + " s of "
                      + juce::String(takes.front().seconds, 2) + " s, peak " + juce::String(heardPeak, 3) + " of " + juce::String(filePeak, 3) + ")");
            check(progressMid > 0.1 && progressMid < 0.9, "audition: it says how far through it is (" + juce::String(progressMid, 2) + ")");
            p.stopRecording();
            check(p.getFilesThisTake() == 0 && p.getTakes().size() == takes.size(), "audition: what is auditioned is not recorded");
            // Stopping part-way fades it out.
            check(p.auditionTake(takes.back().id, error), "audition: another take starts " + error);
            rig.run(0.5);
            p.stopAudition();
            const auto fading = rig.run(0.25);
            check(p.getAuditionTake() == -1 && peak(fading, 0.1, 0.25) == 0.0f, "audition: stop fades it out within a few milliseconds");
        }

        // The takes: stars keep, a full list drops the oldest without a star, delete moves the file to the trash.
        if (takes.size() == 3)
        {
            const int oldest = takes.back().id;
            p.setTakeStarred(oldest, true);
            for (int k = 0; k < SwooshbucklerAudioProcessor::c_maxTakes; ++k)
            {
                const auto extra = harvest.getChildFile("extra_" + juce::String(k) + ".wav");
                takes.front().file.copyFileTo(extra);
                p.registerTake(extra, "Render");
            }
            const auto& now = p.getTakes();
            const bool kept = std::any_of(now.begin(), now.end(), [oldest](const auto& t) { return t.id == oldest && t.starred; });
            const bool others = std::none_of(now.begin(), now.end(), [](const auto& t) { return t.kind == "Whoosh" && ! t.starred; });
            check(static_cast<int>(now.size()) == SwooshbucklerAudioProcessor::c_maxTakes && kept && others && now.front().kind == "Render"
                      && std::abs(now.front().seconds - takes.front().seconds) < 0.01,
                  "takes: the list holds 20; a starred take stays, the oldest others leave; a registered file is listed with its length");
            const auto victim = now.front();
            const int before = p.getTakesGeneration();
            check(p.deleteTake(victim.id, false) && ! victim.file.exists() && static_cast<int>(p.getTakes().size()) == SwooshbucklerAudioProcessor::c_maxTakes - 1
                      && p.getTakesGeneration() != before,
                  "takes: delete takes the file out of the folder and off the list");
        }

        // LAST 60s: named and listed like the rest; its length is a setting.
        {
            p.setOneFilePerWhoosh(false);
            p.setPlaying(true);
            rig.run(3.0);
            juce::File written;
            check(p.saveRetro(written, error) && written.getFileName() == "Fly By_0004_retro.wav" && p.getTakes().front().file == written
                      && p.getTakes().front().kind == "Last 60s",
                  "harvest: LAST 60s is named by the same counter and listed (" + written.getFileName() + ") " + error);
            p.setRetroMinutes(3);
            juce::PropertiesFile reread(settingsFile, options);
            check(p.getRetroMinutes() == 3 && reread.getIntValue("retroMinutes") == 3 && reread.getBoolValue("oneFilePerWhoosh", true) == false
                      && reread.getValue("takeName") == "Fly By" && reread.getValue("creator") == "Anne Bonny",
                  "harvest: the settings are saved");
            p.setPlaying(false);
        }
        p.setRetroMinutes(1);
        p.setTakeName({});
        p.setCreator({});
        p.setRecordFolder(tapeFolder);
    }

    // Perform: Follow Host, the Play parameter, MIDI keys and the wander, on the real plug-in.
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
        // Renders `seconds`; notes: (seconds from the start, note, velocity 0 = note off).
        struct Note { double at; int note; float velocity; };
        auto render = [](SwooshbucklerAudioProcessor& p, double seconds, const std::vector<Note>& notes = {})
        {
            const int total = static_cast<int>(seconds * c_fs) / c_block * c_block;
            juce::AudioBuffer<float> out(2, total);
            for (int pos = 0; pos < total; pos += c_block)
            {
                juce::MidiBuffer midi;
                for (const auto& n : notes)
                    if (const int at = static_cast<int>(n.at * c_fs); at >= pos && at < pos + c_block)
                        midi.addEvent(n.velocity > 0.0f ? juce::MidiMessage::noteOn(1, n.note, n.velocity) : juce::MidiMessage::noteOff(1, n.note), at - pos);
                juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, pos, c_block);
                view.clear();
                p.processBlock(view, midi);
            }
            return out;
        };
        auto maxDiff = [](const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            float d = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < juce::jmin(a.getNumSamples(), b.getNumSamples()); ++i)
                    d = std::max(d, std::abs(a.getSample(ch, i) - b.getSample(ch, i)));
            return d;
        };

        // Follow Host: plays with the transport, stops with a tail, and every start is the same ride (the morph
        // wandering and a modulator running too: both are part of the bounce).
        {
            Rig rig(sources, 0);
            auto& p = rig.proc;
            Transport transport;
            p.setPlayHead(&transport);
            setNorm(p, ParamIDs::morphOn, 1.0f);
            setNorm(p, ParamIDs::morphWander, 1.0f);
            setNorm(p, ParamIDs::morphSpeed, 0.9f);
            p.prepareToPlay(c_fs, c_block);
            check(p.getFollowHost() && p.canFollowHost(), "follow host: on unless switched off (in a host)");
            check(p.getTailLengthSeconds() >= SwooshbucklerAudioProcessor::c_tailMaxSeconds, "follow host: the host is told how long the tail can be");
            const auto idle = render(p, 1.0);
            check(peak(idle, 0.0, 1.0) == 0.0f && ! p.isPlaying(), "follow host: silent while the transport is stopped");
            transport.playing = true;
            const auto first = render(p, 4.0);
            check(p.isPlaying() && peak(first, 0.5, 4.0) > 0.02f, "follow host: the rig plays when the transport starts");
            transport.playing = false;
            const auto tail = render(p, 9.0);
            check(! p.isPlaying() && peak(tail, 0.0, 0.3) > 1.0e-4f && lastSound(tail) > 0.3 && lastSound(tail) < 8.0,
                  "follow host: the transport stops the rig and the tail rings out (" + juce::String(lastSound(tail), 2) + " s)");
            float x1 = 0.0f, y1 = 0.0f, rotation = 0.0f;
            transport.playing = true;
            const auto second = render(p, 4.0);
            p.getMorphPosition(x1, y1, rotation);
            const float differs = maxDiff(first, second);
            std::cout << "         follow host: two transport starts differ by at most " << juce::String(db(differs), 1) << " dB (level "
                      << juce::String(db(peak(first, 0.0, 4.0)), 1) << " dB peak)" << std::endl;
            check(differs < 1.0e-4f, "follow host: every transport start gives the same ride (a bounce repeats)");
            // Started in the middle of playing (not from rest): the same again.
            transport.playing = false;
            render(p, 0.2);
            transport.playing = true;
            const auto third = render(p, 4.0);
            // (the old tail is faded out over the first 5 ms, and the output stage is still settling from it for a while)
            float differsMid = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = static_cast<int>(0.25 * c_fs); i < third.getNumSamples(); ++i)
                    differsMid = std::max(differsMid, std::abs(first.getSample(ch, i) - third.getSample(ch, i)));
            std::cout << "         follow host: a start 0.2 s after a stop (the tail still ringing) differs from the first by at most "
                      << juce::String(db(differsMid), 1) << " dB after its first 250 ms" << std::endl;
            check(differsMid < 0.02f, "follow host: a start over a ringing tail is the same ride too");
            // A host that prepares again and carries straight on playing (a bounce after live playback, with no
            // stopped block in between) gets the same ride from the top too.
            p.prepareToPlay(c_fs, c_block);
            const auto bounced = render(p, 4.0);
            float differsBounce = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = static_cast<int>(0.25 * c_fs); i < bounced.getNumSamples(); ++i)
                    differsBounce = std::max(differsBounce, std::abs(first.getSample(ch, i) - bounced.getSample(ch, i)));
            std::cout << "         follow host: prepared again while the transport plays, the ride differs from the first by at most "
                      << juce::String(db(differsBounce), 1) << " dB after its first 250 ms" << std::endl;
            check(p.isPlaying() && differsBounce < 0.02f, "follow host: preparing again with the transport playing starts the same ride");
            // Play still works by hand while the transport runs or rests.
            p.setPlaying(false);
            render(p, 8.0);
            check(! p.isPlaying() && peak(render(p, 0.5), 0.0, 0.5) == 0.0f, "follow host: Play still stops the rig by hand while the transport runs");
            // Off: the transport is ignored.
            p.setFollowHost(false);
            transport.playing = false;
            render(p, 0.5);
            transport.playing = true;
            check(peak(render(p, 1.0), 0.0, 1.0) == 0.0f && ! p.isPlaying(), "follow host: off, the transport does not start the rig");

            // The wander is heard in the bounce: the same start without it is a different sound.
            p.setFollowHost(true);
            setNorm(p, ParamIDs::morphWander, 0.0f);
            transport.playing = false;
            render(p, 8.0);
            transport.playing = true;
            const auto still = render(p, 4.0);
            check(maxDiff(first, still) > 1.0e-3f, "wander: it moves the morph in an offline render (with no window)");

            // The play state and Follow Host are saved with the plug-in's state.
            p.setFollowHost(false);
            p.setPlaying(true);
            juce::MemoryBlock saved;
            p.getStateInformation(saved);
            Rig other(sources, 0);
            other.proc.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
            other.proc.prepareToPlay(c_fs, c_block);
            check(! other.proc.getFollowHost(), "state: Follow Host is saved");
            check(param(other.proc, ParamIDs::play)->getValue() > 0.5f, "state: the play state is saved");
            check(peak(render(other.proc, 2.0), 0.5, 2.0) > 0.02f && other.proc.isPlaying(), "state: a session saved playing plays when it is opened in a host");
        }

        // Play is a host parameter: automation starts and stops the rig, and the button moves the parameter.
        {
            Rig rig(sources, 0);
            auto& p = rig.proc;
            p.prepareToPlay(c_fs, c_block);
            auto* play = param(p, ParamIDs::play);
            check(play != nullptr && play->isAutomatable(), "play: an automatable host parameter");
            setNorm(p, ParamIDs::play, 1.0f);
            check(peak(render(p, 2.0), 0.5, 2.0) > 0.02f && p.isPlaying(), "play: the parameter starts the rig");
            setNorm(p, ParamIDs::play, 0.0f);
            render(p, 0.1);
            check(! p.isPlaying(), "play: the parameter stops the rig");
            p.setPlaying(true);
            check(play->getValue() > 0.5f, "play: the Play button moves the parameter");
            render(p, 0.5);
            check(p.isPlaying(), "play: and the rig keeps playing");
            p.setPlaying(false);
        }

        // Keys.
        {
            Rig rig(sources, 0);
            auto& p = rig.proc;
            setNorm(p, ParamIDs::space, 0.0f);
            setNorm(p, ParamIDs::fill, 0.0f);
            setNorm(p, ParamIDs::throwLength, param(p, ParamIDs::throwLength)->convertTo0to1(1.0f)); // 1 s throws
            p.prepareToPlay(c_fs, c_block);

            // Throw: one whoosh per note, from rest, and silence again afterwards.
            setNorm(p, ParamIDs::keysMode, 1.0f);
            const int count0 = p.getEngine().getThrowCount();
            const auto thrown = render(p, 8.0, { { 0.5, 60, 0.9f }, { 0.6, 60, 0.0f }, { 1.5, 64, 0.9f }, { 1.6, 64, 0.0f }, { 2.5, 67, 0.9f }, { 2.6, 67, 0.0f } });
            check(p.getEngine().getThrowCount() - count0 == 3, "keys, Throw: three notes throw three whooshes ("
                                                                    + juce::String(p.getEngine().getThrowCount() - count0) + ")");
            check(peak(thrown, 0.0, 0.45) == 0.0f && peak(thrown, 0.8, 1.2) > 0.01f && peak(thrown, 1.8, 2.2) > 0.01f && peak(thrown, 2.8, 3.2) > 0.01f,
                  "keys, Throw: each is heard, peaking half a Throw Length after its note");
            check(! p.isPlaying() && lastSound(thrown) < 7.5, "keys, Throw: with the rig stopped it is silent again afterwards (last sound at "
                                                               + juce::String(lastSound(thrown), 2) + " s)");
            // Three notes together are one whoosh with the three pitches dealt over the tracks.
            const int count1 = p.getEngine().getThrowCount();
            render(p, 0.5, { { 0.1, 60, 0.9f }, { 0.1, 64, 0.9f }, { 0.105, 67, 0.9f } });
            check(p.getEngine().getThrowCount() - count1 == 1, "keys, Throw: a chord is one whoosh");
            check(juce::exactlyEqual(p.getEngine().getKeyPitch(0), 0.0f) && juce::exactlyEqual(p.getEngine().getKeyPitch(1), 4.0f)
                      && juce::exactlyEqual(p.getEngine().getKeyPitch(2), 7.0f),
                  "keys, Throw: the chord's notes pitch the tracks in order");
            check((p.getHeldKeys(0) >> 60 & 1u) != 0 && (p.getHeldKeys(1) >> (64 - 64) & 1u) != 0 && (p.getHeldKeys(1) >> (67 - 64) & 1u) != 0,
                  "keys: the held notes are published for the display");
            render(p, 8.0, { { 0.1, 60, 0.0f }, { 0.1, 64, 0.0f }, { 0.1, 67, 0.0f } });

            // The Throw button (and the F key) from rest.
            const int count2 = p.getEngine().getThrowCount();
            p.throwWhoosh();
            const auto button = render(p, 6.0);
            check(p.getEngine().getThrowCount() - count2 == 1 && peak(button, 0.3, 0.7) > 0.01f && lastSound(button) < 5.5 && ! p.isPlaying(),
                  "throw: the button throws one whoosh from rest, then silence");

            // Pitch: held keys pitch the tracks while the rig plays; letting go returns to the knob.
            setNorm(p, ParamIDs::keysMode, 0.5f);
            p.setPlaying(true);
            render(p, 1.0, { { 0.2, 55, 0.8f } });
            check(juce::exactlyEqual(p.getEngine().getKeyPitch(0), -5.0f) && juce::exactlyEqual(p.getEngine().getKeyPitch(2), -5.0f),
                  "keys, Pitch: one held key moves every track by its distance from middle C");
            const int count3 = p.getEngine().getThrowCount();
            render(p, 0.5, { { 0.1, 55, 0.0f } });
            check(juce::exactlyEqual(p.getEngine().getKeyPitch(0), 0.0f) && p.getEngine().getThrowCount() == count3, "keys, Pitch: released, back to the knob (and nothing is thrown)");
            // Swell: a held note holds Swell and pitches nothing.
            setNorm(p, ParamIDs::keysMode, 0.0f);
            render(p, 0.5, { { 0.1, 72, 0.8f } });
            check(juce::exactlyEqual(p.getEngine().getKeyPitch(0), 0.0f) && (p.getHeldKeys(1) >> (72 - 64) & 1u) != 0, "keys, Swell: a held note pitches nothing");
            p.setPlaying(false);
        }
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
            // The processor flies the dot itself and leaves the X / Y controls alone, so nothing is there to save.
            p.prepareToPlay(c_fs, c_block);
            const float xBefore = param(p, ParamIDs::morphX)->getValue(), yBefore = param(p, ParamIDs::morphY)->getValue();
            setNorm(p, ParamIDs::morphWander, 1.0f);
            check(waitForSave(4, 60000), "autosave: switching Wander on is saved");
            idle(500, {});
            const int before = autosave->getSaveCount();
            float travelled = 0.0f, lastX = 0.0f, lastY = 0.0f, rotation = 0.0f;
            p.getMorphPosition(lastX, lastY, rotation);
            idle(1500, [&]
            {
                juce::AudioBuffer<float> block(2, c_block);
                juce::MidiBuffer midi;
                for (int n = 0; n < 8; ++n) // the rig stopped, as when the app has just opened: the dot still flies
                {
                    block.clear();
                    p.processBlock(block, midi);
                }
                float x = 0.0f, y = 0.0f;
                p.getMorphPosition(x, y, rotation);
                travelled += std::hypot(x - lastX, y - lastY);
                lastX = x;
                lastY = y;
            });
            check(travelled > 0.2f, "wander: the processor flies the dot with no window open (it travelled " + juce::String(travelled, 2) + " pad widths)");
            check(juce::exactlyEqual(param(p, ParamIDs::morphX)->getValue(), xBefore) && juce::exactlyEqual(param(p, ParamIDs::morphY)->getValue(), yBefore),
                  "wander: the X and Y controls are left where they were");
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
