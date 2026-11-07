#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr size_t maxHistory = 64;
}

SoundAdviceProcessor::SoundAdviceProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      library (QuoteLibrary::getEmbedded()),
      picker (library)
{
    const juce::ScopedLock sl (lock);
    current = picker.next (category);
}

//==============================================================================
int SoundAdviceProcessor::getCategory() const
{
    const juce::ScopedLock sl (lock);
    return category;
}

int SoundAdviceProcessor::getCurrentEntry() const
{
    const juce::ScopedLock sl (lock);
    return current;
}

bool SoundAdviceProcessor::isNightMode() const
{
    const juce::ScopedLock sl (lock);
    return night;
}

void SoundAdviceProcessor::setNightMode (bool shouldBeNight)
{
    const juce::ScopedLock sl (lock);
    night = shouldBeNight;
}

void SoundAdviceProcessor::show (int entry)
{
    if (entry < 0 || entry == current)
        return;
    if (current >= 0)
    {
        history.push_back (current);
        if (history.size() > maxHistory)
            history.erase (history.begin());
    }
    current = entry;
}

int SoundAdviceProcessor::selectCategory (int categoryIndex)
{
    const juce::ScopedLock sl (lock);
    if (juce::isPositiveAndBelow (categoryIndex, (int) library.getCategories().size()))
        category = categoryIndex;
    show (picker.next (category, current));
    return current;
}

int SoundAdviceProcessor::roll()
{
    const juce::ScopedLock sl (lock);
    show (picker.next (category, current));
    return current;
}

int SoundAdviceProcessor::back()
{
    const juce::ScopedLock sl (lock);
    if (! history.empty())
    {
        current = history.back();
        history.pop_back();
    }
    return current;
}

bool SoundAdviceProcessor::canGoBack() const
{
    const juce::ScopedLock sl (lock);
    return ! history.empty();
}

//==============================================================================
bool SoundAdviceProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Anything goes, as long as what comes out matches what goes in.
    return ! layouts.getMainOutputChannelSet().isDisabled()
        && layouts.getMainInputChannelSet() == layouts.getMainOutputChannelSet();
}

void SoundAdviceProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    // Pass-through: clear any output channels that have no input.
    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
}

juce::AudioProcessorEditor* SoundAdviceProcessor::createEditor()
{
    return new SoundAdviceEditor (*this);
}

//==============================================================================
void SoundAdviceProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const juce::ScopedLock sl (lock);
    juce::ValueTree state ("SoundAdvice");
    state.setProperty ("version", PLUGIN_VERSION, nullptr);
    state.setProperty ("category", library.getCategories()[(size_t) category].id, nullptr);
    if (current >= 0)
        state.setProperty ("entry", library.getEntries()[(size_t) current].id, nullptr);
    state.setProperty ("night", night, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void SoundAdviceProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;
    const auto state = juce::ValueTree::fromXml (*xml);
    if (! state.hasType ("SoundAdvice"))
        return;

    const juce::ScopedLock sl (lock);
    if (const auto c = library.findCategory (state["category"].toString()); c >= 0)
        category = c;
    night = (bool) state.getProperty ("night", false);
    history.clear();

    // Always open on something new: deal a fresh entry, steering clear of the one on screen last time.
    const auto last = library.findEntry (state["entry"].toString());
    const auto pick = picker.next (category, last);
    if (pick >= 0)
        current = pick;
}

void SoundAdviceProcessor::showEntryById (const juce::String& id)
{
    const juce::ScopedLock sl (lock);
    if (const auto e = library.findEntry (id); e >= 0)
    {
        show (e);
        history.clear();
    }
}

int SoundAdviceProcessor::dealForNewWindow()
{
    const juce::ScopedLock sl (lock);
    if (! dealOnOpen)
        return current;
    if (editorOpened)
        show (picker.next (category, current));
    editorOpened = true;   // the first window shows what the load just dealt
    return current;
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SoundAdviceProcessor();
}
