#include "ZeroLatencyConvolver.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace roar
{
    // =============================================================================================
    // ConvolverLayout

    ConvolverLayout ConvolverLayout::forRate (double fs, int maxLength)
    {
        int r = 1;
        if (fs > 132000.0)
            r = 4;
        else if (fs > 66000.0)
            r = 2;

        ConvolverLayout l;
        l.head = 64 * r;
        l.levels[0] = { l.head, l.head, 7 };
        const int p1 = 8 * l.head;
        const int rest = std::max (0, maxLength - p1);
        l.levels[1] = { p1, p1, (rest + p1 - 1) / p1 };
        return l;
    }

    bool ConvolverLayout::operator== (const ConvolverLayout& o) const
    {
        if (head != o.head)
            return false;
        for (size_t i = 0; i < levels.size(); ++i)
            if (levels[i].partition != o.levels[i].partition || levels[i].start != o.levels[i].start || levels[i].count != o.levels[i].count)
                return false;
        return true;
    }

    // =============================================================================================
    // ConvolverIr

    void ConvolverIr::build (const ConvolverLayout& layout, const std::vector<const float*>& outputs, int length)
    {
        m_layout = layout;
        m_numOutputs = std::clamp (static_cast<int> (outputs.size()), 0, maxOutputs);
        m_length = std::clamp (length, 0, layout.maxLength());

        const int h = layout.head;
        for (int o = 0; o < maxOutputs; ++o)
        {
            auto& head = m_head[static_cast<size_t> (o)];
            head.assign (static_cast<size_t> (h), 0.0f);
            double e = 0.0;
            if (o < m_numOutputs)
                for (int t = 0; t < std::min (h, m_length); ++t)
                {
                    const float x = outputs[static_cast<size_t> (o)][t];
                    head[static_cast<size_t> (h - 1 - t)] = x;
                    e += static_cast<double> (x) * x;
                }
            m_headEnergy[static_cast<size_t> (o)] = e;
        }

        for (size_t l = 0; l < m_levels.size(); ++l)
        {
            const auto& spec = layout.levels[l];
            auto& d = m_levels[l];
            const int p = spec.partition;
            d.bins = p + 1;
            d.used = 0;
            if (m_length > spec.start)
                d.used = std::min (spec.count, (m_length - spec.start + p - 1) / p);

            const size_t perOutput = static_cast<size_t> (spec.count) * static_cast<size_t> (d.bins);
            d.re.assign (perOutput * maxOutputs, 0.0f);
            d.im.assign (perOutput * maxOutputs, 0.0f);
            d.energy.assign (static_cast<size_t> (spec.count) * maxOutputs, 0.0);

            RealFft fft;
            fft.prepare (2 * p);
            std::vector<float> seg (static_cast<size_t> (2 * p), 0.0f);
            for (int o = 0; o < m_numOutputs; ++o)
                for (int k = 0; k < d.used; ++k)
                {
                    std::fill (seg.begin(), seg.end(), 0.0f);
                    double e = 0.0;
                    const int first = spec.start + k * p;
                    for (int t = 0; t < p && first + t < m_length; ++t)
                    {
                        const float x = outputs[static_cast<size_t> (o)][first + t];
                        seg[static_cast<size_t> (t)] = x;
                        e += static_cast<double> (x) * x;
                    }
                    const size_t at = static_cast<size_t> (o) * perOutput + static_cast<size_t> (k) * static_cast<size_t> (d.bins);
                    fft.forward (seg.data(), d.re.data() + at, d.im.data() + at);
                    d.energy[static_cast<size_t> (o * spec.count + k)] = e;
                }
        }
    }

    double ConvolverIr::getPartitionEnergy (int level, int o, int k) const
    {
        const auto& d = m_levels[static_cast<size_t> (level)];
        const int count = m_layout.levels[static_cast<size_t> (level)].count;
        if (o < 0 || o >= m_numOutputs || k < 0 || k >= d.used)
            return 0.0;
        return d.energy[static_cast<size_t> (o * count + k)];
    }

    const float* ConvolverIr::spectrumRe (int level, int o, int k) const
    {
        const auto& d = m_levels[static_cast<size_t> (level)];
        const size_t perOutput = static_cast<size_t> (m_layout.levels[static_cast<size_t> (level)].count) * static_cast<size_t> (d.bins);
        return d.re.data() + static_cast<size_t> (o) * perOutput + static_cast<size_t> (k) * static_cast<size_t> (d.bins);
    }

    const float* ConvolverIr::spectrumIm (int level, int o, int k) const
    {
        const auto& d = m_levels[static_cast<size_t> (level)];
        const size_t perOutput = static_cast<size_t> (m_layout.levels[static_cast<size_t> (level)].count) * static_cast<size_t> (d.bins);
        return d.im.data() + static_cast<size_t> (o) * perOutput + static_cast<size_t> (k) * static_cast<size_t> (d.bins);
    }

    // =============================================================================================
    // ZeroLatencyConvolver

    void ZeroLatencyConvolver::prepare (const ConvolverLayout& layout)
    {
        m_layout = layout;
        const int h = layout.head;
        m_history.assign (static_cast<size_t> (2 * h), 0.0f);

        int maxP = 1;
        for (size_t l = 0; l < m_levels.size(); ++l)
        {
            const auto& spec = layout.levels[l];
            auto& s = m_levels[l];
            s.partition = spec.partition;
            s.count = spec.count;
            s.bins = spec.partition + 1;
            s.input.assign (static_cast<size_t> (2 * spec.partition), 0.0f);
            const size_t fdl = static_cast<size_t> (std::max (1, spec.count)) * static_cast<size_t> (s.bins);
            s.fdlRe.assign (fdl, 0.0f);
            s.fdlIm.assign (fdl, 0.0f);
            m_fft[l].prepare (2 * spec.partition);
            maxP = std::max (maxP, spec.partition);
        }

        for (auto& b : m_bindings)
        {
            b.ir = nullptr;
            b.active = 0;
            for (size_t l = 0; l < m_levels.size(); ++l)
                b.gains[l].assign (static_cast<size_t> (std::max (1, m_levels[l].count)), 1.0f);
            for (auto& o : b.outputs)
                for (size_t l = 0; l < m_levels.size(); ++l)
                {
                    o.accRe[l].assign (static_cast<size_t> (m_levels[l].bins), 0.0f);
                    o.accIm[l].assign (static_cast<size_t> (m_levels[l].bins), 0.0f);
                    o.block[l].assign (static_cast<size_t> (m_levels[l].partition), 0.0f);
                }
        }

        m_scratchTime.assign (static_cast<size_t> (2 * maxP), 0.0f);
        m_scratchRe.assign (static_cast<size_t> (maxP + 1), 0.0f);
        m_scratchIm.assign (static_cast<size_t> (maxP + 1), 0.0f);
        reset();
    }

    void ZeroLatencyConvolver::reset()
    {
        m_gridPos = 0;
        std::fill (m_history.begin(), m_history.end(), 0.0f);
        for (auto& s : m_levels)
        {
            s.pos = 0;
            s.newest = 0;
            std::fill (s.input.begin(), s.input.end(), 0.0f);
            std::fill (s.fdlRe.begin(), s.fdlRe.end(), 0.0f);
            std::fill (s.fdlIm.begin(), s.fdlIm.end(), 0.0f);
        }
        for (auto& b : m_bindings)
            for (auto& o : b.outputs)
                for (size_t l = 0; l < m_levels.size(); ++l)
                {
                    std::fill (o.accRe[l].begin(), o.accRe[l].end(), 0.0f);
                    std::fill (o.accIm[l].begin(), o.accIm[l].end(), 0.0f);
                    std::fill (o.block[l].begin(), o.block[l].end(), 0.0f);
                    o.done[l] = 0;
                }
    }

    void ZeroLatencyConvolver::copyStateFrom (const ZeroLatencyConvolver& other)
    {
        assert (other.m_layout == m_layout);
        m_gridPos = other.m_gridPos;
        std::copy (other.m_history.begin(), other.m_history.end(), m_history.begin());
        for (size_t l = 0; l < m_levels.size(); ++l)
        {
            auto& s = m_levels[l];
            const auto& o = other.m_levels[l];
            s.pos = o.pos;
            s.newest = o.newest;
            std::copy (o.input.begin(), o.input.end(), s.input.begin());
            std::copy (o.fdlRe.begin(), o.fdlRe.end(), s.fdlRe.begin());
            std::copy (o.fdlIm.begin(), o.fdlIm.end(), s.fdlIm.begin());
        }
        for (size_t b = 0; b < m_bindings.size(); ++b)
        {
            auto& mine = m_bindings[b];
            const auto& theirs = other.m_bindings[b];
            mine.ir = theirs.ir;
            mine.active = theirs.active;
            for (size_t l = 0; l < m_levels.size(); ++l)
                std::copy (theirs.gains[l].begin(), theirs.gains[l].end(), mine.gains[l].begin());
            for (size_t o = 0; o < mine.outputs.size(); ++o)
                for (size_t l = 0; l < m_levels.size(); ++l)
                {
                    auto& a = mine.outputs[o];
                    const auto& t = theirs.outputs[o];
                    std::copy (t.accRe[l].begin(), t.accRe[l].end(), a.accRe[l].begin());
                    std::copy (t.accIm[l].begin(), t.accIm[l].end(), a.accIm[l].begin());
                    std::copy (t.block[l].begin(), t.block[l].end(), a.block[l].begin());
                    a.done[l] = t.done[l];
                }
        }
    }

    size_t ZeroLatencyConvolver::getStateBytes() const
    {
        size_t floats = m_history.size();
        for (const auto& s : m_levels)
            floats += s.input.size() + s.fdlRe.size() + s.fdlIm.size();
        for (const auto& b : m_bindings)
        {
            for (const auto& g : b.gains)
                floats += g.size();
            for (const auto& o : b.outputs)
                for (size_t l = 0; l < m_levels.size(); ++l)
                    floats += o.accRe[l].size() + o.accIm[l].size() + o.block[l].size();
        }
        return floats * sizeof (float);
    }

    void ZeroLatencyConvolver::bind (int bi, const ConvolverIr* ir, int numOutputs)
    {
        auto& b = m_bindings[static_cast<size_t> (bi)];
        assert (ir == nullptr || ir->getLayout() == m_layout);
        b.ir = ir;
        b.active = 0;
        for (auto& g : b.gains)
            std::fill (g.begin(), g.end(), 1.0f);
        setActiveOutputs (bi, numOutputs);
    }

    void ZeroLatencyConvolver::setActiveOutputs (int bi, int numOutputs)
    {
        auto& b = m_bindings[static_cast<size_t> (bi)];
        if (b.ir == nullptr)
        {
            b.active = 0;
            return;
        }
        numOutputs = std::clamp (numOutputs, 0, b.ir->getNumOutputs());
        for (int o = b.active; o < numOutputs; ++o)
            prime (b, o);
        b.active = numOutputs;
    }

    void ZeroLatencyConvolver::macPartition (int level, Binding& b, int o, int k, int age)
    {
        // acc += g_k X_(newest - age) H_k
        const auto& s = m_levels[static_cast<size_t> (level)];
        auto& out = b.outputs[static_cast<size_t> (o)];
        const int slot = (s.newest - age + s.count * 2) % s.count;
        const float* xr = s.fdlRe.data() + static_cast<size_t> (slot) * static_cast<size_t> (s.bins);
        const float* xi = s.fdlIm.data() + static_cast<size_t> (slot) * static_cast<size_t> (s.bins);
        const float* hr = b.ir->spectrumRe (level, o, k);
        const float* hi = b.ir->spectrumIm (level, o, k);
        float* ar = out.accRe[static_cast<size_t> (level)].data();
        float* ai = out.accIm[static_cast<size_t> (level)].data();
        const float g = b.gains[static_cast<size_t> (level)][static_cast<size_t> (k)];
        const int bins = s.bins;
        for (int i = 0; i < bins; ++i)
        {
            const float gr = g * hr[i], gi = g * hi[i];
            ar[i] += xr[i] * gr - xi[i] * gi;
            ai[i] += xr[i] * gi + xi[i] * gr;
        }
    }

    void ZeroLatencyConvolver::spread (int level, int target)
    {
        // Partitions 1..target of the block that starts at the next boundary: partition k uses the
        // input spectrum k - 1 blocks older than the newest one.
        for (auto& b : m_bindings)
        {
            if (b.ir == nullptr)
                continue;
            const int used = b.ir->getNumPartitions (level);
            for (int o = 0; o < b.active; ++o)
            {
                int& done = b.outputs[static_cast<size_t> (o)].done[static_cast<size_t> (level)];
                const int upTo = std::min (target, used - 1);
                for (int k = done + 1; k <= upTo; ++k)
                    macPartition (level, b, o, k, k - 1);
                done = std::max (done, upTo);
            }
        }
    }

    void ZeroLatencyConvolver::boundary (int level)
    {
        auto& s = m_levels[static_cast<size_t> (level)];
        const int p = s.partition;

        // Finish the earlier partitions (against the delay line as it was), then add the new block.
        spread (level, s.count);

        s.newest = (s.newest + 1) % s.count;
        float* xr = s.fdlRe.data() + static_cast<size_t> (s.newest) * static_cast<size_t> (s.bins);
        float* xi = s.fdlIm.data() + static_cast<size_t> (s.newest) * static_cast<size_t> (s.bins);
        m_fft[static_cast<size_t> (level)].forward (s.input.data(), xr, xi);
        std::copy (s.input.begin() + p, s.input.end(), s.input.begin());

        for (auto& b : m_bindings)
        {
            if (b.ir == nullptr)
                continue;
            const bool any = b.ir->getNumPartitions (level) > 0;
            for (int o = 0; o < b.active; ++o)
            {
                auto& out = b.outputs[static_cast<size_t> (o)];
                float* ar = out.accRe[static_cast<size_t> (level)].data();
                float* ai = out.accIm[static_cast<size_t> (level)].data();
                float* block = out.block[static_cast<size_t> (level)].data();
                if (any)
                {
                    macPartition (level, b, o, 0, 0);
                    m_fft[static_cast<size_t> (level)].inverse (ar, ai, m_scratchTime.data());
                    std::copy (m_scratchTime.begin() + p, m_scratchTime.begin() + 2 * p, block);
                }
                else
                {
                    std::fill (block, block + p, 0.0f);
                }
                std::fill (ar, ar + s.bins, 0.0f);
                std::fill (ai, ai + s.bins, 0.0f);
                out.done[static_cast<size_t> (level)] = 0;
            }
        }
    }

    void ZeroLatencyConvolver::prime (Binding& b, int o)
    {
        auto& out = b.outputs[static_cast<size_t> (o)];
        for (int level = 0; level < static_cast<int> (m_levels.size()); ++level)
        {
            const auto& s = m_levels[static_cast<size_t> (level)];
            const auto li = static_cast<size_t> (level);
            const int used = b.ir->getNumPartitions (level);
            auto& accRe = out.accRe[li];
            auto& accIm = out.accIm[li];

            // 1. The block playing now, from the delay line as it stood at the last boundary.
            std::fill (accRe.begin(), accRe.end(), 0.0f);
            std::fill (accIm.begin(), accIm.end(), 0.0f);
            for (int k = 0; k < used; ++k)
                macPartition (level, b, o, k, k);
            if (used > 0)
            {
                m_fft[li].inverse (accRe.data(), accIm.data(), m_scratchTime.data());
                std::copy (m_scratchTime.begin() + s.partition, m_scratchTime.begin() + 2 * s.partition, out.block[li].begin());
            }
            else
            {
                std::fill (out.block[li].begin(), out.block[li].end(), 0.0f);
            }

            // 2. The next block's accumulator: empty; spread() catches it up at the next grid point.
            std::fill (accRe.begin(), accRe.end(), 0.0f);
            std::fill (accIm.begin(), accIm.end(), 0.0f);
            out.done[li] = 0;
        }
    }

    void ZeroLatencyConvolver::process (const float* in, int n, float* const (&out)[maxBindings][maxOutputs])
    {
        const int h = m_layout.head;
        assert (n <= h - m_gridPos);
        n = std::min (n, h - m_gridPos);
        if (n <= 0)
            return;

        // History: h - 1 past samples, then this segment.
        float* hist = m_history.data();
        std::copy (in, in + n, hist + (h - 1));

        // Every level's input block.
        for (auto& s : m_levels)
            std::copy (in, in + n, s.input.begin() + s.partition + s.pos);

        for (size_t bi = 0; bi < m_bindings.size(); ++bi)
        {
            auto& b = m_bindings[bi];
            if (b.ir == nullptr)
                continue;
            for (int o = 0; o < b.active; ++o)
            {
                float* y = out[bi][o];
                if (y == nullptr)
                    continue;
                const float* taps = b.ir->headReversed (o);
                const auto& os = b.outputs[static_cast<size_t> (o)];
                const float* b0 = os.block[0].data() + m_levels[0].pos;
                const float* b1 = os.block[1].data() + m_levels[1].pos;
                for (int i = 0; i < n; ++i)
                {
                    const float* x = hist + i;
                    float acc = 0.0f;
                    for (int t = 0; t < h; ++t)
                        acc += taps[t] * x[t];
                    y[i] = acc + b0[i] + b1[i];
                }
            }
        }

        // Keep the last h - 1 samples for the next segment.
        std::copy (hist + n, hist + n + (h - 1), hist);

        for (auto& s : m_levels)
            s.pos += n;
        m_gridPos += n;
        if (m_gridPos < h)
            return;

        // A grid point: complete blocks run their boundary, the others spread their share.
        m_gridPos = 0;
        for (int level = 0; level < static_cast<int> (m_levels.size()); ++level)
        {
            auto& s = m_levels[static_cast<size_t> (level)];
            if (s.count <= 0)
            {
                s.pos = 0;
                continue;
            }
            if (s.pos >= s.partition)
            {
                boundary (level);
                s.pos = 0;
            }
            else
            {
                // ceil ((count - 1) pos / P): all of them by the end of the block.
                const int target = static_cast<int> ((static_cast<long long> (s.count - 1) * s.pos + s.partition - 1) / s.partition);
                spread (level, target);
            }
        }
    }
}
