#include "Engine.h"

#include "SoundRegistry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>

namespace pitchflattener
{
namespace
{
constexpr float kSilence = Locator::kSilence;
constexpr std::int64_t kNever = INT64_MAX;
// The house thread's pace: quick while a capture or a search may need it, leisurely otherwise, so a
// rack full of idle instances costs next to nothing.
constexpr auto kHousePeriod = std::chrono::milliseconds(5);
constexpr auto kIdlePeriod = std::chrono::milliseconds(50);
// The audio thread tries this many times for the spin lock around the published result before
// giving up until its next block: the lock is only ever held for a pointer copy or swap.
constexpr int kAcquireAttempts = 64;
// Offline waits: long enough for any search (a fraction of a second) or render (seconds for a
// long sound with its length kept), short enough that a stuck engine cannot hang a bounce.
constexpr double kSearchWaitSeconds = 3.0;
constexpr double kRenderWaitSeconds = 30.0;
// A render instance given a state with the sound in it decodes and analyses it first: a long
// sound with every reading scored can take this long on a slow machine.
constexpr double kAnalysisWaitSeconds = 60.0;
// A wait this long (or one that gave up) gets a line of its own in the log; shorter ones are counted.
constexpr double kNotableWaitMs = 50.0;
// Faster than real time (see the header): measured over at least this much audio and this many
// blocks; on above the first ratio, off again under the second.
constexpr double kPaceWindowSeconds = 0.1;
constexpr int kPaceWindowBlocks = 4;
constexpr double kFastRatio = 2.5, kRealTimeRatio = 1.5;
// No block for this long ends a run of audio (for the log), and starts the pace measurement afresh.
constexpr double kRunGapSeconds = 0.5;
// After a miss, or a match that stopped holding, the search waits for this much more input before
// it tries again, doubling on each miss in a row up to the longest. Silence (a new sound starting)
// or a match that held for a second starts it again from the shortest.
constexpr double kBackoffSeconds = 0.1;
constexpr double kLongestBackoffSeconds = 2.0;
constexpr double kNewSoundGapSeconds = 0.05;
constexpr double kHeldSeconds = 1.0;
// A playback level this close to the learned one (about a tenth of a decibel) is the learned one:
// the measurement has that much grain, and the same file played back must play the render exactly.
constexpr double kUnityGainTolerance = 0.012;
constexpr double kLowestGain = 1.0e-3, kHighestGain = 1.0e3;

std::int64_t secondsToSamples(double seconds, double rate) noexcept
{
    return static_cast<std::int64_t>(std::llround(seconds * rate));
}

double nowSeconds() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t nowMicros() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Generations are unique in the process: an instance can publish a render another instance made.
std::uint64_t nextGeneration() noexcept
{
    static std::atomic<std::uint64_t> next { 1 };
    return next.fetch_add(1);
}

// A random id for a learned sound, unique in practice across processes and sessions.
std::uint64_t newSoundId()
{
    static std::mutex mutex;
    static std::mt19937_64 random = []
    {
        std::random_device device;
        std::seed_seq seed { device(), device(), device(), device(),
                             static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) };
        return std::mt19937_64(seed);
    }();
    const std::lock_guard<std::mutex> lock(mutex);
    std::uint64_t id = 0;
    while (id == 0)
        id = random();
    return id;
}

double snapGain(double g) noexcept
{
    if (! std::isfinite(g))
        return 1.0;
    g = std::clamp(g, kLowestGain, kHighestGain);
    return std::abs(g - 1.0) < kUnityGainTolerance ? 1.0 : g;
}
} // namespace

bool sameOptions(const core::Options& a, const core::Options& b) noexcept
{
    // Values closer than this render the same (a parameter read back can differ in its last bits).
    const auto same = [](double x, double y) { return std::abs(x - y) < 1.0e-9; };
    if (a.line != b.line || a.target != b.target || a.keepLength != b.keepLength || a.character != b.character || ! same(a.amount, b.amount) || a.edits.size() != b.edits.size())
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
    : m_searchWaitLimit(kSearchWaitSeconds), m_renderWaitLimit(kRenderWaitSeconds), m_analysisWaitLimit(kAnalysisWaitSeconds)
{
    m_ring = std::make_unique<std::atomic<float>[]>(static_cast<std::size_t>(kRingSize));
    m_sideRing = std::make_unique<std::atomic<float>[]>(static_cast<std::size_t>(kRingSize));
    for (std::int64_t i = 0; i < kRingSize; ++i)
    {
        m_ring[static_cast<std::size_t>(i)].store(0.0f, std::memory_order_relaxed);
        m_sideRing[static_cast<std::size_t>(i)].store(0.0f, std::memory_order_relaxed);
    }
    m_analysisThread = std::thread([this] { analysisLoop(); });
    m_houseThread = std::thread([this] { houseLoop(); });
}

Engine::~Engine()
{
    endRun();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        letGoLocked();
    }
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

int Engine::latencyFor(double sampleRate, int maxBlockSize) noexcept
{
    // The output for input sample N must not leave before the search has had the probe after N,
    // a block to receive it in, and its budget (see the header).
    const auto least = secondsToSamples(kLatencySeconds, sampleRate);
    const auto needed = static_cast<std::int64_t>(Locator::nominalProbeLength(sampleRate)) + std::max(1, maxBlockSize)
                      + secondsToSamples(kSearchBudgetSeconds, sampleRate);
    return static_cast<int>(std::max(least, needed));
}

bool Engine::rateDiffers(double learnedRate) const noexcept
{
    // Rates are stored as doubles from different places (the host, the capture): within half a
    // hertz they are the same rate.
    return std::abs(learnedRate - m_rate.load(std::memory_order_relaxed)) > 0.5;
}

void Engine::prepare(double sampleRate, int maxBlockSize)
{
    endRun();
    m_lastBlockWall = 0;
    m_lastBlockDone.store(0);
    m_paceCount = 0;
    m_fast.store(false);
    std::lock_guard<std::mutex> lock(m_mutex);
    const double rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    m_rate.store(rate);
    m_latency = std::max(1, latencyFor(rate, maxBlockSize));
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
    m_fence.store(0, std::memory_order_release);
    for (std::int64_t i = 0; i < kRingSize; ++i)
    {
        m_ring[static_cast<std::size_t>(i)].store(0.0f, std::memory_order_relaxed);
        m_sideRing[static_cast<std::size_t>(i)].store(0.0f, std::memory_order_relaxed);
    }
    m_current = m_previous = Voice {};
    m_fadePosition = m_fadeLength;
    m_numEvents = 0;
    m_match = Match::Off;
    m_segmentResult = nullptr;
    m_gainHead = m_gainTail = 0;
    m_gainNow = 1.0f;
    m_gainRampLeft = 0;
    m_searching.store(false);
    m_active.store(false);
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
    m_captureEndWhy.store(2);
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
    // Silence is what the end of a capture counts as silence: under -70 dBFS, or 50 dB under the
    // loudest of the sound (a noise floor).
    float loudest = 0.0f;
    for (int c = 0; c < numChannels; ++c)
        for (std::int64_t i = 0; i < length; ++i)
            loudest = std::max(loudest, std::abs(m_capture[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]));
    const float floor = std::max(kSilence, loudest * static_cast<float>(std::pow(10.0, kRelativeSilenceDb / 20.0)));
    const auto loud = [this, numChannels, floor](std::int64_t i)
    {
        for (int c = 0; c < numChannels; ++c)
            if (std::abs(m_capture[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]) >= floor)
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
        EngineEvent e;
        e.kind = EngineEvent::Kind::CaptureEnded;
        e.i[0] = 4;
        e.i[1] = std::max<std::int64_t>(0, last + 1 - first);
        e.d[0] = rate;
        log(e);
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

    {
        EngineEvent e;
        e.kind = EngineEvent::Kind::CaptureEnded;
        e.i[0] = m_captureEndWhy.load();
        e.i[1] = length;
        e.d[0] = rate;
        log(e);
    }
    m_pendingCapture = std::move(sound);
    m_pendingStored.reset();
    m_pendingId = newSoundId();
    m_pendingRate = rate;
    m_captureEpoch.fetch_add(1);   // whatever was being analysed or rendered stops
    // The window shows ANALYSING from here, and the audio thread passes the input dry until the
    // new sound is published (see the header). The old result stays for cancelLearn().
    m_analysing.store(true);
    m_analysisWake.notify_all();
}

void Engine::cancelLearn()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const int st = m_captureState.load();
    if (st == CapArmed || st == CapListening || st == CapFinished)
    {
        m_captureState.store(CapIdle, std::memory_order_seq_cst);
        waitForCaptureWriter();
        for (auto& c : m_capture)
            c.reset();
        m_captureCapacity = 0;
    }
    m_captureLength.store(0);
    m_disarmedByUser = false;
    EngineEvent e;
    e.kind = EngineEvent::Kind::Cancelled;
    e.i[0] = st == CapArmed ? 0 : st == CapListening ? 1 : (m_pendingCapture != nullptr || m_analysing.load()) ? 2 : 3;
    if (m_pendingCapture != nullptr || m_pendingStored != nullptr || m_analysing.load())
    {
        m_captureEpoch.fetch_add(1);   // the analysis in flight stops (and a render of the old sound
        m_pendingCapture.reset();      // in flight with it: the loop renders that again if needed)
        m_pendingStored.reset();
        m_analysing.store(false);
        m_analysisWake.notify_all();
    }
    log(e);
    kickHouse();
}

void Engine::letGoLocked()
{
    if (m_learned != nullptr)
        SoundRegistry::instance().letGo(m_learned, result());
}

void Engine::setHooks(Hooks hooks)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hooks = std::move(hooks);
}

