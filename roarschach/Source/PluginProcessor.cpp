#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "Params.h"

namespace
{
    // Host-facing value text. These match the editor's knob readouts, so generic host editors,
    // automation lanes and rack recalls show the same values as the plugin UI.
    float parseNumber(const juce::String& text)
    {
        return text.retainCharacters("0123456789.-").getFloatValue();
    }

    juce::AudioParameterFloatAttributes percent()
    {
        return juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return juce::String(juce::roundToInt(v)) + "%"; })
            .withValueFromStringFunction(parseNumber);
    }

    juce::AudioParameterFloatAttributes semitones()
    {
        return juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int)
            {
                const float rounded = std::round(v * 100.0f) / 100.0f;
                if (std::abs(rounded) < 0.005f)
                    return juce::String("0.00 st");
                return (rounded > 0.0f ? "+" : "") + juce::String(rounded, 2) + " st";
            })
            .withValueFromStringFunction(parseNumber);
    }

    juce::StringArray slotChoices()
    {
        juce::StringArray names;
        for (const auto* name : Params::slotNames)
            names.add(name);
        return names;
    }

    const juce::Identifier c_stateVersion  { "stateVersion" };
    const juce::Identifier c_pluginVersion { "pluginVersion" };

    // The chain's stages hold a little sound after the input stops (the liquid ensemble,
    // resonators, release times); this covers it with room to spare.
    constexpr double c_tailSeconds = 0.3;
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
      , m_presetManager(m_parameters, [this](const juce::AudioProcessorListener::ChangeDetails& details) { m_hostDisplay.post(details); })
{
    m_pitchParam         = m_parameters.getRawParameterValue(Params::pitch);
    m_sizeParam          = m_parameters.getRawParameterValue(Params::size);
    m_growlParam         = m_parameters.getRawParameterValue(Params::growl);
    m_liquidParam        = m_parameters.getRawParameterValue(Params::liquid);
    m_roarParam          = m_parameters.getRawParameterValue(Params::roar);
    m_subParam           = m_parameters.getRawParameterValue(Params::sub);
    m_detailLowParam     = m_parameters.getRawParameterValue(Params::detailLow);
    m_detailHighParam    = m_parameters.getRawParameterValue(Params::detailHigh);
    m_gravelParam        = m_parameters.getRawParameterValue(Params::gravel);
    m_mixParam           = m_parameters.getRawParameterValue(Params::mix);
    m_detailSlotParam    = m_parameters.getRawParameterValue(Params::detailSlot);
    m_vocoderSlotParam   = m_parameters.getRawParameterValue(Params::vocoderSlot);
    m_vocoderOctaveParam = m_parameters.getRawParameterValue(Params::vocoderOctave);
}

