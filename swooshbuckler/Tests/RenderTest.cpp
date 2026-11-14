// Render test: Render Variations through the whole plug-in, offline.
//
//   - A batch of five for each What Varies: five files with the right names, lengths within reason, peaks under the
//     Ceiling, a description in each, and no two files alike.
//   - The same batch number gives the same files again, sample for sample; another gives others. The batches
//     rendered lately are remembered, and one of them rendered again from what was remembered is the same batch.
//   - The rig that is rendered from is left exactly as it was.
//   - Cancelling part-way keeps the finished files and leaves nothing half-written.
//   - Numbers carry on after the files a folder already has; the files follow the tape's format.
//   - File names: Counter, Date And Time, Preset And Counter, Batch Name And Counter.
//   - One File With Markers: one file, a marker named after each variation at its start, the gap between them,
//     the same sound as the separate files; Both; AIFF markers and FLAC comments; never written over.
//   - Whole Ride: files of exactly the length asked for that do not start from silence, repeat and differ.
//   - The estimate shown before rendering.
//   - A rig with nothing to play fails with a message instead of writing empty files.
//   - The files join the Takes (the one file as one take).
//   - How fast a batch renders (--bench: 100 whooshes of 3 s).
//
// Usage: SwooshbucklerRender [scratch folder] [--bench]. Nothing is read from or written to the user's settings.

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "VariationRenderer.h"

