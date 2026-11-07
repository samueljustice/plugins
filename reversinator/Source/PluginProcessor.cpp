#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const juce::Identifier c_stateType { "ReversinatorState" };   // same as v1.x, so old sessions load
    const juce::Identifier c_editorWidthProperty { "editorWidth" };

    constexpr double c_reverserFadeSeconds = 0.010;   // as on the original
    constexpr double c_gainSmoothingSeconds = 0.020;

    float squaredPercent(float percent)
    {
        const float x = juce::jlimit(0.0f, 100.0f, percent) * 0.01f;
        return x * x;
    }

    juce::String percentText(float v, int) { return juce::String(juce::roundToInt(v)) + "%"; }
    float percentValue(const juce::String& text) { return text.retainCharacters("0123456789.-").getFloatValue(); }
}

ReversinatorAudioProcessor::ReversinatorAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , m_parameters(*this, nullptr, c_stateType, createParameterLayout())
{
    m_modeParam     = m_parameters.getRawParameterValue(ParamIDs::mode);
    m_reverserParam = m_parameters.getRawParameterValue(ParamIDs::reverser);
    m_timeParam     = m_parameters.getRawParameterValue(ParamIDs::time);
    m_feedbackParam = m_parameters.getRawParameterValue(ParamIDs::feedback);
    m_wetParam      = m_parameters.getRawParameterValue(ParamIDs::wetMix);
    m_dryParam      = m_parameters.getRawParameterValue(ParamIDs::dryMix);
}

juce::AudioProcessorValueTreeState::ParameterLayout ReversinatorAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<AudioParameterChoice>(
        ParameterID { ParamIDs::mode, 1 }, "Effect Mode",
        StringArray { "Reverse Playback", "Forward Backwards", "Reverse Repeat" }, 0));

    // The value text carries its own unit, so no separate label (hosts would show it twice).
    layout.add(std::make_unique<AudioParameterBool>(
        ParameterID { ParamIDs::reverser, 1 }, "Reverser", true,
        AudioParameterBoolAttributes()
            .withStringFromValueFunction([](bool on, int) { return on ? "Reverse" : "Forward"; })
            .withValueFromStringFunction([](const String& text)
            {
                const auto t = text.trim();
                return t.equalsIgnoreCase("reverse") || t.equalsIgnoreCase("on") || t.equalsIgnoreCase("true")
                    || t.equalsIgnoreCase("yes") || t.getIntValue() != 0;
            })));

    // Same taper as the original: value = 0.5 + 3.5 * n^2.
    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { ParamIDs::time, 1 }, "Time",
        NormalisableRange<float>(ReverseEngine::minTimeSeconds, ReverseEngine::maxTimeSeconds, 0.0f, 0.5f), 2.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return String(v, 2) + " s"; })
            .withValueFromStringFunction([](const String& t) { return t.retainCharacters("0123456789.").getFloatValue(); })));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { ParamIDs::feedback, 1 }, "Feedback Depth",
        NormalisableRange<float>(0.0f, 75.0f, 0.0f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(percentText).withValueFromStringFunction(percentValue)));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { ParamIDs::wetMix, 1 }, "Wet Mix",
        NormalisableRange<float>(0.0f, 100.0f, 0.0f), 100.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(percentText).withValueFromStringFunction(percentValue)));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { ParamIDs::dryMix, 1 }, "Dry Mix",
        NormalisableRange<float>(0.0f, 100.0f, 0.0f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(percentText).withValueFromStringFunction(percentValue)));

    return layout;
}

bool ReversinatorAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
        && out == layouts.getMainInputChannelSet();
}

void ReversinatorAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const int numChannels = std::max(1, std::max(getTotalNumInputChannels(), getTotalNumOutputChannels()));
    m_maxBlock = std::max(1, samplesPerBlock);

    m_engine.prepare(sampleRate, m_maxBlock, numChannels);
    m_engine.setMode(static_cast<ReverseEngine::Mode>(juce::roundToInt(m_modeParam->load())), true);
    m_engine.setTime(m_timeParam->load(), true);
    m_engine.setFeedback(m_feedbackParam->load() * 0.01f, true);

    m_input.setSize(numChannels, m_maxBlock, false, true, false);
    m_wet.setSize(numChannels, m_maxBlock, false, true, false);

    m_reverserMix.reset(sampleRate, c_reverserFadeSeconds);
    m_reverserMix.setCurrentAndTargetValue(m_reverserParam->load() >= 0.5f ? 1.0f : 0.0f);
    m_wetGain.reset(sampleRate, c_gainSmoothingSeconds);
    m_wetGain.setCurrentAndTargetValue(squaredPercent(m_wetParam->load()));
    m_dryGain.reset(sampleRate, c_gainSmoothingSeconds);
    m_dryGain.setCurrentAndTargetValue(squaredPercent(m_dryParam->load()));
    m_bypassMix.reset(sampleRate, c_reverserFadeSeconds);
    m_bypassMix.setCurrentAndTargetValue(m_bypassMix.getTargetValue());

    m_peakAccumulator = 0.0f;
    m_peakCount = 0;
    m_binSamples = std::max(1, juce::roundToInt(sampleRate * displayBinSeconds));
    m_displayBinSeconds.store(m_binSamples / sampleRate);
}

void ReversinatorAudioProcessor::releaseResources()
{
    m_engine.reset();
}

void ReversinatorAudioProcessor::reset()
{
    // Host flush (transport jumps, offline render start): forget all buffered audio.
    m_engine.reset();
    m_reverserMix.setCurrentAndTargetValue(m_reverserMix.getTargetValue());
    m_wetGain.setCurrentAndTargetValue(m_wetGain.getTargetValue());
    m_dryGain.setCurrentAndTargetValue(m_dryGain.getTargetValue());
    m_bypassMix.setCurrentAndTargetValue(m_bypassMix.getTargetValue());
}

double ReversinatorAudioProcessor::getTailLengthSeconds() const
{
    return ReverseEngine::tailSeconds(m_timeParam->load(), m_feedbackParam->load() * 0.01f);
}

void ReversinatorAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    process(buffer, false);
}

void ReversinatorAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // JUCE's default would just pass audio through and starve the engine; instead keep it
    // fed (so un-bypassing lands in a live effect) and crossfade the output to the input.
    process(buffer, true);
}

void ReversinatorAudioProcessor::process(juce::AudioBuffer<float>& buffer, bool bypassed)
{
    juce::ScopedNoDenormals noDenormals;
    m_bypassMix.setTargetValue(bypassed ? 1.0f : 0.0f);

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    // Layouts are limited to mono/stereo, but never trust the buffer beyond that.
    const int numChannels = std::min({ buffer.getNumChannels(), m_input.getNumChannels(), 2 });
    if (numChannels == 0 || m_maxBlock == 0)
        return;

    m_engine.setMode(static_cast<ReverseEngine::Mode>(juce::roundToInt(m_modeParam->load())));
    m_engine.setTime(m_timeParam->load());
    m_engine.setFeedback(m_feedbackParam->load() * 0.01f);
    m_reverserMix.setTargetValue(m_reverserParam->load() >= 0.5f ? 1.0f : 0.0f);
    m_wetGain.setTargetValue(squaredPercent(m_wetParam->load()));
    m_dryGain.setTargetValue(squaredPercent(m_dryParam->load()));

    // Hosts should never exceed the prepared block size, but stay safe if they do.
    std::array<float*, 2> channels {};
    for (int pos = 0; pos < buffer.getNumSamples(); pos += m_maxBlock)
    {
        const int n = std::min(m_maxBlock, buffer.getNumSamples() - pos);
        for (int ch = 0; ch < numChannels; ++ch)
            channels[static_cast<size_t>(ch)] = buffer.getWritePointer(ch, pos);
        processChunk(channels.data(), numChannels, n);
    }

    // Publish the read heads for the display.
    int count = 0;
    const auto voices = m_engine.getVoiceViews(count);
    for (int v = 0; v < ReverseEngine::maxVoices; ++v)
    {
        m_voiceState[static_cast<size_t>(v * 2)].store(voices[static_cast<size_t>(v)].delaySeconds, std::memory_order_relaxed);
        m_voiceState[static_cast<size_t>(v * 2 + 1)].store(voices[static_cast<size_t>(v)].gain, std::memory_order_relaxed);
    }
    m_numVoices.store(count, std::memory_order_release);
}

