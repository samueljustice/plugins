// Tests for the hand-written protocol buffer and gRPC code of the Pro Tools Scripting client
// (Source/ptsl/PtslWire.cpp), with byte sequences worked out by hand. The field numbers here are
// made up for the test: the real ones come from the SDK at build time and are not needed. Also
// the sums Write to Pro Tools makes with what the link reads (Source/protools/PtslSessionMaths.cpp),
// and the parts of "Get from Pro Tools" that need no Pro Tools (Source/capture/CaptureMaths.h).
// Prints one line per test; the exit code is the number of failed tests.

#include "../Source/ptsl/PtslWire.h"
#include "../Source/protools/PtslSession.h"
#include "../Source/capture/CaptureMaths.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace
{

using namespace tt::ptsl::wire;

struct Failure
{
    std::string what;
};

void expect (bool condition, const std::string& what)
{
    if (! condition)
        throw Failure { what };
}

std::string bytes (std::initializer_list<int> values)
{
    std::string out;

    for (int v : values)
        out.push_back ((char) (std::uint8_t) v);

    return out;
}

std::string hex (const std::string& data)
{
    std::string out;
    char two[4];

    for (unsigned char c : data)
    {
        std::snprintf (two, sizeof (two), "%02x ", c);
        out += two;
    }

    return out;
}

void expectBytes (const std::string& got, const std::string& wanted, const std::string& what)
{
    expect (got == wanted, what + ": got " + hex (got) + "wanted " + hex (wanted));
}

int run (const char* name, const std::function<void()>& test)
{
    try
    {
        test();
        std::printf ("PASS  %s\n", name);
        return 0;
    }
    catch (const Failure& f)
    {
        std::printf ("FAIL  %s  %s\n", name, f.what.c_str());
        return 1;
    }
}

// Field numbers made up for the test, none the same as another in its message. They are not the
// SDK's, which this test neither needs nor knows.
Schema testSchema()
{
    Schema s;
    s.requestHeader = 7;
    s.requestBodyJson = 9;
    s.requestHeaderCommand = 11;
    s.requestHeaderVersion = 12;
    s.requestHeaderSessionId = 14;
    s.requestHeaderVersionMinor = 15;
    s.requestHeaderVersionRevision = 17;
    s.responseHeader = 5;
    s.responseBodyJson = 6;
    s.responseErrorJson = 8;
    s.responseHeaderTaskId = 3;
    s.responseHeaderCommand = 4;
    s.responseHeaderStatus = 9;
    return s;
}

} // namespace

