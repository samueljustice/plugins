#pragma once

#include <JuceHeader.h>

#include "Room.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

/**
 * Takes on disk at any room width: how the channels of a take are laid out in files, what the files say about
 * themselves, and how a take is read back in PROCESSING ORDER (room::layout) whichever order it was written in.
 *
 *   Channel Order   Like Pro Tools or WAV Standard (room::FileOrder): the order the channels of an interleaved WAV
 *                   or AIFF file are written in. They differ only at 7.1.4. A FLAC file is always in FLAC's own
 *                   order (the WAV mask order), whatever Channel Order says.
 *   File Layout     Interleaved: one file with every channel. Multi-Mono: one mono file per channel, named
 *                   <name>.<suffix><ext> with the speaker names of Avid's AAX stem formats (.L .C .R .Ls .Rs .LFE
 *                   .Lss .Rss .Lsr .Rsr .Ltf .Rtf .Ltr .Rtr, the 7.1.2 tops .Lts .Rts). Mono is one file either way.
 *
 * WHAT A FILE SAYS ABOUT ITS CHANNELS (as Pro Tools 2026's own bounces do, and a little more):
 *   WAV    Quad and wider: WAVE_FORMAT_EXTENSIBLE with the speaker mask (room::wavChannelMask) when the channels are
 *          in the mask order; a 7.1.4 file Like Pro Tools is in the film order with a PLAIN 40-byte format chunk
 *          (tag 1, or 3 for float) and no mask, exactly as Pro Tools writes it. Every WAV file, of any width and
 *          each file of a Multi-Mono set, ends with an iXML chunk whose TRACK_LIST names each channel the way Pro
 *          Tools does (CHANNEL_INDEX, INTERLEAVE_INDEX, NAME "<name>-Lss", FUNCTION "Sl-7.1.4"). JUCE's own writer
 *          labels 8 channels as the old five-front 7.1 and gives 10 or 12 channels no mask, so the format chunk is
 *          rewritten after the file is closed (setWavChannelMask) and the iXML added after the sound.
 *   AIFF   Quad and wider, and each file of a Multi-Mono set: a CHAN chunk (Core Audio's channel layout, read by
 *          macOS and Logic) naming every channel, and the names in a comment ("Channels: L R C LFE Ls Rs").
 *   FLAC   always the mask order; CHANNELS=L R C LFE Ls Rs and WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3F (FLAC's own
 *          key for the mask) in its Vorbis comments. FLAC holds at most eight channels.
 * Reading: a WAV mask wins; without one, the iXML track list; an AIFF's CHAN chunk or comment; a FLAC's mask
 * comment or its own order. A file that says nothing is taken to be in the mask order.
 *
 * Nothing here is for the audio thread.
 */
namespace takefile
{
    enum class Layout { Interleaved = 0, MultiMono };
    constexpr int c_numLayouts = 2;

    /** The file types (the same numbers as TapeRecorder::FileType). */
    enum class Container { Wav = 0, Aiff, Flac };

    const char* layoutName(Layout layout) noexcept; // "Interleaved", "Multi-Mono"

    /** The width with that many channels (1, 2, 4, 6, 8, 10, 12), or nothing: every width has a count of its own. */
    std::optional<room::Width> widthForChannels(int numChannels) noexcept;

    /** FLAC's limit. */
    constexpr int c_maxFlacChannels = 8;

    /** The order a file of this type is written in: FLAC's own (the mask order) for FLAC, else the one chosen. */
    inline room::FileOrder orderFor(Container container, room::FileOrder chosen) noexcept
    {
        return container == Container::Flac ? room::FileOrder::WavStandard : chosen;
    }

    struct Format
    {
        room::Width width = room::Width::Stereo;
        room::FileOrder order = room::FileOrder::ProTools; // of an interleaved file (a Multi-Mono set has none)
        Layout layout = Layout::Interleaved;
    };

    /** The channels by name in the order they are in the file, for the metadata and the line under the menu:
        "L R C LFE Ls Rs". */
    juce::String channelLine(room::Width width, room::FileOrder order);

    /** The line under the Channel Order and File Layout choices, spelling out what a take of this width gives:
        "5.1 in one file: L R C LFE Ls Rs. Channel Order is for 7.1.4." (interleaved, in the order the file type
        and Channel Order give), "5.1 as 6 files ending .L .C .R .Ls .Rs .LFE." (Multi-Mono), "Mono: one channel
        in one file." */
    juce::String orderLine(room::Width width, room::FileOrder order, Layout layout, Container container = Container::Wav);

    /** Channel Order matters for this width (7.1.4: below it the two orders are the same file), and File Layout
        (Stereo and wider: Mono is one file either way). */
    inline bool orderMatters(room::Width width) noexcept { return room::ordersDiffer(width); }
    inline bool layoutMatters(room::Width width) noexcept { return width != room::Width::Mono; }
    /** Channel Order changes this file: 7.1.4, interleaved, and not FLAC (whose order is its own). */
    inline bool orderMatters(room::Width width, Layout layout, Container container) noexcept
    {
        return orderMatters(width) && layout == Layout::Interleaved && container != Container::Flac;
    }

