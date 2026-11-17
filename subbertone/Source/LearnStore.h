#pragma once

#include "LearnTypes.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace juce
{
    class XmlElement;
}

/**
 * LearnStore: everything learned in one plug-in instance, published to the audio thread and saved with the session.
 *
 * Publishing. Every change builds a new immutable Snapshot and swaps it in through an atomic pointer. The audio
 * thread pins the current snapshot for one block with a hazard pointer: a single slot, as there is one audio thread
 * per instance. It never locks, allocates or frees. A writer that replaces a snapshot keeps it until the hazard no
 * longer names it and then frees it itself, on its own thread. Writers (the message thread, the worker, the host's
 * state thread) take a mutex the audio thread never touches.
 *
 * Every region is checked before it is published (learn::findProblem()), and rounded first to exactly what the
 * saved state holds (learn::canonicalise(): pitch in quarter cents, confidence in 1/255), so what plays before
 * saving is what plays after reopening, bit for bit.
 *
 * Saved state: one child of the plug-in's state XML,
 *     <LEARN version="1" follow="1" data="..."/>
 * where data is the regions as a little-endian binary blob (layout in LearnStore.cpp), zlib-compressed and in
 * base64. It is cached, so frequent state saves and host undo snapshots cost nothing until the regions change.
 * Loading checks sizes, ranges, order and a checksum, and refuses the whole blob on any doubt. A child it cannot
 * read in full, such as one a later version wrote, is not lost: it is saved again as it came until something is
 * learned, edited, forgotten or loaded here (see takeState()).
 */
namespace subbertone::learn
{
    //==============================================================================
    /** Rounds a region to what the saved state holds and works out what is derived from it (lineLogRatio, quality,
        and a fresh RegionLive if it has none). Values that cannot be saved (out of range, not a number) are left as
        they are, for findProblem() to report. */
    void canonicalise(Region& region);

    /** nullptr if the region may be published as it is (canonical and within every limit), else why not. The id is
        not checked here: a region gets it when it is published. */
    const char* findProblem(const Region& region) noexcept;

    /** The same for a complete set of regions: each one, unique non-zero ids, sorted, no overlaps, the limits on
        count and total length. */
    const char* findProblem(const std::vector<std::shared_ptr<const Region>>& regions) noexcept;

    /** The region's Quality figures, from its frames and prints. */
    Quality measure(const Region& region) noexcept;

    /** The saved blob, uncompressed. Ids are not saved, the order the regions were learned in is: decoded regions
        are numbered from `firstId` in that order (in timeline order from a blob saved before that order was), and
        each one's learnOrder is its id. decode returns false and leaves `regions` empty unless the blob is complete,
        exactly as encodeRegions() would write it, and what it holds passes findProblem(). Exposed for tests. */
    std::vector<std::uint8_t> encodeRegions(const std::vector<std::shared_ptr<const Region>>& regions);
    bool decodeRegions(const std::uint8_t* data, std::size_t size, std::vector<std::shared_ptr<const Region>>& regions,
                       std::uint32_t firstId = 1);

    /** The same compressed and in base64: the value of the data attribute. */
    std::string encodeStateData(const std::vector<std::shared_ptr<const Region>>& regions);
    bool decodeStateData(std::string_view text, std::vector<std::shared_ptr<const Region>>& regions,
                         std::uint32_t firstId = 1);

    //==============================================================================
    class LearnStore
    {
    public:
        using Clock = std::chrono::steady_clock;

        LearnStore();
        ~LearnStore();

        LearnStore(const LearnStore&) = delete;
        LearnStore& operator=(const LearnStore&) = delete;

        //==============================================================================
        // Audio thread. Pin the snapshot once per block and unpin it at the end; never nest, never keep the pointer
        // past endAudioRead(). Never null: an empty snapshot is published from the start.

        const Snapshot* beginAudioRead() noexcept
        {
            // Announce, then check that it is still current: a writer that swapped it out before the announcement
            // may already have freed it, one that swaps it out after will see the announcement and keep it.
            const Snapshot* snapshot = m_current.load(std::memory_order_acquire);

            for (;;)
            {
                m_hazard.store(snapshot, std::memory_order_seq_cst);
                const Snapshot* current = m_current.load(std::memory_order_seq_cst);

                if (current == snapshot)
                    return snapshot;

                snapshot = current;
            }
        }

        void endAudioRead() noexcept
        {
            m_hazard.store(nullptr, std::memory_order_release);
        }

        //==============================================================================
        // Any other thread.

        /** The current snapshot, counted: keep it as long as needed (the view, a worker job). */
        std::shared_ptr<const Snapshot> getSnapshot() const;

        /** Changes with every published snapshot: a cheap test for "anything new?" (lock-free). */
        std::uint64_t getRevision() const noexcept { return m_revision.load(std::memory_order_acquire); }

        /** Changes when everything learned is forgotten at once (clear()) or replaced by a loaded state. A capture
            notes it when it starts, and passes it to add(): a capture from before the change is then refused, so
            a job that was still analysing cannot bring back what was cleared or mix into another session. */
        std::uint64_t getEpoch() const noexcept { return m_epoch.load(std::memory_order_acquire); }

        struct AddResult
        {
            std::uint32_t id = 0;            // the new region's id; 0 if it was not added
            int shortened = 0;               // older regions cut back (or split) because the new one overlaps them
            int replaced = 0;                // older regions dropped: covered, or less than c_minRegionSeconds left
            int forgotten = 0;               // older regions forgotten to stay within the limits, oldest first
            const char* problem = nullptr;   // why it was not added
        };

