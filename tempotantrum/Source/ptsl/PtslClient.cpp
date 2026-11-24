#include "PtslClient.h"

#include <JuceHeader.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#if TEMPOTANTRUM_HAS_PTSL
 #include "PtslWire.h"
 #include "PtslGenerated.h"     // generated at configure time from the SDK, in the build folder

 #include <nghttp2/nghttp2.h>

 #include <arpa/inet.h>
 #include <cerrno>
 #include <fcntl.h>
 #include <netinet/in.h>
 #include <netinet/tcp.h>
 #include <poll.h>
 #include <sys/socket.h>
 #include <unistd.h>
#endif

namespace tt::ptsl
{

namespace
{
    using Clock = std::chrono::steady_clock;

    [[maybe_unused]] double secondsSince (Clock::time_point start)
    {
        return std::chrono::duration<double> (Clock::now() - start).count();
    }

    juce::var parse (const std::string& json)
    {
        return json.empty() ? juce::var() : juce::JSON::parse (juce::String::fromUTF8 (json.c_str()));
    }

    std::string str (const juce::var& value)
    {
        return value.isVoid() ? std::string() : value.toString().toStdString();
    }

    long long int64Of (const juce::var& value)
    {
        // Protocol buffers write 64-bit numbers in JSON as strings.
        if (value.isString())
            return value.toString().getLargeIntValue();

        return (long long) (juce::int64) value;
    }

    std::string quoted (const std::string& text)
    {
        return juce::JSON::toString (juce::var (juce::String::fromUTF8 (text.c_str())), true).toStdString();
    }

    // The first error Pro Tools gave in its error JSON, as a sentence.
    [[maybe_unused]] std::string errorSentence (const std::string& errorJson)
    {
        const auto error = parse (errorJson);
        const auto* errors = error.getProperty ("errors", juce::var()).getArray();

        if (errors == nullptr || errors->isEmpty())
            return errorJson.empty() ? std::string() : errorJson;

        const auto& first = errors->getReference (0);
        auto text = first.getProperty ("command_error_message", juce::var()).toString().trim();
        const auto type = first.getProperty ("command_error_type", juce::var()).toString();

        if (text.isEmpty())
            text = type;
        else if (type.isNotEmpty())
            text << " (" << type << ")";

        while (text.endsWithChar ('.'))
            text = text.dropLastCharacters (1);

        return text.toStdString();
    }
}

//==============================================================================
bool EditModeOptions::get (const std::string& key, bool& value) const
{
    for (const auto& [name, on] : options)
        if (name == key)
        {
            value = on;
            return true;
        }

    return false;
}

void EditModeOptions::set (const std::string& key, bool value)
{
    for (auto& [name, on] : options)
        if (name == key)
        {
            on = value;
            return;
        }

    options.emplace_back (key, value);
}

bool Client::isStopped (const std::string& state)
{
    return state == "TS_TransportStopped" || state == "TState_TransportStopped";
}

#if TEMPOTANTRUM_HAS_PTSL
//==============================================================================
// One HTTP/2 connection over a plain TCP socket to Pro Tools on this Mac, with one call at a time.
struct Client::Connection
{
    int fd = -1;
    nghttp2_session* session = nullptr;
    bool broken = false;

    // The call under way.
    int32_t streamId = -1;
    int httpStatus = 0;
    int grpcStatus = -1;
    std::string grpcMessage;
    std::string body;
    bool closed = false;
    uint32_t closeError = 0;

    std::string requestBody;
    std::size_t requestSent = 0;

    ~Connection()
    {
        if (session != nullptr)
            nghttp2_session_del (session);

        if (fd >= 0)
            ::close (fd);
    }

    static nghttp2_ssize onSend (nghttp2_session*, const uint8_t* data, size_t length, int, void* user)
    {
        auto& self = *static_cast<Connection*> (user);
        const auto sent = ::send (self.fd, data, length, 0);

        if (sent < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return NGHTTP2_ERR_WOULDBLOCK;

            self.broken = true;
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }

        return (nghttp2_ssize) sent;
    }

