#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * What LEARN keeps: the types shared by the capture on the audio thread, the analysis on the worker thread, the
 * view and the saved state.
 *
 * A region is one stretch of the host timeline that Subbertone has heard and analysed with hindsight. It holds
 *   - the pitch curve of the line the analysis chose there (5 ms frames, natural log of Hz),
 *   - the other lines the analysis found (candidates, as ratios to that curve), so the view can offer them,
 *   - a fingerprint of the audio every 10 ms, so playback can tell whether it still hears what was learned: an
 *     edit, another take or a change before Subbertone makes it fall back to live tracking there.
 *
 * Positions are timeline samples at the rate the region was learned at (a session at another rate converts them by
 * time). A region starts and ends on its 10 ms hop grid, and every hop has exactly one fingerprint, so the region's
 * span and its prints can never disagree.
 *
 * Regions are immutable once published. An edit (another line, a different gap fill, an alignment) publishes a
 * changed copy under the same id. A Snapshot is the complete set of regions at one moment; LearnStore publishes
 * snapshots to the audio thread and checks every region against the rules below before it does (see
 * learn::findProblem()), so readers can rely on them without checking again.
 *
 * Plain C++20, no JUCE.
 */
namespace subbertone::learn
{
    //==============================================================================
    // Grids. Part of the saved format: hops and frames are counted on these.
    inline constexpr double c_hopSeconds = 0.010;    // fingerprint hop, rounded to whole samples of the learned rate
    inline constexpr double c_frameSeconds = 0.005;  // pitch curve frame spacing

    // Limits
    inline constexpr double c_maxRegionSeconds = 60.0;    // the longest capture, so the longest region
    inline constexpr double c_maxLearnedSeconds = 600.0;  // all regions of one instance together
    inline constexpr int c_maxRegions = 64;
    inline constexpr int c_maxCandidates = 8;             // presence has one bit per candidate
    inline constexpr double c_minCaptureSeconds = 1.0;    // without a loop, stopping completes a capture this long
    inline constexpr double c_minRegionSeconds = 1.0;     // what a newer region leaves of an older one stays if this long

    // Analysis
    inline constexpr double c_lowestHz = 30.0;            // where the analysis looks for lines
    inline constexpr double c_highestHz = 1000.0;
    inline constexpr double c_defaultGapFillSeconds = 0.150;
    inline constexpr double c_maxGapFillSeconds = 1.0;
    inline constexpr float c_silenceBelowThresholdDb = 10.0f;  // frames this far below the Threshold are silent
    inline constexpr float c_unknownSilenceDbfs = -70.0f;      // a region saved without its silence level: the knob's
                                                               // lowest Threshold less the margin above, so it never
                                                               // hands silent frames to live tracking
    inline constexpr float c_minSilenceDbfs = -130.0f;         // what a region's silence level may be
    inline constexpr float c_maxSilenceDbfs = 0.0f;

    inline constexpr double c_undoSeconds = 10.0;         // UNDO stays offered this long after forgetting

    // The fingerprint's octave bands (Hz), measured at the capture rate after a 3.2 kHz anti-alias low-pass that is
    // the same at every host rate. Part of the saved format: prints compare only if they are measured the same way.
    inline constexpr double c_antiAliasHz = 3200.0;
    inline constexpr std::array<double, 7> c_bandEdgesHz { 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 3200.0 };

    // What a region may hold. LearnStore refuses anything outside these, also in a loaded state.
    inline constexpr double c_minSampleRate = 8000.0;
    inline constexpr double c_maxSampleRate = 768000.0;
    inline constexpr double c_maxTimelineSeconds = 1.0e7;  // positions within about 115 days of zero
    inline constexpr double c_maxAlignmentSeconds = 1.0;
    inline constexpr float c_minLineRatio = 1.0f / 16.0f;
    inline constexpr float c_maxLineRatio = 16.0f;
    inline constexpr float c_minLineHz = 10.0f;            // a pitched frame's frequency
    inline constexpr float c_maxLineHz = 20000.0f;
    inline constexpr float c_maxCandidateHz = 100000.0f;

    /** The fingerprint hop at a sample rate, in samples. */
    inline int hopSamplesFor(double sampleRate) noexcept
    {
        return static_cast<int>(std::lround(c_hopSeconds * sampleRate));
    }

    /** The most hops a region learned at this rate (c_minSampleRate .. c_maxSampleRate) may have: a capture of the
        longest region that starts and ends inside a hop touches one hop more than the length suggests. */
    inline std::int64_t maxHopsFor(double sampleRate) noexcept
    {
        const int hop = hopSamplesFor(sampleRate);
        return hop > 0 ? static_cast<std::int64_t>(std::ceil(c_maxRegionSeconds * sampleRate / hop)) + 1 : 0;
    }

    //==============================================================================
    /** Which analysis made a curve. The values are saved: never renumber them. */
    enum class Method : std::uint8_t
    {
        Auto = 0,    // let the analysis choose: it runs both and keeps the one that fits the sound
        Engine = 1,  // for sounds whose partials all move together (engines, drones)
        Tonal = 2    // for notes (bass lines, melodies)
    };

