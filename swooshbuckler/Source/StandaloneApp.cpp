// The standalone app. Same as JUCE's default, except the window uses the normal native
// title bar (no JUCE "Options" bar); the audio device settings live in Swooshbuckler's
// own Settings panel instead.

#include <JuceHeader.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "PluginProcessor.h"
#include "SessionAutosave.h"

namespace
{
    class SwooshbucklerApp final : public juce::JUCEApplication
    {
    public:
        SwooshbucklerApp()
        {
            juce::PropertiesFile::Options options;
            options.applicationName = JucePlugin_Name;
            options.filenameSuffix = ".settings";
            options.osxLibrarySubFolder = "Application Support";
            options.folderName = "Sweet Justice";
            m_properties.setStorageParameters(options);
        }

        const juce::String getApplicationName() override { return JucePlugin_Name; }
        const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
        bool moreThanOneInstanceAllowed() override { return false; }
        void anotherInstanceStarted(const juce::String&) override {}

        void initialise(const juce::String&) override
        {
            // One settings object for the app and the rig: two on the same file would each write back the
            // values they read at launch, undoing the other's changes.
            SwooshbucklerAudioProcessor::useSettings(m_properties.getUserSettings());
            auto holder = std::make_unique<juce::StandalonePluginHolder>(m_properties.getUserSettings(), false, juce::String {}, nullptr,
                                                                       juce::Array<juce::StandalonePluginHolder::PluginInOuts> {}, false);
            m_window = std::make_unique<juce::StandaloneFilterWindow>(getApplicationName(), juce::Colour(0xff0a1a22), std::move(holder));
            m_window->setUsingNativeTitleBar(true);

            // The JUCE Options button lives in the (now hidden) JUCE title bar: take it out entirely.
            for (auto* child : m_window->getChildren())
                if (auto* button = dynamic_cast<juce::TextButton*>(child))
                    button->setVisible(false);

            SwooshbucklerAudioProcessor::showAudioSettings = [this]
            {
                if (m_window != nullptr && m_window->pluginHolder != nullptr)
                    m_window->pluginHolder->showAudioSettingsDialog();
            };
            m_window->setVisible(true);

            // The session is saved on a clean quit (below). While the app runs, a copy is kept beside the settings
            // whenever something changed, and a copy left by a run that did not end cleanly is loaded now.
            if (auto* processor = dynamic_cast<SwooshbucklerAudioProcessor*>(m_window->pluginHolder->processor.get()))
            {
                const auto file = m_properties.getUserSettings()->getFile().getSiblingFile("Swooshbuckler.autosave");
                const bool recovered = SessionAutosave::recover(*processor, file);
                m_autosave = std::make_unique<SessionAutosave>(*processor, &processor->getCargo(),
                                                               [processor] { return processor->getMods().changeCount(); }, file);
                if (recovered)
                    m_autosave->markChanged(); // keep the recovered session on disk until the next clean quit
            }
        }

        void shutdown() override
        {
            SwooshbucklerAudioProcessor::showAudioSettings = nullptr;
            // Both ways out (the window's close button and Quit) have put the session in the settings by now.
            const bool saved = m_properties.saveIfNeeded();
            if (m_autosave != nullptr && saved)
                m_autosave->finish(); // the copy is only for runs that don't get this far
            m_autosave = nullptr;
            m_window = nullptr;
            SwooshbucklerAudioProcessor::useSettings(nullptr);
            m_properties.saveIfNeeded();
        }

        void systemRequestedQuit() override
        {
            if (m_window != nullptr && m_window->pluginHolder != nullptr)
                m_window->pluginHolder->savePluginState();
            quit();
        }

    private:
        juce::ApplicationProperties m_properties;
        std::unique_ptr<juce::StandaloneFilterWindow> m_window;
        std::unique_ptr<SessionAutosave> m_autosave;
    };
}

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new SwooshbucklerApp(); }
