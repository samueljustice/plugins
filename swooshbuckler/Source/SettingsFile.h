#pragma once

#include <JuceHeader.h>

/**
 * The settings file every Swooshbuckler shares: the app and each plug-in instance, in every host, open
 * Swooshbuckler.settings and keep the small options there (the tape's folder and format, Render Variations'
 * choices, Listening, the learned MIDI controls, the keys, the recent sessions ...).
 *
 * Two rules keep it small and keep one copy from undoing another:
 *  - The app's last session (JUCE's "filterState", samples and all, often megabytes) and its audio device
 *    ("audioSetup") are not in it: the app keeps them in a file of its own, appSessionFile().
 *  - A setting is written with store(): what is on disk now, with that one key changed. A copy opened earlier
 *    (another instance, the app) never writes back the values it read when it opened.
 */
namespace settingsfile
{
    inline juce::PropertiesFile::Options options()
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "Swooshbuckler";
        o.folderName = "Sweet Justice";
        o.filenameSuffix = "settings";
        o.osxLibrarySubFolder = "Application Support";
        o.commonToAllUsers = false;
        o.storageFormat = juce::PropertiesFile::storeAsXML;
        o.millisecondsBeforeSaving = -1; // store() writes; nothing is saved behind its back
        return o;
    }

    inline juce::File defaultFile() { return options().getDefaultFile(); }

    /** Where the app keeps its last session and its audio device, beside the shared file. */
    inline juce::File appSessionFile(const juce::File& settingsFile) { return settingsFile.getSiblingFile("Swooshbuckler App Session.settings"); }

    inline juce::PropertiesFile::Options appSessionOptions()
    {
        auto o = options();
        o.millisecondsBeforeSaving = 3000; // JUCE's app holder sets its values and leaves the saving to the file
        return o;
    }

    /** The keys only the app's own session file holds (JUCE's app holder and window write them). */
    inline const juce::StringArray& appSessionKeys()
    {
        static const juce::StringArray keys { "filterState", "audioSetup", "shouldMuteInput", "lastStateFile", "windowX", "windowY" };
        return keys;
    }

    /** A settings file written before the app had a file of its own holds the app's session: it moves to
        appSessionFile() (unless that already has one) and the shared file is small again. Done once; a file
        without them is only read. Message thread. */
    inline void separateAppSession(const juce::File& settingsFile)
    {
        if (! settingsFile.existsAsFile())
            return;
        juce::PropertiesFile shared(settingsFile, options());
        bool found = false;
        for (const auto& key : appSessionKeys())
            found = found || shared.containsKey(key);
        if (! found)
            return;
        juce::PropertiesFile app(appSessionFile(settingsFile), options());
        for (const auto& key : appSessionKeys())
            if (shared.containsKey(key) && ! app.containsKey(key))
                app.setValue(key, shared.getValue(key));
        if (! app.saveIfNeeded())
            return; // keep them where they are rather than lose them
        for (const auto& key : appSessionKeys())
            shared.removeValue(key);
        shared.saveIfNeeded();
    }

    namespace detail
    {
        template <typename Change>
        void write(juce::PropertiesFile& settings, Change&& change)
        {
            // This copy: changed, without leaving it "to be saved" (saving it would write every value it holds).
            const bool pending = settings.needsToBeSaved();
            change(static_cast<juce::PropertySet&>(settings));
            settings.setNeedsToBeSaved(pending);
            const auto file = settings.getFile();
            if (file == juce::File())
                return;
            // The file: as it is now, with this change.
            juce::PropertiesFile onDisk(file, options());
            change(static_cast<juce::PropertySet&>(onDisk));
            onDisk.saveIfNeeded();
        }
    }

    /** Sets one value, in this copy and in the file as it is on disk now. Message thread. */
    inline void store(juce::PropertiesFile& settings, const juce::String& key, const juce::var& value)
    {
        detail::write(settings, [&key, &value](juce::PropertySet& set) { set.setValue(key, value); });
    }

    /** Takes one value away, in this copy and in the file as it is on disk now. Message thread. */
    inline void remove(juce::PropertiesFile& settings, const juce::String& key)
    {
        detail::write(settings, [&key](juce::PropertySet& set) { set.removeValue(key); });
    }
}
