// CoreFoundation first: after JuceHeader.h's "using namespace juce", its Point would be ambiguous.
#if defined (__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include "CaptureHandOff.h"


namespace tt::audiosuite
{

namespace
{
    constexpr const char* formatName = "Tempo Tantrum capture";
    constexpr int formatVersion = 1;
    constexpr const char* handledEnding = ".handled.json";

    juce::String secondsText (juce::int64 samples, double sampleRate)
    {
        if (sampleRate <= 0.0)
            return juce::String (samples) + " samples";

        return juce::String ((double) samples / sampleRate, 2) + " s";
    }
}

juce::File capturesFolder()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
               .getChildFile ("Library/Caches/Sammy J/Tempo Tantrum/Captures");
}

juce::String toJson (const CaptureInfo& info)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", formatName);
    root->setProperty ("version", formatVersion);
    root->setProperty ("wav", info.wav.getFileName());
    root->setProperty ("sampleRate", info.sampleRate);
    root->setProperty ("channels", info.channels);
    root->setProperty ("lengthSamples", info.lengthSamples);
    root->setProperty ("handleSamples", info.handleSamples);
    root->setProperty ("handleSeconds", info.sampleRate > 0.0 ? (double) info.handleSamples / info.sampleRate : 0.0);
    root->setProperty ("handleAssumed", info.handleAssumed);

    if (info.handleAssumed)
        root->setProperty ("handleNote", "Pro Tools does not tell the plug-in its handle length, so the AudioSuite default of 2 s is assumed");

    auto* head = new juce::DynamicObject();
    const auto& p = info.playHead;

    if (p.hasBpm)
        head->setProperty ("bpm", p.bpm);

    if (p.hasTimeSignature)
    {
        head->setProperty ("timeSignatureNumerator", p.numerator);
        head->setProperty ("timeSignatureDenominator", p.denominator);
    }

    if (p.hasPpq)
        head->setProperty ("ppqPosition", p.ppqPosition);

    if (p.hasTimeInSamples)
        head->setProperty ("timeInSamples", p.timeInSamples);

    root->setProperty ("playHead", juce::var (head));
    root->setProperty ("captured", info.captured.toISO8601 (true));
    root->setProperty ("plugInVersion", info.plugInVersion);

    return juce::JSON::toString (juce::var (root));
}

bool readCaptureInfo (const juce::File& json, CaptureInfo& info, juce::String& problem)
{
    const auto parsed = juce::JSON::parse (json.loadFileAsString());

    if (parsed.getProperty ("format", {}).toString() != formatName)
    {
        problem = json.getFileName() + " is not a Tempo Tantrum capture";
        return false;
    }

    info = {};
    info.json = json;
    info.wav = json.getSiblingFile (parsed.getProperty ("wav", {}).toString());
    info.sampleRate = (double) parsed.getProperty ("sampleRate", 0.0);
    info.channels = (int) parsed.getProperty ("channels", 0);
    info.lengthSamples = (juce::int64) parsed.getProperty ("lengthSamples", 0);
    info.handleSamples = juce::jmax ((juce::int64) 0, (juce::int64) parsed.getProperty ("handleSamples", 0));
    info.handleAssumed = (bool) parsed.getProperty ("handleAssumed", true);
    info.captured = juce::Time::fromISO8601 (parsed.getProperty ("captured", {}).toString());
    info.plugInVersion = parsed.getProperty ("plugInVersion", {}).toString();

    const auto head = parsed.getProperty ("playHead", {});
    auto& p = info.playHead;

    if ((p.hasBpm = head.hasProperty ("bpm")))
        p.bpm = (double) head.getProperty ("bpm", 0.0);

    if ((p.hasTimeSignature = head.hasProperty ("timeSignatureNumerator")))
    {
        p.numerator = (int) head.getProperty ("timeSignatureNumerator", 4);
        p.denominator = (int) head.getProperty ("timeSignatureDenominator", 4);
    }

    if ((p.hasPpq = head.hasProperty ("ppqPosition")))
        p.ppqPosition = (double) head.getProperty ("ppqPosition", 0.0);

    if ((p.hasTimeInSamples = head.hasProperty ("timeInSamples")))
        p.timeInSamples = (juce::int64) head.getProperty ("timeInSamples", 0);

    if (info.sampleRate <= 0.0 || info.channels <= 0 || info.lengthSamples <= 0)
    {
        problem = json.getFileName() + " describes no audio";
        return false;
    }

    if (! info.wav.existsAsFile())
    {
        problem = "The audio of " + json.getFileName() + " is missing";
        return false;
    }

    return true;
}

bool isCaptureDescription (const juce::String& fileName, bool* handled)
{
    if (! fileName.startsWith (juce::String (filePrefix) + " ") || ! fileName.endsWithIgnoreCase (".json"))
        return false;

    const bool used = fileName.endsWithIgnoreCase (handledEnding);
    const auto stem = fileName.dropLastCharacters (used ? (int) std::strlen (handledEnding) : 5);

    // The plug-in's names have no other dot: the app's files beside them end ".wav.tempotantrum".
    if (stem.containsChar ('.'))
        return false;

    if (handled != nullptr)
        *handled = used;

    return true;
}

juce::Array<juce::File> unreadCaptures()
{
    juce::Array<juce::File> found;

    for (const auto& entry : juce::RangedDirectoryIterator (capturesFolder(), false, juce::String (filePrefix) + "*.json", juce::File::findFiles))
    {
        bool handled = false;

        if (isCaptureDescription (entry.getFile().getFileName(), &handled) && ! handled)
            found.add (entry.getFile());
    }

    std::sort (found.begin(), found.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getLastModificationTime() < b.getLastModificationTime();
    });

    return found;
}

