#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Audition.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>

using namespace tonalrecall;

namespace
{
    std::atomic<std::uint32_t> g_nextInstanceId { 1 };
    std::atomic<int> g_liveInstances { 0 };

    // Caps per run, so a host that varies its block size (or position) every block cannot turn
    // the log into a per-block log.
    constexpr std::int64_t kMaxDiscontinuityLines = 50;
    constexpr std::int64_t kMaxBlockSizeLines = 20;

    ThreadKind callerThread() noexcept
    {
        return juce::MessageManager::existsAndIsCurrentThread() ? ThreadKind::Message : ThreadKind::Other;
    }

    // ---- Audio-thread events: raw numbers in, text out on the log's writer thread. ----

    enum FirstBlockFlag : std::uint32_t
    {
        fbNonRealtime = 1u << 0, fbHasPlayHead = 1u << 1, fbHasPosition = 1u << 2,
        fbTimeSamples = 1u << 3, fbTimeSeconds = 1u << 4, fbBpm = 1u << 5, fbLoopPoints = 1u << 6,
        fbHostTime = 1u << 7, fbEditOrigin = 1u << 8, fbPpq = 1u << 9, fbTimeSig = 1u << 10,
        fbPlaying = 1u << 11, fbRecording = 1u << 12, fbLooping = 1u << 13, fbFrameRate = 1u << 14,
        fbBarCount = 1u << 15, fbAudioSuite = 1u << 16
    };

    // Appends to out, keeping track of what is left.
    struct Writer
    {
        char* out;
        std::size_t size;
        std::size_t used = 0;

        void operator()(const char* fmt, ...)
        #if defined(__GNUC__) || defined(__clang__)
            __attribute__((format(printf, 2, 3)))
        #endif
        {
            if (used >= size)
                return;
            va_list args;
            va_start(args, fmt);
            const int n = std::vsnprintf(out + used, size - used, fmt, args);
            va_end(args);
            if (n > 0)
                used = std::min(size, used + static_cast<std::size_t>(n));
        }
    };

    void formatFirstBlock(const LogEvent& e, char* out, std::size_t size)
    {
        Writer w { out, size };
        const auto has = [&](std::uint32_t f) { return (e.flags & f) != 0; };
        w("FIRST BLOCK of run: numSamples %" PRId64 ", channels %" PRId64 ", sampleRate %.1f, isNonRealtime %s, inAudioSuite %s",
          e.i[0], e.i[1], e.d[0], has(fbNonRealtime) ? "yes" : "no", has(fbAudioSuite) ? "yes" : "no");
        if (e.i[7] > 0) w(" | selection %" PRId64 " samples (%.3f s, from the loop points and tempo)", e.i[7], e.d[0] > 0.0 ? static_cast<double>(e.i[7]) / e.d[0] : 0.0);
        else w(" | selection length unknown");
        if (! has(fbHasPlayHead)) { w(" | playhead: none"); return; }
        if (! has(fbHasPosition)) { w(" | playhead: present, getPosition(): none"); return; }
        w(" | playhead:");
        if (has(fbTimeSamples)) w(" timeInSamples %" PRId64, e.i[2]); else w(" timeInSamples absent");
        if (has(fbTimeSeconds)) w(", timeInSeconds %.6f", e.d[1]); else w(", timeInSeconds absent");
        w(", isPlaying %s, isRecording %s, isLooping %s", has(fbPlaying) ? "yes" : "no",
          has(fbRecording) ? "yes" : "no", has(fbLooping) ? "yes" : "no");
        if (has(fbLoopPoints)) w(", loopPoints ppq %.4f..%.4f", e.d[3], e.d[4]); else w(", loopPoints absent");
        if (has(fbBpm)) w(", bpm %.3f", e.d[2]); else w(", bpm absent");
        if (has(fbTimeSig)) w(", timeSig %" PRId64 "/%" PRId64, e.i[4], e.i[5]); else w(", timeSig absent");
        if (has(fbPpq)) w(", ppqPosition %.4f", e.d[6]); else w(", ppqPosition absent");
        if (has(fbBarCount)) w(", barCount %" PRId64, e.i[6]); else w(", barCount absent");
        if (has(fbHostTime)) w(", hostTimeNs %" PRIu64, static_cast<std::uint64_t>(e.i[3])); else w(", hostTimeNs absent");
        if (has(fbEditOrigin)) w(", editOriginTime %.6f", e.d[5]); else w(", editOriginTime absent");
        if (has(fbFrameRate)) w(", frameRate %.3f fps", e.d[7]); else w(", frameRate absent");
    }

    void formatDiscontinuity(const LogEvent& e, char* out, std::size_t size)
    {
        std::snprintf(out, size, "timeInSamples discontinuity at block %" PRId64 ": expected %" PRId64 ", got %" PRId64
                      " (%s by %" PRId64 " samples)%s",
                      e.i[3], e.i[0], e.i[1], e.i[2] < 0 ? "back" : "forward", e.i[2] < 0 ? -e.i[2] : e.i[2],
                      e.flags != 0 ? " [further discontinuities this run are not logged]" : "");
    }

    void formatTimeGone(const LogEvent& e, char* out, std::size_t size)
    {
        std::snprintf(out, size, "timeInSamples %s at block %" PRId64,
                      e.flags != 0 ? "appeared" : "disappeared", e.i[0]);
    }

    void formatFrozen(const LogEvent& e, char* out, std::size_t size)
    {
        std::snprintf(out, size, "timeInSamples is not advancing: block %" PRId64 " reports %" PRId64
                      " again (the host repeats one position per block)", e.i[1], e.i[0]);
    }

    void formatBlockSize(const LogEvent& e, char* out, std::size_t size)
    {
        std::snprintf(out, size, "block size changed at block %" PRId64 ": %" PRId64 " -> %" PRId64 "%s",
                      e.i[2], e.i[0], e.i[1], e.flags != 0 ? " [further changes this run are not logged]" : "");
    }

