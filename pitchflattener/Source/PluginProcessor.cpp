#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

namespace
{
    namespace core = pitchflattener::core;

    juce::ParameterID pid (const char* id) { return { id, 1 }; }

    const juce::StringArray c_targetNames { "High Point", "Centre", "Low Point", "Start", "Note" };
    const juce::StringArray c_sourceNames { "Auto", "Voice", "Engine", "Tone" };
    const juce::StringArray c_characterNames { "Harmonic", "Transients", "Voice" };
    const juce::Identifier c_viewProperty { "displayView" };

    /** "A4", "C#3", or a plain number: the MIDI note it names (with cents after it, "A4 +12"). */
    double parseNote (const juce::String& text)
    {
        static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        auto t = text.trim().toUpperCase();
        double cents = 0.0;

        if (const int space = t.indexOfChar (' '); space > 0)
        {
            cents = t.substring (space + 1).retainCharacters ("+-0123456789.").getDoubleValue();
            t = t.substring (0, space);
        }

        for (int pc = 11; pc >= 0; --pc)   // sharps first, so "C#" is not read as "C"
        {
            const juce::String name (names[pc]);
            if (t.startsWith (name) && t.length() > name.length())
            {
                const auto rest = t.substring (name.length());
                if (rest.containsOnly ("-0123456789"))
                    return (rest.getIntValue() + 1) * 12 + pc + cents / 100.0;
            }
        }

        return t.getDoubleValue();
    }
}

//==============================================================================
PitchFlattenerAudioProcessor::PitchFlattenerAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    , parameters (*this, nullptr, "PitchFlattener", createParameterLayout())
{
    syncOptions();
    startTimerHz (20);
}

PitchFlattenerAudioProcessor::~PitchFlattenerAudioProcessor()
{
    stopTimer();
}

juce::AudioProcessorValueTreeState::ParameterLayout PitchFlattenerAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterChoice> (pid (targetId), "Target", c_targetNames, 0));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        pid (noteId), "Note", juce::NormalisableRange<float> (24.0f, 108.0f, 0.01f), 69.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return noteText (v); })
            .withValueFromStringFunction ([] (const juce::String& t) { return (float) juce::jlimit (24.0, 108.0, parseNote (t)); })));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        pid (amountId), "Amount", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)) + "%"; })
            .withValueFromStringFunction ([] (const juce::String& t) { return t.retainCharacters ("0123456789.").getFloatValue(); })));

    layout.add (std::make_unique<juce::AudioParameterChoice> (pid (sourceId), "Source", c_sourceNames, 0));
    layout.add (std::make_unique<juce::AudioParameterBool> (pid (keepLengthId), "Keep Length", false,
        juce::AudioParameterBoolAttributes().withStringFromValueFunction ([] (bool on, int) { return on ? "Keep length" : "Varispeed"; })));
    layout.add (std::make_unique<juce::AudioParameterChoice> (pid (characterId), "Character", c_characterNames, 0));

    return layout;
}

juce::String PitchFlattenerAudioProcessor::noteText (double midi)
{
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int nearest = (int) std::lround (midi);
    const int cents = (int) std::lround ((midi - nearest) * 100.0);
    auto text = juce::String (names[((nearest % 12) + 12) % 12]) + juce::String (nearest / 12 - 1);

    if (cents != 0)
        text << (cents > 0 ? " +" : " ") << cents;

    return text;
}

int PitchFlattenerAudioProcessor::getDisplayView() const
{
    const int v = (int) parameters.state.getProperty (c_viewProperty, kViewUnset);
    return v == kViewScope || v == kViewFlight ? v : kViewUnset;
}

void PitchFlattenerAudioProcessor::setDisplayView (int view)
{
    // A property of the parameters' tree, so it is saved and restored with them.
    parameters.state.setProperty (c_viewProperty, view == kViewScope || view == kViewFlight ? view : kViewUnset, nullptr);
}

//==============================================================================
core::Options PitchFlattenerAudioProcessor::optionsFromParameters() const
{
    core::Options o;
    const auto target = (int) parameters.getRawParameterValue (targetId)->load();
    o.target = (core::Target) juce::jlimit (0, 4, target);
    o.noteMidi = parameters.getRawParameterValue (noteId)->load();
    o.amount = parameters.getRawParameterValue (amountId)->load() / 100.0;
    o.line = juce::jlimit (0, core::kNumTrackers, (int) parameters.getRawParameterValue (sourceId)->load()) - 1;   // Auto = -1
    o.keepLength = parameters.getRawParameterValue (keepLengthId)->load() > 0.5f;
    o.character = (core::Character) juce::jlimit (0, 2, (int) parameters.getRawParameterValue (characterId)->load());
    // The corrections on the reading in use, if they were made on the sound learned now.
    if (const auto r = m_engine.result(); r != nullptr && r->learned != nullptr && r->learned->id == m_editsFor)
        if (const int line = lineInUse(); line >= 0)
            o.edits = m_edits[(size_t) line];
    return o;
}

