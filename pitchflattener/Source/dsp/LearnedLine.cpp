#include "LearnedLine.h"
#include "HindsightTracker.h"
#include "SourcePresets.h"
#include <algorithm>
#include <cmath>

namespace pf
{
    namespace
    {
        constexpr double maxAnalysisRate = 24000.0;   // FlattenerCore decimates to at most this
        constexpr double framesPerSecond = 1.0 / 0.005;
        constexpr int maxTakeFramesFor (double seconds) { return (int) (seconds * framesPerSecond * 1.02) + 8; }
        constexpr int maxTakeFrames = maxTakeFramesFor (LearnedLine::maxTakeSeconds + LearnedLine::preRollSeconds);
        constexpr int advisorSeconds = 20;            // the advisor hears at most this much of the take

        int nextPow2 (int v)
        {
            int p = 1;
            while (p < v)
                p <<= 1;
            return p;
        }

        float percentile (std::vector<float> v, float q)
        {
            if (v.empty())
                return 0.0f;
            const auto k = (size_t) std::clamp ((int) std::lround (q * (float) (v.size() - 1)), 0, (int) v.size() - 1);
            std::nth_element (v.begin(), v.begin() + (long) k, v.end());
            return v[k];
        }
    }

    class LearnedLine::Worker : public juce::Thread
    {
    public:
        explicit Worker (LearnedLine& l) : juce::Thread ("Pitch Flattener analyse"), owner (l) {}
        void run() override
        {
            while (! threadShouldExit())
            {
                const auto s = owner.getState();
                if (s == State::Analysing)
                    owner.runAnalysis();
                else if (s == State::Ready && ! owner.offline.load (std::memory_order_relaxed))
                    owner.runMatch();
                wait (s == State::Ready ? 50 : 20);
            }
        }
    private:
        LearnedLine& owner;
    };

    LearnedLine::LearnedLine()
    {
        // Sized for the highest analysis rate once, so nothing is reallocated while other threads read.
        printRing.assign ((size_t) printRingFrames, Print {});
        takeAudio.assign ((size_t) ((maxTakeSeconds + preRollSeconds + 0.1) * maxAnalysisRate), 0.0f);
        takePrints.assign ((size_t) maxTakeFrames, Print {});
        lineHz.assign ((size_t) maxTakeFrames, 0.0f);
        preRoll.assign ((size_t) nextPow2 ((int) ((preRollSeconds + 0.05) * maxAnalysisRate)), 0.0f);
    }

    LearnedLine::~LearnedLine()
    {
        release();
    }

    void LearnedLine::prepare (double analysisRate, int hopSamples)
    {
        release();

        rate = std::min (analysisRate, maxAnalysisRate);
        hop = std::max (1, hopSamples);
        // A learned take at another analysis rate is kept (it matches again once the rate is back) but does nothing
        // meanwhile. A take still waiting for its analysis needs the rate it was recorded at.
        if (getState() == State::Analysing && (std::abs (rate - takeRate) > 0.5 || hop != takeHop))
            state.store ((int) State::Failed, std::memory_order_release);

        // Fingerprint bands: broadband plus 12 half-octave band-passes from 100 Hz to 8 kHz.
        for (size_t i = 0; i < bands.size(); ++i)
        {
            const double fc = std::min (100.0 * std::pow (2.0, 0.5 * (double) i) * 1.12, 0.4 * rate);
            const double w0 = juce::MathConstants<double>::twoPi * fc / rate, alpha = std::sin (w0) / (2.0 * 2.0);
            const double a0 = 1.0 + alpha;
            auto& b = bands[i];
            b.b0 = alpha / a0; b.b1 = 0.0; b.b2 = -alpha / a0;
            b.a1 = -2.0 * std::cos (w0) / a0; b.a2 = (1.0 - alpha) / a0;
            b.z1 = b.z2 = 0.0;
        }

        reset();
        if (getState() == State::Armed || getState() == State::Recording)
            state.store ((int) State::Empty, std::memory_order_release);

        cancel.store (false);
        worker = std::make_unique<Worker> (*this);
        worker->startThread (juce::Thread::Priority::normal);
    }

