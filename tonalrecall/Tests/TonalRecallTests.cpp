// Offline tests for Tonal Recall: real processor instances driven with a fake play head and a fake
// clock, checking what the capture store promises (passthrough, the shared store, pass-end rules,
// merging, truncation, independent slots), Learn -> analysis -> match -> Stamp end to end, the
// saved options, the editor, and that the log is written.
//
//   TonalRecallTests [log file [folder for editor PNGs]]   non-zero exit on failure

#include <JuceHeader.h>

#include "CaptureStore.h"
#include "DiagnosticLog.h"
#include "Audition.h"
#include "MatchHub.h"
#include "PluginEditor.h"
#include "SammyJUpdateCheck.h"
#include "PluginProcessor.h"
#include "core/Analysis.h"
#include "core/HindsightTracker.h"
#include "core/PeakLimiter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

using namespace tonalrecall;

namespace
{
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what)
{
    ++g_checks;
    if (! ok)
    {
        ++g_failures;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

std::atomic<std::int64_t> g_now { 1'000'000'000 };
std::int64_t fakeClock() { return g_now.load(); }
void advanceMs(double ms) { g_now.fetch_add(static_cast<std::int64_t>(ms * 1.0e6)); }

// A deterministic signal tied to the timeline position, so merged captures can be checked
// sample by sample against where they came from.
float signalAt(std::int64_t t, int channel, int seed)
{
    return static_cast<float>(0.4 * std::sin(0.0137 * static_cast<double>(t) * (1.0 + 0.1 * seed) + 1.3 * channel)
                              + 0.05 * channel);
}

float monoAt(std::int64_t t, int channels, int seed)
{
    float sum = 0.0f;   // same order as the store: channel 0 first, from 0.0f
    for (int c = 0; c < channels; ++c)
        sum += signalAt(t, c, seed);
    return sum;
}

struct FakePlayHead : juce::AudioPlayHead
{
    bool providePosition = true;
    bool provideTime = true;
    bool frozen = false;          // report the same position every block
    std::int64_t time = 0;
    double sampleRate = 48000.0;
    std::int64_t selectionStart = 0, selectionLength = 0;   // > 0: reported as loop points at 120 bpm

    juce::Optional<PositionInfo> getPosition() const override
    {
        if (! providePosition)
            return juce::nullopt;
        PositionInfo info;
        if (provideTime)
        {
            info.setTimeInSamples(time);
            info.setTimeInSeconds(static_cast<double>(time) / sampleRate);
        }
        if (selectionLength > 0)
        {
            // 120 bpm: a quarter note is half a second.
            const double perSample = 2.0 / sampleRate;
            info.setBpm(120.0);
            info.setLoopPoints(juce::AudioPlayHead::LoopPoints { static_cast<double>(selectionStart) * perSample,
                                                                 static_cast<double>(selectionStart + selectionLength) * perSample });
        }
        info.setIsPlaying(true);
        return info;
    }
};

struct Rig
{
    std::unique_ptr<TonalRecallProcessor> processor;
    FakePlayHead head;
    int channels;
    double sampleRate;
    int blockSize;
    bool passthroughOk = true;

    explicit Rig(int numChannels = 2, double rate = 48000.0, int block = 512, bool withPlayHead = true)
        : processor(std::make_unique<TonalRecallProcessor>()), channels(numChannels), sampleRate(rate), blockSize(block)
    {
        const auto set = numChannels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add(set);
        layout.outputBuses.add(set);
        processor->setBusesLayout(layout);
        head.sampleRate = rate;
        processor->setPlayHead(withPlayHead ? &head : nullptr);
        processor->prepareToPlay(rate, block);
    }

    ~Rig() { processor->releaseResources(); }

    std::uint32_t id() const { return processor->getInstanceId(); }

    // Plays [start, start + count) of the timeline in blocks (the last one may be shorter),
    // checking each output block is the input, bit for bit. The fake clock moves in real time.
    // stopAfterBlock: called after each block; returning false stops early.
    void play(std::int64_t start, std::int64_t count, int seed = 0,
              const std::function<bool()>& afterBlock = nullptr)
    {
        juce::AudioBuffer<float> buffer(channels, blockSize), input(channels, blockSize);
        juce::MidiBuffer midi;
        std::int64_t done = 0;
        while (done < count)
        {
            const int n = static_cast<int>(std::min<std::int64_t>(blockSize, count - done));
            buffer.setSize(channels, n, false, false, true);
            input.setSize(channels, n, false, false, true);
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < n; ++i)
                    input.setSample(c, i, signalAt(start + done + i, c, seed));
            for (int c = 0; c < channels; ++c)
                buffer.copyFrom(c, 0, input, c, 0, n);

            if (! head.frozen)
                head.time = start + done;
            processor->processBlock(buffer, midi);

            for (int c = 0; c < channels; ++c)
                if (std::memcmp(buffer.getReadPointer(c), input.getReadPointer(c), sizeof(float) * static_cast<std::size_t>(n)) != 0)
                    passthroughOk = false;

            done += n;
            advanceMs(1000.0 * n / sampleRate);
            if (afterBlock && ! afterBlock())
                break;
        }
    }

    // One block at an arbitrary timeline position.
    void block(std::int64_t at, int n, int seed = 0)
    {
        head.time = at;
        const bool wasFrozen = head.frozen;
        head.frozen = true;   // keep play() from overwriting the position
        play(at, n, seed);
        head.frozen = wasFrozen;
    }

    // Lets the stall rule fire: no audio for longer than the stall time, then the timer's work.
    void stall()
    {
        advanceMs(400.0);
        processor->pollRunState();
    }
};

SlotSnapshot snap(Slot slot)
{
    SlotSnapshot s;
    CaptureStore::instance().snapshot(slot, s);
    return s;
}

std::vector<float> audio(Slot slot)
{
    std::vector<float> out;
    CaptureStore::instance().copyAudio(slot, out);
    return out;
}

// True if the buffer from offset holds the mono sum of the signal from timeline t0.
// Exact comparison on purpose: capture must be the mono sum, bit for bit.
bool sameBits(float a, float b)
{
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

bool matches(const std::vector<float>& got, std::size_t offset, std::int64_t t0, std::int64_t count, int channels, int seed)
{
    if (offset + static_cast<std::size_t>(count) > got.size())
        return false;
    for (std::int64_t i = 0; i < count; ++i)
        if (! sameBits(got[offset + static_cast<std::size_t>(i)], monoAt(t0 + i, channels, seed)))
            return false;
    return true;
}

// A step within a case: the store and the hub are kept.
void step(const char* name)
{
    std::printf("%s\n", name);
}

void beginCase(const char* name)
{
    std::printf("%s\n", name);
    CaptureStore::instance().resetForTesting();
    MatchHub::instance().resetForTesting();
    AuditionBus::instance().resetForTesting();
    {
        // The cases below were written against the root as the target.
        auto o = MatchHub::instance().options();
        o.target = core::Target::Root;
        o.glide = 0.0;   // ... against an unsmoothed curve
        o.bend = 0.0;    // ... with none of the music's movement added
        MatchHub::instance().setOptions(o);
    }
}

//==============================================================================
void testPassthrough()
{
    beginCase("passthrough is bit-identical (idle, armed, mono, stereo)");
    {
        Rig rig;
        rig.play(0, 10000);
        CaptureStore::instance().arm(Slot::Music);
        CaptureStore::instance().arm(Slot::Sfx);
        rig.play(10000, 10000);
        check(rig.passthroughOk, "stereo output differs from input");
    }
    {
        Rig rig(1);
        CaptureStore::instance().arm(Slot::Music);
        rig.play(0, 7777);
        check(rig.passthroughOk, "mono output differs from input");
    }
}

void testSinglePass()
{
    beginCase("arm + one pass captures exactly the audio played, with its timeline start");
    Rig rig;
    CaptureStore::instance().arm(Slot::Music);
    check(snap(Slot::Music).armed, "arm() did not arm");
    const std::int64_t start = 96000, count = 72000;   // 1.5 s, last block partial
    rig.play(start, count);
    auto s = snap(Slot::Music);
    check(s.capturing && s.armed, "should still be capturing until the pass ends");
    rig.stall();
    s = snap(Slot::Music);
    check(! s.armed && ! s.capturing, "the pass should have ended and disarmed");
    check(s.lastEnd == PassEnd::Stall, "end reason should be the stall");
    check(s.samplesCaptured == count, "captured " + std::to_string(s.samplesCaptured) + ", expected " + std::to_string(count));
    check(s.timelineKnown, "timeline should be known");
    check(s.captured.start == start && s.captured.end == start + count, "captured range " + std::to_string(s.captured.start)
          + ".." + std::to_string(s.captured.end));
    check(s.passes == 1 && s.segments == 1, "passes/segments");
    check(s.sampleRate == 48000.0, "sample rate");
    check(s.capturedBy == rig.id(), "capturedBy");
    check(! s.truncated, "not truncated");
    const auto got = audio(Slot::Music);
    check(got.size() == static_cast<std::size_t>(count), "buffer size");
    check(matches(got, 0, start, count, 2, 0), "captured audio is not the mono sum of what was played");
    float peak = 0.0f;
    for (std::int64_t t = start; t < start + count; ++t)
        for (int c = 0; c < 2; ++c)
            peak = std::max(peak, std::abs(signalAt(t, c, 0)));
    check(sameBits(s.peak, peak), "peak should be the loudest input sample");
    check(rig.passthroughOk, "passthrough");
}

void testSharedStore()
{
    beginCase("a second instance sees the first one's capture, and can capture for both");
    Rig a, b;
    check(a.id() != b.id(), "instance ids must differ");
    check(TonalRecallProcessor::liveInstanceCount() == 2, "live instance count should be 2, is "
          + std::to_string(TonalRecallProcessor::liveInstanceCount()));
    CaptureStore::instance().arm(Slot::Music);
    a.play(0, 20000);
    a.stall();
    auto s = snap(Slot::Music);
    check(s.samplesCaptured == 20000 && s.capturedBy == a.id(), "capture by A");

    {
        // A third instance created after the capture (as Pro Tools may do for Render) sees it too.
        Rig c;
        SlotSnapshot seen;
        CaptureStore::instance().snapshot(Slot::Music, seen);
        check(seen.samplesCaptured == 20000, "a new instance should see the existing capture");
    }

    CaptureStore::instance().arm(Slot::Sfx);   // armed "from A's editor"
    b.play(5000, 3000, 1);                     // audio reaches B
    b.stall();
    s = snap(Slot::Sfx);
    check(s.capturedBy == b.id() && s.samplesCaptured == 3000, "B should capture what A armed");
    check(matches(audio(Slot::Sfx), 0, 5000, 3000, 2, 1), "B's capture content");
}

void testOwnership()
{
    beginCase("one writer per pass: a second instance processing at the same time passes through");
    Rig a, b;
    CaptureStore::instance().arm(Slot::Music);
    a.play(0, 2048);
    b.play(0, 2048, 3);   // e.g. another channel's instance
    auto s = snap(Slot::Music);
    check(s.owner == a.id() && s.samplesCaptured == 2048, "A should own the pass and B not add to it");
    check(s.blocksSkippedOtherOwner == 4, "B's 4 blocks should be counted as skipped, got " + std::to_string(s.blocksSkippedOtherOwner));
    check(b.passthroughOk, "B still passes audio through");
    a.play(2048, 1024);
    a.stall();
    check(snap(Slot::Music).samplesCaptured == 3072, "A's pass continues");
}

void testBackwardJump()
{
    beginCase("a backwards timeline jump (Preview loop) ends the pass and disarms");
    Rig rig;
    CaptureStore::instance().arm(Slot::Music);
    rig.play(10000, 5120);
    rig.block(10000, 512);   // looped back to the start
    auto s = snap(Slot::Music);
    check(! s.armed && ! s.capturing, "should be disarmed after the loop");
    check(s.lastEnd == PassEnd::TimelineJumpBack, std::string("end reason: ") + passEndName(s.lastEnd));
    check(s.samplesCaptured == 5120, "the looped block must not be captured, got " + std::to_string(s.samplesCaptured));
    check(s.captured.start == 10000 && s.captured.end == 15120, "range");
    // Further audio while disarmed changes nothing.
    rig.play(10512, 4096);
    check(snap(Slot::Music).samplesCaptured == 5120, "nothing captured once disarmed");

    beginCase("a forward timeline jump also ends the pass");
    CaptureStore::instance().arm(Slot::Sfx);
    rig.play(0, 2048);
    rig.block(50000, 512);
    s = snap(Slot::Sfx);
    check(! s.armed && s.lastEnd == PassEnd::TimelineJump && s.samplesCaptured == 2048, "forward jump");
}

void testStall()
{
    beginCase("a stall (no audio for 300 ms) ends the pass and disarms; a short gap does not");
    Rig rig;
    CaptureStore::instance().arm(Slot::Music);
    rig.play(0, 4096);
    advanceMs(200.0);
    rig.processor->pollRunState();
    auto s = snap(Slot::Music);
    check(s.armed && s.capturing, "200 ms without audio is not a stall");
    rig.play(4096, 4096);   // carries on, contiguous
    advanceMs(250.0);
    rig.processor->pollRunState();
    check(snap(Slot::Music).capturing, "still capturing after 250 ms");
    advanceMs(100.0);
    rig.processor->pollRunState();
    s = snap(Slot::Music);
    check(! s.armed && ! s.capturing && s.lastEnd == PassEnd::Stall, "350 ms without audio ends the pass");
    check(s.samplesCaptured == 8192 && s.passes == 1, "one pass of 8192 samples");
}

void testMerge()
{
    beginCase("a later pass over the same slot merges by timeline position");
    Rig rig;
    auto& store = CaptureStore::instance();
    const std::int64_t n = 24000;

    store.arm(Slot::Music);
    rig.play(100000, n);
    rig.stall();

    store.arm(Slot::Music);
    rig.play(100000 + n / 2, n);   // overlaps the first half, extends past it
    rig.stall();
    auto s = snap(Slot::Music);
    check(s.passes == 2, "passes == 2");
    check(s.segments == 1, "one contiguous range");
    check(s.captured.start == 100000 && s.captured.end == 100000 + n + n / 2, "merged range "
          + std::to_string(s.captured.start) + ".." + std::to_string(s.captured.end));
    check(s.samplesCaptured == n + n / 2, "samples heard " + std::to_string(s.samplesCaptured));
    check(matches(audio(Slot::Music), 0, 100000, n + n / 2, 2, 0), "merged content follows the timeline");

    // A pass after a gap: two ranges, the gap is silence and not counted as heard.
    store.arm(Slot::Music);
    rig.play(200000, 4800);
    rig.stall();
    s = snap(Slot::Music);
    check(s.segments == 2 && s.passes == 3, "a gap gives two ranges");
    check(s.captured.start == 100000 && s.captured.end == 204800, "range spans both");
    check(s.samplesCaptured == n + n / 2 + 4800, "gap not counted as heard");
    check(s.bufferExtent == 104800, "buffer spans the gap");
    const auto got = audio(Slot::Music);
    check(got.size() == 104800 && got[50000] == 0.0f && got[99999] == 0.0f, "gap is silent");
    check(matches(got, 100000, 200000, 4800, 2, 0), "content after the gap");

    // The fraction heard of a clip we were told about.
    store.setExpectedRange(Slot::Music, TimelineRange { 100000, 210000 });
    s = snap(Slot::Music);
    const double expected = static_cast<double>(n + n / 2 + 4800) / 110000.0;
    check(s.fractionHeard && std::abs(*s.fractionHeard - expected) < 1e-12, "fraction heard");

    // A pass before everything we have: merged in front.
    store.arm(Slot::Music);
    rig.play(90000, 10000);
    rig.stall();
    s = snap(Slot::Music);
    check(s.passes == 4 && s.captured.start == 90000 && s.captured.end == 204800, "prepended range");
    check(s.segments == 2 && s.samplesCaptured == 10000 + n + n / 2 + 4800, "prepended coverage");
    check(matches(audio(Slot::Music), 0, 90000, 10000 + n + n / 2, 2, 0), "prepended content");
}

void testNoPlayHead()
{
    beginCase("no play head: capture works with the timeline unknown, and a new pass replaces");
    Rig rig(2, 48000.0, 512, false);
    auto& store = CaptureStore::instance();
    store.arm(Slot::Music);
    rig.play(0, 9000, 1);
    rig.stall();
    auto s = snap(Slot::Music);
    check(s.samplesCaptured == 9000 && ! s.timelineKnown && s.passes == 1, "first pass without a timeline");
    check(! s.armed && s.lastEnd == PassEnd::Stall, "stall still ends the pass");

    store.arm(Slot::Music);
    rig.play(0, 5000, 2);
    rig.stall();
    s = snap(Slot::Music);
    check(s.samplesCaptured == 5000 && s.passes == 1 && ! s.timelineKnown, "second pass replaces");
    check(matches(audio(Slot::Music), 0, 0, 5000, 2, 2), "replaced content is the second pass");

    beginCase("play head without timeInSamples behaves the same");
    Rig rig2;
    rig2.head.provideTime = false;
    store.arm(Slot::Sfx);
    rig2.play(0, 3000);
    rig2.stall();
    s = snap(Slot::Sfx);
    check(s.samplesCaptured == 3000 && ! s.timelineKnown, "captured without timeInSamples");
    store.setExpectedRange(Slot::Sfx, TimelineRange { 0, 6000 });
    s = snap(Slot::Sfx);
    check(s.fractionHeard && *s.fractionHeard == 0.5, "fraction heard falls back to length");
}

void testFrozenTimeline()
{
    beginCase("a host repeating one position every block: capture continues, range from that position");
    Rig rig;
    rig.head.frozen = true;
    rig.head.time = 480000;
    CaptureStore::instance().arm(Slot::Sfx);
    rig.play(0, 10240);
    auto s = snap(Slot::Sfx);
    check(s.capturing && s.timelineFrozen, "should be capturing with a frozen timeline");
    check(s.samplesCaptured == 10240, "all blocks captured: " + std::to_string(s.samplesCaptured));
    rig.stall();
    s = snap(Slot::Sfx);
    check(s.captured.start == 480000 && s.captured.end == 490240, "range from the frozen position");
    check(s.lastEnd == PassEnd::Stall, "ends on the stall");
}

void testCoarseTimeline()
{
    // What AudioSuite Preview does: blocks arrive in bursts that all carry one position, and the
    // position then steps on by about, not exactly, a burst.
    beginCase("a coarse timeline (bursts sharing one position): the whole pass is captured, a loop ends it");
    Rig rig;
    CaptureStore::instance().arm(Slot::Music);
    const std::int64_t start = 8539843;
    std::int64_t pos = start;
    int blocks = 0;
    for (int burst = 0; burst < 30; ++burst)
    {
        const int inBurst = burst == 0 ? 8 : 4;
        for (int b = 0; b < inBurst; ++b, ++blocks)
            rig.block(pos, 1024);
        pos += burst == 0 ? 1744 : 4096;
    }
    auto s = snap(Slot::Music);
    check(s.capturing && s.armed, "still capturing after the bursts");
    check(s.timelineFrozen, "timeline flagged approximate");
    check(s.samplesCaptured == static_cast<std::int64_t>(blocks) * 1024, "every block captured: " + std::to_string(s.samplesCaptured));
    check(s.captured.start == start, "range starts at the first position");

    rig.block(start, 1024);   // Preview looped
    s = snap(Slot::Music);
    check(! s.armed && s.lastEnd == PassEnd::TimelineJumpBack, std::string("loop ends the pass: ") + passEndName(s.lastEnd));
    check(s.samplesCaptured == static_cast<std::int64_t>(blocks) * 1024, "the looped block is not captured");

    // Learning again nearby replaces: approximate positions must not be spliced together.
    CaptureStore::instance().arm(Slot::Music);
    rig.block(start + 720, 1024);
    rig.block(start + 720, 1024);
    rig.stall();
    s = snap(Slot::Music);
    check(s.samplesCaptured == 2048 && s.bufferExtent == 2048, "a new pass replaces an approximate capture: " + std::to_string(s.bufferExtent));
}

void testSelectionLength()
{
    // AudioSuite Preview loops the selection, but the position it reports runs on past the end.
    // The selection's length (from the loop points) is what ends the pass, exactly at the wrap.
    beginCase("a looping Preview: the pass ends by itself after exactly one selection");
    const std::int64_t start = 8524145, length = 207287;   // not a multiple of the block size
    {
        Rig rig(2, 48000.0, 1024);
        rig.processor->useLoopPointsAsSelection(true);
        rig.head.selectionStart = start;
        rig.head.selectionLength = length;
        CaptureStore::instance().arm(Slot::Music);
        rig.play(start, length * 2 + 5000);   // two and a bit times round
        auto s = snap(Slot::Music);
        check(! s.armed && ! s.capturing, "disarmed at the end of the selection");
        check(s.lastEnd == PassEnd::SelectionEnd, std::string("end reason: ") + passEndName(s.lastEnd));
        check(s.samplesCaptured == length && s.bufferExtent == length, "exactly one selection captured: " + std::to_string(s.samplesCaptured));
        check(! s.truncated, "a complete selection is not a truncation");
        check(s.fractionHeard && *s.fractionHeard > 0.9999, "all of the selection heard");
        check(matches(audio(Slot::Music), 0, start, length, 2, 0), "content is the selection, sample for sample");
        check(rig.passthroughOk, "passthrough during the looping pass");
    }

    beginCase("stopping Preview early: the fraction of the selection heard is reported");
    {
        Rig rig(2, 48000.0, 1024);
        rig.processor->useLoopPointsAsSelection(true);
        rig.head.selectionStart = start;
        rig.head.selectionLength = length;
        CaptureStore::instance().arm(Slot::Sfx);
        rig.play(start, 51200);
        rig.stall();
        auto s = snap(Slot::Sfx);
        check(s.lastEnd == PassEnd::Stall && s.samplesCaptured == 51200, "ended on the stall");
        check(s.fractionHeard && std::abs(*s.fractionHeard - 51200.0 / static_cast<double>(length)) < 1.0e-6, "fraction heard");
    }

    beginCase("loop points are ignored outside AudioSuite");
    {
        Rig rig(2, 48000.0, 1024);
        rig.head.selectionStart = start;
        rig.head.selectionLength = 4096;
        CaptureStore::instance().arm(Slot::Music);
        rig.play(start, 20480);
        auto s = snap(Slot::Music);
        check(s.capturing && s.samplesCaptured == 20480, "a host's cycle range does not end the pass");
    }
}

void testTruncation()
{
    beginCase("buffer full: the pass ends, disarms and is flagged truncated");
    Rig rig(2, 48000.0, 4096);
    CaptureStore::instance().arm(Slot::Music);
    const std::int64_t limit = static_cast<std::int64_t>(CaptureStore::kMaxSeconds * 48000.0);
    rig.play(0, limit + 48000);   // 61 s
    auto s = snap(Slot::Music);
    check(! s.armed && ! s.capturing, "disarmed");
    check(s.truncated && s.lastEnd == PassEnd::BufferFull, "truncated flag and reason");
    check(s.samplesCaptured == limit, "exactly 60 s kept: " + std::to_string(s.samplesCaptured));
    const auto got = audio(Slot::Music);
    check(got.size() == static_cast<std::size_t>(limit) && matches(got, static_cast<std::size_t>(limit - 4096), limit - 4096, 4096, 2, 0),
          "the last kept samples are right");
    check(rig.passthroughOk, "passthrough while truncated");
}

void testIndependentSlots()
{
    beginCase("Music and Sfx are independent");
    Rig rig;
    auto& store = CaptureStore::instance();
    store.arm(Slot::Sfx);
    rig.play(0, 6000, 4);
    rig.stall();
    check(snap(Slot::Music).samplesCaptured == 0 && snap(Slot::Music).passes == 0, "Music untouched by an Sfx pass");
    check(snap(Slot::Sfx).samplesCaptured == 6000, "Sfx captured");

    store.arm(Slot::Music);
    rig.play(70000, 8000, 5);
    rig.stall();
    auto sfx = snap(Slot::Sfx);
    check(sfx.samplesCaptured == 6000 && sfx.passes == 1 && sfx.captured.start == 0, "Sfx unchanged by a Music pass");
    check(matches(audio(Slot::Sfx), 0, 0, 6000, 2, 4), "Sfx content unchanged");
    check(snap(Slot::Music).samplesCaptured == 8000, "Music captured");

    store.clear(Slot::Music);
    check(snap(Slot::Music).samplesCaptured == 0 && snap(Slot::Sfx).samplesCaptured == 6000, "clearing Music keeps Sfx");

    // Arming one slot does not arm the other.
    store.arm(Slot::Music);
    check(! snap(Slot::Sfx).armed, "Sfx not armed by arming Music");
    store.disarm(Slot::Music);
    check(! snap(Slot::Music).armed, "disarm");
}

void testUserDisarm()
{
    beginCase("disarming while audio flows ends the pass (and keeps what was heard)");
    Rig rig;
    auto& store = CaptureStore::instance();
    store.arm(Slot::Music);
    rig.play(0, 3000);
    store.disarm(Slot::Music);
    rig.play(3000, 3000);
    auto s = snap(Slot::Music);
    check(! s.armed && s.lastEnd == PassEnd::Disarmed && s.samplesCaptured == 3000, "user disarm");
}

void testInstanceGone()
{
    beginCase("deleting the capturing instance ends its pass, so a new instance is not locked out");
    auto& store = CaptureStore::instance();
    {
        Rig preview;
        store.arm(Slot::Music);
        preview.play(0, 4096);
        check(snap(Slot::Music).capturing, "capturing");
    }
    auto s = snap(Slot::Music);
    check(! s.capturing && ! s.armed && s.lastEnd == PassEnd::OwnerDestroyed && s.samplesCaptured == 4096,
          std::string("pass ended by the deletion: ") + passEndName(s.lastEnd));
    Rig render;
    store.arm(Slot::Music);
    render.play(0, 4096);
    s = snap(Slot::Music);
    check(s.capturing && s.owner == render.id(), "the next instance captures straight away");
}

bool writePng(std::unique_ptr<juce::AudioProcessorEditor>& editor, const juce::File& folder, const char* name)
{
    if (auto* e = dynamic_cast<TonalRecallEditor*>(editor.get()))
        e->refreshNow();
    const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 2.0f);
    if (! image.isValid())
        return false;
    if (folder == juce::File())
        return true;
    folder.createDirectory();
    const auto file = folder.getChildFile(name);
    file.deleteFile();
    juce::FileOutputStream stream(file);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream(image, stream);
    if (ok)
        std::printf("    -> %s\n", file.getFullPathName().toRawUTF8());
    return ok;
}

void testEditor(const juce::File& pngFolder)
{
    beginCase("the editor builds and paints a learned Music side and a capturing Sfx side");
    Rig rig;
    CaptureStore::instance().arm(Slot::Music);
    rig.play(48000, 48000);
    rig.stall();
    CaptureStore::instance().arm(Slot::Sfx);
    rig.play(0, 4800, 1);
    std::unique_ptr<juce::AudioProcessorEditor> editor(rig.processor->createEditor());
    check(editor->getWidth() == TonalRecallEditor::kWidth && editor->getHeight() == TonalRecallEditor::kHeight, "editor size");
    check(writePng(editor, {}, nullptr), "snapshot");
    editor.reset();

    beginCase("the empty editor");
    {
        Rig empty;
        std::unique_ptr<juce::AudioProcessorEditor> e(empty.processor->createEditor());
        check(writePng(e, pngFolder, "editor-empty.png"), "write the empty editor");
        auto* tr = dynamic_cast<TonalRecallEditor*>(e.get());
        check(tr != nullptr && tr->currentWarnings().isEmpty(), "nothing to warn about: the strip says READY");
        tr->showAbout(true);
        check(writePng(e, pngFolder, "editor-about.png"), "write the About overlay");
        if (tr != nullptr)
        {
            // The new-version popup, as the shared update check would show it.
            tr->showAbout(false);
            sammyj::UpdateInfo info;
            info.pluginId = "tonalrecall";
            info.currentVersion = PLUGIN_VERSION;
            info.latestVersion = "9.9.9";
            tr->showUpdate(info);
            check(writePng(e, pngFolder, "editor-update.png"), "write the update popup");
            tr->showInsertNotice(true);
            check(writePng(e, pngFolder, "editor-insert.png"), "write the AudioSuite-only notice");
            tr->showInsertNotice(false);
        }
    }
    check(TonalRecallEditor::textOverflowCount() == 0, "every text fits its box: " + std::to_string(TonalRecallEditor::textOverflowCount()) + " did not");
}

//==============================================================================
// Learn -> analysis -> match -> Stamp, end to end.

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// Adds a note with 1/h harmonics (10 ms fades); offsetAt gives a pitch movement in semitones.
template <typename Fn>
void addNote(std::vector<float>& x, double midi, double amp, int harmonics, std::size_t from, std::size_t to, Fn&& offsetAt)
{
    std::vector<double> phase(static_cast<std::size_t>(harmonics), 0.0);
    to = std::min(to, x.size());
    for (std::size_t i = from; i < to; ++i)
    {
        const double hz = core::midiToHz(midi + offsetAt(static_cast<double>(i - from) / 48000.0));
        const double fade = std::min({ 1.0, static_cast<double>(i - from) / 480.0, static_cast<double>(to - 1 - i) / 480.0 });
        double v = 0.0;
        for (int h = 1; h <= harmonics; ++h)
        {
            if (hz * h > 0.45 * 48000.0)
                break;
            auto& p = phase[static_cast<std::size_t>(h - 1)];
            p += kTwoPi * hz * h / 48000.0;
            v += std::sin(p) / h;
        }
        x[i] += static_cast<float>(amp * fade * v);
    }
}

void addNote(std::vector<float>& x, double midi, double amp, int harmonics, std::size_t from, std::size_t to)
{
    addNote(x, midi, amp, harmonics, from, to, [](double) { return 0.0; });
}

std::vector<float> dMinorPad()
{
    std::vector<float> x(4 * 48000, 0.0f);
    for (double midi : { 38.0, 45.0, 50.0, 53.0, 57.0 })   // D2 A2 D3 F3 A3
        addNote(x, midi, 0.12, 6, 0, x.size());
    std::uint32_t seed = 1;
    for (auto& v : x)
    {
        seed = seed * 1664525u + 1013904223u;
        v += 0.003f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    return x;
}

// The learned effect: B2 +38 cents with a little vibrato. The match to D is about +2.62 st.
std::vector<float> effectTone()
{
    std::vector<float> x(2 * 48000, 0.0f);
    addNote(x, 47.38, 0.3, 8, 0, x.size(), [](double t) { return 0.15 * std::sin(kTwoPi * 5.0 * t); });
    return x;
}

// Stereo from mono: the right channel a little quieter, so the two are not identical.
struct Stereo
{
    std::vector<float> l, r;
};

Stereo stereoOf(const std::vector<float>& mono)
{
    Stereo s { mono, mono };
    for (auto& v : s.r)
        v *= 0.8f;
    return s;
}

// Plays a stereo signal through the rig in blocks, the play head at start (or frozen), and returns
// what came out.
Stereo playStereo(Rig& rig, const Stereo& in, int block, std::int64_t start)
{
    Stereo out { std::vector<float>(in.l.size(), 0.0f), std::vector<float>(in.r.size(), 0.0f) };
    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;
    for (std::size_t at = 0; at < in.l.size(); at += static_cast<std::size_t>(block))
    {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), in.l.size() - at));
        buffer.setSize(2, n, false, false, true);
        std::copy(in.l.begin() + static_cast<std::ptrdiff_t>(at), in.l.begin() + static_cast<std::ptrdiff_t>(at) + n, buffer.getWritePointer(0));
        std::copy(in.r.begin() + static_cast<std::ptrdiff_t>(at), in.r.begin() + static_cast<std::ptrdiff_t>(at) + n, buffer.getWritePointer(1));
        if (! rig.head.frozen)
            rig.head.time = start + static_cast<std::int64_t>(at);
        rig.processor->processBlock(buffer, midi);
        std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + n, out.l.begin() + static_cast<std::ptrdiff_t>(at));
        std::copy(buffer.getReadPointer(1), buffer.getReadPointer(1) + n, out.r.begin() + static_cast<std::ptrdiff_t>(at));
        advanceMs(1000.0 * n / rig.sampleRate);
    }
    return out;
}

