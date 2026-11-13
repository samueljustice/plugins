#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#include "VariationRenderer.h"

/**
 * Render Variations: the panel for a batch of whooshes rendered straight to files (see VariationRenderer). An
 * overlay like Settings and Takes.
 *
 * How many, how long, what varies (and by how much), the batch seed, where the files go; Render and Cancel; and
 * while a batch renders a progress bar with the percentage, "37 of 100", the file in hand and the time left. The
 * batch carries on with the panel closed (the header button shows the percentage), and the rig can be played and
 * changed meanwhile.
 */
class VariationsView : public juce::Component, private juce::Timer
{
public:
    VariationsView(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void visibilityChanged() override;

    /** Open Takes was pressed. */
    std::function<void()> onOpenTakes;
    /** A text box has finished with the keyboard (Return / Escape): hand it back to the rig. */
    std::function<void()> onKeyboardDone;

    /** "Show in Finder" on a Mac, "Show in Explorer" on Windows, "Show Folder" elsewhere. */
    static juce::String showButtonText();

private:
    void timerCallback() override;
    void readSettings();           // the controls from the saved settings and the rig as it is now
    void syncControls();           // what is lit, enabled and showing
    void startBatch();
    VariationRenderer::Job job() const; // what the controls say
    juce::File folder() const;
    juce::String formatLine() const;

    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
    juce::TextEditor m_count, m_length, m_seed;
    juce::TextButton m_newSeed { "New Seed" };
    std::array<juce::TextButton, 3> m_modes;
    juce::Slider m_amount;
    juce::TextButton m_choose { "Choose..." }, m_dated { "Dated Folder" };
    juce::TextButton m_render { "Render" }, m_cancel { "Cancel" }, m_show, m_openTakes { "Open Takes" }, m_close { "Close" };
    std::unique_ptr<juce::FileChooser> m_chooser;

    juce::Rectangle<int> m_panel, m_folderText, m_formatText, m_bar, m_status, m_amountValue, m_lengthUnit;
    std::array<juce::Rectangle<int>, 6> m_captions; // Count, Length, Batch Seed, What Varies, Amount, Folder
    VariationRenderer::Progress m_shown;
    juce::String m_error;          // a batch that could not start
    bool m_seedIsOurs = true;      // the seed in the box was drawn here (a new one follows each batch), not typed

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VariationsView)
};