void Engine::setLogSink(LogSink sink, void* context) noexcept
{
    m_logContext.store(context, std::memory_order_release);
    m_logSink.store(sink, std::memory_order_release);
}

void Engine::log(const EngineEvent& event) const noexcept
{
    if (const auto sink = m_logSink.load(std::memory_order_acquire))
        sink(m_logContext.load(std::memory_order_acquire), event);
}

std::uint64_t Engine::learnedId() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // While a sound is waiting for, or under, analysis, it is the one this instance has.
    if (m_pendingCapture != nullptr || m_pendingStored != nullptr || m_analysing.load())
        return m_pendingId;
    if (m_learned != nullptr)
        return m_learned->id;
    const auto r = result();
    return r != nullptr && r->learned != nullptr ? r->learned->id : 0;
}

std::shared_ptr<const std::vector<char>> Engine::storedSound() const
{
    const auto r = result();
    return r != nullptr && r->learned != nullptr ? r->learned->stored : nullptr;
}

std::shared_ptr<const Learned> Engine::findRegistered(std::uint64_t id)
{
    return SoundRegistry::instance().find(id).learned;
}

bool Engine::adoptLearned(std::uint64_t id, const core::Options& options)
{
    const auto found = SoundRegistry::instance().find(id);
    if (found.learned == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool same = m_learned == found.learned && m_pendingCapture == nullptr && m_pendingStored == nullptr;
    if (! same)
    {
        // Whatever this instance was learning or analysing stops: the state names another sound.
        m_captureEpoch.fetch_add(1);
        m_pendingCapture.reset();
        m_pendingStored.reset();
        m_analysing.store(false);
        letGoLocked();
        m_learned = found.learned;
        m_pendingId = id;
        m_renderedOptions = 0;
    }
    if (! sameOptions(options, m_options))
    {
        m_options = options;
        m_optionsGeneration.fetch_add(1);
        m_renderedOptions = 0;
    }
    // Its render too, when one was made for the same options: then this instance plays exactly
    // what the other one does, at once.
    std::shared_ptr<const Result> render;
    for (const auto& r : found.renders)
        if (r->learned == found.learned && sameOptions(r->requested, m_options))
        {
            render = r;
            break;
        }
    const bool withRender = render != nullptr;
    if (withRender && result() != render)
        publish(render);
    if (withRender)
    {
        m_renderedOptions = m_optionsGeneration.load();
        m_renderPending.store(false);
    }
    else if (m_renderedOptions != m_optionsGeneration.load())
    {
        if (! same)
        {
            publish(nullptr);          // nothing of the sound before is played meanwhile
            m_analysing.store(true);   // the window says ANALYSING until its render is published
        }
        m_renderPending.store(true);
        m_analysisWake.notify_all();
    }
    EngineEvent e;
    e.kind = EngineEvent::Kind::Adopted;
    e.i[0] = static_cast<std::int64_t>(id);
    e.i[1] = withRender ? 1 : 0;
    log(e);
    return true;
}

void Engine::restoreLearned(std::uint64_t id, std::shared_ptr<const std::vector<char>> stored)
{
    if (stored == nullptr || stored->empty())
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if ((m_learned != nullptr && m_learned->id == id) || (m_pendingStored != nullptr && m_pendingId == id))
        return;
    EngineEvent e;
    e.kind = EngineEvent::Kind::RestoreStarted;
    e.i[0] = static_cast<std::int64_t>(id);
    e.i[1] = static_cast<std::int64_t>(stored->size());
    log(e);
    m_captureEpoch.fetch_add(1);
    m_pendingCapture.reset();
    m_pendingStored = std::move(stored);
    m_pendingId = id;
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
    m_pendingStored.reset();
    letGoLocked();
    m_learned.reset();
    EngineEvent e;
    e.kind = EngineEvent::Kind::Cleared;
    log(e);
    m_analysing.store(false);
    m_rendering.store(false);
    m_renderPending.store(false);
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
    if (m_learned != nullptr)
        m_renderPending.store(true);
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
    if (const auto previous = this->result(); previous != nullptr && previous->learned != nullptr
                                              && (result == nullptr || result->learned != previous->learned))
        SoundRegistry::instance().letGo(previous->learned, previous);
    if (result != nullptr)
        SoundRegistry::instance().publish(result->learned, result);
    m_currentRender.store(result != nullptr ? result->generation : 0, std::memory_order_release);
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
    // A shared_ptr copy is a reference count increment: no allocation.
    for (int attempt = 0; attempt < kAcquireAttempts; ++attempt)
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
            return m_stop.load() || m_pendingCapture != nullptr || m_pendingStored != nullptr
                   || (m_learned != nullptr && m_renderedOptions != m_optionsGeneration.load());
        });
        if (m_stop.load())
            return;

        if (m_pendingCapture != nullptr || m_pendingStored != nullptr)
        {
            auto sound = std::move(m_pendingCapture);
            auto stored = std::move(m_pendingStored);
            const auto id = m_pendingId;
            double rate = m_pendingRate;
            const auto epoch = m_captureEpoch.load();
            const auto hooks = m_hooks;
            lock.unlock();

            const auto cancelled = [this, epoch] { return m_stop.load() || m_captureEpoch.load() != epoch; };
            const bool restored = sound == nullptr;
            if (restored)
            {
                // A sound from the plug-in's state: decode it, then everything as after a Learn.
                sound = std::make_unique<core::Channels>();
                if (! hooks.decode || ! hooks.decode(*stored, *sound, rate) || sound->empty() || (*sound)[0].empty() || rate <= 0.0)
                {
                    EngineEvent e;
                    e.kind = EngineEvent::Kind::RestoreFailed;
                    e.i[0] = static_cast<std::int64_t>(id);
                    log(e);
                    lock.lock();
                    if (m_captureEpoch.load() == epoch)
                        m_analysing.store(false);
                    continue;
                }
            }
            const auto started = std::chrono::steady_clock::now();
            auto learned = std::make_shared<Learned>();
            learned->id = id;
            learned->sampleRate = rate;
            learned->analysis = core::analyse(*sound, rate, true, cancelled);
            if (cancelled())
                continue;
            {
                EngineEvent e;
                e.kind = EngineEvent::Kind::Analysed;
                e.i[0] = static_cast<std::int64_t>(id);
                e.i[1] = learned->analysis.pick;
                e.i[2] = static_cast<std::int64_t>(sound->size());
                e.i[3] = static_cast<std::int64_t>((*sound)[0].size());
                e.i[4] = restored ? 1 : 0;
                e.d[0] = rate;
                e.d[1] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                log(e);
            }
            const std::size_t n = (*sound)[0].size();
            std::vector<float> mono(n, 0.0f);
            const float share = 1.0f / static_cast<float>(sound->size());
            for (const auto& ch : *sound)
                for (std::size_t i = 0; i < n; ++i)
                    mono[i] += ch[i] * share;
            learned->locator.prepare(mono.data(), static_cast<std::int64_t>(n), rate);
            learned->soundEnd = 0;
            for (std::size_t i = n; i > 0; --i)
                if (std::abs(mono[i - 1]) >= kSilence)
                {
                    learned->soundEnd = static_cast<std::int64_t>(i);
                    break;
                }
            if (sound->size() == 2)
            {
                learned->side.assign(n + static_cast<std::size_t>(learned->locator.probeLength()), 0.0f);
                for (std::size_t i = 0; i < n; ++i)
                    learned->side[i] = 0.5f * ((*sound)[0][i] - (*sound)[1][i]);
            }
            // The copy for the plug-in's state: the bytes it came from, or compressed once now (every
            // later save reuses them).
            if (restored)
                learned->stored = std::move(stored);
            else if (hooks.encode)
            {
                const auto encodeStart = std::chrono::steady_clock::now();
                auto bytes = hooks.encode(*sound, rate);
                EngineEvent e;
                e.kind = EngineEvent::Kind::Stored;
                e.i[0] = static_cast<std::int64_t>(id);
                const auto limit = m_storeLimit.load();
                e.i[2] = static_cast<std::int64_t>(bytes.size()) > limit ? 1 : 0;
                e.i[1] = e.i[2] != 0 ? 0 : static_cast<std::int64_t>(bytes.size());
                e.d[0] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - encodeStart).count();
                log(e);
                if (! bytes.empty() && static_cast<std::int64_t>(bytes.size()) <= limit)
                    learned->stored = std::make_shared<const std::vector<char>>(std::move(bytes));
            }
            learned->channels = std::move(*sound);
            // Its options, with the hand corrections that belong to its reading (asked without the
            // lock: the host side takes its own).
            core::Options options;
            const bool haveOptions = static_cast<bool>(hooks.optionsFor);
            if (haveOptions)
                options = hooks.optionsFor(*learned);

            lock.lock();
            if (m_captureEpoch.load() != epoch)
                continue;
            letGoLocked();
            m_learned = std::move(learned);
            if (haveOptions && ! sameOptions(options, m_options))
            {
                m_options = options;
                m_optionsGeneration.fetch_add(1);
            }
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
        const auto requested = m_options;
        const auto generation = m_optionsGeneration.load();
        const auto renderStart = std::chrono::steady_clock::now();
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
        result->requested = requested;
        if (cancelled())
            continue;

        lock.lock();
        if (m_captureEpoch.load() != epoch || m_optionsGeneration.load() != generation)
            continue;
        result->generation = nextGeneration();
        {
            EngineEvent e;
            e.kind = EngineEvent::Kind::Rendered;
            e.i[0] = static_cast<int>(options.target);
            e.i[1] = options.line;
            e.i[2] = options.keepLength ? 1 : 0;
            e.i[3] = result->playable() ? static_cast<std::int64_t>(result->flattened.channels[0].size()) : 0;
            e.i[4] = result->playable() ? 1 : 0;
            e.i[5] = static_cast<std::int64_t>(options.edits.size());
            e.d[0] = options.amount;
            e.d[1] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - renderStart).count();
            e.d[2] = learned->sampleRate;
            log(e);
        }
        publish(std::move(result));
        m_renderedOptions = generation;
        m_renderPending.store(false);
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
            // Quick while a capture, a search or a match may need it (the audio thread never wakes
            // it in real time, so this pace is how soon a search starts); leisurely otherwise.
            const int cap = m_captureState.load();
            const bool busy = m_searching.load() || m_active.load() || cap == CapArmed || cap == CapListening || cap == CapFinished;
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
        // A run ends after half a second with no block being processed (a block waiting for the
        // search or the analysis is still being processed).
        const auto done = m_lastBlockDone.load(std::memory_order_relaxed);
        if (m_runOpen.load(std::memory_order_acquire) && done != 0 && blocks == lastBlocks && ! m_inBlock.load(std::memory_order_acquire)
            && static_cast<double>(nowMicros() - done) >= kRunGapSeconds * 1.0e6 && now - lastBlockTime >= kRunGapSeconds)
            endRun();
        if (cap == CapListening && now - lastBlockTime >= kStallSeconds)
        {
            int expected = CapListening;
            m_captureEndWhy.store(1);
            if (m_captureState.compare_exchange_strong(expected, CapFinished))
                waitForCaptureWriter();
        }
        if (m_captureState.load() == CapFinished)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            finaliseCaptureLocked();
        }

        searchStep();

        // Often enough that what Clear let go is given back within a fraction of a second.
        if (--freeCountdown <= 0)
        {
            freeUnusedResults();
            freeCountdown = 10;
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
    if (r == nullptr || ! r->playable() || rateDiffers(r->learned->sampleRate))
        return;
    const Learned& learned = *r->learned;
    const Locator& locator = learned.locator;
    const int P = locator.probeLength();
    if (P <= 0)
        return;
    const auto backoffBase = secondsToSamples(kBackoffSeconds, rate);
    const auto backoffMax = secondsToSamples(kLongestBackoffSeconds, rate);

    // need: the input this thread needs before it can do more; decided: no answer can start before
    // this input position any more (an offline wait only blocks while the output would pass it).
    const auto publishNeed = [this, epoch](std::int64_t need, std::int64_t decided)
    {
        m_workerNeed.store(need, std::memory_order_relaxed);
        m_workerDecided.store(decided, std::memory_order_relaxed);
        m_workerEpoch.store(epoch, std::memory_order_release);
    };

    if (epoch != m_wEpoch || learned.id != m_wLearned)
    {
        const bool newSound = learned.id != m_wLearned;
        m_wEpoch = epoch;
        m_wLearned = learned.id;
        m_wCursor = m_searchFrom.load(std::memory_order_acquire);
        m_wGate = m_wCursor;
        m_wRunStart = -1;
        m_wAttempt = -1;
        m_wSilentRun = 0;
        const auto holds = m_holds.load(std::memory_order_acquire);
        const auto failures = m_failures.load(std::memory_order_acquire);
        if (newSound || holds != m_wHoldsSeen || m_wBackoff <= 0)
            m_wBackoff = backoffBase;
        if (failures != m_wFailuresSeen && ! newSound)
        {
            // The last match stopped holding: wait for more input before trying again.
            m_wGate = m_wCursor + m_wBackoff;
            m_wBackoff = std::min(2 * m_wBackoff, backoffMax);
        }
        m_wHoldsSeen = holds;
        m_wFailuresSeen = failures;
    }

    const auto written = m_ringWritten.load(std::memory_order_acquire);
    const auto oldest = std::max(written - kRingSize + kRingGuard, m_fence.load(std::memory_order_acquire));
    if (m_wCursor < oldest)
    {
        m_wCursor = oldest;
        m_wAttempt = -1;
    }

    // Each attempt starts at the first sound at or after the gate: a probe that began in silence
    // would not match a sound played from part way through. A run of silence on the way means a
    // new sound: the back-off and the gate start again.
    if (m_wAttempt < 0)
    {
        const auto gap = secondsToSamples(kNewSoundGapSeconds, rate);
        std::int64_t i = std::max<std::int64_t>(m_wCursor, 0), found = -1;
        for (; i < written; ++i)
        {
            if (std::abs(m_ring[static_cast<std::size_t>(i & kRingMask)].load(std::memory_order_relaxed)) < kSilence)
            {
                ++m_wSilentRun;
                continue;
            }
            if (m_wSilentRun >= gap)
            {
                m_wBackoff = backoffBase;
                m_wGate = i;
                m_wRunStart = i;
            }
            m_wSilentRun = 0;
            if (i >= m_wGate)
            {
                found = i;
                break;
            }
        }
        m_wCursor = i;
        if (found < 0)
        {
            // Nothing to try yet. Offline, the audio thread may run on a little (a quarter of a
            // probe) before this thread has to look again.
            publishNeed(std::min(std::max(m_wGate, written) + P, written + P / 4 + 1), written);
            return;
        }
        if (m_wRunStart < 0)
            m_wRunStart = found;
        m_wAttempt = found;
    }
    if (written < m_wAttempt + P)
    {
        publishNeed(m_wAttempt + P, m_wAttempt);
        return;
    }

    std::vector<float> probe(static_cast<std::size_t>(P));
    for (int i = 0; i < P; ++i)
        probe[static_cast<std::size_t>(i)] = m_ring[static_cast<std::size_t>((m_wAttempt + i) & kRingMask)].load(std::memory_order_relaxed);
    if (m_ringWritten.load(std::memory_order_acquire) - m_wAttempt > kRingSize - kRingGuard)
    {
        m_wCursor = m_ringWritten.load() - P;   // the audio thread wrote over it: start again later
        m_wAttempt = -1;
        return;
    }

    publishNeed(m_wAttempt + P, m_wAttempt);   // searching now
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
        m_answerGainEnd.store(match.gainEnd, std::memory_order_relaxed);
        m_answerSequence.fetch_add(1, std::memory_order_acq_rel);   // even: complete
        publishNeed(kNever, kNever);
        return;
    }

    // Not the learned sound here. Wait until m_wBackoff more input has arrived, then try the newest.
    m_wGate = written + m_wBackoff - P;
    m_wBackoff = std::min(2 * m_wBackoff, backoffMax);
    m_wCursor = m_wAttempt + 1;
    m_wAttempt = -1;
    m_wSilentRun = 0;
    publishNeed(std::min(std::max(m_wGate, written) + P, m_wCursor + P / 4 + 1), m_wCursor);
}