    static int onHeader (nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t nameLength,
                         const uint8_t* value, size_t valueLength, uint8_t, void* user)
    {
        auto& self = *static_cast<Connection*> (user);

        if (frame->hd.type != NGHTTP2_HEADERS || frame->hd.stream_id != self.streamId)
            return 0;

        const std::string_view key ((const char*) name, nameLength), text ((const char*) value, valueLength);

        if (key == ":status")
            self.httpStatus = std::atoi (std::string (text).c_str());
        else if (key == "grpc-status")
            self.grpcStatus = std::atoi (std::string (text).c_str());
        else if (key == "grpc-message")
            self.grpcMessage.assign (text);

        return 0;
    }

    static int onData (nghttp2_session*, uint8_t, int32_t stream, const uint8_t* data, size_t length, void* user)
    {
        auto& self = *static_cast<Connection*> (user);

        if (stream == self.streamId)
            self.body.append ((const char*) data, length);

        return 0;
    }

    static int onStreamClose (nghttp2_session*, int32_t stream, uint32_t errorCode, void* user)
    {
        auto& self = *static_cast<Connection*> (user);

        if (stream == self.streamId)
        {
            self.closed = true;
            self.closeError = errorCode;
        }

        return 0;
    }

    static int onFrame (nghttp2_session*, const nghttp2_frame* frame, void* user)
    {
        // Pro Tools is closing the connection: the next call opens another.
        if (frame->hd.type == NGHTTP2_GOAWAY)
            static_cast<Connection*> (user)->broken = true;

        return 0;
    }

    static nghttp2_ssize readBody (nghttp2_session*, int32_t, uint8_t* buffer, size_t length, uint32_t* flags,
                                   nghttp2_data_source*, void* user)
    {
        auto& self = *static_cast<Connection*> (user);
        const auto count = std::min (length, self.requestBody.size() - self.requestSent);
        std::memcpy (buffer, self.requestBody.data() + self.requestSent, count);
        self.requestSent += count;

        if (self.requestSent >= self.requestBody.size())
            *flags |= NGHTTP2_DATA_FLAG_EOF;

        return (nghttp2_ssize) count;
    }

    // Plain TCP to Pro Tools' port on this Mac, IPv4 then IPv6.
    bool open (std::string& message)
    {
        constexpr int port = 31416;
        constexpr int connectTimeoutMs = 1000;

        for (int family : { AF_INET, AF_INET6 })
        {
            fd = ::socket (family, SOCK_STREAM, 0);

            if (fd < 0)
                continue;

            int on = 1;
            ::setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof (on));
            ::setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof (on));
            ::fcntl (fd, F_SETFL, ::fcntl (fd, F_GETFL, 0) | O_NONBLOCK);

            sockaddr_storage address {};
            socklen_t size = 0;

            if (family == AF_INET)
            {
                auto& v4 = reinterpret_cast<sockaddr_in&> (address);
                v4.sin_family = AF_INET;
                v4.sin_port = htons (port);
                v4.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
                size = sizeof (sockaddr_in);
            }
            else
            {
                auto& v6 = reinterpret_cast<sockaddr_in6&> (address);
                v6.sin6_family = AF_INET6;
                v6.sin6_port = htons (port);
                v6.sin6_addr = in6addr_loopback;
                size = sizeof (sockaddr_in6);
            }

            bool connected = ::connect (fd, reinterpret_cast<sockaddr*> (&address), size) == 0;

            if (! connected && errno == EINPROGRESS)
            {
                pollfd p { fd, POLLOUT, 0 };

                if (::poll (&p, 1, connectTimeoutMs) == 1)
                {
                    int error = 0;
                    socklen_t length = sizeof (error);
                    connected = ::getsockopt (fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
                }
            }

            if (connected)
                return startSession (message);

            ::close (fd);
            fd = -1;
        }

