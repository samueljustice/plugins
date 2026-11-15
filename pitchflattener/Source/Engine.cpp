#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace pitchflattener
{
namespace
{
constexpr float kSilence = Locator::kSilence;
constexpr std::int64_t kNever = INT64_MAX;
constexpr auto kHousePeriod = std::chrono::milliseconds(5);
constexpr auto kIdlePeriod = std::chrono::milliseconds(50);

std::int64_t secondsToSamples(double seconds, double rate) noexcept
{
    return static_cast<std::int64_t>(std::llround(seconds * rate));
}

double nowSeconds() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

bool sameOptions(const core::Options& a, const core::Options& b) noexcept
{
    // Values closer than this render the same (a parameter read back can differ in its last bits).
    const auto same = [](double x, double y) { return std::abs(x - y) < 1.0e-9; };
    if (a.line != b.line || a.target != b.target || ! same(a.amount, b.amount) || a.edits.size() != b.edits.size())
        return false;
    if (a.target == core::Target::Note && ! same(a.noteMidi, b.noteMidi))
        return false;
    for (std::size_t i = 0; i < a.edits.size(); ++i)
        if (! same(a.edits[i].seconds, b.edits[i].seconds) || ! same(a.edits[i].semitones, b.edits[i].semitones))
            return false;
    return true;
}

void Engine::SpinLock::lock() noexcept
{
    while (! tryLock())
        std::this_thread::yield();
}

//==============================================================================
Engine::Engine()
{
    m_ring = std::make_unique<std::atomic<float>[]>(static_cast<std::size_t>(kRingSize));
    for (std::int64_t i = 0; i < kRingSize; ++i)
        m_ring[static_cast<std::size_t>(i)].store(0.0f, std::memory_order_relaxed);
    m_analysisThread = std::thread([this] { analysisLoop(); });
    m_houseThread = std::thread([this] { houseLoop(); });
}

Engine::~Engine()
{
    m_stop.store(true);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_analysisWake.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(m_threadMutex);
        m_houseWake.notify_all();
    }
    m_analysisThread.join();
    m_houseThread.join();
}

int Engine::latencyFor(double sampleRate) noexcept
{
    return static_cast<int>(secondsToSamples(kLatencySeconds, sampleRate));
}

void Engine::prepare(double sampleRate, int maxBlockSize)
{
    (void) maxBlockSize;   // everything runs sample by sample: any block size works
    std::lock_guard<std::mutex> lock(m_mutex);
    const double rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    m_rate.store(rate);
    m_latency = std::max(1, latencyFor(rate));
    m_fadeLength = std::max(1, static_cast<int>(secondsToSamples(kFadeSeconds, rate)));
    m_verifyWindow = std::max(256, Locator::nominalProbeLength(rate) / 4);
    for (auto& d : m_delay)
        d.assign(static_cast<std::size_t>(m_latency), 0.0f);
    m_delayPosition = 0;
    const auto preRoll = static_cast<std::size_t>(std::max<std::int64_t>(1, secondsToSamples(kPreRollSeconds, rate)));
    for (auto& p : m_preRoll)
        p.assign(preRoll, 0.0f);
    m_preRollPosition = 0;
    m_preRollFill = 0;

    // The playback state starts afresh (positions count from here).
    m_inputPosition = 0;
    m_ringWritten.store(0, std::memory_order_release);
    for (std::int64_t i = 0; i < kRingSize; ++i)
        m_ring[static_cast<std::size_t>(i)].store(0.0f, std::memory_order_relaxed);
    m_current = m_previous = Voice {};
    m_fadePosition = m_fadeLength;
    m_numEvents = 0;
    m_match = Match::Off;
    m_searching.store(false);
    m_resetRequested.store(false);

    const int cap = m_captureState.load();
    if (cap == CapArmed || cap == CapListening)
        ensureCaptureBufferLocked(rate);
    m_prepared.store(true, std::memory_order_release);
}

//==============================================================================
// Learn
//==============================================================================
void Engine::ensureCaptureBufferLocked(double rate)
{
    const auto needed = secondsToSamples(kMaxCaptureSeconds, rate);
    if (m_captureCapacity >= needed && m_capture[0] != nullptr)
        return;
    for (auto& c : m_capture)
        c = std::make_unique<float[]>(static_cast<std::size_t>(needed));
    m_captureCapacity = needed;
}

void Engine::waitForCaptureWriter() const noexcept
{
    // The audio thread raises the flag, then checks the state; whoever changes the state away
    // from Armed or Listening waits here for a block already writing to finish (a block at most).
    while (m_captureWriting.load(std::memory_order_seq_cst))
        std::this_thread::yield();
}

void Engine::armLearn()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_captureState.load() == CapFinished)
        finaliseCaptureLocked();
    const int st = m_captureState.load();
    if (st == CapArmed || st == CapListening)
        return;
    const double rate = m_rate.load();
    ensureCaptureBufferLocked(rate > 0.0 ? rate : 48000.0);
    m_captureLength.store(0);
    m_disarmedByUser = false;
    m_captureState.store(CapArmed, std::memory_order_seq_cst);
    kickHouse();   // from its idle pace to watching the capture
}