//==============================================================================
// Audio thread
//==============================================================================
void Engine::writeInput(const float* const* channels, int numChannels, int numSamples, float& peak) noexcept
{
    const std::int64_t t0 = m_inputPosition;
    const float share = numChannels > 0 ? 1.0f / static_cast<float>(numChannels) : 0.0f;
    m_inputStereo = numChannels == 2;
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
        float side = numChannels == 2 ? 0.5f * (channels[0][i] - channels[1][i]) : 0.0f;
        // A bad sample (NaN, infinity) would poison every sum it enters: the search and the checks
        // see silence there instead. The audio itself passes as it came.
        if (! std::isfinite(mono) || ! std::isfinite(side))
            mono = side = 0.0f;
        const auto at = static_cast<std::size_t>((t0 + i) & kRingMask);
        m_ring[at].store(mono, std::memory_order_relaxed);
        m_sideRing[at].store(side, std::memory_order_relaxed);
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
    m_segmentResult = nullptr;
    m_searching.store(false, std::memory_order_release);
    // What the ring holds from before the reset belongs to the sound the host stopped: never
    // search or check it again.
    m_fence.store(m_inputPosition, std::memory_order_release);
    endRun();
}

void Engine::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    if (! m_prepared.load(std::memory_order_acquire) || numSamples <= 0 || numChannels <= 0)
        return;
    numChannels = std::min(numChannels, kMaxChannels);
    m_inBlock.store(true, std::memory_order_release);
    m_blocks.fetch_add(1, std::memory_order_release);
    handleReset();
    beginRun(numSamples, numChannels, false);

    const std::int64_t t0 = m_inputPosition;
    float peak = 0.0f;
    writeInput(channels, numChannels, numSamples, peak);
    captureBlock(channels, numChannels, numSamples, peak);
    updateMatching(t0, numSamples);
    renderOutput(channels, numChannels, numSamples, t0);
    m_lastBlockDone.store(nowMicros(), std::memory_order_relaxed);
    m_inBlock.store(false, std::memory_order_release);
}

