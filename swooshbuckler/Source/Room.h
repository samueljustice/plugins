#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>
#include <optional>

/**
 * The room: the maths of surround output, with no dependence on the engine.
 *
 *   widths      Mono, Stereo, Quad, 5.1, 7.1, 7.1.2, 7.1.4: the speakers of each, in the order the audio buffers
 *               carry them, and where each speaker stands
 *   pan()       a position in the room -> one gain per speaker
 *   flight()    the four flight paths: pass progress -> position and distance
 *   fold()      fixed matrices from every width to every narrower one
 *   file orders the Like Pro Tools and WAV Standard channel orders, the WAV speaker mask, the line that spells
 *               out the order, and the Multi-Mono file suffixes
 *
 * THE ROOM is a box with the listener in the middle of the floor:
 *
 *       x   -1 left wall ... +1 right wall
 *       y   -1 back wall ... +1 front wall (the screen)
 *       z    0 ear level ... 1 ceiling
 *
 * Speakers stand on the walls of that box, as they do on a film panner: L and R in the front corners, C in the
 * middle of the front wall, the side surrounds half way down the side walls, the rear surrounds (and the Ls / Rs
 * of 5.1 and Quad) in the back corners, the tops of 7.1.2 above the side surrounds, the four tops of 7.1.4 above
 * the side walls half way to the front and back walls.
 *
 * PROCESSING ORDER is JUCE's: the channels of a juce::AudioChannelSet in ascending channel type. That is what
 * processBlock sees in every plug-in format (the AAX wrapper turns Pro Tools' own order into it):
 *
 *       Mono    C
 *       Stereo  L R
 *       Quad    L R Ls Rs
 *       5.1     L R C LFE Ls Rs
 *       7.1     L R C LFE Lss Rss Lsr Rsr
 *       7.1.2   L R C LFE Lss Rss Lsr Rsr Lts Rts
 *       7.1.4   L R C LFE Lss Rss Ltf Rtf Ltr Rtr Lsr Rsr      (the rear surrounds come LAST)
 *
 * Everything here is allocation-free and lock-free: the tables are built when the program loads.
 */
namespace room
{
    constexpr int c_maxChannels = 12; // 7.1.4
    constexpr int c_numWidths = 7;

    enum class Width { Mono, Stereo, Quad, Surround51, Surround71, Surround712, Surround714 };

    /** Every speaker any width has. 5.1 and Quad have one surround pair (Ls Rs); 7.1 and wider have sides and rears. */
    enum class Speaker : std::uint8_t { L, R, C, Lfe, Ls, Rs, Lss, Rss, Lsr, Rsr, Ltf, Rtf, Ltr, Rtr, Lts, Rts };
    constexpr int c_numSpeakers = 16;

    enum class Role { Main, Lfe, Height };

    struct SpeakerInfo
    {
        const char* name;   // "Lss": as it is written in the order line
        const char* suffix; // ".Lss": the Multi-Mono file suffix, Pro Tools style
        Role role;
        // Where it stands for a listener, in degrees (ITU-R BS.2051 / Dolby): azimuth 0 is straight ahead and
        // positive to the right; elevation 0 is ear level. The Ls / Rs of 5.1 (and of Quad, which is laid out as
        // 5.1 without its centre) are at 110 degrees; side surrounds at 90, rear surrounds at 135, tops at 45 up.
        float azimuth, elevation;
        // Where it stands in the room box the panner works in.
        float x, y, z;
        juce::AudioChannelSet::ChannelType type;
        std::uint32_t wavBit; // its bit in a WAV file's speaker mask (0x1 front left ...), see wavChannelMask()
    };

    const SpeakerInfo& info(Speaker speaker) noexcept;

    /** The other side's speaker (L <-> R, Lss <-> Rss ...); C and LFE are their own mirror. */
    Speaker mirrored(Speaker speaker) noexcept;

    /** How much a speaker's power counts in a loudness reading, ITU-R BS.1770-4 (table 4), from where it stands:
        1.41 (+1.5 dB) for an ear-level speaker between 60 and 120 degrees round (the Ls / Rs of Quad and 5.1 at
        110, the side surrounds at 90), 1 for every other speaker (the fronts, the rear surrounds at 135, the tops),
        0 for the LFE, which a loudness reading leaves out. */
    float loudnessWeight(Speaker speaker) noexcept;