// Learns a signal the way AudioSuite Preview delivers it: armed, the selection as loop points,
// blocks of 1024 from the selection's start; the pass ends by itself at the end of the selection.
void learn(Rig& rig, Slot slot, const std::vector<float>& mono, std::int64_t timelineStart)
{
    rig.head.frozen = false;
    rig.head.selectionStart = timelineStart;
    rig.head.selectionLength = static_cast<std::int64_t>(mono.size());
    CaptureStore::instance().arm(slot);
    auto padded = mono;
    padded.resize(mono.size() + 2048, 0.0f);   // Preview keeps going past the end of the selection
    playStereo(rig, stereoOf(padded), 1024, timelineStart);
    rig.stall();
}

double medianMidi(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    hindsight::Settings settings;
    settings.method = hindsight::Method::Tonal;
    const auto a = hindsight::analyse(x.data() + from, static_cast<int>(to - from), 48000.0, settings);
    std::vector<double> midi;
    for (const auto& f : a.line.frames)
        if (f.hz > 0.0f && ! f.bridged)
            midi.push_back(core::hzToMidi(f.hz));
    if (midi.empty())
        return 0.0;
    std::sort(midi.begin(), midi.end());
    return midi[midi.size() / 2];
}

bool sameBitsRange(const std::vector<float>& a, std::size_t aFrom, const std::vector<float>& b, std::size_t bFrom, std::size_t count)
{
    return aFrom + count <= a.size() && bFrom + count <= b.size()
        && std::memcmp(a.data() + aFrom, b.data() + bFrom, count * sizeof(float)) == 0;
}

