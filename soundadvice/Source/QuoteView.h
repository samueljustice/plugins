#pragma once

#include <JuceHeader.h>

#include "QuoteLibrary.h"
#include "Theme.h"

/** The page: the words, set as large as fits, then who said them and where. A new entry is
    written in: each letter's outline is drawn in the accent ink, then fills, and a flourish and
    the attribution follow. The person's name opens their IMDb page and the source line opens the
    article; right-click to copy. */
class QuoteView : public juce::Component,
                  public juce::TooltipClient
{
public:
    explicit QuoteView (const QuoteLibrary& library);

    void setPalette (const Theme::Palette& p);
    /** Shows an entry. With `animate` the old one fades and the new one is written in. */
    void showEntry (int entryIndex, bool animate);
    /** Shows an entry at once, blank, and starts writing it after `delaySeconds` (used under a turning page). */
    void showEntryAfter (int entryIndex, double delaySeconds);
    /** Writes the current entry in the next time the page is on screen (used when the window opens). */
    void writeInWhenShown() { introPending = true; }
    /** For the preview tool: freezes the writing at this many seconds in (a large value = finished). */
    void setInkTimeForPreview (double seconds) { introPending = false; phase = 2.0f; ink = seconds; repaint(); }

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
    /** One letter of the body, ready to be written: its outline as polylines, and where it sits. */
    struct InkGlyph
    {
        std::vector<std::vector<juce::Point<float>>> contours;   // flattened outline, layout coordinates
        std::vector<float> lengths;                              // length of each contour
        juce::Path outline;                                      // the same outline, for filling
        juce::Point<float> position;                             // glyph anchor, layout coordinates
        juce::uint16 code = 0;
        float start = 0.0f;                                      // seconds after the writing begins
    };

    struct Layout
    {
        juce::TextLayout body;
        juce::Rectangle<float> bodyArea, labelArea, ruleArea, nameArea, roleArea, sourceArea;
        juce::String label, name, role, source;
        std::vector<InkGlyph> glyphs;      // in reading order
        juce::Font bodyFont { juce::FontOptions() };
        float glyphSeconds = 0.4f;         // how long each letter takes
        float bodyEnd = 0.0f;              // when the last letter starts
        float flourishStart = 0.0f;        // when the flourish and attribution begin
        float inkEnd = 0.0f;               // when everything has settled
    };

    void rebuildLayout();
    void paintEntry (juce::Graphics&, float alpha, float drift);
    void paintWriting (juce::Graphics&, double inkTime);
    void paintBodyInk (juce::Graphics&, juce::Point<float> origin, float inkTime);
    void paintFlourish (juce::Graphics&, float grow, float alpha);
    void paintFurniture (juce::Graphics&, float labelAlpha, float nameAlpha, float roleAlpha, float sourceAlpha, float drift);
    double currentInk() const;
    enum class Hover { none, name, source };
    Hover linkAt (juce::Point<float>) const;
    void openLink (Hover);
    void tick();

    const QuoteLibrary& library;
    Theme::Palette palette = Theme::palette (false);

    int shown = -1;        // entry on screen (or fading out)
    int incoming = -1;     // entry waiting to fade in
    float phase = 2.0f;    // 0..1: the old entry fades out; 2: the entry on screen is `shown`
    double ink = 1.0e6;    // seconds since the writing of `shown` began (large = finished)
    bool introPending = false;
    double lastTick = 0.0;
    Hover hover = Hover::none;
    Layout layout;

    juce::VBlankAttachment vblank { this, [this] { tick(); } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QuoteView)
};