    void LearnedLine::release()
    {
        if (worker != nullptr)
        {
            cancel.store (true);
            worker->stopThread (10000);
            worker.reset();
        }
        // An analysis cut short here stays Analysing: the worker of the next prepare runs it again.
    }

    //==============================================================================
    void LearnedLine::arm (const ProcessingSettings& current, int currentSource)
    {
        const std::lock_guard<std::mutex> lock (workLock);   // not while an analysis is reading the take
        armedSettings = current;
        armedSource = currentSource;
        takeGeneration.fetch_add (1);
        finishRequested.store (false);
        progress.store (0.0f);
        dropLock (0);
        state.store ((int) State::Armed, std::memory_order_release);
    }

    void LearnedLine::finish()
    {
        int expected = (int) State::Armed;
        if (state.compare_exchange_strong (expected, (int) State::Empty))
            return;
        if (expected == (int) State::Recording)
            finishRequested.store (true);
    }

    void LearnedLine::forget()
    {
        cancel.store (true);                 // a running analysis stops and, as its take is gone, publishes nothing
        const std::lock_guard<std::mutex> sl (suggestionLock);
        takeGeneration.fetch_add (1);
        finishRequested.store (false);
        dropLock (0);
        state.store ((int) State::Empty, std::memory_order_release);
        suggestionPending = false;
    }

    double LearnedLine::getTakeSeconds() const
    {
        return takeFrames.load() * (double) hop / rate;
    }

    bool LearnedLine::popSuggestion (Suggestion& s)
    {
        const std::lock_guard<std::mutex> lock (suggestionLock);
        if (! suggestionPending)
            return false;
        s = suggestion;
        suggestionPending = false;
        return true;
    }

    void LearnedLine::messageTick()
    {
        const auto s = getState();
        const int64_t prints = printsWritten.load (std::memory_order_acquire);
        if (s != State::Recording || prints != tickSeenPrints)
        {
            tickSeenPrints = prints;
            idleTicks = 0;
            return;
        }
        if (++idleTicks < 3)
            return;
        // No audio for a while: the host stopped. End the take here (if the audio thread hasn't already).
        const bool longEnough = (double) takeFrames.load() * hop / rate >= minTakeSeconds + preRollSeconds;
        int expected = (int) State::Recording;
        progress.store (0.0f);
        state.compare_exchange_strong (expected, (int) (longEnough ? State::Analysing : State::Failed));
        idleTicks = 0;
    }

    void LearnedLine::analyseNow()
    {
        if (getState() == State::Analysing)
            runAnalysis();
    }

    //==============================================================================
    void LearnedLine::reset()
    {
        samplesSinceReset = 0;
        frameIndex = 0;
        frameFill = 0;
        frameEnergy.fill (0.0);
        for (auto& b : bands) b.z1 = b.z2 = 0.0;
        printsWritten.store (0, std::memory_order_release);
        streamGeneration.fetch_add (1, std::memory_order_acq_rel);
        dropLock (0);
        // A take being recorded ends with the stream.
        if (getState() == State::Recording)
            finishRequested.store (true);
    }

    void LearnedLine::push (float x)
    {
        const auto preMask = (int64_t) preRoll.size() - 1;
        preRoll[(size_t) (samplesSinceReset & preMask)] = x;
        ++samplesSinceReset;

        if (getState() == State::Recording)
        {
            const int n = takeSamples.load (std::memory_order_relaxed);
            if (n < (int) takeAudio.size())
            {
                takeAudio[(size_t) n] = x;
                takeSamples.store (n + 1, std::memory_order_release);
            }
        }

        frameEnergy[0] += (double) x * x;
        for (size_t i = 0; i < bands.size(); ++i)
        {
            const float y = bands[i].process (x);
            frameEnergy[i + 1] += (double) y * y;
        }
        if (++frameFill >= hop)
            finishFrame();
    }