    void formatPassStart(const LogEvent& e, char* out, std::size_t size)
    {
        const char* how = e.flags & 4 ? "merged in front of the earlier capture"
                        : e.flags & 1 ? "merged with the earlier capture by timeline position"
                        : e.flags & 2 ? "replaced the earlier capture"
                                      : "new capture";
        if (e.flags & 8)
            std::snprintf(out, size, "%s: PASS START at timeline %" PRId64 " (%s)", slotName(static_cast<Slot>(e.i[0])), e.i[1], how);
        else
            std::snprintf(out, size, "%s: PASS START, timeline unknown (%s)", slotName(static_cast<Slot>(e.i[0])), how);
    }

    void formatPassEnd(const LogEvent& e, char* out, std::size_t size)
    {
        Writer w { out, size };
        w("%s: PASS END (%s), %" PRId64 " samples in this pass; disarmed", slotName(static_cast<Slot>(e.i[0])),
          passEndName(static_cast<PassEnd>(e.i[1])), e.i[2]);
        if (e.flags & 1)
            w("; block timeline %" PRId64 " where %" PRId64 " was expected", e.i[3], e.i[4]);
    }

    void formatFrozenPass(const LogEvent& e, char* out, std::size_t size)
    {
        std::snprintf(out, size, "%s: the pass's timeline positions are approximate (not one per block): the pass "
                      "continues, and ends only if the position goes backwards or leaps ahead", slotName(static_cast<Slot>(e.i[0])));
    }

    void formatSkip(const LogEvent& e, char* out, std::size_t size)
    {
        const auto slot = slotName(static_cast<Slot>(e.i[0]));
        switch (e.i[1])
        {
            case 0:  std::snprintf(out, size, "%s: armed, but instance %" PRId64 " owns the pass in progress: this instance passes audio through without capturing (logged once per run)", slot, e.i[2]); break;
            case 1:  std::snprintf(out, size, "%s: armed, but the capture buffers were not allocated yet: block not captured", slot); break;
            default: std::snprintf(out, size, "%s: a block was not captured (store busy); it is kept as a gap", slot); break;
        }
    }

    void formatNonRealtime(const LogEvent& e, char* out, std::size_t size)
    {
        std::snprintf(out, size, "setNonRealtime(%s)%s", e.flags & 1 ? "true" : "false",
                      e.flags & 2 ? " (changed)" : " (unchanged)");
    }

    const char* modeName(std::int64_t mode) noexcept
    {
        switch (static_cast<StampRunInfo::Mode>(mode))
        {
            case StampRunInfo::Mode::Dry:   return "dry (Stamp off, or no match for this audio)";
            case StampRunInfo::Mode::Learn: return "learn (dry while a slot is armed)";
            case StampRunInfo::Mode::Stamp: return "stamp";
            case StampRunInfo::Mode::None:  break;
        }
        return "none";
    }

    // i: 0 mode, 1 plan generation, 2 aligned, 3 offset, 4 samples, 5 latency, 6 selection,
    //    7 non-realtime, 8 stamp output on at the end, 9 run index; d: 0 semitones, 1 sample rate; flags: bend.
    void formatStampSummary(const LogEvent& e, char* out, std::size_t size)
    {
        Writer w { out, size };
        const double sr = e.d[1];
        const auto secs = [sr](std::int64_t n) { return sr > 0.0 ? static_cast<double>(n) / sr : 0.0; };
        w("STAMP RUN %" PRId64 " END: %s, mode %s", e.i[9], e.i[7] != 0 ? "non-realtime (Render)" : "real time (Preview)", modeName(e.i[0]));
        if (static_cast<StampRunInfo::Mode>(e.i[0]) == StampRunInfo::Mode::Stamp)
        {
            w(", output %s at the end, plan generation %" PRId64 ", shift %+.3f st, bend %s", e.i[8] != 0 ? "stamped" : "dry (Stamp switched off)",
              e.i[1], e.d[0], e.flags & 1 ? "yes" : "no");
            if (e.i[2] != 0) w(", alignment found at stream sample %" PRId64 " (%.3f s)", e.i[3], secs(e.i[3]));
            else w(", alignment NOT found (fixed shift throughout)");
        }
        w(", stream %" PRId64 " samples (%.3f s), reported latency %" PRId64 " samples", e.i[4], secs(e.i[4]), e.i[5]);
        if (e.i[7] != 0)
        {
            if (e.i[6] > 0)
                w("; delivered %" PRId64 " samples, selection %" PRId64 " samples, delivered - selection = %" PRId64
                  " (handles, plus the latency if the host compensates it)", e.i[4], e.i[6], e.i[4] - e.i[6]);
            else
                w("; delivered %" PRId64 " samples, selection length unknown", e.i[4]);
        }
    }

    void post(std::uint32_t id, LogEvent::FormatFn fn, LogEvent& e, ThreadKind kind = ThreadKind::Audio) noexcept
    {
        e.instance = id;
        e.thread = kind;
        e.format = fn;
        DiagnosticLog::instance().postEvent(e);
    }
}

//==============================================================================
TonalRecallProcessor::TonalRecallProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      m_id(g_nextInstanceId.fetch_add(1))
{
    auto& log = DiagnosticLog::instance();
    log.retain();
    CaptureStore::instance().ensureAllocated();
    MatchHub::instance().retain();
    const int live = g_liveInstances.fetch_add(1) + 1;

    const juce::PluginHostType host;
    log.post(m_id, callerThread(), "processor constructed (live instances %d); version %s; wrapper %s; host \"%s\"; log %s",
             live, PLUGIN_VERSION, getWrapperTypeDescription(wrapperType), host.getHostDescription(),
             log.filePath().c_str());

    // Ends stalled passes and closes runs even when no editor is open.
    startTimer(50);
}

TonalRecallProcessor::~TonalRecallProcessor()
{
    stopTimer();
    pollRunState();
    if (m_srSummaryPending.exchange(false))
        postStampSummary(callerThread());
    freeRetiredStamper();
    m_stamper.reset();
    MatchHub::instance().release();
    const auto ended = CaptureStore::instance().instanceGone(m_id);
    for (int s = 0; s < kNumSlots; ++s)
        if (ended[static_cast<std::size_t>(s)] != PassEnd::None)
            DiagnosticLog::instance().post(m_id, callerThread(), "%s: PASS END (%s); disarmed",
                                           slotName(static_cast<Slot>(s)), passEndName(ended[static_cast<std::size_t>(s)]));
    const int live = g_liveInstances.fetch_sub(1) - 1;
    auto& log = DiagnosticLog::instance();
    log.post(m_id, callerThread(), "processor destroyed (live instances %d)", live);
    log.release();
}

