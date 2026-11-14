#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"

#include <map>

/**
 * Takes: the recordings of this run (tape takes, whoosh files, LAST 60s saves and whatever else has registered
 * itself with the processor), newest at the top. An overlay like Settings.
 *
 * Each row: a star to keep it in the list, a little waveform, the name, what it is and how long. Click a row to
 * hear it (it plays over the rig and is not recorded), click again to stop; drag a row out to the Finder / Explorer
 * or straight into a DAW; Show reveals the file; the bin moves it to the system trash.
 */
class TakesView : public juce::Component,
                  public juce::TooltipClient,
                  private juce::Timer,
                  private juce::ChangeListener
{
public:
    TakesView(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts);
    ~TakesView() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void visibilityChanged() override;
    juce::String getTooltip() override;

    /** Short messages for the editor's status line. */
    std::function<void(const juce::String&)> onStatus;

    static constexpr int c_rowHeight = 24;

private:
    enum class Zone { None, Star, Body, Show, Bin };
    struct Row
    {
        juce::Rectangle<int> all, star, wave, name, kind, length, show, bin;
    };
    Row rowBounds(int index) const;
    /** The take and the part of its row under a point (-1 = none). */
    int rowAt(juce::Point<int> p, Zone& zone) const;
    void syncTakes();
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(m_list); }

    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
    juce::TextButton m_close { "Close" }, m_folder { "Show Folder" };
    juce::Rectangle<int> m_panel, m_list, m_hint;

    // Little waveforms, read in the background (declared in this order: the thumbnails use the cache and the formats).
    juce::AudioFormatManager m_formats;
    juce::AudioThumbnailCache m_cache { SwooshbucklerAudioProcessor::c_maxTakes + 4 };
    std::map<int, std::unique_ptr<juce::AudioThumbnail>> m_thumbs; // by take id

    int m_generation = -1, m_playing = -1;
    int m_hoverRow = -1, m_pressRow = -1;
    Zone m_hoverZone = Zone::None, m_pressZone = Zone::None;
    bool m_dragged = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TakesView)
};