    struct Layout
    {
        Width width;
        const char* name; // "7.1.4": what menus show
        int numChannels;
        std::array<Speaker, c_maxChannels> speakers; // in processing order; entries past numChannels are unused
        int lfe;    // index of the LFE channel, or -1
        int centre; // index of the centre speaker, or -1 (Mono's one channel counts as a centre)

        /** The processing channel a speaker is on, or -1 when this width has no such speaker. */
        int indexOf(Speaker speaker) const noexcept;
    };

    const Layout& layout(Width width) noexcept;

    /** The width has an LFE channel (5.1, 7.1, 7.1.2, 7.1.4). */
    inline bool hasLfe(Width width) noexcept { return layout(width).lfe >= 0; }

    /** JUCE's own set for the width: mono, stereo, quadraphonic, create5point1, create7point1 (the film 7.1 with
        sides and rears), create7point1point2, create7point1point4. */
    juce::AudioChannelSet channelSet(Width width);

    /** The width a bus layout is, or nothing when it is not one of the seven. */
    std::optional<Width> widthFor(const juce::AudioChannelSet& set);

    //==================================================================================================================
    // THE PANNER
    //
    // A separable constant-power law, the kind a film room panner uses, chosen over panning between pairs or
    // triplets of speakers on a sphere because:
    //   - the power sum is exactly 1 by construction, at every position, for every width;
    //   - it is three sines and a square root per speaker: no search for the active speaker triangle, no matrix
    //     to invert, no branches that depend on the layout's geometry;
    //   - a position means the same thing at every width (x is always left-right, y always back-front), so one
    //     flight path renders sensibly to Quad, 5.1 and 7.1.4 alike, and to layouts whose speakers do not sit on
    //     a sphere round the listener (the tops of 7.1.2);
    //   - it is left-right symmetric by construction and every gain is a continuous function of the position.
    //
    // How it works. The POWER a speaker gets is the product of three shares:
    //   height   the tops get sin^2(pi/2 * z * Height) of the power, the ear-level bed the rest. A width with no
    //            tops keeps everything in the bed.
    //   depth    rows of speakers across the room. Two rows (front and back: Quad, 5.1) share the power as
    //            cos^2 / sin^2 across the whole depth; the four tops of 7.1.4 do the same between y = -0.5 and
    //            0.5 (they stand half way to the walls: c_topDepth), so a sound overhead is heard to travel. Three rows (7.1's front, sides, rears) share
    //            it between the two rows the position lies between, so y = 0 is the side speakers alone. One row
    //            (the tops of 7.1.2) takes it all.
    //   across   within a row, left and right share the power as cos^2 / sin^2 across the width. The front row
    //            also has the centre speaker: Centre 0 leaves it silent (a phantom centre between L and R);
    //            Centre 1 pans left - centre - right, so the middle of the front wall is the centre speaker alone.
    //            In between, the two laws are mixed by power.
    // Spread then mixes, by power, towards every speaker of the layer playing equally (the centre speaker joins in
    // only as far as Centre lets it). Each gain is the square root of its power. The LFE is never a pan target.
    //==================================================================================================================

    struct Position
    {
        float x = 0.0f; // -1 left ... 1 right
        float y = 1.0f; // -1 back ... 1 front
        float z = 0.0f; //  0 ear level ... 1 ceiling
    };

    struct PanSettings
    {
        float spread = 0.0f; // 0 a point ... 1 every speaker equally
        float centre = 0.0f; // 0 phantom centre (the centre speaker stays silent) ... 1 full centre speaker
        float height = 1.0f; // how much of z reaches the tops: 0 keeps everything in the bed
    };

    using Gains = std::array<float, c_maxChannels>;

    /** One gain per processing channel (zero past the width's channel count and on the LFE). Out-of-range positions
        and settings are clamped, and one that is not a number is read as 0, so the gains are always finite and
        their power is always 1. Mono is always 1; Stereo uses x alone. */
    void pan(Width width, Position position, const PanSettings& settings, Gains& gains) noexcept;

