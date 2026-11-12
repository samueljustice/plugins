// Render test: Render Variations through the whole plug-in, offline.
//
//   - A batch of five in each mode: five files with the right names, lengths within reason, peaks under the
//     Ceiling, a description in each, and no two files alike.
//   - The same batch seed gives the same files again, sample for sample; another seed gives others.
//   - The rig that is rendered from is left exactly as it was.
//   - Cancelling part-way keeps the finished files and leaves nothing half-written.
//   - Numbers carry on after the files a folder already has; the files follow the tape's format.
//   - A rig with nothing to play fails with a message instead of writing empty files.
//   - The files join the Takes.
//   - How fast a batch renders (--bench: 100 whooshes of 3 s).
//
// Usage: SwooshbucklerRender [scratch folder] [--bench]. Nothing is read from or written to the user's settings.

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "VariationRenderer.h"

#include <cmath>
#include <iostream>

namespace
{
    constexpr double c_fs = 48000.0;
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
                // Each growl at its own pitch, so which one a whoosh picks can be heard (and measured).
                ph += (90.0 + 35.0 * std::floor(t / 1.5) + 40.0 * std::sin(6.0 * t)) / c_fs;
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

    struct Rig
    {
        std::unique_ptr<juce::AudioProcessor> base { createPluginFilter() };
        SwooshbucklerAudioProcessor& proc { *dynamic_cast<SwooshbucklerAudioProcessor*>(base.get()) };

        explicit Rig(const juce::File& sources, bool load = true)
        {
            proc.setPlayConfigDetails(0, 2, c_fs, 512);
            proc.overrideRecordRate(0);
            const char* names[] = { "growl", "wind", "hits" };
            const CargoRole roles[] = { CargoRole::Tonal, CargoRole::Bed, CargoRole::Impact };
            for (int t = 0; t < 3 && load; ++t)
            {
                juce::String error;
                if (! proc.getCargo().loadFile(t, sources.getChildFile(juce::String(names[t]) + ".wav"), error))
                    check(false, "loads " + juce::String(names[t]) + ": " + error);
                proc.getCargo().setRoleOverride(t, static_cast<int>(roles[t]));
            }
        }

        std::vector<float> values() const
        {
            std::vector<float> v;
            for (auto* p : proc.getParameters())
                v.push_back(p->getValue());
            for (int t = 0; t < CargoHold::c_numTracks; ++t)
                v.push_back(proc.getCargo().getStart(t));
            return v;
        }
    };

    struct Audio
    {
        juce::AudioBuffer<float> samples;
        double rate = 0.0;
        int bits = 0;
        double seconds() const { return rate > 0.0 ? samples.getNumSamples() / rate : 0.0; }
        float peak() const { return samples.getNumSamples() > 0 ? samples.getMagnitude(0, samples.getNumSamples()) : 0.0f; }
    };

