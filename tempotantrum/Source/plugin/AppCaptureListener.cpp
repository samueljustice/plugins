// CoreFoundation first: after JuceHeader.h's "using namespace juce", its Point would be ambiguous.
#if defined (__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include "AppCaptureListener.h"

#include "capture/CaptureInbox.h"
#include "protools/PtslSession.h"
#include "ptsl/PtslClient.h"

#include <algorithm>


namespace tt::audiosuite
{

namespace
{
    // Captures that come in within this long of each other are taken as passes of one Render.
    constexpr double sameRenderWithinMs = 1500.0;

    // A capture from a Render started by hand is placed against the edit selection only when its
    // description was written this recently.
    constexpr juce::int64 freshForMs = 10000;

    double nowMs()
    {
        return juce::Time::getMillisecondCounterHiRes();
    }

    // What "Get from Pro Tools" shares with the listener while it renders.
    std::mutex sharedLock;
    AppCaptureListener* listener = nullptr;
    bool holding = false;
    std::vector<juce::File> held;
    double ignoreUntilMs = 0.0;

   #if JUCE_MAC
    void notificationArrived (CFNotificationCenterRef, void* observer, CFNotificationName, const void* object, CFDictionaryRef)
    {
        if (observer == nullptr || object == nullptr || CFGetTypeID (object) != CFStringGetTypeID())
            return;

        static_cast<AppCaptureListener*> (observer)->take (juce::String::fromCFString (static_cast<CFStringRef> (object)));
    }
   #endif

