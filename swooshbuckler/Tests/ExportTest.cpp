// Export test: files at every room width, through the real renderer and the real tape.
//
//   - Render Variations at every width (Mono to 7.1.4), from a Stereo rig: the same batch rendered three times,
//     Like Pro Tools, in the WAV Standard order and as Multi-Mono sets. Each file has the width's channel count;
//     each interleaved channel holds, sample for sample, the Multi-Mono file of the speaker the order (typed out
//     here) puts there, so the orders, the suffixes and the repeatability of a batch are checked together; the WAV
//     header is the typed one (the mask, or at 7.1.4 Like Pro Tools a plain header); every whoosh has a "Closest"
//     marker at its loudest 50 ms (found here on its own), and the one file with markers has a named marker at the
//     start of each variation and at its closest point, in every file of a set; the description puts the
//     variation, batch, width and room first, inside the BWF description's 256 bytes. Mono is the Stereo render's
//     fold, and every width is the same batch (its loudest moment in the same place).
//   - A 7.1.4 render's sound is really in the room: rears, sides and tops all carry some of it with the passes
//     going Around and Overhead, and the default Across pass is not in L and R alone.
//   - The Width choice: the rig's own width unless another is chosen; the estimate counts every channel (and a
//     Multi-Mono set's one file per channel against the 4 GB limit); FLAC can't hold 7.1.4 in one file and the
//     batch is refused with a plain message before anything is rendered, while 7.1.4 FLAC Multi-Mono and 7.1 FLAC
//     interleaved are written; an AIFF file with markers over 2 GB is refused.
//   - The files join the Takes at their width; a Multi-Mono set is one take.
//   - The tape in a 7.1.4 room: the Stereo fold is the room's fold, lined up, held under the Ceiling, and labelled
//     (the file's description and the Takes list); as played it is 7.1.4; Multi-Mono is one take; FLAC is refused
//     before anything is recorded; the saved last minutes' length is the real one under the 512 MB cap.
//   - The line under Channel Order and File Layout, typed out.
//
// Usage: SwooshbucklerExport [scratch folder]. Run it from a scratch folder (it writes into
// ./SwooshbucklerExportTest by default); nothing is read from or written to the user's settings.

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Parameters.h"
#include "Room.h"
#include "TakeFile.h"
#include "TapeRecorder.h"
#include "VariationRenderer.h"