void Engine::disarmLearn()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    int expected = CapArmed;
    if (m_captureState.compare_exchange_strong(expected, CapIdle))
    {
        waitForCaptureWriter();
        return;
    }
    expected = CapListening;
    if (m_captureState.compare_exchange_strong(expected, CapFinished))
        waitForCaptureWriter();
    if (m_captureState.load() == CapFinished)
    {
        m_disarmedByUser = true;   // a capture too short to keep then leaves Learn off, not armed
        finaliseCaptureLocked();
    }
}

bool Engine::isLearnArmed() const noexcept
{
    const int st = m_captureState.load();
    return st == CapArmed || st == CapListening;
}

void Engine::finaliseCaptureLocked()
{
    if (m_captureState.load() != CapFinished)
        return;
    waitForCaptureWriter();

    const auto rate = m_captureRate.load();
    const int numChannels = std::clamp(m_captureChannels.load(), 1, kMaxChannels);
    std::int64_t length = std::min(m_captureLength.load(std::memory_order_acquire), m_captureCapacity);

    // Trim the silence at the end (what ended the capture), keeping 10 ms after the last sound.
    const auto loud = [this, numChannels](std::int64_t i)
    {
        for (int c = 0; c < numChannels; ++c)
            if (std::abs(m_capture[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]) >= kSilence)
                return true;
        return false;
    };
    std::int64_t last = length - 1;
    while (last >= 0 && ! loud(last))
        --last;
    std::int64_t first = 0;
    while (first < last && ! loud(first))
        ++first;
    length = std::min(length, last + 1 + secondsToSamples(0.01, rate));

    // The sound itself (not the pre-roll before it) has to last kMinCaptureSeconds.
    if (last < 0 || static_cast<double>(last + 1 - first) < kMinCaptureSeconds * rate)
    {
        // A click, not a sound: forget it and keep listening (unless the user stopped Learn).
        m_captureLength.store(0);
        m_captureState.store(m_disarmedByUser ? CapIdle : CapArmed, std::memory_order_seq_cst);
        m_disarmedByUser = false;
        return;
    }

    auto sound = std::make_unique<core::Channels>(static_cast<std::size_t>(numChannels));
    for (int c = 0; c < numChannels; ++c)
    {
        const float* src = m_capture[static_cast<std::size_t>(c)].get();
        (*sound)[static_cast<std::size_t>(c)].assign(src, src + length);
    }
    m_captureState.store(CapIdle, std::memory_order_seq_cst);
    m_disarmedByUser = false;
    // The capture buffer is large (two minutes): give it back until Learn is armed again.
    for (auto& c : m_capture)
        c.reset();
    m_captureCapacity = 0;

    m_pendingCapture = std::move(sound);
    m_pendingRate = rate;
    m_captureEpoch.fetch_add(1);   // whatever was being analysed or rendered stops
    m_analysing.store(true);
    m_analysisWake.notify_all();
}

void Engine::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    int st = m_captureState.load();
    if (st == CapArmed || st == CapListening || st == CapFinished)
    {
        m_captureState.store(CapIdle, std::memory_order_seq_cst);
        waitForCaptureWriter();
    }
    m_captureLength.store(0);
    m_captureEpoch.fetch_add(1);
    m_pendingCapture.reset();
    m_learned.reset();
    m_analysing.store(false);
    m_rendering.store(false);
    publish(nullptr);
    m_analysisWake.notify_all();
}

void Engine::setOptions(const core::Options& options)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (sameOptions(options, m_options))
        return;
    m_options = options;
    m_optionsGeneration.fetch_add(1);
    m_analysisWake.notify_all();
}

core::Options Engine::options() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_options;
}

Engine::State Engine::state() const noexcept
{
    switch (m_captureState.load())
    {
        case CapArmed: return State::Armed;
        case CapListening: return State::Listening;
        case CapFinished: return State::Analysing;
        default: break;
    }
    if (m_analysing.load())
        return State::Analysing;
    std::shared_ptr<const Result> r;
    m_resultLock.lock();
    r = m_result;
    m_resultLock.unlock();
    if (r == nullptr)
        return State::Idle;
    return r->playable() ? State::Ready : State::NothingToFlatten;
}

double Engine::captureSeconds() const noexcept
{
    const double rate = m_captureRate.load();
    return rate > 0.0 ? static_cast<double>(m_captureLength.load()) / rate : 0.0;
}

std::shared_ptr<const Result> Engine::result() const
{
    m_resultLock.lock();
    auto r = m_result;
    m_resultLock.unlock();
    return r;
}

