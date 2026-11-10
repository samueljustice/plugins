#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "dsp/KnobLaws.h"

namespace
{
    // Host-facing value text. These match the editor's knob readouts, so generic host editors,
    // automation lanes and rack recalls show the same values as the plug-in UI.
    float parseNumber(const juce::String& text)
    {
        return text.retainCharacters("0123456789.-+").getFloatValue();
    }

    juce::String signedText(float v, int decimals, const char* unit, bool plusAtZero)
    {
        const float scale = std::pow(10.0f, static_cast<float>(decimals));
        const float rounded = std::round(v * scale) / scale;
        if (std::abs(rounded) < 0.5f / scale)
            return juce::String(plusAtZero ? "+" : "") + juce::String(0.0f, decimals) + unit;
        return juce::String(rounded > 0.0f ? "+" : "") + juce::String(rounded, decimals) + unit;
    }

    juce::String hertzText(float v)
    {
        if (v >= 1000.0f)
            return juce::String(v / 1000.0f, 2) + " kHz";
        if (v >= 10.0f)
            return juce::String(v, 1) + " Hz";
        return juce::String(v, 2) + " Hz";
    }

    juce::String millisecondsText(float v)
    {
        if (v >= 1000.0f)
            return juce::String(v / 1000.0f, 2) + " s";
        if (v >= 10.0f)
            return juce::String(juce::roundToInt(v)) + " ms";
        if (v >= 1.0f)
            return juce::String(v, 1) + " ms";
        return juce::String(v, 2) + " ms";
    }

    /** The display text of a value (the editor's value boxes and the host show the same). */
    juce::String textFor(const Params::Spec& spec, float v)
    {
        const std::string_view id(spec.id);
        switch (spec.kind)
        {
            case Params::Kind::percent:
                if (id == Params::outClipShape)
                {
                    if (v <= 0.05f)
                        return "Soft";
                    if (v >= 99.95f)
                        return "Hard";
                }
                return juce::String(juce::roundToInt(v)) + "%";
            case Params::Kind::bipolar:
            {
                const int pct = juce::roundToInt(std::abs(v));
                if (pct == 0)
                    return "Even";
                return (v < 0.0f ? "Bass " : "Treble ") + juce::String(pct) + "%";
            }
            case Params::Kind::decibels:
                if (id == Params::outClip && v < 0.05f)
                    return "Off";
                if (id == Params::outClip)
                    return juce::String(v, 1) + " dB";
                return signedText(v, 1, " dB", true);
            case Params::Kind::semitones:   return signedText(v, 2, " st", false);
            case Params::Kind::octaves:     return signedText(v, 2, " oct", true);
            case Params::Kind::hertz:       return hertzText(v);
            case Params::Kind::ratio:       return "x" + juce::String(v, 2);
            case Params::Kind::milliseconds: return millisecondsText(v);
            case Params::Kind::choice:
            case Params::Kind::toggle:
            case Params::Kind::position:
                break;
        }
        return juce::String(v);
    }

    /** The inverse of textFor (typed into a value box or a host field). */
    float valueFor(const Params::Spec& spec, const juce::String& text)
    {
        const auto t = text.trim().toLowerCase();
        const std::string_view id(spec.id);
        switch (spec.kind)
        {
            case Params::Kind::percent:
                if (id == Params::outClipShape && t.startsWith("soft"))
                    return 0.0f;
                if (id == Params::outClipShape && t.startsWith("hard"))
                    return 100.0f;
                return parseNumber(t);
            case Params::Kind::bipolar:
                if (t.startsWith("even"))
                    return 0.0f;
                if (t.startsWith("bass"))
                    return -std::abs(parseNumber(t));
                if (t.startsWith("treble"))
                    return std::abs(parseNumber(t));
                return parseNumber(t);
            case Params::Kind::decibels:
                if (t.startsWith("off"))
                    return 0.0f;
                return parseNumber(t);
            case Params::Kind::hertz:
                return parseNumber(t) * (t.contains("k") ? 1000.0f : 1.0f);
            case Params::Kind::milliseconds:
                return parseNumber(t) * (t.endsWith(" s") || (t.endsWith("s") && !t.endsWith("ms")) ? 1000.0f : 1.0f);
            case Params::Kind::semitones:
            case Params::Kind::octaves:
            case Params::Kind::ratio:
            case Params::Kind::choice:
            case Params::Kind::toggle:
            case Params::Kind::position:
                break;
        }
        return parseNumber(t);
    }

    juce::StringArray choiceNames(const Params::Spec& spec)
    {
        juce::StringArray names;
        for (int i = 0; i < spec.numChoices; ++i)
            names.add(spec.choices[i]);
        return names;
    }

    // 2: the first release. 3: the knob laws of dsp/KnobLaws.h (older states are converted on load).
    const juce::Identifier c_stateVersion  { "stateVersion" };
    constexpr int c_currentStateVersion = 3;
    const juce::Identifier c_pluginVersion { "pluginVersion" };

