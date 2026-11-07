// The standalone app: just the page in a native window. It opens no audio device (there is
// nothing to hear), so there is no JUCE Options bar or feedback-mute banner. The category, the
// entry on screen and the theme are kept between launches.

#include <JuceHeader.h>
#include "PluginProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
    class PageWindow final : public juce::DocumentWindow
    {
    public:
        PageWindow (juce::AudioProcessor& processor, std::function<void()> onClose)
            : DocumentWindow (JucePlugin_Name, juce::Colour (0xfff4efe5), DocumentWindow::closeButton | DocumentWindow::minimiseButton),
              closed (std::move (onClose))
        {
            setUsingNativeTitleBar (true);
            setContentOwned (processor.createEditor(), true);
            setResizable (true, false);
            if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (getContentComponent()))
                if (auto* constrainer = editor->getConstrainer())
                    setResizeLimits (constrainer->getMinimumWidth(), constrainer->getMinimumHeight(),
                                     constrainer->getMaximumWidth(), constrainer->getMaximumHeight());
            centreWithSize (getWidth(), getHeight());
        }

        void closeButtonPressed() override { closed(); }

    private:
        std::function<void()> closed;
    };

    class SoundAdviceApp final : public juce::JUCEApplication
    {
    public:
        SoundAdviceApp()
        {
            juce::PropertiesFile::Options options;
            options.applicationName = JucePlugin_Name;
            options.filenameSuffix = ".settings";
            options.osxLibrarySubFolder = "Application Support";
            options.folderName = "Sweet Justice";
            properties.setStorageParameters (options);
        }

        const juce::String getApplicationName() override { return JucePlugin_Name; }
        const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
        bool moreThanOneInstanceAllowed() override { return false; }
        void anotherInstanceStarted (const juce::String&) override {}

        void initialise (const juce::String&) override
        {
            processor.reset (createPluginFilter());

            if (auto* settings = properties.getUserSettings())
            {
                juce::MemoryBlock state;
                if (state.fromBase64Encoding (settings->getValue ("state")))
                    processor->setStateInformation (state.getData(), (int) state.getSize());
                window = std::make_unique<PageWindow> (*processor, [this] { systemRequestedQuit(); });
                if (const auto pos = settings->getValue ("window"); pos.isNotEmpty())
                    window->restoreWindowStateFromString (pos);
            }
            else
            {
                window = std::make_unique<PageWindow> (*processor, [this] { systemRequestedQuit(); });
            }
            window->setVisible (true);
        }

        void shutdown() override
        {
            save();
            window = nullptr;
            processor = nullptr;
        }

        void systemRequestedQuit() override
        {
            save();
            quit();
        }

    private:
        void save()
        {
            auto* settings = properties.getUserSettings();
            if (settings == nullptr || processor == nullptr)
                return;
            juce::MemoryBlock state;
            processor->getStateInformation (state);
            settings->setValue ("state", state.toBase64Encoding());
            if (window != nullptr)
                settings->setValue ("window", window->getWindowStateAsString());
            properties.saveIfNeeded();
        }

        juce::ApplicationProperties properties;
        std::unique_ptr<juce::AudioProcessor> processor;
        std::unique_ptr<PageWindow> window;
    };
}

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new SoundAdviceApp(); }
