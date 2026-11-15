// The standalone app. Same as JUCE's default, except the window uses the normal native
// title bar (no JUCE "Options" bar); the audio device settings live in Swooshbuckler's
// own Settings panel instead. JUCE's player plays AppOutput, not the rig itself, so the
// rig plays the room the Listening setting chooses, whatever outputs the device has
// (AppOutput.h).

#include <JuceHeader.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "AppOutput.h"
#include "PluginProcessor.h"
#include "SessionAutosave.h"
#include "SettingsFile.h"

namespace
{
    class SwooshbucklerApp final : public juce::JUCEApplication
    {
    public:
        const juce::String getApplicationName() override { return JucePlugin_Name; }
        const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
        bool moreThanOneInstanceAllowed() override { return false; }
        void anotherInstanceStarted(const juce::String&) override {}

        void initialise(const juce::String&) override
        {
            // The small options: the file every Swooshbuckler shares, one object for the app and its rig. The
            // app's last session (samples and all) and its audio device: a file of the app's own, so a plug-in
            // writing an option never rewrites megabytes or brings back an older session (SettingsFile.h).
            const auto settingsFile = settingsfile::defaultFile();
            settingsfile::separateAppSession(settingsFile);
            m_settings = std::make_unique<juce::PropertiesFile>(settingsFile, settingsfile::options());
            m_session = std::make_unique<juce::PropertiesFile>(settingsfile::appSessionFile(settingsFile), settingsfile::appSessionOptions());
            SwooshbucklerAudioProcessor::useSettings(m_settings.get());
            // The last argument: every MIDI input is opened (and new ones as they are plugged in), so a keyboard
            // plays the Keys modes without a visit to the audio settings.
            auto holder = std::make_unique<juce::StandalonePluginHolder>(m_session.get(), false, juce::String {}, nullptr,
                                                                       juce::Array<juce::StandalonePluginHolder::PluginInOuts> {}, true);
            m_window = std::make_unique<juce::StandaloneFilterWindow>(getApplicationName(), juce::Colour(0xff0a1a22), std::move(holder));
            m_window->setUsingNativeTitleBar(true);

            // The JUCE Options button lives in the (now hidden) JUCE title bar: take it out entirely.
            for (auto* child : m_window->getChildren())
                if (auto* button = dynamic_cast<juce::TextButton*>(child))
                    button->setVisible(false);

            if (auto* rig = dynamic_cast<SwooshbucklerAudioProcessor*>(m_window->pluginHolder->processor.get()))
            {
                m_audio = std::make_unique<AppAudio>(*rig, m_window->pluginHolder->deviceManager, m_window->pluginHolder->player);
                m_audio->start();
            }
            SwooshbucklerAudioProcessor::showAudioSettings = [this]
            {
                if (m_audio != nullptr)
                    m_audio->showDialog();
            };
            m_window->setVisible(true);

            // The session is saved on a clean quit (below). While the app runs, a copy is kept beside the settings
            // whenever something changed, and a copy left by a run that did not end cleanly is loaded now.
            if (auto* processor = dynamic_cast<SwooshbucklerAudioProcessor*>(m_window->pluginHolder->processor.get()))
            {
                const auto file = settingsFile.getSiblingFile("Swooshbuckler.autosave");
                const bool recovered = SessionAutosave::recover(*processor, file);
                m_autosave = std::make_unique<SessionAutosave>(*processor, &processor->getCargo(),
                                                               [processor] { return processor->getMods().changeCount(); }, file,
                                                               SessionAutosave::Timing {},
                                                               [processor] (int index) { return processor->isUserEdit(index); });
                if (recovered)
                    m_autosave->markChanged(); // keep the recovered session on disk until the next clean quit

                // The window's title says which session file this is (Save Session As / Open Session set it).
                m_title.update = [this, processor]
                {
                    const auto session = processor->getSessionFile();
                    const auto title = session != juce::File() ? getApplicationName() + " - " + session.getFileNameWithoutExtension() : getApplicationName();
                    if (m_window != nullptr && m_window->getName() != title)
                        m_window->setName(title);
                };
                m_title.update();
                m_title.startTimer(500);
            }
        }

        void shutdown() override
        {
            SwooshbucklerAudioProcessor::showAudioSettings = nullptr;
            m_title.stopTimer();
            m_title.update = nullptr;
            // Both ways out (the window's close button and Quit) have put the session in its file by now.
            const bool saved = m_session != nullptr && m_session->saveIfNeeded();
            if (m_autosave != nullptr && saved)
                m_autosave->finish(); // the copy is only for runs that don't get this far
            m_autosave = nullptr;
            m_audio = nullptr; // the player lets go of AppOutput before the rig goes
            m_window = nullptr;
            SwooshbucklerAudioProcessor::useSettings(nullptr);
            if (m_session != nullptr)
                m_session->saveIfNeeded(); // the window's place
            m_session = nullptr;
            m_settings = nullptr;
        }

        void systemRequestedQuit() override
        {
            if (m_window != nullptr && m_window->pluginHolder != nullptr)
                m_window->pluginHolder->savePluginState();
            quit();
        }

    private:
        std::unique_ptr<juce::PropertiesFile> m_settings; // the shared options (SettingsFile.h)
        std::unique_ptr<juce::PropertiesFile> m_session;  // the app's last session, audio device and window
        std::unique_ptr<juce::StandaloneFilterWindow> m_window;
        std::unique_ptr<AppAudio> m_audio;
        std::unique_ptr<SessionAutosave> m_autosave;
        struct TitleTimer : juce::Timer
        {
            std::function<void()> update;
            void timerCallback() override { if (update) update(); }
        } m_title;
    };
}

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new SwooshbucklerApp(); }