bool Engine::waitUntilIdle(double timeoutSeconds) const
{
    const double end = nowSeconds() + timeoutSeconds;
    while (nowSeconds() < end)
    {
        bool idle;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const int st = m_captureState.load();
            idle = st != CapListening && st != CapFinished && m_pendingCapture == nullptr && ! m_analysing.load()
                   && (m_learned == nullptr || m_renderedOptions == m_optionsGeneration.load());
        }
        if (idle)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

//==============================================================================
// Results
//==============================================================================
void Engine::publish(std::shared_ptr<const Result> result)
{
    if (result != nullptr)
        m_published.push_back(result);
    m_resultLock.lock();
    m_result = std::move(result);
    m_resultLock.unlock();
    m_resultGeneration.fetch_add(1, std::memory_order_acq_rel);
    kickHouse();   // a search may start now
}

bool Engine::tryAcquireResult(std::shared_ptr<const Result>& out) const noexcept
{
    // A shared_ptr copy is a reference count increment: no allocation. The lock is only held for
    // that copy or a pointer swap, so a few tries are enough; failing means "try next block".
    for (int attempt = 0; attempt < 64; ++attempt)
        if (m_resultLock.tryLock())
        {
            out = m_result;
            m_resultLock.unlock();
            return true;
        }
    return false;
}

void Engine::freeUnusedResults()
{
    // Only the current result can gain new holders (tryAcquireResult copies m_result), so an older
    // one that only this list holds can never be picked up again: it is freed here, on this
    // thread, never on the audio thread.
    std::vector<std::shared_ptr<const Result>> doomed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto current = result();
        for (auto it = m_published.begin(); it != m_published.end();)
            if (*it != current && it->use_count() == 1)
            {
                doomed.push_back(std::move(*it));
                it = m_published.erase(it);
            }
            else
                ++it;
    }
    doomed.clear();   // outside the lock: a long sound takes a moment to free
}

//==============================================================================
// Analysis thread
//==============================================================================
void Engine::analysisLoop()
{
    for (;;)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_analysisWake.wait(lock, [this]
        {
            return m_stop.load() || m_pendingCapture != nullptr
                   || (m_learned != nullptr && m_renderedOptions != m_optionsGeneration.load());
        });
        if (m_stop.load())
            return;

        if (m_pendingCapture != nullptr)
        {
            auto sound = std::move(m_pendingCapture);
            const double rate = m_pendingRate;
            const auto epoch = m_captureEpoch.load();
            lock.unlock();

            const auto cancelled = [this, epoch] { return m_stop.load() || m_captureEpoch.load() != epoch; };
            auto learned = std::make_shared<Learned>();
            learned->id = epoch;
            learned->sampleRate = rate;
            learned->analysis = core::analyse(*sound, rate, true, cancelled);
            if (cancelled())
                continue;
            const std::size_t n = (*sound)[0].size();
            std::vector<float> mono(n, 0.0f);
            const float share = 1.0f / static_cast<float>(sound->size());
            for (const auto& ch : *sound)
                for (std::size_t i = 0; i < n; ++i)
                    mono[i] += ch[i] * share;
            learned->locator.prepare(mono.data(), static_cast<std::int64_t>(n), rate);
            learned->channels = std::move(*sound);

            lock.lock();
            if (m_captureEpoch.load() != epoch)
                continue;
            m_learned = std::move(learned);
            m_renderedOptions = 0;   // render it with the current options
        }

        if (m_learned == nullptr || m_renderedOptions == m_optionsGeneration.load())
        {
            m_analysing.store(false);
            continue;
        }

        // Render with the latest options. A newer change (a control being dragged), a new capture
        // or Clear stops it, and the loop comes back for the newest.
        const auto learned = m_learned;
        auto options = m_options;
        const auto generation = m_optionsGeneration.load();
        const auto epoch = m_captureEpoch.load();
        m_rendering.store(true);
        lock.unlock();

        if (options.line >= 0
            && (options.line >= core::kNumTrackers || ! learned->analysis.lines[static_cast<std::size_t>(options.line)].usable))
            options.line = -1;   // that line has nothing to flatten by: the analysis' own pick
        const auto cancelled = [this, epoch, generation]
        {
            return m_stop.load() || m_captureEpoch.load() != epoch || m_optionsGeneration.load() != generation;
        };
        auto result = std::make_shared<Result>();
        result->learned = learned;
        if (const auto* line = core::lineFor(learned->analysis, options))
        {
            if (options.line < 0)
                options.line = static_cast<int>(line->tracker);
            result->flattened = core::flatten(learned->channels, learned->sampleRate, learned->analysis, options, cancelled);
        }
        else
            options.line = -1;
        result->options = options;
        if (cancelled())
            continue;

        lock.lock();
        if (m_captureEpoch.load() != epoch || m_optionsGeneration.load() != generation)
            continue;
        result->generation = m_nextGeneration++;
        publish(std::move(result));
        m_renderedOptions = generation;
        m_rendering.store(false);
        m_analysing.store(false);
    }
}