void Engine::processBypassed(float* const* channels, int numChannels, int numSamples) noexcept
{
    if (! m_prepared.load(std::memory_order_acquire) || numSamples <= 0 || numChannels <= 0)
        return;
    numChannels = std::min(numChannels, kMaxChannels);
    m_inBlock.store(true, std::memory_order_release);
    m_blocks.fetch_add(1, std::memory_order_release);
    handleReset();
    beginRun(numSamples, numChannels, true);

    const std::int64_t t0 = m_inputPosition;
    float peak = 0.0f;
    writeInput(channels, numChannels, numSamples, peak);
    // The input is still the input: Learn goes on capturing while bypassed.
    captureBlock(channels, numChannels, numSamples, peak);
    // Bypass is immediate (the host switches abruptly too): only the delayed input from here.
    m_numEvents = 0;
    m_current = m_previous = Voice {};
    m_fadePosition = m_fadeLength;
    m_match = Match::Off;
    m_segmentResult = nullptr;
    m_searching.store(false, std::memory_order_release);
    m_active.store(false, std::memory_order_relaxed);
    m_located.store(false, std::memory_order_relaxed);
    releaseUnheld();
    renderOutput(channels, numChannels, numSamples, t0);
    m_lastBlockDone.store(nowMicros(), std::memory_order_relaxed);
    m_inBlock.store(false, std::memory_order_release);
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
    const auto clean = [](float v) { return std::isfinite(v) ? v : 0.0f; };   // a bad sample would break the reading of the whole sound

    if (st == CapArmed && peak < kSilence)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < kMaxChannels; ++c)
                m_preRoll[static_cast<std::size_t>(c)][static_cast<std::size_t>(m_preRollPosition)] = clean(channels[std::min(c, numChannels - 1)][i]);
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
        m_captureLoudest = 0.0f;
    }

    const int take = static_cast<int>(std::min<std::int64_t>(numSamples, capacity - length));
    for (int c = 0; c < keep; ++c)
    {
        float* dst = m_capture[static_cast<std::size_t>(c)].get() + length;
        const float* src = channels[std::min(c, numChannels - 1)];
        for (int i = 0; i < take; ++i)
            dst[i] = clean(src[i]);
    }
    length += take;
    // Silent: under -70 dBFS, or 50 dB under the loudest block so far (a noise floor that would
    // otherwise keep Learn listening for ever).
    static const float relative = static_cast<float>(std::pow(10.0, kRelativeSilenceDb / 20.0));
    const float blockPeak = std::isfinite(peak) ? peak : 0.0f;
    const bool silent = blockPeak < kSilence || blockPeak < m_captureLoudest * relative;
    m_captureLoudest = std::max(m_captureLoudest, blockPeak);
    m_silentRun = silent ? m_silentRun + numSamples : 0;
    m_captureLength.store(length, std::memory_order_release);

    int expected = st;
    if (st == CapArmed)
    {
        if (m_captureState.compare_exchange_strong(expected, CapListening, std::memory_order_seq_cst))
        {
            EngineEvent e;
            e.kind = EngineEvent::Kind::CaptureStarted;
            e.i[0] = keep;
            e.d[0] = m_rate.load(std::memory_order_relaxed);
            log(e);
        }
    }
    else if (m_silentRun >= secondsToSamples(kEndSilenceSeconds, m_rate.load(std::memory_order_relaxed)) || length >= capacity)
    {
        m_captureEndWhy.store(length >= capacity ? 3 : 0, std::memory_order_relaxed);
        m_captureState.compare_exchange_strong(expected, CapFinished, std::memory_order_seq_cst);
    }
    // The house thread notices the end within a few milliseconds (no wake-up from here: that
    // could take a lock).
    m_captureWriting.store(false, std::memory_order_seq_cst);
}

