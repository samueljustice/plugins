// The standalone app. Same as JUCE's default, except the window uses the normal native
// title bar (no JUCE "Options" bar); the audio device settings live in Swooshbuckler's
// own Settings panel instead.

#include <JuceHeader.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "PluginProcessor.h"

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
        }

        void shutdown() override
        {
            SwooshbucklerAudioProcessor::showAudioSettings = nullptr;
            m_window = nullptr;
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
    };
}

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new SwooshbucklerApp(); }
