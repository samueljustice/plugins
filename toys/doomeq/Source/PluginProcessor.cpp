#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DoomAudio.h"

namespace
{
    constexpr eq::Type defaultTypes[DoomEQProcessor::numBands] = {
        eq::Type::LowShelf, eq::Type::Bell, eq::Type::Bell, eq::Type::Bell, eq::Type::Bell, eq::Type::HighShelf
    };
    constexpr float defaultFreqs[DoomEQProcessor::numBands] = { 80.0f, 250.0f, 800.0f, 2500.0f, 6000.0f, 12000.0f };
    constexpr float defaultQs[DoomEQProcessor::numBands]    = { 0.707f, 1.0f, 1.0f, 1.0f, 1.0f, 0.707f };

    juce::NormalisableRange<float> logRange (float lo, float hi)
    {
        return { lo, hi,
                 [] (float start, float end, float x) { return start * std::pow (end / start, x); },
                 [] (float start, float end, float v) { return std::log (v / start) / std::log (end / start); },
                 [] (float, float, float v) { return v; } };
    }

    juce::String freqToString (float v, int)
    {
        if (v < 1000.0f)
            return juce::String (v, v < 100.0f ? 1 : 0) + " Hz";
        return juce::String (v / 1000.0f, v < 10000.0f ? 2 : 1) + " kHz";
    }

    float stringToFreq (const juce::String& s)
    {
        auto t = s.trim().toLowerCase();
        float v = t.getFloatValue();
        if (t.containsChar ('k'))
            v *= 1000.0f;
        return v;
    }

    juce::String dbToString (float v, int)
    {
        if (std::abs (v) < 0.05f)
            v = 0.0f;
        return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB";
    }
}

DoomEQProcessor::DoomEQProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "DoomEQ", createLayout()),
      presets (apvts, [this] (const juce::AudioProcessorListener::ChangeDetails& d) { hostDisplay.post (d); })
{
    for (int b = 0; b < numBands; ++b)
    {
        auto& band = bands[(size_t) b];
        band.on   = apvts.getRawParameterValue (pid (b, "on"));
        band.type = apvts.getRawParameterValue (pid (b, "type"));
        band.freq = apvts.getRawParameterValue (pid (b, "freq"));
        band.gain = apvts.getRawParameterValue (pid (b, "gain"));
        band.q    = apvts.getRawParameterValue (pid (b, "q"));
    }

    outputParam = apvts.getRawParameterValue ("output");
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    eqBypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("eq_bypass"));

    DoomAudio::init(); // the shared mixer is created here, never on the audio thread
    doomSoundParam = apvts.getRawParameterValue ("doom_sound");
    doomLevelParam = apvts.getRawParameterValue ("doom_level");
}

DoomEQProcessor::~DoomEQProcessor()
{
    DoomAudio::release (this);
}

void DoomEQProcessor::releaseResources()
{
    // Deactivated (track off, device off): let another DOOM EQ take DOOM's sound.
    DoomAudio::release (this);
}

void DoomEQProcessor::setOverlay (float percent)
{
    uiOverlay = percent;
    hostDisplay.post (juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged (true));
}

//==============================================================================
// Host programs: the factory presets, plus a last "User Preset" for every other sound. User
// presets aren't listed one by one: hosts cache the list and it would go stale as files change.
int DoomEQProcessor::getNumPrograms()
{
    return PresetManager::getNumFactory() + 1;
}

int DoomEQProcessor::getCurrentProgram()
{
    const int factoryIndex = presets.getCurrentFactoryIndex();
    return factoryIndex >= 0 ? factoryIndex : PresetManager::getNumFactory();
}

void DoomEQProcessor::setCurrentProgram (int index)
{
    if (juce::isPositiveAndBelow (index, PresetManager::getNumFactory()))
        presets.loadFactoryPresetForHost (index);
    else if (index == PresetManager::getNumFactory() && getCurrentProgram() != index)
        hostDisplay.post (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true)); // snap the host back
}