    void LearnedLine::finishFrame()
    {
        Print p;
        for (size_t i = 0; i < p.size(); ++i)
            p[i] = (float) (10.0 * std::log10 (frameEnergy[i] / hop + 1.0e-12));
        frameEnergy.fill (0.0);
        frameFill = 0;

        printRing[(size_t) (frameIndex & (printRingFrames - 1))] = p;
        printsWritten.store (frameIndex + 1, std::memory_order_release);

        const float gate = gateDb.load (std::memory_order_relaxed);
        const auto s = getState();
        if (s == State::Armed && p[0] > gate)
        {
            // The sound starts: keep it from a quarter second before, on the frame grid.
            const int preFrames = (int) std::lround (preRollSeconds * rate / hop);
            const int64_t startFrame = std::max<int64_t> (0, frameIndex - preFrames);
            const int64_t from = startFrame * hop, to = (frameIndex + 1) * hop;
            const auto preMask = (int64_t) preRoll.size() - 1;
            int n = 0;
            for (int64_t k = std::max<int64_t> (from, samplesSinceReset - (int64_t) preRoll.size()); k < to; ++k)
                takeAudio[(size_t) n++] = preRoll[(size_t) (k & preMask)];
            int f = 0;
            for (int64_t k = startFrame; k <= frameIndex; ++k)
                takePrints[(size_t) f++] = printRing[(size_t) (k & (printRingFrames - 1))];
            takeSamples.store (n, std::memory_order_release);
            takeFrames.store (f, std::memory_order_release);
            takeStartFrame = startFrame;
            takeRate = rate;
            takeHop = hop;
            quietFrames = 0;
            state.store ((int) State::Recording, std::memory_order_release);
        }
        else if (s == State::Recording)
        {
            int f = takeFrames.load (std::memory_order_relaxed);
            if (f < maxTakeFrames)
                takePrints[(size_t) f++] = p;
            takeFrames.store (f, std::memory_order_release);
            quietFrames = p[0] > gate ? 0 : quietFrames + 1;

            const bool full = (double) f * hop / rate >= maxTakeSeconds + preRollSeconds
                              || takeSamples.load (std::memory_order_relaxed) >= (int) takeAudio.size();
            if (full || finishRequested.load() || (double) quietFrames * hop / rate >= endQuietSeconds)
            {
                finishRequested.store (false);
                const bool longEnough = (double) f * hop / rate >= minTakeSeconds + preRollSeconds;
                progress.store (0.0f);
                state.store ((int) (longEnough ? State::Analysing : State::Failed), std::memory_order_release);
            }
        }

        ++frameIndex;
    }

    void LearnedLine::service()
    {
        if (! offline.load (std::memory_order_relaxed) || getState() != State::Ready)
            return;
        const int64_t n = printsWritten.load (std::memory_order_relaxed), last = lastMatchAt.load (std::memory_order_relaxed);
        if (streamGeneration.load (std::memory_order_relaxed) != matchGeneration.load (std::memory_order_relaxed)
            || n < last || n - last >= 20)
            runMatch();
    }

    void LearnedLine::dropLock (int64_t until)
    {
        prevOffset.store (until > 0 ? lockedOffset.load (std::memory_order_acquire) : notLocked, std::memory_order_release);
        prevUntil.store (until, std::memory_order_release);
        lockedOffset.store (notLocked, std::memory_order_release);
    }

