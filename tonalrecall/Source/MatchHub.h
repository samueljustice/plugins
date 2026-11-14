#pragma once

// The process-wide analysis state. Like the capture store it outlives any one plug-in instance:
// whichever instance's editor is open shows the same analysis, and whichever instance renders
// stamps the same shift.
//
// A background thread watches the capture store. When a slot finishes a pass with audio, it copies
// that audio and analyses it (music or effect); when a slot is cleared, its analysis goes. Any
// change to the analyses or to the user's options recomputes the match and publishes a new,
// immutable stamp plan.
//
// Threading:
//   - Editors and the processor's state calls use options()/setOptions()/snapshot() (a mutex;
//     never the audio thread).
//   - The audio thread uses only planGeneration(), tryAcquirePlan() and stampEnabled(): atomics
//     plus a spin try-lock around a shared_ptr copy, which neither allocates nor blocks (see
//     tryAcquirePlan()). A plan is never freed on the audio thread: the hub keeps every plan it
//     published until it sees that nobody else holds it, and frees it on its own thread.
//
// No JUCE in here, so it can be driven from tests directly.

#include "CaptureStore.h"
#include "core/Analysis.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace tonalrecall
{

struct MatchOptions
{
    // A new instance starts Locked: one fixed shift, the smallest move onto a strong note of the
    // music, and no movement. It is the smoothest answer and never sticks out; Subtle and Moving
    // are a click away on the effect's scan.
    core::Target target = core::Target::Nearest;
    int pitchClass = 0;                 // for Target::PitchClass
    int octaveShift = 0;                // -2..+2
    // -1..+1: left flattens the effect's own drift, right takes on the music's movement.
    double bend = 0.0;
    // 0..1: how far the curve is smoothed: note steps (0) to ramps between notes (1). Full by
    // default: when an effect does move under music, it should move gently, not play the tune.
    double glide = 1.0;
    core::SfxMethod method = core::SfxMethod::Auto;
    bool preserveFormants = false;
    bool stampEnabled = true;
    // The single note in use before Subtle or Moving took over, so Locked can go back to it.
    core::Target lockedTarget = core::Target::Nearest;
    int lockedPitchClass = 0;
    // Hand adjustments to the shift along the effect (see core::MatchOptions::edits). They belong
    // to the learned effect: learning another clears them, and they are not saved with the
    // session (the learned audio they were drawn on is not saved either).
    std::vector<core::CurvePoint> edits;

    static constexpr std::size_t kMaxEdits = 512;   // room for freehand lines (about 240 points for one drawn right across)

    // Three ready-made ways for the effect to sit on the music, offered side by side:
    //   Locked  one fixed shift onto a note, no movement
    //   Subtle  rests on the nearest sounding note and takes on a little of the music's movement
    //   Moving  the same, taking on most of the music's movement
    enum class Style : int { Locked = 0, Subtle = 1, Moving = 2 };
    static constexpr int kNumStyles = 3;
    /** These options with a style applied (everything the style does not touch is kept). */
    MatchOptions withStyle(Style style) const noexcept;
    /** The style these options amount to, or -1 if they have been set by hand to something else. */
    int style() const noexcept;

    bool operator==(const MatchOptions& other) const noexcept;
    // True when the shift moves over the effect, so Stamp has to find the learned clip to lay it.
    bool bendActive() const noexcept { return bend > 1.0e-6 || bend < -1.0e-6 || ! edits.empty(); }
    MatchOptions sanitised() const noexcept;
};

// Everything Stamp needs, frozen when it was published. Shared, never modified.
struct StampPlan
{
    std::uint64_t generation = 0;
    double sampleRate = 0.0;            // the rate the effect was learned at: valid only there
    double staticSemitones = 0.0;
    double curveHopSeconds = 0.005;
    std::vector<float> curve;
    // What Learn captured (the channel sum). Shared with the hub and never modified, so plans that
    // differ only in their options do not each copy the audio.
    std::shared_ptr<const std::vector<float>> learnedMono;
    bool preserveFormants = false;
    bool bendActive = false;            // the curve carries a bend (not only the fixed shift)
    double targetMidi = 0.0;
};

// A copy of the hub's state for an editor or a test.
struct HubSnapshot
{
    std::uint64_t generation = 0;
    MatchOptions options;

    bool musicBusy = false, sfxBusy = false;
    bool hasMusic = false, hasSfx = false;     // an analysis exists (valid or not)
    core::MusicAnalysis music;
    core::SfxAnalysis sfx;
    core::MatchResult match;

    double musicRate = 0.0, sfxRate = 0.0;
    bool offsetKnown = false;
    double sfxOffsetInMusicSeconds = 0.0;
    bool planReady = false;

    // Where each style would put the effect's pitch over its length (core::MatchResult::afterMidi
    // for that style), so the editor can draw the ones not chosen and let the user pick. Empty
    // without a match.
    std::array<std::vector<float>, MatchOptions::kNumStyles> styleAfterMidi;

    // The learned effect itself, channel by channel, for auditioning. Shared, never modified.
    std::shared_ptr<const std::vector<std::vector<float>>> sfxChannels;
};

class MatchHub
{
public:
    static MatchHub& instance();

    // Each processor retains the hub for its lifetime; the analysis thread runs while any does.
    void retain();
    void release();

    MatchOptions options() const;
    void setOptions(const MatchOptions& options);

    // Bumped on every change an editor would show.
    std::uint64_t generation() const noexcept { return m_generation.load(std::memory_order_acquire); }
    void snapshot(HubSnapshot& out) const;

    // ---- audio thread ----
    bool stampEnabled() const noexcept { return m_stampEnabled.load(std::memory_order_acquire); }
    // 0 = no plan.
    std::uint64_t planGeneration() const noexcept { return m_planGeneration.load(std::memory_order_acquire); }
    // Copies the current plan into out (a reference count increment: no allocation). False if the
    // hub is publishing a plan at this very moment; the caller keeps what it had and tries again
    // later. out may end up null (no plan).
    bool tryAcquirePlan(std::shared_ptr<const StampPlan>& out) const noexcept;

    // Tests: wait until the hub has caught up with the store and the options (false on timeout).
    bool waitUntilIdle(double timeoutSeconds);
    // Tests: forget every analysis, the plan and the options (after CaptureStore::resetForTesting()).
    void resetForTesting();

private:
    MatchHub();
    ~MatchHub();

    struct SpinLock
    {
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
        bool tryLock() noexcept { return ! flag.test_and_set(std::memory_order_acquire); }
        void lock() noexcept;
        void unlock() noexcept { flag.clear(std::memory_order_release); }
    };

    struct SlotState
    {
        std::uint32_t seenGeneration = 0xffffffffu;
        std::uint64_t audioHash = 0;            // of the audio last analysed (0 = none)
        bool pending = false;                   // the store has audio that is not analysed yet
    };
    std::uint64_t m_editsBelongTo = 0;      // hash of the effect audio the hand adjustments were made on


    void threadLoop();
    bool workOnce();                            // true if it did something
    void recomputeMatchLocked();                // m_mutex held
    void publishPlanLocked(std::shared_ptr<const StampPlan> plan);   // m_mutex held
    void freeUnusedPlansLocked();                                    // m_mutex held
    void bump() noexcept { m_generation.fetch_add(1, std::memory_order_acq_rel); }

    // Guarded by m_mutex.
    mutable std::mutex m_mutex;
    MatchOptions m_options;
    core::MusicAnalysis m_music;
    core::SfxAnalysis m_sfx;
    core::MatchResult m_match;
    std::array<std::vector<float>, MatchOptions::kNumStyles> m_styleAfterMidi;
    bool m_hasMusic = false, m_hasSfx = false;
    bool m_musicBusy = false, m_sfxBusy = false;
    double m_musicRate = 0.0, m_sfxRate = 0.0;
    bool m_musicTimeline = false, m_sfxTimeline = false;
    std::int64_t m_musicStart = 0, m_sfxStart = 0;
    std::shared_ptr<const std::vector<std::vector<float>>> m_sfxChannels;   // the same audio, channel by channel
    std::shared_ptr<const std::vector<float>> m_sfxAudio;   // what the effect analysis ran on
    bool m_sfxDirty = false;                // re-analyse the effect (method changed)
    bool m_matchDirty = false;              // recompute the match and the plan
    std::uint64_t m_epoch = 0;              // bumped by resetForTesting: stale work is dropped

    std::array<SlotState, kNumSlots> m_slots;
    std::vector<std::shared_ptr<const StampPlan>> m_plans;   // every plan published and maybe still held

    // The plan the audio thread picks up.
    mutable SpinLock m_planLock;
    std::shared_ptr<const StampPlan> m_plan;                  // guarded by m_planLock
    std::atomic<std::uint64_t> m_planGeneration { 0 };
    std::uint64_t m_nextPlanGeneration = 1;

    std::atomic<std::uint64_t> m_generation { 1 };
    std::atomic<bool> m_stampEnabled { true };
    std::atomic<std::uint64_t> m_idlePasses { 0 };            // passes of the thread that found nothing to do

    std::mutex m_threadMutex;
    std::condition_variable m_wake;
    bool m_stop = false;
    bool m_kick = false;
    int m_refs = 0;
    std::thread m_thread;
};

} // namespace tonalrecall