bool Engine::isReferenced(const Result* r) const noexcept
{
    if (r == nullptr)
        return false;
    if (r == m_latest || r == m_current.result || r == m_previous.result || r == m_segmentResult)
        return true;
    for (int i = 0; i < m_numEvents; ++i)
        if (m_events[static_cast<std::size_t>(i)].voice.result == r)
            return true;
    return false;
}

void Engine::releaseUnheld() noexcept
{
    // Renders the output can no longer use are let go at once (that only drops a count: the
    // engine's own list still holds them, and frees them on the house thread). After Clear that
    // gives back the learned sound within a block or two.
    for (auto& h : m_held)
        if (h != nullptr && ! isReferenced(h.get()))
            h.reset();
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

void Engine::pushGain(std::int64_t at, double gain) noexcept
{
    const int size = static_cast<int>(m_gainPoints.size());
    if ((m_gainHead + 1) % size == m_gainTail)
        m_gainTail = (m_gainTail + 1) % size;   // full: the oldest goes
    m_gainPoints[static_cast<std::size_t>(m_gainHead)] = { at, static_cast<float>(gain) };
    m_gainHead = (m_gainHead + 1) % size;
}

void Engine::followGain(std::int64_t windowStart, double gain) noexcept
{
    // Each window's level, averaged with its neighbours either side and placed at the middle
    // window's centre: about 60 ms of smoothing with no lag (the output is the latency behind, so
    // the window after is always known in time). The output draws straight lines between points.
    const int W = m_verifyWindow;
    gain = std::clamp(gain, kLowestGain, kHighestGain);
    if (m_gainHistoryCount >= 2)
    {
        const double average = (m_gainHistory[0] + m_gainHistory[1] + gain) / kGainSmoothingWindows;
        pushGain(windowStart - W / 2, snapGain(average));
    }
    m_gainHistory[0] = m_gainHistory[1];
    m_gainHistory[1] = gain;
    m_gainHistoryCount = std::min(2, m_gainHistoryCount + 1);
    m_gainSmoothed = gain;
}

void Engine::beginSearch(std::int64_t from) noexcept
{
    m_match = Match::Waiting;
    m_segmentResult = nullptr;
    ++m_epoch;
    m_searchFrom.store(std::max<std::int64_t>({ from, 0, m_fence.load(std::memory_order_relaxed), m_inputPosition - kRingSize + kRingGuard }),
                       std::memory_order_relaxed);
    m_searchEpoch.store(m_epoch, std::memory_order_release);
    m_searching.store(true, std::memory_order_release);
    m_located.store(false, std::memory_order_relaxed);
}

void Engine::stopMatching(std::int64_t outNext) noexcept
{
    if (m_match != Match::Off || m_current.result != nullptr || m_numEvents > 0)
        pushEvent({ outNext, Voice {} });
    m_match = Match::Off;
    m_segmentResult = nullptr;
    m_searching.store(false, std::memory_order_release);
    m_located.store(false, std::memory_order_relaxed);
}

void Engine::pickUpResult(std::int64_t outNext) noexcept
{
    const auto generation = m_resultGeneration.load(std::memory_order_acquire);
    if (generation == m_seenGeneration)
        return;

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

    const Result* previous = m_latest;
    const Result* old = m_segmentResult;   // the render the current match plays
    const Result* fresh = m_held[slot].get();
    m_latest = fresh;
    const bool sameSound = fresh != nullptr && previous != nullptr && fresh->learned == previous->learned;

    if (sameSound && m_match == Match::Playing && old != nullptr && fresh->playable() && ! rateDiffers(fresh->learned->sampleRate))
    {
        // A new render of the same sound while it plays: carry on from the moment of the sound
        // being heard. Once the start has been heard, that is the old render's position mapped
        // back to the sound and into the new render (the two renders' time maps differ, and in
        // Varispeed the moment heard is not the input's moment). Before it is heard, the start
        // simply moves to the new render.
        const std::int64_t at = std::max(m_segmentStart, outNext);
        double source;
        if (outNext > m_segmentStart)
            source = old->flattened.sourceSampleFor(static_cast<double>(at + m_flatOffset));
        else
            source = static_cast<double>(at + m_sourceOffset);
        m_flatOffset = static_cast<std::int64_t>(std::llround(fresh->flattened.outputSampleFor(source))) - at;
        m_segmentResult = fresh;   // m_segmentStart stays: it says whether the match has been heard yet
        pushEvent({ at, Voice { fresh, m_flatOffset, m_sourceOffset, 1.0f, false } });
    }
    else if (sameSound && m_match == Match::Tail)
    {
        // The old render's tail plays out; the next match uses the new one.
    }
    else if (! sameSound)
    {
        // Another sound (or none): back to the input, and search again for the new one.
        stopMatching(outNext);
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
    const auto gainEnd = m_answerGainEnd.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (m_answerSequence.load(std::memory_order_relaxed) != before)
        return false;   // written meanwhile: next block
    if (epoch != m_epoch || learned != r.learned->id || input < m_fence.load(std::memory_order_relaxed))
        return false;

    m_matchInput = input;
    m_sourceOffset = source - input;
    m_flatOffset = static_cast<std::int64_t>(std::llround(r.flattened.outputSampleFor(static_cast<double>(source)))) - input;
    // Heard when the output reaches the matched input, the latency from now; a late answer starts
    // at once, at the right place for that moment. Every window until then is checked (verify()),
    // and one that fails cancels the start before it is heard.
    m_segmentStart = std::max(input, outNext);
    m_segmentResult = &r;
    m_verifyPosition = input + length;   // the search compared the probe already
    m_holdReported = false;
    // The level starts afresh with this match: at the probe's start and end as the search fitted it.
    m_gainSmoothed = snapGain(gain);
    m_gainTail = m_gainHead;
    m_gainHistoryCount = 0;
    pushGain(input, m_gainSmoothed);
    pushGain(input + length, snapGain(gainEnd));
    m_match = Match::Playing;
    m_searching.store(false, std::memory_order_release);
    m_located.store(true, std::memory_order_relaxed);
    m_lastMatchInput.store(input, std::memory_order_relaxed);
    m_lastMatchSource.store(source, std::memory_order_relaxed);
    m_run[RunMatches].fetch_add(1, std::memory_order_relaxed);
    if (m_run[RunFirstMatchInput].load(std::memory_order_relaxed) < 0)
    {
        m_run[RunFirstMatchInput].store(input, std::memory_order_relaxed);
        m_run[RunFirstMatchSource].store(source, std::memory_order_relaxed);
    }
    {
        const auto& learnedSound = *r.learned;
        const auto end = learnedSound.soundEnd > 0 ? learnedSound.soundEnd : learnedSound.locator.length();
        m_run[RunLearnedEnd].store(end - m_sourceOffset, std::memory_order_relaxed);   // the input position the sound ends at
    }
    pushEvent({ m_segmentStart, Voice { &r, m_flatOffset, m_sourceOffset, static_cast<float>(m_gainSmoothed), true } });
    return true;
}

void Engine::waitForSearch(std::int64_t tEnd) noexcept
{
    // Offline (or faster than real time) the search would answer too late for the latency to hide
    // it. Here the audio thread waits, but only while the output this block reaches would otherwise
    // pass an input position the search has not decided yet, and only while the search can make
    // progress with the input it has.
    const std::int64_t outEnd = tEnd - m_latency;
    if (m_searchGaveUpEpoch == m_epoch)
        return;
    const double started = nowSeconds();
    bool waited = false;
    for (int spins = 0;; ++spins)
    {
        const auto seq = m_answerSequence.load(std::memory_order_acquire);
        if ((seq & 1u) == 0 && m_answerEpoch.load(std::memory_order_relaxed) == m_epoch)
            break;
        if (m_workerEpoch.load(std::memory_order_acquire) == m_epoch)
        {
            if (m_workerDecided.load(std::memory_order_relaxed) >= outEnd || m_workerNeed.load(std::memory_order_relaxed) > tEnd)
                break;
        }
        else if (m_searchFrom.load(std::memory_order_relaxed) >= outEnd)
            break;
        if (m_stop.load())
            break;
        waited = true;
        if ((spins & 63) == 0)
        {
            kickHouse();   // offline or faster than real time only (see Engine.h: Threads)
            if (nowSeconds() - started > m_searchWaitLimit.load(std::memory_order_relaxed))
            {
                m_searchGaveUpEpoch = m_epoch;
                noteWait(0, (nowSeconds() - started) * 1000.0, true);
                return;
            }
        }
        std::this_thread::yield();
    }
    if (waited)
        noteWait(0, (nowSeconds() - started) * 1000.0, false);
}

void Engine::waitForRender() noexcept
{
    // A bounce started right after an option changed must not use the render from before the
    // change: wait for the new one (offline or faster than real time only; a real-time pass carries
    // on and switches when it arrives).
    const auto generation = m_optionsGeneration.load(std::memory_order_acquire);
    if (m_renderGaveUpGeneration == generation)
        return;
    const double started = nowSeconds();
    while (m_renderPending.load(std::memory_order_acquire) && ! m_stop.load())
    {
        if (nowSeconds() - started > m_renderWaitLimit.load(std::memory_order_relaxed))
        {
            m_renderGaveUpGeneration = generation;
            noteWait(1, (nowSeconds() - started) * 1000.0, true);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    noteWait(1, (nowSeconds() - started) * 1000.0, false);
}

void Engine::waitForAnalysis() noexcept
{
    // A sound still being analysed (a render instance that has just been given its state): a fast
    // run waits for it, and its render, so the whole file comes out flattened.
    if (m_analysisGaveUp)
        return;
    const double started = nowSeconds();
    while ((m_analysing.load(std::memory_order_acquire) || m_renderPending.load(std::memory_order_acquire)) && ! m_stop.load())
    {
        if (nowSeconds() - started > m_analysisWaitLimit.load(std::memory_order_relaxed))
        {
            m_analysisGaveUp = true;
            noteWait(2, (nowSeconds() - started) * 1000.0, true);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    noteWait(2, (nowSeconds() - started) * 1000.0, false);
}

void Engine::setWaitLimitsForTesting(double search, double render, double analysis) noexcept
{
    m_searchWaitLimit.store(search);
    m_renderWaitLimit.store(render);
    m_analysisWaitLimit.store(analysis);
}

void Engine::noteWait(int what, double milliseconds, bool gaveUp) noexcept
{
    m_waitedMicros += static_cast<std::int64_t>(milliseconds * 1000.0);
    m_run[RunWaits].fetch_add(1, std::memory_order_relaxed);
    m_run[RunWaitMicros].fetch_add(static_cast<std::int64_t>(milliseconds * 1000.0), std::memory_order_relaxed);
    if (gaveUp)
    {
        m_waitGaveUp.store(true, std::memory_order_relaxed);
        m_run[RunFlags].fetch_or(4, std::memory_order_relaxed);
    }
    if (gaveUp || milliseconds >= kNotableWaitMs)
    {
        EngineEvent e;
        e.kind = EngineEvent::Kind::Wait;
        e.i[0] = what;
        e.i[1] = gaveUp ? 1 : 0;
        e.d[0] = milliseconds;
        log(e);
    }
}

void Engine::measurePace(int numSamples) noexcept
{
    // Audio against the wall clock over the last blocks (see the header). Each block's arrival
    // time against the one before it covers the audio of the blocks in between.
    const std::int64_t now = m_lastBlockWall;
    const double rate = m_rate.load(std::memory_order_relaxed);
    m_paceWall[static_cast<std::size_t>(m_paceHead)] = now;
    m_paceSamples[static_cast<std::size_t>(m_paceHead)] = numSamples;
    m_paceHead = (m_paceHead + 1) % kPaceBlocks;
    m_paceCount = std::min(m_paceCount + 1, kPaceBlocks);
    std::int64_t samples = 0;
    for (int k = 1; k < m_paceCount; ++k)
    {
        const auto at = static_cast<std::size_t>((m_paceHead - 1 - k + 2 * kPaceBlocks) % kPaceBlocks);
        samples += m_paceSamples[at];
        if (k >= kPaceWindowBlocks && static_cast<double>(samples) >= kPaceWindowSeconds * rate)
        {
            const double wall = static_cast<double>(std::max<std::int64_t>(1, now - m_paceWall[at])) / 1.0e6;
            const double ratio = static_cast<double>(samples) / rate / wall;
            const bool fast = m_fast.load(std::memory_order_relaxed);
            if (fast != (fast ? ratio >= kRealTimeRatio : ratio > kFastRatio))
            {
                m_fast.store(! fast, std::memory_order_relaxed);
                EngineEvent e;
                e.kind = EngineEvent::Kind::PaceChanged;
                e.i[0] = fast ? 0 : 1;
                e.d[0] = ratio;
                log(e);
            }
            return;
        }
    }
}

void Engine::beginRun(int numSamples, int numChannels, bool bypassed) noexcept
{
    const std::int64_t now = nowMicros();
    const auto lastDone = m_lastBlockDone.load(std::memory_order_relaxed);
    if (lastDone != 0 && static_cast<double>(now - lastDone) > kRunGapSeconds * 1.0e6)
    {
        endRun();
        m_paceCount = 0;
        m_fast.store(false, std::memory_order_relaxed);
    }
    // The pace clock leaves out the time this engine spent waiting: that is not the host's pace.
    m_lastBlockWall = now - m_waitedMicros;
    measurePace(numSamples);
    if (! m_runOpen.load(std::memory_order_acquire))
    {
        const auto relaxed = std::memory_order_relaxed;
        m_run[RunIndex].store(++m_runCount, relaxed);
        for (const auto field : { RunBlocks, RunSamples, RunMatches, RunFlattened, RunDry, RunFlags, RunWaits, RunWaitMicros })
            m_run[static_cast<std::size_t>(field)].store(0, relaxed);
        m_run[RunSearchesAtStart].store(static_cast<std::int64_t>(m_searches.load(relaxed)), relaxed);
        m_run[RunCancelledAtStart].store(static_cast<std::int64_t>(m_cancelledStarts.load(relaxed)), relaxed);
        m_run[RunFirstMatchInput].store(-1, relaxed);
        m_run[RunFirstMatchSource].store(-1, relaxed);
        m_run[RunLearnedEnd].store(-1, relaxed);
        m_run[RunLatency].store(m_latency, relaxed);
        m_run[RunStartWall].store(now, relaxed);
        m_run[RunStartInput].store(m_inputPosition, relaxed);
        m_runOpen.store(true, std::memory_order_release);
        EngineEvent e;
        e.kind = EngineEvent::Kind::RunStarted;
        e.i[0] = m_runCount;
        e.i[1] = numSamples;
        e.i[2] = numChannels;
        e.i[3] = m_nonRealtime.load(relaxed) ? 1 : 0;
        e.i[4] = bypassed ? 1 : 0;
        e.d[0] = m_rate.load(relaxed);
        log(e);
    }
    const auto relaxed = std::memory_order_relaxed;
    m_run[RunBlocks].fetch_add(1, relaxed);
    m_run[RunSamples].fetch_add(numSamples, relaxed);
    m_run[RunLastWall].store(now, relaxed);
    m_run[RunInputEnd].store(m_inputPosition + numSamples, relaxed);
    m_run[RunFlags].fetch_or((m_nonRealtime.load(relaxed) ? 1 : 0) | (m_fast.load(relaxed) ? 2 : 0), relaxed);
}

void Engine::endRun() noexcept
{
    if (! m_runOpen.exchange(false, std::memory_order_acq_rel))
        return;
    const auto get = [this](int field) { return m_run[static_cast<std::size_t>(field)].load(std::memory_order_relaxed); };
    EngineEvent e;
    e.kind = EngineEvent::Kind::RunEnded;
    const auto startInput = get(RunStartInput);
    e.i[0] = get(RunIndex);
    e.i[1] = get(RunBlocks);
    e.i[2] = get(RunSamples);
    e.i[3] = static_cast<std::int64_t>(m_searches.load(std::memory_order_relaxed)) - get(RunSearchesAtStart);
    e.i[4] = get(RunMatches);
    e.i[5] = static_cast<std::int64_t>(m_cancelledStarts.load(std::memory_order_relaxed)) - get(RunCancelledAtStart);
    e.i[6] = get(RunFirstMatchInput) >= 0 ? get(RunFirstMatchInput) - startInput : 0;
    e.i[14] = get(RunFirstMatchInput) >= 0 ? 1 : 0;
    e.i[7] = get(RunFirstMatchSource);
    e.i[8] = get(RunFlattened);
    e.i[9] = get(RunDry);
    e.i[10] = get(RunLatency);
    e.i[11] = get(RunLearnedEnd) >= 0 ? get(RunInputEnd) - get(RunLearnedEnd) : -1;
    e.i[12] = get(RunFlags);
    e.i[13] = get(RunWaits);
    e.d[0] = m_rate.load(std::memory_order_relaxed);
    e.d[1] = static_cast<double>(get(RunLastWall) - get(RunStartWall)) / 1.0e6;
    e.d[2] = static_cast<double>(get(RunWaitMicros)) / 1000.0;
    log(e);
}

void Engine::failMatch(std::int64_t at, std::int64_t outNext) noexcept
{
    // The input stopped being the learned sound at input position `at`. A start not yet heard is
    // cancelled outright; otherwise the output fades back to the input, the fade ending at `at`,
    // so nothing after the checked input is heard.
    if (outNext <= m_segmentStart)
    {
        pushEvent({ m_segmentStart, Voice {} });
        m_cancelledStarts.fetch_add(1, std::memory_order_relaxed);
    }
    else
        pushEvent({ std::max(at - m_fadeLength, outNext), Voice {} });
    m_failures.fetch_add(1, std::memory_order_relaxed);   // before the new epoch: the house thread backs off
    beginSearch(at);
}

void Engine::verify(const Result& r, std::int64_t tEnd, std::int64_t outNext) noexcept
{
    const Learned& learned = *r.learned;
    const Locator& locator = learned.locator;
    const float* mid = locator.samples();
    const float* side = learned.side.empty() ? nullptr : learned.side.data();
    const std::int64_t learnedLength = learned.soundEnd > 0 ? learned.soundEnd : locator.length();
    const std::int64_t flatLength = static_cast<std::int64_t>(r.flattened.channels[0].size());
    const int W = std::min(m_verifyWindow, std::max(1, locator.probeLength()));
    const double silent = static_cast<double>(W) * kSilence * kSilence;
    const bool checkSide = side != nullptr && m_inputStereo;

    while (m_match == Match::Playing && m_verifyPosition + W <= tEnd)
    {
        const std::int64_t v = m_verifyPosition;
        if (v < tEnd - kRingSize + kRingGuard)
        {
            failMatch(v, outNext);   // a block larger than the ring: nothing left to check against
            return;
        }
        const std::int64_t source = v + m_sourceOffset;
        if (source >= learnedLength || source < 0)
        {
            // Past the end of the sound. A longer render plays out its tail (until the input is
            // no longer silent); a shorter one has finished, and the input comes back when the
            // sound would have ended.
            const std::int64_t learnedEnd = learnedLength - m_sourceOffset, flatEnd = flatLength - m_flatOffset;
            if (flatEnd > std::max(learnedEnd, outNext))
            {
                m_match = Match::Tail;
                m_tailEnd = flatEnd;
                watchTail(tEnd, outNext);
            }
            else
            {
                // The search starts again exactly where the sound ended: a loop played back to back
                // is found again at its seam.
                pushEvent({ std::max(learnedEnd, outNext), Voice {} });
                beginSearch(learnedEnd);
            }
            return;
        }
        // The last window may run past the end of the sound: only the part inside it counts (what
        // follows may be the next sound already). A sliver is not worth checking: the sound ended.
        const int n = static_cast<int>(std::min<std::int64_t>(W, learnedLength - source));
        if (n < W / 4)
        {
            m_verifyPosition = learnedLength - m_sourceOffset;
            continue;
        }
        const double silentHere = silent * static_cast<double>(n) / static_cast<double>(W);

        double xx = 0.0, yy = 0.0, xy = 0.0, ss = 0.0, tt = 0.0, st = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const auto at = static_cast<std::size_t>((v + i) & kRingMask);
            const double x = m_ring[at].load(std::memory_order_relaxed);
            const double y = mid[source + i];
            xx += x * x;
            yy += y * y;
            xy += x * y;
            if (checkSide)
            {
                const double s = m_sideRing[at].load(std::memory_order_relaxed);
                const double t = side[source + i];
                ss += s * s;
                tt += t * t;
                st += s * t;
            }
        }

        bool same;
        if (yy < silentHere)
        {
            // The learned sound is silent here: the input must be too (scaled by the level it is
            // played at), or something else is playing.
            const double g = std::max(1.0, m_gainSmoothed);
            same = xx < silentHere * g * g;
        }
        else
        {
            // Anything else must be the learned recording (silence in the input is not: the player
            // stopped, perhaps in a quiet stretch between two sounds).
            same = matching::residual(xx, yy, xy) <= matching::kKeepResidual;
            const double g = yy > 0.0 ? xy / yy : 0.0;
            // In stereo, the difference of the channels too, where it carries real energy: a copy
            // with the channels swapped has the same mix.
            if (same && checkSide && tt * g * g >= std::max(silentHere, 0.01 * yy * g * g))
            {
                const double left = ss - 2.0 * g * st + g * g * tt;
                same = left <= matching::kKeepResidual * g * g * tt;
            }
            if (same)
                followGain(v, g);   // follow the level
        }
        if (! same)
        {
            failMatch(v, outNext);
            return;
        }
        m_verifyPosition = v + n;
        if (! m_holdReported && m_verifyPosition - m_matchInput >= secondsToSamples(kHeldSeconds, m_rate.load(std::memory_order_relaxed)))
        {
            m_holdReported = true;
            m_holds.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (m_match == Match::Tail)
        watchTail(tEnd, outNext);
}

void Engine::watchTail(std::int64_t tEnd, std::int64_t outNext) noexcept
{
    // The render outlasts the sound. It plays out while the input is silent; as soon as anything
    // plays (the next file), the input comes back at once, with the usual fade.
    const int W = m_verifyWindow;
    const double silent = static_cast<double>(W) * kSilence * kSilence;
    while (m_verifyPosition + W <= tEnd)
    {
        const std::int64_t v = m_verifyPosition;
        if (v >= m_tailEnd)
        {
            pushEvent({ std::max(m_tailEnd, outNext), Voice {} });
            beginSearch(v);
            return;
        }
        double xx = 0.0;
        for (int i = 0; i < W; ++i)
        {
            const double x = m_ring[static_cast<std::size_t>((v + i) & kRingMask)].load(std::memory_order_relaxed);
            xx += x * x;
        }
        if (xx >= silent)
        {
            pushEvent({ std::max(v, outNext), Voice {} });
            beginSearch(v);
            return;
        }
        m_verifyPosition = v + W;
    }
}

void Engine::updateMatching(std::int64_t t0, int numSamples) noexcept
{
    const std::int64_t tEnd = t0 + numSamples;
    const std::int64_t outNext = t0 - m_latency;   // the input position the next output sample is
    // Offline as the host says, or as the pace of the audio shows (see the header).
    const bool offline = m_nonRealtime.load(std::memory_order_relaxed) || m_fast.load(std::memory_order_relaxed);

    releaseUnheld();
    const int capture = m_captureState.load(std::memory_order_relaxed);
    if (! m_analysing.load(std::memory_order_acquire))
        m_analysisGaveUp = false;   // that analysis is over: the next one may be waited for
    if (offline && capture == CapIdle && m_analysing.load(std::memory_order_acquire))
        waitForAnalysis();
    const bool analysing = m_analysing.load(std::memory_order_acquire);
    if (offline && ! analysing && m_renderPending.load(std::memory_order_acquire))
        waitForRender();
    pickUpResult(outNext);
    const Result* r = m_latest;
    const bool rateMismatch = r != nullptr && r->learned != nullptr && rateDiffers(r->learned->sampleRate);
    m_rateMismatch.store(rateMismatch, std::memory_order_relaxed);
    const int cap = m_captureState.load(std::memory_order_relaxed);
    // Nothing plays while Learn listens or a new sound is analysed: the window says so, and the
    // audio agrees.
    const bool playable = r != nullptr && r->playable() && ! rateMismatch && ! analysing && cap == CapIdle;

    if (! playable)
    {
        if (m_match != Match::Off)
            stopMatching(outNext);
        m_active.store(false, std::memory_order_relaxed);
        return;
    }
    m_active.store(true, std::memory_order_relaxed);
    if (m_match == Match::Off)
        beginSearch(outNext);

    if (m_match == Match::Waiting)
    {
        if (offline)
            waitForSearch(tEnd);
        readAnswer(*r, outNext);
    }
    if (m_match == Match::Playing || m_match == Match::Tail)
    {
        const Result* playing = m_segmentResult != nullptr ? m_segmentResult : r;
        if (m_match == Match::Playing)
            verify(*playing, tEnd, outNext);
        else
            watchTail(tEnd, outNext);
    }
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
        return m_gainNow * ch[0][i];                          // mono sound: every channel
    if (numChannels == 1)
        return m_gainNow * (0.5f * (ch[0][i] + ch[1][i]));    // stereo sound, mono track: the two halves summed
    return m_gainNow * ch[static_cast<std::size_t>(std::min(channel, 1))][i];
}

void Engine::renderOutput(float* const* channels, int numChannels, int numSamples, std::int64_t t0) noexcept
{
    const int L = m_latency;
    const int gainPoints = static_cast<int>(m_gainPoints.size());
    std::int64_t flattened = 0;
    for (int i = 0; i < numSamples; ++i)
    {
        const std::int64_t u = t0 + i - L;   // the input position this output sample is
        while (m_numEvents > 0 && m_events[0].at <= u)
        {
            const Voice next = m_events[0].voice;
            for (int k = 1; k < m_numEvents; ++k)
                m_events[static_cast<std::size_t>(k - 1)] = m_events[static_cast<std::size_t>(k)];
            --m_numEvents;
            if (next.newSegment)
            {
                // A new match starts at its own level.
                m_gainNow = next.gain;
                m_gainRampLeft = 0;
            }
            if (next == m_current)
                continue;
            m_previous = m_current;
            m_current = next;
            m_fadePosition = 0;
        }
        // The level as measured on the input, in straight lines from point to point, so it never
        // clicks and never lags. At a point the level is exactly that point's (1 for the same file).
        while (m_gainTail != m_gainHead && m_gainPoints[static_cast<std::size_t>(m_gainTail)].at <= u)
        {
            const GainPoint here = m_gainPoints[static_cast<std::size_t>(m_gainTail)];
            m_gainTail = (m_gainTail + 1) % gainPoints;
            m_gainNow = here.gain;
            m_gainRampLeft = 0;
            if (m_gainTail != m_gainHead)
            {
                const GainPoint next = m_gainPoints[static_cast<std::size_t>(m_gainTail)];
                const std::int64_t distance = next.at - here.at;
                if (distance > 0 && (next.gain < here.gain || next.gain > here.gain))
                {
                    m_gainRampLeft = distance;
                    m_gainStep = (next.gain - here.gain) / static_cast<float>(distance);
                }
            }
        }
        if (m_gainRampLeft > 0)
        {
            m_gainNow += m_gainStep;
            --m_gainRampLeft;
        }

        flattened += m_current.result != nullptr ? 1 : 0;
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
        if (fading && ++m_fadePosition >= m_fadeLength)
            m_previous = Voice {};   // faded out: let go of it
    }
    m_run[RunFlattened].fetch_add(flattened, std::memory_order_relaxed);
    m_run[RunDry].fetch_add(numSamples - flattened, std::memory_order_relaxed);
    publishReadouts(t0 + numSamples - 1 - L);
}

void Engine::publishReadouts(std::int64_t u) noexcept
{
    Playback playback = m_match != Match::Off ? Playback::Searching : Playback::Idle;
    if (const Result* r = m_current.result)
    {
        const std::int64_t index = u + m_current.flatOffset;
        m_playPosition.store(index, std::memory_order_relaxed);
        m_playGeneration.store(r->generation, std::memory_order_relaxed);
        if (index >= static_cast<std::int64_t>(r->flattened.channels[0].size()))
            playback = Playback::FlattenedEnded;
        else if (u + m_current.sourceOffset >= r->learned->locator.length())
            playback = Playback::Tail;
        else
            playback = Playback::Flattened;
        m_playingCurrent.store(r->generation == m_currentRender.load(std::memory_order_acquire), std::memory_order_relaxed);
    }
    else
    {
        m_playPosition.store(-1, std::memory_order_relaxed);
        m_playingCurrent.store(false, std::memory_order_relaxed);
    }
    m_playback.store(static_cast<int>(playback), std::memory_order_relaxed);
}

} // namespace pitchflattener