void ReversinatorAudioProcessor::processChunk(float* const* channels, int numChannels, int numSamples)
{
    // Copy the input, replacing any non-finite samples with silence so one bad sample
    // from the host cannot poison the delay lines (or come out of a zero-gain path).
    for (int ch = 0; ch < numChannels; ++ch)
    {
        m_input.copyFrom(ch, 0, channels[ch], numSamples);
        auto* x = m_input.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
            if (! std::isfinite(x[i]))
                x[i] = 0.0f;
    }

    pushDisplayPeaks(m_input.getArrayOfReadPointers(), numChannels, numSamples);

    // The engine always runs, so switching REVERSER on lands in a live effect, as on the original.
    m_engine.process(m_input.getArrayOfReadPointers(), m_wet.getArrayOfWritePointers(), numChannels, numSamples);

    const float* const* in = m_input.getArrayOfReadPointers();
    const float* const* wet = m_wet.getArrayOfReadPointers();

    for (int i = 0; i < numSamples; ++i)
    {
        const float reverse = m_reverserMix.getNextValue();
        const float wetGain = m_wetGain.getNextValue();
        const float dryGain = m_dryGain.getNextValue();
        const float bypass = m_bypassMix.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float x = in[ch][i];
            const float y = wetGain * (reverse * wet[ch][i] + (1.0f - reverse) * x) + dryGain * x;
            channels[ch][i] = y + bypass * (x - y);
        }
    }
}

//==============================================================================
void ReversinatorAudioProcessor::pushDisplayPeaks(const float* const* channels, int numChannels, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        float v = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            v = std::max(v, std::abs(channels[ch][i]));

        m_peakAccumulator = std::max(m_peakAccumulator, v);

        if (++m_peakCount >= m_binSamples)
        {
            const int w = m_peakWrite.load(std::memory_order_relaxed);
            m_peakRing[static_cast<size_t>(w)].store(m_peakAccumulator, std::memory_order_relaxed);
            m_peakWrite.store((w + 1) % peakHistorySize, std::memory_order_release);

            m_peakAccumulator = 0.0f;
            m_peakCount = 0;
        }
    }
}

void ReversinatorAudioProcessor::copyPeakHistory(float* dest, int count) const
{
    count = std::min(count, peakHistorySize);
    const int end = m_peakWrite.load(std::memory_order_acquire);

    for (int i = 0; i < count; ++i)
        dest[i] = m_peakRing[static_cast<size_t>((end - count + i + peakHistorySize) % peakHistorySize)].load(std::memory_order_relaxed);
}

ReversinatorAudioProcessor::DisplayState ReversinatorAudioProcessor::getDisplayState() const
{
    DisplayState s;
    s.numVoices = juce::jlimit(0, ReverseEngine::maxVoices, m_numVoices.load(std::memory_order_acquire));
    for (int v = 0; v < s.numVoices; ++v)
    {
        s.voices[static_cast<size_t>(v)].delaySeconds = m_voiceState[static_cast<size_t>(v * 2)].load(std::memory_order_relaxed);
        s.voices[static_cast<size_t>(v)].gain = m_voiceState[static_cast<size_t>(v * 2 + 1)].load(std::memory_order_relaxed);
    }
    s.timeSeconds = m_timeParam->load();
    s.mode = juce::jlimit(0, 2, juce::roundToInt(m_modeParam->load()));
    s.reversing = m_reverserParam->load() >= 0.5f;
    return s;
}

//==============================================================================
juce::AudioProcessorEditor* ReversinatorAudioProcessor::createEditor()
{
    return new ReversinatorAudioProcessorEditor(*this);
}

void ReversinatorAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = m_parameters.copyState();
    state.setProperty(c_editorWidthProperty, m_editorWidth.load(), nullptr);

    if (const auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void ReversinatorAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName(c_stateType))
        return;

    // v1.x sessions also carry "envelope" and "crossfade"; they are simply ignored,
    // and out-of-range values (e.g. a 30 ms time) are clamped to the new ranges.
    const auto state = juce::ValueTree::fromXml(*xml);
    m_editorWidth.store(state.getProperty(c_editorWidthProperty, 0));
    m_parameters.replaceState(state);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ReversinatorAudioProcessor();
}
