#include "Modulators.h"
#include "Parameters.h"

namespace
{
    constexpr float c_twoPi = juce::MathConstants<float>::twoPi;
}

const std::vector<juce::String>& ModMatrix::targets()
{
    static const std::vector<juce::String> list = []
    {
        std::vector<juce::String> ids { ParamIDs::energy, ParamIDs::rate, ParamIDs::fill, ParamIDs::chaos, ParamIDs::pitch };
        for (int b = 0; b < ParamIDs::numBuses; ++b)
            for (int k = 0; k < ParamIDs::numBusControls; ++k)
                if (ParamIDs::bus[b][k] != nullptr)
                    ids.emplace_back(ParamIDs::bus[b][k]);
        for (auto* id : ParamIDs::stow) // Stowaway's five knobs (not its Mode)
            ids.emplace_back(id);
        for (auto* id : { ParamIDs::hitSense, ParamIDs::hitBoost, ParamIDs::hitRelease, ParamIDs::hitDuck,
                          ParamIDs::hitHold, ParamIDs::hitFloor, ParamIDs::duckReturn,
                          ParamIDs::space, ParamIDs::inflate, ParamIDs::push, ParamIDs::ceiling,
                          ParamIDs::tremDepth, ParamIDs::tremRate, ParamIDs::tremThreshold }) // appended: wires are saved by ID
            ids.emplace_back(id);
        for (auto* id : ParamIDs::trackSemis) // each track's pitch (the Semitones box): modulated smoothly, not in steps
            ids.emplace_back(id);
        for (auto* id : { ParamIDs::roomSpread, ParamIDs::roomHeight }) // the room's Spread and Height (the speakers' gains glide)
            ids.emplace_back(id);
        // Direction and Side: the modulator decides pass by pass, as each pass starts (see centred()).
        for (auto* id : { ParamIDs::direction[0], ParamIDs::direction[1], ParamIDs::stowDirection,
                          ParamIDs::side[0], ParamIDs::side[1], ParamIDs::stowSide })
            ids.emplace_back(id);
        ids.emplace_back(ParamIDs::hitSpread); // how far a hit bleeds (the speakers' gains glide)
        // Each bus's Varispeed (a slow LFO is a hands-free lean), and Swell: held while its modulator is in the top
        // part of its range (see swellGate()). The detail layers' other wheels are not wire targets.
        ids.emplace_back(ParamIDs::busDetail[0][ParamIDs::Varispeed]);
        ids.emplace_back(ParamIDs::busDetail[1][ParamIDs::Varispeed]);
        ids.emplace_back(ParamIDs::swell);
        jassert(ids.size() <= 64);
        return ids;
    }();
    return list;
}

int ModMatrix::swellTarget()
{
    static const int index = targetIndex(ParamIDs::swell);
    return index;
}

int ModMatrix::targetIndex(const juce::String& paramId)
{
    const auto& t = targets();
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i] == paramId)
            return static_cast<int>(i);
    return -1;
}

juce::String ModMatrix::typeName(Type t)
{
    switch (t)
    {
        case Type::Lfo:      return "LFO";
        case Type::Random:   return "Random";
        case Type::Follower: return "Follower";
        case Type::Macro:    return "Macro";
    }
    return {};
}

juce::String ModMatrix::shapeName(Shape s)
{
    switch (s)
    {
        case Shape::Sine:     return "Sine";
        case Shape::Triangle: return "Triangle";
        case Shape::Ramp:     return "Ramp";
        case Shape::Square:   return "Square";
        case Shape::Steps:    return "Steps";
    }
    return {};
}

//==============================================================================
int ModMatrix::connect(int source, const juce::String& paramId, float depth)
{
    const int target = targetIndex(paramId);
    if (target < 0 || source < 0 || source >= c_numSources)
        return -1;
    int freeSlot = -1;
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        for (int s = 0; s < c_maxRoutes; ++s)
        {
            const auto si = static_cast<size_t>(s);
            if (m_params[si] == paramId && m_slots[si].source.load() == source)
                return s; // already wired
            if (freeSlot < 0 && m_params[si].isEmpty())
                freeSlot = s;
        }
        if (freeSlot < 0)
            return -1;
        auto& slot = m_slots[static_cast<size_t>(freeSlot)];
        slot.source.store(source);
        slot.depth.store(juce::jlimit(-1.0f, 1.0f, depth));
        slot.target.store(target); // last: the audio thread only reads complete slots
        m_params[static_cast<size_t>(freeSlot)] = paramId;
    }
    changed();
    return freeSlot;
}