const juce::String DoomEQProcessor::getProgramName (int index)
{
    if (juce::isPositiveAndBelow (index, PresetManager::getNumFactory()))
        return PresetManager::getFactoryPresetName (index);
    if (index == PresetManager::getNumFactory())
        return "User Preset";
    return {};
}

bool DoomEQProcessor::ownsDoomAudio() const
{
    return DoomAudio::isOwner (this);
}

bool DoomEQProcessor::doomAudioUsedElsewhere() const
{
    return DoomAudio::isOwnedByAnother (this);
}

juce::AudioProcessorValueTreeState::ParameterLayout DoomEQProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    juce::StringArray typeNames;
    for (auto* n : eq::typeNames)
        typeNames.add (n);

    for (int b = 0; b < numBands; ++b)
    {
        const auto n = juce::String (b + 1);
        auto group = std::make_unique<juce::AudioProcessorParameterGroup> ("band" + n, "Band " + n, " | ");

        group->addChild (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { pid (b, "on"), 1 }, "Band " + n + " On", true));

        group->addChild (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { pid (b, "type"), 1 }, "Band " + n + " Type",
                                                                       typeNames, (int) defaultTypes[b]));

        group->addChild (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "freq"), 1 }, "Band " + n + " Freq", logRange (20.0f, 20000.0f), defaultFreqs[b],
            juce::AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (freqToString).withValueFromStringFunction (stringToFreq)));

        group->addChild (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "gain"), 1 }, "Band " + n + " Gain", juce::NormalisableRange<float> (-18.0f, 18.0f, 0.01f), 0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbToString)));

        group->addChild (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (b, "q"), 1 }, "Band " + n + " Q", logRange (0.1f, 10.0f), defaultQs[b],
            juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return juce::String (v, 2); })));

        layout.add (std::move (group));
    }

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output", juce::NormalisableRange<float> (-18.0f, 18.0f, 0.01f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbToString)));

    // Parameters added after 0.0.1 must have a version hint above every hint already shipped (1),
    // and AU parameters must never be removed, or saved sessions and automation break.
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "bypass", 2 }, "Bypass", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "eq_bypass", 2 }, "EQ Bypass", false));

    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "doom_sound", 1 }, "DOOM Sound", false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "doom_level", 1 }, "DOOM Level", juce::NormalisableRange<float> (-40.0f, 6.0f, 0.01f), -6.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbToString)));

    return layout;
}

bool DoomEQProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
    return (in == mono && out == mono) || (in == stereo && out == stereo) || (in == mono && out == stereo);
}

void DoomEQProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    for (auto& b : bands)
    {
        b.f.reset (sampleRate, 0.03);
        b.qs.reset (sampleRate, 0.03);
        b.g.reset (sampleRate, 0.03);
        b.mix.reset (sampleRate, 0.02);
        b.typeFade.reset (sampleRate, 0.008);
        b.typeFade.setCurrentAndTargetValue (1.0f);

        b.f.setCurrentAndTargetValue (b.freq->load());
        b.qs.setCurrentAndTargetValue (b.q->load());
        b.g.setCurrentAndTargetValue (b.gain->load());
        b.mix.setCurrentAndTargetValue (b.on->load() > 0.5f ? 1.0f : 0.0f);
        b.currentType = b.pendingType = juce::jlimit (0, eq::numTypes - 1, (int) b.type->load());
        b.dirty = true;
        b.wasSilent = false;
        b.resetState();
    }

    outputGain.reset (sampleRate, 0.03);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    // Hosts may send bigger blocks than this; processBlock splits them, so nothing is
    // ever resized on the audio thread.
    maxChunk = juce::jmax (64, samplesPerBlock);
    monoScratch.assign ((size_t) maxChunk, 0.0f);
    doomBuffer.setSize (2, maxChunk);
    dryBuffer.setSize (maxChannels, maxChunk);
    wetMix.reset (sampleRate, 0.02);
    wetMix.setCurrentAndTargetValue ((bypassParam->get() || eqBypassParam->get()) ? 0.0f : 1.0f);
    doomGain.reset (sampleRate, 0.05);
    doomGain.setCurrentAndTargetValue (0.0f);
    analyserFifo.reset();
}

void DoomEQProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels(), numOut = getTotalNumOutputChannels();

    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, numSamples);

    // Mono in, stereo out: both sides start from the input, so DOOM can be stereo.
    if (numIn == 1 && numOut >= 2)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    const int numCh = juce::jmin (juce::jmax (numIn, numOut), buffer.getNumChannels(), maxChannels);
    const double sr = currentSampleRate.load();

    updateTargets();

    for (int start = 0; start < numSamples; start += maxChunk)
    {
        const int len = juce::jmin (maxChunk, numSamples - start);
        juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), start, len);
        processChunk (chunk, numCh, sr);
    }

    blockCount.fetch_add (1, std::memory_order_relaxed);
}

void DoomEQProcessor::updateTargets()
{
    const bool hostBypass = bypassParam->get();
    const bool eqOff = hostBypass || eqBypassParam->get();
    wetMix.setTargetValue (eqOff ? 0.0f : 1.0f);
    const bool eqFullyOff = eqOff && ! wetMix.isSmoothing();

    // DOOM's sound goes in ahead of the EQ, so you're EQing the game. Host bypass silences
    // it too, so a bypassed plug-in passes its input through unchanged.
    const bool doomOn = doomSoundParam->load() > 0.5f && ! hostBypass;
    doomGain.setTargetValue (doomOn ? juce::Decibels::decibelsToGain (doomLevelParam->load()) : 0.0f);

    outputGain.setTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    for (auto& b : bands)
    {
        b.f.setTargetValue (b.freq->load());
        b.qs.setTargetValue (b.q->load());
        b.g.setTargetValue (b.gain->load());
        b.mix.setTargetValue (b.on->load() > 0.5f ? 1.0f : 0.0f);

        b.pendingType = juce::jlimit (0, eq::numTypes - 1, (int) b.type->load());
        if (b.pendingType != b.currentType)
        {
            if (b.isSilent() || eqFullyOff)
            {
                b.switchToPendingType(); // nothing audible to click
                b.typeFade.setCurrentAndTargetValue (1.0f);
            }
            else if (b.typeFade.getTargetValue() == 1.0f)
            {
                b.typeFade.setTargetValue (0.0f);
            }
        }
        else if (b.typeFade.getTargetValue() == 0.0f)
        {
            b.typeFade.setTargetValue (1.0f); // changed back before the fade finished
        }
    }
}