#include <algorithm>
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
            described = described && text.contains("Variation " + juce::String(i + 1) + " of 5") && text.contains("Batch 4711") && ! text.containsIgnoreCase("seed")
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
            check(job.mode == VariationRenderer::Mode::NewPasses && VariationRenderer::modeName(job.mode) == "My Settings, New Passes",
                  "defaults: My Settings, New Passes");
            check(job.each == VariationRenderer::Each::OneWhoosh && job.names == VariationRenderer::Names::Counter
                      && job.files == VariationRenderer::Files::PerVariation && job.gapSeconds == 0.5 && job.rideSeconds == 10.0,
                  "defaults: One Whoosh, Counter names, One File Per Variation, a gap of 0.5 s, rides of 10 s");
            {
                juce::StringArray words;
                for (int i = 0; i < 3; ++i)
                    words.add(VariationRenderer::modeName(static_cast<VariationRenderer::Mode>(i)));
                for (int i = 0; i < 2; ++i)
                    words.add(VariationRenderer::eachName(static_cast<VariationRenderer::Each>(i)));
                for (int i = 0; i < 4; ++i)
                    words.add(VariationRenderer::namesName(static_cast<VariationRenderer::Names>(i)));
                for (int i = 0; i < 3; ++i)
                    words.add(VariationRenderer::filesName(static_cast<VariationRenderer::Files>(i)));
                const auto all = words.joinIntoString(" / ");
                bool plain = true;
                for (const auto* word : { "seed", "roll", "jitter", "offline", "instance" })
                    plain = plain && ! all.containsIgnoreCase(word);
                check(plain && words.size() == 12, "wording: no choice is called a seed, a roll, jitter, offline or an instance (" + all + ")");
            }
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
        batchTest(rig, out.getChildFile("seed"), VariationRenderer::Mode::NewPasses, 1.0);
        check(rig.values() == before, "the rig that was rendered from is exactly as it was (every parameter and start marker)");
        check(! rig.proc.isPlaying() && ! rig.proc.isRecording(), "the rig was neither started nor recorded");

        // The same batch seed again: the same files. Another seed: other files.
        for (const auto mode : { VariationRenderer::Mode::RandomiseEverything, VariationRenderer::Mode::SmallChanges, VariationRenderer::Mode::NewPasses })
        {
            const auto label = VariationRenderer::modeName(mode);
            const char* folders[] = { "seed", "small", "randomise" };
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
            const auto progress = render(rig, makeJob(rig, seedFolder, VariationRenderer::Mode::NewPasses, 2, 0.5, 9));
            const auto files = filesIn(seedFolder);
            check(progress.done == 2 && files.size() == 7 && files[5].getFileName() == "Whoosh_006.wav" && files[6].getFileName() == "Whoosh_007.wav",
                  "a second batch into the same folder carries on at Whoosh_006 and writes over nothing");
        }

        // The batches rendered lately are remembered, newest first, and one rendered again from what was remembered
        // is the same batch (Repeat Last Batch, Repeat Batch...).
        {
            const auto firstFolder = out.getChildFile("repeat-first"), againFolder = out.getChildFile("repeat-again");
            auto job = makeJob(rig, firstFolder, VariationRenderer::Mode::SmallChanges, 3, 0.7, 31337);
            job.amount = 0.45f;
            render(rig, job);
            render(rig, makeJob(rig, out.getChildFile("repeat-other"), VariationRenderer::Mode::NewPasses, 1, 0.5, 31338));
            const auto recent = renderer.recentBatches();
            check(recent.size() >= 2 && recent[0].number == 31338u && recent[1].number == 31337u && recent[1].name == "Whoosh" && recent[1].count == 3
                      && recent[1].mode == VariationRenderer::Mode::SmallChanges && std::abs(recent[1].amount - 0.45f) < 1.0e-3f
                      && std::abs(recent[1].seconds - 0.7) < 1.0e-3 && static_cast<int>(recent.size()) <= VariationRenderer::c_maxBatches,
                  "batches: the last ones are remembered, newest first, with what they were made of (" + juce::String(static_cast<int>(recent.size())) + " kept)");
            check(recent.size() >= 2 && VariationRenderer::batchTitle(recent[1]).startsWith("Whoosh, ") && VariationRenderer::batchTitle(recent[1]).contains(":"),
                  "batches: each is listed by name, date and time (" + (recent.size() >= 2 ? VariationRenderer::batchTitle(recent[1]) : juce::String()) + ")");
            if (recent.size() >= 2)
            {
                // What the panel does for Repeat Batch: a job for now, with the remembered batch's number and makings.
                auto again = makeJob(rig, againFolder, recent[1].mode, recent[1].count, recent[1].seconds, recent[1].number);
                again.amount = recent[1].amount;
                again.each = recent[1].each;
                render(rig, again);
                const auto a = filesIn(firstFolder), b = filesIn(againFolder);
                double worst = a.size() == 3 && b.size() == 3 ? 0.0 : 1.0;
                for (int i = 0; i < juce::jmin(a.size(), b.size()); ++i)
                    worst = juce::jmax(worst, difference(readFile(a[i]), readFile(b[i])));
                check(worst == 0.0, "batches: a remembered batch rendered again gives the same files, sample for sample");
                const auto after = renderer.recentBatches();
                int times = 0;
                for (const auto& batch : after)
                    times += batch.number == 31337u ? 1 : 0;
                check(! after.empty() && after[0].number == 31337u && times == 1, "batches: a repeated batch moves to the front of the list and is listed once");
            }
            for (std::uint32_t n = 0; n < 8; ++n)
                render(rig, makeJob(rig, out.getChildFile("repeat-other"), VariationRenderer::Mode::NewPasses, 1, 0.3, 500 + n));
            check(static_cast<int>(renderer.recentBatches().size()) == VariationRenderer::c_maxBatches && renderer.recentBatches()[0].number == 507u,
                  "batches: only the last " + juce::String(VariationRenderer::c_maxBatches) + " are kept");
        }

        // File names: the four stamps, and what a batch really writes for each.
        {
            auto job = makeJob(rig, out.getChildFile("names"), VariationRenderer::Mode::NewPasses, 2, 0.3, 77);
            job.when = juce::Time(2026, 10, 13, 14, 32, 5); // 13 November 2026, 14:32
            job.preset = "Jet Passes";
            job.batchName = "Dragon Wings";
            using Names = VariationRenderer::Names;
            const std::pair<Names, const char*> expected[] = { { Names::Counter, "Whoosh_001" }, { Names::DateAndTime, "Whoosh_2026-11-13_1432_001" },
                                                               { Names::PresetAndCounter, "Jet Passes_001" }, { Names::BatchNameAndCounter, "Dragon Wings_001" } };
            for (const auto& [names, first] : expected)
            {
                job.names = names;
                const auto planned = VariationRenderer::planOf(job).firstName;
                render(rig, job);
                const auto second = juce::String(first).dropLastCharacters(1) + "2";
                check(planned == first && job.folder.getChildFile(juce::String(first) + ".wav").existsAsFile() && job.folder.getChildFile(second + ".wav").existsAsFile(),
                      "names: " + VariationRenderer::namesName(names) + " gives " + first + " (planned " + planned + ")");
            }
            check(filesIn(job.folder).size() == 8, "names: the four batches wrote eight files side by side");
            job.names = Names::DateAndTime;
            check(VariationRenderer::planOf(job).firstName == "Whoosh_2026-11-13_1432_003", "names: a second batch in the same minute carries on at 003");
            job.names = Names::BatchNameAndCounter;
            job.batchName = "  ";
            check(VariationRenderer::planOf(job).firstName == "Batch_001", "names: with no batch name typed the files are called Batch");
            job.batchName = "A/B: Take?";
            check(VariationRenderer::planOf(job).firstName == "AB Take_001", "names: a typed name keeps only what a file name may have");
        }

        // One File With Markers: every variation in one file, a marker with its name at the start of each, a gap between.
        {
            using Files = VariationRenderer::Files;
            const auto separateFolder = out.getChildFile("one-separate"), oneFolder = out.getChildFile("one");
            render(rig, makeJob(rig, separateFolder, VariationRenderer::Mode::NewPasses, 4, 0.5, 2024));
            const auto separate = filesIn(separateFolder);
            auto job = makeJob(rig, oneFolder, VariationRenderer::Mode::NewPasses, 4, 0.5, 2024);
            job.files = Files::OneWithMarkers;
            job.gapSeconds = 0.5;
            const auto progress = render(rig, job);
            const auto files = filesIn(oneFolder);
            check(progress.state == VariationRenderer::State::Finished && progress.done == 4 && progress.files == 1 && files.size() == 1
                      && files[0].getFileName() == "Whoosh_All.wav" && progress.combinedFile == "Whoosh_All.wav",
                  "one file: four variations make one file, Whoosh_All.wav (" + juce::String(files.size()) + " files)");
            const auto all = files.size() == 1 ? readFile(files[0]) : Audio {};
            const auto cues = files.size() == 1 ? TapeRecorder::readCues(files[0]) : std::vector<TapeRecorder::Cue> {};
            const int gap = static_cast<int>(0.5 * c_fs);
            bool names = cues.size() == 4, places = cues.size() == 4 && separate.size() == 4, same = places, quiet = places;
            std::int64_t at = 0;
            juce::String where;
            for (size_t i = 0; i < cues.size() && i < static_cast<size_t>(separate.size()); ++i)
            {
                const auto one = readFile(separate[static_cast<int>(i)]);
                const int n = one.samples.getNumSamples();
                names = names && cues[i].label == "Whoosh_00" + juce::String(static_cast<int>(i) + 1);
                places = places && cues[i].frame == at;
                where << juce::String(static_cast<double>(cues[i].frame) / c_fs, 3) << " s " << cues[i].label << (i + 1 < cues.size() ? ", " : "");
                if (at + n <= all.samples.getNumSamples())
                {
                    // The same samples as the variation's own file, then nothing at all until the next marker.
                    for (int ch = 0; ch < 2 && same; ++ch)
                        for (int k = 0; k < n && same; ++k)
                            same = all.samples.getSample(ch, static_cast<int>(at) + k) == one.samples.getSample(ch, k);
                    if (i + 1 < cues.size())
                        quiet = quiet && at + n + gap <= all.samples.getNumSamples() && all.samples.getMagnitude(static_cast<int>(at) + n, gap) == 0.0f;
                }
                else
                    same = false;
                at += n + gap;
            }
            check(cues.size() == 4, "one file: it has four markers (" + juce::String(static_cast<int>(cues.size())) + ")");
            check(names, "one file: each marker has its variation's name, Whoosh_001 to Whoosh_004 (" + where + ")");
            check(places, "one file: each marker is at the first sample of its variation, half a second of gap after the one before");
            check(same, "one file: between the markers is the same sound as the separate files, sample for sample");
            check(quiet, "one file: the gaps are silence");
            check(places && all.samples.getNumSamples() == static_cast<int>(at) - gap, "one file: it ends with the last variation, with no gap after it");
            check(files.size() == 1 && TapeRecorder::readDescription(files[0]).contains("4 variations") && TapeRecorder::readDescription(files[0]).contains("Batch 2024"),
                  "one file: its description says what it holds and which batch it is");
            rig.proc.refreshTakes();
            const auto& takes = rig.proc.getTakes();
            check(! takes.empty() && takes[0].kind == "Variations" && takes[0].markers == 4 && takes[0].file.getFileName() == "Whoosh_All.wav"
                      && std::abs(takes[0].seconds - all.seconds()) < 0.01,
                  "one file: the Takes list shows it as one take");

            // Another gap; and Both: the separate files and the one file, twice into the same folder with nothing written over.
            const auto bothFolder = out.getChildFile("both");
            job.folder = bothFolder;
            job.files = Files::Both;
            job.gapSeconds = 1.25;
            const auto first = render(rig, job);
            const auto firstAll = bothFolder.getChildFile("Whoosh_All.wav");
            const auto before = firstAll.loadFileAsString().hashCode64();
            const auto bothCues = TapeRecorder::readCues(firstAll);
            const auto firstOne = readFile(bothFolder.getChildFile("Whoosh_001.wav"));
            check(first.files == 5 && filesIn(bothFolder).size() == 5 && bothCues.size() == 4
                      && bothCues[1].frame == firstOne.samples.getNumSamples() + static_cast<int>(1.25 * c_fs),
                  "both: four files and the one file with them all, its gap the 1.25 s asked for");
            const auto second = render(rig, job);
            const auto bothFiles = filesIn(bothFolder);
            const auto secondCues = TapeRecorder::readCues(bothFolder.getChildFile("Whoosh_All_2.wav"));
            check(second.files == 5 && bothFiles.size() == 10 && bothFolder.getChildFile("Whoosh_008.wav").existsAsFile()
                      && bothFolder.getChildFile("Whoosh_All_2.wav").existsAsFile() && firstAll.loadFileAsString().hashCode64() == before
                      && secondCues.size() == 4 && secondCues[0].label == "Whoosh_005",
                  "both: a second batch carries on at Whoosh_005 and Whoosh_All_2, and writes over nothing (" + juce::String(bothFiles.size()) + " files)");

            // AIFF has markers of its own; FLAC has none, so there the names and times go into the comments.
            const auto aiffFolder = out.getChildFile("one-aiff"), flacFolder = out.getChildFile("one-flac");
            job.files = Files::OneWithMarkers;
            job.gapSeconds = 0.5;
            job.folder = aiffFolder;
            job.options.type = TapeRecorder::FileType::Aiff;
            render(rig, job);
            const auto aiffCues = TapeRecorder::readCues(aiffFolder.getChildFile("Whoosh_All.aif"));
            const auto aiff = readFile(aiffFolder.getChildFile("Whoosh_All.aif"));
            bool aiffOk = aiffCues.size() == 4 && cues.size() == 4 && aiff.samples.getNumSamples() == all.samples.getNumSamples();
            for (size_t i = 0; i < aiffCues.size() && aiffOk; ++i)
                aiffOk = aiffCues[i].frame == cues[i].frame && aiffCues[i].label == cues[i].label;
            check(aiffOk, "one file: an AIFF file has the same four named markers in the same places, and the same length");
            check(TapeRecorder::readDescription(aiffFolder.getChildFile("Whoosh_All.aif")).contains("4 variations"), "one file: an AIFF file keeps its description");
            job.folder = flacFolder;
            job.options.type = TapeRecorder::FileType::Flac;
            render(rig, job);
            const auto flacFile = flacFolder.getChildFile("Whoosh_All.flac");
            juce::MemoryBlock raw;
            flacFile.loadFileAsData(raw);
            // The comments sit among binary bytes (zeros included), so they are looked for as bytes.
            const std::string text(static_cast<const char*>(raw.getData()), std::min(raw.getSize(), static_cast<size_t>(16384)));
            auto has = [&text](const char* what) { return text.find(what) != std::string::npos; };
            const auto flac = readFile(flacFile);
            check(flac.samples.getNumSamples() == all.samples.getNumSamples() && TapeRecorder::readDescription(flacFile).contains("4 variations")
                      && has("MARKER=0:00.000 Whoosh_001") && has(" Whoosh_004"),
                  "one file: a FLAC file lists the names and times in its comments");
        }

        // Whole Ride: each file is the rig playing for the length asked for, not a throw from silence.
        {
            using Each = VariationRenderer::Each;
            const auto rideFolder = out.getChildFile("ride");
            auto job = makeJob(rig, rideFolder, VariationRenderer::Mode::NewPasses, 3, 1.0, 808);
            job.each = Each::WholeRide;
            job.rideSeconds = 2.5;
            const auto progress = render(rig, job);
            const auto files = filesIn(rideFolder);
            const float ceiling = juce::Decibels::decibelsToGain(rig.proc.getState().getRawParameterValue(ParamIDs::ceiling)->load());
            std::vector<Audio> audio;
            bool lengths = files.size() == 3, starts = lengths, edges = lengths, peaks = lengths, described = lengths;
            juce::String report;
            for (int i = 0; i < files.size(); ++i)
            {
                audio.push_back(readFile(files[i]));
                const auto& a = audio.back();
                const int n = a.samples.getNumSamples();
                lengths = lengths && n == static_cast<int>(2.5 * c_fs);
                if (n < 9600)
                    continue;
                // Sound from the start: the first fifth of a second is nowhere near silence, though the very first
                // sample is (a 10 ms fade in, so the cut does not click), and the end is faded too.
                const float opening = juce::jmax(a.samples.getRMSLevel(0, 0, 9600), a.samples.getRMSLevel(1, 0, 9600));
                starts = starts && opening > juce::Decibels::decibelsToGain(-50.0f);
                edges = edges && a.samples.getMagnitude(0, 4) < 0.01f && a.samples.getMagnitude(n - 4, 4) < 0.01f;
                peaks = peaks && a.peak() <= ceiling * 1.001f && a.peak() > juce::Decibels::decibelsToGain(-30.0f);
                const auto text = TapeRecorder::readDescription(files[i]);
                described = described && text.contains("Whole Ride, Length 2.5 s") && text.contains("Variation " + juce::String(i + 1) + " of 3");
                report << juce::String(a.seconds(), 2) << " s, first 0.2 s " << juce::String(db(opening), 1) << " dB RMS, peak " << juce::String(db(a.peak()), 1)
                       << " dB" << (i + 1 < files.size() ? "; " : "");
            }
            std::cout << "         Whole Ride: " << report << "  (" << juce::String(progress.audioSeconds / juce::jmax(0.001, progress.elapsedSeconds), 1)
                      << "x real time)" << std::endl;
            check(progress.state == VariationRenderer::State::Finished && progress.done == 3 && files.size() == 3 && files[0].getFileName() == "Whoosh_001.wav",
                  "whole ride: three rides make three files");
            check(lengths, "whole ride: every file is exactly the 2.5 s asked for");
            check(starts, "whole ride: no file starts from silence (the first 0.2 s is over -50 dB RMS)");
            check(edges, "whole ride: both ends are faded, so neither clicks");
            check(peaks, "whole ride: every file has sound in it, under the Ceiling");
            check(described, "whole ride: every file says it is a ride, how long, and which variation");
            double least = 10.0;
            for (size_t i = 0; i < audio.size(); ++i)
                for (size_t k = i + 1; k < audio.size(); ++k)
                    least = juce::jmin(least, difference(audio[i], audio[k]));
            check(audio.size() == 3 && least > 0.1, "whole ride: no two rides are alike (the closest pair differ by " + juce::String(juce::roundToInt(100.0 * least)) + "% RMS)");

            const auto againFolder = out.getChildFile("ride-again");
            job.folder = againFolder;
            render(rig, job);
            const auto again = filesIn(againFolder);
            double worst = again.size() == 3 && files.size() == 3 ? 0.0 : 1.0;
            for (int i = 0; i < juce::jmin(files.size(), again.size()); ++i)
                worst = juce::jmax(worst, difference(readFile(files[i]), readFile(again[i])));
            check(worst == 0.0, "whole ride: the same batch again gives the same rides, sample for sample");
            check(! rig.proc.isPlaying(), "whole ride: the rig you hear was not started");

            // A whoosh, by contrast, opens at its first sound: far quieter over the same stretch.
            const auto whoosh = readFile(out.getChildFile("seed").getChildFile("Whoosh_001.wav"));
            if (whoosh.samples.getNumSamples() > 480 && ! audio.empty())
                std::cout << "         (a whoosh's first 10 ms: " << juce::String(db(whoosh.samples.getRMSLevel(0, 0, 480)), 1) << " dB RMS; a ride's: "
                          << juce::String(db(audio[0].samples.getRMSLevel(0, 480, 480)), 1) << " dB RMS)" << std::endl;
        }

        // The estimate shown before rendering.
        {
            auto job = makeJob(rig, out.getChildFile("estimate"), VariationRenderer::Mode::NewPasses, 100, 3.0, 1);
            using Files = VariationRenderer::Files;
            VariationRenderer::Estimate e { 100, 300.0, 20.0 };
            check(VariationRenderer::estimateText(job, e) == "100 files, one whoosh each: about 5 minutes of sound, ready in about 20 seconds.",
                  "estimate: " + VariationRenderer::estimateText(job, e));
            const auto real = renderer.estimate(job);
            check(real.files == 100 && real.soundSeconds >= 300.0 && real.soundSeconds <= 900.0 && real.renderSeconds > 0.5 && real.renderSeconds < 600.0,
                  "estimate: 100 whooshes of 3 s come to " + juce::String(real.soundSeconds, 0) + " s of sound, ready in about " + juce::String(real.renderSeconds, 0) + " s");
            job.files = Files::OneWithMarkers;
            const auto one = renderer.estimate(job);
            check(one.files == 1 && std::abs(one.soundSeconds - (real.soundSeconds + 99 * 0.5)) < 1.0e-6
                      && VariationRenderer::estimateText(job, one).startsWith("1 file with 100 whooshes in it: "),
                  "estimate: one file with markers is 1 file, and the gaps count (" + VariationRenderer::estimateText(job, one) + ")");
            job.files = Files::Both;
            check(renderer.estimate(job).files == 101 && VariationRenderer::estimateText(job, renderer.estimate(job)).startsWith("101 files (100 whooshes, and one file with all of them): "),
                  "estimate: both is 101 files");
            job.files = Files::PerVariation;
            job.each = VariationRenderer::Each::WholeRide;
            job.rideSeconds = 30.0;
            job.count = 20;
            const auto rides = renderer.estimate(job);
            check(rides.files == 20 && std::abs(rides.soundSeconds - 600.0) < 1.0e-6
                      && VariationRenderer::estimateText(job, rides).startsWith("20 files, one ride each: about 10 minutes of sound, ready in "),
                  "estimate: " + VariationRenderer::estimateText(job, rides));
            check(VariationRenderer::roughly(0.4) == "about a second" && VariationRenderer::roughly(89.0) == "about 89 seconds"
                      && VariationRenderer::roughly(150.0) == "about 3 minutes" && VariationRenderer::roughly(3.0 * 3600.0) == "about 3 hours",
                  "estimate: times are said roughly, in seconds, minutes or hours");
        }

        // The tape's format: FLAC, 16-bit, mono, 96 kHz.
        {
            const auto formatFolder = out.getChildFile("format");
            auto job = makeJob(rig, formatFolder, VariationRenderer::Mode::NewPasses, 1, 0.5, 9);
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

    // Cancelling a batch that goes into one file: the file is closed whole, with a marker for each variation it has.
    {
        Rig rig(sources);
        auto& renderer = rig.proc.getVariations();
        const auto cancelFolder = out.getChildFile("cancel-one");
        auto job = makeJob(rig, cancelFolder, VariationRenderer::Mode::NewPasses, 400, 1.0, 6);
        job.files = VariationRenderer::Files::OneWithMarkers;
        juce::String error;
        check(renderer.start(job, error), "cancel, one file: a batch of 400 starts");
        const auto began = juce::Time::getMillisecondCounter();
        while (renderer.getProgress().done < 3 && juce::Time::getMillisecondCounter() - began < 120000)
            juce::Thread::sleep(2);
        renderer.cancel();
        renderer.waitUntilDone(60000);
        const auto progress = renderer.getProgress();
        const auto files = filesIn(cancelFolder);
        const auto cues = files.size() == 1 ? TapeRecorder::readCues(files[0]) : std::vector<TapeRecorder::Cue> {};
        const auto a = files.size() == 1 ? readFile(files[0]) : Audio {};
        check(progress.state == VariationRenderer::State::Cancelled && progress.done >= 3 && files.size() == 1 && static_cast<int>(cues.size()) == progress.done
                  && a.seconds() > 3.0 && cues.back().frame < a.samples.getNumSamples() && std::abs(a.seconds() - (progress.audioSeconds + 0.5 * (progress.done - 1))) < 0.01,
              "cancel, one file: the file is kept whole, with " + juce::String(static_cast<int>(cues.size())) + " markers for the " + juce::String(progress.done)
                  + " variations it has");
    }

    // Nothing to play: a message, and no empty files.
    {
        Rig rig(sources, false);
        for (int lane = 0; lane < CargoHold::c_numLanes; ++lane)
            rig.proc.getCargo().setMute(lane, true);
        const auto silentFolder = out.getChildFile("silent");
        const auto progress = render(rig, makeJob(rig, silentFolder, VariationRenderer::Mode::NewPasses, 5, 0.5, 1));
        check(progress.state == VariationRenderer::State::Failed && progress.done == 0 && progress.error.isNotEmpty() && filesIn(silentFolder).isEmpty(),
              "a rig with nothing to play fails with a message and writes no files (\"" + progress.error + "\")");
    }

    // Speed.
    if (bench)
    {
        Rig rig(sources);
        for (const auto mode : { VariationRenderer::Mode::NewPasses, VariationRenderer::Mode::RandomiseEverything })
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
        render(rig, makeJob(rig, takesFolder, VariationRenderer::Mode::NewPasses, 3, 0.5, 2));
        rig.proc.refreshTakes();
        const auto& takes = rig.proc.getTakes();
        check(takes.size() == 3 && takes[0].kind == "Variation" && takes[0].file.getFileName() == "Whoosh_003.wav" && takes[0].seconds > 0.25,
              "the rendered files join the Takes, newest first (" + juce::String(static_cast<int>(takes.size())) + " takes)");
    }

    SwooshbucklerAudioProcessor::useSettings(nullptr);
    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