//==============================================================================
// House thread: stalled captures, the search, freeing
//==============================================================================
void Engine::kickHouse() noexcept
{
    m_houseKick.store(true, std::memory_order_release);
    m_houseWake.notify_one();
}

void Engine::houseLoop()
{
    std::uint64_t lastBlocks = 0;
    double lastBlockTime = nowSeconds();
    int freeCountdown = 0;
    while (! m_stop.load())
    {
        {
            // Every 5 ms while a sound is being captured or searched for; otherwise at a leisurely
            // pace, so a rack full of idle instances costs next to nothing.
            const int cap = m_captureState.load();
            const bool busy = m_searching.load() || cap == CapArmed || cap == CapListening || cap == CapFinished;
            std::unique_lock<std::mutex> lock(m_threadMutex);
            m_houseWake.wait_for(lock, busy ? kHousePeriod : kIdlePeriod, [this] { return m_stop.load() || m_houseKick.load(); });
            m_houseKick.store(false);
        }
        if (m_stop.load())
            return;

        // The audio stopped arriving while a sound was being captured: the host stopped playing.
        const auto blocks = m_blocks.load(std::memory_order_acquire);
        const double now = nowSeconds();
        if (blocks != lastBlocks)
        {
            lastBlocks = blocks;
            lastBlockTime = now;
        }
        const int cap = m_captureState.load();
        if (cap == CapListening && now - lastBlockTime >= kStallSeconds)
        {
            int expected = CapListening;
            if (m_captureState.compare_exchange_strong(expected, CapFinished))
                waitForCaptureWriter();
        }
        if (m_captureState.load() == CapFinished)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            finaliseCaptureLocked();
        }

        searchStep();

        if (--freeCountdown <= 0)
        {
            freeUnusedResults();
            freeCountdown = 20;
        }
    }
}

void Engine::searchStep()
{
    if (! m_searching.load(std::memory_order_acquire))
        return;
    const auto epoch = m_searchEpoch.load(std::memory_order_acquire);
    const auto r = result();
    const double rate = m_rate.load();
    if (r == nullptr || ! r->playable() || std::abs(r->learned->sampleRate - rate) > 0.5)
        return;
    const Learned& learned = *r->learned;
    const Locator& locator = learned.locator;
    const int P = locator.probeLength();
    if (P <= 0)
        return;

    const auto publishNeed = [this, epoch](std::int64_t need)
    {
        m_workerNeed.store(need, std::memory_order_relaxed);
        m_workerEpoch.store(epoch, std::memory_order_release);
    };

    if (epoch != m_wEpoch || learned.id != m_wLearned)
    {
        m_wEpoch = epoch;
        m_wLearned = learned.id;
        m_wCursor = m_searchFrom.load(std::memory_order_acquire);
        m_wRunStart = -1;
        m_wAttempt = -1;
    }

    const auto written = m_ringWritten.load(std::memory_order_acquire);
    const auto oldest = written - kRingSize + 8192;
    if (m_wCursor < oldest)
    {
        m_wCursor = oldest;
        m_wAttempt = -1;
    }

    // Each attempt starts at the first sound after the cursor: a probe that began in silence
    // would not match a sound played from part way through.
    if (m_wAttempt < 0)
    {
        std::int64_t i = std::max<std::int64_t>(m_wCursor, 0);
        for (; i < written; ++i)
            if (std::abs(m_ring[static_cast<std::size_t>(i & kRingMask)].load(std::memory_order_relaxed)) >= kSilence)
                break;
        if (i >= written)
        {
            m_wCursor = written;
            publishNeed(written + 1);
            return;
        }
        // A gap of silence before it (or the first sound of this search): a sound starts here, so
        // any place in the learned sound is as likely as any other. Otherwise the input has been
        // sounding since m_wRunStart, which a repeating sound's place has to agree with.
        if (m_wRunStart < 0 || i - m_wCursor >= secondsToSamples(0.05, rate))
            m_wRunStart = i;
        m_wAttempt = i;
    }
    if (written < m_wAttempt + P)
    {
        publishNeed(m_wAttempt + P);
        return;
    }

    std::vector<float> probe(static_cast<std::size_t>(P));
    for (int i = 0; i < P; ++i)
        probe[static_cast<std::size_t>(i)] = m_ring[static_cast<std::size_t>((m_wAttempt + i) & kRingMask)].load(std::memory_order_relaxed);
    if (m_ringWritten.load(std::memory_order_acquire) - m_wAttempt > kRingSize - 4096)
    {
        m_wCursor = m_ringWritten.load() - P;   // the audio thread wrote over it: start again later
        m_wAttempt = -1;
        return;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto match = locator.locate(probe.data(), std::max<std::int64_t>(0, m_wAttempt - m_wRunStart),
                                      [this, epoch] { return m_stop.load() || m_searchEpoch.load() != epoch; });
    m_lastSearchMs.store(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    m_searches.fetch_add(1, std::memory_order_relaxed);
    if (m_searchEpoch.load() != epoch)
        return;

    if (match.found)
    {
        m_answerSequence.fetch_add(1, std::memory_order_acq_rel);   // odd: being written
        m_answerEpoch.store(epoch, std::memory_order_relaxed);
        m_answerLearned.store(learned.id, std::memory_order_relaxed);
        m_answerInput.store(m_wAttempt, std::memory_order_relaxed);
        m_answerSource.store(match.sourceSample, std::memory_order_relaxed);
        m_answerLength.store(P, std::memory_order_relaxed);
        m_answerGain.store(match.gain, std::memory_order_relaxed);
        m_answerSequence.fetch_add(1, std::memory_order_acq_rel);   // even: complete
        publishNeed(kNever);
        return;
    }

    // Not the learned sound here. Try a little later, or the newest audio if the search fell
    // behind (a long sound takes a while to search).
    m_wCursor = std::max(m_wAttempt + P / 4, written - P);
    m_wAttempt = -1;
    publishNeed(m_wCursor + 1);
}

//==============================================================================
// Audio thread
//==============================================================================
void Engine::writeInput(const float* const* channels, int numChannels, int numSamples, float& peak) noexcept
{
    const std::int64_t t0 = m_inputPosition;
    const float share = numChannels > 0 ? 1.0f / static_cast<float>(numChannels) : 0.0f;
    peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        float mono = 0.0f;
        for (int c = 0; c < numChannels; ++c)
        {
            const float v = channels[c][i];
            mono += v * share;
            peak = std::max(peak, std::abs(v));
        }
        m_ring[static_cast<std::size_t>((t0 + i) & kRingMask)].store(mono, std::memory_order_relaxed);
    }
    m_inputPosition = t0 + numSamples;
    m_ringWritten.store(m_inputPosition, std::memory_order_release);
    m_inputLevel.store(peak, std::memory_order_relaxed);
}

void Engine::handleReset() noexcept
{
    if (! m_resetRequested.exchange(false, std::memory_order_acq_rel))
        return;
    for (auto& d : m_delay)
        std::fill(d.begin(), d.end(), 0.0f);
    m_numEvents = 0;
    m_current = m_previous = Voice {};
    m_fadePosition = m_fadeLength;
    m_match = Match::Off;
    m_searching.store(false, std::memory_order_release);
}

void Engine::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    if (! m_prepared.load(std::memory_order_acquire) || numSamples <= 0 || numChannels <= 0)
        return;
    numChannels = std::min(numChannels, kMaxChannels);
    m_blocks.fetch_add(1, std::memory_order_release);
    handleReset();

    const std::int64_t t0 = m_inputPosition;
    float peak = 0.0f;
    writeInput(channels, numChannels, numSamples, peak);
    captureBlock(channels, numChannels, numSamples, peak);
    updateMatching(t0, numSamples);
    renderOutput(channels, numChannels, numSamples, t0);
}