void DoomEQProcessor::processChunk (juce::AudioBuffer<float>& buffer, int numCh, double sr)
{
    const int numSamples = buffer.getNumSamples();

    // DOOM's sound.
    if ((doomGain.getTargetValue() > 0.0f || doomGain.isSmoothing()) && numCh > 0)
    {
        auto* dl = doomBuffer.getWritePointer (0);
        auto* dr = doomBuffer.getWritePointer (1);
        juce::FloatVectorOperations::clear (dl, numSamples);
        juce::FloatVectorOperations::clear (dr, numSamples);

        if (DoomAudio::render (this, dl, dr, numSamples, sr))
        {
            auto* outL = buffer.getWritePointer (0);
            auto* outR = numCh > 1 ? buffer.getWritePointer (1) : nullptr;
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = doomGain.getNextValue();
                if (outR == nullptr)
                {
                    outL[i] += 0.5f * (dl[i] + dr[i]) * g;
                }
                else
                {
                    outL[i] += dl[i] * g;
                    outR[i] += dr[i] * g;
                }
            }
        }
        else
        {
            doomGain.skip (numSamples);
        }
    }
    else
    {
        DoomAudio::release (this);
        doomGain.setCurrentAndTargetValue (doomGain.getTargetValue());
    }

    // EQ bypass crossfade (the BYPASS button or the host's bypass).
    const bool fullyBypassed = ! wetMix.isSmoothing() && wetMix.getTargetValue() == 0.0f;
    const bool needDry = wetMix.isSmoothing();

    if (needDry)
        for (int ch = 0; ch < numCh; ++ch)
            dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    if (fullyBypassed)
    {
        // Settings land straight away while bypassed (no sweep when the EQ comes back), and
        // the filters restart from rest.
        for (auto& b : bands)
        {
            if (! b.wasSilent)
                b.resetState();
            b.wasSilent = true;
            b.f.setCurrentAndTargetValue (b.f.getTargetValue());
            b.qs.setCurrentAndTargetValue (b.qs.getTargetValue());
            b.g.setCurrentAndTargetValue (b.g.getTargetValue());
            b.mix.setCurrentAndTargetValue (b.mix.getTargetValue());
            if (b.pendingType != b.currentType)
                b.switchToPendingType();
            b.typeFade.setCurrentAndTargetValue (1.0f);
            b.dirty = true;
        }
    }

    constexpr int subBlock = 16;
    float wetValues[subBlock];

    for (int start = 0; start < numSamples && ! fullyBypassed; start += subBlock)
    {
        const int len = juce::jmin (subBlock, numSamples - start);

        for (auto& b : bands)
        {
            if (b.isSilent())
            {
                if (! b.wasSilent)
                    b.resetState();
                b.wasSilent = true;
                b.f.setCurrentAndTargetValue (b.f.getTargetValue());
                b.qs.setCurrentAndTargetValue (b.qs.getTargetValue());
                b.g.setCurrentAndTargetValue (b.g.getTargetValue());
                if (b.pendingType != b.currentType)
                    b.switchToPendingType();
                b.typeFade.setCurrentAndTargetValue (1.0f);
                b.dirty = true;
                continue;
            }
            b.wasSilent = false;

            if (b.dirty || b.f.isSmoothing() || b.qs.isSmoothing() || b.g.isSmoothing())
            {
                const float fv = b.f.skip (len);
                const float qv = b.qs.skip (len);
                const float gv = b.g.skip (len);
                b.coeffs = eq::design ((eq::Type) b.currentType, fv, gv, qv, sr);
                b.dirty = false;
            }

            bool fullyOn = ! b.mix.isSmoothing() && ! b.typeFade.isSmoothing()
                           && b.mix.getCurrentValue() >= 1.0f && b.typeFade.getCurrentValue() >= 1.0f;
            for (int i = 0; i < len; ++i)
                wetValues[i] = b.mix.getNextValue() * b.typeFade.getNextValue();

            const int stages = b.coeffs.stages;

            for (int ch = 0; ch < numCh; ++ch)
            {
                float* d = buffer.getWritePointer (ch, start);
                auto& st = b.state[(size_t) ch];

                for (int i = 0; i < len; ++i)
                {
                    const double x = d[i];
                    double y = x;
                    for (int s = 0; s < stages; ++s)
                        y = eq::tick (b.coeffs.s[(size_t) s], st[(size_t) s], y);

                    d[i] = fullyOn ? (float) y : (float) (x + wetValues[i] * (y - x));
                }
            }

            // Faded all the way out: switch type and fade back in.
            if (b.typeFade.getTargetValue() == 0.0f && ! b.typeFade.isSmoothing())
            {
                b.switchToPendingType();
                b.typeFade.setTargetValue (1.0f);
            }
        }
    }

    if (fullyBypassed)
        outputGain.skip (numSamples);
    else
        outputGain.applyGain (buffer, numSamples);

    if (needDry)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float wet = wetMix.getNextValue();
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float dry = dryBuffer.getReadPointer (ch)[i];
                auto* d = buffer.getWritePointer (ch);
                d[i] = dry + wet * (d[i] - dry);
            }
        }
    }
    else
    {
        wetMix.skip (numSamples);
    }

    float peak = 0.0f;
    for (int ch = 0; ch < numCh; ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, numSamples));
    const float decayed = outputPeak.load() * std::pow (0.5f, (float) numSamples / (float) (sr * 0.15));
    outputPeak = juce::jmax (peak, decayed);

    // Analyser feed: post-EQ mono.
    if (numCh > 0)
    {
        juce::FloatVectorOperations::copy (monoScratch.data(), buffer.getReadPointer (0), numSamples);
        for (int ch = 1; ch < numCh; ++ch)
            juce::FloatVectorOperations::add (monoScratch.data(), buffer.getReadPointer (ch), numSamples);
        if (numCh > 1)
            juce::FloatVectorOperations::multiply (monoScratch.data(), 1.0f / (float) numCh, numSamples);

        if (analyserFifo.getFreeSpace() >= numSamples)
        {
            int s1, n1, s2, n2;
            analyserFifo.prepareToWrite (numSamples, s1, n1, s2, n2);
            std::copy_n (monoScratch.data(), n1, analyserBuffer.data() + s1);
            std::copy_n (monoScratch.data() + n1, n2, analyserBuffer.data() + s2);
            analyserFifo.finishedWrite (n1 + n2);
        }
    }
}

