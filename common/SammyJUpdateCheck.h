#pragma once

// Shared by every Sammy J plug-in: checks GitHub for a newer release of this plug-in and, if there is
// one the user hasn't skipped, tells the editor (which shows its own popup in its own style).
//
// - Runs on a background thread when the editor creates it; never touches the audio thread.
// - Fetches the release list at most once a day per plug-in, and caches the answer in
//   ~/Library/Application Support/Sammy J/Updates.settings (macOS) or %APPDATA%\Sammy J\Updates.settings.
// - Any failure (offline, timeout, firewall, rate limit, bad reply) ends silently: no popup, no message.
// - At most one popup per new version per host session, however many instances are open.
//
// Usage, in the editor:
//   m_updateCheck = std::make_unique<sammyj::UpdateCheck>("subbertone", "subbertone-v", JucePlugin_VersionString,
//       [this](const sammyj::UpdateInfo& info) { showUpdatePopup(info); });
// and from the popup's buttons: info.openDownloadPage(), sammyj::UpdateCheck::skipVersion(info).
// Destroying the UpdateCheck (with the editor) cancels the request; the callback never runs after that.

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>

namespace sammyj
{

struct UpdateInfo
{
    juce::String pluginId;        // "subbertone"
    juce::String currentVersion;  // "1.3.1"
    juce::String latestVersion;   // "1.4.0"
    juce::String downloadUrl;     // the release's GitHub page, with its downloads

    // Opens the download page in the default browser.
    void openDownloadPage() const;
};

class UpdateCheck : private juce::Thread
{
public:
    // pluginId names the plug-in in the settings file; tagPrefix is its release tag prefix ("subbertone-v").
    // onUpdateAvailable runs on the message thread, at most once, only if a newer release is found.
    UpdateCheck(juce::String pluginId, juce::String tagPrefix, juce::String currentVersion,
                std::function<void(const UpdateInfo&)> onUpdateAvailable);
    ~UpdateCheck() override;

    // "Skip this version": never offer this version again (a newer one is still offered).
    static void skipVersion(const UpdateInfo& info);

    // "1.10.0" > "1.9.2"; anything that isn't numbers counts as 0.
    static bool isNewer(const juce::String& candidate, const juce::String& current);

private:
    void run() override;
    void cancelRequest();
    bool fetchLatest(juce::String& version, juce::String& url);

    const juce::String m_pluginId, m_tagPrefix, m_currentVersion;
    std::function<void(const UpdateInfo&)> m_onUpdateAvailable;

    // Checked on the message thread by the posted callback, cleared by the destructor (message thread).
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);

    juce::CriticalSection m_streamLock;
    juce::WebInputStream* m_stream = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UpdateCheck)
};

} // namespace sammyj