    Audio readFile(const juce::File& f)
    {
        Audio a;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(f));
        if (r == nullptr)
            return a;
        a.samples.setSize(static_cast<int>(r->numChannels), static_cast<int>(r->lengthInSamples));
        r->read(&a.samples, 0, static_cast<int>(r->lengthInSamples), 0, true, true);
        a.rate = r->sampleRate;
        a.bits = static_cast<int>(r->bitsPerSample);
        return a;
    }

    /** How unlike two files are: 0 = the same samples; otherwise the RMS of their difference against the RMS of
        the first (over the shorter of the two), or 1 when their lengths differ by more than a block. */
    double difference(const Audio& a, const Audio& b)
    {
        if (std::abs(a.samples.getNumSamples() - b.samples.getNumSamples()) > 512 || a.samples.getNumChannels() != b.samples.getNumChannels())
            return 1.0;
        const int n = juce::jmin(a.samples.getNumSamples(), b.samples.getNumSamples());
        double diff = 0.0, level = 0.0;
        for (int ch = 0; ch < a.samples.getNumChannels(); ++ch)
            for (int i = 0; i < n; ++i)
            {
                const double d = static_cast<double>(a.samples.getSample(ch, i)) - b.samples.getSample(ch, i);
                diff += d * d;
                level += static_cast<double>(a.samples.getSample(ch, i)) * a.samples.getSample(ch, i);
            }
        if (a.samples.getNumSamples() != b.samples.getNumSamples() && diff <= 0.0)
            return 1.0;
        return level > 0.0 ? std::sqrt(diff / level) : (diff > 0.0 ? 1.0 : 0.0);
    }

    VariationRenderer::Job makeJob(Rig& rig, const juce::File& folder, VariationRenderer::Mode mode, int count, double seconds, std::uint32_t seed)
    {
        auto job = rig.proc.getVariations().defaultJob();
        job.folder = folder;
        job.mode = mode;
        job.count = count;
        job.seconds = seconds;
        job.batchSeed = seed;
        job.name = "Whoosh";
        job.rate = c_fs;
        job.options.bitDepth = 24;
        job.options.type = TapeRecorder::FileType::Wav;
        job.options.mono = false;
        return job;
    }

    /** Runs a batch to its end and returns what the renderer says about it. */
    VariationRenderer::Progress render(Rig& rig, const VariationRenderer::Job& job)
    {
        auto& renderer = rig.proc.getVariations();
        juce::String error;
        if (! renderer.start(job, error))
        {
            check(false, "the batch starts: " + error);
            return {};
        }
        check(renderer.waitUntilDone(600000), "the batch ends");
        return renderer.getProgress();
    }

    juce::Array<juce::File> filesIn(const juce::File& folder)
    {
        auto files = folder.findChildFiles(juce::File::findFiles, false);
        files.sort();
        return files;
    }

    void batchTest(Rig& rig, const juce::File& folder, VariationRenderer::Mode mode, double seconds)
    {
        const auto label = VariationRenderer::modeName(mode);
        folder.deleteRecursively();
        const auto progress = render(rig, makeJob(rig, folder, mode, 5, seconds, 4711));
        check(progress.state == VariationRenderer::State::Finished && progress.done == 5 && progress.total == 5 && progress.silent == 0,
              label + ": the batch finishes with 5 of 5 files");
        const auto files = filesIn(folder);
        bool names = files.size() == 5;
        for (int i = 0; i < files.size() && names; ++i)
            names = files[i].getFileName() == "Whoosh_00" + juce::String(i + 1) + ".wav";
        check(names, label + ": the folder holds Whoosh_001.wav to Whoosh_005.wav and nothing else (" + juce::String(files.size()) + " files)");
        check(rig.proc.getVariations().getFiles().size() == 5, label + ": the renderer lists its five files");

        const float ceiling = juce::Decibels::decibelsToGain(rig.proc.getState().getRawParameterValue(ParamIDs::ceiling)->load());
        std::vector<Audio> audio;
        bool lengths = true, peaks = true, sound = true, starts = true, ends = true, described = true, format = true;
        juce::String report;
        for (int i = 0; i < files.size(); ++i)
        {
            audio.push_back(readFile(files[i]));
            const auto& a = audio.back();
            const double s = a.seconds();
            // The whoosh itself is most of its Throw Length; after it the tail may ring for up to six seconds and fade.
            lengths = lengths && s > 0.5 * seconds && s < seconds + SwooshbucklerAudioProcessor::c_tailMaxSeconds + 1.0;
            peaks = peaks && a.peak() <= ceiling * 1.001f;
            sound = sound && a.peak() > juce::Decibels::decibelsToGain(-30.0f);
            format = format && a.rate == c_fs && a.bits == 24 && a.samples.getNumChannels() == 2;
            if (a.samples.getNumSamples() > 480)
            {
                // The silence in front is gone (the first sample is the first over -80 dBFS), and the end is faded.
                starts = starts && a.samples.getMagnitude(0, 1) > 0.9f * VariationRenderer::c_silence && a.samples.getMagnitude(0, 1) < 0.05f;
                ends = ends && a.samples.getMagnitude(a.samples.getNumSamples() - 48, 48) < juce::Decibels::decibelsToGain(-60.0f);
            }
            const auto text = TapeRecorder::readDescription(files[i]);
            described = described && text.contains("Variation " + juce::String(i + 1) + " of 5") && text.contains("Batch Seed 4711")
                     && text.contains(label) && text.contains("Energy ");
            report << juce::String(s, 2) << " s " << juce::String(db(a.peak()), 1) << " dB" << (i + 1 < files.size() ? ", " : "");
        }
        std::cout << "         " << label << ": " << report << "  (" << juce::String(progress.audioSeconds / juce::jmax(0.001, progress.elapsedSeconds), 1)
                  << "x real time)" << std::endl;
        check(format, label + ": the files are 24-bit stereo at 48 kHz, as asked");
        check(lengths, label + ": every file is as long as a " + juce::String(seconds, 1) + " s whoosh and its tail");
        check(sound, label + ": every file has a whoosh in it (over -30 dBFS)");
        check(peaks, label + ": every peak is under the Ceiling (" + juce::String(db(ceiling), 2) + " dB)");
        check(starts, label + ": every file starts at its first sound");
        check(ends, label + ": every file ends faded out");
        check(described, label + ": every file says which variation of which batch it is, and its settings");

        double least = 10.0;
        for (size_t i = 0; i < audio.size(); ++i)
            for (size_t j = i + 1; j < audio.size(); ++j)
                least = juce::jmin(least, difference(audio[i], audio[j]));
        check(audio.size() == 5 && least > 0.1, label + ": no two files are alike (the closest pair differ by " + juce::String(juce::roundToInt(100.0 * least)) + "% RMS)");
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SwooshbucklerRenderTest");
    bool bench = false;
    for (int i = 1; i < argc; ++i)
    {
        if (juce::String(argv[i]) == "--bench")
            bench = true;
        else
            folder = juce::File(argv[i]);
    }
    folder.createDirectory();
    const auto sources = folder.getChildFile("sources");
    sources.createDirectory();
    makeSources(sources);
    const auto out = folder.getChildFile("variations");
    out.deleteRecursively();
    out.createDirectory();

    // A scratch settings file, shared with every processor made below (the user's own is never touched).
    juce::PropertiesFile::Options options;
    options.applicationName = "SwooshbucklerRenderTest";
    options.filenameSuffix = ".settings";
    const auto settingsFile = folder.getChildFile("test.settings");
    settingsFile.deleteFile();
    juce::PropertiesFile settings(settingsFile, options);
    settings.setValue("recordFolder", out.getFullPathName());
    SwooshbucklerAudioProcessor::useSettings(&settings);

    {
        Rig rig(sources);
        auto& renderer = rig.proc.getVariations();

        // What a new batch is set to.
        {
            const auto job = renderer.defaultJob();
            check(job.count == 100, "defaults: 100 variations");
            check(job.mode == VariationRenderer::Mode::RandomiseEverything, "defaults: Randomise Everything");
            check(std::abs(job.seconds - Params::throwSeconds(0.0f, rig.proc.getState().getRawParameterValue(ParamIDs::rate)->load())) < 1.0e-3,
                  "defaults: the length is Throw Length (here Rate's pass length, " + juce::String(job.seconds, 2) + " s)");
            check(job.folder.getParentDirectory() == out && job.folder.getFileName().startsWith("Swooshbuckler Variations 20"),
                  "defaults: a dated folder inside the recordings folder (" + job.folder.getFileName() + ")");
            check(job.options.bitDepth == 24 && job.options.type == TapeRecorder::FileType::Wav && ! job.options.mono && job.rate == c_fs,
                  "defaults: the tape's file type, bit depth, rate and channels");
            rig.proc.setVariationCount(5000);
            check(rig.proc.getVariationCount() == VariationRenderer::c_maxCount, "settings: the count is held to 1000");
            rig.proc.setVariationCount(100);
            check(VariationRenderer::fileNameOf("A/B", 7, 100) == "AB_007" && VariationRenderer::fileNameOf("Whoosh", 12, 1000) == "Whoosh_0012",
                  "names: three digits, four for a batch that reaches 1000, and only characters a file name may have");
            check(VariationRenderer::seedOf(1, 0) != VariationRenderer::seedOf(1, 1) && VariationRenderer::seedOf(1, 0) != VariationRenderer::seedOf(2, 0)
                      && VariationRenderer::seedOf(77, 5) == VariationRenderer::seedOf(77, 5),
                  "seeds: each file's seed comes from the batch seed and its place, and only from them");
        }

        // A batch of five in each mode, from a rig that must come out of it unchanged.
        const auto before = rig.values();
        batchTest(rig, out.getChildFile("randomise"), VariationRenderer::Mode::RandomiseEverything, 1.0);
        batchTest(rig, out.getChildFile("small"), VariationRenderer::Mode::SmallChanges, 1.0);
        batchTest(rig, out.getChildFile("seed"), VariationRenderer::Mode::SeedOnly, 1.0);
        check(rig.values() == before, "the rig that was rendered from is exactly as it was (every parameter and start marker)");
        check(! rig.proc.isPlaying() && ! rig.proc.isRecording(), "the rig was neither started nor recorded");

        // The same batch seed again: the same files. Another seed: other files.
        for (const auto mode : { VariationRenderer::Mode::RandomiseEverything, VariationRenderer::Mode::SmallChanges, VariationRenderer::Mode::SeedOnly })
        {
            const auto label = VariationRenderer::modeName(mode);
            const char* folders[] = { "randomise", "small", "seed" };
            const auto first = filesIn(out.getChildFile(folders[static_cast<int>(mode)]));
            const auto againFolder = out.getChildFile("again");
            againFolder.deleteRecursively();
            render(rig, makeJob(rig, againFolder, mode, 5, 1.0, 4711));
            const auto again = filesIn(againFolder);
            double worst = again.size() == first.size() && first.size() == 5 ? 0.0 : 1.0;
            for (int i = 0; i < juce::jmin(first.size(), again.size()); ++i)
                worst = juce::jmax(worst, difference(readFile(first[i]), readFile(again[i])));
            check(worst == 0.0, label + ": the same batch seed gives the same five files, sample for sample");

            againFolder.deleteRecursively();
            render(rig, makeJob(rig, againFolder, mode, 5, 1.0, 4712));
            const auto other = filesIn(againFolder);
            double least = other.size() == 5 && first.size() == 5 ? 10.0 : 0.0;
            for (int i = 0; i < juce::jmin(first.size(), other.size()); ++i)
                least = juce::jmin(least, difference(readFile(first[i]), readFile(other[i])));
            check(least > 0.1, label + ": another batch seed gives other files");
            againFolder.deleteRecursively();
        }

        // Numbers carry on after the files a folder already has.
        {
            const auto seedFolder = out.getChildFile("seed");
            const auto progress = render(rig, makeJob(rig, seedFolder, VariationRenderer::Mode::SeedOnly, 2, 0.5, 9));
            const auto files = filesIn(seedFolder);
            check(progress.done == 2 && files.size() == 7 && files[5].getFileName() == "Whoosh_006.wav" && files[6].getFileName() == "Whoosh_007.wav",
                  "a second batch into the same folder carries on at Whoosh_006 and writes over nothing");
        }

        // The tape's format: FLAC, 16-bit, mono, 96 kHz.
        {
            const auto formatFolder = out.getChildFile("format");
            auto job = makeJob(rig, formatFolder, VariationRenderer::Mode::SeedOnly, 1, 0.5, 9);
            job.rate = 96000.0;
            job.options.bitDepth = 16;
            job.options.type = TapeRecorder::FileType::Flac;
            job.options.mono = true;
            render(rig, job);
            const auto files = filesIn(formatFolder);
            const auto a = files.size() == 1 ? readFile(files[0]) : Audio {};
            check(files.size() == 1 && files[0].getFileName() == "Whoosh_001.flac" && a.rate == 96000.0 && a.bits == 16 && a.samples.getNumChannels() == 1
                      && a.seconds() > 0.25 && a.peak() > 0.03f,
                  "the files follow the format asked for (FLAC, 16-bit, mono, 96 kHz: " + juce::String(a.seconds(), 2) + " s)");
            check(files.size() == 1 && TapeRecorder::readDescription(files[0]).contains("Variation 1 of 1"), "a FLAC file carries its description too");
        }

        // Cancelling part-way: the finished files stay, nothing half-written is left.
        {
            const auto cancelFolder = out.getChildFile("cancel");
            juce::String error;
            check(renderer.start(makeJob(rig, cancelFolder, VariationRenderer::Mode::SmallChanges, 400, 2.0, 5), error), "cancel: a batch of 400 starts");
            juce::String second;
            check(! renderer.start(makeJob(rig, cancelFolder, VariationRenderer::Mode::SmallChanges, 2, 1.0, 5), second) && second.isNotEmpty(),
                  "cancel: a second batch can't be started while one renders");
            const auto began = juce::Time::getMillisecondCounter();
            while (renderer.getProgress().done < 3 && juce::Time::getMillisecondCounter() - began < 120000)
                juce::Thread::sleep(2);
            const auto midway = renderer.getProgress();
            check(midway.state == VariationRenderer::State::Rendering && midway.currentFile.startsWith("Whoosh_") && midway.secondsLeft > 0.0,
                  "cancel: while rendering it says which file it is on and how long is left (" + midway.currentFile + ", "
                      + juce::String(midway.secondsLeft, 1) + " s)");
            renderer.cancel();
            check(renderer.waitUntilDone(60000), "cancel: the batch stops");
            const auto progress = renderer.getProgress();
            const auto files = filesIn(cancelFolder);
            check(progress.state == VariationRenderer::State::Cancelled && progress.done >= 3 && progress.done < 400,
                  "cancel: it stopped part-way (" + juce::String(progress.done) + " of 400)");
            bool whole = files.size() == progress.done && static_cast<int>(renderer.getFiles().size()) == progress.done;
            for (int i = 0; i < files.size(); ++i)
            {
                const auto a = readFile(files[i]);
                whole = whole && files[i].getFileName() == VariationRenderer::fileNameOf("Whoosh", i + 1, 400) + ".wav" && a.seconds() > 1.0
                     && a.samples.getMagnitude(juce::jmax(0, a.samples.getNumSamples() - 48), juce::jmin(48, a.samples.getNumSamples())) < 0.001f;
            }
            check(whole, "cancel: the " + juce::String(files.size()) + " finished files are kept, each whole, and nothing half-written is left");
        }
    }

    // Nothing to play: a message, and no empty files.
    {
        Rig rig(sources, false);
        for (int lane = 0; lane < CargoHold::c_numLanes; ++lane)
            rig.proc.getCargo().setMute(lane, true);
        const auto silentFolder = out.getChildFile("silent");
        const auto progress = render(rig, makeJob(rig, silentFolder, VariationRenderer::Mode::SeedOnly, 5, 0.5, 1));
        check(progress.state == VariationRenderer::State::Failed && progress.done == 0 && progress.error.isNotEmpty() && filesIn(silentFolder).isEmpty(),
              "a rig with nothing to play fails with a message and writes no files (\"" + progress.error + "\")");
    }

    // Speed.
    if (bench)
    {
        Rig rig(sources);
        for (const auto mode : { VariationRenderer::Mode::SeedOnly, VariationRenderer::Mode::RandomiseEverything })
        {
            const auto benchFolder = out.getChildFile("bench");
            benchFolder.deleteRecursively();
            const auto progress = render(rig, makeJob(rig, benchFolder, mode, 100, 3.0, 1));
            std::cout << "         bench, " << VariationRenderer::modeName(mode) << ": 100 x 3 s at 48 kHz took " << juce::String(progress.elapsedSeconds, 1)
                      << " s for " << juce::String(progress.audioSeconds, 0) << " s of sound in " << progress.done << " files ("
                      << juce::String(progress.audioSeconds / juce::jmax(0.001, progress.elapsedSeconds), 1) << "x real time)" << std::endl;
            benchFolder.deleteRecursively();
        }
    }

    // The files join the Takes (the rig collects them on its timer; here it is asked to look now).
    {
        Rig rig(sources);
        const auto takesFolder = out.getChildFile("takes");
        render(rig, makeJob(rig, takesFolder, VariationRenderer::Mode::SeedOnly, 3, 0.5, 2));
        rig.proc.refreshTakes();
        const auto& takes = rig.proc.getTakes();
        check(takes.size() == 3 && takes[0].kind == "Variation" && takes[0].file.getFileName() == "Whoosh_003.wav" && takes[0].seconds > 0.25,
              "the rendered files join the Takes, newest first (" + juce::String(static_cast<int>(takes.size())) + " takes)");
    }

    SwooshbucklerAudioProcessor::useSettings(nullptr);
    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
