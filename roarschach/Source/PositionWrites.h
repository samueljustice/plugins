#pragma once

#include <atomic>

namespace roar
{
    /**
     * Marks a stretch in which the module positions are being written one parameter at a time
     * (a tile drop, the move menu, a preset load, a state restore). The audio thread keeps the
     * last complete order while a write is in progress, so it never resolves a half-written set
     * into an order that is neither the old one nor the new one. Lock-free; scopes may nest and
     * may come from different threads.
     */
    class PositionWrites
    {
    public:
        class Scope
        {
        public:
            explicit Scope(PositionWrites* writes) : m_writes(writes)
            {
                if (m_writes != nullptr)
                    m_writes->m_depth.fetch_add(1, std::memory_order_acq_rel);
            }
            ~Scope()
            {
                if (m_writes != nullptr)
                    m_writes->m_depth.fetch_sub(1, std::memory_order_acq_rel);
            }
            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;

        private:
            PositionWrites* m_writes;
        };

        bool inProgress() const { return m_depth.load(std::memory_order_acquire) > 0; }

    private:
        std::atomic<int> m_depth { 0 };
    };
}