    // The creature (saved among the view options: setStateInformation keeps only "view..." names).
    // Seed and mode are user-visible changes; the growth values are only written when the host saves.
    const juce::Identifier c_creatureSeed       { "viewCreatureSeed" };
    const juce::Identifier c_creatureMode       { "viewCreatureMode" };
    const juce::Identifier c_creatureEnergy     { "viewCreatureEnergy" };
    const juce::Identifier c_creatureExperience { "viewCreatureExperience" };
    const juce::Identifier c_creatureStage      { "viewCreatureStage" };
    const std::array<juce::Identifier, 4> c_creatureFamily { juce::Identifier { "viewCreatureFluid" }, juce::Identifier { "viewCreatureBeast" },
                                                             juce::Identifier { "viewCreatureEldritch" }, juce::Identifier { "viewCreatureDeep" } };

    // The chain holds sound after the input stops (CHAMBER's rooms ring for up to 0.75 s, with
    // Resonance up to about 1.1 s; the ensemble lines, the body combs, the release times); this
    // covers it with room to spare.
    constexpr double c_tailSeconds = 1.5;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RoarschachAudioProcessor();
}

RoarschachAudioProcessor::RoarschachAudioProcessor()
     : AudioProcessor(BusesProperties()
                       .withInput("Input", juce::AudioChannelSet::stereo(), true)
                       .withOutput("Output", juce::AudioChannelSet::stereo(), true))
      , m_parameters(*this, nullptr, juce::Identifier("RoarschachParameters"), createParameterLayout())
      , m_presetManager(m_parameters, [this](const juce::AudioProcessorListener::ChangeDetails& details) { m_hostDisplay.post(details); }, &m_positionWrites)
      , m_dice(m_parameters, m_presetManager, [this](const juce::AudioProcessorListener::ChangeDetails& details) { m_hostDisplay.post(details); })
{
    for (int i = 0; i < Params::numParameters; ++i)
    {
        m_values[static_cast<size_t>(i)] = m_parameters.getRawParameterValue(Params::all[static_cast<size_t>(i)].id);
        jassert(m_values[static_cast<size_t>(i)] != nullptr);
    }

    // One creature per instance: a new seed (a session load replaces it). Written without telling
    // the host: a fresh instance hasn't changed anything yet.
    const auto seed = static_cast<std::uint32_t>(juce::Random::getSystemRandom().nextInt());
    m_creature.setSeed(seed);
    m_creatureShared.publish(m_creature.evaluate());
    m_viewOptions.set(c_creatureSeed, static_cast<int>(seed));
}

juce::AudioProcessorValueTreeState::ParameterLayout RoarschachAudioProcessor::createParameterLayout()
{
    // The order of Params::all is the AU index order: it never changes once released, so saved
    // sessions and automation keep mapping to the same parameters (Logic addresses AU
    // parameters by index). Each row carries its own version hint: 1 for 1.0's, 2 for the rows
    // appended in 1.1.
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (const auto& spec : Params::all)
    {
        const juce::ParameterID id(spec.id, spec.version);
        switch (spec.kind)
        {
            case Params::Kind::toggle:
                layout.add(std::make_unique<juce::AudioParameterBool>(id, spec.name, spec.defaultValue >= 0.5f));
                break;

            case Params::Kind::choice:
                layout.add(std::make_unique<juce::AudioParameterChoice>(id, spec.name, choiceNames(spec),
                                                                        juce::roundToInt(spec.defaultValue)));
                break;

            case Params::Kind::position:
                if (std::string_view(spec.id) == Params::chamberPos)
                {
                    // CHAMBER's insertion slot: how many of the other 14 modules come before it
                    // (their positions count those 14 only, as in 1.0).
                    layout.add(std::make_unique<juce::AudioParameterInt>(
                        id, spec.name, juce::roundToInt(spec.minValue), juce::roundToInt(spec.maxValue), juce::roundToInt(spec.defaultValue),
                        juce::AudioParameterIntAttributes()
                            .withStringFromValueFunction([](int p, int)
                            {
                                const int slot = juce::jlimit(0, Params::numLegacyModules, p);
                                return slot == 0 ? juce::String("before 1st")
                                                 : "after " + juce::String(Params::positionNames[static_cast<size_t>(slot - 1)]);
                            })
                            .withValueFromStringFunction([max = juce::roundToInt(spec.maxValue)](const juce::String& text)
                            {
                                return text.trim().startsWithIgnoreCase("before") ? 0
                                                                                  : juce::jlimit(0, max, text.retainCharacters("0123456789").getIntValue());
                            })));
                    break;
                }
                layout.add(std::make_unique<juce::AudioParameterInt>(
                    id, spec.name, juce::roundToInt(spec.minValue), juce::roundToInt(spec.maxValue), juce::roundToInt(spec.defaultValue),
                    juce::AudioParameterIntAttributes()
                        .withStringFromValueFunction([](int p, int)
                        {
                            return juce::String(Params::positionNames[static_cast<size_t>(juce::jlimit(0, Params::numModules - 1, p))]);
                        })
                        .withValueFromStringFunction([max = juce::roundToInt(spec.maxValue)](const juce::String& text)
                        {
                            return juce::jlimit(0, max, text.getIntValue() - 1);
                        })));
                break;

            case Params::Kind::percent:
            case Params::Kind::bipolar:
            case Params::Kind::decibels:
            case Params::Kind::semitones:
            case Params::Kind::octaves:
            case Params::Kind::hertz:
            case Params::Kind::ratio:
            case Params::Kind::milliseconds:
            {
                const Params::Spec* s = &spec;   // points into the constexpr table: lives forever
                juce::NormalisableRange<float> range(spec.minValue, spec.maxValue, spec.step);
                // The log kinds (hertz, ratio, milliseconds) get a true log taper: equal travel per
                // octave, the geometric centre at mid travel. Decibel knobs with a centre are skewed
                // so it sits at mid travel (Push, Clip). Everything else is linear, even where 0 lies
                // inside an asymmetric range (Pitch, -24 to +12 st).
                if (Params::hasLogTaper(spec))
                {
                    range = juce::NormalisableRange<float>(
                        spec.minValue, spec.maxValue,
                        [s](float, float, float n) { return Params::logTaperValue(*s, n); },
                        [s](float, float, float v) { return Params::logTaperPosition(*s, v); },
                        [s](float, float, float v)
                        {
                            v = juce::jlimit(s->minValue, s->maxValue, v);
                            return s->step > 0.0f ? juce::jlimit(s->minValue, s->maxValue, s->step * std::round(v / s->step)) : v;
                        });
                }
                else if (spec.kind == Params::Kind::decibels && spec.centre > spec.minValue && spec.centre < spec.maxValue)
                {
                    range.setSkewForCentre(spec.centre);
                }
                layout.add(std::make_unique<juce::AudioParameterFloat>(
                    id, spec.name, range, spec.defaultValue,
                    juce::AudioParameterFloatAttributes()
                        .withStringFromValueFunction([s](float value, int) { return textFor(*s, value); })
                        .withValueFromStringFunction([s](const juce::String& text) { return valueFor(*s, text); })));
                break;
            }
        }
    }

    return layout;
}

