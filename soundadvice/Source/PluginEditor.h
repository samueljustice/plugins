#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"
#include "QuoteView.h"
#include "Theme.h"

#include <functional>

/** Popup menus and tooltips in the same paper and type as the page. */
class SoundAdviceLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void setPalette (const Theme::Palette& p);

    juce::Font getPopupMenuFont() override { return Theme::text (17.0f); }
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon,
                            const juce::Colour* textColour) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;
    int getPopupMenuBorderSize() override { return 6; }
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

private:
    Theme::Palette palette = Theme::palette (false);
};

/** A small drawn button: the die, the back chevron, the night toggle. */
class GlyphButton : public juce::Button
{
public:
    enum class Glyph { die, back, moon, sun };

    explicit GlyphButton (Glyph g);

    void setGlyph (Glyph g) { glyph = g; repaint(); }
    void setPalette (const Theme::Palette& p) { palette = p; repaint(); }

    /** Tumbles the die and settles on a new face. */
    void roll();

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    void drawDie (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    void tick();

    Glyph glyph;
    Theme::Palette palette = Theme::palette (false);
    int face = 5;
    float spin = 1.0f;  // 0..1 while tumbling
    double lastTick = 0.0;
    juce::Random random;
    juce::VBlankAttachment vblank { this, [this] { tick(); } };
};

/** "‹  MIXING  ›": steps through the categories; click the name for the full list. */
class CategoryBar : public juce::Component
{
public:
    explicit CategoryBar (const QuoteLibrary& library);

    std::function<void (int)> onChoose;

    void setCategory (int index);
    void setPalette (const Theme::Palette& p) { palette = p; repaint(); }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    enum class Zone { none, prev, name, next };
    Zone zoneAt (juce::Point<float>) const;
    juce::Rectangle<float> nameBounds() const;
    void showMenu();
    void step (int delta);

    const QuoteLibrary& library;
    Theme::Palette palette = Theme::palette (false);
    int category = 0;
    Zone hover = Zone::none;
};

//==============================================================================
class SoundAdviceEditor : public juce::AudioProcessorEditor
{
public:
    explicit SoundAdviceEditor (SoundAdviceProcessor&);
    ~SoundAdviceEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void applyTheme();
    void chooseCategory (int index);
    void rollDice();
    void goBack();
    void refreshButtons();
    void rebuildGrain();

    SoundAdviceProcessor& owner;
    const QuoteLibrary& library;
    SoundAdviceLookAndFeel lookAndFeel;
    Theme::Palette palette;

    QuoteView quote;
    CategoryBar categories;
    GlyphButton dice { GlyphButton::Glyph::die };
    GlyphButton backButton { GlyphButton::Glyph::back };
    GlyphButton nightButton { GlyphButton::Glyph::moon };
    juce::TooltipWindow tooltips { this, 900 };

    juce::Image grain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundAdviceEditor)
};