void markHandled (const juce::File& json)
{
    bool handled = false;

    if (json.existsAsFile() && isCaptureDescription (json.getFileName(), &handled) && ! handled)
        json.moveFileTo (json.getSiblingFile (json.getFileNameWithoutExtension() + handledEnding));
}

void removeOldCaptures (int days)
{
    const auto cutOff = juce::Time::getCurrentTime() - juce::RelativeTime::days (days);

    for (const auto& entry : juce::RangedDirectoryIterator (capturesFolder(), false, juce::String (filePrefix) + "*" + handledEnding, juce::File::findFiles))
    {
        const auto json = entry.getFile();

        if (! isCaptureDescription (json.getFileName()) || json.getLastModificationTime() >= cutOff)
            continue;

        const auto base = json.getFileName().dropLastCharacters ((int) std::strlen (handledEnding));

        // The capture and anything the app made from it start with the same name, followed by a
        // dot or " selection" (not " 2", which is another capture of the same second).
        for (const auto& pattern : { base + ".*", base + " selection*" })
            for (const auto& sibling : juce::RangedDirectoryIterator (capturesFolder(), false, pattern, juce::File::findFiles))
                sibling.getFile().deleteFile();
    }
}

void postCaptureNotification (const juce::File& json)
{
   #if JUCE_MAC
    auto center = CFNotificationCenterGetDistributedCenter();

    if (center == nullptr)
        return;

    auto name = CFStringCreateWithCString (kCFAllocatorDefault, notificationName, kCFStringEncodingUTF8);
    auto path = CFStringCreateWithCString (kCFAllocatorDefault, json.getFullPathName().toRawUTF8(), kCFStringEncodingUTF8);

    if (name != nullptr && path != nullptr)
        CFNotificationCenterPostNotification (center, name, path, nullptr, true);

    if (path != nullptr) CFRelease (path);
    if (name != nullptr) CFRelease (name);
   #else
    juce::ignoreUnused (json);
   #endif
}

//==============================================================================
Placement placeCapture (const CaptureInfo& capture, const EditSelection& selection, juce::int64 tolerance)
{
    Placement p;

    const auto total = capture.lengthSamples;
    const auto handle = capture.handleSamples;
    const bool canTrim = handle > 0 && total > 2 * handle;
    const auto withoutHandles = canTrim ? total - 2 * handle : total;

    // Without a match, the capture less its handles, from the start of the selection's audio.
    p.trimStart = canTrim ? handle : 0;
    p.length = withoutHandles;

    if (! selection.known || selection.out <= selection.in)
    {
        p.description = "Where this audio starts on the Pro Tools timeline could not be read"
                        + (selection.problem.isNotEmpty() ? " (" + selection.problem + ")" : juce::String())
                        + ". Bars and beats are counted from its start.";
        return p;
    }

    const auto selected = selection.out - selection.in;
    const auto rate = capture.sampleRate;

    auto matched = [&] (juce::int64 trimStart)
    {
        p.startKnown = true;
        p.startSample = selection.in;
        p.trimStart = trimStart;
        p.length = juce::jmin (selected, total - trimStart);
        p.description = "From Pro Tools: the edit selection, " + secondsText (p.length, rate)
                        + " from sample " + juce::String (selection.in) + ".";
        return p;
    };

    if (canTrim && std::abs (withoutHandles - selected) <= tolerance)
        return matched (handle);

    if (std::abs (total - selected) <= tolerance)
        return matched (0);

    p.description = "This audio (" + secondsText (withoutHandles, rate) + ") is not as long as the edit selection in Pro Tools ("
                    + secondsText (selected, rate) + "), so where it starts on the timeline is not known";

    if (canTrim && capture.handleAssumed)
        p.description << ". The AudioSuite handles were taken to be " << juce::String ((double) capture.handleSamples / rate, 2)
                      << " s; if the handle length in the AudioSuite window is different, set it to that and render again";

    p.description << ".";
    return p;
}

//==============================================================================
bool writeSelection (const juce::File& source, juce::int64 trimStart, juce::int64 length, const juce::File& destination,
                     juce::String& problem)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (source.createInputStream().release(), true));

    if (reader == nullptr)
    {
        problem = "Could not read " + source.getFileName();
        return false;
    }

    if (trimStart < 0 || length <= 0 || trimStart + length > reader->lengthInSamples)
    {
        problem = "The selection is not inside " + source.getFileName();
        return false;
    }

    const auto temporary = destination.withFileExtension (".wav.part");
    temporary.deleteFile();

    {
        std::unique_ptr<juce::OutputStream> stream (temporary.createOutputStream());

        if (stream == nullptr)
        {
            problem = "Could not write " + temporary.getFullPathName();
            return false;
        }

        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), reader->sampleRate,
                                                                               reader->numChannels, 32, {}, 0));

        if (writer == nullptr)
        {
            problem = "Could not write " + temporary.getFullPathName();
            return false;
        }

        stream.release();   // the writer owns it now

        if (! writer->writeFromAudioReader (*reader, trimStart, length))
        {
            writer.reset();
            temporary.deleteFile();
            problem = "Could not copy the selection out of " + source.getFileName();
            return false;
        }
    }

    if (! temporary.moveFileTo (destination))
    {
        temporary.deleteFile();
        problem = "Could not write " + destination.getFullPathName();
        return false;
    }

    return true;
}

} // namespace tt::audiosuite