juce::AudioProcessorEditor* RoarschachAudioProcessor::createEditor()
{
    return new RoarschachAudioProcessorEditor(*this);
}

bool RoarschachAudioProcessor::hasEditor() const
{
    return true;
}

const juce::String RoarschachAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool RoarschachAudioProcessor::acceptsMidi() const
{
    return false;
}

bool RoarschachAudioProcessor::producesMidi() const
{
    return false;
}

bool RoarschachAudioProcessor::isMidiEffect() const
{
    return false;
}

double RoarschachAudioProcessor::getTailLengthSeconds() const
{
    return c_tailSeconds;
}

// Host programs: the factory presets, plus a last "User Preset" program for every other sound
// (user presets, unsaved sounds). User presets are not listed one by one, because hosts cache
// the program list and it would go stale as files change. Without the extra entry, every
// non-factory sound would report program 0, and a VST3 host could then never select "Init":
// JUCE only calls setCurrentProgram when the program changes.
int RoarschachAudioProcessor::getNumPrograms()
{
    return PresetManager::getNumFactory() + 1;
}

int RoarschachAudioProcessor::getCurrentProgram()
{
    const int factoryIndex = m_presetManager.getCurrentFactoryIndex();
    return factoryIndex >= 0 ? factoryIndex : PresetManager::getNumFactory();
}

void RoarschachAudioProcessor::setCurrentProgram(int index)
{
    // Applied right away on the calling thread (hosts may call this from any thread, and JUCE's
    // AU wrapper does so synchronously for kAudioUnitProperty_PresentPreset), so the program
    // and the state reflect it as soon as this returns. "User Preset" itself does nothing.
    if (juce::isPositiveAndBelow(index, PresetManager::getNumFactory()))
        m_presetManager.loadFactoryPresetForHost(index);
    else if (index == PresetManager::getNumFactory() && getCurrentProgram() != index)
        // "User Preset" can't be chosen while a factory preset is current: tell the host, so its
        // program display snaps back. (JUCE's VST3 wrapper then sets its Program parameter to
        // getCurrentProgram(), which does not call setCurrentProgram again, so nothing reloads.)
        m_hostDisplay.post(juce::AudioProcessorListener::ChangeDetails().withProgramChanged(true));
}

const juce::String RoarschachAudioProcessor::getProgramName(int index)
{
    // Names come from the immutable factory table, never from the (refreshable) preset list.
    if (juce::isPositiveAndBelow(index, PresetManager::getNumFactory()))
        return PresetManager::getFactoryPresetName(index);

    if (index == PresetManager::getNumFactory())
        return "User Preset";

    return {};
}

//==============================================================================
roar::CreatureEvolution::Inputs RoarschachAudioProcessor::readCreatureInputs()
{
    const auto& f = m_features;
    const auto get = [](const std::atomic<float>& a) { return a.load(std::memory_order_relaxed); };

    // The host stops calling processBlock when the transport stops: no new chunk for half a second
    // counts as silence, and the creature cools slowly (nothing resets).
    const int generation = f.generation.load(std::memory_order_relaxed);
    if (generation != m_creatureGeneration)
    {
        m_creatureGeneration = generation;
        m_creatureStaleSeconds = 0.0f;
    }

    roar::CreatureEvolution::Inputs in;
    in.stale = m_creatureStaleSeconds >= 0.5f;
    in.inputLevel = get(f.inputLevel);
    in.outputLevel = get(f.outputLevel);
    in.jaw = get(f.jaw);
    in.roarHeat = get(f.roarHeat);
    in.growlMod = get(f.growlMod);
    in.subLevel = get(f.subLevel);
    in.limiterGrDb = get(f.limiterGrDb);
    in.clipDb = get(f.clipDb);
    in.gravelActive = get(f.gravelActive);
    float bands = 0.0f;
    for (const auto& b : f.vocoderBands)
        bands += get(b);
    in.meanBand = bands / static_cast<float>(f.vocoderBands.size());
    in.ringAmount = get(f.ringAmount);
    in.overtoneLevel = get(f.overtoneLevel);
    in.ensembleWidth = get(f.ensembleWidth);
    in.pitchSt = get(f.pitchSt);
    const int mask = f.moduleOnMask.load(std::memory_order_relaxed);
    const auto on = [mask](Params::ModuleId id) { return (mask >> static_cast<int>(id)) & 1 ? 1.0f : 0.0f; };
    in.shimmerOn = on(Params::ModuleId::shimmer);
    in.swirlOn = on(Params::ModuleId::swirl);
    in.liquidKnob = in.shimmerOn * static_cast<float>(roar::laws::view::shimmer(static_cast<double>(percent(Params::shimmer))));
    in.eggVisible = m_creatureShared.eggVisible.load(std::memory_order_relaxed);
    return in;
}