void ModMatrix::disconnect(int slot, int expectedGeneration)
{
    if (slot < 0 || slot >= c_maxRoutes)
        return;
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        if (expectedGeneration >= 0 && expectedGeneration != m_generation.load())
            return; // the wires were restored meanwhile: this slot is another wire now
        m_slots[static_cast<size_t>(slot)].target.store(-1);
        m_params[static_cast<size_t>(slot)].clear();
    }
    changed();
}

bool ModMatrix::setDepth(int slot, float depth, int expectedGeneration)
{
    if (slot < 0 || slot >= c_maxRoutes)
        return false;
    const std::lock_guard<std::mutex> lock(m_lock);
    if ((expectedGeneration >= 0 && expectedGeneration != m_generation.load()) || m_params[static_cast<size_t>(slot)].isEmpty())
        return false;
    const float limited = juce::jlimit(-1.0f, 1.0f, depth);
    if (! juce::exactlyEqual(m_slots[static_cast<size_t>(slot)].depth.exchange(limited), limited))
        m_changeCount.fetch_add(1); // the session changed (no onChange: see the header)
    return true;
}

void ModMatrix::clearSource(int source)
{
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        for (size_t s = 0; s < m_slots.size(); ++s)
            if (! m_params[s].isEmpty() && m_slots[s].source.load() == source)
            {
                m_slots[s].target.store(-1);
                m_params[s].clear();
            }
    }
    changed();
}

void ModMatrix::clear()
{
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        ++m_generation;
        for (size_t s = 0; s < m_slots.size(); ++s)
        {
            m_slots[s].target.store(-1);
            m_params[s].clear();
        }
    }
    changed();
}

std::vector<ModMatrix::Route> ModMatrix::getRoutes() const
{
    std::vector<Route> routes;
    const std::lock_guard<std::mutex> lock(m_lock);
    for (size_t s = 0; s < m_slots.size(); ++s)
        if (! m_params[s].isEmpty())
            routes.push_back({ static_cast<int>(s), m_slots[s].source.load(), m_params[s], m_slots[s].depth.load() });
    return routes;
}

int ModMatrix::numRoutes(int source) const
{
    int n = 0;
    const std::lock_guard<std::mutex> lock(m_lock);
    for (size_t s = 0; s < m_slots.size(); ++s)
        if (! m_params[s].isEmpty() && m_slots[s].source.load() == source)
            ++n;
    return n;
}

bool ModMatrix::hasWireTo(const juce::String& paramId) const
{
    const std::lock_guard<std::mutex> lock(m_lock);
    for (const auto& param : m_params)
        if (param == paramId)
            return true;
    return false;
}

juce::ValueTree ModMatrix::toValueTree() const
{
    juce::ValueTree tree("Mods");
    for (const auto& r : getRoutes())
    {
        juce::ValueTree w("Wire");
        w.setProperty("source", r.source, nullptr);
        w.setProperty("param", r.param, nullptr);
        w.setProperty("depth", r.depth, nullptr);
        tree.appendChild(w, nullptr);
    }
    return tree;
}

void ModMatrix::fromValueTree(const juce::ValueTree& tree)
{
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        ++m_generation;
        for (size_t s = 0; s < m_slots.size(); ++s)
        {
            m_slots[s].target.store(-1);
            m_params[s].clear();
        }
        size_t next = 0;
        for (const auto& w : tree)
        {
            if (! w.hasType("Wire") || next >= m_slots.size())
                continue;
            const juce::String param = w.getProperty("param").toString();
            const int target = targetIndex(param);
            const int source = static_cast<int>(w.getProperty("source", 0));
            if (target < 0 || source < 0 || source >= c_numSources)
                continue;
            m_slots[next].source.store(source);
            m_slots[next].depth.store(juce::jlimit(-1.0f, 1.0f, static_cast<float>(w.getProperty("depth", 0.5f))));
            m_slots[next].target.store(target);
            m_params[next] = param;
            ++next;
        }
    }
    if (juce::MessageManager::existsAndIsCurrentThread())
        changed();
}