    bool LearnedLine::lookup (int64_t frame, float& cents) const
    {
        if (getState() != State::Ready)
            return false;
        if (std::abs (rate - learnedRate) > 0.5 || hop != learnedHop)
            return false;
        int64_t offset = lockedOffset.load (std::memory_order_acquire);
        if (offset == notLocked || frame < lockFrom.load (std::memory_order_acquire))
        {
            // Frames from before the current lock began still belong to the lock it replaced, if any.
            offset = frame < prevUntil.load (std::memory_order_acquire) ? prevOffset.load (std::memory_order_acquire) : notLocked;
            if (offset == notLocked)
                return false;
        }
        const int64_t k = frame - offset;
        if (k < 0 || k >= lineFrames.load (std::memory_order_acquire))
            return false;
        const float hz = lineHz[(size_t) k];
        if (! (hz > 0.0f))
            return false;
        cents = hzToCents (hz);
        return true;
    }

    //==============================================================================
    float LearnedLine::matchError (const Print* incoming, int64_t takeStart) const
    {
        // Shapes, not levels: each window's values are clamped 50 dB under its loudest frame and taken relative to
        // its own mean, so a pass at another gain still matches and silence matches silence.
        float inMax = -200.0f, takeMax = -200.0f;
        for (int i = 0; i < matchFrames; ++i)
        {
            inMax = std::max (inMax, incoming[i][0]);
            takeMax = std::max (takeMax, takePrints[(size_t) (takeStart + i)][0]);
        }
        double inMean = 0.0, takeMean = 0.0;
        for (int i = 0; i < matchFrames; ++i)
            for (int b = 0; b < numBands; ++b)
            {
                inMean += std::max (incoming[i][(size_t) b], inMax - 50.0f);
                takeMean += std::max (takePrints[(size_t) (takeStart + i)][(size_t) b], takeMax - 50.0f);
            }
        inMean /= matchFrames * numBands;
        takeMean /= matchFrames * numBands;
        double err = 0.0;
        for (int i = 0; i < matchFrames; ++i)
            for (int b = 0; b < numBands; ++b)
            {
                const double a = std::max (incoming[i][(size_t) b], inMax - 50.0f) - inMean;
                const double t = std::max (takePrints[(size_t) (takeStart + i)][(size_t) b], takeMax - 50.0f) - takeMean;
                err += std::abs (a - t);
            }
        return (float) (err / (matchFrames * numBands));
    }

