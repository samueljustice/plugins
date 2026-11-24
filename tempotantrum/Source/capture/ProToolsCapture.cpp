#include "ProToolsCapture.h"

#include "protools/ProToolsDriver.h"
#include "protools/PtslSession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>

namespace tt::capture
{

namespace
{
    using Clock = std::chrono::steady_clock;

    juce::String utf8 (const std::string& text)
    {
        return juce::String::fromUTF8 (text.c_str());
    }

    juce::File mapFileBeside (const juce::File& audio)
    {
        // The same name the map file code gives: "<file name>.tempotantrum.json".
        return audio.getSiblingFile (audio.getFileName() + ".tempotantrum.json");
    }

    // Pro Tools' error, when it says the command is not there or not supported.
    bool saysUnsupported (const ptsl::Reply& reply)
    {
        const auto text = (utf8 (reply.message) + " " + utf8 (reply.error)).toLowerCase();
        return text.contains ("not supported") || text.contains ("unsupported") || text.contains ("not known to this build")
               || text.contains ("unknown command") || text.contains ("not implemented") || text.contains ("unknown_command");
    }

    // Every channel of every file, in order. False with a sentence when a file cannot be read.
    bool readChannels (juce::AudioFormatManager& formats, const std::vector<juce::File>& files, Channels& channels,
                       RenderedTrack& info, juce::String& problem)
    {
        for (const auto& file : files)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

            if (reader == nullptr)
            {
                problem = "Could not read the file Pro Tools rendered, " + file.getFileName();
                return false;
            }

            if (info.sampleRate <= 0.0)
            {
                info.sampleRate = reader->sampleRate;
                info.bitsPerSample = (int) reader->bitsPerSample;
                info.floatingPoint = reader->usesFloatingPointData;
                info.length = reader->lengthInSamples;
            }
            else if (std::abs (reader->sampleRate - info.sampleRate) > 0.5)
            {
                problem = "Pro Tools rendered the channels of one track at different sample rates";
                return false;
            }

            const int count = (int) reader->numChannels;
            const auto length = (int) std::min<juce::int64> (reader->lengthInSamples, std::numeric_limits<int>::max() / 2);
            juce::AudioBuffer<float> buffer (count, length);

            if (! reader->read (&buffer, 0, length, 0, true, true))
            {
                problem = "Could not read the file Pro Tools rendered, " + file.getFileName();
                return false;
            }

            for (int c = 0; c < count; ++c)
                channels.emplace_back (buffer.getReadPointer (c), buffer.getReadPointer (c) + length);

            info.channels += count;
        }

        return true;
    }

    juce::Result writeWav (const juce::File& file, const Channels& channels, double sampleRate)
    {
        auto stream = file.createOutputStream();

        if (stream == nullptr)
            return juce::Result::fail ("Could not write to " + file.getParentDirectory().getFullPathName());

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate, (unsigned int) channels.size(), 32, {}, 0));

        if (writer == nullptr)
            return juce::Result::fail ("Could not make a WAV file at this sample rate");

        stream.release(); // the writer owns the stream now

        std::vector<const float*> pointers;

        for (const auto& channel : channels)
            pointers.push_back (channel.data());

        const auto length = channels.empty() ? 0 : channels.front().size();

        if (! writer->writeFromFloatArrays (pointers.data(), (int) pointers.size(), (int) length))
            return juce::Result::fail ("Could not write the capture");

        return juce::Result::ok();
    }
}

//==============================================================================
juce::File capturesFolder()
{
    const auto folder = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                            .getChildFile ("Library/Caches/Sammy J/Tempo Tantrum/Captures");
    folder.createDirectory();
    return folder;
}

int pruneCaptures (const juce::File& folder, std::size_t keep, const juce::File& keepAlways)
{
    if (! folder.isDirectory())
        return 0;

    std::vector<StoredCapture> stored;

    for (const auto& entry : juce::RangedDirectoryIterator (folder, false, juce::String (filePrefix) + "*.wav", juce::File::findFiles))
        if (entry.getFile() != keepAlways)
            stored.push_back ({ entry.getFile().getFileNameWithoutExtension().toStdString(), entry.getModificationTime().toMilliseconds() });

    // The capture that must stay counts as the newest.
    const auto gone = capturesToRemove (stored, keepAlways.existsAsFile() && keep > 0 ? keep - 1 : keep);
    int removed = 0;

    for (const auto& stem : gone)
    {
        const auto audio = folder.getChildFile (utf8 (stem) + ".wav");

        if (audio.deleteFile())
            ++removed;

        mapFileBeside (audio).deleteFile();
    }

    // Folders of Pro Tools' own files that a capture did not get to clear away (the app was
    // quit part way), once they are ten minutes old.
    const auto old = juce::Time::getCurrentTime() - juce::RelativeTime::minutes (10);

    for (const auto& entry : juce::RangedDirectoryIterator (folder, false, juce::String (filePrefix) + "* renders", juce::File::findDirectories))
        if (entry.getModificationTime() < old)
            entry.getFile().deleteRecursively();

    return removed;
}

