#include "HindsightGuide.h"
#include "HindsightTracker.h"
#include "FlattenerTypes.h"
#include <algorithm>
#include <cmath>

namespace pf
{
    namespace
    {
        int64_t nextPow2 (int64_t v)
        {
            int64_t p = 1;
            while (p < v)
                p <<= 1;
            return p;
        }
    }

    class HindsightGuide::Worker : public juce::Thread
    {
    public:
        explicit Worker (HindsightGuide& g) : juce::Thread ("Pitch Flattener hindsight"), guide (g) {}
        void run() override
        {
            while (! threadShouldExit())
            {
                if (! guide.offline.load (std::memory_order_relaxed))
                    guide.runDueWindow();
                wait (20);
            }
        }
    private:
        HindsightGuide& guide;
    };

    HindsightGuide::HindsightGuide() = default;

    HindsightGuide::~HindsightGuide()
    {
        release();
    }

    void HindsightGuide::prepare (double analysisRate, int hopSamples)
    {
        release();

        rate = analysisRate;
        hop = std::max (1, hopSamples);
        windowSamples = (int) std::ceil ((historySeconds + aheadSeconds) * rate);
        stepSamples = (int) std::lround (stepSeconds * rate);

        // room for a whole window plus the audio that arrives while it is copied
        ring.assign ((size_t) nextPow2 (windowSamples + (int64_t) (2.0 * rate)), 0.0f);
        ringMask = (int64_t) ring.size() - 1;
        slots = std::vector<Slot> ((size_t) nextPow2 (2 * (windowSamples / hop + 2)));
        slotMask = (int64_t) slots.size() - 1;
        window.assign ((size_t) (windowSamples + hop), 0.0f);   // the start snaps down to a frame boundary

        reset();
        cancel.store (false);
        worker = std::make_unique<Worker> (*this);
        worker->startThread (juce::Thread::Priority::normal);   // "low" is background QoS on macOS: far too slow to keep up
    }

    void HindsightGuide::release()
    {
        if (worker != nullptr)
        {
            cancel.store (true);
            worker->stopThread (10000);
            worker.reset();
        }
    }

    void HindsightGuide::reset()
    {
        // Slots and windows from before this point carry the old generation, so they no longer match. The counters are
        // reset first, so a worker that sees the new generation also sees them at zero.
        written.store (0, std::memory_order_release);
        used.store (-1, std::memory_order_release);
        generation.fetch_add (1, std::memory_order_acq_rel);
    }

    void HindsightGuide::setActive (bool on, float lowestHz, float highestHz)
    {
        active.store (on, std::memory_order_relaxed);
        loHz.store (lowestHz, std::memory_order_relaxed);
        hiHz.store (highestHz, std::memory_order_relaxed);
    }

    void HindsightGuide::push (float x)
    {
        if (ring.empty())
            return;
        const int64_t n = written.load (std::memory_order_relaxed);
        ring[(size_t) (n & ringMask)] = x;
        written.store (n + 1, std::memory_order_release);
    }

    void HindsightGuide::service()
    {
        if (offline.load (std::memory_order_relaxed))
            runDueWindow();
    }

    bool HindsightGuide::lookup (int64_t frame, float& cents) const
    {
        if (frame < 0 || slots.empty() || ! active.load (std::memory_order_relaxed))
            return false;
        const auto& s = slots[(size_t) (frame & slotMask)];
        const uint64_t want = makeTag (generation.load (std::memory_order_acquire), frame);
        if (s.tag.load (std::memory_order_acquire) != want)
            return false;
        const float hz = s.hz.load (std::memory_order_relaxed);
        if (! (hz > 0.0f) || s.tag.load (std::memory_order_acquire) != want)
            return false;
        cents = hzToCents (hz);
        return true;
    }