void RoarschachAudioProcessor::stepCreature(float dt)
{
    const bool wasStale = m_creatureStaleSeconds >= 0.5f;
    auto in = readCreatureInputs();
    m_creatureStaleSeconds = std::min(m_creatureStaleSeconds + dt, 60.0f);
    in.stale = in.stale || wasStale;

    // Published under the lock, like every other publish: a state loaded on another thread between
    // the evaluate and the publish can never be overwritten by these older values.
    const juce::ScopedLock sl(m_creatureLock);
    m_creature.step(dt, in);
    m_creatureShared.publish(m_creature.evaluate());
}

void RoarschachAudioProcessor::CreatureClock::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float dt = static_cast<float>((now - m_last) * 0.001);
    m_last = now;
    m_processor.stepCreature(dt);   // (clamped to half a second inside: a stalled thread doesn't jump it)
}

bool RoarschachAudioProcessor::isCreatureFrozen() const
{
    const juce::ScopedLock sl(m_creatureLock);
    return m_creature.getMode() == roar::CreatureEvolution::Mode::frozen;
}

void RoarschachAudioProcessor::setCreatureFrozen(bool frozen)
{
    {
        const juce::ScopedLock sl(m_creatureLock);
        m_creature.setMode(frozen ? roar::CreatureEvolution::Mode::frozen : roar::CreatureEvolution::Mode::live);
    }
    setViewValue(c_creatureMode, frozen ? "frozen" : "live");
}

void RoarschachAudioProcessor::resetCreature()
{
    const auto seed = static_cast<std::uint32_t>(juce::Random::getSystemRandom().nextInt());
    {
        const juce::ScopedLock sl(m_creatureLock);
        m_creature.reset(seed);
        m_creatureShared.publish(m_creature.evaluate());
    }
    setViewValue(c_creatureSeed, static_cast<int>(seed));
}

void RoarschachAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName); // factory programs are read-only
}

//==============================================================================
void RoarschachAudioProcessor::HostDisplayUpdater::post(const juce::AudioProcessorListener::ChangeDetails& details)
{
    const int flags = (details.programChanged ? c_programChanged : 0)
                    | (details.nonParameterStateChanged ? c_stateChanged : 0);

    if (flags == 0)
        return;

    // Always delivered from the message thread, after the current call returns: the JUCE 8
    // wrappers expect updateHostDisplay there (the VST3 one reads its Program parameter), and
    // it is never re-entered from inside a host call. Bursts (an editor resize) coalesce.
    m_flags.fetch_or(flags);
    triggerAsyncUpdate();
}

void RoarschachAudioProcessor::HostDisplayUpdater::handleAsyncUpdate()
{
    const int flags = m_flags.exchange(0);

    if (flags != 0)
        m_processor.updateHostDisplay(juce::AudioProcessorListener::ChangeDetails()
                                          .withProgramChanged((flags & c_programChanged) != 0)
                                          .withNonParameterStateChanged((flags & c_stateChanged) != 0));
}

//==============================================================================
void RoarschachAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // Everything needed for a full recall: every parameter, the current preset and its edited
    // flag, and the editor's view options. Built on a private copy of the tree.
    juce::ValueTree state = m_parameters.copyState();
    state.removeAllProperties(nullptr);

    // APVTS only writes a value once a parameter has changed; write every one, so a session
    // keeps its exact sound even if a later version changes a default.
    for (auto child : state)
    {
        if (!child.hasType("PARAM") || child.hasProperty("value"))
            continue;

        if (auto* value = m_parameters.getRawParameterValue(child.getProperty("id").toString()))
            child.setProperty("value", value->load(), nullptr);
    }

    state.setProperty(c_stateVersion, c_currentStateVersion, nullptr);
    state.setProperty(c_pluginVersion, JucePlugin_VersionString, nullptr);

    {
        const juce::ScopedLock sl(m_viewLock);
        for (const auto& option : m_viewOptions)
            state.setProperty(option.name, option.value, nullptr);
    }

    // The creature's growth is carried along whenever the host saves, without ever telling the
    // host the session changed (it changes all the time). Quantised so loads are stable.
    {
        roar::CreatureEvolution::Persisted creature;
        {
            const juce::ScopedLock sl(m_creatureLock);
            creature = m_creature.getPersisted();
        }
        const auto q = [](float v) { return std::round(static_cast<double>(v) * 1.0e4) / 1.0e4; };
        state.setProperty(c_creatureEnergy, q(creature.energy), nullptr);
        state.setProperty(c_creatureExperience, q(creature.experience), nullptr);
        state.setProperty(c_creatureStage, q(creature.stage), nullptr);
        for (size_t k = 0; k < creature.family.size(); ++k)
            state.setProperty(c_creatureFamily[k], q(creature.family[k]), nullptr);
    }

    m_presetManager.writeToState(state);

    if (std::unique_ptr<juce::XmlElement> xml { state.createXml() })
        copyXmlToBinary(*xml, destData);
}

void RoarschachAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState == nullptr || !xmlState->hasTagName(m_parameters.state.getType()))
        return;

    // Everything that isn't a parameter is read from the XML (a private copy) first.
    const auto presetInfo = PresetManager::readStateInfo(*xmlState);

    juce::NamedValueSet viewOptions;
    for (int i = 0; i < xmlState->getNumAttributes(); ++i)
    {
        const auto name = xmlState->getAttributeName(i);
        const auto value = xmlState->getAttributeValue(i);

        // A non-finite number ("nan", "inf") counts as missing, so the editor uses its default.
        if (name.startsWith("view") && std::isfinite(value.getDoubleValue()))
            viewOptions.set(name, value);
    }

    // The creature: its growth goes to the evolution (not the view options), the seed and the
    // mode stay with the view options too. A state without a creature keeps this one.
    {
        roar::CreatureEvolution::Persisted creature;
        bool haveCreature = false;
        const auto take = [&viewOptions, &haveCreature](const juce::Identifier& id, float& out)
        {
            if (const auto* v = viewOptions.getVarPointer(id))
            {
                out = juce::jlimit(0.0f, 1.0f, static_cast<float>(v->toString().getDoubleValue()));
                haveCreature = true;
            }
            viewOptions.remove(id);
        };
        take(c_creatureEnergy, creature.energy);
        take(c_creatureExperience, creature.experience);
        take(c_creatureStage, creature.stage);
        for (size_t k = 0; k < creature.family.size(); ++k)
            take(c_creatureFamily[k], creature.family[k]);

        const juce::ScopedLock sl(m_creatureLock);
        if (haveCreature)
            m_creature.setPersisted(creature);
        if (const auto* seed = viewOptions.getVarPointer(c_creatureSeed))
            m_creature.setSeed(static_cast<std::uint32_t>(seed->toString().getLargeIntValue()));
        else
            viewOptions.set(c_creatureSeed, static_cast<int>(m_creature.getSeed()));
        if (const auto* mode = viewOptions.getVarPointer(c_creatureMode))
            m_creature.setMode(mode->toString() == "frozen" ? roar::CreatureEvolution::Mode::frozen : roar::CreatureEvolution::Mode::live);
        m_creatureShared.publish(m_creature.evaluate());
    }

    {
        const juce::ScopedLock sl(m_viewLock);
        m_viewOptions = std::move(viewOptions);
    }

    // The parameter tree: PARAM children only. Non-finite values are dropped (that parameter
    // takes its default), and parameters a state lacks get their defaults.
    auto tree = juce::ValueTree::fromXml(*xmlState);
    tree.removeAllProperties(nullptr);

    for (auto child : tree)
        if (child.hasProperty("value") && !std::isfinite(static_cast<double>(child.getProperty("value"))))
            child.removeProperty("value", nullptr);

    // A state saved with the first release's knob laws: convert its values to the nearest setting under
    // the new laws (close, not identical: see migrateFromV1 for what cannot be carried over).
    if (xmlState->getIntAttribute(c_stateVersion.toString(), 0) < c_currentStateVersion)
    {
        std::map<std::string, double> values;
        for (auto child : tree)
            if (child.hasProperty("value"))
                values[child.getProperty("id").toString().toStdString()] = static_cast<double>(child.getProperty("value"));
        roar::laws::migrateFromV1(values);
        for (auto child : tree)
        {
            const auto it = values.find(child.getProperty("id").toString().toStdString());
            if (it != values.end() && child.hasProperty("value"))
                child.setProperty("value", it->second, nullptr);
        }
    }

    // APVTS::replaceState leaves a parameter without a PARAM child at its current value, so a
    // partial or older state would recall differently depending on what this instance held.
    // Give every missing parameter an explicit default instead.
    static const juce::Identifier paramType("PARAM"), idProperty("id"), valueProperty("value");
    for (const auto& spec : Params::all)
        if (!tree.getChildWithProperty(idProperty, juce::String(spec.id)).isValid())
            tree.appendChild(juce::ValueTree(paramType, { { idProperty, juce::String(spec.id) },
                                                          { valueProperty, static_cast<double>(spec.defaultValue) } }),
                             nullptr);

    m_presetManager.restoreState(presetInfo, [this, &tree] { m_parameters.replaceState(tree); });
}

bool RoarschachAudioProcessor::getViewOption(const juce::Identifier& option, bool defaultValue) const
{
    const juce::ScopedLock sl(m_viewLock);

    if (const auto* value = m_viewOptions.getVarPointer(option))
        return static_cast<bool>(*value);

    return defaultValue;
}

void RoarschachAudioProcessor::setViewOption(const juce::Identifier& option, bool value)
{
    setViewValue(option, value);
}