bool allZero(const std::vector<float>& a, std::size_t from, std::size_t count)
{
    for (std::size_t i = from; i < from + count && i < a.size(); ++i)
        if (std::abs(a[i]) > 0.0f)
            return false;
    return true;
}

// The render: [2 s of other audio][the learned effect][2 s of other audio], then `latency` samples
// more, as the host would have to deliver to collect the delayed output.
Stereo renderStream(const std::vector<float>& effect, int latency, bool withEffect)
{
    const std::size_t handle = 2 * 48000;
    std::vector<float> mono(handle + effect.size() + handle, 0.0f);
    addNote(mono, 52.0, 0.2, 6, 0, handle);
    addNote(mono, 40.0, 0.2, 6, handle + effect.size(), mono.size());
    if (withEffect)
        std::copy(effect.begin(), effect.end(), mono.begin() + static_cast<std::ptrdiff_t>(handle));
    else
        addNote(mono, 45.0, 0.3, 8, handle, handle + effect.size());
    auto s = stereoOf(mono);
    s.l.resize(mono.size() + static_cast<std::size_t>(latency), 0.0f);
    s.r.resize(s.l.size(), 0.0f);
    return s;
}

Stereo render(Rig& rig, const Stereo& stream, std::int64_t selectionLength)
{
    rig.stall();   // the previous run is over
    rig.processor->setNonRealtime(true);
    rig.head.frozen = true;   // offline, the host reports timeInSamples 0 throughout
    rig.head.time = 0;
    rig.head.selectionStart = 0;
    rig.head.selectionLength = selectionLength;
    auto out = playStereo(rig, stream, 1024, 0);
    rig.stall();
    return out;
}

void testEndToEnd(const juce::File& pngFolder)
{
    beginCase("end to end: learn a D minor pad and a tone, the hub matches them, a Render stamps the shift");
    auto& hub = MatchHub::instance();
    Rig rig(2, 48000.0, 1024);
    rig.processor->useLoopPointsAsSelection(true);
    const int latency = rig.processor->getReportedLatency();
    check(latency > 0 && rig.processor->getLatencySamples() == latency, "latency reported: " + std::to_string(latency));

    const auto pad = dMinorPad();
    const auto tone = effectTone();
    learn(rig, Slot::Music, pad, 480000);
    learn(rig, Slot::Sfx, tone, 960000);
    check(snap(Slot::Music).samplesCaptured == static_cast<std::int64_t>(pad.size()) && snap(Slot::Music).lastEnd == PassEnd::SelectionEnd,
          "the whole pad was learned");
    check(snap(Slot::Sfx).samplesCaptured == static_cast<std::int64_t>(tone.size()), "the whole tone was learned");
    {
        std::vector<std::vector<float>> separate;
        CaptureStore::instance().copyChannels(Slot::Sfx, separate);
        check(snap(Slot::Sfx).channels == 2 && separate.size() == 2 && separate[0].size() == tone.size(), "the effect is kept in stereo");
        std::vector<std::vector<float>> music;
        CaptureStore::instance().copyChannels(Slot::Music, music);
        check(snap(Slot::Music).channels == 2 && music.size() == 2 && music[0].size() == pad.size(), "the music is kept in stereo too");
    }
    check(hub.waitUntilIdle(60.0), "the hub finished analysing in time");

    HubSnapshot h;
    hub.snapshot(h);
    std::printf("    -> %s, root %s; effect %s; shift %+.3f st to %s; plan %s\n", h.music.keyName().c_str(),
                core::noteName(h.music.rootMidi).c_str(), core::noteName(h.sfx.centreMidi).c_str(), h.match.semitones,
                core::noteName(h.match.targetMidi).c_str(), h.planReady ? "ready" : "missing");
    check(h.hasMusic && h.music.valid && h.music.rootPitchClass == 2 && h.music.minor, "music: D minor");
    check(h.hasSfx && h.sfx.pitched && std::abs(h.sfx.centreMidi - 47.38) < 0.06, "effect: B2 +38 c");
    check(h.match.valid && h.match.targetPitchClass == 2 && std::abs(h.match.semitones - 2.62) < 0.08, "match: about +2.62 st to D");
    check(h.offsetKnown && std::abs(h.sfxOffsetInMusicSeconds - 10.0) < 1.0e-9, "the effect sits 10 s after the music's start");
    check(h.planReady && hub.planGeneration() != 0, "a stamp plan is published");

    {
        std::unique_ptr<juce::AudioProcessorEditor> e(rig.processor->createEditor());
        check(writePng(e, pngFolder, "editor-matched.png"), "write the matched editor");
        auto* tr = dynamic_cast<TonalRecallEditor*>(e.get());
        check(tr != nullptr && tr->currentWarnings().isEmpty(), "nothing to warn about once both are learned: "
              + (tr != nullptr ? tr->currentWarnings().joinIntoString(" | ").toStdString() : std::string()));
    }

    step("end to end: Render with handles, Stamp on");
    {
        const auto stream = renderStream(tone, latency, true);
        const auto out = render(rig, stream, static_cast<std::int64_t>(tone.size()));
        const auto run = rig.processor->lastStampRun();
        check(run.mode == StampRunInfo::Mode::Stamp && run.nonRealtime && run.stampOn, "the run stamped");
        check(run.aligned && run.alignmentOffset == 96000, "alignment found at sample 96000: "
              + std::to_string(run.aligned) + " / " + std::to_string(run.alignmentOffset));
        check(run.samples == static_cast<std::int64_t>(stream.l.size()) && run.ended, "the run's numbers are final");
        check(std::abs(run.semitones - h.match.semitones) < 1.0e-9, "the shift applied is the match");
        const std::vector<float> aligned(out.l.begin() + latency, out.l.end());
        const double midi = medianMidi(aligned, 96000 + 4800, 96000 + tone.size() - 4800);
        const double before = medianMidi(stream.l, 96000 + 4800, 96000 + tone.size() - 4800);
        std::printf("    -> learned region %.3f before, %.3f after (target %.3f)\n", before, midi, h.match.targetMidi);
        check(std::abs(midi - h.match.targetMidi) < 0.1, "the learned region lands on the target note");
        const double handle = medianMidi(aligned, 4800, 96000 - 4800);
        check(std::abs(handle - (52.0 + h.match.semitones)) < 0.15, "the handles get the fixed shift too");
    }

    step("end to end: Stamp off, Render: the dry input, delayed by the latency");
    {
        auto o = hub.options();
        o.stampEnabled = false;
        hub.setOptions(o);
        const auto stream = renderStream(tone, latency, true);
        const auto out = render(rig, stream, static_cast<std::int64_t>(tone.size()));
        const auto n = stream.l.size() - static_cast<std::size_t>(latency);
        check(rig.processor->lastStampRun().mode == StampRunInfo::Mode::Dry, "a dry run");
        check(allZero(out.l, 0, static_cast<std::size_t>(latency)) && allZero(out.r, 0, static_cast<std::size_t>(latency)), "silence for the latency");
        check(sameBitsRange(out.l, static_cast<std::size_t>(latency), stream.l, 0, n) && sameBitsRange(out.r, static_cast<std::size_t>(latency), stream.r, 0, n),
              "then the input, bit for bit");

        step("end to end: Stamp off, Preview: the dry input, not delayed");
        rig.stall();
        rig.processor->setNonRealtime(false);
        rig.head.frozen = false;
        const auto preview = stereoOf(tone);
        const auto heard = playStereo(rig, preview, 1024, 960000);
        check(sameBitsRange(heard.l, 0, preview.l, 0, preview.l.size()) && sameBitsRange(heard.r, 0, preview.r, 0, preview.r.size()),
              "bit-identical passthrough");
        o.stampEnabled = true;
        hub.setOptions(o);
    }

    step("end to end: a bend on a clip that was not learned: fixed shift, and the editor warns");
    {
        auto o = hub.options();
        o.bend = -1.0;
        hub.setOptions(o);
        check(hub.waitUntilIdle(30.0), "the hub recomputed the match");
        const auto stream = renderStream(tone, latency, false);
        render(rig, stream, static_cast<std::int64_t>(tone.size()));
        const auto run = rig.processor->lastStampRun();
        check(run.mode == StampRunInfo::Mode::Stamp && run.bendActive && ! run.aligned, "stamped without alignment");
        CaptureStore::instance().arm(Slot::Music);
        std::unique_ptr<juce::AudioProcessorEditor> e(rig.processor->createEditor());
        auto* tr = dynamic_cast<TonalRecallEditor*>(e.get());
        tr->refreshNow();
        const auto warnings = tr->currentWarnings();
        check(warnings.size() >= 2 && warnings[0].startsWith("THE CURVE WAS NOT APPLIED") && warnings.contains("LEARN ARMED FOR THE MUSIC: SELECT IT AND PRESS PREVIEW IN THE AUDIOSUITE WINDOW"),
              "warnings: " + warnings.joinIntoString(" | ").toStdString());
        check(writePng(e, pngFolder, "editor-warning.png"), "write the warning editor");
        e.reset();
        CaptureStore::instance().disarm(Slot::Music);
        o.bend = 0.0;
        hub.setOptions(o);
    }

    step("end to end: a slot armed: Render is dry and delayed, Preview is dry");
    {
        CaptureStore::instance().arm(Slot::Music);   // a "learn by Render": the output must line up
        const auto stream = renderStream(tone, latency, true);
        const auto out = render(rig, stream, static_cast<std::int64_t>(stream.l.size()));
        const auto n = stream.l.size() - static_cast<std::size_t>(latency);
        check(rig.processor->lastStampRun().mode == StampRunInfo::Mode::Learn, "a learn run");
        check(allZero(out.l, 0, static_cast<std::size_t>(latency)) && sameBitsRange(out.l, static_cast<std::size_t>(latency), stream.l, 0, n)
              && sameBitsRange(out.r, static_cast<std::size_t>(latency), stream.r, 0, n), "Render: the input delayed by the latency");

        rig.stall();
        rig.processor->setNonRealtime(false);
        rig.head.frozen = false;
        CaptureStore::instance().arm(Slot::Sfx);
        const auto preview = stereoOf(tone);
        const auto heard = playStereo(rig, preview, 1024, 960000);
        check(sameBitsRange(heard.l, 0, preview.l, 0, preview.l.size()) && sameBitsRange(heard.r, 0, preview.r, 0, preview.r.size()),
              "Preview: bit-identical passthrough");
        rig.stall();
    }
    check(TonalRecallEditor::textOverflowCount() == 0, "every text fits its box: " + std::to_string(TonalRecallEditor::textOverflowCount()) + " did not");
}

