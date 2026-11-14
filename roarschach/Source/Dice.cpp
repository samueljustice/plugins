#include "Dice.h"

//==============================================================================
/** One roll: the state before and after it. The first perform() is the roll itself (already
    applied when the action is made); later ones redo it. */
class DiceRoller::RollAction final : public juce::UndoableAction
{
public:
    RollAction(DiceRoller& owner, State before, State after)
        : m_owner(owner), m_before(std::move(before)), m_after(std::move(after))
    {
        for (size_t i = 0; i < m_before.normalised.size() && i < m_after.normalised.size(); ++i)
            if (!juce::exactlyEqual(m_before.normalised[i], m_after.normalised[i]))
                m_changed.push_back(static_cast<int>(i));
    }

    bool perform() override
    {
        if (m_first)
        {
            m_first = false;
            return true;
        }
        m_owner.restore(m_after, m_before, m_changed);
        return true;
    }

    bool undo() override
    {
        m_owner.restore(m_before, m_after, m_changed);
        return true;
    }

    int getSizeInUnits() override { return static_cast<int>(m_before.normalised.size() + m_after.normalised.size()); }

private:
    DiceRoller& m_owner;
    const State m_before, m_after;
    std::vector<int> m_changed;   // the parameters the roll changed
    bool m_first = true;
};

//==============================================================================
DiceRoller::DiceRoller(juce::AudioProcessorValueTreeState& parameters, PresetManager& presets, HostNotifier notifyHost)
    : m_parameters(parameters), m_presets(presets), m_notifyHost(std::move(notifyHost))
{
    // A different sequence per instance.
    const auto ticks = static_cast<std::uint64_t>(juce::Time::getHighResolutionTicks());
    setSeed(static_cast<std::uint32_t>(juce::Random::getSystemRandom().nextInt()) ^ static_cast<std::uint32_t>(ticks ^ (ticks >> 32)));
}

void DiceRoller::setSeed(std::uint32_t seed)
{
    m_rng.seed(seed);
    m_bag.clear();
    m_lastPick = -1;
}

//==============================================================================
DiceRoller::State DiceRoller::capture() const
{
    State s;
    for (auto* p : m_parameters.processor.getParameters())
        s.normalised.push_back(p->getValue());

    juce::ValueTree tree("DiceState");
    m_presets.writeToState(tree);
    if (const auto xml = tree.createXml())
        s.preset = PresetManager::readStateInfo(*xml);
    return s;
}

juce::String DiceRoller::presetKey(const PresetManager::StateInfo& info)
{
    return juce::String(static_cast<int>(info.kind)) + "|" + info.name + "|" + info.fileName;
}

bool DiceRoller::historyLive() const
{
    // A preset or session loaded since the last roll (even the same preset again), or the preset
    // saved under another name: that sound is not the roll's to undo.
    return m_presets.getLoadGeneration() == m_historyGeneration && presetKey(capture().preset) == m_historyPreset;
}

bool DiceRoller::historyApplies()
{
    if (historyLive())
        return true;
    m_undo.clearUndoHistory();
    return false;
}

void DiceRoller::restore(const State& target, const State& expected, const std::vector<int>& changed)
{
    const auto& params = m_parameters.processor.getParameters();
    const int programBefore = m_presets.getCurrentFactoryIndex();
    auto preset = target.preset;
    m_presets.restoreState(preset, [&]
    {
        for (const int i : changed)
        {
            if (!juce::isPositiveAndBelow(i, params.size()) || i >= static_cast<int>(target.normalised.size())
                || i >= static_cast<int>(expected.normalised.size()))
                continue;
            auto* p = params[i];
            // Turned by hand since the roll (or its redo) left it: the hand edit wins.
            if (!juce::exactlyEqual(p->getValue(), expected.normalised[static_cast<size_t>(i)]))
                continue;
            const float v = target.normalised[static_cast<size_t>(i)];
            if (juce::exactlyEqual(p->getValue(), v))
                continue;
            p->beginChangeGesture();
            p->setValueNotifyingHost(v);
            p->endChangeGesture();
        }
    }, false);

    // Anything edited by hand since the roll stays, so the preset stays marked as edited.
    bool editedSince = false;
    for (int i = 0; i < params.size() && i < static_cast<int>(target.normalised.size()); ++i)
        editedSince = editedSince || !juce::exactlyEqual(params[i]->getValue(), target.normalised[static_cast<size_t>(i)]);
    if (editedSince && !target.preset.modified)
    {
        preset.modified = true;
        m_presets.restoreState(preset, nullptr, false);
    }
    m_historyPreset = presetKey(preset);
    m_historyGeneration = m_presets.getLoadGeneration();

    // The name and the edited mark always change; the host's program only when it really did.
    if (m_notifyHost != nullptr)
        m_notifyHost(juce::AudioProcessorListener::ChangeDetails().withProgramChanged(programBefore != m_presets.getCurrentFactoryIndex())
                                                                  .withNonParameterStateChanged(true));
}