        /** Publishes a newly learned region. It replaces whatever older regions it overlaps (they are cut back, or
            dropped when less than c_minRegionSeconds would remain), and the oldest regions are forgotten if that is
            what it takes to stay within c_maxRegions and c_maxLearnedSeconds: the first learned, what is left of one
            that was cut back counting as old as it was (Region::learnedBefore()). Refused if it fails findProblem()
            after canonicalise(), or if `epoch` is given and getEpoch() has moved on since. */
        AddResult add(Region region, std::optional<std::uint64_t> epoch = std::nullopt);

        /** Publishes an edited copy of a region under the same id: another line (lineRatio), new gap filling
            (frames, gapFillSeconds), an alignment. The edit runs on a copy, outside the store's lock, and may not
            move or resize the region (learnedRate, firstHop and the number of hops stay). If the region is replaced
            while the edit runs, the edit runs again on the newer version. Returns false if there is no such region
            (any more), the edit moved it, or the result fails findProblem(). */
        bool update(std::uint32_t id, const std::function<void(Region&)>& edit);

        /** Forget one region, or everything. What was forgotten can be brought back with undo() for c_undoSeconds.
            clear() also moves the epoch on (so captures and jobs in flight are refused), whether or not anything
            was learned, and forgets a LEARN child kept from a state this build could not read in full (see
            takeState(); undo() does not bring that back, as it was never on show). Both return false if no region
            was forgotten. */
        bool forget(std::uint32_t id, Clock::time_point now = Clock::now());
        bool clear(Clock::time_point now = Clock::now());

        /** Brings back what the last forget() or clear() removed, or what a state that left nothing learned replaced
            (see takeState()), if that was less than c_undoSeconds ago. Regions learned since then win where they
            overlap. False if nothing came back. */
        bool canUndo(Clock::time_point now = Clock::now()) const;
        bool undo(Clock::time_point now = Clock::now());

        /** USE LEARNED. Saved with the state, outside the regions' blob. */
        void setFollow(bool follow);

        /** Called after every change made through this interface (not after takeState()), on the thread that made it,
            once the store's lock is released: the processor passes it on to the host, so the session counts as
            changed. Set it before other threads use the store. */
        void setChangeCallback(std::function<void()> callback);

        /** Frees replaced snapshots the audio thread has finished with, and what was kept for undo() once that has
            expired. Every change does this too; call it from a timer so memory is returned while nothing changes. */
        void collectGarbage(Clock::time_point now = Clock::now());

        /** Replaced snapshots still held (diagnostics). */
        std::size_t getNumRetired() const;

        //==============================================================================
        // State (any thread but the audio thread).

        static constexpr const char* c_stateTag = "LEARN";
        static constexpr int c_stateVersion = 1;

        /** Adds <LEARN version="1" follow="..." data="..."/> to `state` (replacing any LEARN child it has). Written
            even when nothing is learned, so that loading the state also clears what an instance had learned.
            While the regions are still the ones published for a LEARN child this build could not read in full (see
            takeState()), that child is written instead, as it came, with USE LEARNED as it is now. */
        void writeState(juce::XmlElement& state) const;

        enum class StateResult
        {
            absent,     // no LEARN child: nothing changed (a state saved before Learn existed)
            loaded,     // the learned data was replaced by the state's
            unreadable  // a LEARN child that could not be read: the learned data was cleared (the child is kept)
        };

        /** Reads the LEARN child of `state` and removes it (so it never reaches the parameter tree). With one, the
            learned data is replaced even if the child holds no regions or cannot be read, and the epoch moves on.
            The loaded regions get new ids, in the order they were learned, so the oldest are still forgotten first.

            A state that leaves nothing learned (a host preset or default saved with nothing learned, or one that
            cannot be read) can be undone like clear(): undo() brings back what it replaced for c_undoSeconds. A
            state with regions of its own withdraws UNDO, so two sessions' regions are never mixed.

            A child this build cannot read in full (a later version's format or limits, a damaged one, or a readable
            one with attributes or child elements this build does not write) is kept, and writeState() writes it
            again as it came until the regions change (anything learned, edited, forgotten, brought back or loaded).
            Opening and saving a session here then keeps what a later version learned. What it holds beyond what
            this build reads is never used. */
        StateResult takeState(juce::XmlElement& state);

    private:
        struct StateCache;
        using RegionList = std::vector<std::shared_ptr<const Region>>;

        void publishLocked(RegionList regions, bool follow, bool regionsChanged);
        void reclaimLocked();
        void rememberForgottenLocked(RegionList forgotten, Clock::time_point now);
        void notifyChange();

        std::atomic<const Snapshot*> m_current { nullptr };
        std::atomic<const Snapshot*> m_hazard { nullptr };   // what the audio thread is reading, or null
        std::atomic<std::uint64_t> m_revision { 0 };
        std::atomic<std::uint64_t> m_epoch { 0 };

        mutable std::mutex m_lock;                            // writers only
        std::shared_ptr<const Snapshot> m_owner;              // keeps m_current alive
        std::vector<std::shared_ptr<const Snapshot>> m_retired;
        std::uint32_t m_nextId = 1;
        RegionList m_forgotten;                               // for undo()
        Clock::time_point m_undoDeadline {};
        std::function<void()> m_changeCallback;

        std::unique_ptr<StateCache> m_stateCache;             // the encoded data attribute and a kept child, own lock
    };
} // namespace subbertone::learn