int TonalRecallProcessor::liveInstanceCount() noexcept
{
    return g_liveInstances.load();
}

void TonalRecallProcessor::refreshAudioSuiteFlag(const char* when)
{
    // Only valid once the AAX wrapper has registered this instance, so it is re-read on the timer.
    const bool inAudioSuite = juce::PluginHostType::isInAAXAudioSuite(*this);
    m_inAudioSuite.store(inAudioSuite);
    // Tonal Recall is an AudioSuite tool. As a real-time insert in Pro Tools it does nothing but
    // pass audio through (the wrapper cannot be told not to offer the insert), and its editor says
    // where to find it.
    m_aaxInsert.store(wrapperType == wrapperType_AAX && m_prepared.load() && ! inAudioSuite);
    const int now = inAudioSuite ? 1 : 0;
    if (m_inAudioSuiteLogged.exchange(now) != now)
        DiagnosticLog::instance().post(m_id, callerThread(), "in AudioSuite: %s (checked %s)", inAudioSuite ? "yes" : "no", when);
}

void TonalRecallProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    CaptureStore::instance().ensureAllocated();
    m_sampleRate.store(sampleRate);
    const auto in = getChannelLayoutOfBus(true, 0).getDescription();
    const auto out = getChannelLayoutOfBus(false, 0).getDescription();
    DiagnosticLog::instance().post(m_id, callerThread(),
        "prepareToPlay(sampleRate %.1f, blockSize %d); layout in \"%s\" (%d ch) out \"%s\" (%d ch); isNonRealtime %s",
        sampleRate, samplesPerBlock, in.toRawUTF8(), getTotalNumInputChannels(), out.toRawUTF8(),
        getTotalNumOutputChannels(), isNonRealtime() ? "yes" : "no");
    m_inAudioSuiteLogged.store(-1);
    m_prepared.store(true);
    refreshAudioSuiteFlag("in prepareToPlay");

    // The Stamper's latency depends only on the sample rate (not on the shift), so a throwaway
    // one tells us what to report. Offline every mode is delayed by it, so the host's
    // compensation lines a Render up whatever the plug-in did.
    const int channels = std::max(1, getTotalNumOutputChannels());
    core::Stamper::Setup probe;
    probe.sampleRate = sampleRate;
    probe.channels = channels;
    const int latency = core::Stamper(probe).latencySamples();
    probe.preserveFormants = true;
    const int latencyFormants = core::Stamper(probe).latencySamples();
    m_latency.store(latency);
    // (An insert in Pro Tools only passes audio through: it adds no delay.)
    setLatencySamples(m_aaxInsert.load() ? 0 : latency);
    DiagnosticLog::instance().post(m_id, callerThread(), "reported latency %d samples (%.1f ms)%s", latency,
                                   sampleRate > 0.0 ? 1000.0 * latency / sampleRate : 0.0,
                                   latencyFormants != latency ? "; WARNING: differs with formants preserved" : "");

    // The delay line for dry audio offline, and the scratch that holds the dry signal while a
    // Stamp run is switched off mid-run.
    m_delay.assign(static_cast<std::size_t>(std::max(2, channels)), std::vector<float>(static_cast<std::size_t>(std::max(1, latency)), 0.0f));
    m_dryScratch.assign(m_delay.size(), std::vector<float>(static_cast<std::size_t>(kScratchSamples), 0.0f));
    m_delayPos = 0;
    m_stamper.reset();
    freeRetiredStamper();
    m_resetRequested.store(true);
}

void TonalRecallProcessor::releaseResources()
{
    DiagnosticLog::instance().post(m_id, callerThread(), "releaseResources()");
}

void TonalRecallProcessor::reset()
{
    // Hosts may call this from any thread; this line formats no floating point. The next block
    // starts a new Stamp run.
    m_resetRequested.store(true);
    DiagnosticLog::instance().post(m_id, callerThread(), "reset()");
}

void TonalRecallProcessor::setNonRealtime(bool nonRealtime) noexcept
{
    const bool changed = isNonRealtime() != nonRealtime;
    AudioProcessor::setNonRealtime(nonRealtime);
    LogEvent e;
    e.flags = (nonRealtime ? 1u : 0u) | (changed ? 2u : 0u);
    post(m_id, formatNonRealtime, e, callerThread());
}

