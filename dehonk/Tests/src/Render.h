// Runs audio through dehonk::Core the way a host would: fixed or varying block sizes,
// optional per-block parameter automation, display frames captured as the core writes them,
// and latency compensation so the output lines up sample for sample with the input.
#pragma once

#include "Audio.h"
#include "DeHonkCore.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace dh
{

struct Frame
{
    double timeSec = 0.0;       // input time the frame was read at (end of the block)
    std::uint32_t counter = 0;  // Core::frameCounter() when it was read
    float totalCutDb = 0.0f;
    float programDb = -120.0f;  // programme level the threshold is compared with
    float activity = 0.0f;      // 0..1, how far above the threshold
    std::array<float, dehonk::kMaxBands> cutDb {};
    std::array<float, dehonk::kMaxBands> levelDb {};
    std::array<float, dehonk::kNumModules> moduleCutDb {};   // deepest notch in each module
    float snapCutDb = 0.0f;    // deepest cut the Snap module made since the core's previous frame
    float diffOffsetDb = 0.0f; // the gain Diff applies to the untreated signal, dB (0 or below)
    float guard = 0.0f;        // 0..1: how much of the cut the impact guard is holding back
    std::array<float, 12> pitchClass {};   // the core's note profile, 0 = C
};

/** Module names as the plug-in shows them. */
const char* moduleName (int module);

/** The edges as DeHonkCore.h documents them: each clamped to kEdgeMinHz..kEdgeMaxHz. */
std::array<float, dehonk::kNumEdges> clampedEdges (const dehonk::Params& p);

/** The module whose documented range (edgeHz[m] up to edgeHz[m+1]) holds hz, whether it is
    on or not, or -1 outside every module. Worked out by the harness from the edges; the core
    does not expose its own band-to-module map. */
int moduleAt (double hz, const dehonk::Params& p);

struct RenderSpec
{
    dehonk::Params params;
    bool useTuning = false;
    dehonk::Tuning tuning;
    int blockSize = 512;
    int maxBlockSize = 0;                                   // 0 = blockSize
    std::function<int()> nextBlockSize;                     // optional: varying block sizes
    std::function<void (std::int64_t, dehonk::Params&)> automate; // optional: called before every block
    bool compensateLatency = true;                          // uses the latency after the initial params
    bool collectFrames = false;
    bool timeBlocks = false;
    std::vector<float> staticCuts;                          // non-empty: Core::debugSetStaticCuts
    bool useLayout = false;                                 // prepare with `layout` (else the three-argument prepare)
    dehonk::ChannelLayout layout;
    /** Read every frame the core writes. The core writes a frame at every detector tick and
        DisplayFrame::snapCutDb covers only the time since the previous one, so reading once per
        block of more than a tick misses most of what Snap did. With this set, each block is
        processed in pieces of at most kFrameSubBlock samples (no larger than the shortest tick at
        any rate) and the frame is read after each piece. Parameters still change only at block
        starts; the audio is the same, since the core is block-size invariant. */
    bool frameEveryTick = false;
};

inline constexpr int kFrameSubBlock = 4;

/** A tuning applied by render() when the spec does not give its own (useTuning false). Lets a
    whole test run be repeated with a deliberately wrong constant (dehonk_tests --set). nullptr
    clears it. The pointer is kept, not copied. */
void setTuningOverride (const dehonk::Tuning* tuning);
const dehonk::Tuning* tuningOverride();

struct RenderResult
{
    Audio out;
    int latency = 0;
    int numBands = 0;
    std::vector<float> bandHz;
    std::vector<Frame> frames;
    std::vector<double> blockSeconds;   // when timeBlocks
    std::vector<int> blockSizes;        // when timeBlocks
    std::vector<std::int64_t> blockStarts; // when timeBlocks
};

/** prepare(), setTuning(), setParams(), reset(), then debugSetStaticCuts() if asked, then
    process the whole input. With compensateLatency, latency samples of silence are appended to
    the input and the first latency samples of output are dropped. */
RenderResult render (const Audio& in, const RenderSpec& spec);
RenderResult render (dehonk::Core& core, const Audio& in, const RenderSpec& spec);

/** Pushes the input through an already prepared core with no latency compensation and no
    re-preparation, in blocks of blockSize. For mid-stream prepare()/reset() tests. */
Audio streamThrough (dehonk::Core& core, const Audio& in, int blockSize);

/** Sets flush-to-zero (and denormals-are-zero on x86) on this thread, as JUCE's
    ScopedNoDenormals does around a plug-in's processBlock. */
void enableFlushDenormals();

} // namespace dh
