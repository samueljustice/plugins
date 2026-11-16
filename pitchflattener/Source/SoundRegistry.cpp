#include "SoundRegistry.h"

#include "Engine.h"

#include <algorithm>

namespace pitchflattener
{

SoundRegistry& SoundRegistry::instance()
{
    static SoundRegistry registry;
    return registry;
}

void SoundRegistry::pruneLocked(std::deque<Kept>& expired)
{
    const auto now = std::chrono::steady_clock::now();
    while (! m_kept.empty() && (m_kept.size() > kKeep || m_kept.back().until < now))
    {
        expired.push_back(std::move(m_kept.back()));
        m_kept.pop_back();
    }
    for (auto it = m_entries.begin(); it != m_entries.end();)
        if (it->second.learned.expired())
            it = m_entries.erase(it);
        else
            ++it;
}

void SoundRegistry::publish(const std::shared_ptr<const Learned>& learned, const std::shared_ptr<const Result>& render)
{
    if (learned == nullptr || learned->id == 0)
        return;
    std::deque<Kept> expired;   // freed outside the lock
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto& entry = m_entries[learned->id];
        entry.learned = learned;
        if (render != nullptr)
        {
            auto& renders = entry.renders;
            renders.erase(std::remove_if(renders.begin(), renders.end(), [&](const std::weak_ptr<const Result>& w)
                                         { const auto r = w.lock(); return r == nullptr || r == render; }),
                          renders.end());
            renders.insert(renders.begin(), render);
            if (renders.size() > kRendersPerSound)
                renders.resize(kRendersPerSound);
        }
        pruneLocked(expired);
    }
}

void SoundRegistry::letGo(const std::shared_ptr<const Learned>& learned, const std::shared_ptr<const Result>& render)
{
    if (learned == nullptr || learned->id == 0)
        return;
    std::deque<Kept> expired;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_kept.begin(); it != m_kept.end(); ++it)
            if (it->learned == learned)
            {
                expired.push_back(std::move(*it));
                m_kept.erase(it);
                break;
            }
        const auto until = std::chrono::steady_clock::now()
                         + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(kKeepSeconds));
        m_kept.push_front({ learned, render, until });
        pruneLocked(expired);
    }
}

SoundRegistry::Found SoundRegistry::find(std::uint64_t id)
{
    Found found;
    std::deque<Kept> expired;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        pruneLocked(expired);
        if (const auto it = m_entries.find(id); it != m_entries.end())
        {
            found.learned = it->second.learned.lock();
            for (const auto& w : it->second.renders)
                if (auto r = w.lock())
                    found.renders.push_back(std::move(r));
        }
        for (const auto& k : m_kept)
            if (k.learned != nullptr && k.learned->id == id && k.render != nullptr
                && std::find(found.renders.begin(), found.renders.end(), k.render) == found.renders.end())
                found.renders.push_back(k.render);
    }
    return found;
}

void SoundRegistry::clearForTesting()
{
    std::deque<Kept> expired;
    std::unordered_map<std::uint64_t, Entry> entries;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        expired.swap(m_kept);
        entries.swap(m_entries);
    }
}

} // namespace pitchflattener