void Engine::processBypassed(float* const* channels, int numChannels, int numSamples) noexcept
{
    if (! m_prepared.load(std::memory_order_acquire) || numSamples <= 0 || numChannels <= 0)
        return;
    numChannels = std::min(numChannels, kMaxChannels);
    m_blocks.fetch_add(1, std::memory_order_release);
    handleReset();

    const std::int64_t t0 = m_inputPosition;
    float peak = 0.0f;
    writeInput(channels, numChannels, numSamples, peak);
    // Bypass is immediate (the host switches abruptly too): only the delayed input from here.
    m_numEvents = 0;
    m_current = m_previous = Voice {};
    m_fadePosition = m_fadeLength;
    m_match = Match::Off;
    m_searching.store(false, std::memory_order_release);
    m_located.store(false, std::memory_order_relaxed);
    renderOutput(channels, numChannels, numSamples, t0);
}

void Engine::captureBlock(const float* const* channels, int numChannels, int numSamples, float peak) noexcept
{
    const int st = m_captureState.load(std::memory_order_acquire);
    const bool justArmed = st == CapArmed && m_lastCaptureState != CapArmed;
    m_lastCaptureState = st;
    if (st != CapArmed && st != CapListening)
    {
        m_silentRun = 0;
        return;
    }
    const int preRollLength = static_cast<int>(m_preRoll[0].size());
    if (justArmed)
        m_preRollFill = 0;   // what came before Learn was armed is not part of the sound

    if (st == CapArmed && peak < kSilence)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < kMaxChannels; ++c)
                m_preRoll[static_cast<std::size_t>(c)][static_cast<std::size_t>(m_preRollPosition)] = channels[std::min(c, numChannels - 1)][i];
            m_preRollPosition = (m_preRollPosition + 1) % preRollLength;
        }
        m_preRollFill = std::min(preRollLength, m_preRollFill + numSamples);
        return;
    }

    // Writing: raise the flag, then make sure the state still lets us (see waitForCaptureWriter()).
    m_captureWriting.store(true, std::memory_order_seq_cst);
    if (m_captureState.load(std::memory_order_seq_cst) != st)
    {
        m_captureWriting.store(false, std::memory_order_seq_cst);
        return;
    }

    const std::int64_t capacity = m_captureCapacity;
    std::int64_t length = st == CapArmed ? 0 : m_captureLength.load(std::memory_order_relaxed);
    const int keep = st == CapArmed ? std::clamp(numChannels, 1, kMaxChannels) : std::clamp(m_captureChannels.load(), 1, kMaxChannels);

    if (st == CapArmed)
    {
        // The first sound: the pre-roll first, oldest sample first.
        m_captureRate.store(m_rate.load(std::memory_order_relaxed));
        m_captureChannels.store(keep);
        const int from = (m_preRollPosition - m_preRollFill + preRollLength) % preRollLength;
        for (int i = 0; i < m_preRollFill && length < capacity; ++i, ++length)
            for (int c = 0; c < keep; ++c)
                m_capture[static_cast<std::size_t>(c)][static_cast<std::size_t>(length)]
                    = m_preRoll[static_cast<std::size_t>(c)][static_cast<std::size_t>((from + i) % preRollLength)];
        m_preRollFill = 0;
        m_silentRun = 0;
    }

    const int take = static_cast<int>(std::min<std::int64_t>(numSamples, capacity - length));
    for (int c = 0; c < keep; ++c)
        std::memcpy(m_capture[static_cast<std::size_t>(c)].get() + length, channels[std::min(c, numChannels - 1)], sizeof(float) * static_cast<std::size_t>(take));
    length += take;
    m_silentRun = peak < kSilence ? m_silentRun + numSamples : 0;
    m_captureLength.store(length, std::memory_order_release);

    int expected = st;
    if (st == CapArmed)
        m_captureState.compare_exchange_strong(expected, CapListening, std::memory_order_seq_cst);
    else if (m_silentRun >= secondsToSamples(kEndSilenceSeconds, m_rate.load(std::memory_order_relaxed)) || length >= capacity)
        m_captureState.compare_exchange_strong(expected, CapFinished, std::memory_order_seq_cst);
    // The house thread notices the end within a few milliseconds (no wake-up from here: that
    // could take a lock).
    m_captureWriting.store(false, std::memory_order_seq_cst);
}