    //==================================================================================================================
    // FLIGHT PATHS
    //
    // Where a pass is, as a pure function of how far along its path it is. `closeness` (0 far ... 1 close) brings
    // the path nearer the listener; `direction` and `side` are +1 or -1 (anything else is read by its sign);
    // `altitude` (0 ear level ... 1 the ceiling) is how high the pass is at its nearest point.
    //
    //   Across    along a line in front of the listener, from the left wall to the right (direction +1) or right
    //             to left (-1). The line is at the front wall for a far pass and comes half way to the listener
    //             for the closest: no nearer, so that the pass stays in front where there are side speakers too
    //             (7.1 and wider give a position half way down the room half to the fronts, and one nearer than
    //             that mostly to the sides). `side` is not used.
    //                 x = direction * u      y = 1 - 0.5 * closeness                     (u = 2 * progress - 1)
    //   Through   from the front wall to the back (direction +1) or back to front (-1), passing the head on its
    //             left (side -1) or right (+1), always well out to that side: 0.6 for a far pass, 0.5 for the
    //             closest. (How close a pass is, is heard in its level, its brightness and its bend; brought in
    //             to the middle of the room it would be in both sides' speakers alike, the opposite of close.)
    //                 x = side * (0.6 - 0.1 * closeness)      y = -direction * u
    //   Overhead  front to back (direction +1) or back to front, over the listener, a little to one side for a
    //             far pass. Its altitude is its arch.
    //                 x = side * 0.3 * (1 - closeness)      y = -direction * u
    //   Around    round the listener, clockwise seen from above (direction +1: front, right, back, left) or
    //             anticlockwise. Half way through (progress 0.5) it is straight behind (side +1) or straight ahead
    //             (-1); progress 0 to 1 is one whole circle, and progress beyond that carries on round. The circle
    //             touches the walls for a far pass and shrinks to 0.45 for the closest.
    //                 r = 1 - 0.55 * closeness      x = r * sin(a)      y = r * cos(a)
    //                 a = direction * 2 pi * (progress - 0.5) (+ pi when side is +1)
    //
    // HEIGHT. Across, Through and Overhead rise to their altitude at the nearest point and are at ear level at the
    // walls: z = altitude * cos^2(pi/2 * u). Around keeps its altitude all the way: z = altitude. An Overhead pass
    // is given c_archTop by the engine, which also flies every path over the part of the pass that is heard
    // (RigEngine::pathProgress): the pass is heard to come up from the front speakers, is nearly all in the
    // speakers overhead at the top of its arch (sin^2(pi/2 * 0.85) = 0.95; the side speakers the rest), and goes
    // down into the rear ones. Over a whole pass the speakers overhead have about three fifths of the sound, and
    // a room with one pair of them still hears it travel. (Flown to the ceiling, and from wall to wall over the
    // whole pass, loud or not, nine tenths of a pass was in the tops: the ear-level speakers were 10 to 15 dB
    // down.)
    //
    // `distance` is how far the pass is from the listener in room units (what the engine spreads a near sound
    // by). On the floor that is the length of (x, y). Overhead flies an arch whose top is 0.8 - 0.55 * closeness
    // above the listener: distance^2 = x^2 + y^2 + top^2 * (1 - y^2). Across, Through and Overhead are nearest half
    // way through; Around keeps one distance all the way: a circle has no approach.
    //==================================================================================================================

    enum class Path { Across, Through, Overhead, Around };
    constexpr int c_numPaths = 4;
    constexpr float c_acrossNearest = 0.5f;                     // Across: how far from the front wall the closest pass flies
    constexpr float c_throughFar = 0.6f, c_throughNear = 0.5f;  // Through: how far out to its side a far and a close pass fly
    constexpr float c_archTop = 0.85f;                          // Overhead: the top of the arch (all but a twentieth in the tops there)
    constexpr float c_topDepth = 0.5f;                          // where the four tops of 7.1.4 stand between the listener and the walls

    const char* pathName(Path path) noexcept; // "Across" ...

    struct Flight
    {
        Position position;
        float distance = 1.0f;
    };

    /** Closeness and altitude outside 0..1 are held at the ends, and so is progress (but Around's, which may lap);
        not a number is read as 0. The position is always inside the room. */
    Flight flight(Path path, float progress, float closeness, float direction, float side, float altitude = 0.0f) noexcept;