bool TonalRecallProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void TonalRecallProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Passthrough: the buffer is never written, so the output is the input, bit for bit.
    const int numSamples = buffer.getNumSamples();
    const int numChannels = std::min(getTotalNumInputChannels(), buffer.getNumChannels());
    const auto nowNs = CaptureStore::now();

    auto* playHead = getPlayHead();
    juce::Optional<juce::AudioPlayHead::PositionInfo> info;
    if (playHead != nullptr)
        info = playHead->getPosition();
    const auto timeOpt = info ? info->getTimeInSamples() : juce::nullopt;
    std::optional<std::int64_t> time;
    if (timeOpt)
        time = *timeOpt;

    // The selection's length. The host gives the selection as loop points in quarter notes; at
    // the tempo it reports that is this many samples (exact while the tempo is constant across
    // the selection).
    std::optional<std::int64_t> selectionLength;
    if (info && (m_inAudioSuite.load(std::memory_order_relaxed) || m_loopIsSelection.load(std::memory_order_relaxed)))
    {
        const auto loop = info->getLoopPoints();
        const auto bpm = info->getBpm();
        const double sr = m_sampleRate.load(std::memory_order_relaxed);
        if (loop && bpm && *bpm > 0.0 && loop->ppqEnd > loop->ppqStart && sr > 0.0)
        {
            const auto length = std::llround((loop->ppqEnd - loop->ppqStart) * 60.0 / *bpm * sr);
            if (length > 0)
                selectionLength = length;
        }
    }

    if (selectionLength)
        m_srSelection.store(*selectionLength, std::memory_order_relaxed);

    // ---- this instance's run, for the log ----
    const bool newRun = ! m_runActive.load(std::memory_order_acquire);
    if (newRun)
    {
        m_runBlocks.store(0, std::memory_order_relaxed);
        m_runSamples.store(0, std::memory_order_relaxed);
        m_runHasTime.store(time.has_value(), std::memory_order_relaxed);
        m_runFirstTime.store(time.value_or(0), std::memory_order_relaxed);
        m_runLastTime.store(time.value_or(0), std::memory_order_relaxed);
        m_runStartNs.store(nowNs, std::memory_order_relaxed);
        m_runDiscontinuities.store(0, std::memory_order_relaxed);
        m_runSizeChanges.store(0, std::memory_order_relaxed);
        m_runMinBlock.store(numSamples, std::memory_order_relaxed);
        m_runMaxBlock.store(numSamples, std::memory_order_relaxed);
        m_runSkippedOwner.store(0, std::memory_order_relaxed);
        m_runFrozen.store(false, std::memory_order_relaxed);
        m_runChannels.store(numChannels, std::memory_order_relaxed);
        m_runNonRealtime.store(isNonRealtime(), std::memory_order_relaxed);
        m_lastBlockNs.store(nowNs, std::memory_order_relaxed);
        for (auto& logged : m_skipLogged)
            logged = false;
        m_lastBlockSize = numSamples;
        m_runActive.store(true, std::memory_order_release);

        LogEvent e;
        e.i[0] = numSamples;
        e.i[1] = numChannels;
        e.d[0] = m_sampleRate.load(std::memory_order_relaxed);
        e.flags |= isNonRealtime() ? fbNonRealtime : 0u;
        e.flags |= m_inAudioSuite.load(std::memory_order_relaxed) ? fbAudioSuite : 0u;
        if (playHead != nullptr)
            e.flags |= fbHasPlayHead;
        if (info)
        {
            e.flags |= fbHasPosition;
            if (auto v = info->getTimeInSamples())        { e.flags |= fbTimeSamples; e.i[2] = *v; }
            if (auto v = info->getTimeInSeconds())        { e.flags |= fbTimeSeconds; e.d[1] = *v; }
            if (auto v = info->getBpm())                  { e.flags |= fbBpm; e.d[2] = *v; }
            if (auto v = info->getLoopPoints())           { e.flags |= fbLoopPoints; e.d[3] = v->ppqStart; e.d[4] = v->ppqEnd; }
            if (auto v = info->getHostTimeNs())           { e.flags |= fbHostTime; e.i[3] = static_cast<std::int64_t>(*v); }
            if (auto v = info->getEditOriginTime())       { e.flags |= fbEditOrigin; e.d[5] = *v; }
            if (auto v = info->getPpqPosition())          { e.flags |= fbPpq; e.d[6] = *v; }
            if (auto v = info->getTimeSignature())        { e.flags |= fbTimeSig; e.i[4] = v->numerator; e.i[5] = v->denominator; }
            if (auto v = info->getBarCount())             { e.flags |= fbBarCount; e.i[6] = *v; }
            if (auto v = info->getFrameRate())            { e.flags |= fbFrameRate; e.d[7] = v->getEffectiveRate(); }
            e.flags |= info->getIsPlaying() ? fbPlaying : 0u;
            e.flags |= info->getIsRecording() ? fbRecording : 0u;
            e.flags |= info->getIsLooping() ? fbLooping : 0u;
        }
        e.i[7] = selectionLength.value_or(0);
        post(m_id, formatFirstBlock, e);
    }
    else
    {
        const auto block = m_runBlocks.load(std::memory_order_relaxed);
        if (numSamples != m_lastBlockSize)
        {
            const auto changes = m_runSizeChanges.fetch_add(1, std::memory_order_relaxed) + 1;
            if (changes <= kMaxBlockSizeLines)
            {
                LogEvent e;
                e.i[0] = m_lastBlockSize;
                e.i[1] = numSamples;
                e.i[2] = block;
                e.flags = changes == kMaxBlockSizeLines ? 1u : 0u;
                post(m_id, formatBlockSize, e);
            }
            m_lastBlockSize = numSamples;
        }

        const bool hadTime = m_runHasTime.load(std::memory_order_relaxed);
        if (time.has_value() != hadTime)
        {
            LogEvent e;
            e.i[0] = block;
            e.flags = time ? 1u : 0u;
            post(m_id, formatTimeGone, e);
            m_runHasTime.store(time.has_value(), std::memory_order_relaxed);
            if (time && m_runBlocks.load(std::memory_order_relaxed) > 0)
                m_runFirstTime.store(*time, std::memory_order_relaxed);
        }
        else if (time && *time != m_expectedTime)
        {
            if (*time == m_lastTime)
            {
                if (! m_runFrozen.exchange(true, std::memory_order_relaxed))
                {
                    LogEvent e;
                    e.i[0] = *time;
                    e.i[1] = block;
                    post(m_id, formatFrozen, e);
                }
            }
            else
            {
                const auto count = m_runDiscontinuities.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count <= kMaxDiscontinuityLines)
                {
                    LogEvent e;
                    e.i[0] = m_expectedTime;
                    e.i[1] = *time;
                    e.i[2] = *time - m_expectedTime;
                    e.i[3] = block;
                    e.flags = count == kMaxDiscontinuityLines ? 1u : 0u;
                    post(m_id, formatDiscontinuity, e);
                }
            }
        }
    }

    if (time)
    {
        m_lastTime = *time;
        m_expectedTime = *time + numSamples;
        m_runLastTime.store(*time, std::memory_order_relaxed);
    }
    m_runBlocks.fetch_add(1, std::memory_order_relaxed);
    m_runSamples.fetch_add(numSamples, std::memory_order_relaxed);
    if (numSamples < m_runMinBlock.load(std::memory_order_relaxed)) m_runMinBlock.store(numSamples, std::memory_order_relaxed);
    if (numSamples > m_runMaxBlock.load(std::memory_order_relaxed)) m_runMaxBlock.store(numSamples, std::memory_order_relaxed);
    m_lastBlockNs.store(nowNs, std::memory_order_release);

    // As an insert in Pro Tools: audio passes through, and that is all.
    if (m_aaxInsert.load(std::memory_order_relaxed))
        return;

    // ---- capture ----
    auto& store = CaptureStore::instance();
    const float* const* channels = buffer.getArrayOfReadPointers();
    // Learn listens to Preview only. A Render's stream starts with the host's handle, not with the
    // selection, so capturing it would learn the wrong stretch of audio: Learn stays armed and
    // the editor says to press Preview.
    if (isNonRealtime())
    {
        if (store.isArmed(Slot::Music) || store.isArmed(Slot::Sfx))
            m_learnSkippedInRender.store(true, std::memory_order_relaxed);
    }
    else
    {
        for (int s = 0; s < kNumSlots; ++s)
        {
            const auto slot = static_cast<Slot>(s);
            const auto ev = store.captureBlock(slot, channels, numChannels, numSamples, m_sampleRate.load(std::memory_order_relaxed), time, m_id, nowNs, selectionLength);
            logPassEvent(slot, ev);
            if (ev.passStarted)
                m_learnSkippedInRender.store(false, std::memory_order_relaxed);
        }
    }

    // ---- output ----
    // A run's mode and plan are fixed at its first block: a Stamper is stateful, so its plan is
    // never swapped mid-run.
    const bool nonRealtime = isNonRealtime();
    const bool resetRequested = m_resetRequested.exchange(false, std::memory_order_acq_rel);
    bool begin = newRun || resetRequested || ! m_stampRunOpen || nonRealtime != m_runIsNonRealtime || numChannels != m_runChannelsStamp;

    // A Preview often keeps running while the user works: Learn finishes inside it, Stamp is
    // switched on, the effect is cleared. What the run should be doing has then changed under
    // it, so it is begun again on the spot (a Render keeps what it started with).
    if (! begin && ! nonRealtime)
    {
        auto& hub = MatchHub::instance();
        const bool armed = store.isArmed(Slot::Music) || store.isArmed(Slot::Sfx);
        const bool learning = m_runMode == StampRunInfo::Mode::Learn;
        if (armed != learning)
            begin = true;                                                   // Learn armed, or just finished
        else if (m_runMode == StampRunInfo::Mode::Dry)
            begin = hub.stampEnabled() && (! m_runStampWanted || hub.planGeneration() != m_runPlanSeen);   // Stamp on, or a match arrived
        else if (m_runMode == StampRunInfo::Mode::Stamp)
            begin = m_restartStamp || hub.planGeneration() == 0;            // formants changed, another effect, or cleared
    }
    if (begin)
        beginStampRun(numChannels, nonRealtime, selectionLength.value_or(0));

    if (m_runMode == StampRunInfo::Mode::Stamp)
        stampBlock(buffer, numChannels);
    else if (m_runIsNonRealtime)
        delayInPlace(buffer, numChannels);   // offline: dry, but where the reported latency says
    // else real time, dry: the buffer is untouched.

    // Preview is the audition's way out: an edit being made in the editor is mixed over the
    // stream here (never into a Render, and never while Learn is listening).
    if (! m_runIsNonRealtime && m_runMode != StampRunInfo::Mode::Learn)
        AuditionBus::instance().render(buffer.getArrayOfWritePointers(), numChannels, numSamples,
                                       m_sampleRate.load(std::memory_order_relaxed));
    else if (! m_runIsNonRealtime)
        AuditionBus::instance().notePreviewRunning();

    m_srSamples.fetch_add(numSamples, std::memory_order_relaxed);
}

