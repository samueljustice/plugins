#pragma once

// Learned sounds, findable by their id across every instance of the plug-in in this process.
//
// A host may render with an instance other than the one the user learned with (a copy made from
// its saved state, a separate render instance). The state carries the learned sound's id, so an
// instance given that state can take the sound, its reading and (when its options are the same)
// its render from here at once, instead of decoding and analysing the copy the state also carries.
//
// Entries are weak: a sound lives as long as some instance uses it. When an instance lets go of
// one (Clear, another sound learned, the instance deleted), the last kKeep of them are held a
// further kKeepSeconds, so a render instance made just after the instance that learned the sound
// is deleted still finds it.
//
// Never used on the audio thread (it locks and may free). No JUCE.

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pitchflattener
{

struct Learned;
struct Result;

class SoundRegistry
{
public:
    static constexpr std::size_t kKeep = 4;
    static constexpr double kKeepSeconds = 600.0;

    static SoundRegistry& instance();

    /** An instance published a render of a learned sound (both shared, never modified). */
    void publish(const std::shared_ptr<const Learned>& learned, const std::shared_ptr<const Result>& render);
    /** An instance no longer uses this sound: hold it a while for anyone who may still ask. */
    void letGo(const std::shared_ptr<const Learned>& learned, const std::shared_ptr<const Result>& render);

    struct Found
    {
        std::shared_ptr<const Learned> learned;
        std::vector<std::shared_ptr<const Result>> renders;   // its renders still about, newest first
    };
    Found find(std::uint64_t id);

    /** Tests: forget everything (as a fresh process would have nothing). */
    void clearForTesting();

private:
    static constexpr std::size_t kRendersPerSound = 4;
    struct Entry
    {
        std::weak_ptr<const Learned> learned;
        std::vector<std::weak_ptr<const Result>> renders;   // newest first
    };
    struct Kept
    {
        std::shared_ptr<const Learned> learned;
        std::shared_ptr<const Result> render;
        std::chrono::steady_clock::time_point until;
    };

    void pruneLocked(std::deque<Kept>& expired);

    std::mutex m_mutex;
    std::unordered_map<std::uint64_t, Entry> m_entries;
    std::deque<Kept> m_kept;   // newest first
};

} // namespace pitchflattener