int main()
{
    int failed = 0;

    failed += run ("varints of one and more bytes", []
    {
        Writer w;
        w.varint (0);
        w.varint (1);
        w.varint (127);
        w.varint (128);
        w.varint (300);
        w.varint (16384);
        expectBytes (w.bytes(), bytes ({ 0x00, 0x01, 0x7f, 0x80, 0x01, 0xac, 0x02, 0x80, 0x80, 0x01 }), "varints");

        Writer big;
        big.varint (UINT64_MAX);
        expectBytes (big.bytes(), bytes ({ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01 }), "the largest varint");
    });

    failed += run ("the protocol buffer documentation's own examples", []
    {
        Writer w;
        w.intField (1, 150);
        expectBytes (w.bytes(), bytes ({ 0x08, 0x96, 0x01 }), "field 1 = 150");

        Writer s;
        s.stringField (2, "testing");
        expectBytes (s.bytes(), bytes ({ 0x12, 0x07, 't', 'e', 's', 't', 'i', 'n', 'g' }), "field 2 = \"testing\"");
    });

    failed += run ("negative numbers take ten bytes", []
    {
        Writer w;
        w.intField (3, -1);
        expectBytes (w.bytes(), bytes ({ 0x18, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01 }), "field 3 = -1");

        Reader r (w.bytes());
        Field f;
        expect (r.next (f) && f.number == 3 && f.type == varintType && (std::int32_t) f.value == -1, "-1 reads back");
        expect (! r.next (f) && ! r.failed(), "nothing after it");
    });

    failed += run ("zeros and empty strings are left out, empty messages are not", []
    {
        Writer w;
        w.intField (1, 0);
        w.stringField (2, "");
        expect (w.bytes().empty(), "nothing written: " + hex (w.bytes()));

        w.messageField (4, "");
        expectBytes (w.bytes(), bytes ({ 0x22, 0x00 }), "an empty message");

        Reader r (w.bytes());
        Field f;
        expect (r.next (f) && f.number == 4 && f.type == lengthType && f.bytes.empty(), "the empty message reads back");
    });

    failed += run ("large field numbers and long strings", []
    {
        const std::string text (300, 'x');
        Writer w;
        w.stringField (2000, text);
        // Field 2000, wire type 2: (2000 << 3) | 2 = 16002 = 0x82 0x7d. Length 300 = 0xac 0x02.
        expectBytes (w.bytes().substr (0, 4), bytes ({ 0x82, 0x7d, 0xac, 0x02 }), "tag and length");

        Reader r (w.bytes());
        Field f;
        expect (r.next (f) && f.number == 2000 && f.bytes == text, "reads back");
    });

    failed += run ("fields of other types are skipped over", []
    {
        // field 1 fixed64, field 2 fixed32, field 3 a varint, field 4 a string.
        const auto data = bytes ({ 0x09, 1, 2, 3, 4, 5, 6, 7, 8, 0x15, 9, 10, 11, 12, 0x18, 0x05, 0x22, 0x02, 'h', 'i' });
        Reader r (data);
        Field f;
        expect (r.next (f) && f.number == 1 && f.type == fixed64Type && f.value == 0x0807060504030201ull, "fixed64");
        expect (r.next (f) && f.number == 2 && f.type == fixed32Type && f.value == 0x0c0b0a09u, "fixed32");
        expect (r.next (f) && f.number == 3 && f.value == 5, "varint");
        expect (r.next (f) && f.number == 4 && f.bytes == "hi", "string");
        expect (! r.next (f) && ! r.failed(), "the end");
    });

    failed += run ("broken messages are noticed", []
    {
        Field f;

        // A Reader looks at the bytes it is given, so they are kept in variables.
        const auto varintBytes = bytes ({ 0x08, 0x96 }), stringBytes = bytes ({ 0x12, 0x07, 't', 'e' });
        const auto groupBytes = bytes ({ 0x0b }), zeroBytes = bytes ({ 0x00, 0x01 });

        Reader cutVarint (varintBytes);
        expect (! cutVarint.next (f) && cutVarint.failed(), "a varint cut short");

        Reader cutString (stringBytes);
        expect (! cutString.next (f) && cutString.failed(), "a string longer than the message");

        Reader group (groupBytes);
        expect (! group.next (f) && group.failed(), "a wire type this reader does not take");

        Reader fieldZero (zeroBytes);
        expect (! fieldZero.next (f) && fieldZero.failed(), "field number 0");
    });

    failed += run ("gRPC frames", []
    {
        expectBytes (frame ("abc"), bytes ({ 0, 0, 0, 0, 3, 'a', 'b', 'c' }), "a frame");
        expectBytes (frame (""), bytes ({ 0, 0, 0, 0, 0 }), "an empty frame");

        const std::string big (70000, 'z');
        expectBytes (frame (big).substr (0, 5), bytes ({ 0, 0, 0x01, 0x11, 0x70 }), "the length of a long message");

        std::string message;
        std::size_t used = 0;
        expect (unframe (bytes ({ 0, 0, 0 }), message, used) == Unframed::needMore, "a cut header");
        expect (unframe (bytes ({ 0, 0, 0, 0, 3, 'a' }), message, used) == Unframed::needMore, "a cut message");
        expect (unframe (bytes ({ 1, 0, 0, 0, 1, 'a' }), message, used) == Unframed::compressed, "a compressed message");

        const auto two = frame ("abc") + frame ("de");
        expect (unframe (two, message, used) == Unframed::message && message == "abc" && used == 8, "the first of two");
        expect (unframe (std::string_view (two).substr (used), message, used) == Unframed::message && message == "de", "the second");

        expect (unframe (frame (big), message, used) == Unframed::message && message == big, "a long message round trip");
    });

    failed += run ("a request, byte for byte", []
    {
        Request request;
        request.command = 300;
        request.version = 7;
        request.versionMinor = 3;
        request.versionRevision = 0;
        request.bodyJson = "{}";

        // Header: command (11) = 300 (0xac 0x02), version (12) = 7, minor (15) = 3. No session id, no revision.
        const auto header = bytes ({ 0x58, 0xac, 0x02, 0x60, 0x07, 0x78, 0x03 });
        const auto wanted = bytes ({ 0x3a, (int) header.size() }) + header + bytes ({ 0x4a, 0x02, '{', '}' });
        expectBytes (encodeRequest (testSchema(), request), wanted, "a request with a body");

        // With a session id and a revision (17, a two-byte tag), and no body.
        request.sessionId = "ab";
        request.versionRevision = 2;
        request.bodyJson = "";
        const auto header2 = bytes ({ 0x58, 0xac, 0x02, 0x60, 0x07, 0x72, 0x02, 'a', 'b', 0x78, 0x03, 0x88, 0x01, 0x02 });
        expectBytes (encodeRequest (testSchema(), request), bytes ({ 0x3a, (int) header2.size() }) + header2,
                     "a request with a session id and no body");
    });

    failed += run ("a response round trip, with fields the reader does not use", []
    {
        Writer header;
        header.intField (1, 100);           // not read
        header.stringField (3, "task-1");
        header.intField (4, 300);
        header.intField (9, 4);
        header.stringField (2, "{}");       // not read

        Writer response;
        response.stringField (2, "something newer");
        response.messageField (5, header.bytes());
        response.stringField (6, "{\"name\":\"S\"}");
        response.stringField (8, "{\"errors\":[]}");
        response.intField (3, 1);

        Response r;
        expect (decodeResponse (testSchema(), response.bytes(), r), "decodes");
        expect (r.taskId == "task-1" && r.command == 300 && r.status == 4, "the header: " + r.taskId);
        expect (r.bodyJson == "{\"name\":\"S\"}" && r.errorJson == "{\"errors\":[]}", "the body: " + r.bodyJson);

        Response empty;
        expect (decodeResponse (testSchema(), "", empty) && empty.status == 0 && empty.bodyJson.empty(), "an empty response");

        Response broken;
        const auto cut = bytes ({ 0x2a, 0x05, 0x18 });
        expect (! decodeResponse (testSchema(), cut, broken), "a header cut short");
    });

    failed += run ("how a call ended", []
    {
        Writer body;
        body.stringField (2, "{}");
        const auto framed = frame (body.bytes());

        auto r = finishCall (200, 0, "", framed);
        expect (r.ok && r.message == body.bytes(), "a good answer");

        // A trailers-only answer: the gRPC status comes in the headers and there is no body.
        r = finishCall (200, 14, "Connection%20refused", "");
        expect (! r.ok && r.grpcStatus == 14 && r.error == "gRPC status 14: Connection refused", "trailers only: " + r.error);

        r = finishCall (200, 12, "", "");
        expect (! r.ok && r.error == "gRPC status 12", "unimplemented: " + r.error);

        r = finishCall (200, -1, "", framed);
        expect (! r.ok && r.error.find ("without a gRPC status") != std::string::npos, "no status: " + r.error);

        r = finishCall (503, -1, "", "");
        expect (! r.ok && r.error.find ("503") != std::string::npos, "an HTTP error: " + r.error);

        r = finishCall (200, 0, "", "");
        expect (! r.ok && r.error.find ("no message") != std::string::npos, "no message: " + r.error);

        r = finishCall (200, 0, "", framed.substr (0, framed.size() - 1));
        expect (! r.ok && r.error.find ("cut short") != std::string::npos, "cut short: " + r.error);

        r = finishCall (200, 0, "", framed + framed);
        expect (! r.ok && r.error.find ("more than one") != std::string::npos, "two messages: " + r.error);

        expect (percentDecode ("100%25 %zz%2") == "100% %zz%2", "percent decoding leaves broken escapes alone");
    });

    failed += run ("multi-mono clips", []
    {
        using namespace tt::pt;
        expect (clipNameOf ("Cue 3.L.wav") == "cue 3" && clipNameOf ("Cue 3.R.WAV") == "cue 3" && clipNameOf ("cue 3.wav") == "cue 3",
                "channel endings and case: " + clipNameOf ("Cue 3.L.wav"));
        expect (clipNameOf ("Mix v1.2.wav") == "mix v1.2", "a numbered ending is not a channel");
        expect (clipNameOf ("Mix v1.final.wav") == "mix v1.final", "other endings are kept");
        expect (clipNameOf ("Cue.Lfe.wav") == "cue" && clipNameOf (".L.wav") == ".l", "LFE, and a name that is only an ending");

        auto clips = clipsOf ({ "Cue 3.L.wav", "Cue 3.R.wav" });
        expect (clips.size() == 1 && clips[0] == "Cue 3 (L, R)", "a stereo pair is one clip: " + (clips.empty() ? std::string() : clips[0]));

        clips = clipsOf ({ "Cue 3.wav" });
        expect (clips.size() == 1 && clips[0] == "Cue 3.wav", "an interleaved file is kept as it is");

        clips = clipsOf ({ "Cue 3.L.wav", "Other.wav", "Cue 3.R.wav", "Stem.L.wav" });
        expect (clips.size() == 3 && clips[0] == "Cue 3 (L, R)" && clips[1] == "Other.wav" && clips[2] == "Stem (L)", "three clips");

        clips = clipsOf ({ "Cue.L.wav", "Cue.C.wav", "Cue.R.wav", "Cue.Ls.wav", "Cue.Rs.wav", "Cue.LFE.wav" });
        expect (clips.size() == 1 && clips[0] == "Cue (L, C, R, Ls, Rs, LFE)", "5.1 is one clip");

        expect (clipsOf ({}).empty(), "nothing selected");
    });

    failed += run ("the tempo of the rest of the session", []
    {
        using namespace tt::pt;

        const auto survey = [] (std::vector<long long> ticks, long long perBeat = 960)
        {
            TempoSurvey s;
            s.ok = true;
            s.ticksPerBeat = perBeat;
            s.ticks = ticks;

            for (size_t i = 0; i < ticks.size(); ++i)
                s.samples.push_back (1000000 + 96000 * (long long) i);

            return s;
        };

        const auto before = survey ({ 0, 3840, 7680, 11520 });

        auto c = compareTempo (before, survey ({ 0, 3840, 7680, 11520 }));
        expect (c.compared && ! c.changed && ! c.shifted && ! c.partBeat, "unchanged");

        c = compareTempo (before, survey ({ 1, 3840, 7682, 11521 }));
        expect (! c.changed && ! c.shifted, "within 2 ticks");

        c = compareTempo (before, survey ({ 0, 3840, 7700, 11540 }));
        expect (c.changed && c.firstSample == 1096000 && c.lastSample == 1192000, "a change between the second and third points");

        c = compareTempo (before, survey ({ 3840, 7680, 11520, 15360 }));
        expect (! c.changed && c.shifted && ! c.partBeat && c.shiftBeats == 4 && c.shiftTicks == 3840, "a whole bar later");
        expect (describeShift (c.shiftBeats, 4) == "1 bar later", "said as bars: " + describeShift (c.shiftBeats, 4));

        c = compareTempo (before, survey ({ -1920, 1920, 5760, 9600 }));
        expect (! c.changed && c.shifted && ! c.partBeat && c.shiftBeats == -2, "two beats earlier");
        expect (describeShift (c.shiftBeats, 4) == "2 beats earlier", "said as beats: " + describeShift (c.shiftBeats, 4));

        // Every point moved by a third of a beat: the tempo is the same, the beats are not.
        c = compareTempo (before, survey ({ 300, 4140, 7980, 11820 }));
        expect (! c.changed && c.shifted && c.partBeat, "300 ticks is part of a beat");

        c = compareTempo (before, survey ({ 1536, 5376, 9216, 13056 }));
        expect (c.shifted && c.partBeat && c.shiftBeats == 2, "1.6 beats is not 2 beats");

        c = compareTempo (before, survey ({ 963, 4803, 8643, 12483 }));
        expect (c.shifted && c.partBeat, "a beat and 3 ticks");

        c = compareTempo (before, survey ({ 962, 4802, 8642, 12482 }));
        expect (c.shifted && ! c.partBeat && c.shiftBeats == 1, "a beat and 2 ticks is a beat");

        // Ticks counted at another rate: the tolerance and the beat follow it.
        const auto fine = survey ({ 0, 3840000, 7680000 }, 960000);
        c = compareTempo (fine, survey ({ 960001, 4800001, 8640001 }, 960000));
        expect (! c.changed && c.shifted && ! c.partBeat && c.shiftBeats == 1, "a beat in fine ticks");

        expect (describeShift (0, 4).empty(), "no shift says nothing");
        expect (describeShift (8, 4) == "2 bars later" && describeShift (-1, 4) == "1 beat earlier" && describeShift (6, 3) == "2 bars later",
                "counts");

        auto bad = before;
        bad.ok = false;
        bad.problem = "x";
        c = compareTempo (bad, before);
        expect (! c.compared && c.problem == "x", "a survey that failed");

        c = compareTempo (before, survey ({ 0, 3840 }));
        expect (! c.compared, "surveys of other points");
    });

    failed += run ("probe points and times", []
    {
        using namespace tt::pt;
        auto points = probePoints (0, 48000 * 10, 48000.0);
        expect (points.size() == 6 && points.front() == 0 && points.back() == 480000, "one every 2 s, both ends");

        points = probePoints (0, 48000LL * 3600, 48000.0);
        expect (points.size() == 400, "at most 400: " + std::to_string (points.size()));

        points = probePoints (0, 48000LL * 600, 48000.0);
        expect (points.size() == 301 && points[1] == 96000, "10 minutes, one every 2 s: " + std::to_string (points.size()));

        expect (probePoints (10, 10, 48000.0).empty() && probePoints (0, 10, 0.0).empty(), "nothing to probe");

        long long n = 0;
        expect (samplesOf ("2,880,000", n) && n == 2880000 && samplesOf ("-48", n) && n == -48 && ! samplesOf ("1|1|000", n), "samples");
        expect (secondsOfTimecode ("00:03:12:00") == 192.0 && secondsOfTimecode ("24:00:00:00") == 86400.0, "timecode");
        expect (minutesAndSeconds (192.7) == "3:12" && minutesAndSeconds (-1) == "0:00", "min:sec");
    });

    // The answers as Pro Tools 2026.11 gave them.
    failed += run ("answer formats", []
    {
        using namespace tt::pt;
        long long n = 0;
        expect (samplesOf ("2882880", n) && n == 2882880, "samples");
        expect (samplesOf ("1000141611150", n) && n == 1000141611150LL, "ticks as a large number");
        expect (samplesOf ("1000065280000", n) && n == 1000065280000LL, "ticks at bar 1");

        long long bar = 0, beat = 0, tick = 0;
        expect (barsBeatsOf ("   20| 4| 491", bar, beat, tick) && bar == 20 && beat == 4 && tick == 491, "bars|beats with padding");
        expect (barsBeatsOf ("1|2|000", bar, beat, tick) && bar == 1 && beat == 2 && tick == 0, "bars|beats as typed");
        expect (barsBeatsOf ("  -21| 3| 155", bar, beat, tick) && bar == -21 && beat == 3 && tick == 155, "a bar before bar 0");
        expect (! barsBeatsOf ("20|-4|000", bar, beat, tick) && ! barsBeatsOf ("-|1|000", bar, beat, tick), "a minus only on the bar");
        expect (! barsBeatsOf ("20|4", bar, beat, tick) && ! barsBeatsOf ("20|4|491|1", bar, beat, tick) && ! barsBeatsOf ("a|1|000", bar, beat, tick)
                    && ! barsBeatsOf ("2882880", bar, beat, tick),
                "not bars|beats");

        double s = 0.0;
        expect (secondsOfMinSecs ("   1:00.060", s) && std::abs (s - 60.06) < 1e-9, "min:sec with padding");
        expect (secondsOfMinSecs ("12:03", s) && s == 723.0, "min:sec without a fraction");
        expect (! secondsOfMinSecs ("1:75.000", s) && ! secondsOfMinSecs ("00:58:00:00.00", s), "not min:sec");

        expect (secondsOfTimecode ("00:58:00:00.00") == 3480.0, "timecode with sub-frames");
        expect (secondsOfTimecode ("24:00:00:00") == 86400.0, "the default session length");

        expect (sampleRateOfName ("SR_48000") == 48000.0 && sampleRateOfName ("SRate_96000") == 96000.0, "rates");
        expect (sampleRateOfName ("SR_Unknown") == 0.0 && sampleRateOfName ("48000") == 0.0, "not rates");

        expect (isStoppedState ("TS_TransportStopped") && isStoppedState ("TState_TransportStopped"), "stopped");
        expect (! isStoppedState ("TS_TransportPlaying") && ! isStoppedState (""), "not stopped");

        // Ticks are absolute, 960000 to the beat: 1|1|000 and 1|2|000 one beat apart.
        expect (1000066240000LL - 1000065280000LL == 960000 && 1000069120000LL - 1000065280000LL == 4 * 960000, "ticks to the beat");
    });

    failed += run ("session info EDLs", []
    {
        using namespace tt::pt;

        // The shape of the session info text with the track EDLs in samples, as Pro Tools 2026.11
        // gives it: times right-aligned in padded columns, and the second channel of a stereo
        // track after two blank lines with no column names of its own. Names are made up.
        const std::string text =
            "SESSION NAME:\tTest\n"
            "SAMPLE RATE:\t48000.000000\n"
            "BIT DEPTH:\t24-bit\n"
            "SESSION START TIMECODE:\t00:58:00:00\n"
            "TIMECODE FORMAT:\t29.97 Frame\n"
            "\n"
            "\n"
            "T R A C K  L I S T I N G\n"
            "TRACK NAME:\tMusic (Stereo)\n"
            "COMMENTS:\tA comment\n"
            "USER DELAY:\t0 Samples\n"
            "STATE: \n"
            "CHANNEL \tEVENT   \tCLIP NAME                     \tSTART TIME    \tEND TIME      \tDURATION      \tSTATE\n"
            "1       \t1       \tTone-01.L                     \t       2786784\t       2788386\t          1602\tUnmuted\n"
            "1       \t2       \tCue_01-02.L                   \t       2920943\t       3746470\t        825527\tUnmuted\n"
            "1       \t3       \tCue_01-03.L                   \t       3746470\t       4012925\t        266455\tMuted\n"
            "1       \t4       \tCue_01-04.L                   \t       4021780\t       4647843\t        626063\tUnmuted\n"
            "\n"
            "\n"
            "2       \t1       \tTone-01.R                     \t       2786784\t       2788386\t          1602\tUnmuted\n"
            "2       \t2       \tCue_01-02.R                   \t       2920943\t       3746470\t        825527\tUnmuted\n"
            "2       \t3       \tCue_01-03.R                   \t       3746470\t       4012925\t        266455\tMuted\n"
            "2       \t4       \tCue_01-04.R                   \t       4021780\t       4647843\t        626063\tUnmuted\n"
            "\n"
            "\n"
            "TRACK NAME:\tEffects\n"
            "COMMENTS:\t\n"
            "USER DELAY:\t0 Samples\n"
            "STATE: Hidden Inactive Muted\n"
            "CHANNEL \tEVENT   \tCLIP NAME                     \tSTART TIME    \tEND TIME      \tDURATION      \tSTATE\n"
            "1       \t1       \tHit.wav-14                    \t     272400000\t     272517988\t        117988\tUnmuted\n"
            "\n"
            "TRACK NAME:\tTimecode\n"
            "CHANNEL \tEVENT   \tCLIP NAME                     \tSTART TIME    \tEND TIME      \tDURATION      \tSTATE\n"
            "1       \t1       \tOther                         \t00:59:00:00   \t01:00:00:00   \t00:01:00:00   \tUnmuted\n";

        const auto events = edlEventsOf (text);
        expect (events.size() == 9, "nine clips in samples: " + std::to_string (events.size()));

        if (events.size() == 9)
        {
            expect (events[1].track == "Music (Stereo)" && events[1].channel == 1 && events[1].clip == "Cue_01-02.L" && events[1].start == 2920943
                        && events[1].end == 3746470,
                    "a clip with padded times");
            expect (events[5].channel == 2 && events[5].clip == "Cue_01-02.R" && events[5].start == 2920943, "the second channel, with no column names");
            expect (events[8].track == "Effects" && events[8].end == 272517988, "a clip on a hidden, inactive track");
        }

        long long last = 0;

        for (const auto& e : events)
            last = std::max (last, e.end);

        expect (last == 272517988, "the end of the last clip");
        expect (edlEventsOf ("").empty() && edlEventsOf ("TRACK NAME:\tA\nCHANNEL\tEVENT\n1\t1\n").empty(), "no clips");

        // The clip list's facts for those clips: one stereo file cut into three clips, the later
        // two moved, and the whole-file clip, which is not on the track.
        const auto clip = [] (const char* name, const char* file, long long sourceStart, long long sourceEnd)
        {
            tt::ptsl::Clip c;
            c.fullName = name;
            c.fileId = file;
            c.sourceStart.known = true;
            c.sourceStart.type = "BTType_Samples";
            c.sourceStart.position = sourceStart;
            c.sourceEnd.known = true;
            c.sourceEnd.type = "BTType_Samples";
            c.sourceEnd.position = sourceEnd;
            return c;
        };

        const std::vector<tt::ptsl::Clip> clips = {
            clip ("Tone-01.L", "tone", 0, 1602),               clip ("Tone-01.R", "tone", 0, 1602),
            clip ("Cue_01-02.L", "cue", 2766, 866356),         clip ("Cue_01-02.R", "cue", 2766, 866356),
            clip ("Cue_01-03.L", "cue", 866356, 1138044),      clip ("Cue_01-03.R", "cue", 866356, 1138044),
            clip ("Cue_01-04.L", "cue", 1138044, 1828590),     clip ("Cue_01-04.R", "cue", 1138044, 1828590),
            clip ("Cue_01.L", "cue", 0, 1900000),
        };

        // This text was asked for without its fades: the first clip seems to start where its fade
        // in ends, so the file seems to start later, and the other clips seem moved against it.
        auto place = placeMusic (events, 2882880, 4708704, clips, { "cue" });
        expect (place.found && place.clipName == "Cue_01-02.L" && place.clipStart == 2920943 && place.offsetKnown && place.offset == 2766,
                "without fades: the first clip of the selected file in the selection");
        expect (place.clips == 3 && place.clipEnd == 4647843 && place.clipsOffLine == 2, "without fades: three clips, two seem moved: " + std::to_string (place.clipsOffLine));
        expect (place.clipStart - place.offset == 2918177, "without fades: where the file seems to start");

        // The same clips with their fades shown, as the session reader asks for them (measured in a real
        // session: a capture of the selection lines up with the file starting at 2880114).
        {
            const std::string faded =
                "TRACK NAME:\tMusic (Stereo)\n"
                "CHANNEL \tEVENT   \tCLIP NAME                     \tSTART TIME    \tEND TIME      \tDURATION      \tSTATE\n"
                "1       \t1       \tTone-01.L                     \t       2786784\t       2788386\t          1602\tUnmuted\n"
                "1       \t2       \t(fade in)                     \t       2882880\t       2920943\t         38063\tUnmuted\n"
                "1       \t3       \tCue_01-02.L                   \t       2920943\t       3746470\t        825527\tUnmuted\n"
                "1       \t4       \tCue_01-03.L                   \t       3746470\t       4012925\t        266455\tMuted\n"
                "1       \t5       \t(cross fade)                  \t       4012925\t       4021780\t          8855\tUnmuted\n"
                "1       \t6       \tCue_01-04.L                   \t       4021780\t       4647843\t        626063\tUnmuted\n"
                "1       \t7       \t(fade out)                    \t       4647843\t       4708704\t         60861\tUnmuted\n";

            const auto withFades = edlEventsOf (faded);
            expect (withFades.size() == 7 && isFadeEvent (withFades[1]) && isFadeEvent (withFades[4]) && ! isFadeEvent (withFades[2]),
                    "fades are events of their own: " + std::to_string (withFades.size()));

            if (withFades.size() == 7)
            {
                const auto in = clipEdges (withFades, withFades[2], 863590);
                expect (in.startKnown && in.start == 2882880 && in.endKnown && in.end == 3746470, "a fade in is the start of its clip");

                const auto under = clipEdges (withFades, withFades[3], 271688);
                expect (under.startKnown && under.start == 3746470 && ! under.endKnown && under.end == 4012925, "a crossfade after: the end is not known");

                const auto placed = clipEdges (withFades, withFades[5], 690546);
                expect (placed.startKnown && placed.start == 4018158 && placed.endKnown && placed.end == 4708704,
                        "a crossfade before and a fade out after: the start found from the clip's length, " + std::to_string (placed.start));

                const auto noLength = clipEdges (withFades, withFades[5], 0);
                expect (! noLength.startKnown && noLength.earliest == 4012925 && noLength.latest == 4021780,
                        "without the length, only the range under the crossfade");
            }

            const auto fp = placeMusic (withFades, 2882880, 4708704, clips, { "cue" });
            expect (fp.found && fp.clipName == "Cue_01-02.L" && fp.clipStart == 2882880 && fp.offset == 2766,
                    "with fades: the first clip starts where its fade in starts");
            expect (fp.clipStart - fp.offset == 2880114, "with fades: where the file starts, " + std::to_string (fp.clipStart - fp.offset));
            expect (fp.clips == 3 && fp.clipsOffLine == 0 && fp.clipEnd == 4708704,
                    "with fades: three clips, none moved, ending where the fade out ends (" + std::to_string (fp.clipsOffLine) + " moved)");

            // A clip that really was moved is still counted.
            auto moved = clips;

            for (auto& c : moved)
                if (c.fullName == "Cue_01-03.L")
                    c.sourceStart.position += 48000;

            expect (placeMusic (withFades, 2882880, 4708704, moved, { "cue" }).clipsOffLine == 1, "with fades: a moved clip is counted");
        }

        place = placeMusic (events, 2700000, 4708704, clips, {});
        expect (place.found && place.clipName == "Tone-01.L", "with no file given, the first clip in the selection");

        place = placeMusic (events, 2700000, 4708704, clips, { "other" });
        expect (! place.found, "no clip of the file in the selection");

        place = placeMusic (events, 3000000, 3000000, clips, { "cue" });
        expect (place.found && place.clipName == "Cue_01-02.L" && place.clips == 1, "a cursor inside a clip");
    });

    failed += run ("survey plan", []
    {
        using namespace tt::pt;
        constexpr double rate = 48000.0;

        auto plan = planSurvey (2882880, 4708704, true, 9600000, rate);
        expect (plan.before.use && plan.before.from == 0 && plan.before.to == 2882880, "before: session start to the music");
        expect (plan.after.use && plan.after.from == 4708704 && plan.after.to == 9600000 && plan.afterSource == AfterEnd::lastClip, "after: to the last clip");

        plan = planSurvey (0, 4708704, true, 48000LL * 3 * 3600, rate);
        expect (! plan.before.use, "the music starts the session: nothing before it");
        expect (plan.after.to == 48000LL * 2 * 3600 && plan.afterSource == AfterEnd::cappedAtTwoHours, "after: at most two hours from the start");

        plan = planSurvey (2882880, 4708704, false, 0, rate);
        expect (plan.after.to == 4708704 + 48000LL * 1800 && plan.afterSource == AfterEnd::halfHourAfterMusic, "after: 30 minutes when the clips are not known");

        plan = planSurvey (2882880, 4708704, true, 4708704, rate);
        expect (! plan.after.use, "the music is the last clip: nothing after it");

        expect (! describeAfterEnd (AfterEnd::lastClip).empty() && ! describeAfterEnd (AfterEnd::halfHourAfterMusic).empty(), "said");

        // At most 400 points a side, about 5 s at 13 ms a conversion.
        expect (probePoints (plan.before.from, 48000LL * 2 * 3600, rate).size() <= (size_t) maxProbePoints, "points capped");
    });

    failed += run ("pin check in ticks", []
    {
        using namespace tt::pt;

        // 165 BPM at 48 kHz in 960000 ticks to the beat: 55 ticks a sample.
        const double perSample = 960000.0 * 165.0 / 60.0 / 48000.0;
        expect (std::abs (perSample - 55.0) < 1e-9, "55 ticks a sample");
        expect (samplesOffInTicks (1000141611150LL, 1000141611150LL, perSample) == 0.0, "the same ticks: no error");
        expect (std::abs (samplesOffInTicks (1000141611150LL, 1000141611150LL + 110, perSample) - 2.0) < 1e-9, "110 ticks later is 2 samples");
        expect (samplesOffInTicks (1000141611150LL, 1000141611150LL - 165, perSample) < -2.0, "165 ticks earlier is past 2 samples");
        expect (samplesOffInTicks (1, 2, 0.0) == 0.0, "no rate");
    });

    failed += run ("the Track Bounce request", []
    {
        using namespace tt::capture;
        const auto has = [] (const std::string& text, const std::string& part) { return text.find (part) != std::string::npos; };

        BounceSpec spec;
        spec.trackId = "{00000000-11111111-22222222-33333333}";
        spec.in = 2882880;
        spec.out = 4708704;
        spec.directory = "/tmp/Captures/a \"quoted\" folder";
        spec.sampleRate = 48000;
        spec.bitDepth = bitDepthName ("Bit24");

        const auto body = bounceTrackRequest (spec);
        expect (has (body, "\"src_track_id\":\"{00000000-11111111-22222222-33333333}\""), "the track, by id");
        expect (has (body, "\"in_location\":{\"location\":\"2882880\",\"time_type\":\"TLType_Samples\"}"), "in, in samples");
        expect (has (body, "\"out_location\":{\"location\":\"4708704\",\"time_type\":\"TLType_Samples\"}"), "out, in samples");
        expect (has (body, "\"offline_bounce\":\"TBool_True\""), "offline");
        expect (has (body, "\"import_after_bounce\":\"TBool_False\""), "not imported");
        expect (has (body, "\"file_destination\":\"EMFDestination_Directory\""), "to a folder");
        expect (has (body, "\"directory\":\"/tmp/Captures/a \\\"quoted\\\" folder\""), "the folder, quoted");
        expect (has (body, "\"file_type\":\"EMFType_WAV\""), "WAV");
        expect (has (body, "\"export_format\":\"EFormat_Interleaved\""), "interleaved");
        expect (has (body, "\"bit_depth\":\"BDepth_24\""), "the session's bit depth");
        expect (has (body, "\"sample_rate\":\"SRate_48000\""), "the session's rate");
        expect (has (body, "\"file_name_prefix\":\"Tempo Tantrum capture\""), "the prefix");
        expect (has (body, "\"audio_encoding_options\":{}"), "no encoding window");

        int depth = 0, least = 0;

        for (const char c : body)
        {
            depth += c == '{' ? 1 : c == '}' ? -1 : 0;
            least = std::min (least, depth);
        }

        expect (depth == 0 && least == 0 && body.front() == '{' && body.back() == '}', "braces balanced");

        spec.sampleRate = 47952;
        expect (has (bounceTrackRequest (spec), "\"sample_rate\":\"SRate_None\",\"sample_rate_custom\":47952"), "a custom rate");

        expect (bitDepthName ("Bit16") == "BDepth_16" && bitDepthName ("BDepth_32Float") == "BDepth_32Float"
                && bitDepthName ("Bit24") == "BDepth_24" && bitDepthName ("") == "BDepth_24", "bit depths");
        expect (hasTrackBounce ("2025.6.0") && hasTrackBounce ("2026.11.0") && ! hasTrackBounce ("2025.3.0")
                && ! hasTrackBounce ("2024.10.2") && ! hasTrackBounce (""), "Track Bounce from 2025.06");
        expect (canBounce ("TT_Audio") && canBounce ("TType_Audio") && canBounce ("TType_Aux") && canBounce ("TType_Instrument")
                && ! canBounce ("TType_Midi") && ! canBounce ("TT_Video") && ! canBounce ("TType_BasicFolder"), "track types");
        expect (jsonQuoted ("a\\b\n\x01") == "\"a\\\\b\\n\\u0001\"", "JSON escapes");
    });

    failed += run ("summing track bounces", []
    {
        using namespace tt::capture;

        // Two mono tracks: one mono sum, padded to the selection's length.
        auto sum = sumTracks ({ Channels { { 1.0f, 2.0f, 3.0f } }, Channels { { 0.5f, 0.5f } } }, 4);
        expect (sum.size() == 1 && sum[0] == std::vector<float> ({ 1.5f, 2.5f, 3.0f, 0.0f }), "mono plus mono");

        // A mono track with a stereo one: stereo, the mono on both sides; longer channels are cut.
        sum = sumTracks ({ Channels { { 1.0f, 1.0f, 1.0f } }, Channels { { 0.25f, 0.25f, 0.25f, 9.0f }, { -0.25f, -0.25f, -0.25f, 9.0f } } }, 3);
        expect (sum.size() == 2, "stereo");
        expect (sum[0] == std::vector<float> ({ 1.25f, 1.25f, 1.25f }) && sum[1] == std::vector<float> ({ 0.75f, 0.75f, 0.75f }), "each side");

        // Wider tracks go left, right, left, right.
        sum = sumTracks ({ Channels { { 1.0f }, { 2.0f }, { 4.0f }, { 8.0f } } }, 1);
        expect (sum.size() == 2 && sum[0][0] == 5.0f && sum[1][0] == 10.0f, "four channels folded");

        expect (sumTracks ({}, 3).size() == 1 && sumTracks ({}, 3)[0].size() == 3, "nothing: silence");
        expect (summedChannelCount ({ Channels { { 0.0f } } }) == 1 && summedChannelCount ({ Channels (12) }) == 2, "channel counts");

        // One output of a track routed to several.
        expect (filesToUse ({ 12, 2 }) == std::vector<std::size_t> ({ 1 }), "a 7.1.4 bed and a stereo path: the stereo one");
        expect (filesToUse ({ 6, 1 }) == std::vector<std::size_t> ({ 1 }), "else a mono one");
        expect (filesToUse ({ 12, 6 }) == std::vector<std::size_t> ({ 1 }), "else the narrowest");
        expect (filesToUse ({ 1, 1 }) == std::vector<std::size_t> ({ 0, 1 }), "multi-mono: all");
        expect (filesToUse ({ 2 }) == std::vector<std::size_t> ({ 0 }) && filesToUse ({}).empty(), "one file, or none");
    });

    failed += run ("capture folder pruning", []
    {
        using namespace tt::capture;
        std::vector<StoredCapture> stored;

        for (int i = 0; i < 25; ++i)
            stored.push_back ({ "Tempo Tantrum capture " + std::to_string (100 + i), 1000LL * i });

        auto gone = capturesToRemove (stored);
        expect (gone.size() == 5, "the last 20 stay");

        for (int i = 0; i < 5; ++i)
            expect (std::find (gone.begin(), gone.end(), "Tempo Tantrum capture " + std::to_string (100 + i)) != gone.end(), "the oldest go");

        expect (capturesToRemove ({ stored.begin(), stored.begin() + 20 }).empty(), "20 or fewer: none go");
        expect (capturesToRemove (stored, 0).size() == 25, "keep none");

        // The same time: the later name stays.
        gone = capturesToRemove ({ { "b", 5 }, { "a", 5 }, { "c", 1 } }, 1);
        expect (gone.size() == 2 && gone[0] == "a" && gone[1] == "c", "ties");
    });

    failed += run ("capture names", []
    {
        using namespace tt::capture;
        expect (startLabel ("TLType_BarsBeats", "   20| 4| 491", "") == "20|4|491", "bar|beat");
        expect (startLabel ("TLType_TimeCode", "", "00:59:00:01.12") == "00:59:00:01", "timecode without sub-frames");
        expect (startLabel ("TLType_TimeCode", "", "00:59:00;01") == "00:59:00;01", "drop-frame timecode");
        expect (startLabel ("TLType_Samples", "", "") == "", "not read");
        expect (captureName ({ "Music" }, "20|4|491") == "Music 20|4|491", "one track");
        expect (captureName ({ "Music A", "Music B", "Music C" }, "00:59:00:01") == "Music A + 2 more 00:59:00:01", "several tracks");
        expect (captureName ({}, "") == "Pro Tools", "no track");
        expect (safeFileName ("a/b:c|d") == "a b c d", "file names");
    });

    failed += run ("the time-0 model in Write", []
    {
        using namespace tt::pt;

        // Music captured from Pro Tools starts where the selection did: no clip is matched.
        auto start = musicStartFor (false, 0, true, true, 2882880);
        expect (start.known && start.sample == 2882880 && start.from == StartFrom::capture, "a capture starts at its selection");

        // A pin at 1.5 s of the capture lands 1.5 s after the selection's start.
        expect (start.sample + std::llround (1.5 * 48000.0) == 2954880, "pins from the selection's start");

        start = musicStartFor (true, 1000, true, true, 2882880);
        expect (start.known && start.sample == 1000 && start.from == StartFrom::typed, "a typed start wins");

        start = musicStartFor (false, 0, false, false, 0);
        expect (! start.known && start.from == StartFrom::session, "music from a file: read from the session during the run");

        start = musicStartFor (false, 0, true, false, 2882880);
        expect (! start.known && start.from == StartFrom::session, "a capture whose start is not known: as a file");

        start = musicStartFor (false, 0, true, true, -1);
        expect (! start.known, "a negative start is not used");

        expect (describeStart (StartFrom::capture) != describeStart (StartFrom::typed), "said apart in the log");
        expect (otherSession ("Trailer v3", "Trailer v4") && ! otherSession ("Trailer v3", "Trailer v3")
                && ! otherSession ("", "Trailer v4") && ! otherSession ("Trailer v3", ""), "another session");
    });

    std::printf ("\n%s: %d failed\n", failed == 0 ? "All passed" : "FAILED", failed);
    return failed;
}