void TonalRecallProcessor::beginStampRun(int numChannels, bool nonRealtime, std::int64_t selectionLength) noexcept
{
    if (m_srSummaryPending.exchange(false, std::memory_order_acq_rel))
        postStampSummary(ThreadKind::Audio);

    // The previous run's Stamper goes to the timer to be freed (or is freed here if the timer has
    // not collected the one before: rare).
    if (m_stamper != nullptr)
    {
        // Handed to the timer to free. Four slots: the timer empties them every 50 ms, so they
        // only fill if runs restart faster than that, and then the oldest wait is the stamper's.
        auto* old = m_stamper.release();
        bool handed = false;
        for (auto& slot : m_retiredStampers)
        {
            core::Stamper* expected = nullptr;
            if (slot.compare_exchange_strong(expected, old, std::memory_order_acq_rel))
            {
                handed = true;
                break;
            }
        }
        if (! handed)
            delete old;   // no room (four restarts inside 50 ms): freed here as the last resort
    }

    m_stampRunOpen = true;
    m_runIsNonRealtime = nonRealtime;
    m_runChannelsStamp = numChannels;
    for (auto& line : m_delay)
        std::fill(line.begin(), line.end(), 0.0f);
    m_delayPos = 0;

    auto& store = CaptureStore::instance();
    auto& hub = MatchHub::instance();
    const bool armed = store.isArmed(Slot::Music) || store.isArmed(Slot::Sfx);
    StampRunInfo::Mode mode = armed ? StampRunInfo::Mode::Learn : StampRunInfo::Mode::Dry;
    m_runStampWanted = hub.stampEnabled();
    m_runPlanSeen = hub.planGeneration();
    m_restartStamp = false;

    if (! armed && hub.stampEnabled())
    {
        // Pick up the newest plan. tryAcquirePlan copies a shared_ptr (no allocation); the plan
        // this releases is still held by the hub, so it is not freed here.
        const auto wanted = hub.planGeneration();
        if (wanted != (m_plan != nullptr ? m_plan->generation : 0))
            hub.tryAcquirePlan(m_plan);

        const double sr = m_sampleRate.load(std::memory_order_relaxed);
        const bool usable = m_plan != nullptr && numChannels >= 1 && numChannels <= 2 && sr > 0.0
                            && std::abs(m_plan->sampleRate - sr) < 0.5
                            && static_cast<int>(m_delay.size()) >= numChannels;
        if (usable)
        {
            // Creating a Stamper allocates (its stretcher, history and search buffers): done here,
            // once, at the first block of the run. Preview's first block and an offline Render
            // tolerate it; nothing allocates in the blocks after.
            m_setupScratch.sampleRate = sr;
            m_setupScratch.channels = numChannels;
            m_setupScratch.staticSemitones = m_plan->staticSemitones;
            m_setupScratch.curveHopSeconds = m_plan->curveHopSeconds;
            m_setupScratch.curve = m_plan->curve;
            m_setupScratch.learned = m_plan->learnedMono;   // shared: nothing is copied
            // A looping Preview plays the selection round and round; a Render plays it once.
            m_setupScratch.loopLength = nonRealtime ? 0 : selectionLength;
            m_setupScratch.preserveFormants = m_plan->preserveFormants;
            m_stamper = std::make_unique<core::Stamper>(m_setupScratch);
            // The Stamper has taken what it needs. Let go of the learned audio now, while the plan
            // still holds it: kept here, it would be this thread that freed it (tens of megabytes)
            // when a later run replaced it after the hub had dropped the plan.
            m_setupScratch.learned.reset();
            if (m_stamper->latencySamples() == m_latency.load(std::memory_order_relaxed))
                mode = StampRunInfo::Mode::Stamp;
            else
                m_stamper.reset();   // would not line up with what was reported: stay dry
        }
    }
    m_runMode = mode;

    m_srMode.store(static_cast<int>(mode), std::memory_order_relaxed);
    m_srNonRealtime.store(nonRealtime, std::memory_order_relaxed);
    m_srStampOn.store(mode == StampRunInfo::Mode::Stamp, std::memory_order_relaxed);
    m_srBend.store(mode == StampRunInfo::Mode::Stamp && m_plan->bendActive, std::memory_order_relaxed);
    m_srSemitones.store(mode == StampRunInfo::Mode::Stamp ? m_plan->staticSemitones : 0.0, std::memory_order_relaxed);
    m_srPlanGeneration.store(mode == StampRunInfo::Mode::Stamp ? m_plan->generation : 0, std::memory_order_relaxed);
    m_srAligned.store(false, std::memory_order_relaxed);
    m_srOffset.store(0, std::memory_order_relaxed);
    m_srSamples.store(0, std::memory_order_relaxed);
    m_srSelection.store(selectionLength, std::memory_order_relaxed);
    m_srEnded.store(false, std::memory_order_relaxed);
    m_srRunIndex.fetch_add(1, std::memory_order_release);
    m_srSummaryPending.store(true, std::memory_order_release);
}

