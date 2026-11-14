#pragma once

#include <JuceHeader.h>

#include <array>

#include "CaptureStore.h"
#include "DiagnosticLog.h"
#include "MatchHub.h"
#include "core/Stamper.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// What one run of audio (a Preview or a Render) did, for the editor and the tests.
struct StampRunInfo
{
    enum class Mode : int { None = 0, Dry, Learn, Stamp };
    Mode mode = Mode::None;
    std::uint32_t runIndex = 0;          // 0 = no run yet
    bool ended = false;                  // the run is over (the numbers are final)
    bool nonRealtime = false;
    bool stampOn = false;                // the last block was stamped (Stamp may be switched off mid-run)
    bool bendActive = false;             // the plan carried a bend
    double semitones = 0.0;              // the fixed shift of the plan
    std::uint64_t planGeneration = 0;
    bool aligned = false;                // the learned effect was found in the stream
    std::int64_t alignmentOffset = 0;    // stream sample where it starts
    std::int64_t samples = 0;            // samples processed in the run
    std::int64_t selectionLength = 0;    // from the loop points (0 = unknown)
};

// Tonal Recall's processor. It feeds the shared capture store with the dry input (Learn), and
// otherwise stamps the matched shift onto the audio with a core::Stamper. It reports the
// Stamper's latency; offline (Render) every mode is delayed by it, so a Render always lines up.
class TonalRecallProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    TonalRecallProcessor();
    ~TonalRecallProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    void setNonRealtime(bool isNonRealtime) noexcept override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    std::uint32_t getInstanceId() const noexcept { return m_id; }
    static int liveInstanceCount() noexcept;
    bool isInAudioSuite() const noexcept { return m_inAudioSuite.load(std::memory_order_relaxed); }
    /** Loaded as a real-time insert in Pro Tools: it only passes audio through there. */
    bool isAaxInsert() const noexcept { return m_aaxInsert.load(std::memory_order_relaxed); }
    /** In a host other than Pro Tools (VST3, AU): works, but is an offline tool all the same. */
    bool isOtherHost() const noexcept { return wrapperType != wrapperType_AAX; }

    // The timer's work, public so tests can run it with a fake clock: ends stalled capture passes
    // and closes this instance's run in the log once audio has stopped for kStallNs.
    void pollRunState();

    // In AudioSuite the host's loop points are the selection being previewed or rendered. Tests
    // switch this on by hand (they are not hosted in AudioSuite).
    void useLoopPointsAsSelection(bool shouldUse) noexcept { m_loopIsSelection.store(shouldUse); }

    // The editor reports itself (open/close) through these.
    void logEditor(bool opened);

    int getReportedLatency() const noexcept { return m_latency.load(std::memory_order_relaxed); }
    StampRunInfo lastStampRun() const noexcept;

    /** True after a Render ran while Learn was armed (Learn only listens to Preview). */
    bool learnSkippedInRender() const noexcept { return m_learnSkippedInRender.load(std::memory_order_relaxed); }

private:
    void timerCallback() override { pollRunState(); }
    void refreshAudioSuiteFlag(const char* when);
    void logPassEvent(tonalrecall::Slot slot, const tonalrecall::BlockEvent& ev) noexcept;

    // Stamp (audio thread unless noted).
    void beginStampRun(int numChannels, bool nonRealtime, std::int64_t selectionLength) noexcept;
    void postStampSummary(tonalrecall::ThreadKind thread) noexcept;   // any thread
    void delayInPlace(juce::AudioBuffer<float>& buffer, int numChannels) noexcept;
    void stampBlock(juce::AudioBuffer<float>& buffer, int numChannels) noexcept;
    void freeRetiredStamper();                                        // off the audio thread

    const std::uint32_t m_id;
    // The rate prepareToPlay was given (what the capture is tagged with).
    std::atomic<double> m_sampleRate { 0.0 };
    std::atomic<bool> m_inAudioSuite { false };
    std::atomic<bool> m_aaxInsert { false }, m_prepared { false };
    std::atomic<bool> m_loopIsSelection { false };
    std::atomic<bool> m_learnSkippedInRender { false };
    std::atomic<int> m_inAudioSuiteLogged { -1 };

    // This instance's current run of audio (blocks with no gap longer than the stall time).
    // Written by the audio thread, read by the timer: hence atomics.
    std::atomic<bool> m_runActive { false };
    std::atomic<std::int64_t> m_runBlocks { 0 }, m_runSamples { 0 };
    std::atomic<std::int64_t> m_runFirstTime { 0 }, m_runLastTime { 0 };
    std::atomic<bool> m_runHasTime { false };
    std::atomic<std::int64_t> m_runStartNs { 0 }, m_lastBlockNs { 0 };
    std::atomic<std::int64_t> m_runDiscontinuities { 0 }, m_runSizeChanges { 0 };
    std::atomic<std::int64_t> m_runMinBlock { 0 }, m_runMaxBlock { 0 };
    std::atomic<std::int64_t> m_runSkippedOwner { 0 };
    std::atomic<bool> m_runFrozen { false };
    std::atomic<int> m_runChannels { 0 };
    std::atomic<bool> m_runNonRealtime { false };

    // ---- Stamp ----
    std::atomic<int> m_latency { 0 };
    std::atomic<bool> m_resetRequested { true };

    // The current/last stamp run, written by the audio thread, read by the editor, the timer and
    // the tests.
    std::atomic<int> m_srMode { 0 };
    std::atomic<std::uint32_t> m_srRunIndex { 0 };
    std::atomic<bool> m_srEnded { false }, m_srNonRealtime { false }, m_srStampOn { false }, m_srBend { false };
    std::atomic<bool> m_srAligned { false };
    std::atomic<double> m_srSemitones { 0.0 };
    std::atomic<std::uint64_t> m_srPlanGeneration { 0 };
    std::atomic<std::int64_t> m_srOffset { 0 }, m_srSamples { 0 }, m_srSelection { 0 };
    std::atomic<bool> m_srSummaryPending { false };   // the run's log line is still to be written

    // Audio thread only (allocated in prepareToPlay, or at the first block of a run: see
    // beginStampRun()).
    std::unique_ptr<tonalrecall::core::Stamper> m_stamper;
    std::shared_ptr<const tonalrecall::StampPlan> m_plan;   // the hub keeps every plan alive: never freed here
    tonalrecall::core::Stamper::Setup m_setupScratch;
    std::vector<std::vector<float>> m_delay;               // per channel, m_latency samples
    std::vector<std::vector<float>> m_dryScratch;          // per channel, kScratchSamples
    int m_delayPos = 0;
    int m_runChannelsStamp = 0;
    bool m_runIsNonRealtime = false;
    bool m_stampRunOpen = false;
    StampRunInfo::Mode m_runMode = StampRunInfo::Mode::None;
    // What the run in hand saw when it began, to tell when it has to begin again (audio thread).
    bool m_runStampWanted = false, m_restartStamp = false;
    std::uint64_t m_runPlanSeen = 0;
    static constexpr int kScratchSamples = 4096;

    // A Stamper replaced at the start of a run is handed to the timer to free.
    std::array<std::atomic<tonalrecall::core::Stamper*>, 4> m_retiredStampers {};

    // Audio thread only.
    int m_lastBlockSize = 0;
    std::int64_t m_expectedTime = 0;
    std::int64_t m_lastTime = 0;
    bool m_skipLogged[tonalrecall::kNumSlots] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TonalRecallProcessor)
};
