#pragma once

#include <JuceHeader.h>

#include "QuoteLibrary.h"

#include <vector>

/** Sound Advice passes audio straight through; it is here for the words. The processor keeps
    what the editor shows (category, entry, history, theme) so it survives closing the window and
    is saved with the session. */
class SoundAdviceProcessor : public juce::AudioProcessor
{
public:
    SoundAdviceProcessor();
    ~SoundAdviceProcessor() override = default;

    //==============================================================================
    const QuoteLibrary& getLibrary() const noexcept { return library; }

    int getCategory() const;
    int getCurrentEntry() const;
    bool isNightMode() const;
    void setNightMode (bool night);

    /** Chooses a category and deals a fresh entry from it. Returns the entry index. */
    int selectCategory (int categoryIndex);
    /** Deals another entry from the current category. Returns the entry index. */
    int roll();
    /** Steps back to the previously shown entry, if any. Returns the entry index. */
    int back();
    bool canGoBack() const;

    /** Called when the window opens: every new window after the first deals a fresh entry
        (the first shows the one just dealt on load). Returns the entry to show. */
    int dealForNewWindow();
    /** For tools: show this entry and stop windows from dealing new ones. */
    void showEntryById (const juce::String& id);
    void setDealOnOpen (bool shouldDeal) { const juce::ScopedLock sl (lock); dealOnOpen = shouldDeal; }

    //==============================================================================
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    void show (int entry);   // caller holds lock

    const QuoteLibrary& library;
    QuotePicker picker;

    juce::CriticalSection lock;
    int category = 0;
    int current = -1;
    bool night = false;
    bool dealOnOpen = true, editorOpened = false;
    std::vector<int> history;   // earlier entries, most recent last

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundAdviceProcessor)
};