bool Engine::isReferenced(const Result* r) const noexcept
{
    if (r == nullptr)
        return false;
    if (r == m_latest || r == m_current.result || r == m_previous.result)
        return true;
    for (int i = 0; i < m_numEvents; ++i)
        if (m_events[static_cast<std::size_t>(i)].voice.result == r)
            return true;
    return false;
}

void Engine::pushEvent(const Event& e) noexcept
{
    // A switch replaces every switch queued for the same moment or later.
    while (m_numEvents > 0 && m_events[static_cast<std::size_t>(m_numEvents - 1)].at >= e.at)
        --m_numEvents;
    if (m_numEvents == static_cast<int>(m_events.size()))
    {
        for (int i = 1; i < m_numEvents; ++i)
            m_events[static_cast<std::size_t>(i - 1)] = m_events[static_cast<std::size_t>(i)];
        --m_numEvents;
    }
    m_events[static_cast<std::size_t>(m_numEvents++)] = e;
}

void Engine::beginSearch(std::int64_t from) noexcept
{
    m_match = Match::Waiting;
    ++m_epoch;
    m_searchFrom.store(std::max<std::int64_t>({ from, 0, m_inputPosition - kRingSize + 8192 }), std::memory_order_relaxed);
    m_searchEpoch.store(m_epoch, std::memory_order_release);
    m_searching.store(true, std::memory_order_release);
    m_located.store(false, std::memory_order_relaxed);
}

void Engine::pickUpResult(std::int64_t outNext) noexcept
{
    const auto generation = m_resultGeneration.load(std::memory_order_acquire);
    if (generation == m_seenGeneration)
        return;

    // A free slot to hold it in (one not used by the output). Slots no longer needed are let go
    // first: that only drops a count, as the engine's own list still holds them.
    for (auto& h : m_held)
        if (h != nullptr && h.get() != m_latest && ! isReferenced(h.get()))
            h.reset();
    std::size_t slot = m_held.size();
    for (std::size_t i = 0; i < m_held.size(); ++i)
        if (m_held[i] == nullptr)
        {
            slot = i;
            break;
        }
    if (slot == m_held.size())
        return;   // every slot is still playing: try again next block
    if (! tryAcquireResult(m_held[slot]))
        return;
    m_seenGeneration = generation;

    const Result* old = m_latest;
    const Result* fresh = m_held[slot].get();
    m_latest = fresh;
    const bool sameSound = fresh != nullptr && old != nullptr && fresh->learned == old->learned;

    if (sameSound && m_match == Match::Playing && fresh->playable()
        && std::abs(fresh->learned->sampleRate - m_rate.load(std::memory_order_relaxed)) <= 0.5)
    {
        // A new render of the same sound while it plays: carry on in the new one from the same
        // moment of the sound (the time maps differ, so the flattened positions do).
        const std::int64_t at = std::max(m_segmentStart, outNext);
        const double source = static_cast<double>(at + m_sourceOffset);
        m_flatOffset = static_cast<std::int64_t>(std::llround(fresh->flattened.outputSampleFor(source))) - at;
        m_segmentStart = at;
        pushEvent({ at, Voice { fresh, m_flatOffset, static_cast<float>(m_gain) } });
    }
    else if (! sameSound)
    {
        // Another sound (or none): back to the input, and search again for the new one.
        if (m_match != Match::Off || m_current.result != nullptr || m_numEvents > 0)
            pushEvent({ outNext, Voice {} });
        m_match = Match::Off;
        m_searching.store(false, std::memory_order_release);
    }
}