// A loud, rich tone close to full scale whose partials are lined up to keep its peaks as low as
// they can be (as a normalised or limited recording's are): any shift raises them. A2, 2 s.
std::vector<float> loudTone(float peakWanted)
{
    std::vector<float> x(2 * 48000, 0.0f);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        double v = 0.0;
        for (int h = 1; h <= 16; ++h)
            v += std::sin(kTwoPi * 110.0 * h * static_cast<double>(i) / 48000.0 - std::numbers::pi * h * (h - 1) / 16.0);
        x[i] = static_cast<float>(v);
    }
    float peak = 0.0f;
    for (const float v : x)
        peak = std::max(peak, std::abs(v));
    for (auto& v : x)
        v *= peakWanted / peak;
    return x;
}

float peakOf(const Stereo& s)
{
    float peak = 0.0f;
    for (std::size_t i = 0; i < s.l.size(); ++i)
        peak = std::max({ peak, std::abs(s.l[i]), std::abs(s.r[i]) });
    return peak;
}

bool anyWarningStartsWith(const juce::StringArray& warnings, const char* start)
{
    for (const auto& w : warnings)
        if (w.startsWith(start))
            return true;
    return false;
}

void testOutputSafety(const juce::File& pngFolder)
{
    beginCase("output safety: a loud effect is measured and trimmed to stay under full scale; the limiter holds the rest");
    using core::PeakLimiter;
    auto& hub = MatchHub::instance();
    Rig rig(2, 48000.0, 1024);
    rig.processor->useLoopPointsAsSelection(true);
    const int latency = rig.processor->getReportedLatency();
    const auto tone = loudTone(0.985f);
    learn(rig, Slot::Music, dMinorPad(), 480000);
    learn(rig, Slot::Sfx, tone, 960000);
    check(hub.waitUntilIdle(60.0), "analysed, matched and measured");

    HubSnapshot h;
    hub.snapshot(h);
    std::printf("    -> shift %+.3f st; effect peak %.3f, stamped peak %.3f, trim %.2f dB\n", h.match.semitones,
                static_cast<double>(h.levels.inputPeak), static_cast<double>(h.levels.outputPeak), static_cast<double>(h.trimDb));
    check(h.match.valid && h.planReady && std::abs(h.match.semitones) > 0.5, "a shift to stamp");
    check(h.trimKnown && ! h.trimMeasuring && h.levels.complete, "the stamp's level is measured once the hub is idle");
    check(std::abs(h.levels.inputPeak - 0.985f) < 1.0e-3f, "the effect's own peak is reported");
    check(h.levels.outputPeak > PeakLimiter::kCeiling && h.trimDb < -0.1f, "shifted, it would go over full scale: a trim is asked for");
    const auto plan = hub.planGeneration();
    {
        float gain = 0.0f;
        check(hub.trimFor(plan, gain) && std::abs(gain - std::pow(10.0f, h.trimDb / 20.0f)) < 1.0e-4f, "the audio thread is handed that trim");
        check(! hub.trimFor(plan + 1, gain), "and no trim for a plan that was not measured");
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> e(rig.processor->createEditor());
        check(writePng(e, pngFolder, "editor-trim.png"), "write the editor with a trim applied");
        auto* tr = dynamic_cast<TonalRecallEditor*>(e.get());
        const auto warnings = tr != nullptr ? tr->currentWarnings() : juce::StringArray();
        check(! anyWarningStartsWith(warnings, "TRIM IS OFF") && ! anyWarningStartsWith(warnings, "THE LAST RENDER")
              && ! anyWarningStartsWith(warnings, "THE EFFECT REACHES"), "nothing to warn about with TRIM on: " + warnings.joinIntoString(" | ").toStdString());
    }

    step("output safety: Render with TRIM on: under the ceiling, and the limiter has nothing to do");
    {
        // The clip as a Render with no handles delivers it: what was measured.
        auto stream = stereoOf(tone);
        stream.l.resize(stream.l.size() + static_cast<std::size_t>(latency), 0.0f);
        stream.r.resize(stream.l.size(), 0.0f);
        const auto out = render(rig, stream, static_cast<std::int64_t>(tone.size()));
        const auto run = rig.processor->lastStampRun();
        const float peak = peakOf(out);
        std::printf("    -> rendered peak %.4f, trim %.2f dB, limiter %.3f dB\n", static_cast<double>(peak), static_cast<double>(run.trimDb),
                    static_cast<double>(run.limiterDb));
        check(run.mode == StampRunInfo::Mode::Stamp && run.nonRealtime && run.aligned, "the Render stamped the learned effect");
        check(run.trimKnown && std::abs(run.trimDb - h.trimDb) < 0.01f, "with the measured trim");
        check(peak <= PeakLimiter::kCeiling, "the rendered peak is at or under -0.1 dBFS: " + std::to_string(peak));
        check(peak > 0.9f * PeakLimiter::kCeiling, "and not turned down more than it needs: " + std::to_string(peak));
        check(run.limiterDb < 0.05f && rig.processor->lastRenderLimiterDb() < 0.05f,
              "the limiter had next to nothing to do: " + std::to_string(run.limiterDb) + " dB");

        // With other audio running into the clip (handles), the stretcher comes into the effect in
        // another state and the peaks are not quite those measured: the limiter holds what is left.
        const auto withHandles = render(rig, renderStream(tone, latency, true), static_cast<std::int64_t>(tone.size()));
        const auto handled = rig.processor->lastStampRun();
        std::printf("    -> with handles: rendered peak %.4f, limiter %.3f dB\n", static_cast<double>(peakOf(withHandles)),
                    static_cast<double>(handled.limiterDb));
        check(handled.trimKnown && peakOf(withHandles) <= PeakLimiter::kCeiling, "with handles, still at or under -0.1 dBFS");
    }

    step("output safety: TRIM off: no new measurement, and the limiter holds the Render under the ceiling");
    {
        auto o = hub.options();
        o.trimEnabled = false;
        hub.setOptions(o);
        check(hub.waitUntilIdle(10.0), "idle");
        HubSnapshot off;
        hub.snapshot(off);
        float gain = 0.0f;
        check(hub.planGeneration() == plan && off.trimKnown && std::abs(off.trimDb - h.trimDb) < 1.0e-6f,
              "switching TRIM keeps the plan and its measurement");
        check(hub.trimFor(plan, gain) && gain == 1.0f, "and hands out no trim");

        auto stream = stereoOf(tone);
        stream.l.resize(stream.l.size() + static_cast<std::size_t>(latency), 0.0f);
        stream.r.resize(stream.l.size(), 0.0f);
        const auto out = render(rig, stream, static_cast<std::int64_t>(tone.size()));
        const auto run = rig.processor->lastStampRun();
        const float peak = peakOf(out);
        std::printf("    -> rendered peak %.4f, limiter %.3f dB\n", static_cast<double>(peak), static_cast<double>(run.limiterDb));
        check(peak <= PeakLimiter::kCeiling, "the rendered peak is still at or under -0.1 dBFS: " + std::to_string(peak));
        check(run.limiterDb > 0.1f && std::abs(rig.processor->lastRenderLimiterDb() - run.limiterDb) < 1.0e-6f,
              "and the limiter reports that it held it: " + std::to_string(run.limiterDb) + " dB");

        std::unique_ptr<juce::AudioProcessorEditor> e(rig.processor->createEditor());
        auto* tr = dynamic_cast<TonalRecallEditor*>(e.get());
        tr->refreshNow();
        const auto warnings = tr->currentWarnings();
        check(anyWarningStartsWith(warnings, "THE LAST RENDER WENT OVER FULL SCALE: THE LIMITER HELD ITS PEAKS DOWN BY")
              && anyWarningStartsWith(warnings, "TRIM IS OFF AND THE STAMP GOES"),
              "the strip says so: " + warnings.joinIntoString(" | ").toStdString());
        check(writePng(e, pngFolder, "editor-trim-off.png"), "write the editor with TRIM off");
        e.reset();

        o.trimEnabled = true;
        hub.setOptions(o);
    }

    step("output safety: Preview picks the trim up, and the audition is played at it");
    {
        rig.stall();
        rig.processor->setNonRealtime(false);
        rig.head.frozen = false;
        rig.head.selectionStart = 960000;
        rig.head.selectionLength = static_cast<std::int64_t>(tone.size());
        auto preview = stereoOf(tone);
        preview.l.resize(preview.l.size() + static_cast<std::size_t>(latency), 0.0f);
        preview.r.resize(preview.l.size(), 0.0f);
        const auto out = playStereo(rig, preview, 1024, 960000);
        const auto run = rig.processor->lastStampRun();
        const float peak = peakOf(out);
        std::printf("    -> previewed peak %.4f, trim %.2f dB, limiter %.3f dB\n", static_cast<double>(peak), static_cast<double>(run.trimDb),
                    static_cast<double>(run.limiterDb));
        check(run.mode == StampRunInfo::Mode::Stamp && ! run.nonRealtime && run.trimKnown && std::abs(run.trimDb - h.trimDb) < 0.01f,
              "Preview stamps with the measured trim");
        check(peak <= PeakLimiter::kCeiling && run.limiterDb < 0.05f, "under the ceiling with next to nothing for the limiter: " + std::to_string(peak));

        // The audition is mixed in after the limiter: it is turned down by the trim itself, so a
        // drag is heard at the level the stamp will have. Played with TRIM on and off, sample by
        // sample over the steady part (away from full scale, where the audition is clipped).
        auto& bus = AuditionBus::instance();
        HubSnapshot now;
        hub.snapshot(now);
        bus.setSource(now.sfxChannels, 48000.0, false);
        bus.setCurve(now.match.curve, now.match.hopSeconds);
        const Stereo silence { std::vector<float>(96000, 0.0f), std::vector<float>(96000, 0.0f) };
        const auto auditionLevel = [&](bool trimOn)
        {
            auto o = hub.options();
            o.stampEnabled = false;   // only the audition is heard
            o.trimEnabled = trimOn;
            hub.setOptions(o);
            bus.playFrom(0.0);
            const auto heard = playStereo(rig, silence, 1024, 0);
            bus.stop();
            playStereo(rig, silence, 1024, 0);
            return heard.l;
        };
        const auto on = auditionLevel(true), off = auditionLevel(false);
        auto o = hub.options();
        o.stampEnabled = true;
        o.trimEnabled = true;
        hub.setOptions(o);
        std::vector<double> ratios;
        for (std::size_t i = 24000; i < 72000; ++i)
            if (std::abs(off[i]) > 0.1f && std::abs(off[i]) < 0.9f)
                ratios.push_back(static_cast<double>(on[i]) / static_cast<double>(off[i]));
        std::sort(ratios.begin(), ratios.end());
        const double difference = ratios.empty() ? 0.0 : 20.0 * std::log10(std::max(1.0e-6, ratios[ratios.size() / 2]));
        std::printf("    -> audition with TRIM on is %.2f dB against off (trim %.2f dB)\n", difference, static_cast<double>(h.trimDb));
        check(ratios.size() > 1000 && std::abs(difference - static_cast<double>(h.trimDb)) < 0.1, "the audition is turned down by the trim");
    }

    step("output safety: a Render that starts while the level is still being measured waits for it");
    {
        auto o = hub.options();
        o.octaveShift = -1;
        hub.setOptions(o);
        // The new plan is out, and its measurement has only just begun.
        for (int tries = 0; hub.planGeneration() == plan && tries < 5000; ++tries)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        float gain = 0.0f;
        const bool knownAlready = hub.trimFor(hub.planGeneration(), gain);
        auto stream = stereoOf(tone);
        stream.l.resize(stream.l.size() + static_cast<std::size_t>(latency), 0.0f);
        stream.r.resize(stream.l.size(), 0.0f);
        const auto out = render(rig, stream, static_cast<std::int64_t>(tone.size()));
        const auto run = rig.processor->lastStampRun();
        HubSnapshot now;
        hub.snapshot(now);
        std::printf("    -> measured before the Render began: %s; trim %.2f dB, limiter %.3f dB\n", knownAlready ? "yes" : "no",
                    static_cast<double>(run.trimDb), static_cast<double>(run.limiterDb));
        check(run.planGeneration == hub.planGeneration() && run.trimKnown && std::abs(run.trimDb - now.trimDb) < 0.01f,
              "the Render used the new plan's trim");
        check(peakOf(out) <= PeakLimiter::kCeiling && run.limiterDb < 0.05f, "and the limiter had next to nothing to do");
        o.octaveShift = 0;
        hub.setOptions(o);
        check(hub.waitUntilIdle(30.0), "idle");
        rig.processor->setNonRealtime(false);   // Learn listens to Preview
    }

    step("output safety: an effect learned at full scale is flagged");
    {
        learn(rig, Slot::Sfx, loudTone(1.0f), 4800000);
        check(hub.waitUntilIdle(60.0), "analysed again");
        std::unique_ptr<juce::AudioProcessorEditor> e(rig.processor->createEditor());
        auto* tr = dynamic_cast<TonalRecallEditor*>(e.get());
        tr->refreshNow();
        const auto warnings = tr->currentWarnings();
        check(warnings.contains("THE EFFECT REACHES FULL SCALE AS LEARNED: IT MAY HAVE BEEN CLIPPED BEFORE IT GOT HERE"),
              "the strip says it may have been clipped: " + warnings.joinIntoString(" | ").toStdString());
    }
    check(TonalRecallEditor::textOverflowCount() == 0, "every text fits its box: " + std::to_string(TonalRecallEditor::textOverflowCount()) + " did not");
}