    /** The speaker mask a WAV file of this width in this order carries: room::wavChannelMask when the order is the
        mask's own, nothing (a plain header, the channels named in iXML) for 7.1.4 Like Pro Tools. */
    std::optional<std::uint32_t> maskFor(room::Width width, room::FileOrder order) noexcept;

    /** The iXML a WAV file gets (Pro Tools' layout): a TRACK_LIST with every channel of the file. For an
        interleaved file `name` is the file's name without its extension and the tracks are the width's channels
        in this order; for one file of a Multi-Mono set (processingChannel >= 0) it is the one track, its
        CHANNEL_INDEX its place in the set. FUNCTION is Pro Tools' word for the speaker and the width ("Sl-7.1.4"). */
    juce::String ixmlFor(room::Width width, room::FileOrder order, const juce::String& name, int processingChannel = -1);

    /** Pro Tools' word for a speaker in an iXML FUNCTION (L C R Sl Sr Ls Rs LFE Tl Tr Trl Trr; the 7.1.4 ones are
        Pro Tools' own, the 7.1.2 tops Tl Tr after them). */
    const char* ixmlFunction(room::Width width, room::Speaker speaker) noexcept;

    /** The files a take written under the name `file` is made of, in processing order: `file` itself, or for a
        Multi-Mono take of Stereo or wider one file per channel, <name>.<suffix><ext> beside it. */
    juce::Array<juce::File> filesOf(const juce::File& file, room::Width width, Layout layout);
    bool anyExists(const juce::Array<juce::File>& files);

    /** A take found on disk from any one of its files. */
    struct Found
    {
        juce::Array<juce::File> files;  // a Multi-Mono set in processing order, or the one file
        Format format;
        int numChannels = 0;            // what the take has, in all
        bool knownWidth = false;        // numChannels is one of the seven widths
        /** Interleaved: file channel i is processing channel processingOf[i]. */
        std::array<int, room::c_maxChannels> processingOf {};
        bool described = false;         // the file said which speaker each channel is (otherwise it is assumed)
    };
    /** Finds the take `anyFile` belongs to: the whole Multi-Mono set when it is one of a complete set (the widest
        whose every file is there), else the file itself, read in the order it states (see the top of this file;
        a file that says nothing is taken to be in the mask order). numChannels is 0 when the file can't be read. */
    Found find(const juce::File& anyFile);

    /** What a file says its channels are, in file order ("L R C LFE Ls Rs"; empty when it says nothing or names
        speakers its width does not have). */
    juce::String readChannelLine(const juce::File& file);

    /** A take read in processing order: Multi-Mono sets as one reader, interleaved files with their channels put
        back where the rig had them. Null (and an error) when it can't be read. */
    std::unique_ptr<juce::AudioFormatReader> openReader(const juce::File& anyFile, juce::String* error = nullptr);
    std::unique_ptr<juce::AudioFormatReader> openReader(const Found& take, juce::String* error = nullptr);

    /** How the Takes list names a take: its file's name, or for a Multi-Mono set "<name> (6 Mono Files)". */
    juce::String displayName(const juce::Array<juce::File>& files);

    //==================================================================================================================
    // WAV headers
    struct WavFormat
    {
        bool ok = false;
        bool rf64 = false;
        int formatTag = 0;     // 1 PCM, 3 float, 0xFFFE extensible
        int channels = 0;
        int bits = 0;
        double rate = 0.0;
        bool extensible = false;
        std::uint32_t mask = 0;
    };
    WavFormat readWavFormat(const juce::File& file);

    /** Gives a closed WAV file's format chunk this speaker mask: WAVE_FORMAT_EXTENSIBLE with the mask, or with no
        mask the plain 40-byte chunk Pro Tools writes for 7.1.4 (tag 1 or 3, no extension, the rest zero). A plain
        16-byte format chunk grows to 40 bytes into the room of the JUNK chunk JUCE's writer leaves in front of it
        (so nothing moves); a 40-byte one is rewritten in place. False if the header is not one of those. */
    bool setWavChannelMask(const juce::File& file, std::optional<std::uint32_t> mask);

    /** A closed WAV file's iXML chunk (the first one, wherever it is), or empty. */
    juce::String readIxml(const juce::File& file);

    /** Adds a chunk at the end of a closed WAV (RIFF or RF64) file and makes the header's size say so. */
    bool appendWavChunk(const juce::File& file, const char* id, const void* data, size_t size);

    //==================================================================================================================
    // AIFF channel layout (Core Audio's CHAN chunk)
    /** Adds a CHAN chunk naming each channel (in file order) to the end of a closed AIFF file. */
    bool appendAiffChannelLayout(const juce::File& file, const juce::Array<room::Speaker>& speakers);
    /** The speakers a CHAN chunk names, in file order (empty when there is none, or it names a channel no width has). */
    juce::Array<room::Speaker> readAiffChannelLayout(const juce::File& file);

    //==================================================================================================================
    // FLAC Vorbis comments
    juce::StringArray flacComments(const juce::File& file, juce::String* vendor = nullptr);
    /** Gives a closed FLAC file these comments (NAME=value), replacing the ones it had. */
    bool setFlacComments(const juce::File& file, const juce::StringArray& comments);
}