juce::var RoarschachAudioProcessor::getViewValue(const juce::Identifier& option, const juce::var& defaultValue) const
{
    const juce::ScopedLock sl(m_viewLock);
    return m_viewOptions.getWithDefault(option, defaultValue);
}

void RoarschachAudioProcessor::setViewValue(const juce::Identifier& option, const juce::var& value)
{
    if ((value.isDouble() || value.isInt() || value.isInt64()) && !std::isfinite(static_cast<double>(value)))
        return;

    {
        const juce::ScopedLock sl(m_viewLock);

        // Compared as text: values read back from a state are strings.
        if (const auto* existing = m_viewOptions.getVarPointer(option); existing != nullptr && existing->toString() == value.toString())
            return;

        m_viewOptions.set(option, value);
    }

    // Saved with the session, so the host should see the session as changed.
    m_hostDisplay.post(juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged(true));
}

bool RoarschachAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Mono or stereo in, mono or stereo out. The core is mono up to the first module that makes stereo;
    // a mono output gets the fold of the two.
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const auto monoOrStereo = [](const juce::AudioChannelSet& set)
    {
        return set == juce::AudioChannelSet::mono() || set == juce::AudioChannelSet::stereo();
    };

    return monoOrStereo(in) && monoOrStereo(out);
}


void RoarschachAudioProcessor::releaseResources()
{
}

void RoarschachAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);

    const double fs = sampleRate > 0.0 ? sampleRate : 44100.0;

    for (auto* buffer : { &m_inL, &m_inR, &m_wetL, &m_wetR })
        buffer->assign(c_chunkSize, 0.0f);

    // The latency is the same for every setting and every order, so it is reported once here.
    m_chain.prepare(fs, c_chunkSize);
    setLatencySamples(m_chain.getLatencySamples());

    // Bypass path: a delay of exactly the reported latency, per channel.
    m_dryDelayLength = std::max(0, getLatencySamples());
    for (auto& channel : m_dryDelay)
        channel.assign(static_cast<size_t>(m_dryDelayLength), 0.0f);
    for (auto& channel : m_dryChunk)
        channel.assign(c_chunkSize, 0.0f);
    m_dryDelayPosition = 0;
    m_bypassFadeStep = static_cast<float>(1.0 / std::max(1.0, c_bypassFadeSeconds * fs));
    m_snapBypassFade = true;
    m_wasBypassed = false;
    m_resetRequestsHandled = m_resetRequests.load(std::memory_order_acquire); // this is a reset already
}

void RoarschachAudioProcessor::reset()
{
    // May be called from any thread while audio is running: only count the request.
    m_resetRequests.fetch_add(1, std::memory_order_release);
}

float RoarschachAudioProcessor::valueAt(int index) const
{
    const auto i = static_cast<size_t>(index);
    const float v = m_values[i]->load(std::memory_order_relaxed);
    return std::isfinite(v) ? v : Params::all[i].defaultValue;
}

int RoarschachAudioProcessor::choice(ParamIndex p) const
{
    const auto& spec = Params::all[static_cast<size_t>(p.index)];
    return juce::jlimit(juce::roundToInt(spec.minValue), juce::roundToInt(spec.maxValue), juce::roundToInt(valueAt(p.index)));
}

