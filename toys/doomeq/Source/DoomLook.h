#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "WadArt.h"

/*
 * DOOM EQ's look: palette, the textures cut from the IWAD, and the LookAndFeel that draws
 * every control, menu and dialog with them (pixel HUD font, bevels, rivets).
 */

namespace Palette
{
    inline const juce::Colour background  { 0xff100e0c };
    inline const juce::Colour text        { 0xffe8dcc0 };
    inline const juce::Colour textDim     { 0xffa89870 };
    inline const juce::Colour gold        { 0xffe8b84a };
    inline const juce::Colour hudYellow   { 0xfff4d442 };
    inline const juce::Colour doomRed     { 0xffd22a1c };
    inline const juce::Colour bevelLight  { 0xffa49c8a };
    inline const juce::Colour bevelDark   { 0xff181512 };

    inline const juce::Colour bands[DoomEQProcessor::numBands] = {
        juce::Colour (0xffe8503c), juce::Colour (0xfff09030), juce::Colour (0xffeed040),
        juce::Colour (0xff70cc58), juce::Colour (0xff4cb4e0), juce::Colour (0xffb078e8)
    };
}

/** The textures every part of the UI shares, cut from the IWAD once. */
struct Textures
{
    explicit Textures (const WadArt&);
    juce::Image background, panel, plate, hellish, hazard, groove;
};

/** The band type box. Shows its own menu, so a pick by the user (not automation, a preset or
    a session recall) can be told apart: onUserPick runs before the new type is applied. */
struct TypeCombo : public juce::ComboBox
{
    std::function<void (int oldIndex, int newIndex)> onUserPick;

    void showPopup() override
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        for (int i = 0; i < getNumItems(); ++i)
            menu.addItem (getItemId (i), getItemText (i), true, getItemId (i) == getSelectedId());

        const int before = getSelectedItemIndex();
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()).withStandardItemHeight (24),
                            [safe = juce::Component::SafePointer<TypeCombo> (this), before] (int result)
                            {
                                if (safe == nullptr || result == 0)
                                    return;
                                const int after = safe->indexOfItemId (result);
                                if (after != before && safe->onUserPick)
                                    safe->onUserPick (before, after);
                                safe->setSelectedId (result); // the attachment sets the parameter
                            });
    }
};

class DoomEQLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DoomEQLookAndFeel (const WadArt&, const Textures&);

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawLabel (juce::Graphics&, juce::Label&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;

    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    void drawPopupMenuUpDownArrow (juce::Graphics&, int width, int height, bool isScrollUpArrow) override;
    int getPopupMenuBorderSize() override { return 4; }

    /** Options for every menu: drawn by this LookAndFeel, at least as wide as the button it drops from. */
    static juce::PopupMenu::Options menuOptionsFor (juce::Component& target, int minWidth = 0);

private:
    const WadArt& art;
    const Textures& tex;

    // Pixel knobs are rendered small then scaled; reuse the render until something changes.
    struct KnobCache { int key = -1; juce::Image image; };
    std::map<const juce::Slider*, KnobCache> knobCache;
};