void DiceRoller::applyRealValues(const Params::Random::Values& values)
{
    for (const auto& [id, value] : values)
    {
        auto* p = m_parameters.getParameter(id);
        if (p == nullptr)
            continue;
        const float normalised = p->convertTo0to1(value);
        if (juce::approximatelyEqual(p->getValue(), normalised))
            continue;
        p->beginChangeGesture();
        p->setValueNotifyingHost(normalised);
        p->endChangeGesture();
    }
}

DiceRoller::State DiceRoller::begin()
{
    historyApplies();
    return capture();
}

void DiceRoller::commit(State before, const juce::String& name, bool newFace)
{
    m_undo.beginNewTransaction(name);
    auto after = capture();
    m_historyPreset = presetKey(after.preset);
    m_historyGeneration = m_presets.getLoadGeneration();
    m_undo.perform(new RollAction(*this, std::move(before), std::move(after)));
    m_undo.beginNewTransaction();
    if (newFace)
        m_face = 1 + Params::Random::uniformInt(m_rng, 6);
}

//==============================================================================
bool DiceRoller::load(const std::function<bool()>& loadIt)
{
    auto before = begin();
    if (!loadIt())
        return false;
    commit(std::move(before), "Load", false);
    return true;
}

bool DiceRoller::loadPreset(int index)
{
    return load([this, index] { return m_presets.loadPreset(index); });
}

bool DiceRoller::loadUserPreset(const juce::File& file)
{
    return load([this, &file] { return m_presets.loadUserPreset(file); });
}

bool DiceRoller::loadNext()
{
    return load([this] { return m_presets.loadNext(); });
}

bool DiceRoller::loadPrevious()
{
    return load([this] { return m_presets.loadPrevious(); });
}

//==============================================================================
int DiceRoller::pickPreset(int current)
{
    const int n = PresetManager::getNumFactory();
    const auto eligible = [&](int i) { return i != current && i != m_lastPick; };

    for (int round = 0; round < 2; ++round)
    {
        for (size_t k = 0; k < m_bag.size(); ++k)
        {
            if (eligible(m_bag[k]))
            {
                const int pick = m_bag[k];
                m_bag.erase(m_bag.begin() + static_cast<std::ptrdiff_t>(k));
                m_lastPick = pick;
                return pick;
            }
        }

        // The round is over (or only the preset showing is left in it): a new shuffled round.
        m_bag.clear();
        for (int i = 0; i < n; ++i)
            m_bag.push_back(i);
        for (int i = n - 1; i > 0; --i)
            std::swap(m_bag[static_cast<size_t>(i)], m_bag[static_cast<size_t>(Params::Random::uniformInt(m_rng, i + 1))]);
    }

    // Two presets, the other one picked last time and then changed by hand: anything but the current.
    for (int i = 0; i < n; ++i)
        if (i != current)
        {
            m_lastPick = i;
            return i;
        }
    return -1;
}

int DiceRoller::rollPreset()
{
    auto before = begin();
    const int pick = pickPreset(m_presets.getCurrentFactoryIndex());
    // Factory presets lead the preset list, so a factory index is also its list index.
    if (pick < 0 || !m_presets.loadPreset(pick))
        return -1;
    commit(std::move(before), "Roll Preset");
    return pick;
}

void DiceRoller::rollAll()
{
    auto before = begin();
    Params::Random::Values values;
    Params::Random::rollAll(m_rng, values);
    applyRealValues(values);
    commit(std::move(before), "Roll All");
}

void DiceRoller::rollModule(int module)
{
    if (!juce::isPositiveAndBelow(module, Params::numModules))
        return;
    auto before = begin();
    Params::Random::Values values;
    Params::Random::rollModule(m_rng, module, values);
    applyRealValues(values);
    commit(std::move(before), juce::String("Roll ") + Params::modules[static_cast<size_t>(module)].uiName);
}

bool DiceRoller::canUndo() const
{
    return m_undo.canUndo() && historyLive();
}

bool DiceRoller::canRedo() const
{
    return m_undo.canRedo() && historyLive();
}

bool DiceRoller::undo()
{
    return m_undo.canUndo() && historyApplies() && m_undo.undo();
}

bool DiceRoller::redo()
{
    return m_undo.canRedo() && historyApplies() && m_undo.redo();
}