    void LearnedLine::runMatch()
    {
        std::unique_lock<std::mutex> lock (workLock, std::try_to_lock);
        if (! lock.owns_lock() || getState() != State::Ready)
            return;
        if (std::abs (rate - learnedRate) > 0.5 || hop != learnedHop)
            return;

        const uint32_t gen = streamGeneration.load (std::memory_order_acquire);
        if (gen != matchGeneration.load())
        {
            matchGeneration.store (gen);
            dropLock (0);
            missedChecks = 0;
            lastMatchAt.store (0);
            lastSearchAt = -1000000;
        }

        const int64_t n = printsWritten.load (std::memory_order_acquire);
        if (n == lastMatchAt.load())
            return;   // nothing new since last time
        lastMatchAt.store (n);
        const int frames = takeFrames.load (std::memory_order_acquire);
        if (n < matchFrames || frames < matchFrames)
            return;

        std::array<Print, matchFrames> window;
        const int64_t first = n - matchFrames;
        for (int i = 0; i < matchFrames; ++i)
            window[(size_t) i] = printRing[(size_t) ((first + i) & (printRingFrames - 1))];
        if (printsWritten.load (std::memory_order_acquire) - first > printRingFrames
            || streamGeneration.load (std::memory_order_acquire) != gen)
            return;

        // Only judge windows with enough sound in them.
        const float gate = gateDb.load (std::memory_order_relaxed);
        int loud = 0;
        for (const auto& p : window)
            loud += p[0] > gate ? 1 : 0;
        if (loud < matchFrames / 2)
            return;

        const int64_t offset = lockedOffset.load (std::memory_order_acquire);
        if (offset != notLocked)
        {
            const int64_t k = first - offset;
            if (k < 0 || k + matchFrames > frames)
            {
                // Past the end of the take: let go, so a repeat of the sound can be found again. What was matched so far
                // keeps its line.
                dropLock (n);
                return;
            }
            if (matchError (window.data(), k) < 1.5f)
                missedChecks = 0;
            else
            {
                if (missedChecks++ == 0)
                    mismatchFrom = first + matchFrames / 2;
                if (missedChecks >= 3)
                    dropLock (mismatchFrom);   // frames before the mismatch keep the old lock
            }
            return;
        }

        // The full search costs a few ms on a long take: at most every 20 frames.
        if (n - lastSearchAt < 20)
            return;
        lastSearchAt = n;

        // Coarse search on every other frame, then refine around the best few.
        struct Cand { float err; int64_t k; };
        std::array<Cand, 4> best;
        best.fill ({ 1.0e9f, -1 });
        for (int64_t k = 0; k + matchFrames <= frames; k += 2)
        {
            const float e = matchError (window.data(), k);
            if (e < best.back().err)
            {
                best.back() = { e, k };
                std::sort (best.begin(), best.end(), [] (const Cand& a, const Cand& b) { return a.err < b.err; });
            }
        }
        Cand winner { 1.0e9f, -1 };
        for (const auto& c : best)
            for (int64_t k = std::max<int64_t> (0, c.k - 2); c.k >= 0 && k <= std::min<int64_t> (frames - matchFrames, c.k + 2); ++k)
            {
                const float e = matchError (window.data(), k);
                if (e < winner.err)
                    winner = { e, k };
            }
        // The same audio matches within a few tenths of a dB; a different sound of the same kind measured 1.5 dB and up.
        // A sound that repeats (a siren's cycles) may match more than one place: any of them carries the same line.
        if (winner.k >= 0 && winner.err < 0.8f)
        {
            missedChecks = 0;
            lockFrom.store (first, std::memory_order_release);
            lockedOffset.store (first - winner.k, std::memory_order_release);
        }
    }