    //==================================================================================================================
    // FOLD-DOWNS
    //
    // A fixed matrix from every width to every narrower one, each speaker going to its nearest neighbours in the
    // narrower width. Every coefficient is -3 dB (0.7071) for each step a speaker's signal takes, but the last:
    //
    //   7.1.4 -> 7.1.2   Ltf and Ltr -> Lts, -3 dB each (and the same on the right)
    //   tops  -> 7.1     Ltf -> L, Ltr -> Lsr, Lts -> Lss, -3 dB
    //   7.1   -> 5.1     Lss and Lsr -> Ls, -3 dB each
    //   5.1   -> Quad    C -> L and R, -3 dB each; the LFE is dropped
    //   Quad  -> Stereo  Ls -> L, Rs -> R, -3 dB       (so 5.1 -> Stereo is the ITU-R BS.775 fold: L + 0.7071 C + 0.7071 Ls)
    //   Stereo -> Mono   0.5 (L + R)
    //
    // A wider fold between rooms is those steps in a row (7.1.4 -> 5.1: Ltr reaches Ls at 0.7071^2). Only the
    // 5.1 -> Stereo coefficients are a standard; the 7.1 and height steps are this plug-in's own choice: -3 dB
    // wherever two speakers merge into one, which keeps the power of a sound panned half way between them and of
    // sounds that are unrelated in the two (a reverb's returns).
    //
    // TO STEREO (and Mono, which is always the Stereo fold averaged, never an average of all the channels) the
    // steps are NOT taken in a row below ear level: every surround speaker goes straight to its side at -3 dB,
    // side and rear surrounds of 7.1 as the Ls of 5.1 does in the ITU fold. In a row, by way of Ls, they would
    // lose 3 dB twice, and that is the wrong sum for what this plug-in puts there: a pass beside the listener is
    // in ONE side speaker at its loudest moment, and one going Around is in one rear pair, so they came out 6 dB
    // down where the same pass at 5.1 loses the ITU fold's 3 dB, and a 7.1.4 take folded to Stereo was 2 to 5 dB
    // quieter at its climax than the same whoosh rendered at Stereo. A top still takes its step to the ear-level
    // speaker under it first:
    //
    //   to Stereo   L = L + 0.7071 (C + Ls + Lss + Lsr + Ltf) + 0.5 (Ltr + Lts)        (and the same on the right)
    //
    // So one sound in one surround speaker is 3 dB down in Stereo at every width, one in a rear or side top 6 dB,
    // and one shared by a front speaker and a surround of the same side comes out up to 1.8 dB up, as it does
    // in the ITU fold of 5.1.
    //
    // THE LFE is kept where the narrower width has one and dropped where it has none (LfeFold::Drop, the default):
    // the mains already carry the low end, so nothing is lost. That holds for everything this plug-in puts in the
    // LFE: in Discrete the Sub stays in L and R as well, and Mixdown is made from the mains themselves. Folding it
    // in as well would count that low end twice. LfeFold::IntoFronts (for an LFE that carries what the mains do
    // not) adds it to L and R at -3 dB each instead. That keeps its level as it is in the file; a calibrated room
    // plays the LFE 10 dB louder than a main speaker, and the fold does not add those 10 dB (it would clip).
    //
    // WHAT A FOLD DOES TO THE LEVEL of one full-scale sound placed anywhere by pan() (measured by the Room test
    // over every position, Centre and Spread setting, for every pair of widths, and pinned there):
    //   - no channel of the folded take is more than c_foldMaxPeakDb above full scale (7.1.4 to Stereo, a sound
    //     that is in L, C and Ltf at once);
    //   - folded to Stereo or wider, its power is between c_foldMinPowerDb and c_foldMaxPowerDb of the sound's.
    //     The loss is 3 dB for each step a speaker's signal takes: the most is a sound in one top speaker of
    //     7.1.2 folded to Stereo or to 5.1, two steps;
    //   - folded to Mono it can lose 6 dB more (c_foldMonoMinPowerDb): that is the Stereo to Mono average, which
    //     halves a sound that is hard to one side.
    // A take of several unrelated full-scale channels can go higher, up to foldWorstCaseGain() (12.7 dB for
    // 7.1.4 to Stereo); folds that are written to a file or played therefore go through a limiter.
    //==================================================================================================================