void testTail(const juce::File& pngFolder)
{
    beginCase("tail: an effect longer than the learned music, with a follow bend and a hand adjustment");
    auto& hub = MatchHub::instance();
    Rig rig(2, 48000.0, 1024);
    rig.processor->useLoopPointsAsSelection(true);

    auto music = dMinorPad();
    music.resize(96000);   // 2 s
    auto effect = effectTone();
    const auto once = effect;
    while (effect.size() < 192000)
        effect.insert(effect.end(), once.begin(), once.end());
    effect.resize(192000);   // 4 s, starting where the music starts
    learn(rig, Slot::Music, music, 480000);
    learn(rig, Slot::Sfx, effect, 480000);
    check(hub.waitUntilIdle(60.0), "analysed");

    auto o = hub.options();
    o.bend = 1.0;
    o.edits = { { 3.0, -0.5 } };
    hub.setOptions(o);
    check(hub.waitUntilIdle(10.0), "matched");

    HubSnapshot h;
    hub.snapshot(h);
    const auto frames = static_cast<int>(h.match.curve.size());
    std::printf("    -> covered frames %d..%d of %d, %zu hand point(s)\n", h.match.coveredFrom, h.match.coveredTo, frames, h.options.edits.size());
    check(h.match.valid && h.match.coveredFrom == 0 && std::abs(h.match.coveredTo - frames / 2) <= 4, "the second half is outside the learned music");
    check(h.options.edits.size() == 1 && h.options.bendActive(), "the hand point is kept and counts as a moving shift");
    const auto at = static_cast<std::size_t>(3.0 / h.match.hopSeconds);
    check(std::abs(h.match.curve[at] - h.match.curve.front() + 0.5f) < 0.1f, "half a semitone down at the point");

    {
        std::unique_ptr<juce::AudioProcessorEditor> e(rig.processor->createEditor());
        check(writePng(e, pngFolder, "editor-tail.png"), "write the tail editor");
    }

    // The three curves on offer are worked out, and picking one sets the options to it.
    {
        HubSnapshot hs;
        hub.snapshot(hs);
        check(hs.styleAfterMidi[0].size() == hs.match.curve.size() && hs.styleAfterMidi[1].size() == hs.match.curve.size()
                  && hs.styleAfterMidi[2].size() == hs.match.curve.size(), "all three styles have a curve");
        // (This case carries a hand point, which every style keeps; the styles differ beyond it.)
        const MatchOptions start;
        check(start.style() == static_cast<int>(MatchOptions::Style::Locked) && start.target == core::Target::Nearest && start.bend == 0.0,
              "a new instance starts Locked: the nearest note, no movement");
        const auto fresh = start.withStyle(MatchOptions::Style::Subtle);
        check(fresh.style() == static_cast<int>(MatchOptions::Style::Subtle) && fresh.target == core::Target::Follow && fresh.bend > 0.0,
              "Subtle: follows, with a little of the music's movement");
        const auto lockedOptions = fresh.withStyle(MatchOptions::Style::Locked), movingOptions = fresh.withStyle(MatchOptions::Style::Moving);
        check(lockedOptions.style() == 0 && lockedOptions.target == core::Target::Nearest && lockedOptions.bend == 0.0, "Locked: the nearest note, no movement");
        check(movingOptions.style() == 2 && movingOptions.target == core::Target::Follow && movingOptions.bend > fresh.bend, "Moving: more of the music's movement");
        check(movingOptions.withStyle(MatchOptions::Style::Subtle).style() == 1, "and back to Subtle");
        auto root = start;
        root.target = core::Target::Root;
        check(root.withStyle(MatchOptions::Style::Locked).target == core::Target::Root, "Locked keeps a note the user picked");
        auto custom = fresh;
        custom.bend = 0.6;
        check(custom.style() == -1, "a bend set by hand is none of the three");
        check(lockedOptions.withStyle(MatchOptions::Style::Subtle).withStyle(MatchOptions::Style::Locked).target == core::Target::Nearest,
              "Locked, Subtle and back to Locked returns to the same note");
        auto fifth = start;
        fifth.target = core::Target::Fifth;
        check(fifth.withStyle(MatchOptions::Style::Moving).withStyle(MatchOptions::Style::Locked).target == core::Target::Fifth,
              "a note picked before Subtle or Moving is the one Locked goes back to");
    }

    // Moving a hand point, and nothing else, must reach the match and the stamp plan.
    {
        const auto planBefore = hub.planGeneration();
        auto moved = hub.options();
        moved.edits[0].semitones = 1.0;
        hub.setOptions(moved);
        check(hub.waitUntilIdle(10.0), "matched again");
        HubSnapshot h2;
        hub.snapshot(h2);
        check(std::abs(h2.match.curve[at] - h2.match.curve.front() - 1.0f) < 0.1f, "the curve follows the moved point");
        check(hub.planGeneration() != planBefore, "and a new stamp plan is published");
        moved.edits[0].semitones = -0.5;
        hub.setOptions(moved);
        check(hub.waitUntilIdle(10.0), "and back");
    }

    // Saved state carries the options but not the hand points: those belong to the effect learned
    // now, and whatever is drawn on it stays when a state is restored over it.
    juce::MemoryBlock saved;
    rig.processor->getStateInformation(saved);
    auto changed = hub.options();
    changed.bend = 0.0;
    changed.edits = { { 1.0, 0.25 } };
    hub.setOptions(changed);
    rig.processor->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    const auto restored = hub.options();
    check(std::abs(restored.bend - 1.0) < 1.0e-9, "the bend comes back from saved state");
    check(restored.edits.size() == 1 && std::abs(restored.edits[0].seconds - 1.0) < 1.0e-9 && std::abs(restored.edits[0].semitones - 0.25) < 1.0e-9,
          "hand points are not part of saved state: the ones drawn now stay");

    // State that is not a number does not get in.
    {
        juce::ValueTree bad("TonalRecallOptions");
        bad.setProperty("bend", "nan", nullptr);
        bad.setProperty("glide", "inf", nullptr);
        bad.setProperty("octave", 99, nullptr);
        juce::MemoryBlock block;
        if (auto xml = bad.createXml())
            juce::AudioProcessor::copyXmlToBinary(*xml, block);
        rig.processor->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        const auto got = hub.options();
        check(std::isfinite(got.bend) && std::abs(got.bend) <= 1.0 && std::isfinite(got.glide) && got.glide >= 0.0 && got.glide <= 1.0 && got.octaveShift == 2,
              "a state full of nonsense is brought back into range");
        hub.setOptions(restored);
    }

    // Learning another effect drops them: they were made on the old one's timeline.
    // (A different sound at a different place: the same audio over the same stretch of timeline
    // would merge into an identical capture, which is rightly not a new effect.)
    learn(rig, Slot::Sfx, dMinorPad(), 4800000);
    check(hub.waitUntilIdle(60.0), "analysed again");
    check(hub.options().edits.empty(), "a newly learned effect starts with no hand points");
}

void testAudition()
{
    beginCase("audition: an edit is heard over Preview, with no click going in or coming out, and never in a Render");
    auto& bus = AuditionBus::instance();
    auto& hub = MatchHub::instance();
    Rig rig(2, 48000.0, 1024);
    auto o = hub.options();
    o.stampEnabled = false;   // the Preview stream is the dry input: easy to tell from the audition
    hub.setOptions(o);

    const auto tone = effectTone();
    // A stereo effect: the tone on the left and the same, quieter, on the right, so the two can be told apart.
    auto channels = std::make_shared<AuditionBus::Channels>(2, tone);
    for (auto& v : (*channels)[1])
        v *= 0.25f;
    bus.setSource(channels, 48000.0, false);
    bus.setCurve(std::vector<float>(400, 0.0f), 0.005);
    check(! bus.previewRunning(), "no Preview yet");

    // Preview: one second of silence coming through.
    const Stereo silence { std::vector<float>(48000, 0.0f), std::vector<float>(48000, 0.0f) };
    auto out = playStereo(rig, silence, 1024, 0);
    check(bus.previewRunning(), "Preview is seen running");
    check(*std::max_element(out.l.begin(), out.l.end()) == 0.0f, "nothing sounds until asked");

    bus.hold(0.5, 3.0);
    out = playStereo(rig, silence, 1024, 48000);
    float peak = 0.0f, worstStep = 0.0f;
    for (std::size_t i = 0; i < out.l.size(); ++i)
    {
        peak = std::max(peak, std::abs(out.l[i]));
        if (i > 0)
            worstStep = std::max(worstStep, std::abs(out.l[i] - out.l[i - 1]));
    }
    std::printf("    -> held: peak %.3f, first sample %.5f, largest step between samples %.4f\n", static_cast<double>(peak),
                static_cast<double>(std::abs(out.l[0])), static_cast<double>(worstStep));
    check(peak > 0.02f, "the held sound is heard");
    {
        float left = 0.0f, right = 0.0f;
        for (std::size_t i = 24000; i < out.l.size(); ++i)
        {
            left = std::max(left, std::abs(out.l[i]));
            right = std::max(right, std::abs(out.r[i]));
        }
        std::printf("    -> held: left peak %.3f, right peak %.3f\n", static_cast<double>(left), static_cast<double>(right));
        check(right > 0.0f && std::abs(right / left - 0.25f) < 0.08f, "stereo stays stereo: the right channel is the quieter one");
        // The held note is the effect's own pitch plus the three semitones asked for.
        hindsight::Settings settings;
        settings.method = hindsight::Method::Tonal;
        const auto heard = hindsight::analyse(out.l.data() + 24000, 24000, 48000.0, settings);
        const auto original = hindsight::analyse(tone.data(), static_cast<int>(std::min<std::size_t>(tone.size(), 48000)), 48000.0, settings);
        const double shift = 12.0 * std::log2(static_cast<double>(heard.summary.logMeanHz) / static_cast<double>(original.summary.logMeanHz));
        std::printf("    -> held pitch is %+.2f st from the effect's own\n", shift);
        check(std::abs(shift - 3.0) < 0.25, "held three semitones up");
    }
    check(std::abs(out.l[0]) < 1.0e-3f && std::abs(out.l[32]) < 0.01f, "it starts from silence");
    check(worstStep < 0.12f, "and nowhere jumps (no click)");
    check(bus.positionSeconds() >= 0.0, "a play position is reported");

    // Let go: it plays on from there, then stops by itself at the end of the effect.
    bus.playFrom(0.5);
    out = playStereo(rig, silence, 1024, 96000);
    worstStep = 0.0f;
    for (std::size_t i = 1; i < out.l.size(); ++i)
        worstStep = std::max(worstStep, std::abs(out.l[i] - out.l[i - 1]));
    check(worstStep < 0.12f, "switching from held to playing on does not click");

    bus.stop();
    out = playStereo(rig, silence, 1024, 144000);
    check(std::abs(out.l[2400]) < 1.0e-3f && *std::max_element(out.l.begin() + 4800, out.l.end()) == 0.0f, "stopping fades out to nothing");
    check(bus.positionSeconds() < 0.0, "and the play position goes away");

    // A Render never carries an audition.
    bus.hold(0.5, 3.0);
    rig.stall();
    rig.processor->setNonRealtime(true);
    out = playStereo(rig, silence, 1024, 0);
    rig.processor->setNonRealtime(false);
    check(*std::max_element(out.l.begin(), out.l.end()) == 0.0f, "a Render is untouched by an audition");
    bus.stop();

    step("audition: a mono effect sounds on both channels of a stereo Preview, and a mono Preview takes a stereo effect");
    {
        bus.setSource(std::make_shared<AuditionBus::Channels>(1, tone), 48000.0, false);
        bus.hold(0.5, 0.0);
        rig.stall();
        out = playStereo(rig, silence, 1024, 0);
        float left = 0.0f, difference = 0.0f;
        for (std::size_t i = 0; i < out.l.size(); ++i)
        {
            left = std::max(left, std::abs(out.l[i]));
            difference = std::max(difference, std::abs(out.l[i] - out.r[i]));
        }
        check(left > 0.02f && difference == 0.0f, "mono effect: the same on left and right");
        bus.stop();
        playStereo(rig, silence, 1024, 48000);

        Rig mono(1, 48000.0, 1024);
        bus.setSource(channels, 48000.0, false);   // the stereo effect again
        bus.hold(0.5, 0.0);
        juce::AudioBuffer<float> buffer(1, 1024);
        juce::MidiBuffer midi;
        float heard = 0.0f;
        for (int block = 0; block < 40; ++block)
        {
            buffer.clear();
            mono.processor->processBlock(buffer, midi);
            heard = std::max(heard, buffer.getMagnitude(0, 0, 1024));
            advanceMs(1000.0 * 1024 / 48000.0);
        }
        check(heard > 0.02f, "a mono Preview hears the stereo effect");
        bus.stop();
    }

    step("audition: a looping Preview picks up a curve edit without restarting");
    {
        hub.resetForTesting();
        CaptureStore::instance().resetForTesting();
        Rig live(2, 48000.0, 1024);
        live.processor->useLoopPointsAsSelection(true);
        learn(live, Slot::Music, dMinorPad(), 480000);
        learn(live, Slot::Sfx, tone, 480000);
        check(hub.waitUntilIdle(60.0), "analysed");
        live.stall();

        const auto stream = stereoOf(tone);
        playStereo(live, stream, 1024, 480000);   // Preview, Stamp on
        const auto before = live.processor->lastStampRun();
        check(before.mode == StampRunInfo::Mode::Stamp && ! before.nonRealtime, "Preview is stamping");

        auto edited = hub.options();
        edited.edits = { { 0.5, 1.0 } };
        hub.setOptions(edited);
        check(hub.waitUntilIdle(10.0), "matched");
        playStereo(live, stream, 1024, 480000 + static_cast<std::int64_t>(tone.size()));   // the loop carries on
        const auto after = live.processor->lastStampRun();
        check(after.runIndex == before.runIndex, "the same run: Preview did not restart");
        check(after.planGeneration != before.planGeneration && after.planGeneration == hub.planGeneration(), "but it is on the new curve");
        check(after.bendActive, "which now moves");
    }
}

// One or two channels through a rig of that width, block by block.
std::vector<std::vector<float>> playChannels(Rig& rig, const std::vector<std::vector<float>>& in, int block, std::int64_t start)
{
    const int width = static_cast<int>(in.size());
    std::vector<std::vector<float>> out(in.size(), std::vector<float>(in[0].size(), 0.0f));
    juce::AudioBuffer<float> buffer(width, block);
    juce::MidiBuffer midi;
    for (std::size_t at = 0; at < in[0].size(); at += static_cast<std::size_t>(block))
    {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), in[0].size() - at));
        buffer.setSize(width, n, false, false, true);
        for (int c = 0; c < width; ++c)
            std::copy(in[static_cast<std::size_t>(c)].begin() + static_cast<std::ptrdiff_t>(at),
                      in[static_cast<std::size_t>(c)].begin() + static_cast<std::ptrdiff_t>(at) + n, buffer.getWritePointer(c));
        if (! rig.head.frozen)
            rig.head.time = start + static_cast<std::int64_t>(at);
        rig.processor->processBlock(buffer, midi);
        for (int c = 0; c < width; ++c)
            std::copy(buffer.getReadPointer(c), buffer.getReadPointer(c) + n, out[static_cast<std::size_t>(c)].begin() + static_cast<std::ptrdiff_t>(at));
        advanceMs(1000.0 * n / rig.sampleRate);
    }
    return out;
}