void ModMatrix::changed()
{
    m_changeCount.fetch_add(1);
    if (onChange)
        onChange();
}

//==============================================================================
void ModMatrix::prepare(double sampleRate)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    swellTarget(); // (looked up once, here: the first process() would otherwise build the list of targets, which allocates)
    reset();
}

void ModMatrix::reset()
{
    for (auto& b : m_bipolarView)
        b.store(true, std::memory_order_relaxed); // until process() knows the types (the UI derives them itself anyway)
    for (auto& s : m_state)
    {
        s = State {};
        // Random and Steps have somewhere to go from the start (not a whole silent first cycle).
        s.to = m_rng.nextFloat() * 2.0f - 1.0f;
        s.held = m_rng.nextFloat() * 2.0f - 1.0f;
    }
    std::fill(m_offsets.begin(), m_offsets.end(), 0.0f);
    for (auto& v : m_targetView)
        v.store(-1.0f, std::memory_order_relaxed);
    m_gateOpen.fill(false);
    m_gateHeld.fill(0.0);
    m_swellGate = false;
    m_swellGateView.store(false, std::memory_order_relaxed);
}

void ModMatrix::scatter(juce::Random& random)
{
    for (auto& s : m_state)
    {
        s.phase = random.nextDouble();
        s.from = random.nextFloat() * 2.0f - 1.0f;
        s.to = random.nextFloat() * 2.0f - 1.0f;
        s.held = random.nextFloat() * 2.0f - 1.0f;
        s.env = random.nextFloat();
    }
}

