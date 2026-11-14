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
 *   Channel Order   Pro Tools (Film) or SMPTE / WAV (room::FileOrder): the order the channels of an interleaved
 *                   file are written in. Mono, Stereo and Quad are the same in both.
 *   File Layout     Interleaved: one file with every channel. Multi-Mono: one mono file per channel, named
 *                   <name>.<suffix><ext> with the Pro Tools suffixes (.L .C .R .Ls .Rs .LFE .Lss .Rss .Lsr .Rsr
 *                   .Ltf .Rtf .Ltr .Rtr, the 7.1.2 tops .Ltm .Rtm), so Pro Tools takes the set as one clip.
 *                   Mono is one file either way.
 *
 * WHAT A FILE SAYS ABOUT ITS CHANNELS (wider than Stereo, and every file of a Multi-Mono set):
 *   WAV    WAVE_FORMAT_EXTENSIBLE with the speaker mask of room::wavChannelMask in the SMPTE / WAV order (and for
 *          Quad, whose two orders are one); mask 0 ("no speaker positions") in the Pro Tools (Film) order, which no
 *          mask can describe. The channels by name in the iXML chunk (ASWG channelConfig: "L C R Ls Rs LFE").
 *          JUCE's own writer labels 8 channels as the old five-front 7.1 and gives 10 or 12 channels no mask, so
 *          the header is rewritten after the file is closed (setWavChannelMask).
 *   AIFF   the channels by name in a comment ("Channels: L C R Ls Rs LFE").
 *   FLAC   CHANNELS=L C R Ls Rs LFE and WAVEFORMATEXTENSIBLE_CHANNEL_MASK (FLAC's own key for the mask) in its
 *          Vorbis comments. FLAC holds at most eight channels.
 * Mono and Stereo files are written as they always were (a plain header, nothing added).
 *
 * Nothing here is for the audio thread.
 */
namespace takefile
{
    enum class Layout { Interleaved = 0, MultiMono };
    constexpr int c_numLayouts = 2;

    const char* layoutName(Layout layout) noexcept; // "Interleaved", "Multi-Mono"

    /** The width with that many channels (1, 2, 4, 6, 8, 10, 12), or nothing: every width has a count of its own. */
    std::optional<room::Width> widthForChannels(int numChannels) noexcept;

    /** FLAC's limit. */
    constexpr int c_maxFlacChannels = 8;

    struct Format
    {
        room::Width width = room::Width::Stereo;
        room::FileOrder order = room::FileOrder::ProTools; // of an interleaved file (a Multi-Mono set has none)
        Layout layout = Layout::Interleaved;
    };

    /** The channels by name in the order they are in the file, for the metadata and the line under the menu:
        "L C R Ls Rs LFE". */
    juce::String channelLine(room::Width width, room::FileOrder order);

    /** The line under the Channel Order and File Layout choices, spelling out what a take of this width gives:
        "5.1 in one file: L C R Ls Rs LFE." (interleaved, in the order chosen; below 5.1 it adds that Channel Order
        is for 5.1 and wider), "5.1 as 6 files ending .L .C .R .Ls .Rs .LFE." (Multi-Mono, the suffixes in the Pro
        Tools order), "Mono: one channel in one file." */
    juce::String orderLine(room::Width width, room::FileOrder order, Layout layout);

    /** Channel Order matters for this width (5.1 and wider: Mono, Stereo and Quad have one order), and File Layout
        (Stereo and wider: Mono is one file either way). */
    inline bool orderMatters(room::Width width) noexcept { return room::layout(width).numChannels >= 6; }
    inline bool layoutMatters(room::Width width) noexcept { return width != room::Width::Mono; }

    /** The speaker mask a WAV file of this width in this order carries: room::wavChannelMask where the order is the
        mask's own (SMPTE / WAV, and Mono to Quad, which have one order), 0 for the Pro Tools (Film) order of 5.1 and
        wider. */
    std::uint32_t maskFor(room::Width width, room::FileOrder order) noexcept;

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
        whose every file is there), else the file itself, read with the order its metadata states (a WAV file
        without one but with the mask of its width is read in mask order; anything else wider than Stereo is
        taken to be in the SMPTE / WAV order). numChannels is 0 when the file can't be read. */
    Found find(const juce::File& anyFile);

    /** What a file says its channels are, in file order ("L C R Ls Rs LFE"; empty when it says nothing). */
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

    /** Makes a closed WAV file's format chunk WAVE_FORMAT_EXTENSIBLE with this speaker mask. A plain 16-byte format
        chunk becomes the 40-byte one in the room of the JUNK chunk JUCE's writer leaves in front of it (so nothing
        moves); an RF64 file's is extensible already. False if the header is not one of those. */
    bool setWavChannelMask(const juce::File& file, std::uint32_t mask);

    //==================================================================================================================
    // FLAC Vorbis comments
    juce::StringArray flacComments(const juce::File& file, juce::String* vendor = nullptr);
    /** Gives a closed FLAC file these comments (NAME=value), replacing the ones it had. */
    bool setFlacComments(const juce::File& file, const juce::StringArray& comments);
}