void learnChannels(Rig& rig, Slot slot, std::vector<std::vector<float>> channels, std::int64_t timelineStart)
{
    rig.head.frozen = false;
    rig.head.selectionStart = timelineStart;
    rig.head.selectionLength = static_cast<std::int64_t>(channels[0].size());
    CaptureStore::instance().arm(slot);
    for (auto& c : channels)
        c.resize(c.size() + 2048, 0.0f);   // Preview keeps going past the end of the selection
    playChannels(rig, channels, 1024, timelineStart);
    rig.stall();
}

// A Render of [2 s of other audio][the effect][2 s of other audio] through a rig, with the
// reported latency taken off the front of what comes back.
std::vector<std::vector<float>> renderChannels(Rig& rig, const std::vector<std::vector<float>>& effect)
{
    const std::size_t handle = 96000;
    const auto latency = static_cast<std::size_t>(rig.processor->getReportedLatency());
    std::vector<std::vector<float>> stream;
    for (const auto& channel : effect)
    {
        std::vector<float> s(handle + channel.size() + handle + latency, 0.0f);
        addNote(s, 52.0, 0.2, 6, 0, handle);
        addNote(s, 40.0, 0.2, 6, handle + channel.size(), handle + channel.size() + handle);
        std::copy(channel.begin(), channel.end(), s.begin() + static_cast<std::ptrdiff_t>(handle));
        stream.push_back(std::move(s));
    }
    rig.stall();
    rig.processor->setNonRealtime(true);
    rig.head.frozen = true;
    rig.head.time = 0;
    rig.head.selectionStart = 0;
    rig.head.selectionLength = static_cast<std::int64_t>(effect[0].size());
    auto out = playChannels(rig, stream, 1024, 0);
    rig.stall();
    rig.processor->setNonRealtime(false);
    for (auto& channel : out)
        channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(latency));
    return out;
}

double pitchOf(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    hindsight::Settings settings;
    settings.method = hindsight::Method::Tonal;
    const auto a = hindsight::analyse(x.data() + from, static_cast<int>(to - from), 48000.0, settings);
    return a.summary.logMeanHz > 0.0f ? core::hzToMidi(a.summary.logMeanHz) : 0.0;
}

void testMixedWidths()
{
    auto& hub = MatchHub::instance();
    const auto pad = dMinorPad();
    const auto tone = effectTone();
    const std::size_t from = 96000 + 4800, to = 96000 + tone.size() - 4800;

    beginCase("widths: stereo music, mono effect");
    {
        Rig stereo(2, 48000.0, 1024), mono(1, 48000.0, 1024);
        stereo.processor->useLoopPointsAsSelection(true);
        mono.processor->useLoopPointsAsSelection(true);
        auto right = pad;
        for (auto& v : right)
            v *= 0.6f;
        learnChannels(stereo, Slot::Music, { pad, right }, 480000);
        learnChannels(mono, Slot::Sfx, { tone }, 480000);
        check(hub.waitUntilIdle(60.0), "analysed");
        HubSnapshot h;
        hub.snapshot(h);
        check(snap(Slot::Sfx).channels == 1, "the effect was learned as mono");
        check(h.music.valid && h.music.rootPitchClass == 2 && h.match.valid && std::abs(h.match.semitones - 2.62) < 0.08, "matched: about +2.62 st to D");

        const auto out = renderChannels(mono, { tone });
        const auto run = mono.processor->lastStampRun();
        check(out.size() == 1 && run.mode == StampRunInfo::Mode::Stamp && run.aligned && run.alignmentOffset == 96000, "the mono Render stamped, effect found");
        const double pitch = pitchOf(out[0], from, to);
        std::printf("    -> mono effect after stamp: %.2f (target %.2f)\n", pitch, h.match.targetMidi);
        check(std::abs(pitch - h.match.targetMidi) < 0.1, "on the target");
    }

    beginCase("widths: mono music, stereo effect");
    {
        Rig stereo(2, 48000.0, 1024), mono(1, 48000.0, 1024);
        stereo.processor->useLoopPointsAsSelection(true);
        mono.processor->useLoopPointsAsSelection(true);
        auto right = tone;
        for (auto& v : right)
            v *= 0.5f;
        learnChannels(mono, Slot::Music, { pad }, 480000);
        learnChannels(stereo, Slot::Sfx, { tone, right }, 480000);
        check(hub.waitUntilIdle(60.0), "analysed");
        HubSnapshot h;
        hub.snapshot(h);
        check(snap(Slot::Sfx).channels == 2, "the effect was learned as stereo");
        check(h.music.valid && h.music.rootPitchClass == 2 && h.match.valid && std::abs(h.match.semitones - 2.62) < 0.08, "matched: about +2.62 st to D");

        const auto out = renderChannels(stereo, { tone, right });
        const auto run = stereo.processor->lastStampRun();
        check(out.size() == 2 && run.mode == StampRunInfo::Mode::Stamp && run.aligned && run.alignmentOffset == 96000, "the stereo Render stamped, effect found");
        const double left = pitchOf(out[0], from, to), rightPitch = pitchOf(out[1], from, to);
        float peakL = 0.0f, peakR = 0.0f;
        for (std::size_t i = from; i < to; ++i)
        {
            peakL = std::max(peakL, std::abs(out[0][i]));
            peakR = std::max(peakR, std::abs(out[1][i]));
        }
        std::printf("    -> stereo effect after stamp: left %.2f, right %.2f (target %.2f); right / left level %.2f\n", left, rightPitch,
                    h.match.targetMidi, static_cast<double>(peakR / peakL));
        check(std::abs(left - h.match.targetMidi) < 0.1 && std::abs(rightPitch - h.match.targetMidi) < 0.1, "both channels on the target");
        check(std::abs(peakR / peakL - 0.5f) < 0.05f, "and the stereo balance is kept");
    }
}

// A real pair, when one is pointed at: TONALRECALL_EXAMPLE_DIR holding music.wav (or strings.wav)
// and before.wav (24-bit PCM). Learns both, prints what the plug-in makes of them, and draws the
// editor. Not part of the pass/fail count unless the folder is given.
std::vector<std::vector<float>> loadWav24(const juce::File& file, double& rate)
{
    juce::MemoryBlock block;
    std::vector<std::vector<float>> out;
    if (! file.loadFileAsData(block))
        return out;
    const auto* d = static_cast<const unsigned char*>(block.getData());
    const std::size_t total = block.getSize();
    std::size_t at = 12;
    int channels = 0, bits = 0;
    while (at + 8 <= total)
    {
        const std::size_t size = static_cast<std::size_t>(d[at + 4]) | (static_cast<std::size_t>(d[at + 5]) << 8) | (static_cast<std::size_t>(d[at + 6]) << 16) | (static_cast<std::size_t>(d[at + 7]) << 24), body = at + 8;
        if (std::memcmp(d + at, "fmt ", 4) == 0)
        {
            channels = d[body + 2];
            rate = d[body + 4] | (d[body + 5] << 8) | (d[body + 6] << 16);
            bits = d[body + 14];
        }
        else if (std::memcmp(d + at, "data", 4) == 0 && channels > 0 && bits == 24)
        {
            const std::size_t frames = std::min(size, total - body) / (3 * static_cast<std::size_t>(channels));
            out.assign(static_cast<std::size_t>(std::min(channels, 2)), std::vector<float>(frames));
            for (std::size_t i = 0; i < frames; ++i)
                for (std::size_t c = 0; c < out.size(); ++c)
                {
                    const unsigned char* q = d + body + (i * static_cast<std::size_t>(channels) + c) * 3;
                    out[c][i] = static_cast<float>((static_cast<std::int32_t>(q[0] | (q[1] << 8) | (q[2] << 16)) << 8) >> 8) / 8388608.0f;
                }
            return out;
        }
        at = body + size + (size & 1);
    }
    return out;
}

void testRealPair(const juce::File& folder, const juce::File& pngFolder)
{
    beginCase("a real pair from TONALRECALL_EXAMPLE_DIR");
    auto& hub = MatchHub::instance();
    auto o = hub.options();
    o.target = core::Target::Nearest;   // as shipped
    hub.setOptions(o);

    double musicRate = 0.0, effectRate = 0.0;
    auto musicFile = folder.getChildFile("music.wav");
    if (! musicFile.existsAsFile())
        musicFile = folder.getChildFile("strings.wav");
    const auto music = loadWav24(musicFile, musicRate);
    const auto effect = loadWav24(folder.getChildFile("before.wav"), effectRate);
    check(! music.empty() && ! effect.empty(), "the files were read");
    if (music.empty() || effect.empty())
        return;

    Rig musicRig(static_cast<int>(music.size()), musicRate, 1024), effectRig(static_cast<int>(effect.size()), effectRate, 1024);
    musicRig.processor->useLoopPointsAsSelection(true);
    effectRig.processor->useLoopPointsAsSelection(true);
    learnChannels(musicRig, Slot::Music, music, 480000);
    learnChannels(effectRig, Slot::Sfx, effect, 480000);
    check(hub.waitUntilIdle(120.0), "analysed");
    HubSnapshot h;
    hub.snapshot(h);
    std::printf("    -> music %s (lock %.0f%%), root %s; effect %s, drift %.1f st; shift %+.2f st to %s\n", h.music.keyName().c_str(),
                static_cast<double>(h.music.keyConfidence) * 100.0, core::noteName(h.music.rootMidi).c_str(), core::noteName(h.sfx.centreMidi).c_str(),
                static_cast<double>(h.sfx.driftSemitones), h.match.semitones, core::noteName(h.match.targetMidi).c_str());
    std::unique_ptr<juce::AudioProcessorEditor> e(effectRig.processor->createEditor());
    check(writePng(e, pngFolder, "editor-real.png"), "write the editor for the real pair");

    // The same window as Pro Tools shows it (no line about other hosts), for the manual's pictures.
    e.reset();
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_AAX);
    auto proTools = std::make_unique<TonalRecallProcessor>();
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
    e.reset(proTools->createEditor());
    check(writePng(e, pngFolder, "editor-real-protools.png"), "write the editor as Pro Tools shows it");
    e.reset();
}

//==============================================================================
// The play buttons: the music and the effect as learned, and the effect as it will be stamped,
// through Preview.

// Plays a stereo stream through the rig as Preview does, calling after(first sample, count) after
// each block.
Stereo playWatching(Rig& rig, const Stereo& in, int block, std::int64_t start, const std::function<void(std::size_t, int)>& after)
{
    Stereo out { std::vector<float>(in.l.size(), 0.0f), std::vector<float>(in.r.size(), 0.0f) };
    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;
    for (std::size_t at = 0; at < in.l.size(); at += static_cast<std::size_t>(block))
    {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), in.l.size() - at));
        buffer.setSize(2, n, false, false, true);
        std::copy(in.l.begin() + static_cast<std::ptrdiff_t>(at), in.l.begin() + static_cast<std::ptrdiff_t>(at) + n, buffer.getWritePointer(0));
        std::copy(in.r.begin() + static_cast<std::ptrdiff_t>(at), in.r.begin() + static_cast<std::ptrdiff_t>(at) + n, buffer.getWritePointer(1));
        if (! rig.head.frozen)
            rig.head.time = start + static_cast<std::int64_t>(at);
        rig.processor->processBlock(buffer, midi);
        std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + n, out.l.begin() + static_cast<std::ptrdiff_t>(at));
        std::copy(buffer.getReadPointer(1), buffer.getReadPointer(1) + n, out.r.begin() + static_cast<std::ptrdiff_t>(at));
        advanceMs(1000.0 * n / rig.sampleRate);
        if (after)
            after(at, n);
    }
    return out;
}

float largestStep(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    float worst = 0.0f;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < to && i < x.size(); ++i)
        worst = std::max(worst, std::abs(x[i] - x[i - 1]));
    return worst;
}

// What Preview is playing underneath: a quiet hum, different on each side, so that what is played
// over it can be told from it.
Stereo previewBed(std::size_t n)
{
    Stereo s { std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f) };
    for (std::size_t i = 0; i < n; ++i)
    {
        s.l[i] = static_cast<float>(0.05 * std::sin(kTwoPi * 97.0 * static_cast<double>(i) / 48000.0));
        s.r[i] = static_cast<float>(0.04 * std::sin(kTwoPi * 131.0 * static_cast<double>(i) / 48000.0));
    }
    return s;
}

// Every sample of out in [from, to) is exactly src from srcFrom on, both sides.
bool exactly(const Stereo& out, std::size_t from, std::size_t to, const std::vector<float>& l, const std::vector<float>& r, std::size_t srcFrom)
{
    if (to <= from)
        return false;
    return sameBitsRange(out.l, from, l, srcFrom, to - from) && sameBitsRange(out.r, from, r, srcFrom, to - from);
}

// The block a sample fell in, and the playhead reported after it.
struct Report
{
    std::size_t end = 0;     // one past the block's last sample
    AuditionBus::Playhead head;
};

// The reported playhead against the moment a marker in the clip is heard: the marker (at
// markSeconds in the clip) comes out at the loudest sample of out in [from, to); the report after
// that block should be markSeconds plus the time from the marker to the block's end. Returns the
// difference in seconds (a large number if there is no report for it).
double playheadError(const std::vector<float>& out, std::size_t from, std::size_t to, const std::vector<Report>& reports, double markSeconds)
{
    std::size_t peak = from;
    for (std::size_t i = from; i < to && i < out.size(); ++i)
        if (std::abs(out[i]) > std::abs(out[peak]))
            peak = i;
    for (const auto& r : reports)
        if (r.end > peak)
            return r.head.seconds - (markSeconds + static_cast<double>(r.end - 1 - peak) / 48000.0);
    return 1.0e9;
}

// A short decaying burst: a marker that survives being shifted.
void addMarker(std::vector<float>& x, std::size_t at, float amp)
{
    for (std::size_t i = 0; i < 96 && at + i < x.size(); ++i)
        x[at + i] += amp * std::exp(-static_cast<float>(i) / 12.0f) * (i % 2 == 0 ? 1.0f : -1.0f);
}

