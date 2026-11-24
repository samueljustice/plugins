// Tests for the hand-written protocol buffer and gRPC code of the Pro Tools Scripting client
// (Source/ptsl/PtslWire.cpp), with byte sequences worked out by hand. The field numbers here are
// made up for the test: the real ones come from the SDK at build time and are not needed. Also
// the sums Write to Pro Tools makes with what the link reads (Source/protools/PtslSessionMaths.cpp).
// Prints one line per test; the exit code is the number of failed tests.

#include "../Source/ptsl/PtslWire.h"
#include "../Source/protools/PtslSession.h"

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
        expect (points.size() == 300, "at most 300: " + std::to_string (points.size()));

        expect (probePoints (10, 10, 48000.0).empty() && probePoints (0, 10, 0.0).empty(), "nothing to probe");

        long long n = 0;
        expect (samplesOf ("2,880,000", n) && n == 2880000 && samplesOf ("-48", n) && n == -48 && ! samplesOf ("1|1|000", n), "samples");
        expect (secondsOfTimecode ("00:03:12:00") == 192.0 && secondsOfTimecode ("24:00:00:00") == 86400.0, "timecode");
        expect (minutesAndSeconds (192.7) == "3:12" && minutesAndSeconds (-1) == "0:00", "min:sec");
    });

    std::printf ("\n%s: %d failed\n", failed == 0 ? "All passed" : "FAILED", failed);
    return failed;
}