bool Engine::readAnswer(const Result& r, std::int64_t outNext) noexcept
{
    const auto before = m_answerSequence.load(std::memory_order_acquire);
    if ((before & 1u) != 0)
        return false;
    const auto epoch = m_answerEpoch.load(std::memory_order_relaxed);
    const auto learned = m_answerLearned.load(std::memory_order_relaxed);
    const auto input = m_answerInput.load(std::memory_order_relaxed);
    const auto source = m_answerSource.load(std::memory_order_relaxed);
    const auto length = m_answerLength.load(std::memory_order_relaxed);
    const auto gain = m_answerGain.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (m_answerSequence.load(std::memory_order_relaxed) != before)
        return false;   // written meanwhile: next block
    if (epoch != m_epoch || learned != r.learned->id)
        return false;

    m_sourceOffset = source - input;
    m_flatOffset = static_cast<std::int64_t>(std::llround(r.flattened.outputSampleFor(static_cast<double>(source)))) - input;
    m_segmentStart = std::max(input, outNext);   // a late answer starts now, at the right place for now
    m_verifyPosition = input + length;           // the search compared the probe already
    // A level within about a tenth of a decibel of the learned one is the learned one (the
    // measurement has that much grain), so the same file played back plays the render exactly.
    m_gain = gain > 0.0 ? (std::abs(gain - 1.0) < 0.012 ? 1.0 : gain) : 1.0;
    m_match = Match::Playing;
    m_searching.store(false, std::memory_order_release);
    m_located.store(true, std::memory_order_relaxed);
    m_lastMatchInput.store(input, std::memory_order_relaxed);
    m_lastMatchSource.store(source, std::memory_order_relaxed);
    pushEvent({ m_segmentStart, Voice { &r, m_flatOffset, static_cast<float>(m_gain) } });
    return true;
}

void Engine::waitForSearch(std::int64_t tEnd) noexcept
{
    // Offline the host runs faster than real time, so the search would answer too late for the
    // latency to hide it. Here (and only here) the audio thread waits for it: rendering offline
    // is allowed to take its time.
    const double deadline = nowSeconds() + 3.0;
    for (int spins = 0;; ++spins)
    {
        const auto seq = m_answerSequence.load(std::memory_order_acquire);
        if ((seq & 1u) == 0 && m_answerEpoch.load(std::memory_order_relaxed) == m_epoch)
            return;
        if (m_workerEpoch.load(std::memory_order_acquire) == m_epoch && m_workerNeed.load(std::memory_order_relaxed) > tEnd)
            return;
        if (m_stop.load())
            return;
        if ((spins & 63) == 0)
        {
            kickHouse();
            if (nowSeconds() > deadline)
                return;
        }
        std::this_thread::yield();
    }
}

void Engine::verify(const Result& r, std::int64_t tEnd, std::int64_t outNext) noexcept
{
    const Locator& locator = r.learned->locator;
    const float* learned = locator.samples();
    const std::int64_t learnedLength = locator.length();
    const std::int64_t flatLength = static_cast<std::int64_t>(r.flattened.channels[0].size());
    const int W = std::min(m_verifyWindow, std::max(1, locator.probeLength()));
    const double silent = static_cast<double>(W) * kSilence * kSilence;
    const double loud = silent * 100.0;   // 20 dB over silence
    const double gain2 = m_gain * m_gain;

    while (m_verifyPosition + W <= tEnd)
    {
        if (m_verifyPosition < tEnd - kRingSize + 8192)
            m_verifyPosition = tEnd - W;   // cannot happen with sane blocks; never read stale ring data
        const std::int64_t source = m_verifyPosition + m_sourceOffset;
        if (source >= learnedLength || source < 0)
        {
            // Past the end of the sound: whatever the flattened sound has left plays out, then the
            // input again.
            const std::int64_t end = std::max(learnedLength - m_sourceOffset, flatLength - m_flatOffset);
            pushEvent({ std::max(end, outNext), Voice {} });
            beginSearch(m_verifyPosition);
            return;
        }
        double xx = 0.0, yy = 0.0, xy = 0.0;
        for (int i = 0; i < W; ++i)
        {
            const double x = m_ring[static_cast<std::size_t>((m_verifyPosition + i) & kRingMask)].load(std::memory_order_relaxed);
            const double y = learned[source + i];   // the learned sound has a probe's length of zeros after it
            xx += x * x;
            yy += y * y;
            xy += x * y;
        }
        const double expected = yy * gain2;
        bool same;
        if (xx < silent)
            same = expected < loud;            // both quiet; or the player stopped while the sound goes on
        else if (expected < silent)
            same = false;                      // something else is playing where the sound is quiet
        else
            same = xy / std::sqrt(xx * yy) >= kKeepCorrelation;
        if (! same)
        {
            pushEvent({ std::max(m_verifyPosition, outNext), Voice {} });
            beginSearch(m_verifyPosition);
            return;
        }
        m_verifyPosition += W;
    }
}

