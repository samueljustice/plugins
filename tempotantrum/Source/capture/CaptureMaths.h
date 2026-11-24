#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

// The parts of "Get from Pro Tools" that are plain numbers and text: the Track Bounce request,
// how the bounces of several tracks are summed into one analysis file, which captures the cache
// folder keeps, and the name a capture is shown with. Header only and plain C++, so the tests
// build it without Pro Tools, the link or JUCE.
namespace tt::capture
{

// The prefix of every file Pro Tools writes for a capture, and of the captures themselves.
inline constexpr const char* filePrefix = "Tempo Tantrum capture";

// How many captures the cache folder keeps: the oldest go when a newer one is made.
inline constexpr std::size_t capturesKept = 20;

// A string as a JSON string, quotes included.
inline std::string jsonQuoted (const std::string& text)
{
    std::string out = "\"";

    for (const char ch : text)
    {
        const auto c = (unsigned char) ch;

        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char escaped[8];
                    std::snprintf (escaped, sizeof (escaped), "\\u%04x", c);
                    out += escaped;
                }
                else
                {
                    out += ch;
                }
        }
    }

    return out + "\"";
}

// One track rendered over the edit selection.
struct BounceSpec
{
    std::string trackId;            // the track's id from GetTrackList
    long long in = 0, out = 0;      // the edit selection, in samples
    std::string directory;          // an absolute folder for the files
    std::string prefix = filePrefix;
    int sampleRate = 48000;         // the session's
    std::string bitDepth = "BDepth_24";     // the session's, as bitDepthName() gives it
};

// The session's bit depth as GetSessionBitDepth gives it ("Bit24" or "BDepth_24") as the name the
// request takes. 24-bit when it is not one of the three.
inline std::string bitDepthName (const std::string& setting)
{
    if (setting.find ("16") != std::string::npos)
        return "BDepth_16";

    if (setting.find ("32") != std::string::npos)
        return "BDepth_32Float";

    return "BDepth_24";
}

// The sample rate members of the request: one of the rates Pro Tools names, or a custom rate.
inline std::string sampleRateMembers (int rate)
{
    for (const int named : { 44100, 48000, 88200, 96000, 176400, 192000 })
        if (rate == named)
            return "\"sample_rate\":\"SRate_" + std::to_string (rate) + "\"";

    return "\"sample_rate\":\"SRate_None\",\"sample_rate_custom\":" + std::to_string (rate);
}

// The BounceTrack body: that track, from `in` to `out` in samples, rendered offline into
// `directory` as an interleaved WAV at the session's rate and bit depth, with the track's volume
// and pan automation, not imported into the session. Pro Tools' scripting documentation lists
// the audio and automation settings as not yet supported, but Pro Tools refuses a request
// without the audio settings, so both are sent; the files are read as they come whatever their
// format. An empty set of encoding options keeps any encoding window from opening.
inline std::string bounceTrackRequest (const BounceSpec& spec)
{
    const auto location = [] (long long samples)
    {
        return "{\"location\":" + jsonQuoted (std::to_string (samples)) + ",\"time_type\":\"TLType_Samples\"}";
    };

    return "{\"file_name_prefix\":" + jsonQuoted (spec.prefix)
           + ",\"file_type\":\"EMFType_WAV\""
           + ",\"audio_info\":{\"compression_type\":\"CType_PCM\",\"export_format\":\"EFormat_Interleaved\",\"bit_depth\":"
               + jsonQuoted (spec.bitDepth) + "," + sampleRateMembers (spec.sampleRate)
               + ",\"pad_to_frame_boundary\":\"TBool_False\",\"delivery_format\":\"EMDFormat_FilePerMixSource\"}"
           + ",\"location_info\":{\"file_destination\":\"EMFDestination_Directory\",\"directory\":" + jsonQuoted (spec.directory)
           + ",\"import_after_bounce\":\"TBool_False\"}"
           + ",\"offline_bounce\":\"TBool_True\""
           + ",\"audio_encoding_options\":{}"
           + ",\"src_track_id\":" + jsonQuoted (spec.trackId)
           + ",\"in_location\":" + location (spec.in)
           + ",\"out_location\":" + location (spec.out)
           + ",\"automation_options\":{\"render_volume_automation\":true,\"render_pan_automation\":true}"
           + "}";
}

// Whether a Pro Tools scripting version ("2025.6.0") has Track Bounce: 2025.06 and later.
inline bool hasTrackBounce (const std::string& version)
{
    int year = 0, month = 0;

    if (std::sscanf (version.c_str(), "%d.%d", &year, &month) < 1)
        return false;

    return year > 2025 || (year == 2025 && month >= 6);
}

// Whether a track of this type (GetTrackList's "TType_Audio", ...) can be rendered with Track Bounce.
inline bool canBounce (const std::string& trackType)
{
    for (const char* kind : { "Audio", "Aux", "Instrument" })
    {
        const std::string ending (kind);

        if (trackType.size() >= ending.size() && trackType.compare (trackType.size() - ending.size(), ending.size(), ending) == 0)
            return true;
    }

    return false;
}