    enum class LfeFold { Drop, IntoFronts };

    constexpr float c_foldStep = 0.70710678f; // -3 dB
    constexpr float c_foldMaxPeakDb = 3.9f;
    constexpr float c_foldMaxPowerDb = 5.7f;
    constexpr float c_foldMinPowerDb = -6.1f;
    constexpr float c_foldMonoMinPowerDb = -12.1f;

    struct FoldMatrix
    {
        int numIn = 0;  // channels of the wider width
        int numOut = 0; // channels of the narrower width
        // gain[out][in]: both in processing order.
        std::array<std::array<float, c_maxChannels>, c_maxChannels> gain {};
    };

    /** True when `to` is `from` or narrower. Quad is narrower than 5.1 and wider than Stereo. */
    bool canFold(Width from, Width to) noexcept;

    /** The matrix from `from` down to `to` (the identity when they are the same). Asking for a wider `to` returns
        the matrix of `from` to itself: there is no up-mix. */
    const FoldMatrix& fold(Width from, Width to, LfeFold lfe = LfeFold::Drop) noexcept;

    /** The most one output channel can reach when every input channel is at full scale and in phase: the largest
        row sum of the matrix. */
    float foldWorstCaseGain(const FoldMatrix& matrix) noexcept;

    /** Folds `numSamples` of `in` (matrix.numIn channels) into `out` (matrix.numOut channels). The buffers must
        not overlap. */
    void applyFold(const FoldMatrix& matrix, const float* const* in, float* const* out, int numSamples) noexcept;

    //==================================================================================================================
    // FILE CHANNEL ORDERS (Channel Order, for an interleaved WAV or AIFF file)
    //
    //   Like Pro Tools   exactly what Pro Tools 2026 writes when it bounces each width (measured from its own files):
    //                      Quad .. 7.1.2  the WAV speaker-mask order, with the mask in the header
    //                                     (L R Ls Rs / L R C LFE Ls Rs / L R C LFE Lsr Rsr Lss Rss, then Lts Rts)
    //                      7.1.4          the film order, L C R Lss Rss Lsr Rsr LFE Ltf Rtf Ltr Rtr, a plain header
    //                                     with NO mask, and the channels named in an iXML track list
    //   WAV Standard     the WAV speaker-mask order at every width, with the mask in the header (7.1.4: L R C LFE
    //                    Lsr Rsr Lss Rss Ltf Rtf Ltr Rtr, mask 0x2D63F), for programs that only go by the mask.
    // So the two differ only at 7.1.4. Mono and Stereo have one order. (A film order at every width, with no
    // speaker labels, is not offered: nothing reads it right but by luck.) FLAC fixes its own channel order, the
    // mask order, so a FLAC file is always written in it whatever Channel Order says.
    //
    // THE WAV SPEAKER MASK (WAVEFORMATEXTENSIBLE dwChannelMask) of the mask order:
    //       Mono    0x4       front centre
    //       Stereo  0x3       front left, front right
    //       Quad    0x33      + back left, back right
    //       5.1     0x3F      + front centre, LFE (the surrounds are the BACK pair, as Pro Tools writes 5.1)
    //       7.1     0x63F     + side left, side right (0x200, 0x400): the film 7.1, not the old five-front 0xFF
    //       7.1.2   0x563F    + top front left, top front right (0x1000, 0x4000), as Pro Tools writes 7.1.2
    //       7.1.4   0x2D63F   + top back left, top back right (0x8000, 0x20000)
    // The mask has eighteen speaker positions and none of them is a top side speaker, so the two tops of 7.1.2 are
    // written as the top front pair, as Pro Tools does (macOS then calls the file 9.1).
    //==================================================================================================================

    enum class FileOrder { ProTools, WavStandard };
    constexpr int c_numFileOrders = 2;

    const char* fileOrderName(FileOrder order) noexcept; // "Like Pro Tools", "WAV Standard"

    /** For each channel of the file, the processing channel it takes: file[i] = processing[fileChannels(...)[i]]. */
    const std::array<int, c_maxChannels>& fileChannels(Width width, FileOrder order) noexcept;

