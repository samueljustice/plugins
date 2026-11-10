#include "SammyJUpdateCheck.h"

#include <juce_data_structures/juce_data_structures.h>

#include <mutex>
#include <set>

namespace sammyj
{
namespace
{
    const char* const releasesUrl = "https://api.github.com/repos/samueljustice/plugins/releases?per_page=100";
    constexpr juce::int64 checkIntervalMs = 24 * 60 * 60 * 1000; // once a day per plug-in
    constexpr int timeoutMs = 5000;

    // Shared by every Sammy J plug-in in every host process; the lock keeps two hosts from writing at once.
    juce::InterProcessLock& settingsLock()
    {
        static juce::InterProcessLock lock("SammyJUpdates");
        return lock;
    }

    std::unique_ptr<juce::PropertiesFile> openSettings()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "Updates";
        options.folderName = "Sammy J";
        options.filenameSuffix = "settings";
        options.osxLibrarySubFolder = "Application Support";
        options.commonToAllUsers = false;
        options.millisecondsBeforeSaving = -1; // save explicitly
        options.processLock = &settingsLock();
        return std::make_unique<juce::PropertiesFile>(options);
    }

    // Versions already offered in this process ("subbertone 1.4.0"), so ten open instances show one popup.
    std::mutex offeredMutex;
    std::set<juce::String> offered;

    bool markOffered(const juce::String& key)
    {
        const std::lock_guard<std::mutex> lock(offeredMutex);
        return offered.insert(key).second;
    }

    int versionPart(const juce::StringArray& parts, int i)
    {
        return i < parts.size() ? juce::jmax(0, parts[i].getIntValue()) : 0;
    }
}

void UpdateInfo::openDownloadPage() const
{
    if (downloadUrl.startsWith("https://github.com/"))
        juce::URL(downloadUrl).launchInDefaultBrowser();
}

bool UpdateCheck::isNewer(const juce::String& candidate, const juce::String& current)
{
    const auto a = juce::StringArray::fromTokens(candidate.trim(), ".", "");
    const auto b = juce::StringArray::fromTokens(current.trim(), ".", "");

    for (int i = 0; i < 3; ++i)
        if (versionPart(a, i) != versionPart(b, i))
            return versionPart(a, i) > versionPart(b, i);

    return false;
}

UpdateCheck::UpdateCheck(juce::String pluginId, juce::String tagPrefix, juce::String currentVersion,
                         std::function<void(const UpdateInfo&)> onUpdateAvailable)
    : juce::Thread("Sammy J update check")
    , m_pluginId(std::move(pluginId))
    , m_tagPrefix(std::move(tagPrefix))
    , m_currentVersion(std::move(currentVersion))
    , m_onUpdateAvailable(std::move(onUpdateAvailable))
{
    startThread(juce::Thread::Priority::low);
}

UpdateCheck::~UpdateCheck()
{
    m_alive->store(false);

    // A cancel can land before the request exists and be lost, so keep cancelling until the thread
    // has gone (normally at once). The force-kill in stopThread() is the last resort.
    for (int i = 0; i < 200 && isThreadRunning(); ++i)
    {
        cancelRequest();
        waitForThreadToExit(10);
    }
    stopThread(1000);
}

void UpdateCheck::skipVersion(const UpdateInfo& info)
{
    if (auto settings = openSettings())
    {
        settings->setValue(info.pluginId + ".skipped", info.latestVersion);
        settings->saveIfNeeded();
    }
}

void UpdateCheck::cancelRequest()
{
    signalThreadShouldExit();

    // WebInputStream::cancel() unblocks connect() and read() from another thread.
    const juce::ScopedLock sl(m_streamLock);
    if (m_stream != nullptr)
        m_stream->cancel();
}

bool UpdateCheck::fetchLatest(juce::String& version, juce::String& url)
{
    juce::String body;
    int status = 0;

    {
        juce::WebInputStream stream(juce::URL(releasesUrl), false);
        stream.withConnectionTimeout(timeoutMs)
              .withExtraHeaders("User-Agent: SammyJ-Plugins-UpdateCheck\r\nAccept: application/vnd.github+json");

        {
            const juce::ScopedLock sl(m_streamLock);
            if (threadShouldExit())
                return false;
            m_stream = &stream;
        }

        // No listener: a listener that aborts connect() makes JUCE (macOS) destroy the connection on
        // this thread while cancelRequest() may be using it.
        if (stream.connect(nullptr))
        {
            status = stream.getStatusCode();
            if (status == 200)
                body = stream.readEntireStreamAsString();
        }

        const juce::ScopedLock sl(m_streamLock);
        m_stream = nullptr;
    }

    if (status != 200 || threadShouldExit())
        return false;

    // Keep the parsed tree in a named local: getArray() points into it.
    const auto releases = juce::JSON::parse(body);
    const auto* list = releases.getArray();
    if (list == nullptr)
        return false;

    juce::String best;
    juce::String bestUrl;

    for (const auto& release : *list)
    {
        const auto tag = release.getProperty("tag_name", {}).toString();
        if (! tag.startsWith(m_tagPrefix)
            || static_cast<bool>(release.getProperty("prerelease", false))
            || static_cast<bool>(release.getProperty("draft", false)))
            continue;

        const auto candidate = tag.substring(m_tagPrefix.length());
        if (! candidate.containsOnly("0123456789."))
            continue;

        if (best.isEmpty() || isNewer(candidate, best))
        {
            best = candidate;
            bestUrl = release.getProperty("html_url", {}).toString();
        }
    }

    if (best.isEmpty() || ! bestUrl.startsWith("https://github.com/"))
        return false;

    version = best;
    url = bestUrl;
    return true;
}

void UpdateCheck::run()
{
    juce::String latest, url, skipped;

    {
        auto settings = openSettings();
        const auto now = juce::Time::currentTimeMillis();
        const auto lastCheck = settings->getValue(m_pluginId + ".lastCheck").getLargeIntValue();
        skipped = settings->getValue(m_pluginId + ".skipped");

        if (lastCheck > 0 && now - lastCheck >= 0 && now - lastCheck < checkIntervalMs)
        {
            // Checked recently: use the cached answer, no network.
            latest = settings->getValue(m_pluginId + ".latest");
            url = settings->getValue(m_pluginId + ".url");
        }
        else
        {
            if (! fetchLatest(latest, url))
                return; // offline, blocked, rate-limited or cancelled: say nothing, try again next time

            settings->setValue(m_pluginId + ".lastCheck", juce::String(now));
            settings->setValue(m_pluginId + ".latest", latest);
            settings->setValue(m_pluginId + ".url", url);
            settings->saveIfNeeded();
        }
    }

    if (threadShouldExit() || latest.isEmpty() || ! url.startsWith("https://github.com/"))
        return;
    if (! isNewer(latest, m_currentVersion) || latest == skipped)
        return;
    if (! markOffered(m_pluginId + " " + latest))
        return;

    UpdateInfo info { m_pluginId, m_currentVersion, latest, url };
    auto alive = m_alive;
    auto callback = m_onUpdateAvailable;

    juce::MessageManager::callAsync([alive, callback, info]
    {
        if (alive->load() && callback != nullptr)
            callback(info);
    });
}

} // namespace sammyj