//==============================================================================
Outcome captureSelection (ptsl::Client& client, const std::function<void (const juce::String&)>& progress,
                          const std::function<bool()>& stop, const Options& options)
{
    const auto started = Clock::now();
    Outcome o;

    const auto say = [&o] (const std::string& line) { o.log.push_back (line); };
    const auto finish = [&o, &started] (juce::String error = {})
    {
        o.ok = error.isEmpty();
        o.error = std::move (error);
        o.seconds = std::chrono::duration<double> (Clock::now() - started).count();
        return o;
    };
    const auto tell = [&progress] (const juce::String& text)
    {
        if (progress)
            progress (text);
    };
    const auto audioSuiteInstead = "This Pro Tools has no offline track bounce; use Tempo Tantrum Capture in AudioSuite instead";

    if (! ptsl::Client::hasLink())
    {
        say (ptsl::noLinkMessage);
        o.useAudioSuite = true;
        return finish (audioSuiteInstead);
    }

    tell ("Looking at the selection in Pro Tools\xe2\x80\xa6");

    std::string problem;

    // Pro Tools running without a scripting link that answers: Tempo Tantrum Capture may still work.
    const auto noLink = [&]
    {
        o.useAudioSuite = pt::Driver::isRunning();
        return finish (o.useAudioSuite ? "Pro Tools' scripting link did not answer" : "Pro Tools is not open");
    };

    if (! pt::openLink (client, problem))
    {
        say ("No link: " + problem);
        return noLink();
    }

    const auto version = client.ptslVersion();
    say ("Scripting version " + (version.ok() ? version.value : "not read: " + version.reply.message));

    if (! version.ok())
        return noLink();

    if (! hasTrackBounce (version.value))
    {
        o.useAudioSuite = true;
        return finish (audioSuiteInstead);
    }

    const auto session = client.sessionName();

    if (! session.ok() || session.value.empty())
    {
        say ("Session name: " + (session.ok() ? std::string ("none") : session.reply.message));
        return finish ("Open the session in Pro Tools first");
    }

    const auto rate = client.sessionSampleRate();
    const auto depth = client.sessionBitDepth();
    const auto selection = client.editSelection ("TLType_Samples");
    long long in = 0, out = 0;

    if (! rate.ok() || ! selection.ok() || ! pt::samplesOf (selection.value.in, in) || ! pt::samplesOf (selection.value.out, out))
    {
        say ("Rate or selection not read: " + (rate.ok() ? selection.reply.message : rate.reply.message));
        return finish (selection.reply.timedOut || rate.reply.timedOut ? "Pro Tools did not answer. Close any window open in it and try again"
                                                                       : "Could not read the selection in Pro Tools");
    }

    o.selectionIn = in;
    o.selectionOut = out;
    say ("Edit selection " + std::to_string (in) + " to " + std::to_string (out) + " (" + std::to_string (out - in) + " samples) at "
         + std::to_string ((int) rate.value) + " Hz");

    if (out <= in)
        return finish ("Select the music in Pro Tools first");

    const auto tracks = client.trackList();

    if (! tracks.ok())
    {
        say ("Track list not read: " + tracks.reply.message);
        return finish ("Could not read the tracks in Pro Tools");
    }

    std::vector<ptsl::Track> chosen;
    int skipped = 0;

    for (const auto& track : tracks.value)
    {
        if (! track.selected)
            continue;

        if (canBounce (track.type))
            chosen.push_back (track);
        else
            ++skipped;
    }

    say ("Selected tracks: " + std::to_string (chosen.size()) + " to render" + (skipped > 0 ? ", " + std::to_string (skipped) + " left out (not audio)" : std::string()));

    if (chosen.empty())
        return finish ("Select the music in Pro Tools first");

    // The start as the Main Counter shows it, for the name.
    const auto counter = client.mainCounterFormat();
    const bool inBars = counter.ok() && counter.value.type.find ("Bars") != std::string::npos;
    const auto at = client.timeAsType (std::to_string (in), "TLType_Samples", inBars ? "TLType_BarsBeats" : "TLType_TimeCode");
    const auto label = startLabel (counter.ok() ? counter.value.type : std::string(), inBars && at.ok() ? at.value : std::string(),
                                   ! inBars && at.ok() ? at.value : std::string());

    // Where everything goes: Pro Tools' files in a folder of their own, the capture beside it.
    const auto folder = options.folder == juce::File() ? capturesFolder() : options.folder;
    folder.createDirectory();

    auto stem = juce::String (filePrefix) + " " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S");

    for (int n = 2; folder.getChildFile (stem + ".wav").exists() || folder.getChildFile (stem + " renders").exists(); ++n)
        stem = juce::String (filePrefix) + " " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S") + " " + juce::String (n);

    const auto renders = folder.getChildFile (stem + " renders");

    if (! renders.createDirectory())
        return finish ("Could not make the folder " + renders.getFullPathName());

    const auto cleanUp = [&]
    {
        if (! options.keepRenders)
            renders.deleteRecursively();
    };

    // An offline bounce is quicker than playing the selection, but it is given as long as
    // playing it three times over, and at least two minutes.
    const double selectionSeconds = (double) (out - in) / rate.value;
    const int timeoutMs = (int) std::clamp (selectionSeconds * 3000.0 + 60000.0, 120000.0, 3600000.0);

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::vector<Channels> rendered;
    std::set<juce::File> taken;
    double fileRate = 0.0;

    for (size_t i = 0; i < chosen.size(); ++i)
    {
        const auto& track = chosen[i];

        tell (chosen.size() == 1 ? juce::String (renderingText)
                                 : "Rendering track " + juce::String ((int) i + 1) + " of " + juce::String ((int) chosen.size())
                                       + " of the selection in Pro Tools\xe2\x80\xa6");

        BounceSpec spec;
        spec.trackId = track.id;
        spec.in = in;
        spec.out = out;
        spec.directory = renders.getFullPathName().toStdString();
        spec.sampleRate = (int) std::lround (rate.value);
        spec.bitDepth = bitDepthName (depth.ok() ? depth.value : std::string());

        const auto request = bounceTrackRequest (spec);
        say ("BounceTrack " + request);

        client.setStopCheck (stop);
        const auto bounce = client.bounceTrack (request, timeoutMs);
        client.setStopCheck ({});

        say ("  " + std::to_string (bounce.reply.seconds) + " s: " + (bounce.ok() ? std::string ("ok") : "FAILED: " + bounce.reply.message));

        if (! bounce.reply.body.empty())
            say ("  answer: " + bounce.reply.body);

        if (! bounce.reply.error.empty())
            say ("  error: " + bounce.reply.error);

        if (! bounce.ok())
        {
            // Stopped or out of time, Pro Tools may still be bouncing into the folder: it is left
            // for pruneCaptures to clear once it is ten minutes old. And nothing else is tried
            // while Pro Tools may be busy.
            if (bounce.reply.stopped)
                return finish ("Stopped");

            if (bounce.reply.timedOut)
                return finish ("Pro Tools did not finish rendering the selection in time. It may still be rendering; try again once it has finished");

            cleanUp();

            // Any other failure of the bounce: the window tries Tempo Tantrum Capture next.
            o.useAudioSuite = true;

            if (saysUnsupported (bounce.reply))
                return finish (audioSuiteInstead);

            return finish ("Pro Tools could not render the selection: " + utf8 (bounce.reply.message));
        }

        RenderedTrack info;
        info.name = track.name;
        info.id = track.id;
        info.seconds = bounce.reply.seconds;
        info.files = bounce.value;

        std::vector<juce::File> files;

        for (const auto& path : bounce.value)
            files.push_back (juce::File (utf8 (path)));

        // Without a list in the answer: whatever it wrote into the folder that is not taken yet.
        if (files.empty())
        {
            for (const auto& entry : juce::RangedDirectoryIterator (renders, false, "*.wav", juce::File::findFiles))
                if (taken.count (entry.getFile()) == 0)
                    files.push_back (entry.getFile());

            std::sort (files.begin(), files.end());
        }

        for (const auto& file : files)
            taken.insert (file);

        if (files.empty())
        {
            cleanUp();
            return finish ("Pro Tools rendered no file for " + utf8 (track.name));
        }

        // One output of the track (see filesToUse).
        {
            std::vector<int> counts;

            for (const auto& file : files)
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                counts.push_back (reader != nullptr ? (int) reader->numChannels : 0);
            }

            std::vector<juce::File> used;

            for (const auto index : filesToUse (counts))
                used.push_back (files[index]);

            if (used.size() < files.size())
            {
                say ("  " + std::to_string (files.size()) + " outputs; using " + used.front().getFileName().toStdString() + " ("
                     + std::to_string (counts[(size_t) std::distance (files.begin(), std::find (files.begin(), files.end(), used.front()))])
                     + " channels)");
                info.outputs = (int) files.size();
            }

            files = used;
        }

        Channels channels;
        juce::String why;

        if (! readChannels (formats, files, channels, info, why))
        {
            cleanUp();
            return finish (why);
        }

        if (fileRate <= 0.0)
            fileRate = info.sampleRate;
        else if (std::abs (fileRate - info.sampleRate) > 0.5)
        {
            cleanUp();
            return finish ("Pro Tools rendered the tracks at different sample rates");
        }

        say ("  " + std::to_string (files.size()) + (files.size() == 1 ? " file, " : " files, ") + std::to_string (info.channels) + " channel(s), "
             + std::to_string (info.length) + " samples at " + std::to_string ((int) info.sampleRate) + " Hz, "
             + std::to_string (info.bitsPerSample) + (info.floatingPoint ? "-bit float" : "-bit"));

        o.tracks.push_back (info);
        rendered.push_back (std::move (channels));
    }

    // The selection's length at the files' rate (the session's, unless Track Bounce was set to
    // another): every track starts at the selection's start, and is padded or cut to its end.
    const auto length = (std::size_t) std::llround ((double) (out - in) * fileRate / rate.value);
    const auto summed = sumTracks (rendered, length);
    const auto audio = folder.getChildFile (stem + ".wav");

    tell ("Opening the capture\xe2\x80\xa6");

    if (const auto written = writeWav (audio, summed, fileRate); written.failed())
    {
        cleanUp();
        return finish (written.getErrorMessage());
    }

    cleanUp();
    pruneCaptures (folder, capturesKept, audio);

    std::vector<std::string> names;

    for (const auto& track : chosen)
        names.push_back (track.name);

    auto& c = o.capture;
    c.audio = audio;
    c.startSample = in;
    c.endSample = out;
    c.sessionRate = rate.value;
    c.sessionName = utf8 (session.value);
    c.name = utf8 (captureName (names, label));
    c.source = chosen.size() == 1 ? "an offline Track Bounce of " + utf8 (chosen.front().name)
                                  : "offline Track Bounces of " + juce::String ((int) chosen.size()) + " tracks";

    if (chosen.size() > 1)
        c.note = juce::String ((int) chosen.size()) + " tracks were rendered and summed into one " + (summed.size() == 2 ? "stereo" : "mono") + " file";

    for (const auto& track : o.tracks)
        if (track.outputs > 1)
            c.note << (c.note.isEmpty() ? "" : ". ") << utf8 (track.name) << " goes to " << track.outputs << " outputs; its "
                   << (track.channels == 2 ? "stereo" : track.channels == 1 ? "mono" : juce::String (track.channels) + "-channel") << " one was used";

    if (std::abs (fileRate - rate.value) > 0.5)
        c.note << (c.note.isEmpty() ? "" : ". ") << "Track Bounce rendered at " << juce::String (fileRate / 1000.0, 1)
               << " kHz, not the session's " << juce::String (rate.value / 1000.0, 1) << " kHz";

    say ("Capture " + audio.getFullPathName().toStdString() + ": " + std::to_string (summed.size()) + " channel(s), " + std::to_string (length) + " samples");
    return finish();
}

//==============================================================================
CaptureJob::CaptureJob() : juce::Thread ("Get from Pro Tools") {}

CaptureJob::~CaptureJob()
{
    alive->store (false);
    // A wait for Pro Tools asks every tenth of a second whether to stop.
    stopThread (20000);
}

bool CaptureJob::start (std::function<void (const juce::String&)> onProgress, std::function<void (const Outcome&)> onDone)
{
    if (isThreadRunning())
        return false;

    progressCallback = std::move (onProgress);
    doneCallback = std::move (onDone);
    return startThread();
}

void CaptureJob::run()
{
    ptsl::Client client;
    const auto flag = alive;

    const auto outcome = captureSelection (client, [this, flag] (const juce::String& line)
    {
        juce::MessageManager::callAsync ([this, flag, line]
        {
            if (flag->load() && progressCallback)
                progressCallback (line);
        });
    }, [this] { return threadShouldExit(); });

    juce::MessageManager::callAsync ([this, flag, outcome]
    {
        if (flag->load() && doneCallback)
            doneCallback (outcome);
    });
}

} // namespace tt::capture