int PitchFlattenerAudioProcessor::lineInUse() const
{
    const auto r = m_engine.result();
    if (r == nullptr || r->learned == nullptr)
        return -1;
    const auto& analysis = r->learned->analysis;
    const int source = juce::jlimit (0, core::kNumTrackers, (int) parameters.getRawParameterValue (sourceId)->load()) - 1;
    if (source >= 0 && analysis.lines[(size_t) source].usable)
        return source;
    return analysis.pick;
}

std::vector<core::CurvePoint> PitchFlattenerAudioProcessor::getEdits (int line) const
{
    const auto r = m_engine.result();
    if (line < 0 || line >= core::kNumTrackers || r == nullptr || r->learned == nullptr || r->learned->id != m_editsFor)
        return {};
    return m_edits[(size_t) line];
}

void PitchFlattenerAudioProcessor::setEdits (int line, std::vector<core::CurvePoint> edits)
{
    const auto r = m_engine.result();
    if (line < 0 || line >= core::kNumTrackers || r == nullptr || r->learned == nullptr)
        return;
    if (r->learned->id != m_editsFor)
    {
        m_edits = {};
        m_editsFor = r->learned->id;
    }
    if (edits.size() > kMaxEdits)
        edits.resize (kMaxEdits);
    std::sort (edits.begin(), edits.end(), [] (const core::CurvePoint& a, const core::CurvePoint& b) { return a.seconds < b.seconds; });
    m_edits[(size_t) line] = std::move (edits);
    syncOptions();
}

void PitchFlattenerAudioProcessor::syncOptions()
{
    // Another sound learned (or none): the corrections were for the one before.
    if (const auto r = m_engine.result(); m_editsFor != 0 && (r == nullptr || r->learned == nullptr || r->learned->id != m_editsFor))
    {
        m_edits = {};
        m_editsFor = 0;
    }

    m_engine.setOptions (optionsFromParameters());
}

void PitchFlattenerAudioProcessor::setParameterFromEditor (const char* id, float plainValue)
{
    if (auto* param = parameters.getParameter (id))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
        param->endChangeGesture();
    }
}

void PitchFlattenerAudioProcessor::timerCallback()
{
    syncOptions();

    // A render longer than the sound (flattened to its low point) carries on after the input
    // stops: that much tail, for hosts that render one.
    double tail = 0.0;
    if (const auto r = m_engine.result(); r != nullptr && r->playable())
        tail = std::max (0.0, (double) r->flattened.channels[0].size() - (double) r->learned->locator.length()) / r->learned->sampleRate;
    m_tailSeconds.store (tail);
}

double PitchFlattenerAudioProcessor::getTailLengthSeconds() const
{
    return m_tailSeconds.load();
}

//==============================================================================
void PitchFlattenerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    m_engine.prepare (sampleRate, samplesPerBlock);
    setLatencySamples (m_engine.latencySamples());
    syncOptions();
}

void PitchFlattenerAudioProcessor::reset()
{
    // Some hosts call this from the message thread (an AU's bypass does): only a request here,
    // which the audio thread carries out at its next block.
    m_engine.requestReset();
}

void PitchFlattenerAudioProcessor::setNonRealtime (bool isNonRealtime) noexcept
{
    AudioProcessor::setNonRealtime (isNonRealtime);
    m_engine.setNonRealtime (isNonRealtime);
}

bool PitchFlattenerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return in == out;
}

void PitchFlattenerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels(), pitchflattener::Engine::kMaxChannels);

    for (int c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    m_engine.process (buffer.getArrayOfWritePointers(), channels, buffer.getNumSamples());
}

void PitchFlattenerAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels(), pitchflattener::Engine::kMaxChannels);

    for (int c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    // Delayed like the processed audio, so bypassing does not shift the track in time.
    m_engine.processBypassed (buffer.getArrayOfWritePointers(), channels, buffer.getNumSamples());
}

//==============================================================================
juce::AudioProcessorEditor* PitchFlattenerAudioProcessor::createEditor()
{
    return new PitchFlattenerAudioProcessorEditor (*this);
}

void PitchFlattenerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty ("stateVersion", stateVersion, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PitchFlattenerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;   // not a state of this plug-in: keep the defaults

    parameters.replaceState (juce::ValueTree::fromXml (*xml));
    syncOptions();
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PitchFlattenerAudioProcessor();
}