roar::ChainSettings RoarschachAudioProcessor::readSettings() const
{
    using namespace Params;
    ::roar::ChainSettings s;

    s.in.trimDb = value(inTrim);
    s.in.autoLevel = toggle(inAuto);

    s.pitch.on = toggle(pitchOn);
    s.pitch.pitchSemitones = value(pitch);
    s.pitch.size = percent(pitchSize);
    s.pitch.drive = percent(pitchDrive);
    s.pitch.mode = static_cast<PitchMode>(choice(pitchMode));
    s.pitch.formant = static_cast<PitchFormant>(choice(pitchFormant));

    s.clean.on = toggle(cleanOn);
    s.clean.contain = percent(cleanContain);
    s.clean.gate = percent(cleanGate);
    s.clean.tone = static_cast<CleanTone>(choice(cleanTone));

    s.deess.on = toggle(deessOn);
    s.deess.amount = percent(deess);
    s.deess.mode = static_cast<DeessMode>(choice(deessMode));

    s.detail.on = toggle(detailOn);
    s.detail.low = percent(detailLow);
    s.detail.high = percent(detailHigh);
    s.detail.speed = static_cast<DetailSpeed>(choice(detailSpeed));

    s.vocoder.on = toggle(vocoderOn);
    s.vocoder.gravel = percent(gravel);
    s.vocoder.octave = static_cast<VocoderOctave>(choice(vocoderOctave));

    s.ring.on = toggle(ringOn);
    s.ring.amount = percent(ring);
    s.ring.freqHz = value(ringFreq);
    s.ring.ratio = value(ringRatio);
    s.ring.carrier = static_cast<::roar::RingCarrier>(choice(ringCarrier));
    s.ring.track = static_cast<::roar::RingTrack>(choice(ringTrack));
    s.ring.mode = static_cast<::roar::RingMode>(choice(ringMode));
    s.ring.body = static_cast<::roar::RingBody>(choice(ringBody));

    s.shimmer.on = toggle(shimmerOn);
    s.shimmer.amount = percent(shimmer);
    s.shimmer.rateHz = value(shimmerRate);
    s.shimmer.mode = static_cast<ShimmerMode>(choice(shimmerMode));

    s.growl.on = toggle(growlOn);
    s.growl.depth = percent(growl);
    s.growl.rateHz = value(growlRate);
    s.growl.shape = static_cast<GrowlShape>(choice(growlShape));
    s.growl.character = static_cast<GrowlCharacter>(choice(growlCharacter));

    s.swirl.on = toggle(swirlOn);
    s.swirl.amount = percent(swirl);
    s.swirl.rateHz = value(swirlRate);
    s.swirl.circuit = static_cast<SwirlCircuit>(choice(swirlCircuit));
    s.swirl.motion = static_cast<SwirlMotion>(choice(swirlMotion));

    s.roar.on = toggle(roarOn);
    s.roar.drive = percent(Params::roar);
    s.roar.tone = percent(roarTone);
    s.roar.voicing = static_cast<RoarVoicing>(choice(roarVoicing));

    s.ensemble.on = toggle(ensembleOn);
    s.ensemble.amount = percent(ensemble);
    s.ensemble.width = percent(ensembleWidth);
    s.ensemble.voices = static_cast<EnsembleVoices>(choice(ensembleVoices));
    s.ensemble.motion = static_cast<EnsembleMotion>(choice(ensembleMotion));

    s.tame.on = toggle(tameOn);
    s.tame.amount = percent(tame);

    s.sub.on = toggle(subOn);
    s.sub.level = percent(sub);
    s.sub.drive = percent(subDrive);
    s.sub.reg = static_cast<SubRegister>(choice(subRegister));
    s.sub.character = static_cast<SubCharacter>(choice(subCharacter));

    s.overtone.on = toggle(overtoneOn);
    s.overtone.amount = percent(overtone);
    s.overtone.freqOct = value(overtoneFreq);
    s.overtone.pattern = static_cast<::roar::OvertonePattern>(choice(overtonePattern));
    s.overtone.mode = static_cast<::roar::OvertoneMode>(choice(overtoneMode));

    s.chamber.on = toggle(chamberOn);
    s.chamber.amount = percent(chamber);
    s.chamber.resonance = percent(chamberResonance);
    s.chamber.edge = percent(chamberDrive);
    s.chamber.width = percent(chamberWidth);
    s.chamber.room = choice(chamberRoom);
    s.chamber.speaker = choice(chamberSpeaker);
    s.chamber.boom = choice(chamberBoom);

    s.output.pushDb = value(outPush);
    s.output.ceilingDb = value(outCeiling);
    s.output.clipDb = value(outClip);
    s.output.clipShape = percent(outClipShape);
    s.output.attackMs = value(outAttack);
    s.output.releaseMs = value(outRelease);
    s.output.autoRelease = toggle(outAuto);
    s.output.mix = percent(mix);

    static constexpr auto positionIndices = []
    {
        std::array<int, numModules> a {};
        for (int m = 0; m < numModules; ++m)
            a[static_cast<size_t>(m)] = indexOf(modules[static_cast<size_t>(m)].posId);
        return a;
    }();
    for (int m = 0; m < numModules; ++m)
        s.positions[static_cast<size_t>(m)] = juce::roundToInt(valueAt(positionIndices[static_cast<size_t>(m)]));

    return s;
}

void RoarschachAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    render(buffer, false);
}

void RoarschachAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    render(buffer, true);
}

void RoarschachAudioProcessor::handlePendingReset(bool bypassed)
{
    const auto requests = m_resetRequests.load(std::memory_order_acquire);
    const auto pending = requests - m_resetRequestsHandled;

    if (pending == 0)
        return;

    m_resetRequestsHandled = requests;

    // JUCE's AU wrapper resets the plug-in once each time bypass is switched on. That is not a
    // transport jump: the processed sound is fading out and keeps running underneath, and the
    // delayed input must continue without a gap. So one reset arriving together with entering
    // bypass is ignored; any further one (e.g. the host's own reset at transport start, after
    // bypass was switched on while stopped) is a real reset.
    if (bypassed && !m_wasBypassed && pending == 1)
        return;

    m_chain.reset();

    for (auto& channel : m_dryDelay)
        std::fill(channel.begin(), channel.end(), 0.0f);
    m_dryDelayPosition = 0;
    m_bypassFade = bypassed ? 1.0f : 0.0f;
}

void RoarschachAudioProcessor::delayDry(const juce::AudioBuffer<float>& buffer, int start, int numSamples, int numInputs, int numOutChannels)
{
    const float* in0 = numInputs > 0 ? buffer.getReadPointer(0, start) : nullptr;
    const float* in1 = numInputs > 1 ? buffer.getReadPointer(1, start) : nullptr;

    for (int ch = 0; ch < numOutChannels; ++ch)
    {
        float* out = m_dryChunk[static_cast<size_t>(ch)].data();
        float* ring = m_dryDelay[static_cast<size_t>(ch)].data();
        int position = m_dryDelayPosition;

        for (int i = 0; i < numSamples; ++i)
        {
            // Stereo out: each input channel (a mono input on both). Mono out: the mono fold.
            float x = 0.0f;
            if (numOutChannels == 1)
                x = in1 != nullptr ? 0.5f * (in0[i] + in1[i]) : (in0 != nullptr ? in0[i] : 0.0f);
            else
                x = ch == 1 && in1 != nullptr ? in1[i] : (in0 != nullptr ? in0[i] : 0.0f);

            if (m_dryDelayLength == 0)
            {
                out[i] = x;
            }
            else
            {
                out[i] = ring[position];
                ring[position] = x;

                if (++position == m_dryDelayLength)
                    position = 0;
            }
        }
    }

    if (m_dryDelayLength > 0)
        m_dryDelayPosition = (m_dryDelayPosition + numSamples) % m_dryDelayLength;
}

