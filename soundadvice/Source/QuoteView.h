#pragma once

#include <JuceHeader.h>

#include "QuoteLibrary.h"
#include "Theme.h"

/** The page: the words, set as large as fits, then who said them and where. Cross-fades
    between entries. The person's name opens their IMDb page and the source line opens the
    article; right-click to copy. */
class QuoteView : public juce::Component,
                  public juce::TooltipClient
{
public:
    explicit QuoteView (const QuoteLibrary& library);

    void setPalette (const Theme::Palette& p);
    /** Shows an entry; fades across unless `animate` is false. */
    void showEntry (int entryIndex, bool animate);

    /** Plain text of the entry on screen, for the clipboard. */
    juce::String getClipboardText() const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

    /** Horizontal room the corner buttons need at the bottom edge. */
    static constexpr int cornerReserve = 64;

private:
    struct Layout
    {
        juce::TextLayout body;
        juce::Rectangle<float> bodyArea, labelArea, ruleArea, nameArea, roleArea, sourceArea;
        juce::String label, name, role, source;
    };

    void rebuildLayout();
    void paintEntry (juce::Graphics&, float alpha, float drift);
    enum class Hover { none, name, source };
    Hover linkAt (juce::Point<float>) const;
    void openLink (Hover);
    void tick();

    const QuoteLibrary& library;
    Theme::Palette palette = Theme::palette (false);

    int shown = -1;        // entry on screen (or fading out)
    int incoming = -1;     // entry waiting to fade in
    float phase = 1.0f;    // 0..1 out, 1..2 in; 2 = settled
    double lastTick = 0.0;
    Hover hover = Hover::none;
    Layout layout;

    juce::VBlankAttachment vblank { this, [this] { tick(); } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QuoteView)
};