    /** One 5 ms point of a region's pitch curve. */
    struct Frame
    {
        float logLineHz = 0.0f;   // natural log of the line's frequency in Hz; 0 where there is no pitch
        float confidence = 0.0f;  // 0..1; 0 on bridged frames
        std::uint8_t flags = 0;

        static constexpr std::uint8_t voiced = 1;   // there is a pitch to follow (measured, or bridged)
        static constexpr std::uint8_t bridged = 2;  // filled in across a short gap, not measured (always voiced)
        static constexpr std::uint8_t silent = 4;   // below the silence threshold: a note boundary, never voiced

        bool isVoiced() const noexcept { return (flags & voiced) != 0; }
    };

    /** What the audio sounded like over one 10 ms hop (8 bytes). */
    struct Fingerprint
    {
        static constexpr int numBands = 6;

        std::uint8_t level = 0;                      // mean energy of the hop: dBFS + 120, in 1 dB steps
        std::array<std::uint8_t, numBands> band {};  // each octave band's share of that energy: dB below it, 0..60
        std::uint8_t flags = 0;

        // A hop with neither `complete` nor `partial` was not heard at all (a gap in the capture, kept as silence):
        // playback never compares it.
        static constexpr std::uint8_t complete = 1;  // every sample of the hop was captured
        static constexpr std::uint8_t partial = 2;   // only part of it was (an edge of the capture, a dropped block)
        static constexpr std::uint8_t clipped = 4;   // the input reached full scale in it

        bool isHeard() const noexcept { return (flags & (complete | partial)) != 0; }
    };

    static_assert(sizeof(Fingerprint) == 8);

    /** Another line the analysis found in a region. */
    struct Candidate
    {
        float ratio = 1.0f;  // its frequency over the region's curve (1 = the curve itself)
        float score = 0.0f;  // how strong it is, relative to the strongest candidate (0..1)
        float maxHz = 0.0f;  // its highest frequency in the region
    };

    /** Figures for the LEARN button and the view's status line. LearnStore works them out from the frames and
        prints (learn::measure()), so they always describe the data as it is, also after an edit or a reload. */
    struct Quality
    {
        float heardShare = 0.0f;      // hops that were captured (the rest were gaps, kept as silence)
        float voicedShare = 0.0f;     // frames above the silence threshold that have a pitch
        float meanConfidence = 0.0f;  // over the measured frames (voiced, not bridged)
        float bridgedSeconds = 0.0f;  // how much of the curve gap fill bridged
        int jumps = 0;                // the view's STEPS: how often the measured line moves by more than a semitone at
                                      // once, each move counted once, through gap fill and never across silence (a
                                      // bass line's note changes; an engine's line glides, so it has few if any)
    };

    /** What playback found out about a region, for the LEARN button and the view. Written by the audio thread only,
        with relaxed atomics: each value is a display figure on its own, so a reader that sees one field updated before
        another shows a slightly stale figure, nothing worse. Not saved: it is worked out again from the audio on
        every pass, so playback and a bounce decide the same. */
    struct RegionLive
    {
        std::atomic<std::uint32_t> passes { 0 };        // passes heard since it was learned or loaded
        std::atomic<std::uint32_t> comparedHops { 0 };  // hops of the latest pass compared with the learned prints
        std::atomic<std::uint32_t> invalidHops { 0 };   // ... of which fell back to live tracking

        // The latest pass's stretches that fell back to live tracking, as hop indices from the region's first hop,
        // [first, end). A pass keeps its first c_maxStretches.
        static constexpr int c_maxStretches = 32;
        std::array<std::atomic<std::uint32_t>, c_maxStretches> stretchFirst {};
        std::array<std::atomic<std::uint32_t>, c_maxStretches> stretchEnd {};
        std::atomic<int> numStretches { 0 };

        // The same figures of the pass before the latest, kept when a pass starts (if the one ending compared any
        // hops): what a display shows of the part the latest pass has not reached yet, so that what no longer matches
        // does not vanish from view at every loop start.
        std::atomic<std::uint32_t> lastComparedHops { 0 };
        std::atomic<std::uint32_t> lastInvalidHops { 0 };
        std::array<std::atomic<std::uint32_t>, c_maxStretches> lastStretchFirst {};
        std::array<std::atomic<std::uint32_t>, c_maxStretches> lastStretchEnd {};
        std::atomic<int> lastNumStretches { 0 };
    };

    /** One learned stretch of the timeline. Immutable once published. */
    struct Region
    {
        std::uint32_t id = 0;              // given by LearnStore: unique within the instance and never reused, also
                                           // across state loads (not saved: a loaded region gets a new one)
        std::uint32_t learnOrder = 0;      // given by LearnStore: the id it was learned under, so the oldest can be
                                           // forgotten first. A part that a newer region cuts off keeps it (it was
                                           // learned no later than the rest). Saved as the order the regions were
                                           // learned in; a loaded region's is its new id

