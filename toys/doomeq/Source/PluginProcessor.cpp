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
      apvts (*this, nullptr, "DoomEQ", createLayout())
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
    doomSoundParam = apvts.getRawParameterValue ("doom_sound");
    doomLevelParam = apvts.getRawParameterValue ("doom_level");
}

DoomEQProcessor::~DoomEQProcessor()
{
    DoomAudio::release (this);
}

bool DoomEQProcessor::ownsDoomAudio() const
{
    return DoomAudio::isOwner (this);
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

    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "doom_sound", 1 }, "DOOM Sound", false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "doom_level", 1 }, "DOOM Level", juce::NormalisableRange<float> (-40.0f, 6.0f, 0.01f), -6.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbToString)));

    return layout;
}

bool DoomEQProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != layouts.getMainInputChannelSet())
        return false;
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
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

        b.f.setCurrentAndTargetValue (b.freq->load());
        b.qs.setCurrentAndTargetValue (b.q->load());
        b.g.setCurrentAndTargetValue (b.gain->load());
        b.mix.setCurrentAndTargetValue (b.on->load() > 0.5f ? 1.0f : 0.0f);
        b.currentType = (int) b.type->load();
        b.dirty = true;
        b.wasSilent = false;
        b.resetState();
    }

    outputGain.reset (sampleRate, 0.03);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    monoScratch.assign ((size_t) juce::jmax (1, samplesPerBlock), 0.0f);
    doomBuffer.setSize (2, juce::jmax (1, samplesPerBlock));
    doomGain.reset (sampleRate, 0.05);
    doomGain.setCurrentAndTargetValue (0.0f);
    analyserFifo.reset();
}

void DoomEQProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();

    for (int ch = numIn; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    const int numCh = juce::jmin (numIn, maxChannels);
    const double sr = currentSampleRate.load();

    // DOOM's sound goes in ahead of the EQ, so you're EQing the game.
    const bool doomOn = doomSoundParam->load() > 0.5f;
    doomGain.setTargetValue (doomOn ? juce::Decibels::decibelsToGain (doomLevelParam->load()) : 0.0f);

    if ((doomOn || doomGain.isSmoothing()) && numCh > 0)
    {
        if (doomBuffer.getNumSamples() < numSamples)
            doomBuffer.setSize (2, numSamples, false, false, true);
        doomBuffer.clear();

        auto* dl = doomBuffer.getWritePointer (0);
        auto* dr = doomBuffer.getWritePointer (1);

        if (DoomAudio::render (this, dl, dr, numSamples, sr))
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = doomGain.getNextValue();
                if (numCh == 1)
                {
                    buffer.getWritePointer (0)[i] += 0.5f * (dl[i] + dr[i]) * g;
                }
                else
                {
                    buffer.getWritePointer (0)[i] += dl[i] * g;
                    buffer.getWritePointer (1)[i] += dr[i] * g;
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

    for (auto& b : bands)
    {
        b.f.setTargetValue (b.freq->load());
        b.qs.setTargetValue (b.q->load());
        b.g.setTargetValue (b.gain->load());
        b.mix.setTargetValue (b.on->load() > 0.5f ? 1.0f : 0.0f);

        const int t = juce::jlimit (0, eq::numTypes - 1, (int) b.type->load());
        if (t != b.currentType)
        {
            // Same SVF core for every type, so state carries over cleanly unless the
            // cascade length or the filter family changes.
            const auto oldT = (eq::Type) b.currentType, newT = (eq::Type) t;
            if (eq::numStages (oldT) != eq::numStages (newT) || eq::isCut (oldT) != eq::isCut (newT))
                b.resetState();
            b.currentType = t;
            b.dirty = true;
        }
    }

    constexpr int subBlock = 16;
    float mixValues[subBlock];

    for (int start = 0; start < numSamples; start += subBlock)
    {
        const int len = juce::jmin (subBlock, numSamples - start);

        for (auto& b : bands)
        {
            const bool silent = ! b.mix.isSmoothing() && b.mix.getTargetValue() == 0.0f;
            if (silent)
            {
                if (! b.wasSilent)
                    b.resetState();
                b.wasSilent = true;
                b.f.setCurrentAndTargetValue (b.f.getTargetValue());
                b.qs.setCurrentAndTargetValue (b.qs.getTargetValue());
                b.g.setCurrentAndTargetValue (b.g.getTargetValue());
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

            for (int i = 0; i < len; ++i)
                mixValues[i] = b.mix.getNextValue();

            const bool fullyOn = mixValues[0] >= 1.0f && mixValues[len - 1] >= 1.0f;
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

                    d[i] = fullyOn ? (float) y : (float) (x + mixValues[i] * (y - x));
                }
            }
        }
    }

    outputGain.setTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));
    outputGain.applyGain (buffer, numSamples);

    float peak = 0.0f;
    for (int ch = 0; ch < numCh; ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, numSamples));
    const float decayed = outputPeak.load() * std::pow (0.5f, (float) numSamples / (float) (sr * 0.15));
    outputPeak = juce::jmax (peak, decayed);


    // Analyser feed: post-EQ mono.
    if (numCh > 0 && (int) monoScratch.size() >= numSamples)
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
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void DoomEQProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* DoomEQProcessor::createEditor()
{
    return new DoomEQEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DoomEQProcessor();
}