void TonalRecallProcessor::delayInPlace(juce::AudioBuffer<float>& buffer, int numChannels) noexcept
{
    const int latency = m_latency.load(std::memory_order_relaxed);
    if (latency <= 0 || static_cast<int>(m_delay.size()) < numChannels)
        return;
    const int n = buffer.getNumSamples();
    int pos = m_delayPos;
    for (int c = 0; c < numChannels; ++c)
    {
        auto* line = m_delay[static_cast<std::size_t>(c)].data();
        auto* data = buffer.getWritePointer(c);
        pos = m_delayPos;
        for (int i = 0; i < n; ++i)
        {
            const float out = line[pos];
            line[pos] = data[i];
            data[i] = out;
            if (++pos == latency)
                pos = 0;
        }
    }
    m_delayPos = pos;
}

void TonalRecallProcessor::stampBlock(juce::AudioBuffer<float>& buffer, int numChannels) noexcept
{
    // The dry signal goes through the delay line alongside the Stamper, so switching Stamp off
    // mid-run (an A/B in Preview) swaps to dry audio at the same delay, without a jump.
    auto& hub = MatchHub::instance();
    const bool on = hub.stampEnabled();

    // A looping Preview picks up curve edits as they are made: the same learned effect with a new
    // shift is swapped in on the spot (a Render keeps the plan it started with).
    if (! m_runIsNonRealtime && m_plan != nullptr && hub.planGeneration() != m_plan->generation)
    {
        std::shared_ptr<const StampPlan> next;
        if (hub.tryAcquirePlan(next) && next != nullptr)
        {
            // Another effect, another rate or the formant switch: a Stamper cannot take those on
            // the run. The run is begun again at the next block instead.
            if (next->learnedMono != m_plan->learnedMono || std::abs(next->sampleRate - m_plan->sampleRate) >= 0.5
                || next->preserveFormants != m_plan->preserveFormants)
                m_restartStamp = true;
        }
        if (! m_restartStamp && next != nullptr && m_stamper->updateShift(next->staticSemitones, next->curve))
        {
            // The old plan stays alive in the hub, so letting go of it here frees nothing.
            m_plan = std::move(next);
            m_srBend.store(m_plan->bendActive, std::memory_order_relaxed);
            m_srSemitones.store(m_plan->staticSemitones, std::memory_order_relaxed);
            m_srPlanGeneration.store(m_plan->generation, std::memory_order_relaxed);
        }
    }

    const int latency = m_latency.load(std::memory_order_relaxed);
    const int total = buffer.getNumSamples();
    float* chunk[2] {};
    for (int done = 0; done < total;)
    {
        const int n = std::min(kScratchSamples, total - done);
        int pos = m_delayPos;
        for (int c = 0; c < numChannels; ++c)
        {
            auto* line = m_delay[static_cast<std::size_t>(c)].data();
            auto* dry = m_dryScratch[static_cast<std::size_t>(c)].data();
            auto* data = buffer.getWritePointer(c) + done;
            pos = m_delayPos;
            for (int i = 0; i < n; ++i)
            {
                dry[i] = line[pos];
                line[pos] = data[i];
                if (++pos == latency)
                    pos = 0;
            }
            chunk[c] = data;
        }
        m_delayPos = pos;

        m_stamper->process(chunk, n);

        if (! on)
            for (int c = 0; c < numChannels; ++c)
                std::copy(m_dryScratch[static_cast<std::size_t>(c)].data(), m_dryScratch[static_cast<std::size_t>(c)].data() + n, chunk[c]);
        done += n;
    }

    m_srStampOn.store(on, std::memory_order_relaxed);
    if (m_stamper->alignmentFound() && ! m_srAligned.load(std::memory_order_relaxed))
    {
        m_srOffset.store(m_stamper->alignmentOffset(), std::memory_order_relaxed);
        m_srAligned.store(true, std::memory_order_release);
    }
}

