#pragma once

#include "RealFft.h"

#include <array>
#include <cstdint>
#include <vector>

namespace roar
{
    /**
     * How the convolver splits an impulse response (all lengths in samples at the rate it runs at):
     *
     *   head      taps [0, B)              direct-form FIR, per sample: no latency
     *   level 0   taps [B, 8B)             uniformly partitioned, partition B (FFT 2B)
     *   level 1   taps [8B, length)        uniformly partitioned, partition 8B (FFT 16B)
     *
     * B = 64 at 44.1 / 48 kHz, 128 at 88.2 / 96 kHz, 256 at 176.4 / 192 kHz, so the parts cover
     * the same times at every rate (head 1.3 ms, level 0 to 10.7 ms). A level with partition P
     * starts at tap P: the block of P inputs that completes at time T, convolved with the
     * partition starting at tap P + kP, first reaches the output at T + 1 + kP, so every level's
     * output for the next P samples is ready the moment its block completes. That is what makes
     * the whole convolver latency-free.
     */
    struct ConvolverLayout
    {
        struct Level
        {
            int partition = 0;   // P
            int start = 0;       // first tap (= P)
            int count = 0;       // partitions (enough for the longest IR this layout serves)
        };

        int head = 64;           // B, also the internal grid
        std::array<Level, 2> levels {};

        /** The layout for a sample rate and the longest IR (samples at that rate) it must hold. */
        static ConvolverLayout forRate (double fs, int maxLength);
        int maxLength() const { return levels[1].start + levels[1].count * levels[1].partition; }
        bool operator== (const ConvolverLayout& o) const;
    };

    /**
     * An impulse response prepared for one ConvolverLayout: up to two outputs (here the mid and
     * the side of a stereo room), each as a reversed head and the partition spectra of both
     * levels, plus the energy of every part (for loudness bookkeeping). Immutable after build(),
     * so one instance can be shared by any number of convolvers and threads.
     */
    class ConvolverIr
    {
    public:
        static constexpr int maxOutputs = 2;

        /** outputs[o]: `length` samples each (length <= layout.maxLength()). Allocates. */
        void build (const ConvolverLayout& layout, const std::vector<const float*>& outputs, int length);

        int getNumOutputs() const { return m_numOutputs; }
        int getLength() const { return m_length; }
        const ConvolverLayout& getLayout() const { return m_layout; }

        /** Partitions of level l that hold any part of this IR. */
        int getNumPartitions (int level) const { return m_levels[static_cast<size_t> (level)].used; }
        /** Energy (sum of squares) of the head of output o, and of partition k of level l. */
        double getHeadEnergy (int o) const { return m_headEnergy[static_cast<size_t> (o)]; }
        double getPartitionEnergy (int level, int o, int k) const;

        // For the convolver.
        const float* headReversed (int o) const { return m_head[static_cast<size_t> (o)].data(); }
        const float* spectrumRe (int level, int o, int k) const;
        const float* spectrumIm (int level, int o, int k) const;

    private:
        struct LevelData
        {
            int used = 0, bins = 0;
            std::vector<float> re, im;      // [output][partition][bin]
            std::vector<double> energy;     // [output][partition]
        };

        ConvolverLayout m_layout;
        int m_numOutputs = 0, m_length = 0;
        std::array<std::vector<float>, maxOutputs> m_head;
        std::array<double, maxOutputs> m_headEnergy {};
        std::array<LevelData, 2> m_levels;
    };