void RoarschachAudioProcessor::applyBypassFade(juce::AudioBuffer<float>& buffer, int start, int numSamples, int numChannels, float target)
{
    if (juce::exactlyEqual(m_bypassFade, target))
    {
        // Settled. Bypassed: the delayed input, bit for bit. Active: the processed output as is.
        if (target >= 1.0f)
            for (int ch = 0; ch < numChannels; ++ch)
                juce::FloatVectorOperations::copy(buffer.getWritePointer(ch, start), m_dryChunk[static_cast<size_t>(ch)].data(), numSamples);
        return;
    }

    // Fading: one weight per sample, shared by every channel (smoothstep of a linear ramp).
    float fade = m_bypassFade;
    for (int i = 0; i < numSamples; ++i)
    {
        fade = target > fade ? std::min(target, fade + m_bypassFadeStep) : std::max(target, fade - m_bypassFadeStep);
        m_fadeWeights[static_cast<size_t>(i)] = fade * fade * (3.0f - 2.0f * fade);
    }
    m_bypassFade = fade;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* out = buffer.getWritePointer(ch, start);
        const float* dry = m_dryChunk[static_cast<size_t>(ch)].data();

        for (int i = 0; i < numSamples; ++i)
        {
            const float w = m_fadeWeights[static_cast<size_t>(i)];
            if (w >= 1.0f)
                out[i] = dry[i];
            else if (w > 0.0f)
                out[i] = (1.0f - w) * out[i] + w * dry[i];
        }
    }
}

void RoarschachAudioProcessor::render(juce::AudioBuffer<float>& buffer, bool bypassed)
{
    juce::ScopedNoDenormals noDenormals;

    const int numInputs  = std::min(getTotalNumInputChannels(), buffer.getNumChannels());
    const int numOutputs = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();
    const int numBufferChannels = buffer.getNumChannels();

    // Output channels with a signal (mono or stereo).
    const int numOutChannels = std::min({ numOutputs, numBufferChannels, c_maxChannels });

    if (m_wetL.size() < static_cast<size_t>(c_chunkSize))
    {
        buffer.clear(); // not prepared (hosts must call prepareToPlay first)
        return;
    }

    handlePendingReset(bypassed);

    if (m_snapBypassFade)
    {
        m_snapBypassFade = false;
        m_bypassFade = bypassed ? 1.0f : 0.0f;
    }

    if (numOutChannels <= 0 || numSamples <= 0)
    {
        m_wasBypassed = bypassed;
        return;
    }

    auto settings = readSettings();
    // While the editor or a preset is writing the positions one by one, keep the last complete
    // order (the chain also waits for a requested order to settle before it fades).
    if (m_positionWrites.inProgress() && m_haveHeldPositions)
        settings.positions = m_heldPositions;
    else
    {
        m_heldPositions = settings.positions;
        m_haveHeldPositions = true;
    }
    const float bypassTarget = bypassed ? 1.0f : 0.0f;

    // Process in fixed-size chunks so any host block size works without allocation.
    for (int start = 0; start < numSamples; start += c_chunkSize)
    {
        const int n = std::min(c_chunkSize, numSamples - start);

        // The input through the latency delay (for the bypass), before it is overwritten in place.
        delayDry(buffer, start, n, numInputs, numOutChannels);

        const float* in0 = numInputs > 0 ? buffer.getReadPointer(0, start) : nullptr;
        const float* in1 = numInputs > 1 ? buffer.getReadPointer(1, start) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            m_inL[static_cast<size_t>(i)] = in0 != nullptr ? in0[i] : 0.0f;
            m_inR[static_cast<size_t>(i)] = in1 != nullptr ? in1[i] : m_inL[static_cast<size_t>(i)];
        }

        // The whole chain, Mix included (it sits before the maximizer, so the ceiling holds at any Mix).
        m_chain.process(m_inL.data(), m_inR.data(), in1 != nullptr, m_wetL.data(), m_wetR.data(), n, settings, m_features);

        float* out0 = buffer.getWritePointer(0, start);
        float* out1 = numOutChannels > 1 ? buffer.getWritePointer(1, start) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float wl = m_wetL[static_cast<size_t>(i)];
            const float wr = m_wetR[static_cast<size_t>(i)];
            if (out1 != nullptr)
            {
                out0[i] = wl;
                out1[i] = wr;
            }
            else
            {
                out0[i] = 0.5f * (wl + wr);
            }
        }

        // Host bypass: crossfade to / from the delayed input.
        applyBypassFade(buffer, start, n, numOutChannels, bypassTarget);
    }

    for (int ch = numOutChannels; ch < std::min(numOutputs, numBufferChannels); ++ch)
        buffer.clear(ch, 0, numSamples);

    m_wasBypassed = bypassed;
}
