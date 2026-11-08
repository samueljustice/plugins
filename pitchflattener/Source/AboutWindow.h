#pragma once

#include <JuceHeader.h>
#include "PitchFlattenerLookAndFeel.h"
#include "Theme.h"

#include <functional>

/**
 * The About window. Its owner (the editor) creates at most one, and deletes it when the
 * user closes it (onClose) or when the editor itself goes, so it never outlives the plugin.
 * It is drawn with the owner's LookAndFeel, which outlives it.
 */
class AboutWindow : public juce::DocumentWindow
{
public:
    /** onClose is called when the user closes the window; it should delete the window.
        editorLookAndFeel (the editor's) must outlive the window. */
    AboutWindow (PitchFlattenerLookAndFeel& editorLookAndFeel, std::function<void()> onClose);
    ~AboutWindow() override;

    void closeButtonPressed() override;

    /**
     * Looks up the latest Pitch Flattener release on a worker thread.
     *
     * The thread never touches the window: it hands its result to the message thread, where
     * onResult is called only while this object exists. Deleting it (or cancel()) aborts the
     * request in flight and waits for the thread, so no thread outlives the window or the plugin.
     */
    class UpdateCheck final : private juce::Thread, private juce::AsyncUpdater
    {
    public:
        struct Result
        {
            juce::String text;
            juce::Colour colour;
        };

        UpdateCheck (juce::URL releasesUrl, juce::String currentVersion, std::function<void (const Result&)> onResult);
        ~UpdateCheck() override;

        /** Starts a check; false (and nothing happens) if one is already running. */
        bool start();
        /** Aborts a running check and waits for its thread; its result is never delivered. */
        void cancel();
        bool isChecking() const { return isThreadRunning(); }

        /** The message for a releases response (GitHub's JSON list). */
        static Result interpret (const juce::String& response, const juce::String& currentVersion);

    private:
        void run() override;
        void handleAsyncUpdate() override;
        void abortRequest();

        const juce::URL m_url;
        const juce::String m_currentVersion;
        const std::function<void (const Result&)> m_onResult;

        juce::CriticalSection m_lock;
        juce::WebInputStream* m_request = nullptr; // the request in flight, so cancel() can abort it
        Result m_result;                           // handed from the worker to the message thread

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateCheck)
    };

private:
    class AboutContent : public juce::Component
    {
    public:
        explicit AboutContent (PitchFlattenerLookAndFeel& editorLookAndFeel);
        ~AboutContent() override;

        void paint (juce::Graphics& g) override;
        void resized() override;

        void stopUpdateCheck() { m_updateCheck.cancel(); }

    private:
        void checkForUpdates();

        PitchFlattenerLookAndFeel& m_lookAndFeel;
        juce::SharedResourcePointer<Theme::Fonts> m_fonts;
        juce::Rectangle<int> m_creditsPanel, m_updatesPanel, m_licencePanel; // panels like the editor's sections

        juce::TextButton m_websiteButton, m_emailButton, m_checkUpdatesButton;
        juce::Label m_updateStatusLabel;
        juce::TextEditor m_licenceText;

        UpdateCheck m_updateCheck; // last: stopped before the label it reports to is deleted

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutContent)
    };

    const std::function<void()> m_onClose;
    AboutContent m_content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutWindow)
};