    //==============================================================================
    void LearnedLine::runAnalysis()
    {
        const std::lock_guard<std::mutex> lock (workLock);
        if (getState() != State::Analysing)
            return;
        // A forget, a loaded state or a new take while this runs makes the result stale: it is then not published.
        const uint32_t gen = takeGeneration.load();
        cancel.store (false);
        auto fail = [this, gen]
        {
            const std::lock_guard<std::mutex> sl (suggestionLock);
            int expected = (int) State::Analysing;
            if (takeGeneration.load() == gen)
                state.compare_exchange_strong (expected, (int) State::Failed);
        };
        const double rate = takeRate;   // the take's own rate and hop (prepare() fails the take if they changed)
        const int hop = takeHop;

        const int n = takeSamples.load (std::memory_order_acquire);
        const int frames = std::min (takeFrames.load (std::memory_order_acquire), maxTakeFrames);
        progress.store (0.05f);

        // 1. Which Source Type flattens the take best.
        const int advisorSamples = std::min (n, (int) (advisorSeconds * rate));
        const auto advice = adviseSourceType (takeAudio.data(), advisorSamples, rate, armedSettings, armedSource, &cancel);
        if (cancel.load())
            return;
        progress.store (0.6f);
        int type = advice.recommended >= 0 ? advice.recommended : armedSource;
        if (type < 0 || type >= (int) SourceType::NumSourceTypes)
            type = (int) SourceType::Custom;

        // 2. The take's pitch line, in hindsight, over that Source Type's range.
        const bool custom = type == (int) SourceType::Custom;
        const auto& preset = getSourcePreset ((SourceType) type);
        hindsight::Settings hs;
        hs.method = hindsight::Method::Auto;
        hs.lowestHz = std::max (20.0, (double) (custom ? armedSettings.minHz : preset.minHz));
        hs.highestHz = std::max (hs.lowestHz * 2.0, (double) (custom ? armedSettings.maxHz : preset.maxHz));
        hs.hopSeconds = (double) hop / rate;
        hs.silenceDbfs = std::min (-40.0, (double) gateDb.load() - 10.0);
        hindsight::Analysis a;
        try
        {
            a = hindsight::analyse (takeAudio.data(), n, rate, hs, &cancel);
        }
        catch (...)
        {
            fail();
            return;
        }
        if (a.cancelled)
            return;
        progress.store (0.9f);

        const int lineCount = std::min (frames, (int) a.line.frames.size());
        std::vector<float> pitched;
        pitched.reserve ((size_t) lineCount);
        for (int i = 0; i < lineCount; ++i)
        {
            const float hz = a.line.frames[(size_t) i].hz;
            lineHz[(size_t) i] = hz;
            if (hz > 0.0f)
                pitched.push_back (hz);
        }
        if ((int) pitched.size() < std::max (20, lineCount / 10))
        {
            fail();
            return;
        }

        // 3. Settings from the line, as far as it can be trusted. A line resting on the edge of the search range, or
        // one that jumps about, is no guide: then the Source Type is still suggested, but the range, Max Shift and
        // the line itself are not used.
        const float lo = percentile (pitched, 0.10f), hi = percentile (pitched, 0.90f);
        const float span = 12.0f * std::log2 (hi / std::max (lo, 1.0f));
        int jumps = 0, pairs = 0;
        double st = 0.0, sy = 0.0, stt = 0.0, syy = 0.0, sty = 0.0;
        for (int i = 0; i < lineCount; ++i)
        {
            const float hz = lineHz[(size_t) i];
            if (! (hz > 0.0f))
                continue;
            const double y = std::log2 ((double) hz), t = (double) i;
            st += t; sy += y; stt += t * t; syy += y * y; sty += t * y;
            if (i > 0 && lineHz[(size_t) i - 1] > 0.0f)
            {
                ++pairs;
                if (std::abs (12.0 * std::log2 ((double) hz / lineHz[(size_t) i - 1])) > 1.0)
                    ++jumps;
            }
        }
        const double np = (double) pitched.size();
        const double cov = sty - st * sy / np, vt = stt - st * st / np, vy = syy - sy * sy / np;
        const double trend = vt > 0.0 && vy > 0.0 ? cov / std::sqrt (vt * vy) : 0.0;   // -1..1: falls .. climbs
        const bool onEdge = lo < hs.lowestHz * 1.15 || hi > hs.highestHz / 1.15;
        const bool steady = pairs > 0 && jumps <= std::max (2, pairs / 100);
        const bool trusted = ! onEdge && steady;

        const bool harmonic = ! custom && preset.detector == DetectorType::HarmonicSum;
        if (harmonic && trusted && span > 12.0f && std::abs (trend) > 0.85)
            type = (int) SourceType::EngineRunup;   // an engine climbing (or falling) steadily over an octave
        const auto& chosen = getSourcePreset ((SourceType) type);
        const bool customNow = type == (int) SourceType::Custom;
        const float presetMin = customNow ? armedSettings.minHz : chosen.minHz;
        const float presetMax = customNow ? armedSettings.maxHz : chosen.maxHz;
        const float presetShift = customNow ? armedSettings.live.maxShiftSemitones : chosen.maxShiftSemitones;

        Suggestion s;
        s.valid = true;
        s.sourceType = type;
        s.lineLowHz = lo;
        s.lineHighHz = hi;
        s.lineTrusted = trusted;
        // The range only ever widens, to make room for the line: narrowing it measured worse (a detector held off the
        // low end of a noisy recording jumps about more, not less).
        s.minHz = trusted ? std::clamp (std::min (presetMin, lo / 1.5f), 20.0f, 2000.0f) : presetMin;
        s.maxHz = trusted ? std::clamp (std::max (presetMax, hi * 1.5f), std::max (100.0f, s.minHz * 2.0f), 8000.0f) : presetMax;
        // Max Shift stays the Source Type's: raising it to the line's span measured worse on pass-bys and ambiences (a
        // tracking slip then becomes a bigger jump). A run-up gets its room from Engine Run-up's 36 semitones.
        s.maxShiftSemitones = presetShift;
        s.shiftBelowHz = customNow ? armedSettings.live.shiftBelowHz : chosen.shiftBelowHz;
        if (! trusted)
            std::fill (lineHz.begin(), lineHz.begin() + lineCount, 0.0f);   // the take is still matched, but guides nothing

        const std::lock_guard<std::mutex> sl (suggestionLock);
        if (takeGeneration.load() != gen || cancel.load())
            return;
        learnedRate = rate;
        learnedHop = hop;
        lineFrames.store (lineCount, std::memory_order_release);
        dropLock (0);
        suggestion = s;
        suggestionPending = true;
        progress.store (1.0f);
        int expected = (int) State::Analysing;
        state.compare_exchange_strong (expected, (int) State::Ready);
    }