        // Where. The region covers hops [firstHop, firstHop + prints.size()) of the grid at learnedRate.
        double learnedRate = 48000.0;      // host sample rate while it was captured
        int hopSamples = 480;              // hopSamplesFor(learnedRate)
        std::int64_t firstHop = 0;         // the region starts at timeline sample firstHop * hopSamples
        std::vector<Fingerprint> prints;   // one per hop

        // The pitch curve of the line the analysis chose. frames[0] lies within two frames of the region start, the
        // last frame no more than four frames before its end (the analysis of a buffer that starts at the region
        // start gives exactly that), and there are at most durationSeconds() / c_frameSeconds + 5 of them.
        double firstFrameSeconds = 0.0;    // timeline time of frames[0]; frame i lies c_frameSeconds * i later
        std::vector<Frame> frames;
        std::vector<std::uint8_t> presence;  // one per frame: bit k set where candidate k is present
        std::vector<Candidate> candidates;   // strongest first

        Method requested = Method::Auto;   // what was asked for
        Method used = Method::Tonal;       // what made the curve: Engine or Tonal
        bool uncertain = false;            // the analysis could not be sure of the line: worth a look in the view

        float lineRatio = 1.0f;            // the line the sub follows, over the curve (1 = the analysis' own choice)
        float gapFillSeconds = static_cast<float>(c_defaultGapFillSeconds);  // the longest gap the curve bridges
        double alignmentSeconds = 0.0;     // how much later the learned audio now arrives on the timeline, as found
                                           // by aligning (an inserted latency, a nudged clip); 0 = where it was learned
        float silenceDbfs = c_unknownSilenceDbfs;  // the analysis' silence level: frames below it are flagged silent
                                                   // (the Threshold when it was learned, less c_silenceBelowThresholdDb)

        // Worked out by LearnStore when it accepts the region (learn::canonicalise()), not saved
        float lineLogRatio = 0.0f;         // log(lineRatio): the followed line is frames[i].logLineHz + this
        Quality quality;
        std::shared_ptr<RegionLive> live;  // never null in a published region (the pointee is written by the audio
                                           // thread, the region itself never changes)

        /** Learned before `other`: earlier in learnOrder, or (of the parts of one region) with the lower id. */
        bool learnedBefore(const Region& other) const noexcept
        {
            return learnOrder != other.learnOrder ? learnOrder < other.learnOrder : id < other.id;
        }

        std::int64_t numHops() const noexcept { return static_cast<std::int64_t>(prints.size()); }
        std::int64_t start() const noexcept { return firstHop * hopSamples; }
        std::int64_t end() const noexcept { return (firstHop + numHops()) * hopSamples; }

        double startSeconds() const noexcept { return static_cast<double>(start()) / learnedRate; }
        double endSeconds() const noexcept { return static_cast<double>(end()) / learnedRate; }
        double durationSeconds() const noexcept { return static_cast<double>(numHops() * hopSamples) / learnedRate; }

        double frameTime(std::size_t index) const noexcept
        {
            return firstFrameSeconds + c_frameSeconds * static_cast<double>(index);
        }

        // Where the sub follows it now: the learned span moved by the alignment.
        double followStartSeconds() const noexcept { return startSeconds() + alignmentSeconds; }
        double followEndSeconds() const noexcept { return endSeconds() + alignmentSeconds; }
        bool covers(double timelineSeconds) const noexcept
        {
            return timelineSeconds >= followStartSeconds() && timelineSeconds < followEndSeconds();
        }
    };

    /** Everything learned in one instance at one moment. Immutable once published.

        The audio thread reads it through the raw pointer LearnStore pins for it (LearnStore::beginAudioRead()) and
        must not copy the shared_ptrs: it never owns a reference, so nothing is ever freed on it. */
    struct Snapshot
    {
        std::vector<std::shared_ptr<const Region>> regions;  // sorted by start, learned spans never overlap
        bool follow = true;                                   // USE LEARNED: off = live tracking everywhere
        std::uint64_t revision = 0;                           // new with every published snapshot
        std::uint64_t regionsRevision = 0;                    // new whenever the regions change (what a state saves)

        bool empty() const noexcept { return regions.empty(); }

        /** The region the sub follows at this timeline time, if any. Where alignments make two neighbours overlap
            by a few milliseconds, the earlier one wins. At most 64 regions: a scan is cheap. */
        const Region* find(double timelineSeconds) const noexcept
        {
            for (const auto& region : regions)
                if (region->covers(timelineSeconds))
                    return region.get();

            return nullptr;
        }

        const Region* findId(std::uint32_t id) const noexcept
        {
            for (const auto& region : regions)
                if (region->id == id)
                    return region.get();

            return nullptr;
        }

        double learnedSeconds() const noexcept
        {
            double total = 0.0;

            for (const auto& region : regions)
                total += region->durationSeconds();

            return total;
        }
    };
} // namespace subbertone::learn