    /** The order spelt out, for the line under the Channel Order menu: "L R C LFE Ls Rs". */
    const char* orderLabel(Width width, FileOrder order) noexcept;

    /** The speaker mask of a file in the mask order (WAV Standard). */
    std::uint32_t wavChannelMask(Width width) noexcept;

    /** The two orders are different at this width (7.1.4 only). */
    bool ordersDiffer(Width width) noexcept;

    const char* multiMonoSuffix(Width width, int processingChannel) noexcept;

    //==================================================================================================================
    // THE ORDER TO AN AUDIO DEVICE (the app)
    //
    // The speakers at ear level and the LFE first, in processing order, then the speakers overhead:
    //       Quad    L R Ls Rs
    //       5.1     L R C LFE Ls Rs
    //       7.1     L R C LFE Lss Rss Lsr Rsr
    //       7.1.2   L R C LFE Lss Rss Lsr Rsr Lts Rts
    //       7.1.4   L R C LFE Lss Rss Lsr Rsr Ltf Rtf Ltr Rtr
    // This is how a monitor controller or interface is wired for 5.1, 7.1 and Dolby Atmos rooms (and Core Audio's
    // own 7.1.4 layout), and each room's outputs are the next narrower room's with speakers added, so a room wired
    // for 7.1 keeps every speaker where it is when four tops are added. It is the processing order for every width
    // but 7.1.4, where JUCE puts the rear surrounds after the tops.
    //==================================================================================================================

    /** The speakers in the order a meter bridge or a plan shows them (the room's display order): the fronts left,
        centre, right; the surrounds, sides before rears, left before right; the tops, front before back; the LFE
        last, set apart. Mono: C. Stereo: L R. Each entry is a processing channel; entries past the width's
        channels are -1. */
    inline std::array<int, c_maxChannels> displayChannels(Width width) noexcept
    {
        const Layout& l = layout(width);
        std::array<int, c_maxChannels> order;
        order.fill(-1);
        size_t next = 0;
        for (const Speaker s : { Speaker::L, Speaker::C, Speaker::R, Speaker::Ls, Speaker::Rs, Speaker::Lss, Speaker::Rss, Speaker::Lsr,
                                 Speaker::Rsr, Speaker::Ltf, Speaker::Rtf, Speaker::Lts, Speaker::Rts, Speaker::Ltr, Speaker::Rtr, Speaker::Lfe })
            if (const int ch = l.indexOf(s); ch >= 0 && next < order.size())
                order[next++] = ch;
        return order;
    }

    /** The speakers in the order Pro Tools lists a track's channels (Avid's AAX stem formats: L C R Ls Rs LFE,
        L C R Lss Rss Lsr Rsr LFE, then the tops): the order a Multi-Mono set's files are listed in. Each entry is
        a processing channel; entries past the width's channels are -1. */
    inline std::array<int, c_maxChannels> trackChannels(Width width) noexcept
    {
        const Layout& l = layout(width);
        std::array<int, c_maxChannels> order;
        order.fill(-1);
        size_t next = 0;
        for (const Speaker s : { Speaker::L, Speaker::C, Speaker::R, Speaker::Ls, Speaker::Rs, Speaker::Lss, Speaker::Rss, Speaker::Lsr,
                                 Speaker::Rsr, Speaker::Lfe, Speaker::Ltf, Speaker::Rtf, Speaker::Lts, Speaker::Rts, Speaker::Ltr, Speaker::Rtr })
            if (const int ch = l.indexOf(s); ch >= 0 && next < order.size())
                order[next++] = ch;
        return order;
    }

    /** For each output of the device (of those switched on, lowest first), the processing channel it plays:
        output[i] = processing[deviceChannels(width)[i]]. Entries past the width's channels are -1. */
    inline std::array<int, c_maxChannels> deviceChannels(Width width) noexcept
    {
        const Layout& l = layout(width);
        std::array<int, c_maxChannels> order;
        order.fill(-1);
        size_t next = 0;
        for (const bool overhead : { false, true })
            for (int ch = 0; ch < l.numChannels; ++ch)
                if ((info(l.speakers[static_cast<size_t>(ch)]).role == Role::Height) == overhead)
                    order[next++] = ch;
        return order;
    }
}