#include <cmath>
#include <iostream>
#include <map>

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

    juce::String num(double v, int places = 2) { return juce::String(v, places); }

    using W = room::Width;
    constexpr W c_widths[] = { W::Mono, W::Stereo, W::Quad, W::Surround51, W::Surround71, W::Surround712, W::Surround714 };
    juce::String nameOf(W w) { return room::layout(w).name; }

    // The orders, typed out (file channel by file channel), not taken from Room.
    // Like Pro Tools is what Pro Tools 2026 bounces: the WAV order up to 7.1.2, the film order at 7.1.4.
    juce::StringArray typedOrder(W w, room::FileOrder order)
    {
        const bool film = order == room::FileOrder::ProTools;
        const char* text = "";
        switch (w)
        {
            case W::Mono:        text = "C"; break;
            case W::Stereo:      text = "L R"; break;
            case W::Quad:        text = "L R Ls Rs"; break;
            case W::Surround51:  text = "L R C LFE Ls Rs"; break;
            case W::Surround71:  text = "L R C LFE Lsr Rsr Lss Rss"; break;
            case W::Surround712: text = "L R C LFE Lsr Rsr Lss Rss Lts Rts"; break;
            case W::Surround714: text = film ? "L C R Lss Rss Lsr Rsr LFE Ltf Rtf Ltr Rtr" : "L R C LFE Lsr Rsr Lss Rss Ltf Rtf Ltr Rtr"; break;
        }
        return juce::StringArray::fromTokens(text, " ", "");
    }

    std::uint32_t typedMask(W w)
    {
        switch (w)
        {
            case W::Quad:        return 0x33;
            case W::Surround51:  return 0x3F;
            case W::Surround71:  return 0x63F;
            case W::Surround712: return 0x563F;
            case W::Surround714: return 0x2D63F;
            case W::Mono:
            case W::Stereo:      return 0;
        }
        return 0;
    }

    void writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio, double rate)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        auto* stream = new juce::FileOutputStream(file);
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream, rate, static_cast<unsigned int>(audio.getNumChannels()), 24, {}, 0));
        writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    }

    // Three sources: a growl with gaps, a steady wind, and a row of hits (as the Render test has them).
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

        explicit Rig(const juce::File& sources)
        {
            proc.setPlayConfigDetails(0, 2, c_fs, 512);
            proc.overrideRecordRate(0);
            proc.prepareToPlay(c_fs, 512);
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

        /** The rig's own output made `width` wide (a host's track), and prepared again. */
        void widen(W width, double rate = c_fs)
        {
            auto layout = proc.getBusesLayout();
            layout.outputBuses.getReference(0) = room::channelSet(width);
            const bool ok = proc.setBusesLayout(layout);
            check(ok, "the rig takes a " + nameOf(width) + " track");
            proc.prepareToPlay(rate, 512);
        }

        void run(double seconds)
        {
            juce::AudioBuffer<float> block(proc.getTotalNumOutputChannels(), 512);
            juce::MidiBuffer midi;
            for (int i = 0; i < static_cast<int>(seconds * proc.getDeviceRate() / 512.0); ++i)
            {
                block.clear();
                proc.processBlock(block, midi);
            }
        }

        void set(const char* id, float plain)
        {
            if (auto* p = proc.getState().getParameter(id))
                p->setValueNotifyingHost(p->convertTo0to1(plain));
        }
    };

    struct Audio
    {
        juce::AudioBuffer<float> samples;
        double rate = 0.0;
        int channels() const { return samples.getNumChannels(); }
        int length() const { return samples.getNumSamples(); }
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
        return a;
    }

    bool sameChannel(const Audio& a, int ca, const Audio& b, int cb)
    {
        if (a.length() != b.length() || a.length() == 0 || ca >= a.channels() || cb >= b.channels())
            return false;
        for (int i = 0; i < a.length(); ++i)
            if (! juce::exactlyEqual(a.samples.getSample(ca, i), b.samples.getSample(cb, i)))
                return false;
        return true;
    }

    double energy(const Audio& a, int ch)
    {
        double e = 0.0;
        for (int i = 0; i < a.length(); ++i)
            e += static_cast<double>(a.samples.getSample(ch, i)) * a.samples.getSample(ch, i);
        return e;
    }

    /** Where a file is loudest (all its channels' power together, over 20 ms), in seconds. */
    double loudestAt(const Audio& a)
    {
        const int win = static_cast<int>(0.02 * c_fs);
        double best = -1.0;
        int at = 0;
        for (int i = 0; i + win <= a.length(); i += win / 2)
        {
            double e = 0.0;
            for (int ch = 0; ch < a.channels(); ++ch)
                for (int k = i; k < i + win; ++k)
                    e += static_cast<double>(a.samples.getSample(ch, k)) * a.samples.getSample(ch, k);
            if (e > best)
            {
                best = e;
                at = i;
            }
        }
        return (at + win / 2) / c_fs;
    }

    juce::Array<juce::File> filesIn(const juce::File& folder)
    {
        auto files = folder.findChildFiles(juce::File::findFiles, false);
        files.sort();
        return files;
    }

    VariationRenderer::Job makeJob(Rig& rig, const juce::File& folder, W width, int count, std::uint32_t seed)
    {
        auto job = rig.proc.getVariations().defaultJob();
        job.folder = folder;
        job.mode = VariationRenderer::Mode::NewPasses;
        job.each = VariationRenderer::Each::OneWhoosh;
        job.count = count;
        job.seconds = 0.8;
        job.batchSeed = seed;
        job.name = "Whoosh";
        job.names = VariationRenderer::Names::Counter;
        job.rate = c_fs;
        job.width = width;
        job.options.bitDepth = 24;
        job.options.type = TapeRecorder::FileType::Wav;
        job.options.order = room::FileOrder::ProTools;
        job.options.layout = takefile::Layout::Interleaved;
        job.files = VariationRenderer::Files::PerVariation;
        job.gapSeconds = 0.25;
        return job;
    }

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

    /** The speaker share of a Multi-Mono render: each speaker's energy over the whole room's (the LFE left out). */
    std::map<juce::String, double> shares(const juce::File& folder, W width, const juce::String& stem)
    {
        std::map<juce::String, double> e;
        double total = 0.0;
        const auto& layout = room::layout(width);
        for (int ch = 0; ch < layout.numChannels; ++ch)
        {
            const auto speaker = layout.speakers[static_cast<size_t>(ch)];
            if (speaker == room::Speaker::Lfe)
                continue;
            const juce::String name(room::info(speaker).name);
            const auto a = readFile(folder.getChildFile(stem + "." + name + ".wav"));
            e[name] = a.channels() == 1 ? energy(a, 0) : 0.0;
            total += e[name];
        }
        for (auto& [name, value] : e)
            value = total > 0.0 ? value / total : 0.0;
        return e;
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::File folder = juce::File::getCurrentWorkingDirectory().getChildFile("SwooshbucklerExportTest");
    if (argc > 1)
        folder = juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]);
    folder.deleteRecursively();
    folder.createDirectory();
    const auto sources = folder.getChildFile("sources");
    sources.createDirectory();
    makeSources(sources);
    const auto out = folder.getChildFile("out");
    out.createDirectory();

    juce::PropertiesFile::Options options;
    options.applicationName = "SwooshbucklerExportTest";
    options.filenameSuffix = ".settings";
    const auto settingsFile = folder.getChildFile("test.settings");
    settingsFile.deleteFile();
    juce::PropertiesFile settings(settingsFile, options);
    settings.setValue("recordFolder", out.getChildFile("tape").getFullPathName());
    SwooshbucklerAudioProcessor::useSettings(&settings);

    // The line under Channel Order and File Layout, typed out.
    std::cout << "The order line" << std::endl;
    {
        using L = takefile::Layout;
        using O = room::FileOrder;
        using C = takefile::Container;
        const std::pair<juce::String, juce::String> lines[] = {
            { takefile::orderLine(W::Surround51, O::ProTools, L::Interleaved), "5.1 in one file: L R C LFE Ls Rs. Channel Order is for 7.1.4." },
            { takefile::orderLine(W::Surround51, O::WavStandard, L::Interleaved), "5.1 in one file: L R C LFE Ls Rs. Channel Order is for 7.1.4." },
            { takefile::orderLine(W::Surround71, O::ProTools, L::Interleaved), "7.1 in one file: L R C LFE Lsr Rsr Lss Rss. Channel Order is for 7.1.4." },
            { takefile::orderLine(W::Surround51, O::WavStandard, L::MultiMono), "5.1 as 6 files ending .L .C .R .Ls .Rs .LFE." },
            { takefile::orderLine(W::Surround714, O::ProTools, L::Interleaved), "7.1.4 in one file: L C R Lss Rss Lsr Rsr LFE Ltf Rtf Ltr Rtr, as Pro Tools writes it." },
            { takefile::orderLine(W::Surround714, O::ProTools, L::Interleaved, C::Aiff), "7.1.4 in one file: L C R Lss Rss Lsr Rsr LFE Ltf Rtf Ltr Rtr." },
            { takefile::orderLine(W::Surround714, O::WavStandard, L::Interleaved), "7.1.4 in one file: L R C LFE Lsr Rsr Lss Rss Ltf Rtf Ltr Rtr." },
            { takefile::orderLine(W::Surround71, O::ProTools, L::Interleaved, C::Flac), "7.1 in one file: L R C LFE Lsr Rsr Lss Rss, FLAC's own order." },
            { takefile::orderLine(W::Surround714, O::ProTools, L::Interleaved, C::Flac), "7.1.4 is more channels than one FLAC file holds." },
            { takefile::orderLine(W::Surround714, O::ProTools, L::MultiMono, C::Flac), "7.1.4 as 12 files ending .L .C .R .Lss .Rss .Lsr .Rsr .LFE .Ltf .Rtf .Ltr .Rtr." },
            { takefile::orderLine(W::Surround714, O::ProTools, L::MultiMono), "7.1.4 as 12 files ending .L .C .R .Lss .Rss .Lsr .Rsr .LFE .Ltf .Rtf .Ltr .Rtr." },
            { takefile::orderLine(W::Surround712, O::ProTools, L::MultiMono), "7.1.2 as 10 files ending .L .C .R .Lss .Rss .Lsr .Rsr .LFE .Lts .Rts." },
            { takefile::orderLine(W::Stereo, O::ProTools, L::Interleaved), "Stereo in one file: L R." },
            { takefile::orderLine(W::Stereo, O::ProTools, L::MultiMono), "Stereo as 2 files ending .L .R." },
            { takefile::orderLine(W::Mono, O::ProTools, L::MultiMono), "Mono: one channel in one file." },
        };
        for (const auto& [got, want] : lines)
            check(got == want, "\"" + got + "\"");
        check(! takefile::orderMatters(W::Surround712) && takefile::orderMatters(W::Surround714) && ! takefile::layoutMatters(W::Mono) && takefile::layoutMatters(W::Stereo)
                  && takefile::orderMatters(W::Surround714, L::Interleaved, C::Aiff) && ! takefile::orderMatters(W::Surround714, L::Interleaved, C::Flac)
                  && ! takefile::orderMatters(W::Surround714, L::MultiMono, C::Wav),
              "Channel Order is lit for one 7.1.4 WAV or AIFF file only (not FLAC, not Multi-Mono), File Layout from Stereo up");
    }

    // Render Variations at every width, from a Stereo rig: Like Pro Tools with the one file too, WAV Standard, and
    // Multi-Mono with the one file too. The same batch every time.
    std::cout << "Render Variations at every width" << std::endl;
    std::map<W, Audio> firstOf; // each width's first variation, in the Pro Tools order
    {
        Rig rig(sources);
        check(rig.proc.getRoomWidth() == W::Stereo, "the rig plays in Stereo");
        for (const auto width : c_widths)
        {
            const int n = room::layout(width).numChannels;
            const auto name = nameOf(width);
            const auto base = out.getChildFile("w" + name.replace(".", ""));
            auto job = makeJob(rig, base.getChildFile("pt"), width, 2, 4711);
            job.files = VariationRenderer::Files::Both;
            const auto ptProgress = render(rig, job);
            job.folder = base.getChildFile("smpte");
            job.files = VariationRenderer::Files::PerVariation;
            job.options.order = room::FileOrder::WavStandard;
            render(rig, job);
            job.folder = base.getChildFile("mm");
            job.files = VariationRenderer::Files::Both;
            job.options.layout = takefile::Layout::MultiMono;
            const auto mmProgress = render(rig, job);

            const auto pt = readFile(base.getChildFile("pt").getChildFile("Whoosh_001.wav"));
            const auto pt2 = readFile(base.getChildFile("pt").getChildFile("Whoosh_002.wav"));
            const auto smpte = readFile(base.getChildFile("smpte").getChildFile("Whoosh_001.wav"));
            firstOf[width] = pt;
            check(ptProgress.done == 2 && pt.channels() == n && smpte.channels() == n && pt.rate == c_fs && pt.length() > static_cast<int>(0.5 * c_fs),
                  name + ": the files have " + juce::String(n) + (n == 1 ? " channel" : " channels") + " (" + num(pt.length() / c_fs) + " s)");
            check(pt2.channels() == n && pt2.length() > 0 && ! sameChannel(pt, 0, pt2, 0), name + ": the two variations differ");

            // The Multi-Mono set: one mono file per speaker, named for its speaker, nothing else beside it.
            const auto ptNames = typedOrder(width, room::FileOrder::ProTools);
            juce::StringArray expected;
            for (const auto& s : ptNames)
                for (const auto* stem : { "Whoosh_001", "Whoosh_002", "Whoosh_All" })
                    expected.add(width == W::Mono ? juce::String(stem) + ".wav" : juce::String(stem) + "." + s + ".wav");
            expected.sort(false);
            juce::StringArray found;
            for (const auto& f : filesIn(base.getChildFile("mm")))
                found.add(f.getFileName());
            found.sort(false);
            check(found == expected, name + ": Multi-Mono is " + juce::String(width == W::Mono ? 3 : 3 * n) + " files with the Pro Tools suffixes ("
                                         + found.joinIntoString(" ").substring(0, 120) + ")");

            // Every channel of both orders holds that speaker's Multi-Mono file, sample for sample: the order, the
            // suffixes and the batch's repeatability in one.
            bool ordered = true;
            for (const auto order : { room::FileOrder::ProTools, room::FileOrder::WavStandard })
            {
                const auto& file = order == room::FileOrder::ProTools ? pt : smpte;
                const auto names = typedOrder(width, order);
                for (int i = 0; i < names.size(); ++i)
                {
                    const auto mono = readFile(base.getChildFile("mm").getChildFile(width == W::Mono ? juce::String("Whoosh_001.wav")
                                                                                                    : "Whoosh_001." + names[i] + ".wav"));
                    ordered = ordered && mono.channels() == 1 && sameChannel(file, i, mono, 0);
                }
            }
            check(ordered, name + ": each channel is where the order puts it (" + typedOrder(width, room::FileOrder::ProTools).joinIntoString(" ")
                               + " / " + typedOrder(width, room::FileOrder::WavStandard).joinIntoString(" ") + "), and the same batch gives the same samples");

            // The headers: the typed mask in the WAV Standard order, and Like Pro Tools too below 7.1.4; at 7.1.4 Like
            // Pro Tools a plain header with no mask (its channels named in iXML), as Pro Tools writes it. Mono and
            // Stereo plain.
            const auto ptHeader = takefile::readWavFormat(base.getChildFile("pt").getChildFile("Whoosh_001.wav"));
            const auto smpteHeader = takefile::readWavFormat(base.getChildFile("smpte").getChildFile("Whoosh_001.wav"));
            const bool wide = n > 2;
            const bool masks = ! wide ? ! ptHeader.extensible && ! smpteHeader.extensible
                                      : smpteHeader.extensible && smpteHeader.mask == typedMask(width)
                                            && (width == W::Surround714 ? ! ptHeader.extensible && ptHeader.formatTag == 1 && ptHeader.mask == 0u
                                                                        : ptHeader.extensible && ptHeader.mask == typedMask(width));
            check(masks, name + ": the header is right (Like Pro Tools 0x" + juce::String::toHexString(static_cast<int>(ptHeader.mask)) + ", WAV Standard 0x"
                             + juce::String::toHexString(static_cast<int>(smpteHeader.mask)) + ")");

            // Every whoosh has a Closest marker at its loudest moment: the middle of its loudest 50 ms, found here
            // on its own (the power of every channel), in its file and in each file of its Multi-Mono set.
            auto loudest = [](const Audio& a)
            {
                const int window = juce::roundToInt(0.05 * c_fs);
                double best = -1.0;
                int at = -1;
                for (int start = 0; start + window <= a.length(); start += 24)
                {
                    double p = 0.0;
                    for (int ch = 0; ch < a.channels(); ++ch)
                        for (int i = start; i < start + window; ++i)
                            p += static_cast<double>(a.samples.getSample(ch, i)) * a.samples.getSample(ch, i);
                    if (p > best)
                    {
                        best = p;
                        at = start + window / 2;
                    }
                }
                return at;
            };
            const auto closest = TapeRecorder::readCues(base.getChildFile("pt").getChildFile("Whoosh_001.wav"));
            const int peak = loudest(pt);
            bool setClosest = true;
            for (const auto& s : ptNames)
            {
                const auto c = TapeRecorder::readCues(base.getChildFile("mm").getChildFile(width == W::Mono ? juce::String("Whoosh_001.wav") : "Whoosh_001." + s + ".wav"));
                setClosest = setClosest && c.size() == 1 && ! closest.empty() && c[0].frame == closest[0].frame;
            }
            check(closest.size() == 1 && closest[0].label == "Closest" && std::abs(static_cast<int>(closest[0].frame) - peak) <= 48 && setClosest,
                  name + ": a Closest marker at the whoosh's loudest moment (" + (closest.empty() ? juce::String("none") : num(closest[0].frame / c_fs, 3) + " s")
                      + ", loudest 50 ms centred at " + num(peak / c_fs, 3) + " s), in every file of its set");

            // One File With Markers: a named marker at the start of each variation, in every file of a set.
            const auto all = readFile(base.getChildFile("pt").getChildFile("Whoosh_All.wav"));
            const auto cues = TapeRecorder::readCues(base.getChildFile("pt").getChildFile("Whoosh_All.wav"));
            const auto gap = juce::roundToInt(0.25 * c_fs);
            const auto closest2 = TapeRecorder::readCues(base.getChildFile("pt").getChildFile("Whoosh_002.wav"));
            const bool marked = cues.size() == 4 && cues[0].frame == 0 && cues[0].label == "Whoosh_001" && cues[2].label == "Whoosh_002"
                             && cues[1].label == "Whoosh_001 Closest" && ! closest.empty() && cues[1].frame == closest[0].frame
                             && cues[3].label == "Whoosh_002 Closest" && closest2.size() == 1 && cues[3].frame == cues[2].frame + closest2[0].frame
                             && cues[2].frame == pt.length() + gap && all.channels() == n && all.length() == pt.length() + gap + pt2.length();
            bool setMarked = mmProgress.files == 3 && mmProgress.filesPerTake == (width == W::Mono ? 1 : n);
            for (const auto& s : ptNames)
            {
                const auto f = base.getChildFile("mm").getChildFile(width == W::Mono ? juce::String("Whoosh_All.wav") : "Whoosh_All." + s + ".wav");
                const auto c = TapeRecorder::readCues(f);
                setMarked = setMarked && c.size() == 4 && c[2].frame == cues[2].frame && c[3].frame == cues[3].frame;
            }
            check(marked && setMarked, name + ": the one file has a marker at each variation and at its closest point (" + juce::String(static_cast<int>(cues.size()))
                                           + " markers" + (n > 1 ? ", in every file of its Multi-Mono set too)" : ")"));

            // What the file says about itself: variation, batch, width and (wider than Stereo) the room first, and
            // all of that inside the BWF description, which ends on a whole item.
            const auto described = TapeRecorder::readDescription(base.getChildFile("pt").getChildFile("Whoosh_001.wav"));
            juce::String bext;
            {
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                if (std::unique_ptr<juce::AudioFormatReader> r { formats.createReaderFor(base.getChildFile("pt").getChildFile("Whoosh_001.wav")) })
                    bext = r->metadataValues[juce::WavAudioFormat::bwavDescription];
            }
            const juce::String head = "Variation 1 of 2, Batch 4711, " + name + ".";
            const juce::String roomPart = "Flight Paths A Across, B Across, Stowaway Across. Spread ";
            const bool says = described.contains(head) && described.contains("Length 0.8 s") && (n > 2) == described.contains(roomPart)
                           && (n <= 2 || (described.contains("Centre Speaker Off") && described.contains("LFE Off")));
            const bool inBext = bext.getNumBytesAsUTF8() <= 256 && bext.endsWithChar('.') && described.startsWith(bext.dropLastCharacters(1)) && bext.contains(head)
                             && (n <= 2 || bext.contains("LFE Off"));
            check(says && inBext, name + ": the description puts the variation, batch, width" + (n > 2 ? juce::String(" and room") : juce::String())
                                      + " first, all of it in the BWF description (" + juce::String(static_cast<int>(bext.getNumBytesAsUTF8())) + " bytes: \""
                                      + bext.substring(0, 150) + "...\")");
        }

        // Mono is the Stereo render folded (the same rig, the same batch), and every width is the same batch: its
        // loudest moment falls in the same place.
        {
            // The silence in front of each file is cut where it first gets louder than -80 dBFS, which the fold
            // reaches a few samples later than the louder of L and R: the two are lined up first.
            const auto& m = firstOf[W::Mono];
            const auto& s = firstOf[W::Stereo];
            double best = 1.0;
            int shift = 0;
            for (int offset = 0; offset < 2000; ++offset)
            {
                double diff = 0.0, level = 0.0;
                const int len = juce::jmin(m.length(), s.length() - offset);
                for (int i = 0; i < len; ++i)
                {
                    const double fold = 0.5 * (static_cast<double>(s.samples.getSample(0, i + offset)) + s.samples.getSample(1, i + offset));
                    diff += (m.samples.getSample(0, i) - fold) * (m.samples.getSample(0, i) - fold);
                    level += fold * fold;
                }
                const double rel = level > 0.0 ? std::sqrt(diff / level) : 1.0;
                if (rel < best)
                {
                    best = rel;
                    shift = offset;
                }
            }
            // (and its end, where it falls under -120 dBFS, a little sooner).
            check(best < 1.0e-3 && std::abs(m.length() + shift - s.length()) < static_cast<int>(0.05 * c_fs),
                  "Mono: the Stereo render's fold, half of L plus R (difference " + num(20.0 * std::log10(juce::jmax(1.0e-12, best)), 1) + " dB, its front cut "
                      + juce::String(shift) + " samples later, its end " + juce::String(s.length() - shift - m.length()) + " sooner)");
            juce::String at;
            double spread = 0.0;
            const double stereoAt = loudestAt(s);
            for (const auto width : c_widths)
            {
                const double t = loudestAt(firstOf[width]);
                spread = juce::jmax(spread, std::abs(t - stereoAt));
                at << nameOf(width) << " " << num(t) << " s  ";
            }
            check(spread < 0.15, "every width is the same whoosh: loudest within " + num(spread * 1000.0, 0) + " ms of Stereo's (" + at.trim() + ")");
        }

        // The files join the Takes at their width; a Multi-Mono set is one take.
        {
            auto job = makeJob(rig, out.getChildFile("takes"), W::Surround714, 1, 9);
            job.options.layout = takefile::Layout::MultiMono;
            render(rig, job);
            job.options.layout = takefile::Layout::Interleaved;
            job.width = W::Surround51;
            render(rig, job);
            rig.proc.refreshTakes();
            const auto& takes = rig.proc.getTakes();
            const bool ok = takes.size() >= 2 && takes[0].format.width == W::Surround51 && takes[0].files.size() == 1
                         && SwooshbucklerAudioProcessor::widthTag(takes[0]) == "5.1" && takes[1].format.width == W::Surround714
                         && takes[1].files.size() == 12 && takes[1].format.layout == takefile::Layout::MultiMono
                         && SwooshbucklerAudioProcessor::widthTag(takes[1]) == "7.1.4" && ! takes[1].foldedFrom.has_value();
            check(ok, "the files join the Takes at their width (" + (takes.size() >= 2 ? SwooshbucklerAudioProcessor::widthTag(takes[0]) + ", "
                                                                                          + SwooshbucklerAudioProcessor::widthTag(takes[1]) + " as "
                                                                                          + juce::String(takes[1].files.size()) + " files in one take"
                                                                                    : juce::String("missing")) + ")");
        }

        // The Width choice: the rig's own width unless another is chosen.
        {
            auto& renderer = rig.proc.getVariations();
            check(! renderer.chosenWidth().has_value() && renderer.defaultJob().width == W::Stereo, "Width: the width the rig plays at, until one is chosen");
            renderer.chooseWidth(W::Surround714);
            check(renderer.defaultJob().width == W::Surround714, "Width: a chosen width is kept");
            renderer.chooseWidth(std::nullopt);
            rig.widen(W::Surround51);
            check(renderer.defaultJob().width == W::Surround51 && ! renderer.defaultJob().options.mono && ! renderer.defaultJob().options.stereo,
                  "Width: in a 5.1 track it starts at 5.1, never folded");
            rig.widen(W::Stereo);
        }

        // The estimate counts every channel, and a Multi-Mono set's one file per channel.
        {
            auto& renderer = rig.proc.getVariations();
            auto job = makeJob(rig, out.getChildFile("estimate"), W::Stereo, 100, 1);
            const auto stereo = renderer.estimate(job);
            job.width = W::Surround714;
            const auto wide = renderer.estimate(job);
            job.options.layout = takefile::Layout::MultiMono;
            const auto mono12 = renderer.estimate(job);
            const auto mono12Text = VariationRenderer::estimateText(job, mono12);
            check(mono12.files == 1200 && mono12.filesPerTake == 12 && mono12Text.startsWith("100 whooshes, each as 12 mono files (1,200 files): "),
                  "estimate: Multi-Mono says takes and files (" + mono12Text + ")");
            job.files = VariationRenderer::Files::Both;
            const auto bothText = VariationRenderer::estimateText(job, renderer.estimate(job));
            check(bothText.startsWith("100 whooshes and one set with all of them, each as 12 mono files (1,212 files): "), "estimate: " + bothText);
            job.files = VariationRenderer::Files::OneWithMarkers;
            const auto oneText = VariationRenderer::estimateText(job, renderer.estimate(job));
            check(oneText.startsWith("100 whooshes in one set of 12 mono files: "), "estimate: " + oneText);
            job.files = VariationRenderer::Files::PerVariation;
            job.options.layout = takefile::Layout::Interleaved;
            check(std::abs(wide.totalBytes / stereo.totalBytes - 6.0) < 1.0e-9 && wide.totalBytes > 0.0
                      && VariationRenderer::estimateText(job, wide).contains(" of sound (about ") && VariationRenderer::estimateText(job, wide).contains(" MB), ready in "),
                  "estimate: 7.1.4 files come to six times Stereo's (" + VariationRenderer::estimateText(job, wide) + ")");
            job.files = VariationRenderer::Files::OneWithMarkers;
            job.each = VariationRenderer::Each::WholeRide;
            job.rideSeconds = 120.0;
            job.count = 120;
            check(VariationRenderer::oneFileTooBig(job, renderer.estimate(job)), "estimate: four hours of 7.1.4 in one file is over 4 GB, too big for markers");
            job.options.layout = takefile::Layout::MultiMono;
            const auto set = renderer.estimate(job);
            check(! VariationRenderer::oneFileTooBig(job, set) && std::abs(set.oneFileBytes * 12.0 - renderer.estimate([&] { auto j = job; j.options.layout = takefile::Layout::Interleaved; return j; }()).oneFileBytes) < 1.0,
                  "estimate: as Multi-Mono each of its twelve files is a twelfth of that (" + VariationRenderer::roughSize(set.oneFileBytes) + " each)");
            job.options.layout = takefile::Layout::Interleaved;
            job.options.type = TapeRecorder::FileType::Aiff;
            job.width = W::Stereo;
            job.count = 60;
            juce::String error;
            check(VariationRenderer::oneFileTooBig(job, renderer.estimate(job)) && ! renderer.start(job, error) && error.contains("2 GB")
                      && ! out.getChildFile("estimate").exists(),
                  "estimate: an AIFF file with markers over 2 GB is refused before anything is rendered (" + error + ")");
        }

        // FLAC: one file holds at most eight channels. 7.1.4 interleaved is refused before anything is rendered;
        // 7.1.4 as Multi-Mono and 7.1 in one file are written.
        {
            auto& renderer = rig.proc.getVariations();
            auto job = makeJob(rig, out.getChildFile("flac714"), W::Surround714, 2, 3);
            job.options.type = TapeRecorder::FileType::Flac;
            juce::String error;
            const bool started = renderer.start(job, error);
            if (started)
                renderer.waitUntilDone(60000);
            check(! started && error.contains("at most 8 channels") && error.contains("7.1.4 has 12") && VariationRenderer::formatProblem(job) == error
                      && ! job.folder.exists(),
                  "FLAC: 7.1.4 in one file is refused with a plain message, before anything is rendered (" + error + ")");
            job.options.layout = takefile::Layout::MultiMono;
            check(VariationRenderer::formatProblem(job).isEmpty(), "FLAC: 7.1.4 as Multi-Mono is fine");
            job.count = 1;
            render(rig, job);
            const auto flacs = filesIn(job.folder);
            check(flacs.size() == 12 && flacs[0].hasFileExtension("flac") && readFile(flacs[0]).channels() == 1, "FLAC: 7.1.4 as Multi-Mono is twelve mono FLAC files");
            job.folder = out.getChildFile("flac71");
            job.options.layout = takefile::Layout::Interleaved;
            job.width = W::Surround71;
            render(rig, job);
            const auto f71 = filesIn(job.folder);
            check(f71.size() == 1 && readFile(f71[0]).channels() == 8, "FLAC: 7.1 fits one FLAC file (8 channels)");
        }

        // A 7.1.4 render's sound is really in the room. Each speaker's share of the whole room's energy, from
        // Multi-Mono renders, with the passes Across (the default), going Around and Overhead.
        {
            auto share = [&](const char* path, float value)
            {
                rig.set(ParamIDs::flightPath[0], value);
                rig.set(ParamIDs::flightPath[1], value);
                auto job = makeJob(rig, out.getChildFile(juce::String("room-") + path), W::Surround714, 3, 77);
                job.options.layout = takefile::Layout::MultiMono;
                render(rig, job);
                std::map<juce::String, double> sum;
                for (const auto* stem : { "Whoosh_001", "Whoosh_002", "Whoosh_003" })
                    for (const auto& [speaker, s] : shares(job.folder, W::Surround714, stem))
                        sum[speaker] += s / 3.0;
                juce::String line;
                for (const auto& [speaker, s] : sum)
                    line << speaker << " " << juce::String(100.0 * s, 1) << "%  ";
                std::cout << "         " << path << ": " << line << std::endl;
                return sum;
            };
            auto part = [](std::map<juce::String, double>& s, std::initializer_list<const char*> names)
            {
                double total = 0.0;
                for (const auto* n : names)
                    total += s[n];
                return total;
            };
            auto across = share("across", 0.0f);
            auto around = share("around", 3.0f);
            auto overhead = share("overhead", 2.0f);
            rig.set(ParamIDs::flightPath[0], 0.0f);
            rig.set(ParamIDs::flightPath[1], 0.0f);
            const double acrossOut = 1.0 - part(across, { "L", "R" });
            check(acrossOut > 0.05, "in the room: Across is not in L and R alone (" + num(100.0 * acrossOut, 1) + "% elsewhere)");
            const double sides = part(around, { "Lss", "Rss" }), rears = part(around, { "Lsr", "Rsr" }), fronts = part(around, { "L", "R", "C" });
            check(sides > 0.1 && rears > 0.1,
                  "in the room: going Around, the sides and the rears each carry their share (" + num(100.0 * sides, 1) + "%, " + num(100.0 * rears, 1)
                      + "%; the fronts " + num(100.0 * fronts, 1) + "%)");
            const double tops = part(overhead, { "Ltf", "Rtf", "Ltr", "Rtr" });
            check(tops > 0.15, "in the room: Overhead, the tops carry " + num(100.0 * tops, 1) + "% of it");
        }
    }

    // The app: its own rig plays its Listening width only (AppTest), but Render Variations' second rig is no app and renders any
    // width. A stretch of the Whole Ride in 5.1 and a whoosh in 7.1.4 from an app's rig: the right channels, sound
    // outside L and R, and every channel faded in and out (no channel left to start or stop at full level).
    std::cout << "Render Variations in the app, and a Whole Ride in a room" << std::endl;
    {
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
        Rig rig(sources);
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        auto wide = rig.proc.getBusesLayout();
        wide.outputBuses.getReference(0) = room::channelSet(W::Surround51);
        check(rig.proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone && ! rig.proc.checkBusesLayoutSupported(wide),
              "the app's own rig refuses a 5.1 output while Listening is Stereo");
        auto edges = [](const Audio& a, int& sounding)
        {
            // The loudest of every channel's first and last sample, against the file's peak; and how many channels carry sound.
            float edge = 0.0f;
            sounding = 0;
            for (int ch = 0; ch < a.channels(); ++ch)
            {
                edge = juce::jmax(edge, std::abs(a.samples.getSample(ch, 0)), std::abs(a.samples.getSample(ch, a.length() - 1)));
                if (a.samples.getMagnitude(ch, 0, a.length()) > 1.0e-3f)
                    ++sounding;
            }
            const float peak = a.samples.getMagnitude(0, a.length());
            return peak > 0.0f ? edge / peak : 1.0f;
        };
        auto job = makeJob(rig, out.getChildFile("app-ride"), W::Surround51, 2, 31);
        job.each = VariationRenderer::Each::WholeRide;
        job.rideSeconds = 1.5;
        render(rig, job);
        const auto ride = readFile(job.folder.getChildFile("Whoosh_001.wav"));
        int sounding = 0;
        const float rideEdge = ride.length() > 0 ? edges(ride, sounding) : 1.0f;
        // (The LFE is off by default and the centre speaker too: L, R, Ls and Rs carry the sound.)
        check(ride.channels() == 6 && std::abs(ride.length() - juce::roundToInt(1.5 * c_fs)) <= 1 && sounding == 4 && rideEdge < 0.01f,
              "the app: a Whole Ride in 5.1 has 6 channels, sound in the surrounds, and every channel fades in and out (edges "
                  + num(100.0 * rideEdge, 2) + "% of the peak, " + juce::String(sounding) + " channels with sound)");
        job = makeJob(rig, out.getChildFile("app-714"), W::Surround714, 1, 31);
        render(rig, job);
        const auto whoosh = readFile(job.folder.getChildFile("Whoosh_001.wav"));
        const float whooshEnd = [&]
        {
            float e = 0.0f;
            for (int ch = 0; ch < whoosh.channels(); ++ch)
                e = juce::jmax(e, std::abs(whoosh.samples.getSample(ch, whoosh.length() - 1)));
            return whoosh.length() > 0 ? e / juce::jmax(1.0e-9f, whoosh.samples.getMagnitude(0, whoosh.length())) : 1.0f;
        }();
        check(whoosh.channels() == 12 && whooshEnd < 0.01f,
              "the app: a whoosh in 7.1.4 has 12 channels and ends in silence on every one (" + num(100.0 * whooshEnd, 2) + "% of the peak)");
    }

    // The tape in a 7.1.4 room.
    std::cout << "The tape in a 7.1.4 room" << std::endl;
    {
        // The Stereo fold itself: the room's fold, lined up with the take, held under the Ceiling, and labelled.
        {
            const auto dir = out.getChildFile("fold");
            dir.createDirectory();
            const int len = 2 * 48000;
            juce::AudioBuffer<float> take(12, len);
            juce::Random r(5);
            for (int ch = 0; ch < 12; ++ch)
                for (int i = 0; i < len; ++i)
                    take.setSample(ch, i, (i < len / 2 ? 0.02f : 1.0f) * (r.nextFloat() * 2.0f - 1.0f)); // quiet, then everything at full scale
            TapeRecorder::Options o;
            o.bitDepth = 32;
            o.stereo = true;
            o.foldCeilingDb = -1.0f;
            TapeRecorder::Naming naming;
            naming.description = "A test take.";
            juce::String error;
            const auto file = dir.getChildFile("Fold.wav");
            check(TapeRecorder::writeFile(file, take, 0, len, c_fs, o, naming, error), "the Stereo fold is written " + error);
            const auto folded = readFile(file);
            const auto& matrix = room::fold(W::Surround714, W::Stereo);
            bool lined = folded.channels() == 2 && folded.length() == len;
            for (int out2 = 0; out2 < 2 && lined; ++out2)
                for (const int i : { 100, 4000, 20000, len / 2 - 2000 })
                {
                    double expect = 0.0;
                    for (int ch = 0; ch < 12; ++ch)
                        expect += matrix.gain[static_cast<size_t>(out2)][static_cast<size_t>(ch)] * take.getSample(ch, i);
                    lined = lined && std::abs(folded.samples.getSample(out2, i) - expect) < 1.0e-5;
                }
            const float peak = folded.samples.getMagnitude(0, folded.length());
            check(lined, "the Stereo fold of a quiet 7.1.4 take is the room's fold, sample for sample, lined up with the take");
            check(peak <= juce::Decibels::decibelsToGain(-1.0f) * 1.001f,
                  "the Stereo fold of twelve full-scale channels is held under the Ceiling (peak " + num(juce::Decibels::gainToDecibels(peak), 2) + " dBFS, Ceiling -1)");
            check(TapeRecorder::readDescription(file).startsWith("Folded down from 7.1.4 to Stereo, held under the Ceiling. A test take."),
                  "the fold says so in its description (" + TapeRecorder::readDescription(file).substring(0, 70) + ")");
            o.played = W::Surround714;
            check(TapeRecorder::foldedFrom(o, 12) == W::Surround714 && ! TapeRecorder::foldedFrom({}, 12).has_value()
                      && TapeRecorder::fileWidthOf(o, 2) == W::Stereo && TapeRecorder::fileWidthOf(o, 6) == W::Stereo,
                  "a file narrower than the room it was played in is a fold");
        }

        // The rig in a 7.1.4 track: the saved last minutes as played, folded to Stereo, as Multi-Mono, and in FLAC.
        {
            Rig rig(sources);
            rig.widen(W::Surround714);
            rig.proc.setPlaying(true);
            rig.run(3.0);
            auto& p = rig.proc;
            juce::File written;
            juce::String error;
            check(p.getTapeFold() == SwooshbucklerAudioProcessor::TapeFold::AsPlayed && p.getTapeWidth() == W::Surround714, "the tape: as played, unless folded");
            check(p.saveRetro(written, error) && readFile(written).channels() == 12 && SwooshbucklerAudioProcessor::widthTag(p.getTakes()[0]) == "7.1.4",
                  "the tape: as played, a 7.1.4 take of 12 channels " + error);
            p.setTapeFold(SwooshbucklerAudioProcessor::TapeFold::Stereo);
            check(p.saveRetro(written, error) && readFile(written).channels() == 2 && p.getTakes()[0].foldedFrom == W::Surround714
                      && SwooshbucklerAudioProcessor::widthTag(p.getTakes()[0]) == "Stereo Fold"
                      && TapeRecorder::readDescription(written).startsWith("Folded down from 7.1.4 to Stereo"),
                  "the tape: folded to Stereo, a labelled fold in the file and in the Takes (" + SwooshbucklerAudioProcessor::widthTag(p.getTakes()[0]) + ")");
            check(readFile(written).samples.getMagnitude(0, readFile(written).length()) <= juce::Decibels::decibelsToGain(p.tapeOptions().foldCeilingDb) * 1.001f,
                  "the tape: the Stereo fold is under the Ceiling");
            p.setTapeFold(SwooshbucklerAudioProcessor::TapeFold::Mono);
            check(p.saveRetro(written, error) && readFile(written).channels() == 1 && SwooshbucklerAudioProcessor::widthTag(p.getTakes()[0]) == "Mono Fold",
                  "the tape: folded to Mono, a labelled fold");
            p.setTapeFold(SwooshbucklerAudioProcessor::TapeFold::AsPlayed);
            p.setFileLayout(takefile::Layout::MultiMono);
            check(p.saveRetro(written, error) && p.getTakes()[0].files.size() == 12 && SwooshbucklerAudioProcessor::widthTag(p.getTakes()[0]) == "7.1.4",
                  "the tape: a Multi-Mono set is one 7.1.4 take of twelve files");
            p.setFileLayout(takefile::Layout::Interleaved);
            p.setFileType(2);
            const auto before = filesIn(p.getRecordFolder()).size();
            juce::String retroError, armError;
            const bool saved = p.saveRetro(written, retroError);
            const bool armed = p.arm(armError);
            if (armed)
                p.stopRecording();
            check(! saved && ! armed && retroError.contains("at most 8 channels") && armError == retroError && filesIn(p.getRecordFolder()).size() == before,
                  "the tape: FLAC can't hold 7.1.4 in one file, and says so before anything is written (" + armError + ")");
            p.setFileType(0);
            p.setPlaying(false);

            // The saved last minutes' length is the real one under the 512 MB cap: 7.1.4 at 192 kHz.
            p.setRetroMinutes(5);
            rig.widen(W::Surround714, 192000.0);
            const double kept = TapeRecorder::retroCapacityFor(192000.0, 12, 300);
            check(kept < 60.0 && std::abs(p.getRetroSecondsKept() - kept) < 1.0e-6 && p.retroLabel() == "Last " + juce::String(static_cast<int>(kept)) + " s",
                  "the saved last minutes: 7.1.4 at 192 kHz holds " + num(kept) + " s of the 5 minutes asked, and the button says " + p.retroLabel());
            p.setRetroMinutes(1);
            rig.widen(W::Stereo);
            p.setTapeFold(SwooshbucklerAudioProcessor::TapeFold::Stereo);
            check(p.getTapeWidth() == W::Stereo && ! TapeRecorder::foldedFrom(p.tapeOptions(), 2).has_value(), "the tape: in Stereo, Stereo is as played, not a fold");
            p.setTapeFold(SwooshbucklerAudioProcessor::TapeFold::AsPlayed);
        }
    }

    SwooshbucklerAudioProcessor::useSettings(nullptr);
    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