juce::AudioProcessorValueTreeState::ParameterLayout RoarschachAudioProcessor::createParameterLayout()
{
    // Append-only: IDs, ranges and order never change, so saved sessions and automation keep
    // mapping to the same parameters (Logic addresses AU parameters by index).
    using Float = juce::AudioParameterFloat;
    using Choice = juce::AudioParameterChoice;
    using Range = juce::NormalisableRange<float>;
    constexpr int v = Params::versionHint;

    const auto percentParam = [](const char* id, const char* name, float defaultValue)
    {
        return std::make_unique<Float>(juce::ParameterID(id, v), name, Range(0.0f, 100.0f, 0.1f), defaultValue, percent());
    };

    juce::StringArray octaves;
    for (const auto* name : Params::octaveNames)
        octaves.add(name);

    return {
        std::make_unique<Float>(juce::ParameterID(Params::pitch, v), "Pitch",
                                Range(Params::pitchMin, Params::pitchMax, 0.01f), Params::pitchDefault, semitones()),
        percentParam(Params::size, "Size", Params::sizeDefault),
        percentParam(Params::growl, "Growl", Params::growlDefault),
        percentParam(Params::liquid, "Liquid", Params::liquidDefault),
        percentParam(Params::roar, "Roar", Params::roarDefault),
        percentParam(Params::sub, "Sub", Params::subDefault),
        percentParam(Params::detailLow, "Detail Low", Params::detailLowDefault),
        percentParam(Params::detailHigh, "Detail High", Params::detailHighDefault),
        percentParam(Params::gravel, "Gravel", Params::gravelDefault),
        percentParam(Params::mix, "Mix", Params::mixDefault),
        std::make_unique<Choice>(juce::ParameterID(Params::detailSlot, v), "Detail Position", slotChoices(), Params::detailSlotDefault),
        std::make_unique<Choice>(juce::ParameterID(Params::vocoderSlot, v), "Vocoder Position", slotChoices(), Params::vocoderSlotDefault),
        std::make_unique<Choice>(juce::ParameterID(Params::vocoderOctave, v), "Vocoder Octave", octaves, Params::vocoderOctaveDefault),
    };
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

    state.setProperty(c_stateVersion, 1, nullptr);
    state.setProperty(c_pluginVersion, JucePlugin_VersionString, nullptr);

    {
        const juce::ScopedLock sl(m_viewLock);
        for (const auto& option : m_viewOptions)
            state.setProperty(option.name, option.value, nullptr);
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
    // Mono or stereo in, mono or stereo out. The core is mono up to LIQUID, which makes it stereo;
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

    m_wetL.assign(c_chunkSize, 0.0f);
    m_wetR.assign(c_chunkSize, 0.0f);

    // The latency is the same for every setting and slot position, so it is reported once here.
    m_chain.prepare(fs, c_chunkSize);
    setLatencySamples(m_chain.getLatencySamples());

    // Dry path: a delay of exactly the reported latency, per channel.
    m_dryDelayLength = std::max(0, getLatencySamples());
    for (auto& channel : m_dryDelay)
        channel.assign(static_cast<size_t>(m_dryDelayLength), 0.0f);
    for (auto& channel : m_dryChunk)
        channel.assign(c_chunkSize, 0.0f);
    for (auto& channel : m_dryMatchedChunk)
        channel.assign(c_chunkSize, 0.0f);
    for (auto& match : m_dryMatch)
        match.prepare(fs);
    m_dryDelayPosition = 0;
    m_bypassFadeStep = static_cast<float>(1.0 / std::max(1.0, c_bypassFadeSeconds * fs));
    m_snapBypassFade = true;
    m_wasBypassed = false;
    m_resetRequestsHandled = m_resetRequests.load(std::memory_order_acquire); // this is a reset already

    m_mixSmoothed.reset(fs, 0.02);
    m_mixSmoothed.setCurrentAndTargetValue(m_mixParam->load() * 0.01f);
    m_dryMatchAmount.reset(fs, 0.02);
    m_dryMatchAmount.setCurrentAndTargetValue(m_mixParam->load() > 0.0f ? 1.0f : 0.0f);
}

void RoarschachAudioProcessor::reset()
{
    // May be called from any thread while audio is running: only count the request.
    m_resetRequests.fetch_add(1, std::memory_order_release);
}

roar::CreatureChain::Settings RoarschachAudioProcessor::readSettings() const
{
    roar::CreatureChain::Settings s;
    s.pitchSt       = m_pitchParam->load();
    s.size          = m_sizeParam->load() * 0.01f;
    s.growl         = m_growlParam->load() * 0.01f;
    s.liquid        = m_liquidParam->load() * 0.01f;
    s.roar          = m_roarParam->load() * 0.01f;
    s.sub           = m_subParam->load() * 0.01f;
    s.detailLow     = m_detailLowParam->load() * 0.01f;
    s.detailHigh    = m_detailHighParam->load() * 0.01f;
    s.gravel        = m_gravelParam->load() * 0.01f;
    s.detailSlot    = juce::jlimit(0, Params::numSlots - 1, juce::roundToInt(m_detailSlotParam->load()));
    s.vocoderSlot   = juce::jlimit(0, Params::numSlots - 1, juce::roundToInt(m_vocoderSlotParam->load()));
    s.vocoderOctave = juce::jlimit(0, 1, juce::roundToInt(m_vocoderOctaveParam->load()));
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
    for (auto& match : m_dryMatch)
        match.reset();
    m_dryDelayPosition = 0;

    m_mixSmoothed.setCurrentAndTargetValue(m_mixParam->load() * 0.01f);
    m_dryMatchAmount.setCurrentAndTargetValue(m_mixParam->load() > 0.0f ? 1.0f : 0.0f);
    m_bypassFade = bypassed ? 1.0f : 0.0f;
}

void RoarschachAudioProcessor::delayDry(const juce::AudioBuffer<float>& buffer, int start, int numSamples, int numInputs, int numOutChannels)
{
    const float* in0 = numInputs > 0 ? buffer.getReadPointer(0, start) : nullptr;
    const float* in1 = numInputs > 1 ? buffer.getReadPointer(1, start) : nullptr;

    for (int ch = 0; ch < numOutChannels; ++ch)
    {
        float* out = m_dryChunk[static_cast<size_t>(ch)].data();
        float* matched = m_dryMatchedChunk[static_cast<size_t>(ch)].data();
        auto& match = m_dryMatch[static_cast<size_t>(ch)];
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

            matched[i] = match.process(out[i]);
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

    // The processed sound fades against the phase-matched dry, which itself hands over to the
    // plain delayed input as the fade completes (weights: processed 1 - w, matched w (1 - w),
    // plain w^2), so the crossfade never passes through a comb and still ends on the exact input.
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* out = buffer.getWritePointer(ch, start);
        const float* dry = m_dryChunk[static_cast<size_t>(ch)].data();
        const float* matched = m_dryMatchedChunk[static_cast<size_t>(ch)].data();

        for (int i = 0; i < numSamples; ++i)
        {
            const float w = m_fadeWeights[static_cast<size_t>(i)];
            if (w >= 1.0f)
                out[i] = dry[i];
            else if (w > 0.0f)
                out[i] = (1.0f - w) * out[i] + w * ((1.0f - w) * matched[i] + w * dry[i]);
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

    // Output channels with a dry path (mono or stereo).
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

    const auto settings = readSettings();
    m_mixSmoothed.setTargetValue(m_mixParam->load() * 0.01f);
    m_dryMatchAmount.setTargetValue(m_mixParam->load() > 0.0f ? 1.0f : 0.0f);

    const float bypassTarget = bypassed ? 1.0f : 0.0f;

    // Process in fixed-size chunks so any host block size works without allocation.
    for (int start = 0; start < numSamples; start += c_chunkSize)
    {
        const int n = std::min(c_chunkSize, numSamples - start);

        // The input through the latency delay, before it is overwritten in place.
        delayDry(buffer, start, n, numInputs, numOutChannels);

        // Chain input: the input channels (a mono input on both); the chain sums them to mono.
        const float* in0 = numInputs > 0 ? buffer.getReadPointer(0, start) : nullptr;
        const float* in1 = numInputs > 1 ? buffer.getReadPointer(1, start) : in0;

        for (int i = 0; i < n; ++i)
        {
            m_wetL[static_cast<size_t>(i)] = in0 != nullptr ? in0[i] : 0.0f;
            m_wetR[static_cast<size_t>(i)] = in1 != nullptr ? in1[i] : 0.0f;
        }

        m_chain.process(m_wetL.data(), m_wetR.data(), n, settings, m_features);

        // MIX: the wet against the dry delayed by the chain's latency (one smoother step per
        // sample, shared by both channels). A mono output takes the fold of the wet.
        float* out0 = buffer.getWritePointer(0, start);
        float* out1 = numOutChannels > 1 ? buffer.getWritePointer(1, start) : nullptr;
        const float* dry0 = m_dryChunk[0].data();
        const float* dry1 = m_dryChunk[1].data();
        const float* matched0 = m_dryMatchedChunk[0].data();
        const float* matched1 = m_dryMatchedChunk[1].data();

        // Weighted as (1 - mix) * dry + mix * wet, with the dry term dropped at full wet: the wet
        // chain sanitises its input, so at Mix 100 a NaN / Inf in the input never reaches the
        // output (dry + mix * (wet - dry) would carry it through). Above Mix 0 the dry is the
        // phase-matched one, so a partial mix does not comb against the wet.
        const auto blend = [](float dry, float matched, float wet, float mix, float matchAmount)
        {
            if (mix >= 1.0f)
                return wet;
            if (matchAmount > 0.0f)
                dry = matchAmount >= 1.0f ? matched : dry + matchAmount * (matched - dry);
            if (mix <= 0.0f)
                return dry;
            return (1.0f - mix) * dry + mix * wet;
        };

        for (int i = 0; i < n; ++i)
        {
            const float mix = m_mixSmoothed.getNextValue();
            const float match = m_dryMatchAmount.getNextValue();
            const float wl = m_wetL[static_cast<size_t>(i)];
            const float wr = m_wetR[static_cast<size_t>(i)];

            if (out1 != nullptr)
            {
                out0[i] = blend(dry0[i], matched0[i], wl, mix, match);
                out1[i] = blend(dry1[i], matched1[i], wr, mix, match);
            }
            else
            {
                out0[i] = blend(dry0[i], matched0[i], 0.5f * (wl + wr), mix, match);
            }
        }

        // Host bypass: crossfade to / from the delayed input.
        applyBypassFade(buffer, start, n, numOutChannels, bypassTarget);
    }

    for (int ch = numOutChannels; ch < std::min(numOutputs, numBufferChannels); ++ch)
        buffer.clear(ch, 0, numSamples);

    m_detailSlotInUse.store(m_chain.getDetailSlotInUse(), std::memory_order_relaxed);
    m_vocoderSlotInUse.store(m_chain.getVocoderSlotInUse(), std::memory_order_relaxed);

    m_wasBypassed = bypassed;
}