        message = "Pro Tools did not answer on its scripting port. Is Pro Tools running, with a session open?";
        return false;
    }

    bool startSession (std::string& message)
    {
        nghttp2_session_callbacks* callbacks = nullptr;
        nghttp2_session_callbacks_new (&callbacks);
        nghttp2_session_callbacks_set_send_callback2 (callbacks, onSend);
        nghttp2_session_callbacks_set_on_header_callback (callbacks, onHeader);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback (callbacks, onData);
        nghttp2_session_callbacks_set_on_stream_close_callback (callbacks, onStreamClose);
        nghttp2_session_callbacks_set_on_frame_recv_callback (callbacks, onFrame);
        const int created = nghttp2_session_client_new (&session, callbacks, this);
        nghttp2_session_callbacks_del (callbacks);

        if (created != 0)
        {
            message = "the HTTP/2 session could not be made";
            return false;
        }

        // Room for long answers (a long clip list) without waiting for window updates.
        const nghttp2_settings_entry settings[] = {
            { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 16 },
            { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 16 * 1024 * 1024 },
        };

        nghttp2_submit_settings (session, NGHTTP2_FLAG_NONE, settings, std::size (settings));
        nghttp2_session_set_local_window_size (session, NGHTTP2_FLAG_NONE, 0, 64 * 1024 * 1024);
        return true;
    }

