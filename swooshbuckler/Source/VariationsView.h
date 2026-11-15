#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#include "VariationRenderer.h"

/**
 * Render Variations: the panel for a batch of sounds rendered straight to files (see VariationRenderer). An
 * overlay like Settings and Takes.
 *
 * Five questions, each a menu whose items say what you get: how many, what each file is (one whoosh or a stretch
 * of the whole ride) and how long, what varies between them (and by how much), whether the batch is a new one or
 * one rendered before, what the files are called, and whether every variation is a file, they all go into one
 * file with markers, or both. Then the files' width (Mono to 7.1.4: the batch is rendered natively in that room,
 * whatever the track or device is; it starts at the width the rig plays at), their Channel Order and File Layout
 * (one setting with the tape's, also in Settings), and a line that spells out what each file holds. Under them the
 * panel says in a sentence what Render will make: the name of the first file, how many files, how much sound, how
 * big and roughly how long it will take, or why the format chosen can't hold it.
 *
 * Render and Cancel; and while a batch renders a progress bar with the percentage, "37 of 100", the variation in
 * hand and the time left. The batch carries on with the panel closed (the header button shows the percentage),
 * and the rig can be played and changed meanwhile.
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
    /** The sentence under the menus that says what the chosen What Varies gives. */
    static juce::String whatVariesSentence(VariationRenderer::Mode mode, float amount);

    static constexpr int c_panelWidth = 760, c_panelHeight = 590;

    /** What Channel Order and File Layout do (their tooltips here and in Settings, after the name). */
    static constexpr const char* c_channelOrderTip =
        "Like Pro Tools writes each width the way Pro Tools itself does: up to 7.1.2 in the standard WAV order with the speakers "
        "labelled in the header, and 7.1.4 in Pro Tools' own order (L C R ...) with each channel named inside the file. WAV Standard "
        "writes 7.1.4 in the standard WAV order with the speakers labelled in the header too, for programs that only read that. "
        "Below 7.1.4 both give the same file, and a FLAC file always has FLAC's own order. The line underneath spells out the "
        "order you get.";
    static constexpr const char* c_fileLayoutTip =
        "Interleaved: one file that holds every channel. Multi-Mono: a mono file for each channel, its name ending in the speaker "
        "as Pro Tools names it (Whoosh_0001.L.wav, Whoosh_0001.C.wav ...). The Takes list shows a set as one take.";

private:
    void timerCallback() override;
    void readSettings();           // the controls from the saved settings and the rig as it is now
    void syncControls();           // what is lit, enabled and showing, and the lines that say what Render will make
    void startBatch();
    void fillMenus();              // the menus' items, with what each one gives beside it
    void fillBatchMenu();          // New Batch Every Time, Repeat Last Batch and the batches rendered lately
    void keepChoices();            // the text boxes into the settings file
    VariationRenderer::Job job() const; // what the controls say
    juce::File folder() const;
    juce::String formatLine() const;
    bool repeatedBatch(VariationRenderer::Batch& batch) const; // the batch Render would repeat, if any
    juce::String planKey(const VariationRenderer::Job& j) const; // every choice that changes what Render makes
    void takeFrom(const VariationRenderer::Batch& batch);      // a batch to repeat: its count, lengths and what varied

    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
    juce::TextEditor m_count, m_length, m_batchName, m_gap;
    juce::ComboBox m_each, m_mode, m_batch, m_names, m_files;
    juce::ComboBox m_width, m_order, m_layout; // the files' width (the panel's own), Channel Order and File Layout (the tape's)
    juce::Slider m_amount;
    juce::TextButton m_choose { "Choose..." }, m_dated { "Dated Folder" };
    juce::TextButton m_render { "Render" }, m_cancel { "Cancel" }, m_show, m_openTakes { "Open Takes" }, m_close { "Close" };
    std::unique_ptr<juce::FileChooser> m_chooser;

    juce::Rectangle<int> m_panel, m_folderText, m_firstText, m_getText, m_variesText, m_bar, m_status, m_amountValue, m_lengthUnit, m_gapUnit, m_orderText;
    std::array<juce::Rectangle<int>, 14> m_captions;
    VariationRenderer::Progress m_shown;
    juce::String m_shownPlan;          // the choices the batch shown was made with (or Render was refused for)
    bool m_planChanged = false;        // a choice has changed since: the batch's Done / Cancelled line has gone
    juce::String m_error;              // a batch that could not start
    juce::String m_firstLine, m_getLine; // what Render will make, worked out when a control changes
    bool m_getProblem = false;         // m_getLine says why Render would be refused (the format, the one file's size)
    double m_whooshSeconds = 3.0, m_rideSeconds = 10.0; // Length, for each kind of file
    enum class Repeat { New = 0, Last, Chosen };
    Repeat m_repeat = Repeat::New;
    std::uint32_t m_repeatNumber = 0;  // Chosen: which batch
    int m_syncedMinute = -1;           // the minute the First File line was worked out in

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VariationsView)
};