void testPlayback(const juce::File& pngFolder)
{
    beginCase("play: the music is learned in stereo, and analysed from the same sum as before");
    auto& bus = AuditionBus::instance();
    auto& hub = MatchHub::instance();
    auto& store = CaptureStore::instance();
    Rig rig(2, 48000.0, 1024);
    rig.processor->useLoopPointsAsSelection(true);
    constexpr std::size_t fade = 240;   // 5 ms at 48 kHz

    const auto pad = dMinorPad();
    // The right side differs from the left: the pad's upper notes only.
    std::vector<float> padRight(pad.size(), 0.0f);
    for (double midi : { 50.0, 53.0, 57.0 })
        addNote(padRight, midi, 0.12, 6, 0, padRight.size());
    learnChannels(rig, Slot::Music, { pad, padRight }, 480000);
    {
        std::vector<std::vector<float>> kept;
        store.copyChannels(Slot::Music, kept);
        check(snap(Slot::Music).channels == 2 && kept.size() == 2 && kept[0] == pad && kept[1] == padRight,
              "the music keeps both its channels, sample for sample");
        const auto sum = audio(Slot::Music);
        bool same = sum.size() == pad.size();
        for (std::size_t i = 0; same && i < sum.size(); ++i)
            same = sameBits(sum[i], 0.0f + pad[i] + padRight[i]);
        check(same, "and the sum the analysis runs on is what it was");
    }
    const auto tone = effectTone();
    auto toneRight = tone;
    for (auto& v : toneRight)
        v *= 0.5f;
    learnChannels(rig, Slot::Sfx, { tone, toneRight }, 960000);
    check(hub.waitUntilIdle(60.0), "analysed");
    HubSnapshot h;
    hub.snapshot(h);
    {
        const auto sum = audio(Slot::Music);
        const auto direct = core::analyseMusic(sum.data(), static_cast<int>(sum.size()), 48000.0);
        check(h.music.valid && direct.keyName() == h.music.keyName() && direct.rootPitchClass == h.music.rootPitchClass
                  && sameBits(direct.tuningCents, h.music.tuningCents),
              "the music's analysis is the analysis of its sum");
    }
    check(h.musicChannels != nullptr && h.musicChannels->size() == 2 && h.sfxChannels != nullptr && h.sfxChannels->size() == 2,
          "both sides are held channel by channel for playing back");
    check(h.match.valid && h.planReady, "matched");
    const double musicLength = static_cast<double>(pad.size()) / 48000.0;
    const double effectLength = static_cast<double>(tone.size()) / 48000.0;

    // Stamp off: what Preview carries underneath is the dry input, so anything played over it can
    // be checked sample for sample.
    {
        auto o = hub.options();
        o.stampEnabled = false;
        hub.setOptions(o);
    }

    step("play: pressed with Preview stopped, the music waits for it and starts with the stream");
    rig.stall();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(! bus.previewRunning(), "Preview has stopped");
    std::unique_ptr<juce::AudioProcessorEditor> e(rig.processor->createEditor());
    auto* tr = dynamic_cast<TonalRecallEditor*>(e.get());
    check(tr != nullptr, "editor");
    if (tr == nullptr)
        return;
    tr->togglePlayForTesting(Playback::Music);
    check(bus.playing() == Playback::Music && bus.startedByPlay(), "the music is waiting to play");
    tr->refreshNow();
    check(anyWarningStartsWith(tr->currentWarnings(), "PRESS PREVIEW (THE SPEAKER, BOTTOM LEFT)")
              || anyWarningStartsWith(tr->currentWarnings(), "PLAY THE TRACK IN THE HOST"),
          "the strip says what to press: " + tr->currentWarnings().joinIntoString(" | ").toStdString());
    check(writePng(e, pngFolder, "editor-play-armed.png"), "write the editor waiting for Preview");
    {
        const auto bed = previewBed(pad.size() + 9600);
        std::vector<double> heads;
        bool monotonic = true, shot = false;
        const auto out = playWatching(rig, bed, 1024, 0, [&](std::size_t, int)
        {
            const auto p = bus.playhead();
            if (p.what != Playback::Music)
                return;
            if (! heads.empty() && p.seconds < heads.back())
                monotonic = false;
            heads.push_back(p.seconds);
            if (! shot && p.seconds > 0.45 * musicLength)
            {
                check(writePng(e, pngFolder, "editor-play-music.png"), "write the editor playing the music");
                shot = true;
            }
        });
        check(std::abs(out.l[0] - bed.l[0]) < 1.0e-6f && std::abs(out.r[0] - bed.r[0]) < 1.0e-6f, "it starts from the stream, with no step");
        check(largestStep(out.l, 0, 2 * fade) < 0.08f, "and fades in without a click");
        check(exactly(out, fade, pad.size() - fade, pad, padRight, fade), "then it is the music as learned, sample for sample, both sides");
        check(exactly(out, pad.size(), out.l.size(), bed.l, bed.r, pad.size()), "after its end the stream is back, untouched");
        check(! heads.empty() && heads.front() > 0.0 && monotonic, "the playhead only moves forwards");
        check(! heads.empty() && std::abs(heads.back() - musicLength) < 1.0e-9,
              "and ends at the music's length: " + std::to_string(heads.empty() ? -1.0 : heads.back()));
        check(bus.playing() == Playback::None, "and it has stopped by itself");
    }

    step("play: the effect as learned, started while the music plays: one hands over to the other");
    {
        const auto bed = previewBed(24576 + tone.size() + 9600);
        tr->togglePlayForTesting(Playback::Music);
        const Stereo first { std::vector<float>(bed.l.begin(), bed.l.begin() + 24576), std::vector<float>(bed.r.begin(), bed.r.begin() + 24576) };
        const auto a = playWatching(rig, first, 1024, 0, nullptr);
        check(exactly(a, fade, a.l.size(), pad, padRight, fade), "the music is playing");
        tr->togglePlayForTesting(Playback::Input);
        check(bus.playing() == Playback::Input, "the effect takes over");
        const Stereo second { std::vector<float>(bed.l.begin() + 24576, bed.l.end()), std::vector<float>(bed.r.begin() + 24576, bed.r.end()) };
        bool shot = false;
        std::vector<double> heads;
        const auto b = playWatching(rig, second, 1024, 24576, [&](std::size_t, int)
        {
            const auto p = bus.playhead();
            if (p.what == Playback::Input)
                heads.push_back(p.seconds);
            if (! shot && p.what == Playback::Input && p.seconds > 0.5 * effectLength)
            {
                check(writePng(e, pngFolder, "editor-play-input.png"), "write the editor playing the effect as learned");
                shot = true;
            }
        });
        // The worst step anywhere in what was played, or in the stream, is the most a hand-over
        // may make.
        const float natural = std::max({ largestStep(pad, 0, pad.size()), largestStep(tone, 0, tone.size()), largestStep(bed.l, 0, bed.l.size()) });
        const float across = std::max(std::abs(b.l[0] - a.l.back()), largestStep(b.l, 0, 2 * fade));
        std::printf("    -> hand-over: largest step %.4f (the material's own largest %.4f)\n", static_cast<double>(across), static_cast<double>(natural));
        check(across <= natural * 1.25f + 0.002f, "the hand-over makes no jump");
        check(exactly(b, fade + 1, tone.size() - fade, tone, toneRight, fade + 1), "then it is the effect as learned, sample for sample, both sides");
        check(exactly(b, tone.size(), b.l.size(), bed.l, bed.r, 24576 + tone.size()), "and the stream comes back after it");
        check(! heads.empty() && std::abs(heads.back() - effectLength) < 1.0e-9, "its playhead ends at the effect's length");
    }

    step("play: a second click stops it, fading out");
    {
        const auto bed = previewBed(8192);
        tr->togglePlayForTesting(Playback::Music);
        playWatching(rig, Stereo { std::vector<float>(bed.l.begin(), bed.l.begin() + 4096), std::vector<float>(bed.r.begin(), bed.r.begin() + 4096) }, 1024, 0, nullptr);
        tr->togglePlayForTesting(Playback::Music);
        check(bus.playing() == Playback::None, "stopped");
        const auto out = playWatching(rig, Stereo { std::vector<float>(bed.l.begin() + 4096, bed.l.end()), std::vector<float>(bed.r.begin() + 4096, bed.r.end()) },
                                      1024, 4096, nullptr);
        check(largestStep(out.l, 0, 2 * fade) < 0.08f, "with no click");
        check(exactly(out, fade + 1, out.l.size(), bed.l, bed.r, 4096 + fade + 1), "and the stream is back untouched");
    }

    step("play: the effect as it will be stamped");
    {
        const auto bed = previewBed(tone.size() + 24000);
        tr->togglePlayForTesting(Playback::Output);
        check(bus.playing() == Playback::Output, "playing the effect as stamped");
        std::vector<double> heads;
        bool monotonic = true, shot = false;
        const auto out = playWatching(rig, bed, 1024, 0, [&](std::size_t, int)
        {
            const auto p = bus.playhead();
            if (p.what != Playback::Output)
                return;
            if (! heads.empty() && p.seconds < heads.back())
                monotonic = false;
            heads.push_back(p.seconds);
            if (! shot && p.seconds > 0.5 * effectLength)
            {
                check(writePng(e, pngFolder, "editor-play-output.png"), "write the editor playing the effect as stamped");
                shot = true;
            }
        });
        const double midi = medianMidi(out.l, 9600, tone.size() - 9600);
        std::printf("    -> output at %.3f, the target %.3f\n", midi, h.match.targetMidi);
        check(std::abs(midi - h.match.targetMidi) < 0.15, "it is the effect on the target note");
        float left = 0.0f, right = 0.0f;
        for (std::size_t i = 9600; i < tone.size() - 9600; ++i)
        {
            left = std::max(left, std::abs(out.l[i]));
            right = std::max(right, std::abs(out.r[i]));
        }
        check(left > 0.05f && std::abs(right / left - 0.5f) < 0.05f, "in stereo: the right side is the quieter one, as learned");
        check(monotonic && ! heads.empty() && std::abs(heads.back() - effectLength) < 1.0e-9,
              "the playhead moves forwards and ends at the effect's length: " + std::to_string(heads.empty() ? -1.0 : heads.back()));
        check(bus.playing() == Playback::None, "it stops by itself at the end");
        check(exactly(out, out.l.size() - 4800, out.l.size(), bed.l, bed.r, out.l.size() - 4800), "and the stream is back after it");
    }

    step("play: a Render carries none of it");
    {
        tr->togglePlayForTesting(Playback::Music);
        const int latency = rig.processor->getReportedLatency();
        const auto stream = renderStream(tone, latency, true);
        const auto out = render(rig, stream, static_cast<std::int64_t>(tone.size()));
        const auto n = stream.l.size() - static_cast<std::size_t>(latency);
        check(allZero(out.l, 0, static_cast<std::size_t>(latency))
                  && sameBitsRange(out.l, static_cast<std::size_t>(latency), stream.l, 0, n) && sameBitsRange(out.r, static_cast<std::size_t>(latency), stream.r, 0, n),
              "the Render is the dry input, delayed, bit for bit");
        rig.processor->setNonRealtime(false);
        rig.head.frozen = false;
        tr->togglePlayForTesting(Playback::Music);   // (it was waiting for Preview)
        check(bus.playing() == Playback::None, "and the wait is called off");
    }

    step("play: nothing is mixed in while Learn listens, and it learns what was played");
    {
        std::vector<std::vector<float>> before;
        store.copyChannels(Slot::Sfx, before);
        tr->togglePlayForTesting(Playback::Music);
        const auto bed = previewBed(4096);
        const auto sounding = playWatching(rig, bed, 1024, 0, nullptr);
        check(! sameBitsRange(sounding.l, 2048, bed.l, 2048, 1024), "the music is sounding");
        // Armed behind the editor's back: the processor alone keeps Learn's stream clean.
        store.arm(Slot::Sfx);
        rig.head.selectionStart = 960000;
        rig.head.selectionLength = static_cast<std::int64_t>(tone.size());
        Stereo effect { tone, toneRight };
        effect.l.resize(tone.size() + 2048, 0.0f);
        effect.r.resize(tone.size() + 2048, 0.0f);
        const auto heard = playWatching(rig, effect, 1024, 960000, nullptr);
        check(heard.l == effect.l && heard.r == effect.r, "Preview passes through untouched while Learn listens");
        std::vector<std::vector<float>> after;
        store.copyChannels(Slot::Sfx, after);
        check(! store.isArmed(Slot::Sfx) && after == before && after.size() == 2 && after[0] == tone, "and the effect learned is the effect played, sample for sample");
        const auto later = playWatching(rig, bed, 1024, 0, nullptr);
        check(later.l == bed.l && later.r == bed.r && bus.playing() == Playback::None, "what was playing does not come back afterwards");
    }
    e.reset();
    check(TonalRecallEditor::textOverflowCount() == 0, "every text fits its box: " + std::to_string(TonalRecallEditor::textOverflowCount()) + " did not");

    step("play: a curve change while the effect plays as stamped is heard without a restart");
    {
        // A steady A3, shifted along a curve that is changed half-way through.
        std::vector<float> steady(3 * 48000, 0.0f);
        addNote(steady, 57.0, 0.3, 6, 0, steady.size());
        bus.setSource(std::make_shared<AuditionBus::Channels>(2, steady), 48000.0, false);
        bus.setCurve(std::vector<float>(700, 0.0f), 0.005);
        bus.play(Playback::Output);
        const auto bed = previewBed(48000);
        std::vector<double> heads;
        const auto watch = [&](std::size_t, int) { if (bus.playhead().what == Playback::Output) heads.push_back(bus.playhead().seconds); };
        const auto a = playWatching(rig, bed, 1024, 0, watch);
        bus.setCurve(std::vector<float>(700, 5.0f), 0.005);
        check(bus.playing() == Playback::Output, "still playing when the curve changes");
        const auto b = playWatching(rig, bed, 1024, 48000, watch);
        bool monotonic = ! heads.empty();
        for (std::size_t i = 1; i < heads.size(); ++i)
            monotonic = monotonic && heads[i] >= heads[i - 1];
        const double first = pitchOf(a.l, 14400, 43200), second = pitchOf(b.l, 19200, 45000);
        std::printf("    -> before the change %.2f, after %.2f\n", first, second);
        check(std::abs(first - 57.0) < 0.15 && std::abs(second - 62.0) < 0.15, "the new curve is heard");
        check(monotonic && heads.back() > 1.9, "and the effect played on from where it was");
        bus.stop();
        playWatching(rig, bed, 1024, 0, nullptr);
    }

    step("play: the playhead of the effect as stamped is where the sound heard is");
    for (const double shift : { 0.0, 5.0, -7.0 })
    {
        std::vector<float> marked(2 * 48000, 0.0f);
        addNote(marked, 57.0, 0.05, 4, 0, marked.size());
        addMarker(marked, 48000, 0.9f);
        bus.setSource(std::make_shared<AuditionBus::Channels>(2, marked), 48000.0, false);
        bus.setCurve(std::vector<float>(500, static_cast<float>(shift)), 0.005);
        bus.play(Playback::Output);
        std::vector<Report> reports;
        const auto out = playWatching(rig, previewBed(3 * 48000), 1024, 0, [&](std::size_t at, int n)
        {
            reports.push_back({ at + static_cast<std::size_t>(n), bus.playhead() });
        });
        const double error = playheadError(out.l, 24000, 72000, reports, 1.0);
        std::printf("    -> %+.0f st: the playhead is %+.2f ms from the sound\n", shift, 1000.0 * error);
        check(std::abs(error) < 0.004, "within a few milliseconds at " + std::to_string(shift) + " st");
    }

    beginCase("play: Preview's own stamp has a playhead too, and it comes round with the loop");
    {
        Rig live(2, 48000.0, 1024);
        live.processor->useLoopPointsAsSelection(true);
        learn(live, Slot::Music, dMinorPad(), 480000);
        // The tone kept quiet, so the marker stands well clear of it.
        auto marked = effectTone();
        for (auto& v : marked)
            v *= 0.3f;
        addMarker(marked, 48000, 0.8f);
        learn(live, Slot::Sfx, marked, 960000);
        check(hub.waitUntilIdle(60.0), "analysed");
        live.stall();
        live.head.frozen = false;
        live.head.selectionStart = 960000;
        live.head.selectionLength = static_cast<std::int64_t>(marked.size());
        Stereo loop = stereoOf(marked);
        Stereo twice { loop.l, loop.r };
        twice.l.insert(twice.l.end(), loop.l.begin(), loop.l.end());
        twice.r.insert(twice.r.end(), loop.r.begin(), loop.r.end());
        std::vector<Report> reports;
        bool shown = false;
        const auto out = playWatching(live, twice, 1024, 960000, [&](std::size_t at, int n)
        {
            const auto p = bus.stampPlayhead();
            shown = shown || p.what != Playback::None;
            reports.push_back({ at + static_cast<std::size_t>(n), p });
        });
        const auto run = live.processor->lastStampRun();
        check(run.mode == StampRunInfo::Mode::Stamp && run.aligned && run.alignmentOffset == 0, "Preview stamps the effect, found at its start");
        check(shown, "a playhead is reported");
        const auto latency = static_cast<std::size_t>(live.processor->getReportedLatency());
        const auto n = marked.size();
        // The marker comes out a latency late, in each time round.
        const double first = playheadError(out.l, latency + 24000, latency + 72000, reports, 1.0);
        const double second = playheadError(out.l, n + latency + 24000, n + latency + 72000, reports, 1.0);
        std::printf("    -> Preview's playhead: %+.2f ms from the sound, %+.2f ms the second time round\n", 1000.0 * first, 1000.0 * second);
        check(std::abs(first) < 0.004 && std::abs(second) < 0.004, "where the sound heard is, both times round");
        auto o = hub.options();
        o.stampEnabled = false;
        hub.setOptions(o);
        playWatching(live, loop, 1024, 960000, nullptr);
        check(bus.stampPlayhead().what == Playback::None, "and none with Stamp off");
    }

    beginCase("play: a mono clip plays on both sides; a stereo one into a mono Preview is summed at half");
    {
        Rig mono(1, 48000.0, 1024);
        mono.processor->useLoopPointsAsSelection(true);
        learnChannels(mono, Slot::Music, { pad }, 480000);
        check(hub.waitUntilIdle(60.0), "analysed");
        HubSnapshot m;
        hub.snapshot(m);
        check(snap(Slot::Music).channels == 1 && m.musicChannels != nullptr && m.musicChannels->size() == 1, "a mono clip is kept mono");
        bus.setMusic(m.musicChannels, m.musicRate);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        bus.play(Playback::Music);
        Rig stereo(2, 48000.0, 1024);
        const auto bed = previewBed(48000);
        const auto out = playWatching(stereo, bed, 1024, 0, nullptr);
        check(exactly(out, fade, out.l.size(), pad, pad, fade), "the same on both sides of a stereo Preview");
        bus.stop();
        playWatching(stereo, bed, 1024, 0, nullptr);

        bus.setMusic(std::make_shared<AuditionBus::Channels>(AuditionBus::Channels { pad, padRight }), 48000.0);
        bus.play(Playback::Music);
        std::vector<std::vector<float>> quiet(1, std::vector<float>(48000, 0.01f));
        const auto heard = playChannels(mono, quiet, 1024, 0);
        bool half = true;
        for (std::size_t i = fade; i < heard[0].size(); ++i)
            half = half && sameBits(heard[0][i], 0.5f * (pad[i] + padRight[i]));
        check(half, "a stereo clip into a mono Preview is the two sides summed at half");
        bus.stop();
    }
}