    /**
     * Zero-latency, block-size-independent, copyable convolver (CHAMBER's room engine). One mono
     * input feeds up to two "bindings" (IRs, e.g. the room and the room it is fading from), each
     * with up to two outputs (mid, side) sharing the input's history and partition spectra.
     *
     *   - Head: direct form. Levels: uniformly partitioned overlap-save on a frequency-domain
     *     delay line of the input. A level's boundary work (one input FFT, the newest partition's
     *     multiply-add and one inverse FFT per output) runs when its block completes; the other
     *     partitions' multiply-adds are spread over the block before (they only need older input),
     *     so no call does much more than its share.
     *   - Timing: everything runs on an internal grid of B samples counted from reset(): call
     *     process() with at most samplesToGrid() samples; the work done at a grid point never
     *     depends on how the host split its blocks, so the output is bit-identical for any block
     *     sizes. Settings that change the result (the partition gains) are read only at grid
     *     points, so a caller that changes them only at grid points (gridPosition() == 0) keeps
     *     that guarantee.
     *   - Partition gains: every partition's multiply-add is scaled by gains(b, level)[k] (1 after
     *     bind). The head is never scaled. CHAMBER's Resonance shapes the late part with them.
     *   - Clone: copyStateFrom() copies every state (input history, delay lines, accumulators,
     *     outputs, gains, counters) into the buffers prepare() allocated. Bindings point at shared,
     *     immutable ConvolverIr instances.
     *   - An output that becomes active (bind, or setActiveOutputs) is primed at once: its current
     *     block is computed from the delay line, so it starts exactly as if it had always run.
     *     That block is needed at once, so its multiply-add over every used partition of both
     *     levels (plus one IFFT per level) runs in that one call: the block holding a Room change
     *     or Width turning on costs about 2.5-3x a steady block (CHAMBER at 192 kHz, block 256,
     *     Width 70 %: ~250 us against ~90 us, ~19 % of the block), and the 50 ms crossfade that
     *     follows runs both bindings (~1.7x). The next block's accumulator is left for spread() to
     *     fill over the following ticks. testChamber reports this worst block.
     *
     * prepare() allocates; nothing else does. Plain C++, no JUCE.
     */
    class ZeroLatencyConvolver
    {
    public:
        static constexpr int maxBindings = 2;
        static constexpr int maxOutputs = ConvolverIr::maxOutputs;

        void prepare (const ConvolverLayout& layout);
        void reset();
        void copyStateFrom (const ZeroLatencyConvolver& other);

        const ConvolverLayout& getLayout() const { return m_layout; }
        int gridPosition() const { return m_gridPos; }
        int samplesToGrid() const { return m_layout.head - m_gridPos; }

        /** Binds an IR (same layout, or nullptr) to slot b with `numOutputs` outputs active; gains reset to 1. */
        void bind (int b, const ConvolverIr* ir, int numOutputs);
        /** Activates or stops outputs of a binding (newly active ones are primed). */
        void setActiveOutputs (int b, int numOutputs);
        const ConvolverIr* getBinding (int b) const { return m_bindings[static_cast<size_t> (b)].ir; }
        int getActiveOutputs (int b) const { return m_bindings[static_cast<size_t> (b)].ir != nullptr ? m_bindings[static_cast<size_t> (b)].active : 0; }

        /** The partition gains of binding b, level l (count entries). Change them at grid points only. */
        float* gains (int b, int level) { return m_bindings[static_cast<size_t> (b)].gains[static_cast<size_t> (level)].data(); }

        /**
         * n <= samplesToGrid(). out[b][o] receives binding b's output o (only for active ones; may
         * be nullptr for inactive ones).
         */
        void process (const float* in, int n, float* const (&out)[maxBindings][maxOutputs]);

        /** Bytes of state (what copyStateFrom copies), for the tests. */
        size_t getStateBytes() const;

    private:
        struct OutputState
        {
            std::array<std::vector<float>, 2> accRe, accIm;   // per level: P + 1 bins
            std::array<std::vector<float>, 2> block;          // per level: P samples, the block playing now
            std::array<int, 2> done {};                       // per level: partitions (1..) already in acc
        };

        struct Binding
        {
            const ConvolverIr* ir = nullptr;
            int active = 0;
            std::array<std::vector<float>, 2> gains;          // per level: count
            std::array<OutputState, maxOutputs> outputs;
        };

        struct LevelState
        {
            int partition = 0, count = 0, bins = 0;
            int pos = 0;                      // samples into the current block
            int newest = 0;                   // FDL slot of the newest input spectrum
            std::vector<float> input;         // 2P: the last block and the one filling
            std::vector<float> fdlRe, fdlIm;  // [count][bins]
        };

        void boundary (int level);
        void spread (int level, int target);
        void macPartition (int level, Binding& b, int o, int k, int age);
        void prime (Binding& b, int o);

        ConvolverLayout m_layout;
        int m_gridPos = 0;
        std::vector<float> m_history;         // head - 1 past samples + up to head new ones
        std::array<LevelState, 2> m_levels;
        std::array<Binding, maxBindings> m_bindings;

        // Scratch (no state).
        std::array<RealFft, 2> m_fft;
        std::vector<float> m_scratchTime, m_scratchRe, m_scratchIm;
    };
}