void Engine::updateMatching(std::int64_t t0, int numSamples) noexcept
{
    const std::int64_t tEnd = t0 + numSamples;
    const std::int64_t outNext = t0 - m_latency;   // the input position the next output sample is

    pickUpResult(outNext);
    const Result* r = m_latest;
    const bool rateMismatch = r != nullptr && r->learned != nullptr && std::abs(r->learned->sampleRate - m_rate.load(std::memory_order_relaxed)) > 0.5;
    m_rateMismatch.store(rateMismatch, std::memory_order_relaxed);
    const int cap = m_captureState.load(std::memory_order_relaxed);
    const bool playable = r != nullptr && r->playable() && ! rateMismatch && cap != CapArmed && cap != CapListening;

    if (! playable)
    {
        if (m_match != Match::Off)
        {
            pushEvent({ outNext, Voice {} });
            m_match = Match::Off;
            m_searching.store(false, std::memory_order_release);
            m_located.store(false, std::memory_order_relaxed);
        }
        return;
    }
    if (m_match == Match::Off)
        beginSearch(outNext);

    if (m_match == Match::Waiting)
    {
        if (m_nonRealtime.load(std::memory_order_relaxed))
            waitForSearch(tEnd);
        readAnswer(*r, outNext);
    }
    if (m_match == Match::Playing)
        verify(*r, tEnd, outNext);
}

float Engine::voiceSample(const Voice& v, int channel, int numChannels, std::int64_t u, float dry) const noexcept
{
    if (v.result == nullptr)
        return dry;
    const auto& ch = v.result->flattened.channels;
    const std::int64_t index = u + v.flatOffset;
    if (index < 0 || index >= static_cast<std::int64_t>(ch[0].size()))
        return 0.0f;   // before or after the flattened sound: silence (it may be shorter than the original)
    const auto i = static_cast<std::size_t>(index);
    if (ch.size() == 1)
        return v.gain * ch[0][i];                          // mono sound: every channel
    if (numChannels == 1)
        return v.gain * 0.5f * (ch[0][i] + ch[1][i]);      // stereo sound, mono track: the two halves summed
    return v.gain * ch[static_cast<std::size_t>(std::min(channel, 1))][i];
}

void Engine::renderOutput(float* const* channels, int numChannels, int numSamples, std::int64_t t0) noexcept
{
    const int L = m_latency;
    for (int i = 0; i < numSamples; ++i)
    {
        const std::int64_t u = t0 + i - L;   // the input position this output sample is
        while (m_numEvents > 0 && m_events[0].at <= u)
        {
            const Voice next = m_events[0].voice;
            for (int k = 1; k < m_numEvents; ++k)
                m_events[static_cast<std::size_t>(k - 1)] = m_events[static_cast<std::size_t>(k)];
            --m_numEvents;
            if (next == m_current)
                continue;
            m_previous = m_current;
            m_current = next;
            m_fadePosition = 0;
        }
        const bool fading = m_fadePosition < m_fadeLength;
        const float w = fading ? (static_cast<float>(m_fadePosition) + 0.5f) / static_cast<float>(m_fadeLength) : 1.0f;
        for (int c = 0; c < numChannels; ++c)
        {
            auto& line = m_delay[static_cast<std::size_t>(c)];
            const float dry = line[static_cast<std::size_t>(m_delayPosition)];
            line[static_cast<std::size_t>(m_delayPosition)] = channels[c][i];
            float v = voiceSample(m_current, c, numChannels, u, dry);
            if (fading)
            {
                const float before = voiceSample(m_previous, c, numChannels, u, dry);
                v = before + (v - before) * w;
            }
            channels[c][i] = v;
        }
        if (++m_delayPosition >= L)
            m_delayPosition = 0;
        if (fading)
            ++m_fadePosition;
    }

    if (m_current.result != nullptr)
    {
        m_playPosition.store(t0 + numSamples - 1 - L + m_current.flatOffset, std::memory_order_relaxed);
        m_playGeneration.store(m_current.result->generation, std::memory_order_relaxed);
    }
    else
        m_playPosition.store(-1, std::memory_order_relaxed);
}

} // namespace pitchflattener