void ModMatrix::process(const std::array<SourceSettings, c_numSources>& settings, int numSamples, const FollowerInputs& followerInputs)
{
    const double dt = static_cast<double>(numSamples) / m_sampleRate;
    std::array<float, c_numSources> values {};
    std::array<bool, c_numSources> bipolar {};

    for (int i = 0; i < c_numSources; ++i)
    {
        const auto si = static_cast<size_t>(i);
        const auto& set = settings[si];
        auto& st = m_state[si];
        switch (set.type)
        {
            case Type::Lfo:
            {
                st.phase += rateHz(set.knob) * dt;
                const bool wrapped = st.phase >= 1.0;
                st.phase -= std::floor(st.phase);
                const float p = static_cast<float>(st.phase);
                switch (set.shape)
                {
                    case Shape::Sine:     st.value = std::sin(c_twoPi * p); break;
                    case Shape::Triangle: st.value = 1.0f - 4.0f * std::abs(p - 0.5f); break;
                    case Shape::Ramp:     st.value = 2.0f * p - 1.0f; break;
                    case Shape::Square:   st.value = p < 0.5f ? 1.0f : -1.0f; break;
                    case Shape::Steps:
                        if (wrapped) st.held = m_rng.nextFloat() * 2.0f - 1.0f;
                        st.value = st.held;
                        break;
                }
                bipolar[si] = true;
                break;
            }
            case Type::Random:
            {
                // A smooth random walk: a new target every cycle, eased in with a cosine.
                st.phase += rateHz(set.knob) * dt;
                if (st.phase >= 1.0)
                {
                    st.phase -= std::floor(st.phase);
                    st.from = st.to;
                    st.to = m_rng.nextFloat() * 2.0f - 1.0f;
                }
                const float e = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * static_cast<float>(st.phase));
                st.value = st.from + (st.to - st.from) * e;
                bipolar[si] = true;
                break;
            }
            case Type::Follower:
            {
                // Loudness of the chosen input, -40..-6 dBFS -> 0..1. Speed: slow (2 s, follows whole passes) .. fast (26 ms).
                // The Master inputs are the Master track's contours, already 0..1 over the sound's own range.
                const int which = juce::jlimit(0, c_numFollowerInputs - 1, set.follow);
                const float in = followerInputs[static_cast<size_t>(which)];
                const float db = juce::Decibels::gainToDecibels(juce::jmax(1.0e-6f, in), -100.0f);
                const float target = which >= FollowMaster ? juce::jlimit(0.0f, 1.0f, in) : juce::jlimit(0.0f, 1.0f, (db + 40.0f) / 34.0f);
                // (A Pass or a Filter input is a place that already moves smoothly: one time both ways, a tenth of
                // the loudness inputs', so its top is not held back by a slow fall.)
                const bool place = followsPlace(which);
                const float tau = followerSeconds(set.knob, which);
                const float tauNow = target > st.env && ! place ? tau * 0.3f : tau;
                st.env += (target - st.env) * (1.0f - std::exp(static_cast<float>(-dt) / juce::jmax(0.001f, tauNow)));
                st.value = st.env;
                bipolar[si] = false;
                break;
            }
            case Type::Macro:
                st.value = juce::jlimit(0.0f, 1.0f, set.knob);
                bipolar[si] = false;
                break;
        }
        values[si] = st.value;
        m_sourceView[si].store(st.value, std::memory_order_relaxed);
        m_bipolarView[si].store(bipolar[si], std::memory_order_relaxed);
    }

    // Sum the wires per target, then glide (15 ms) so square / steps don't click.
    std::fill(m_targetOffsets.begin(), m_targetOffsets.end(), 0.0f);
    std::fill(m_centred.begin(), m_centred.end(), 0.0f);
    auto& wired = m_wired;
    wired.fill(false);
    for (auto& slot : m_slots)
    {
        const int target = slot.target.load(std::memory_order_acquire);
        if (target < 0 || target >= 64)
            continue;
        const int source = juce::jlimit(0, c_numSources - 1, slot.source.load(std::memory_order_relaxed));
        const auto src = static_cast<size_t>(source);
        // Bipolar sources swing +-depth/2 around the knob (depth 100% covers the whole range).
        const float v = bipolar[src] ? 0.5f * values[src] : values[src];
        m_targetOffsets[static_cast<size_t>(target)] += slot.depth.load(std::memory_order_relaxed) * v;
        // Against the source's own middle (0 for an LFO or Random, 0.5 for a Follower or a Macro).
        m_centred[static_cast<size_t>(target)] += slot.depth.load(std::memory_order_relaxed) * (bipolar[src] ? 0.5f * values[src] : values[src] - 0.5f);
        wired[static_cast<size_t>(target)] = true;
    }
    // Swell's wires: each one a gate on its source (see swellGate()); the wires OR together.
    const int swell = swellTarget();
    bool swellOpen = false;
    for (size_t s = 0; s < m_slots.size(); ++s)
    {
        auto& slot = m_slots[s];
        const int target = slot.target.load(std::memory_order_acquire);
        if (target != swell || swell < 0)
        {
            m_gateOpen[s] = false;
            m_gateHeld[s] = 0.0;
            continue;
        }
        const auto src = static_cast<size_t>(juce::jlimit(0, c_numSources - 1, slot.source.load(std::memory_order_relaxed)));
        const float level = bipolar[src] ? 0.5f * (values[src] + 1.0f) : values[src]; // 0..1
        const float depth = slot.depth.load(std::memory_order_relaxed);
        const float amount = std::abs(depth);
        const bool wasOpen = m_gateOpen[s];
        // Positive depth: the top `amount` of the range holds it. Negative: the bottom `amount`.
        const float margin = wasOpen ? c_gateHysteresis : 0.0f;
        const bool inside = depth >= 0.0f ? level > 1.0f - amount - margin : level < amount + margin;
        bool open = amount > 0.0f && inside;
        if (wasOpen && ! open && m_gateHeld[s] < c_gateMinSeconds)
            open = true; // a hold is never shorter than the minimum
        m_gateHeld[s] = open ? (wasOpen ? m_gateHeld[s] + dt : dt) : 0.0;
        m_gateOpen[s] = open;
        swellOpen = swellOpen || open;
    }
    m_swellGate = swellOpen;
    m_swellGateView.store(swellOpen, std::memory_order_relaxed);

    const float a = 1.0f - std::exp(static_cast<float>(-dt) / 0.015f);
    for (size_t t = 0; t < m_offsets.size(); ++t)
    {
        m_offsets[t] += (m_targetOffsets[t] - m_offsets[t]) * a;
        if (! wired[t])
        {
            if (std::abs(m_offsets[t]) < 1.0e-5f)
                m_offsets[t] = 0.0f;
            m_targetView[t].store(-1.0f, std::memory_order_relaxed);
        }
    }
}
