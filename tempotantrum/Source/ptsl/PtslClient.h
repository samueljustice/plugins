#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// A small client for Pro Tools' scripting link (PTSL): a local, unencrypted gRPC connection to
// Pro Tools on this Mac. A command is sent by name with a JSON body and answered with JSON.
// Plain C++; every call blocks until Pro Tools answers or the time runs out, so it is only ever
// used on a background thread. One client is used by one thread at a time (a lock makes sure).
//
// A build without the Pro Tools Scripting SDK has a stand-in whose every call fails with
// noLinkMessage, so the app and its tests build without the SDK.
//
namespace juce { class var; }

// Pro Tools queues commands while one of its modal dialogs is open, so a call made then waits
// until the dialog closes, and here runs out of time. Callers must not send anything while they
// have a dialog open in Pro Tools.
namespace tt::ptsl
{

inline constexpr const char* noLinkMessage = "This build has no Pro Tools Scripting link";
inline constexpr int defaultTimeoutMs = 3000;

struct Reply
{
    bool ok = false;            // Pro Tools carried the command out
    bool answered = false;      // Pro Tools answered, even if with an error
    bool timedOut = false;      // no answer in time
    bool stopped = false;       // the caller's stop check ended the wait
    std::string body;           // the response JSON
    std::string error;          // the error JSON, when Pro Tools gave one
    std::string message;        // why it failed, one sentence without a full stop
    double seconds = 0.0;       // how long the call took
};

// A value read with a command, with the reply it came in.
template <typename T>
struct Answer
{
    Reply reply;
    T value {};

    bool ok() const             { return reply.ok; }
};

struct Selection
{
    std::string in, out;
};

struct EditModeOptions
{
    // Every option Pro Tools gave, by its JSON key, so the same set can be sent back.
    std::vector<std::pair<std::string, bool>> options;

    bool get (const std::string& key, bool& value) const;
    void set (const std::string& key, bool value);
};

struct MainCounterFormat
{
    std::string type;           // "TLType_Samples", "TLType_BarsBeats", ...
    std::string setting;        // "TOOptions_Samples", ...
};

struct FileLocation
{
    std::string path;
    std::string fileId;
    bool online = false;
};

struct Position
{
    bool known = false;
    long long position = 0;
    std::string type;           // "BTType_Samples", "BTType_Ticks", ...
};

struct Clip
{
    std::string fileId, clipId, fullName, rootName;
    Position start, end, sourceStart, sourceEnd;
};

struct Track
{
    std::string name, id, type;
    int index = 0;
    bool selected = false;      // explicitly or implicitly
    std::string selectionState; // the selection as Pro Tools wrote it ("SetExplicitly", ...)
    bool hasClips = false;
};

class Client
{
public:
    Client();
    ~Client();

    // Whether this build has the link at all.
    static bool hasLink();

    // Opens the connection to Pro Tools. False, with a sentence, when nothing answers.
    bool connect (std::string& message);
    bool isConnected() const;

    // Closes the connection. The session id goes with it.
    void disconnect();

    // Gives Pro Tools the company and application name and keeps the session id it returns for
    // every command after. The names are kept too, to register again after a reconnection.
    Reply registerConnection (const std::string& company, const std::string& application);
    bool isRegistered() const;

    // Asked about every tenth of a second while a call waits for its answer. When it returns
    // true the call ends unanswered (Reply::stopped) and the connection is closed, as an answer
    // may still come. For a reader that must stop quickly; a run that has to put things back
    // sets none.
    void setStopCheck (std::function<bool()> shouldStop);

    // Sends one command by name ("GetSessionName") with a JSON body ("" for none). When the
    // connection has been closed (after a call that ran out of time, say), connects again and,
    // if the client had registered, registers again before the command, as the old session id
    // went with the old connection.
    Reply send (const std::string& command, const std::string& bodyJson, int timeoutMs = defaultTimeoutMs);

    //==============================================================================
    // The commands Tempo Tantrum uses. Times are strings as Pro Tools writes them; a location
    // type is a TLType_ name such as "TLType_Samples".
    Reply hostReadyCheck();
    Answer<std::string> ptslVersion();                       // "2025.6.0"
    Answer<std::string> sessionName();
    Answer<double> sessionSampleRate();
    Answer<std::string> sessionBitDepth();                  // "BDepth_24", "Bit24", ...
    Answer<std::string> sessionStartTime();
    Answer<std::string> sessionLength();
    Answer<std::string> transportState();                    // "TS_TransportStopped", ...
    Answer<MainCounterFormat> mainCounterFormat();
    Answer<Selection> editSelection (const std::string& locationType);
    Answer<Selection> timelineSelection (const std::string& locationType);
    Answer<EditModeOptions> editModeOptions();
    // The lists are read a page at a time until all are in.
    Answer<std::vector<FileLocation>> fileLocations (const std::string& filter);    // "SelectedClipsTimeline", "All_Files", ...
    Answer<std::string> timeAsType (const std::string& location, const std::string& fromType, const std::string& toType);
    Answer<std::vector<Track>> trackList();
    Answer<int> trackCount();
    Answer<std::vector<Clip>> clipList();

    // The session info text with each track's clips (its EDL) in samples, as a string: no file
    // is written. Every track, or only the selected ones.
    Answer<std::string> sessionInfoText (bool selectedTracksOnly);

    // These change Pro Tools.
    Reply togglePlayState();
    Reply setMainCounterFormat (const std::string& locationType);
    Reply setEditModeOptions (const EditModeOptions& options);
    Reply setTimelineSelection (const std::string& in, const std::string& out, const std::string& locationType);

    // Renders one track to files with Pro Tools' Track Bounce (Pro Tools 2025.06 and later). The
    // request is the whole JSON body (capture/CaptureMaths.h writes it); the answer is the path of
    // every file written. It writes files but leaves the session as it was when the request asks
    // for no import. Waits up to `timeoutMs`; the stop check ends the wait.
    Answer<std::vector<std::string>> bounceTrack (const std::string& requestJson, int timeoutMs);

    static bool isStopped (const std::string& transportState);

private:
    struct Connection;
    std::unique_ptr<Connection> connection;
    std::string sessionId;
    std::string registeredCompany, registeredApplication;
    std::function<bool()> stopCheck;
    mutable std::recursive_mutex lock;

    void dropConnection();

    // Sends a list command page by page with `fields` (JSON members, without braces) and calls
    // `each` with every item of the list `listKey`. The reply is the last page's, with the time of all.
    Reply sendPaged (const std::string& command, const std::string& fields, const char* listKey,
                     const std::function<void (const juce::var&)>& each, int timeoutMs = defaultTimeoutMs);
};

} // namespace tt::ptsl