void TonalRecallProcessor::postStampSummary(ThreadKind thread) noexcept
{
    LogEvent e;
    e.i[0] = m_srMode.load(std::memory_order_relaxed);
    e.i[1] = static_cast<std::int64_t>(m_srPlanGeneration.load(std::memory_order_relaxed));
    e.i[2] = m_srAligned.load(std::memory_order_acquire) ? 1 : 0;
    e.i[3] = m_srOffset.load(std::memory_order_relaxed);
    e.i[4] = m_srSamples.load(std::memory_order_relaxed);
    e.i[5] = m_latency.load(std::memory_order_relaxed);
    e.i[6] = m_srSelection.load(std::memory_order_relaxed);
    e.i[7] = m_srNonRealtime.load(std::memory_order_relaxed) ? 1 : 0;
    e.i[8] = m_srStampOn.load(std::memory_order_relaxed) ? 1 : 0;
    e.i[9] = m_srRunIndex.load(std::memory_order_relaxed);
    e.d[0] = m_srSemitones.load(std::memory_order_relaxed);
    e.d[1] = m_sampleRate.load(std::memory_order_relaxed);
    e.flags = m_srBend.load(std::memory_order_relaxed) ? 1u : 0u;
    m_srEnded.store(true, std::memory_order_release);
    post(m_id, formatStampSummary, e, thread);
}

StampRunInfo TonalRecallProcessor::lastStampRun() const noexcept
{
    StampRunInfo info;
    info.runIndex = m_srRunIndex.load(std::memory_order_acquire);
    info.mode = static_cast<StampRunInfo::Mode>(m_srMode.load(std::memory_order_relaxed));
    info.ended = m_srEnded.load(std::memory_order_acquire);
    info.nonRealtime = m_srNonRealtime.load(std::memory_order_relaxed);
    info.stampOn = m_srStampOn.load(std::memory_order_relaxed);
    info.bendActive = m_srBend.load(std::memory_order_relaxed);
    info.semitones = m_srSemitones.load(std::memory_order_relaxed);
    info.planGeneration = m_srPlanGeneration.load(std::memory_order_relaxed);
    info.aligned = m_srAligned.load(std::memory_order_acquire);
    info.alignmentOffset = m_srOffset.load(std::memory_order_relaxed);
    info.samples = m_srSamples.load(std::memory_order_relaxed);
    info.selectionLength = m_srSelection.load(std::memory_order_relaxed);
    return info;
}

void TonalRecallProcessor::freeRetiredStamper()
{
    for (auto& slot : m_retiredStampers)
        delete slot.exchange(nullptr, std::memory_order_acq_rel);
}

void TonalRecallProcessor::logPassEvent(Slot slot, const BlockEvent& ev) noexcept
{
    const auto s = static_cast<std::int64_t>(slot);
    const auto idx = static_cast<std::size_t>(s);

    if (ev.skippedOtherOwner || ev.skippedUnallocated || ev.skippedLock)
    {
        if (ev.skippedOtherOwner)
            m_runSkippedOwner.fetch_add(1, std::memory_order_relaxed);
        if (! m_skipLogged[idx])
        {
            m_skipLogged[idx] = true;
            LogEvent e;
            e.i[0] = s;
            e.i[1] = ev.skippedOtherOwner ? 0 : ev.skippedUnallocated ? 1 : 2;
            e.i[2] = ev.owner;
            post(m_id, formatSkip, e);
        }
        return;
    }

    if (ev.passStarted)
    {
        LogEvent e;
        e.i[0] = s;
        e.i[1] = ev.position;
        e.flags = (ev.merged ? 1u : 0u) | (ev.replaced ? 2u : 0u) | (ev.prepended ? 4u : 0u);
        e.flags |= ev.hasPosition ? 8u : 0u;
        post(m_id, formatPassStart, e);
    }
    if (ev.timelineFrozen)
    {
        LogEvent e;
        e.i[0] = s;
        post(m_id, formatFrozenPass, e);
    }
    if (ev.ended != PassEnd::None)
    {
        LogEvent e;
        e.i[0] = s;
        e.i[1] = static_cast<std::int64_t>(ev.ended);
        e.i[2] = ev.passSamples;
        e.i[3] = ev.position;
        e.i[4] = ev.expectedPosition;
        e.flags = (ev.ended == PassEnd::TimelineJumpBack || ev.ended == PassEnd::TimelineJump) ? 1u : 0u;
        post(m_id, formatPassEnd, e);
    }
}