//==============================================================================
// The channels of one rendered track: every channel of every file Pro Tools wrote for it, in
// order (a multi-mono bounce comes as one file a channel).
using Channels = std::vector<std::vector<float>>;

// Which of the files Pro Tools wrote for one track to use, from each file's channel count. A
// track routed to more than one output (a stereo path and a 7.1.4 bed, say) gets one
// interleaved file per output, each the same music: one is used, the stereo one if there is
// one, else a mono one, else the narrowest. Files that are all mono are the channels of one
// multi-mono output, and all are used.
inline std::vector<std::size_t> filesToUse (const std::vector<int>& channelCounts)
{
    std::vector<std::size_t> use;

    if (channelCounts.empty())
        return use;

    const bool allMono = std::all_of (channelCounts.begin(), channelCounts.end(), [] (int n) { return n == 1; });

    if (allMono)
    {
        for (std::size_t i = 0; i < channelCounts.size(); ++i)
            use.push_back (i);

        return use;
    }

    std::size_t best = 0;
    const auto rank = [] (int n) { return n == 2 ? 0 : n == 1 ? 1 : 1 + n; };

    for (std::size_t i = 1; i < channelCounts.size(); ++i)
        if (rank (channelCounts[i]) < rank (channelCounts[best]))
            best = i;

    use.push_back (best);
    return use;
}

// How many channels the analysis file of these tracks has: 2 when any track has two or more, else 1.
inline int summedChannelCount (const std::vector<Channels>& tracks)
{
    for (const auto& track : tracks)
        if (track.size() >= 2)
            return 2;

    return 1;
}

// The tracks summed into one mono or stereo signal `length` samples long. A mono track goes to
// both sides of a stereo sum; a wider track's channels go left, right, left, right... Shorter
// channels are padded with silence and longer ones cut, so every track starts at the selection's
// start. Nothing is scaled: the sum is floating point and only listened to by the analysis.
inline Channels sumTracks (const std::vector<Channels>& tracks, std::size_t length)
{
    const int count = summedChannelCount (tracks);
    Channels sum ((std::size_t) count, std::vector<float> (length, 0.0f));

    for (const auto& track : tracks)
    {
        for (std::size_t c = 0; c < track.size(); ++c)
        {
            const auto& source = track[c];
            const std::size_t n = std::min (length, source.size());

            for (int target = 0; target < count; ++target)
            {
                const bool goesHere = count == 1 || track.size() == 1 || (int) (c % 2) == target;

                if (! goesHere)
                    continue;

                auto& out = sum[(std::size_t) target];

                for (std::size_t i = 0; i < n; ++i)
                    out[i] += source[i];
            }
        }
    }

    return sum;
}

//==============================================================================
// A capture in the cache folder: its file name without the extension and when it was made.
struct StoredCapture
{
    std::string stem;
    long long madeAt = 0;           // any clock that orders them (milliseconds since 1970)
};

// The captures to delete so that only the newest `keep` stay. Ties keep the later name.
inline std::vector<std::string> capturesToRemove (std::vector<StoredCapture> captures, std::size_t keep = capturesKept)
{
    std::sort (captures.begin(), captures.end(), [] (const StoredCapture& a, const StoredCapture& b)
    {
        return a.madeAt != b.madeAt ? a.madeAt > b.madeAt : a.stem > b.stem;
    });

    std::vector<std::string> gone;

    for (std::size_t i = keep; i < captures.size(); ++i)
        gone.push_back (captures[i].stem);

    return gone;
}

//==============================================================================
// The start of a capture as the Main Counter shows positions: bar|beat ("20|4|491", from
// Pro Tools' padded "   20| 4| 491") when it counts in bars and beats, otherwise timecode
// ("00:59:00:01.00" as "00:59:00:01"). Empty when the one wanted could not be read.
inline std::string startLabel (const std::string& counterType, const std::string& barsBeats, const std::string& timecode)
{
    const bool bars = counterType.find ("Bars") != std::string::npos;
    std::string text;

    for (const char c : bars ? barsBeats : timecode)
        if (c != ' ')
            text += c;

    if (! bars)
    {
        // Sub-frames are left out: "hh:mm:ss:ff".
        int colons = 0;

        for (std::size_t i = 0; i < text.size(); ++i)
            if (text[i] == ':' || text[i] == ';')
                if (++colons == 3 && text.size() > i + 3)
                    text.resize (i + 3);
    }

    return text;
}

// What a capture is called in the window: "<track> <start>", or "<first track> + 2 more <start>"
// for several tracks.
inline std::string captureName (const std::vector<std::string>& trackNames, const std::string& start)
{
    std::string name = trackNames.empty() ? std::string ("Pro Tools") : trackNames.front();

    if (trackNames.size() > 1)
        name += " + " + std::to_string (trackNames.size() - 1) + " more";

    if (! start.empty())
        name += " " + start;

    return name;
}

// A file name made safe for the cache folder: the characters macOS and Pro Tools object to are
// replaced with a space.
inline std::string safeFileName (const std::string& text)
{
    std::string out;

    for (const char c : text)
        out += (c == '/' || c == ':' || c == '\\' || c == '|' || (unsigned char) c < 0x20) ? ' ' : c;

    return out;
}

} // namespace tt::capture