    EditSelection readEditSelection (ptsl::Client& client, std::string& sessionName)
    {
        EditSelection selection;
        std::string problem;

        if (! pt::openLink (client, problem))
        {
            selection.problem = juce::String (problem);
            return selection;
        }

        auto read = client.editSelection ("TLType_Samples");

        // Pro Tools before 2025.06 gives only the timeline selection.
        if (! read.ok())
            read = client.timelineSelection ("TLType_Samples");

        long long in = 0, out = 0;

        if (! read.ok())
            selection.problem = "Pro Tools did not give its edit selection: " + juce::String (read.reply.message);
        else if (! pt::samplesOf (read.value.in, in) || ! pt::samplesOf (read.value.out, out))
            selection.problem = "Pro Tools gave the edit selection in a form that is not samples";
        else
        {
            selection.known = true;
            selection.in = in;
            selection.out = out;
        }

        if (const auto name = client.sessionName(); name.ok())
            sessionName = name.value;

        return selection;
    }
}

//==============================================================================
AppCaptureListener::AppCaptureListener()
    : juce::Thread ("Tempo Tantrum AudioSuite captures")
{
    {
        const std::lock_guard<std::mutex> l (sharedLock);
        listener = this;
    }

   #if JUCE_MAC
    if (auto center = CFNotificationCenterGetDistributedCenter())
    {
        auto name = CFStringCreateWithCString (kCFAllocatorDefault, notificationName, kCFStringEncodingUTF8);
        CFNotificationCenterAddObserver (center, this, notificationArrived, name, nullptr,
                                         CFNotificationSuspensionBehaviorDeliverImmediately);
        CFRelease (name);
    }
   #endif

    // Captures rendered while the app was closed: the newest is opened (its start not known), the
    // others are passed over.
    const auto unread = unreadCaptures();

    for (int i = 0; i < unread.size() - 1; ++i)
        markHandled (unread[i]);

    if (! unread.isEmpty())
        add ({ unread.getLast(), Kind::atStart, {}, {} });

    removeOldCaptures (7);
    startThread();
}

AppCaptureListener::~AppCaptureListener()
{
   #if JUCE_MAC
    if (auto center = CFNotificationCenterGetDistributedCenter())
        CFNotificationCenterRemoveEveryObserver (center, this);
   #endif

    {
        const std::lock_guard<std::mutex> l (sharedLock);
        listener = nullptr;
    }

    signalThreadShouldExit();
    notify();
    stopThread (5000);
}

bool AppCaptureListener::take (const juce::String& jsonPath)
{
    if (! juce::File::isAbsolutePath (jsonPath))
        return false;

    const juce::File json (jsonPath);
    bool handled = false;

    if (json.getParentDirectory() != capturesFolder() || ! isCaptureDescription (json.getFileName(), &handled) || handled
        || ! json.existsAsFile())
        return false;

    {
        const std::lock_guard<std::mutex> l (sharedLock);

        if (holding)
        {
            if (std::find (held.begin(), held.end(), json) == held.end())
                held.push_back (json);

            return true;
        }

        if (nowMs() < ignoreUntilMs)
        {
            markHandled (json);
            return true;
        }
    }

    add ({ json, Kind::live, {}, {} });
    return true;
}

void AppCaptureListener::add (Job job)
{
    {
        const std::lock_guard<std::mutex> l (queueLock);

        if (job.kind == Kind::live)
            lastLiveMs = nowMs();

        const auto same = std::find_if (queue.begin(), queue.end(), [&] (const Job& j) { return j.json == job.json; });

        if (same == queue.end())
            queue.push_back (std::move (job));
        else if (job.kind == Kind::placed)
            *same = std::move (job);
    }

    notify();
}

//==============================================================================
void AppCaptureListener::startHolding()
{
    const std::lock_guard<std::mutex> l (sharedLock);
    holding = true;
    held.clear();
    ignoreUntilMs = 0.0;
}

std::vector<juce::File> AppCaptureListener::heldCaptures()
{
    const std::lock_guard<std::mutex> l (sharedLock);
    return held;
}

void AppCaptureListener::stopHolding (const juce::File& open, const EditSelection& selection, const juce::String& sessionName,
                                      int ignoreLaterForMs)
{
    const std::lock_guard<std::mutex> l (sharedLock);

    if (! holding)
        return;

    holding = false;
    ignoreUntilMs = ignoreLaterForMs > 0 ? nowMs() + ignoreLaterForMs : 0.0;

    for (const auto& json : held)
        if (json != open)
            markHandled (json);

    held.clear();

    if (open != juce::File() && listener != nullptr)
        listener->add ({ open, Kind::placed, selection, sessionName });
}

//==============================================================================
void AppCaptureListener::run()
{
    while (! threadShouldExit())
    {
        std::vector<Job> jobs;
        bool waitingForMore = false;

        {
            const std::lock_guard<std::mutex> l (queueLock);

            if (! queue.empty())
            {
                if (queue.front().kind != Kind::live)
                {
                    jobs.push_back (queue.front());
                    queue.pop_front();
                }
                else if (nowMs() - lastLiveMs < sameRenderWithinMs)
                {
                    // More passes of the same Render may follow, and Pro Tools is busy rendering them.
                    waitingForMore = true;
                }
                else
                {
                    while (! queue.empty() && queue.front().kind == Kind::live)
                    {
                        jobs.push_back (queue.front());
                        queue.pop_front();
                    }
                }
            }
        }

        if (jobs.empty())
        {
            wait (waitingForMore ? 100 : -1);
            continue;
        }

        handle (jobs.front(), (int) jobs.size());

        for (size_t i = 1; i < jobs.size(); ++i)
            markHandled (jobs[i].json);
    }
}

void AppCaptureListener::handle (const Job& job, int capturesInRender)
{
    const auto& json = job.json;
    CaptureInfo info;
    juce::String problem;

    if (! readCaptureInfo (json, info, problem))
    {
        DBG (problem);
        markHandled (json);
        return;
    }

    EditSelection selection;
    std::string sessionName;

    if (job.kind == Kind::placed)
    {
        selection = job.selection;
        sessionName = job.sessionName.toStdString();
    }
    else if (job.kind == Kind::atStart)
    {
        selection.problem = "it was rendered while Tempo Tantrum was closed";
    }
    else if (capturesInRender > 1)
    {
        selection.problem = "Pro Tools rendered " + juce::String (capturesInRender) + " separate files in one go";
    }
    else if (juce::Time::getCurrentTime() - json.getLastModificationTime() > juce::RelativeTime::milliseconds (freshForMs))
    {
        selection.problem = "it was rendered too long before it arrived";
    }
    else
    {
        ptsl::Client client;
        client.setStopCheck ([this] { return threadShouldExit(); });
        selection = readEditSelection (client, sessionName);
        client.disconnect();

        if (threadShouldExit())
            return;
    }

    const auto place = placeCapture (info, selection);

    // The selection alone, without the handles.
    auto audio = info.wav;

    if (place.trimStart != 0 || place.length != info.lengthSamples)
    {
        audio = info.wav.getSiblingFile (info.wav.getFileNameWithoutExtension() + " selection.wav");

        if (! writeSelection (info.wav, place.trimStart, place.length, audio, problem))
        {
            DBG (problem);
            markHandled (json);
            return;
        }
    }

    capture::Capture c;
    c.audio = audio;
    c.startKnown = place.startKnown;
    c.startSample = place.startKnown ? place.startSample : 0;
    c.endSample = place.startKnown ? place.startSample + place.length : 0;
    c.sessionRate = info.sampleRate;
    c.sessionName = place.startKnown ? juce::String (sessionName) : juce::String();
    c.name = "AudioSuite capture " + info.captured.formatted ("%H:%M:%S");
    c.source = "an AudioSuite Render";
    c.note = place.startKnown ? juce::String() : place.description;

    if (capturesInRender > 1)
        c.note << " Only the first of the " << capturesInRender << " files is open (several tracks, multi-mono or clip by clip make one "
               << "file each). To get the music in one file, select one track and render the whole selection as one file.";

    // The window registers with the inbox when it is made; give it a moment at start.
    for (int tries = 0; ! capture::deliver (c); ++tries)
    {
        if (tries >= 20 || threadShouldExit())
            return;     // left unread: the next start takes it

        wait (250);
    }

    markHandled (json);
}

} // namespace tt::audiosuite