    //==============================================================================
    juce::String LearnedLine::saveState() const
    {
        if (getState() != State::Ready)
            return {};
        const int frames = std::min (lineFrames.load(), takeFrames.load());

        juce::MemoryOutputStream raw;
        raw.writeInt (2);                       // format version (2: 13 bands)
        raw.writeDouble (learnedRate);
        raw.writeInt (learnedHop);
        raw.writeInt (frames);
        for (int i = 0; i < frames; ++i)
            raw.writeFloat (lineHz[(size_t) i]);
        for (int i = 0; i < frames; ++i)
            for (int b = 0; b < numBands; ++b)
                raw.writeShort ((short) std::clamp ((int) std::lround (takePrints[(size_t) i][(size_t) b] * 100.0f), -32000, 32000));

        juce::MemoryOutputStream packed;
        {
            juce::GZIPCompressorOutputStream gz (packed, 9);
            gz.write (raw.getData(), raw.getDataSize());
        }
        return packed.getMemoryBlock().toBase64Encoding();
    }

    void LearnedLine::loadState (const juce::String& text)
    {
        juce::MemoryBlock packed;
        if (text.isEmpty() || ! packed.fromBase64Encoding (text))
            return;
        juce::MemoryInputStream packedIn (packed, false);
        juce::GZIPDecompressorInputStream gz (packedIn);
        juce::MemoryBlock rawBlock;
        gz.readIntoMemoryBlock (rawBlock);
        juce::MemoryInputStream in (rawBlock, false);

        if (in.readInt() != 2)
            return;
        const double r = in.readDouble();
        const int h = in.readInt();
        const int frames = in.readInt();
        if (! (r >= 4000.0 && r <= maxAnalysisRate + 1.0) || h < 1 || frames < matchFrames || frames > maxTakeFrames
            || in.getNumBytesRemaining() < (juce::int64) frames * (4 + 2 * numBands))
            return;

        // Stop any analysis or match first, then write the arrays with nothing else reading them.
        cancel.store (true);
        const std::lock_guard<std::mutex> lock (workLock);
        const std::lock_guard<std::mutex> sl (suggestionLock);
        takeGeneration.fetch_add (1);
        suggestionPending = false;
        state.store ((int) State::Empty, std::memory_order_release);
        dropLock (0);
        for (int i = 0; i < frames; ++i)
        {
            const float hz = in.readFloat();
            lineHz[(size_t) i] = std::isfinite (hz) && hz >= 0.0f && hz < 20000.0f ? hz : 0.0f;
        }
        for (int i = 0; i < frames; ++i)
            for (int b = 0; b < numBands; ++b)
                takePrints[(size_t) i][(size_t) b] = (float) in.readShort() / 100.0f;
        learnedRate = r;
        learnedHop = h;
        takeFrames.store (frames, std::memory_order_release);
        lineFrames.store (frames, std::memory_order_release);
        state.store ((int) State::Ready, std::memory_order_release);
    }
}