    void HindsightGuide::runDueWindow()
    {
        if (ring.empty() || ! active.load (std::memory_order_relaxed))
            return;

        std::unique_lock<std::mutex> lock (windowLock, std::try_to_lock);
        if (! lock.owns_lock())
            return;

        const uint32_t gen = generation.load (std::memory_order_acquire);
        if (gen != windowGen)
        {
            windowGen = gen;
            lastWindowEnd = 0;
        }

        const int64_t end = written.load (std::memory_order_acquire);
        if (end - lastWindowEnd < stepSamples)
            return;

        // Start on a frame boundary, so window frame i is plugin frame firstFrame + i exactly.
        int64_t start = std::max<int64_t> (0, end - windowSamples);
        start -= start % hop;
        const int n = (int) (end - start);
        for (int i = 0; i < n; ++i)
            window[(size_t) i] = ring[(size_t) ((start + i) & ringMask)];

        // A reset, or the audio overtaking what was being copied, makes the copy useless.
        if (generation.load (std::memory_order_acquire) != gen
            || written.load (std::memory_order_acquire) - start > (int64_t) ring.size())
            return;
        lastWindowEnd = end;

        hindsight::Settings s;
        s.method = hindsight::Method::Auto;
        s.lowestHz = std::max (20.0f, loHz.load (std::memory_order_relaxed));
        s.highestHz = std::max ((float) s.lowestHz * 2.0f, hiHz.load (std::memory_order_relaxed));
        s.hopSeconds = (double) hop / rate;
        s.silenceDbfs = -60.0;
        s.autoCrossCheckShare = 0.8;   // stricter than the tracker's default: tonal pads and synths stay with the own track

        hindsight::Analysis a;
        try
        {
            a = hindsight::analyse (window.data(), n, rate, s, &cancel);
        }
        catch (...)   // std::bad_alloc: no guide for this window
        {
            return;
        }
        if (a.cancelled)
            return;
        windowsAnalysed.fetch_add (1);

        const bool guided = a.usedMethod == hindsight::Method::Engine && a.engineOnGrid;
        const int64_t firstFrame = start / hop;
        const int64_t lastFrame = firstFrame + (int64_t) a.line.frames.size() - 1;

        // Of the window's lines, the one that best continues what has been published already.
        const hindsight::Line* best = &a.line;
        if (guided)
        {
            windowsGuided.fetch_add (1);
            double bestErr = 1.0e30;
            auto consider = [&] (const hindsight::Line& line)
            {
                double err = 0.0;
                int count = 0;
                for (int64_t f = firstFrame; f <= lastFrame; ++f)
                {
                    const auto& slot = slots[(size_t) (f & slotMask)];
                    if (slot.tag.load (std::memory_order_acquire) != makeTag (gen, f))
                        continue;
                    const float prev = slot.hz.load (std::memory_order_relaxed);
                    const float h = line.frames[(size_t) (f - firstFrame)].hz;
                    if (prev > 0.0f && h > 0.0f)
                    {
                        err += std::abs (1200.0 * std::log2 ((double) h / prev));
                        ++count;
                    }
                }
                // with nothing to continue, the window's own choice
                const double e = count > 0 ? err / count : (&line == &a.line ? 0.0 : 1.0e20);
                if (e < bestErr)
                {
                    bestErr = e;
                    best = &line;
                }
            };
            consider (a.line);
            for (const auto& p : a.parallel)
                if (p.frames.size() == a.line.frames.size())
                    consider (p);
        }

        // Publish every frame the plugin has not used yet.
        const int64_t from = std::max (firstFrame, used.load (std::memory_order_acquire) + 1);
        for (int64_t f = from; f <= lastFrame; ++f)
        {
            auto& slot = slots[(size_t) (f & slotMask)];
            const float hz = guided ? best->frames[(size_t) (f - firstFrame)].hz : 0.0f;
            const uint64_t tag = makeTag (gen, f);
            if (slot.tag.load (std::memory_order_acquire) == tag)
            {
                // Refining a frame already published: old and new values are both valid to read.
                slot.hz.store (hz, std::memory_order_release);
                continue;
            }
            slot.tag.store (0, std::memory_order_release);
            slot.hz.store (hz, std::memory_order_relaxed);
            slot.tag.store (tag, std::memory_order_release);
        }
    }
}
