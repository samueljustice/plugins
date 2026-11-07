#pragma once

#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>

/**
 * A rolling min/max waveform for the little section scopes: the audio thread
 * pushes samples, every bucket of them becomes one (min, max) point, and the
 * UI reads the last c_points points. Lock-free (relaxed atomics per point;
 * a torn read only ever draws one slightly wrong column).
 */
class ScopeRing
{
public:
    static constexpr int c_points = 240;

    /** Audio thread (prepare): how many seconds the scope shows. */
    void prepare(double sampleRate, double seconds = 3.0)
    {
        m_bucket = std::max(1, static_cast<int>(sampleRate * seconds / c_points));
        m_count = 0;
        m_lo = 1.0f;
        m_hi = -1.0f;
    }

    /** Audio thread. */
    void push(float x)
    {
        if (! std::isfinite(x))
            x = 0.0f;
        m_lo = std::min(m_lo, x);
        m_hi = std::max(m_hi, x);
        if (++m_count < m_bucket)
            return;
        const int w = m_write.load(std::memory_order_relaxed);
        m_points[static_cast<size_t>(w)].lo.store(m_lo, std::memory_order_relaxed);
        m_points[static_cast<size_t>(w)].hi.store(m_hi, std::memory_order_relaxed);
        m_write.store((w + 1) % c_points, std::memory_order_release);
        m_count = 0;
        m_lo = 1.0f;
        m_hi = -1.0f;
    }

    /** Any thread: oldest first. */
    void read(std::array<float, c_points>& lo, std::array<float, c_points>& hi) const
    {
        const int w = m_write.load(std::memory_order_acquire);
        for (int k = 0; k < c_points; ++k)
        {
            const auto& p = m_points[static_cast<size_t>((w + k) % c_points)];
            lo[static_cast<size_t>(k)] = p.lo.load(std::memory_order_relaxed);
            hi[static_cast<size_t>(k)] = p.hi.load(std::memory_order_relaxed);
        }
    }

    /** Any thread: a counter that moves whenever a new point arrives (to skip repaints when nothing changed). */
    int position() const { return m_write.load(std::memory_order_relaxed); }

private:
    struct Point
    {
        std::atomic<float> lo { 0.0f }, hi { 0.0f };
    };
    std::array<Point, c_points> m_points;
    std::atomic<int> m_write { 0 };
    int m_bucket = 600, m_count = 0;
    float m_lo = 1.0f, m_hi = -1.0f;
};