void TonalRecallProcessor::pollRunState()
{
    auto& log = DiagnosticLog::instance();
    const auto now = CaptureStore::now();

    // Any instance's timer may end a stalled pass; whichever does logs it.
    const auto ended = CaptureStore::instance().checkStalls(now);
    for (int s = 0; s < kNumSlots; ++s)
    {
        if (ended[static_cast<std::size_t>(s)] == PassEnd::None)
            continue;
        SlotSnapshot snap;
        CaptureStore::instance().snapshot(static_cast<Slot>(s), snap);
        log.post(m_id, callerThread(), "%s: PASS END (%s); disarmed; slot now holds %" PRId64 " samples heard, %d pass(es), %d range(s)",
                 slotName(static_cast<Slot>(s)), passEndName(ended[static_cast<std::size_t>(s)]),
                 snap.samplesCaptured, snap.passes, snap.segments);
    }

    if (juce::MessageManager::existsAndIsCurrentThread())
        refreshAudioSuiteFlag("on the timer");
    freeRetiredStamper();

    if (! m_runActive.load(std::memory_order_acquire))
        return;
    const auto last = m_lastBlockNs.load(std::memory_order_acquire);
    if (now - last <= CaptureStore::kStallNs)
        return;

    const auto blocks = m_runBlocks.load(std::memory_order_relaxed);
    const auto samples = m_runSamples.load(std::memory_order_relaxed);
    const bool hasTime = m_runHasTime.load(std::memory_order_relaxed);
    const auto first = m_runFirstTime.load(std::memory_order_relaxed);
    const auto lastTime = m_runLastTime.load(std::memory_order_relaxed);
    const auto startNs = m_runStartNs.load(std::memory_order_relaxed);
    const auto disc = m_runDiscontinuities.load(std::memory_order_relaxed);
    const auto sizes = m_runSizeChanges.load(std::memory_order_relaxed);
    const auto minB = m_runMinBlock.load(std::memory_order_relaxed);
    const auto maxB = m_runMaxBlock.load(std::memory_order_relaxed);
    const auto skipped = m_runSkippedOwner.load(std::memory_order_relaxed);
    const bool frozen = m_runFrozen.load(std::memory_order_relaxed);
    const int channels = m_runChannels.load(std::memory_order_relaxed);
    const bool nonRealtime = m_runNonRealtime.load(std::memory_order_relaxed);

    bool expected = true;
    if (! m_runActive.compare_exchange_strong(expected, false))
        return;

    const double sr = m_sampleRate.load();
    const double audioSeconds = sr > 0.0 ? static_cast<double>(samples) / sr : 0.0;
    const double wallSeconds = static_cast<double>(last - startNs) / 1.0e9;
    char timeText[160];
    if (hasTime)
        std::snprintf(timeText, sizeof(timeText), "timeInSamples first %" PRId64 ", last %" PRId64 " (%s)",
                      first, lastTime, frozen ? "frozen: not advancing" : "advancing");
    else
        std::snprintf(timeText, sizeof(timeText), "no timeInSamples");

    log.post(m_id, callerThread(),
             "END OF RUN: %" PRId64 " blocks, %" PRId64 " samples (%.3f s at %.1f Hz), %d ch, block size %" PRId64 "..%" PRId64
             ", %s, %" PRId64 " discontinuities, %" PRId64 " block-size changes, wall time %.3f s (first to last block; %.2fx real time), "
             "isNonRealtime at start %s, blocks not captured because another instance owned the pass %" PRId64,
             blocks, samples, audioSeconds, sr, channels, minB, maxB, timeText, disc, sizes, wallSeconds,
             wallSeconds > 0.0 ? audioSeconds / wallSeconds : 0.0, nonRealtime ? "yes" : "no", skipped);

    // The audio has stopped, so this run's Stamp numbers are final. The next block starts a new
    // run (and a new Stamper).
    if (m_srSummaryPending.exchange(false, std::memory_order_acq_rel))
        postStampSummary(callerThread());
}

void TonalRecallProcessor::logEditor(bool opened)
{
    DiagnosticLog::instance().post(m_id, callerThread(), "editor %s", opened ? "opened" : "closed");
}

namespace
{
    const juce::Identifier kStateType { "TonalRecallOptions" };

    const char* targetId(core::Target t) noexcept
    {
        switch (t)
        {
            case core::Target::Root:       return "root";
            case core::Target::Fifth:      return "fifth";
            case core::Target::Third:      return "third";
            case core::Target::Nearest:    return "nearest";
            case core::Target::PitchClass: return "note";
            case core::Target::Follow:     return "follow";
        }
        return "root";
    }

    const char* methodId(core::SfxMethod m) noexcept
    {
        switch (m)
        {
            case core::SfxMethod::Auto:      return "auto";
            case core::SfxMethod::Tonal:     return "tonal";
            case core::SfxMethod::Engine:    return "engine";
            case core::SfxMethod::Resonance: return "resonance";
        }
        return "auto";
    }
}

void TonalRecallProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // The match options only: captures stay in memory (they can be tens of megabytes).
    const auto o = MatchHub::instance().options();
    juce::ValueTree state(kStateType);
    state.setProperty("version", 1, nullptr);
    state.setProperty("target", targetId(o.target), nullptr);
    state.setProperty("pitchClass", o.pitchClass, nullptr);
    state.setProperty("octave", o.octaveShift, nullptr);
    state.setProperty("bend", o.bend, nullptr);
    state.setProperty("glide", o.glide, nullptr);
    state.setProperty("method", methodId(o.method), nullptr);
    state.setProperty("formants", o.preserveFormants, nullptr);
    state.setProperty("stamp", o.stampEnabled, nullptr);
    // Hand adjustments are not saved: they were drawn on a learned effect, and learned audio is
    // not saved, so after a reload there would be nothing for them to belong to.
    destData.reset();
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
    DiagnosticLog::instance().post(m_id, callerThread(), "getStateInformation() -> %d bytes", static_cast<int>(destData.getSize()));
}

void TonalRecallProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    DiagnosticLog::instance().post(m_id, callerThread(), "setStateInformation(%d bytes)", sizeInBytes);
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr)
        return;
    const auto state = juce::ValueTree::fromXml(*xml);
    if (! state.hasType(kStateType))
        return;

    MatchOptions o;
    const auto target = state.getProperty("target").toString();
    for (auto t : { core::Target::Root, core::Target::Fifth, core::Target::Third, core::Target::Nearest, core::Target::PitchClass, core::Target::Follow })
        if (target == targetId(t))
            o.target = t;
    const auto method = state.getProperty("method").toString();
    for (auto m : { core::SfxMethod::Auto, core::SfxMethod::Tonal, core::SfxMethod::Engine, core::SfxMethod::Resonance })
        if (method == methodId(m))
            o.method = m;
    o.pitchClass = state.getProperty("pitchClass", 0);
    o.octaveShift = state.getProperty("octave", 0);
    o.bend = state.getProperty("bend", 0.0);
    o.glide = state.getProperty("glide", 0.0);
    o.preserveFormants = state.getProperty("formants", false);
    o.stampEnabled = state.getProperty("stamp", true);
    // Whatever has been drawn on the effect now learned stays: it is not part of saved state.
    o.edits = MatchHub::instance().options().edits;
    MatchHub::instance().setOptions(o);
}

juce::AudioProcessorEditor* TonalRecallProcessor::createEditor()
{
    return new TonalRecallEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TonalRecallProcessor();
}