void testRunningPreview()
{
    // Pro Tools users leave Preview looping while they work. What the run should be doing changes
    // under it: Learn finishes, Stamp is switched, the effect is cleared. None of these helpers
    // stop the audio in between (no stall), as a looping Preview would not.
    beginCase("a Preview left running: Learn ends inside it and the stamp comes in without a restart");
    auto& hub = MatchHub::instance();
    auto& store = CaptureStore::instance();
    Rig rig(2, 48000.0, 1024);
    rig.processor->useLoopPointsAsSelection(true);
    const auto pad = dMinorPad();
    const auto tone = effectTone();
    learn(rig, Slot::Music, pad, 480000);
    check(hub.waitUntilIdle(60.0), "music analysed");

    // Learn the effect, and keep Preview looping straight on.
    rig.head.frozen = false;
    rig.head.selectionStart = 480000;
    rig.head.selectionLength = static_cast<std::int64_t>(tone.size());
    store.arm(Slot::Sfx);
    const auto stream = stereoOf(tone);
    playStereo(rig, stream, 1024, 480000);                       // the pass that is learned
    check(! store.isArmed(Slot::Sfx) && snap(Slot::Sfx).lastEnd == PassEnd::SelectionEnd, "Learn switched itself off at the end of the selection");
    // (The run stopped being a Learn pass in the very block Learn ended in.)
    check(rig.processor->lastStampRun().mode != StampRunInfo::Mode::Learn, "the run stopped being a Learn pass as Learn ended");
    check(hub.waitUntilIdle(60.0), "effect analysed");
    check(AuditionBus::instance().previewRunning(), "Preview is known to be running while Learn listens");

    playStereo(rig, stream, 1024, 480000 + static_cast<std::int64_t>(tone.size()));   // the loop carries on
    auto run = rig.processor->lastStampRun();
    check(run.mode == StampRunInfo::Mode::Stamp && run.stampOn, "the next time round is stamped, with no stop in between");

    step("a Preview left running: Stamp off and on, and arming Learn, take effect at once");
    {
        auto o = hub.options();
        o.stampEnabled = false;
        hub.setOptions(o);
        playStereo(rig, stream, 1024, 0);
        // (Off inside a Stamp run is an A/B: the run stays a Stamp run and plays the dry signal.)
        check(! rig.processor->lastStampRun().stampOn, "Stamp off: dry");
        o.stampEnabled = true;
        hub.setOptions(o);
        playStereo(rig, stream, 1024, 0);
        check(rig.processor->lastStampRun().stampOn, "Stamp on again: stamped");

        store.arm(Slot::Music);
        const Stereo block { std::vector<float>(2048, 0.1f), std::vector<float>(2048, 0.1f) };
        const auto out = playStereo(rig, block, 1024, 0);
        check(rig.processor->lastStampRun().mode == StampRunInfo::Mode::Learn, "arming Learn turns the run into a Learn pass");
        check(out.l == block.l && out.r == block.r, "which passes the audio through untouched");
        store.disarm(Slot::Music);
    }

    step("a Preview left running: clearing the effect stops the stamp");
    {
        playStereo(rig, stream, 1024, 0);
        check(rig.processor->lastStampRun().mode == StampRunInfo::Mode::Stamp, "stamping again once Learn is disarmed");
        store.clear(Slot::Sfx);
        check(hub.waitUntilIdle(10.0), "cleared");
        const Stereo block { std::vector<float>(4096, 0.1f), std::vector<float>(4096, 0.1f) };
        const auto out = playStereo(rig, block, 1024, 0);
        check(rig.processor->lastStampRun().mode == StampRunInfo::Mode::Dry, "nothing to stamp: the run goes dry");
        check(std::equal(out.l.begin() + 1024, out.l.end(), block.l.begin() + 1024), "and the audio comes through untouched");
    }

    beginCase("one Learn at a time: arming one side disarms the other");
    {
        Rig other(2, 48000.0, 1024);
        std::unique_ptr<juce::AudioProcessorEditor> e(other.processor->createEditor());
        auto* tr = dynamic_cast<TonalRecallEditor*>(e.get());
        check(tr != nullptr, "editor");
        if (tr != nullptr)
        {
            tr->toggleLearnForTesting(Slot::Music);
            check(store.isArmed(Slot::Music) && ! store.isArmed(Slot::Sfx), "music armed");
            tr->toggleLearnForTesting(Slot::Sfx);
            check(store.isArmed(Slot::Sfx) && ! store.isArmed(Slot::Music), "arming the effect disarmed the music");
            tr->toggleLearnForTesting(Slot::Sfx);
            check(! store.isArmed(Slot::Sfx), "and off again");
        }
    }
}

void testState()
{
    beginCase("the options are saved with the session and restored");
    auto& hub = MatchHub::instance();
    Rig rig;
    MatchOptions o;
    o.target = core::Target::PitchClass;
    o.pitchClass = 7;
    o.octaveShift = -1;
    o.bend = 0.35;
    o.method = core::SfxMethod::Engine;
    o.preserveFormants = true;
    o.stampEnabled = false;
    o.trimEnabled = false;
    hub.setOptions(o);

    juce::MemoryBlock state;
    rig.processor->getStateInformation(state);
    check(state.getSize() > 0, "state written");
    hub.setOptions({});
    check(hub.options() == MatchOptions {}, "options back to the defaults");
    rig.processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    check(hub.options() == o, "every option restored");

    Rig other;
    juce::MemoryBlock again;
    other.processor->getStateInformation(again);
    check(again == state, "a new instance shows (and saves) the same options");

    rig.processor->setStateInformation("garbage", 7);
    check(hub.options() == o, "unreadable state is ignored");

    {
        // A state without the TRIM switch has it on.
        juce::ValueTree older("TonalRecallOptions");
        older.setProperty("version", 1, nullptr);
        older.setProperty("target", "root", nullptr);
        older.setProperty("stamp", true, nullptr);
        juce::MemoryBlock block;
        if (auto xml = older.createXml())
            juce::AudioProcessor::copyXmlToBinary(*xml, block);
        rig.processor->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        check(hub.options().trimEnabled, "a state without TRIM loads with it on");
        rig.processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        check(! hub.options().trimEnabled, "and TRIM off is restored as off");
    }

    MatchOptions wild;
    wild.octaveShift = 9;
    wild.bend = -4.0;
    wild.pitchClass = 14;
    hub.setOptions(wild);
    const auto got = hub.options();
    check(got.octaveShift == 2 && std::abs(got.bend + 1.0) < 1.0e-12 && got.pitchClass == 2, "out-of-range options are clamped");
}

void testLog(const juce::File& logFile)
{
    beginCase("the log has the expected lines");
    DiagnosticLog::instance().flush();
    const auto text = logFile.loadFileAsString();
    for (const char* needle : { "processor constructed", "processor destroyed", "prepareToPlay(sampleRate 48000.0, blockSize 512)",
                                "FIRST BLOCK of run", "timeInSamples 96000", "playhead: none", "timeInSamples absent",
                                "END OF RUN", "PASS START at timeline", "PASS END (timeline jumped backwards (loop))",
                                "PASS END (audio stopped (stall))", "timeInSamples discontinuity", "block size changed",
                                "is not advancing", "owns the pass in progress", "capturing instance was deleted",
                                "editor opened", "editor closed", "releaseResources()", "pid ", "reported latency",
                                "STAMP RUN", "alignment found at stream sample 96000", "alignment NOT found",
                                "delivered - selection", "mode learn", "getStateInformation() -> ", "trim -", "LIMITER held peaks by" })
        check(text.contains(needle), std::string("log is missing \"") + needle + "\"");
    check(DiagnosticLog::instance().droppedEvents() == 0, "no log events dropped");
}
} // namespace

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CaptureStore::setClockForTesting(fakeClock);
    TonalRecallEditor::setMotionEnabled(false);   // still frames for the pictures

    const juce::File logFile = argc >= 2 ? juce::File(argv[1])
                                         : juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("TonalRecallTests.log");
    logFile.deleteFile();
    DiagnosticLog::instance().setFileForTesting(logFile.getFullPathName().toStdString());
    DiagnosticLog::instance().retain();   // keeps the writer alive between cases
    MatchHub::instance().retain();        // and the analysis thread
    const juce::File pngFolder = argc >= 3 ? juce::File(argv[2]) : juce::File();

    testPassthrough();
    testSinglePass();
    testSharedStore();
    testOwnership();
    testBackwardJump();
    testStall();
    testMerge();
    testNoPlayHead();
    testFrozenTimeline();
    testCoarseTimeline();
    testSelectionLength();
    testTruncation();
    testIndependentSlots();
    testUserDisarm();
    testInstanceGone();
    testEditor(pngFolder);
    testTail(pngFolder);
    testAudition();
    testRunningPreview();
    testMixedWidths();
    testPlayback(pngFolder);
    if (const auto example = juce::SystemStats::getEnvironmentVariable("TONALRECALL_EXAMPLE_DIR", {}); example.isNotEmpty())
        testRealPair(juce::File(example), pngFolder);
    testEndToEnd(pngFolder);
    testOutputSafety(pngFolder);
    testState();
    check(TonalRecallProcessor::liveInstanceCount() == 0, "all instances destroyed");
    testLog(logFile);

    MatchHub::instance().release();
    DiagnosticLog::instance().release();
    CaptureStore::setClockForTesting(nullptr);
    std::printf("\nlog: %s\n%d checks, %d failed\n", logFile.getFullPathName().toRawUTF8(), g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