int DoomEQProcessor::pullAnalyserSamples (float* dest, int maxSamples)
{
    const int ready = juce::jmin (maxSamples, analyserFifo.getNumReady());
    int s1, n1, s2, n2;
    analyserFifo.prepareToRead (ready, s1, n1, s2, n2);
    std::copy_n (analyserBuffer.data() + s1, n1, dest);
    std::copy_n (analyserBuffer.data() + s2, n2, dest + n1);
    analyserFifo.finishedRead (n1 + n2);
    return n1 + n2;
}

void DoomEQProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("overlay", uiOverlay.load(), nullptr);
    if (uiWidth.load() > 0 && uiHeight.load() > 0)
    {
        state.setProperty ("uiW", uiWidth.load(), nullptr);
        state.setProperty ("uiH", uiHeight.load(), nullptr);
    }
    presets.writeToState (state);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void DoomEQProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            const auto presetInfo = PresetManager::readStateInfo (*xml);

            auto state = juce::ValueTree::fromXml (*xml);
            const double overlay = state.getProperty ("overlay", 70.0);
            uiOverlay = std::isfinite (overlay) ? (float) juce::jlimit (0.0, 100.0, overlay) : 70.0f;
            uiWidth = (int) state.getProperty ("uiW", 0);
            uiHeight = (int) state.getProperty ("uiH", 0);

            // A parameter the session has no value for (saved before it existed, e.g. bypass in a
            // 0.0.1 session) gets the default it was first given, rather than keeping whatever
            // this instance had: the session then recalls exactly as it sounded.
            for (auto* parameter : getParameters())
            {
                auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);
                if (ranged == nullptr)
                    continue;

                bool listed = false;
                for (auto child : state)
                {
                    if (child.getProperty ("id").toString() != ranged->paramID)
                        continue;
                    listed = true;
                    const auto v = child.getProperty ("value");
                    if (v.isVoid() || ! std::isfinite ((double) v))
                        child.setProperty ("value", PresetManager::getValueWhenAbsent (*ranged), nullptr);
                }
                if (! listed)
                    state.appendChild (juce::ValueTree ("PARAM", { { "id", ranged->paramID },
                                                                   { "value", PresetManager::getValueWhenAbsent (*ranged) } }), nullptr);
            }

            presets.restoreState (presetInfo, [this, &state] { apvts.replaceState (state); });
        }
    }
}

juce::AudioProcessorEditor* DoomEQProcessor::createEditor()
{
    return new DoomEQEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DoomEQProcessor();
}