    // One unary call. False, with a sentence, when it could not be made or timed out.
    bool call (const std::string& path, const std::string& message, int timeoutMs, const std::function<bool()>& shouldStop,
               wire::CallResult& result, std::string& failure, bool& timedOut, bool& stoppedEarly)
    {
        timedOut = false;
        stoppedEarly = false;
        streamId = -1;
        httpStatus = 0;
        grpcStatus = -1;
        grpcMessage.clear();
        body.clear();
        closed = false;
        closeError = 0;
        requestBody = wire::frame (message);
        requestSent = 0;

        const auto header = [] (const char* name, const std::string& value)
        {
            return nghttp2_nv { (uint8_t*) name, (uint8_t*) value.data(), std::strlen (name), value.size(),
                                NGHTTP2_NV_FLAG_NO_COPY_NAME };
        };

        const std::string post = "POST", scheme = "http", authority = "localhost:31416", grpc = "application/grpc",
                          trailers = "trailers", agent = "tempo-tantrum";
        const nghttp2_nv headers[] = {
            header (":method", post), header (":scheme", scheme), header (":path", path),
            header (":authority", authority), header ("content-type", grpc), header ("te", trailers),
            header ("user-agent", agent),
        };

        nghttp2_data_provider2 provider {};
        provider.read_callback = readBody;

        streamId = nghttp2_submit_request2 (session, nullptr, headers, std::size (headers), &provider, nullptr);

        if (streamId < 0)
        {
            broken = true;
            failure = "the request could not be made";
            return false;
        }

        const auto deadline = Clock::now() + std::chrono::milliseconds (timeoutMs);
        uint8_t buffer[16384];

        while (! closed)
        {
            if (nghttp2_session_send (session) != 0 || broken)
            {
                broken = true;
                failure = "the connection to Pro Tools was lost";
                return false;
            }

            if (shouldStop && shouldStop())
            {
                broken = true;
                stoppedEarly = true;
                failure = "the call was stopped before Pro Tools answered";
                return false;
            }

            const auto left = std::chrono::duration_cast<std::chrono::milliseconds> (deadline - Clock::now()).count();

            if (left <= 0)
            {
                broken = true;
                timedOut = true;
                failure = "Pro Tools did not answer within " + std::to_string (timeoutMs / 1000) + " s";
                return false;
            }

            pollfd p { fd, (short) (POLLIN | (nghttp2_session_want_write (session) ? POLLOUT : 0)), 0 };

            if (::poll (&p, 1, (int) std::min<long long> (left, 100)) < 0 && errno != EINTR)
            {
                broken = true;
                failure = "the connection to Pro Tools failed";
                return false;
            }

            if ((p.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            {
                const auto got = ::recv (fd, buffer, sizeof (buffer), 0);

                if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                {
                    broken = true;
                    failure = "Pro Tools closed the connection";
                    return false;
                }

                if (got > 0 && nghttp2_session_mem_recv2 (session, buffer, (size_t) got) < 0)
                {
                    broken = true;
                    failure = "Pro Tools' answer could not be read";
                    return false;
                }
            }
        }

        // What is left to send (window updates, say) goes now.
        nghttp2_session_send (session);

        if (closeError != NGHTTP2_NO_ERROR && grpcStatus < 0)
        {
            failure = "Pro Tools reset the call (HTTP/2 error " + std::to_string (closeError) + ")";
            return false;
        }

        result = wire::finishCall (httpStatus, grpcStatus, grpcMessage, body);
        return true;
    }
};

namespace
{
    wire::Schema schema()
    {
        namespace g = generated;
        wire::Schema s;
        s.requestHeader = g::requestHeaderField;
        s.requestBodyJson = g::requestBodyJsonField;
        s.requestHeaderCommand = g::requestHeaderCommandField;
        s.requestHeaderVersion = g::requestHeaderVersionField;
        s.requestHeaderSessionId = g::requestHeaderSessionIdField;
        s.requestHeaderVersionMinor = g::requestHeaderVersionMinorField;
        s.requestHeaderVersionRevision = g::requestHeaderVersionRevisionField;
        s.responseHeader = g::responseHeaderField;
        s.responseBodyJson = g::responseBodyJsonField;
        s.responseErrorJson = g::responseErrorJsonField;
        s.responseHeaderTaskId = g::responseHeaderTaskIdField;
        s.responseHeaderCommand = g::responseHeaderCommandField;
        s.responseHeaderStatus = g::responseHeaderStatusField;
        return s;
    }

    int commandNumber (const std::string& name)
    {
        for (const auto& command : generated::commands)
            if (name == command.name)
                return command.number;

        return -1;
    }

    const char* statusName (int status)
    {
        namespace g = generated;

        if (status == g::statusQueued)                  return "queued";
        if (status == g::statusPending)                 return "pending";
        if (status == g::statusInProgress)              return "in progress";
        if (status == g::statusCompleted)               return "completed";
        if (status == g::statusFailed)                  return "failed";
        if (status == g::statusWaitingForUserInput)     return "waiting for the user";
        return "unknown";
    }
}

Client::Client() = default;
Client::~Client() = default;

bool Client::hasLink()
{
    return true;
}

bool Client::connect (std::string& message)
{
    const std::lock_guard<std::recursive_mutex> hold (lock);

    if (isConnected())
        return true;

    connection = std::make_unique<Connection>();

    if (! connection->open (message))
    {
        connection = nullptr;
        return false;
    }

    return true;
}

bool Client::isConnected() const
{
    const std::lock_guard<std::recursive_mutex> hold (lock);
    return connection != nullptr && ! connection->broken;
}

void Client::dropConnection()
{
    // A session id is not known to outlive its connection: it is registered again.
    connection = nullptr;
    sessionId.clear();
}

void Client::disconnect()
{
    const std::lock_guard<std::recursive_mutex> hold (lock);
    dropConnection();
}

Reply Client::send (const std::string& command, const std::string& bodyJson, int timeoutMs)
{
    const std::lock_guard<std::recursive_mutex> hold (lock);
    const auto start = Clock::now();
    Reply reply;

    const auto fail = [&] (std::string why)
    {
        reply.message = std::move (why);
        reply.seconds = secondsSince (start);
        return reply;
    };

    const int number = commandNumber (command);

    if (number < 0)
        return fail ("the command " + command + " is not known to this build");

    if (! isConnected())
    {
        std::string why;
        dropConnection();

        if (! connect (why))
            return fail (why);

        if (command != "RegisterConnection" && ! registeredApplication.empty())
        {
            const auto again = registerConnection (registeredCompany, registeredApplication);

            if (! again.ok)
            {
                reply.timedOut = again.timedOut;
                reply.stopped = again.stopped;
                return fail ("Pro Tools did not take the scripting connection again: " + again.message);
            }
        }
    }

    wire::Request request;
    request.command = number;
    request.sessionId = command == "RegisterConnection" ? std::string() : sessionId;
    request.version = generated::apiVersion;
    request.versionMinor = generated::apiVersionMinor;
    request.versionRevision = generated::apiVersionRevision;
    request.bodyJson = bodyJson;

    wire::CallResult result;
    std::string failure;
    bool timedOut = false, stoppedEarly = false;

    if (! connection->call (generated::unaryPath, wire::encodeRequest (schema(), request), timeoutMs, stopCheck, result, failure,
                            timedOut, stoppedEarly))
    {
        reply.timedOut = timedOut;
        reply.stopped = stoppedEarly;

        // A call that ran out of time may still be answered later: the connection is not used again.
        if (connection->broken)
            dropConnection();

        return fail (failure);
    }

    if (connection->broken)
        dropConnection();

    if (! result.ok)
        return fail (result.error);

    wire::Response response;

    if (! wire::decodeResponse (schema(), result.message, response))
        return fail ("Pro Tools' answer was not a well-formed response");

    reply.answered = true;
    reply.body = std::move (response.bodyJson);
    reply.error = std::move (response.errorJson);

    if (response.status != generated::statusCompleted)
    {
        const auto why = errorSentence (reply.error);
        return fail (why.empty() ? std::string ("Pro Tools answered with the status ") + statusName (response.status) : why);
    }

    reply.ok = true;
    reply.seconds = secondsSince (start);
    return reply;
}

#else
//==============================================================================
// Without the SDK: every call fails and says why.
struct Client::Connection {};

Client::Client() = default;
Client::~Client() = default;

bool Client::hasLink()                      { return false; }

bool Client::connect (std::string& message)
{
    message = noLinkMessage;
    return false;
}

bool Client::isConnected() const            { return false; }
void Client::dropConnection()               {}
void Client::disconnect()                   {}

Reply Client::send (const std::string&, const std::string&, int)
{
    Reply reply;
    reply.message = noLinkMessage;
    return reply;
}
#endif

//==============================================================================
void Client::setStopCheck (std::function<bool()> shouldStop)
{
    const std::lock_guard<std::recursive_mutex> hold (lock);
    stopCheck = std::move (shouldStop);
}

Reply Client::registerConnection (const std::string& company, const std::string& application)
{
    const std::lock_guard<std::recursive_mutex> hold (lock);
    registeredCompany = company;
    registeredApplication = application;
    sessionId.clear();
    auto reply = send ("RegisterConnection", "{\"company_name\":" + quoted (company) + ",\"application_name\":" + quoted (application) + "}");

    if (reply.ok)
    {
        sessionId = str (parse (reply.body).getProperty ("session_id", juce::var()));

        if (sessionId.empty())
        {
            reply.ok = false;
            reply.message = "Pro Tools gave no session id";
        }
    }

    return reply;
}

bool Client::isRegistered() const
{
    const std::lock_guard<std::recursive_mutex> hold (lock);
    return ! sessionId.empty();
}

Reply Client::hostReadyCheck()
{
    return send ("HostReadyCheck", {});
}

Answer<std::string> Client::ptslVersion()
{
    Answer<std::string> a;
    a.reply = send ("GetPTSLVersion", {});

    if (a.ok())
    {
        const auto body = parse (a.reply.body);
        a.value = str (body.getProperty ("version", 0)) + "." + str (body.getProperty ("version_minor", 0)) + "."
                  + str (body.getProperty ("version_revision", 0));
    }

    return a;
}

Answer<std::string> Client::sessionName()
{
    Answer<std::string> a;
    a.reply = send ("GetSessionName", {});

    if (a.ok())
        a.value = str (parse (a.reply.body).getProperty ("session_name", juce::var()));

    return a;
}

Answer<double> Client::sessionSampleRate()
{
    Answer<double> a;
    a.reply = send ("GetSessionSampleRate", {});

    if (a.ok())
    {
        // "SR_48000" or "SRate_48000".
        const auto text = parse (a.reply.body).getProperty ("sample_rate", juce::var()).toString();
        a.value = text.fromLastOccurrenceOf ("_", false, false).getDoubleValue();

        if (! (a.value > 0.0))
        {
            a.reply.ok = false;
            a.reply.message = "the session sample rate \"" + text.toStdString() + "\" is not a number";
        }
    }

    return a;
}

Answer<std::string> Client::sessionStartTime()
{
    Answer<std::string> a;
    a.reply = send ("GetSessionStartTime", {});

    if (a.ok())
        a.value = str (parse (a.reply.body).getProperty ("session_start_time", juce::var()));

    return a;
}

Answer<std::string> Client::sessionLength()
{
    Answer<std::string> a;
    a.reply = send ("GetSessionLength", {});

    if (a.ok())
        a.value = str (parse (a.reply.body).getProperty ("session_length", juce::var()));

    return a;
}

Answer<std::string> Client::transportState()
{
    Answer<std::string> a;
    a.reply = send ("GetTransportState", {});

    if (a.ok())
        a.value = str (parse (a.reply.body).getProperty ("current_setting", juce::var()));

    return a;
}

Answer<MainCounterFormat> Client::mainCounterFormat()
{
    Answer<MainCounterFormat> a;
    a.reply = send ("GetMainCounterFormat", {});

    if (a.ok())
    {
        const auto body = parse (a.reply.body);
        a.value.type = str (body.getProperty ("current_type", juce::var()));
        a.value.setting = str (body.getProperty ("current_setting", juce::var()));
    }

    return a;
}

Answer<Selection> Client::editSelection (const std::string& locationType)
{
    Answer<Selection> a;
    a.reply = send ("GetEditSelection", "{\"location_type\":" + quoted (locationType) + "}");

    if (a.ok())
    {
        const auto body = parse (a.reply.body);
        a.value.in = str (body.getProperty ("in_time", juce::var()));
        a.value.out = str (body.getProperty ("out_time", juce::var()));
    }

    return a;
}

Answer<Selection> Client::timelineSelection (const std::string& locationType)
{
    Answer<Selection> a;
    a.reply = send ("GetTimelineSelection", "{\"location_type\":" + quoted (locationType) + "}");

    if (a.ok())
    {
        const auto body = parse (a.reply.body);
        a.value.in = str (body.getProperty ("in_time", juce::var()));
        a.value.out = str (body.getProperty ("out_time", juce::var()));
    }

    return a;
}

Answer<EditModeOptions> Client::editModeOptions()
{
    Answer<EditModeOptions> a;
    a.reply = send ("GetEditModeOptions", {});

    if (a.ok())
    {
        if (auto* options = parse (a.reply.body).getProperty ("edit_mode_options", juce::var()).getDynamicObject())
            for (const auto& property : options->getProperties())
                a.value.options.emplace_back (property.name.toString().toStdString(), (bool) property.value);
    }

    return a;
}

Answer<std::vector<FileLocation>> Client::fileLocations (const std::string& filter)
{
    Answer<std::vector<FileLocation>> a;
    a.reply = send ("GetFileLocation", "{\"file_filters\":[" + quoted (filter) + "],\"pagination_request\":{\"limit\":0,\"offset\":0}}");

    if (a.ok())
    {
        if (const auto* files = parse (a.reply.body).getProperty ("file_locations", juce::var()).getArray())
            for (const auto& file : *files)
                a.value.push_back ({ str (file.getProperty ("path", juce::var())), str (file.getProperty ("file_id", juce::var())),
                                     (bool) file.getProperty ("info", juce::var()).getProperty ("is_online", false) });
    }

    return a;
}

Answer<std::string> Client::timeAsType (const std::string& location, const std::string& fromType, const std::string& toType)
{
    Answer<std::string> a;
    a.reply = send ("GetTimeAsType", "{\"location\":{\"location\":" + quoted (location) + ",\"time_type\":" + quoted (fromType)
                                     + "},\"time_type\":" + quoted (toType) + "}");

    if (a.ok())
    {
        const auto converted = parse (a.reply.body).getProperty ("converted_location", juce::var());
        a.value = str (converted.getProperty ("location", juce::var()));

        if (a.value.empty())
        {
            a.reply.ok = false;
            a.reply.message = "Pro Tools gave no converted location";
        }
    }

    return a;
}

Answer<int> Client::trackCount()
{
    Answer<int> a;
    a.reply = send ("GetTrackList", "{\"page_limit\":1000,\"pagination_request\":{\"limit\":0,\"offset\":0}}");

    if (a.ok())
        if (const auto* tracks = parse (a.reply.body).getProperty ("track_list", juce::var()).getArray())
            a.value = tracks->size();

    return a;
}

Answer<std::vector<Clip>> Client::clipList()
{
    Answer<std::vector<Clip>> a;
    a.reply = send ("GetClipList", "{\"pagination_request\":{\"limit\":0,\"offset\":0}}");

    if (! a.ok())
        return a;

    const auto position = [] (const juce::var& value)
    {
        Position p;
        p.known = value.isObject() && value.hasProperty ("position");
        p.position = int64Of (value.getProperty ("position", 0));
        p.type = str (value.getProperty ("time_type", juce::var()));
        return p;
    };

    const auto body = parse (a.reply.body);
    auto list = body.getProperty ("clip_list", juce::var());

    if (! list.isArray())
        list = body.getProperty ("clips", juce::var());

    if (const auto* clips = list.getArray())
        for (const auto& clip : *clips)
        {
            Clip c;
            c.fileId = str (clip.getProperty ("file_id", juce::var()));
            c.clipId = str (clip.getProperty ("clip_id", juce::var()));
            c.fullName = str (clip.getProperty ("clip_full_name", juce::var()));
            c.rootName = str (clip.getProperty ("clip_root_name", juce::var()));
            c.start = position (clip.getProperty ("start_point", juce::var()));
            c.end = position (clip.getProperty ("end_point", juce::var()));
            c.sourceStart = position (clip.getProperty ("src_start_point", juce::var()));
            c.sourceEnd = position (clip.getProperty ("src_end_point", juce::var()));
            a.value.push_back (std::move (c));
        }

    return a;
}

Reply Client::togglePlayState()
{
    return send ("TogglePlayState", {});
}

Reply Client::setMainCounterFormat (const std::string& locationType)
{
    return send ("SetMainCounterFormat", "{\"location_type\":" + quoted (locationType) + "}");
}

Reply Client::setEditModeOptions (const EditModeOptions& options)
{
    std::string json = "{\"edit_mode_options\":{";

    for (size_t i = 0; i < options.options.size(); ++i)
        json += (i > 0 ? "," : "") + quoted (options.options[i].first) + ":" + (options.options[i].second ? "true" : "false");

    return send ("SetEditModeOptions", json + "}}");
}

Reply Client::setTimelineSelection (const std::string& in, const std::string& out, const std::string& locationType)
{
    return send ("SetTimelineSelection", "{\"in_time\":" + quoted (in) + ",\"out_time\":" + quoted (out)
                                         + ",\"location_type\":" + quoted (locationType) + "}");
}

} // namespace tt::ptsl
