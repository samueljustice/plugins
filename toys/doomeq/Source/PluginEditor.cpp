#include "PluginEditor.h"
#include "SammyJUpdateCheck.h"

extern "C"
{
#include "doomkeys.h"
}

namespace
{
    constexpr float minFreq = 20.0f, maxFreq = 20000.0f;
    constexpr float displayDbRange = 18.0f;
    constexpr float handleRadius = 10.0f;
    constexpr int stripsHeight = 268;
    constexpr int controlRowHeight = 34;

    // Preset menu item IDs: factory presets are 1..getNumFactory(), user presets count up from
    // firstUserItemId (as in the other Sammy J plug-ins).
    constexpr int saveAsItemId = 1001, deleteItemId = 1002, revealItemId = 1003, aboutItemId = 1004;
    constexpr int firstUserItemId = 100000;
    constexpr int minWidth = 1000, minHeight = 720;
    constexpr int headerHeight = 58;
    constexpr float butterworthQ = 0.70710678f;

    juce::String formatFreq (float f)
    {
        return f < 1000.0f ? juce::String (f, f < 100.0f ? 1 : 0) + " HZ"
                           : juce::String (f / 1000.0f, f < 10000.0f ? 2 : 1) + " KHZ";
    }

    juce::String formatDb (float db)
    {
        if (std::abs (db) < 0.05f)
            db = 0.0f;
        return (db > 0.0f ? "+" : "") + juce::String (db, 1) + " DB";
    }

    /** Shift turns 1-0 into !@#... (and on AZERTY the digits need Shift): map them back,
        so weapons switch while running and on any layout. */
    int unshiftDigit (int c)
    {
        static const char us[] = "!@#$%^&*()";
        for (int i = 0; i < 10; ++i)
            if (c == us[i])
                return i == 9 ? '0' : '1' + i;

        // AZERTY's unshifted digit row, only the characters that can't be plain US keys
        // (- ' " _ are, so those digits need Shift there, which gives the digit itself).
        static const std::pair<juce::juce_wchar, char> azerty[] = { { 0xE9, '2' }, { 0xE8, '7' }, { 0xE7, '9' }, { 0xE0, '0' } };
        for (auto [ch, digit] : azerty)
            if (c == (int) ch || c == (int) juce::CharacterFunctions::toUpperCase (ch))
                return digit;

        return c;
    }

    /** The character a key types, for cheats and savegame names (lower-case ASCII), or 0. */
    int typedCharFor (const juce::KeyPress& k)
    {
        // What the user actually typed: a hyphen stays a hyphen in a savegame name.
        int code = (int) k.getTextCharacter();
        if (code == 0)
            code = k.getKeyCode();
        if (code >= 'A' && code <= 'Z')
            code = code - 'A' + 'a';
        return (code >= 32 && code < 127) ? code : 0;
    }

    /** JUCE key -> doomkeys.h code. Arrows and WASD move, F/Ctrl fire, Space/E use. */
    int doomKeyFor (const juce::KeyPress& k)
    {
        int code = unshiftDigit (k.getKeyCode());

        if (code == juce::KeyPress::upKey)         return KEY_UPARROW;
        if (code == juce::KeyPress::downKey)       return KEY_DOWNARROW;
        if (code == juce::KeyPress::leftKey)       return KEY_LEFTARROW;
        if (code == juce::KeyPress::rightKey)      return KEY_RIGHTARROW;
        if (code == juce::KeyPress::returnKey)     return KEY_ENTER;
        if (code == juce::KeyPress::escapeKey)     return KEY_ESCAPE;
        if (code == juce::KeyPress::tabKey)        return KEY_TAB;
        if (code == juce::KeyPress::backspaceKey)  return KEY_BACKSPACE;
        if (code == juce::KeyPress::deleteKey)     return KEY_BACKSPACE;
        if (code == juce::KeyPress::spaceKey)      return KEY_USE;
        if (code == juce::KeyPress::pageUpKey)     return KEY_PGUP;
        if (code == juce::KeyPress::pageDownKey)   return KEY_PGDN;
        if (code == juce::KeyPress::homeKey)       return KEY_HOME;
        if (code == juce::KeyPress::endKey)        return KEY_END;

        static const int fKeys[] = { juce::KeyPress::F1Key, juce::KeyPress::F2Key, juce::KeyPress::F3Key, juce::KeyPress::F4Key,
                                     juce::KeyPress::F5Key, juce::KeyPress::F6Key, juce::KeyPress::F7Key, juce::KeyPress::F8Key,
                                     juce::KeyPress::F9Key, juce::KeyPress::F10Key, juce::KeyPress::F11Key, juce::KeyPress::F12Key };
        static const int doomF[] = { KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12 };
        for (int i = 0; i < 12; ++i)
            if (code == fKeys[i])
                return doomF[i];

        if (code >= 'A' && code <= 'Z')
            code = code - 'A' + 'a';

        switch (code)
        {
            case 'w': return KEY_UPARROW;
            case 's': return KEY_DOWNARROW;
            case 'a': return KEY_STRAFE_L;
            case 'd': return KEY_STRAFE_R;
            case 'f': return KEY_FIRE;
            case 'e': return KEY_USE;
            default: break;
        }

        if (code >= 32 && code < 127)
            return code;

        return 0;
    }

    juce::Image firstFlat (const WadArt& art, std::initializer_list<const char*> names)
    {
        for (auto* n : names)
            if (auto img = art.flat (n); img.isValid())
                return img;
        return {};
    }

    void drawPlate (juce::Graphics& g, juce::Rectangle<int> r, const juce::Image& tile, float brightness, bool pressed, int bevelSize = 2)
    {
        DoomStyle::fillTextured (g, r.toFloat(), tile, 2.0f, brightness, r.getPosition().toFloat());
        DoomStyle::bevel (g, r, bevelSize,
                          (pressed ? Palette::bevelDark : Palette::bevelLight).withAlpha (0.9f),
                          (pressed ? Palette::bevelLight : Palette::bevelDark).withAlpha (0.9f));
    }
}

//==============================================================================
Textures::Textures (const WadArt& art)
{
    background = firstFlat (art, { "AQF026", "FLOOR4_8", "CEIL5_1" });
    panel      = firstFlat (art, { "FLAT23", "FLAT20", "CEIL5_2" });
    plate      = firstFlat (art, { "FLAT20", "FLAT23", "FLOOR5_1" });
    hellish    = firstFlat (art, { "SLIME09", "BLOOD1", "LAVA1" });
    hazard     = firstFlat (art, { "AQF018", "FLAT5_4" });
    groove     = firstFlat (art, { "FCGRATE1", "AQF050", "FLAT1" });
}

//==============================================================================
DoomEQLookAndFeel::DoomEQLookAndFeel (const WadArt& a, const Textures& t) : art (a), tex (t)
{
    setColour (juce::ResizableWindow::backgroundColourId, Palette::background);
    setColour (juce::ComboBox::textColourId, Palette::gold);
    setColour (juce::ComboBox::arrowColourId, Palette::gold);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1a1613));
    setColour (juce::PopupMenu::textColourId, Palette::gold);
    setColour (juce::Slider::textBoxTextColourId, Palette::hudYellow);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, Palette::doomRed);
    setColour (juce::Label::textColourId, Palette::textDim);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0b0a08));
    setColour (juce::TextEditor::textColourId, Palette::hudYellow);
    setColour (juce::TextEditor::highlightColourId, Palette::doomRed);
    setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::CaretComponent::caretColourId, Palette::hudYellow);
    setColour (juce::TextButton::buttonColourId, juce::Colours::grey);
    setColour (juce::TextButton::textColourOffId, Palette::gold);
    setColour (juce::TextButton::textColourOnId, Palette::hudYellow);
    setColour (juce::PopupMenu::headerTextColourId, Palette::textDim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::doomRed.withAlpha (0.55f));
    setColour (juce::PopupMenu::highlightedTextColourId, Palette::hudYellow);
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff17130f));
    setColour (juce::TooltipWindow::textColourId, Palette::text);
    setColour (juce::TooltipWindow::outlineColourId, Palette::bevelLight);
}

void DoomEQLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                          float startAngle, float endAngle, juce::Slider& slider)
{
    // Drawn small, alpha snapped, then blown up 3x with nearest-neighbour: chunky pixel knobs.
    constexpr int px = 3;
    const int size = juce::jmin (w, h);
    const int lw = juce::jmax (10, size / px);
    const auto accent = slider.isEnabled() ? slider.findColour (juce::Slider::rotarySliderFillColourId) : juce::Colour (0xff4a443a);
    const float angle = startAngle + pos * (endAngle - startAngle);

    const int key = (int) (pos * 1000.0f) * 4096 + lw * 2 + (slider.isEnabled() ? 1 : 0)
                    + (int) (accent.getARGB() & 0xffu) * 7919;
    auto& cached = knobCache[&slider];
    if (cached.key == key && cached.image.isValid())
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        const float d = (float) (lw * px);
        g.drawImage (cached.image, { x + (w - d) * 0.5f, y + (h - d) * 0.5f, d, d });
        return;
    }

    juce::Image img (juce::Image::ARGB, lw, lw, true);
    {
        juce::Graphics gi (img);
        const float c = lw * 0.5f;
        const float arcR = lw * 0.5f - 1.2f;

        juce::Path track;
        track.addCentredArc (c, c, arcR, arcR, 0.0f, startAngle, endAngle, true);
        gi.setColour (juce::Colour (0xff0e0c0a));
        gi.strokePath (track, juce::PathStrokeType (2.2f));

        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
        juce::Path fill;
        fill.addCentredArc (c, c, arcR, arcR, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        gi.setColour (accent);
        gi.strokePath (fill, juce::PathStrokeType (2.2f));

        const float br = arcR - 3.0f;
        gi.setColour (juce::Colour (0xff0a0908));
        gi.fillEllipse (c - br + 0.8f, c - br + 0.8f, br * 2.0f, br * 2.0f);
        gi.setGradientFill (juce::ColourGradient (juce::Colour (0xffb8ae98), c - br * 0.55f, c - br * 0.55f,
                                                  juce::Colour (0xff2a2520), c + br * 0.9f, c + br * 0.9f, true));
        gi.fillEllipse (c - br, c - br, br * 2.0f, br * 2.0f);
        gi.setColour (juce::Colour (0xff1a1714));
        gi.drawEllipse (c - br, c - br, br * 2.0f, br * 2.0f, 1.0f);

        const auto centre = juce::Point<float> (c, c);
        gi.setColour (slider.isEnabled() ? juce::Colour (0xfff8e8b0) : juce::Colour (0xff6a6458));
        gi.drawLine ({ centre.getPointOnCircumference (br * 0.2f, angle), centre.getPointOnCircumference (br - 0.8f, angle) }, 1.6f);
    }
    DoomStyle::crispen (img);
    cached.key = key;
    cached.image = img;

    juce::Graphics::ScopedSaveState ss (g);
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    const float d = (float) (lw * px);
    g.drawImage (img, { x + (w - d) * 0.5f, y + (h - d) * 0.5f, d, d });
}

void DoomEQLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                          juce::Slider::SliderStyle, juce::Slider& slider)
{
    // A segmented health-bar.
    const auto accent = slider.findColour (juce::Slider::trackColourId);
    const auto r = juce::Rectangle<int> (x, y + h / 2 - 8, w, 16);
    g.setColour (juce::Colour (0xff0b0a08));
    g.fillRect (r);
    DoomStyle::bevel (g, r, 2, Palette::bevelDark, Palette::bevelLight.withAlpha (0.6f));

    const auto inner = r.reduced (4, 4);
    const int segW = 4, gap = 2;
    const int n = juce::jmax (1, (inner.getWidth() + gap) / (segW + gap));
    const float frac = juce::jlimit (0.0f, 1.0f, (pos - (float) x) / (float) juce::jmax (1, w));
    const int lit = juce::roundToInt (frac * n);

    for (int i = 0; i < n; ++i)
    {
        auto seg = juce::Rectangle<int> (inner.getX() + i * (segW + gap), inner.getY(), segW, inner.getHeight());
        g.setColour (i < lit ? accent.withMultipliedBrightness (0.75f + 0.35f * (float) i / (float) n) : juce::Colour (0xff2a2420));
        g.fillRect (seg);
        if (i < lit)
        {
            g.setColour (accent.brighter (0.5f));
            g.fillRect (seg.removeFromTop (1));
        }
    }

    // While you point at or drag it, the bar shows its value in the HUD font (instead of
    // JUCE's popup bubble, which can't use the pixel font).
    if (slider.isMouseOverOrDragging())
    {
        const auto text = slider.getTextFromValue (slider.getValue()).toUpperCase();
        const float tw = (float) art.font().widthOf (text) * 1.5f + 10.0f;
        const auto box = juce::Rectangle<float> (juce::jmax ((float) inner.getX(), (float) inner.getRight() - tw), (float) r.getY() + 2.0f,
                                                 juce::jmin (tw, (float) inner.getWidth()), (float) r.getHeight() - 4.0f);
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.fillRect (box);
        art.font().drawFitted (g, text, box.reduced (4.0f, 1.0f), juce::Justification::centredRight, Palette::hudYellow, 1.5f);
    }
}

juce::StringArray DoomEQLookAndFeel::wrapPixelText (const juce::String& text, float scale, float maxWidth) const
{
    juce::StringArray lines;
    juce::String line;
    for (auto word : juce::StringArray::fromTokens (text.toUpperCase(), " \n", ""))
    {
        if (word.isEmpty())
            continue;
        const auto candidate = line.isEmpty() ? word : line + " " + word;
        if (line.isNotEmpty() && (float) art.font().widthOf (candidate) * scale > maxWidth)
        {
            lines.add (line);
            line = word;
        }
        else
        {
            line = candidate;
        }
    }
    if (line.isNotEmpty())
        lines.add (line);
    return lines;
}

namespace
{
    constexpr float tipScale = 1.5f, tipLineH = 15.0f, tipPad = 10.0f, tipMaxText = 340.0f;
}

juce::Rectangle<int> DoomEQLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    const auto lines = wrapPixelText (tipText, tipScale, tipMaxText);
    float textW = 0.0f;
    for (auto& l : lines)
        textW = juce::jmax (textW, (float) art.font().widthOf (l) * tipScale);

    const int w = (int) std::ceil (textW + tipPad * 2.0f);
    const int h = (int) std::ceil ((float) lines.size() * tipLineH + tipPad * 2.0f - 4.0f);

    // Below and right of the pointer, kept on screen (above it if there's no room below).
    auto x = screenPos.x + 12, y = screenPos.y + 18;
    if (x + w > parentArea.getRight()) x = parentArea.getRight() - w;
    if (y + h > parentArea.getBottom()) y = screenPos.y - h - 6;
    return juce::Rectangle<int> (x, y, w, h).constrainedWithin (parentArea);
}

void DoomEQLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto r = juce::Rectangle<int> (width, height);
    DoomStyle::fillTextured (g, r.toFloat(), tex.panel, 2.0f, 0.3f);
    DoomStyle::bevel (g, r, 2, Palette::bevelLight.withAlpha (0.8f), Palette::bevelDark);

    auto y = tipPad - 2.0f;
    for (auto& line : wrapPixelText (text, tipScale, tipMaxText))
    {
        art.font().draw (g, line, tipPad, y, tipScale, Palette::text);
        y += tipLineH;
    }
}

void DoomEQLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    const auto r = b.getLocalBounds();
    const bool on = b.getToggleState();
    const auto accent = b.findColour (juce::ToggleButton::tickColourId);

    drawPlate (g, r, tex.plate, on ? 0.62f : (highlighted ? 0.48f : 0.4f), on);

    // Square LED
    const auto led = juce::Rectangle<int> (r.getX() + 8, r.getCentreY() - 5, 10, 10);
    g.setColour (juce::Colour (0xff0a0908));
    g.fillRect (led.expanded (1));
    g.setColour (on ? accent : accent.withMultipliedBrightness (0.25f));
    g.fillRect (led);
    if (on)
    {
        g.setColour (accent.brighter (0.7f));
        g.fillRect (led.getX() + 1, led.getY() + 1, 4, 2);
    }

    art.font().drawFitted (g, b.getButtonText(), r.withTrimmedLeft (22).reduced (4, 3).toFloat(),
                           juce::Justification::centred, on ? Palette::hudYellow : Palette::gold);
}

void DoomEQLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down)
{
    const bool on = b.getToggleState();
    drawPlate (g, b.getLocalBounds(), tex.plate, on ? 0.6f : (highlighted ? 0.5f : 0.42f), on || down, 3);
    if (on)
    {
        g.setColour (Palette::doomRed.withAlpha (0.35f));
        g.fillRect (b.getLocalBounds().reduced (3));
    }
}

void DoomEQLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool down)
{
    auto r = b.getLocalBounds().reduced (6, 4).toFloat();
    if (down || b.getToggleState())
        r.translate (1.0f, 1.0f);
    art.font().drawFitted (g, b.getButtonText(), r, juce::Justification::centred,
                           b.getToggleState() ? Palette::hudYellow : Palette::gold);
}

void DoomEQLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<int> (0, 0, w, h);
    g.setColour (juce::Colour (0xff0d0b09));
    g.fillRect (r);
    DoomStyle::bevel (g, r, 2, Palette::bevelDark, Palette::bevelLight.withAlpha (0.55f));

    // Pixel arrow
    const int ax = w - 16, ay = h / 2 - 2;
    g.setColour (box.findColour (juce::ComboBox::arrowColourId));
    for (int i = 0; i < 4; ++i)
        g.fillRect (ax + i, ay + i, 8 - i * 2, 1);
}

void DoomEQLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (6, 2, box.getWidth() - 26, box.getHeight() - 4);
}

void DoomEQLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    if (label.isBeingEdited())
        return;

    const bool isValueBox = dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr;
    const bool inCombo = dynamic_cast<juce::ComboBox*> (label.getParentComponent()) != nullptr;
    auto r = label.getLocalBounds();

    if (isValueBox)
    {
        g.setColour (juce::Colour (0xff0a0908));
        g.fillRect (r);
        DoomStyle::bevel (g, r, 1, Palette::bevelDark, Palette::bevelLight.withAlpha (0.4f));
        r = r.reduced (3, 2);
    }

    const auto colour = inCombo ? Palette::gold : label.findColour (juce::Label::textColourId);
    // A value box's size lives on its slider: JUCE recreates the box's label at times.
    float maxScale = label.getProperties().getWithDefault ("pixelScale", 2.0f);
    if (auto* slider = dynamic_cast<juce::Slider*> (label.getParentComponent()))
        maxScale = slider->getProperties().getWithDefault ("valueScale", maxScale);
    art.font().drawFitted (g, label.getText(), r.toFloat(), inCombo ? juce::Justification::centredLeft : label.getJustificationType(),
                           colour.withMultipliedAlpha (label.isEnabled() ? 1.0f : 0.4f), maxScale);
}

juce::Label* DoomEQLookAndFeel::createSliderTextBox (juce::Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setJustificationType (juce::Justification::centred);
    l->setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold)));
    l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::textColourId, Palette::hudYellow);
    return l;
}

void DoomEQLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    const auto r = juce::Rectangle<int> (0, 0, w, h);
    DoomStyle::fillTextured (g, r.toFloat(), tex.panel, 2.0f, 0.25f);
    DoomStyle::bevel (g, r, 2, Palette::bevelLight.withAlpha (0.7f), Palette::bevelDark);
}

void DoomEQLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                           bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                           const juce::String&, const juce::Drawable*, const juce::Colour*)
{
    if (isSeparator)
    {
        g.setColour (Palette::bevelDark);
        g.fillRect (area.reduced (8, 0).withHeight (1).withY (area.getCentreY()));
        g.setColour (Palette::bevelLight.withAlpha (0.4f));
        g.fillRect (area.reduced (8, 0).withHeight (1).withY (area.getCentreY() + 1));
        return;
    }

    if (isHighlighted && isActive)
    {
        g.setColour (Palette::doomRed.withAlpha (0.55f));
        g.fillRect (area.reduced (3, 1));
    }

    auto r = area.reduced (10, 3);
    const float alpha = isActive ? 1.0f : 0.4f;

    // Tick column: a small square, like the HUD's key boxes.
    auto tickColumn = r.removeFromLeft (22);
    if (isTicked)
    {
        const auto box = juce::Rectangle<int> (8, 8).withCentre (tickColumn.getCentre()).withX (tickColumn.getX() + 2);
        g.setColour (Palette::hudYellow.withMultipliedAlpha (alpha));
        g.fillRect (box);
        g.setColour (Palette::hudYellow.brighter (0.6f).withMultipliedAlpha (alpha));
        g.fillRect (box.getX(), box.getY(), 4, 2);
    }

    // Submenu arrow column.
    if (hasSubMenu)
    {
        const auto arrowArea = r.removeFromRight (20);
        const auto arrowColour = (isHighlighted ? Palette::hudYellow : Palette::textDim).withMultipliedAlpha (alpha);
        const int ax = arrowArea.getCentreX() - 2, ay = arrowArea.getCentreY();
        g.setColour (arrowColour);
        for (int i = 0; i < 4; ++i)
            g.fillRect (ax + i, ay - 3 + i, 1, 7 - i * 2); // a pixel triangle
    }

    // "NAME (FILE)" for duplicate preset names: the qualifier is dimmer.
    const auto mainColour = (isHighlighted ? Palette::hudYellow : Palette::gold).withMultipliedAlpha (alpha);
    const int paren = text.lastIndexOf (" (");
    if (paren > 0 && text.endsWithChar (')'))
    {
        const auto head = text.substring (0, paren), tail = text.substring (paren);
        const float headW = juce::jmin ((float) r.getWidth() * 0.7f, art.font().widthOf (head) * 2.0f);
        art.font().drawFitted (g, head, r.toFloat().withWidth (headW), juce::Justification::centredLeft, mainColour);
        art.font().drawFitted (g, tail, r.toFloat().withTrimmedLeft (headW), juce::Justification::centredLeft,
                               Palette::textDim.withMultipliedAlpha (alpha), 1.5f);
    }
    else
    {
        art.font().drawFitted (g, text, r.toFloat(), juce::Justification::centredLeft, mainColour);
    }
}

void DoomEQLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    auto r = area.reduced (10, 2);
    const auto marker = juce::Rectangle<int> (4, 4).withCentre ({ r.getX() + 4, r.getCentreY() });
    g.setColour (Palette::doomRed);
    g.fillRect (marker);
    r.removeFromLeft (22);

    const float textW = art.font().widthOf (sectionName) * 1.5f;
    art.font().drawFitted (g, sectionName, r.toFloat(), juce::Justification::centredLeft, Palette::textDim, 1.5f);

    // A rule from the end of the name to the right edge.
    const int x0 = r.getX() + (int) textW + 8;
    if (x0 < r.getRight())
    {
        g.setColour (Palette::bevelDark);
        g.fillRect (x0, r.getCentreY(), r.getRight() - x0, 1);
        g.setColour (Palette::bevelLight.withAlpha (0.4f));
        g.fillRect (x0, r.getCentreY() + 1, r.getRight() - x0, 1);
    }
}

void DoomEQLookAndFeel::drawPopupMenuUpDownArrow (juce::Graphics& g, int width, int height, bool isScrollUpArrow)
{
    DoomStyle::fillTextured (g, juce::Rectangle<int> (width, height).toFloat(), tex.panel, 2.0f, 0.3f);
    g.setColour (Palette::gold);
    const int cx = width / 2, cy = height / 2;
    for (int i = 0; i < 4; ++i)
    {
        const int y = isScrollUpArrow ? cy - 2 + i : cy + 1 - i;
        g.fillRect (cx - 3 + i, y, 7 - i * 2, 1);
    }
}

void DoomEQLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool isVertical,
                                       int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown)
{
    // A dark groove with a bevelled metal thumb.
    const auto track = juce::Rectangle<int> (x, y, width, height);
    g.setColour (juce::Colour (0xff0b0a08));
    g.fillRect (track);
    DoomStyle::bevel (g, track, 1, Palette::bevelDark, Palette::bevelLight.withAlpha (0.35f));

    if (thumbSize <= 0)
        return;
    auto thumb = isVertical ? juce::Rectangle<int> (x + 2, thumbStart + 1, width - 4, thumbSize - 2)
                            : juce::Rectangle<int> (thumbStart + 1, y + 2, thumbSize - 2, height - 4);
    DoomStyle::fillTextured (g, thumb.toFloat(), tex.plate, 2.0f, isMouseDown ? 0.75f : isMouseOver ? 0.62f : 0.5f, thumb.getPosition().toFloat());
    DoomStyle::bevel (g, thumb, 1, Palette::bevelLight, Palette::bevelDark);
}

void DoomEQLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
{
    // Three steps of pixel blocks in the corner.
    const auto colour = (isMouseOver || isMouseDragging) ? Palette::hudYellow : Palette::gold.withAlpha (0.6f);
    const int s = juce::jmax (2, juce::jmin (w, h) / 8);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col <= row; ++col)
        {
            const int px = w - (col + 1) * (s + s), py = h - (3 - row) * (s + s);
            g.setColour (juce::Colours::black.withAlpha (0.8f));
            g.fillRect (px + 1, py + 1, s, s);
            g.setColour (colour);
            g.fillRect (px, py, s, s);
        }
}

void DoomEQLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor&)
{
    const auto r = juce::Rectangle<int> (width, height);
    g.setColour (juce::Colour (0xff0a0908));
    g.fillRect (r);
    DoomStyle::bevel (g, r, 1, Palette::bevelDark, Palette::bevelLight.withAlpha (0.5f));
}

void DoomEQLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (editor.hasKeyboardFocus (true))
    {
        g.setColour (Palette::hudYellow.withAlpha (0.8f));
        g.drawRect (0, 0, width, height, 1);
    }
}

juce::PopupMenu::Options DoomEQLookAndFeel::menuOptionsFor (juce::Component& target, int minimumWidth)
{
    return juce::PopupMenu::Options().withTargetComponent (&target).withMinimumWidth (minimumWidth).withStandardItemHeight (24);
}

void DoomEQLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int, int& idealWidth, int& idealHeight)
{
    idealHeight = isSeparator ? 8 : 24;
    idealWidth = art.font().widthOf (text) * 2 + 22 + 24 + 20; // tick column, padding, submenu arrow
}

//==============================================================================
EqDisplay::EqDisplay (DoomEQProcessor& p, const WadArt& a, const Textures& t) : proc (p), art (a), tex (t)
{
    setOpaque (true);
    setWantsKeyboardFocus (false);
    readBands();
}

bool EqDisplay::isTakeCommandKey (const juce::KeyPress& k)
{
    // ` toggles both ways. (Shift+Esc used to leave, but Shift is "run": running and
    // opening DOOM's menu threw you out of the game.)
    return k.getTextCharacter() == '`' || k.getTextCharacter() == '~' || k.getKeyCode() == '`';
}

juce::RangedAudioParameter* EqDisplay::param (int band, const char* name) const
{
    return proc.apvts.getParameter (DoomEQProcessor::pid (band, name));
}

void EqDisplay::setParam (int band, const char* name, float value)
{
    if (auto* p = param (band, name))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

void EqDisplay::setParamGesture (int band, const char* name, float value)
{
    if (auto* p = param (band, name))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture();
    }
}

void EqDisplay::setBandType (int band, int newType)
{
    // Switching a bell or shelf to a cut starts it as a flat Butterworth (Q 0.707), not a
    // resonant one left over from the bell's Q.
    const auto oldType = bandViews[(size_t) band].type;
    if (eq::usesGain (oldType) && eq::isCut ((eq::Type) newType))
        setParamGesture (band, "q", butterworthQ);
    setParamGesture (band, "type", (float) newType);
}

void EqDisplay::endDragGestures()
{
    if (dragBand >= 0)
        for (auto* name : { "freq", "gain", "q" })
            if (auto* p = param (dragBand, name))
                p->endChangeGesture();
    dragBand = -1;
}

EqDisplay::~EqDisplay()
{
    endDragGestures(); // a drag in progress when the window closes

    // Our focusLost can't run once destruction has started, so leave command mode here:
    // held keys and modifiers are released and the game pauses.
    if (commandMode)
        setCommandMode (false);
}

bool EqDisplay::readBands()
{
    const auto before = bandViews;
    const double srBefore = sampleRate;

    sampleRate = proc.getCurrentSampleRate();
    if (sampleRate <= 0.0)
        sampleRate = 44100.0;

    for (int b = 0; b < DoomEQProcessor::numBands; ++b)
    {
        auto& v = bandViews[(size_t) b];
        auto& ap = proc.apvts;
        v.on   = ap.getRawParameterValue (DoomEQProcessor::pid (b, "on"))->load() > 0.5f;
        v.type = (eq::Type) juce::jlimit (0, eq::numTypes - 1, (int) ap.getRawParameterValue (DoomEQProcessor::pid (b, "type"))->load());
        v.freq = ap.getRawParameterValue (DoomEQProcessor::pid (b, "freq"))->load();
        v.gain = ap.getRawParameterValue (DoomEQProcessor::pid (b, "gain"))->load();
        v.q    = ap.getRawParameterValue (DoomEQProcessor::pid (b, "q"))->load();
        v.coeffs = eq::design (v.type, v.freq, v.gain, v.q, sampleRate);
    }

    bool changed = sampleRate != srBefore;
    for (size_t b = 0; b < bandViews.size(); ++b)
    {
        const auto& x = before[b];
        const auto& y = bandViews[b];
        changed |= x.on != y.on || x.type != y.type || x.freq != y.freq || x.gain != y.gain || x.q != y.q;
    }
    return changed;
}

float EqDisplay::xForFreq (float f) const
{
    return (float) getWidth() * std::log (f / minFreq) / std::log (maxFreq / minFreq);
}

float EqDisplay::freqForX (float x) const
{
    return minFreq * std::pow (maxFreq / minFreq, juce::jlimit (0.0f, 1.0f, x / (float) getWidth()));
}

float EqDisplay::yForDb (float db) const
{
    const float h = (float) getHeight();
    const float usable = h * 0.5f - 26.0f;
    return h * 0.5f - db / displayDbRange * usable;
}

float EqDisplay::dbForY (float y) const
{
    const float h = (float) getHeight();
    const float usable = h * 0.5f - 26.0f;
    return (h * 0.5f - y) / usable * displayDbRange;
}

juce::Point<float> EqDisplay::handlePosition (int band) const
{
    const auto& v = bandViews[(size_t) band];
    // A cut's node height is its resonance: on the 0 dB line it's a flat Butterworth.
    const float db = eq::usesGain (v.type) ? v.gain
                                           : juce::jlimit (-displayDbRange, displayDbRange, 20.0f * std::log10 (v.q / butterworthQ));
    return { xForFreq (v.freq), yForDb (db) };
}

int EqDisplay::hitTestHandle (juce::Point<float> p) const
{
    for (int b = DoomEQProcessor::numBands - 1; b >= 0; --b)
        if (handlePosition (b).getDistanceFrom (p) <= handleRadius + 4.0f)
            return b;
    return -1;
}

juce::Rectangle<float> EqDisplay::doomArea() const
{
    // Whole frame, as big as fits.
    return juce::RectanglePlacement (juce::RectanglePlacement::centred)
        .appliedTo (juce::Rectangle<float> (0, 0, DoomEngine::width, DoomEngine::height), getLocalBounds().reduced (4).toFloat())
        .toNearestInt().toFloat();
}

void EqDisplay::setCommandMode (bool inCommand)
{
    if (inCommand == commandMode)
        return;

    commandMode = inCommand;
    setWantsKeyboardFocus (commandMode);
    if (commandMode)
    {
        showHelp = false;
        grabKeyboardFocus();
    }
    else
    {
        releaseAllKeys();
    }
    // Leaving command pauses your game; taking it again resumes. Process-wide, on the DOOM thread.
    DoomEngine::requestPause (! commandMode);
    needsFullRepaint = true;
}

void EqDisplay::setHelpVisible (bool shouldShow)
{
    showHelp = shouldShow;
    repaint();
}

void EqDisplay::setOverlayOpacity (float v)
{
    if (overlay == juce::jlimit (0.0f, 1.0f, v))
        return;
    overlay = juce::jlimit (0.0f, 1.0f, v);
    repaint();
}

void EqDisplay::resized()
{
    rebuildBackdrop();
}

void EqDisplay::rebuildBackdrop()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    backdrop = juce::Image (juce::Image::RGB, getWidth(), getHeight(), true);
    juce::Graphics g (backdrop);
    const auto r = getLocalBounds();

    DoomStyle::fillTextured (g, r.toFloat(), tex.hellish, 3.0f, 0.32f);

    // Sunken screen bezel around the game.
    const auto screen = doomArea().toNearestInt();
    g.setColour (juce::Colours::black);
    g.fillRect (screen.expanded (4));
    DoomStyle::bevel (g, screen.expanded (4), 3, Palette::bevelDark, Palette::bevelLight.withAlpha (0.55f));

    DoomStyle::bevel (g, r, 3, Palette::bevelDark, Palette::bevelLight.withAlpha (0.6f));
}

void EqDisplay::tick()
{
    // Modifier changes go to the component under the mouse, which may not be this one: poll
    // them, so fire/run/strafe never stay held in the game.
    if (commandMode && hasKeyboardFocus (false))
        syncModifiers (juce::ModifierKeys::getCurrentModifiersRealtime());

    const bool bandsChanged = readBands();
    const bool spectrumChanged = updateAnalyser();
    const bool frameChanged = DoomEngine::fetchFrame (doomFrame, doomSerial);

    // Booting -> running or failed: show it even if nothing else changes.
    const int engineState = (int) DoomEngine::getState();
    const bool engineChanged = engineState != lastEngineState;
    lastEngineState = engineState;

    const auto hint = currentHint();
    const bool hintChanged = hint != lastHint;
    lastHint = hint;

    const bool overlayVisible = overlay > 0.0f;

    if (needsFullRepaint || hintChanged || (overlayVisible && (bandsChanged || spectrumChanged)) || (! overlayVisible && bandsChanged))
    {
        repaint();
        needsFullRepaint = false;
    }
    else if (frameChanged || engineChanged)
    {
        repaint (doomArea().toNearestInt()); // only the game moved
    }
}

juce::String EqDisplay::currentHint() const
{
    const int flags = DoomEngine::getGameFlags();
    const bool inGame = (flags & DoomEngine::inLevel) != 0, menuOpen = (flags & DoomEngine::menuOpen) != 0;
    const bool editorHasFocus = getParentComponent() != nullptr && getParentComponent()->hasKeyboardFocus (true);

    if (! commandMode)
    {
        if (! editorHasFocus)
            return "CLICK HERE, THEN PRESS `";
        if (inGame && (flags & DoomEngine::pausedByHost) != 0)
            return "GAME PAUSED   PRESS ` TO PLAY ON";
        return "PRESS ` TO TAKE COMMAND";
    }

    if (! hasKeyboardFocus (false))
        return "CLICK THE SCREEN TO PLAY ON";
    if (! inGame && ! menuOpen)
        return "ESC FOR MENU, THEN NEW GAME   ` LEAVES";
    return "IN COMMAND   ` TO LEAVE";
}

bool EqDisplay::updateAnalyser()
{
    const int got = proc.pullAnalyserSamples (pullScratch.data(), (int) pullScratch.size());
    for (int i = 0; i < got; ++i)
    {
        ring[(size_t) ringPos] = pullScratch[(size_t) i];
        ringPos = (ringPos + 1) % fftSize;
    }

    if (overlay <= 0.0f)
        return false; // nothing to show it on

    // The host stopped sending audio (transport stopped, track asleep): let the bars fall
    // instead of freezing on the last block.
    ticksWithoutAudio = got > 0 ? 0 : ticksWithoutAudio + 1;
    const bool live = ticksWithoutAudio < 4;

    if (live)
    {
        for (int i = 0; i < fftSize; ++i)
            fftData[(size_t) i] = ring[(size_t) ((ringPos + i) % fftSize)];
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

        window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (fftData.data());
    }

    const float norm = 4.0f / (float) fftSize;
    bool visible = false;
    for (int i = 1; i < fftSize / 2; ++i)
    {
        auto& s = spectrumDb[(size_t) i];
        const float db = live ? juce::Decibels::gainToDecibels (fftData[(size_t) i] * norm, -120.0f) : -120.0f;
        s = juce::jmax (-120.0f, db > s ? db : s - 1.6f);
        visible |= s > -110.0f;
    }

    // Repaint while anything shows, plus one more frame to clear the last bars.
    const bool changed = visible || spectrumWasVisible;
    spectrumWasVisible = visible;
    return changed;
}

//==============================================================================
void EqDisplay::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();

    if (backdrop.isValid())
        g.drawImageAt (backdrop, 0, 0);
    else
        g.fillAll (juce::Colours::black);

    const auto screen = doomArea();
    if (DoomEngine::getState() == DoomEngine::State::Running)
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (doomFrame, screen);
    }
    else
    {
        art.font().drawFitted (g, DoomEngine::getStatusText(), screen.withTrimmedTop (screen.getHeight() * 0.45f).withHeight (30.0f),
                               juce::Justification::centred, Palette::doomRed, 3.0f);
    }

    if (overlay > 0.0f)
    {
        updateLayers ((float) g.getInternalContext().getPhysicalPixelScaleFactor());

        // A light wash so the curve reads, scaled by the overlay amount.
        g.setColour (juce::Colours::black.withAlpha (overlay * (commandMode ? 0.12f : 0.3f)));
        g.fillRect (r);

        g.setOpacity (overlay);
        g.drawImage (gridLayer, r);
        paintSpectrum (g, r);
        g.setOpacity (overlay);
        g.drawImage (curveLayer, r);
    }
    paintHandles (g);

    // Readout for the band under the mouse.
    const int b = dragBand >= 0 ? dragBand : hoverBand;
    if (b >= 0)
    {
        const auto& v = bandViews[(size_t) b];
        juce::String s = juce::String (b + 1) + " " + juce::String (eq::typeNames[(int) v.type]).toUpperCase() + "  " + formatFreq (v.freq);
        if (eq::usesGain (v.type))
            s << "  " << formatDb (v.gain);
        s << "  Q " << juce::String (v.q, 2);

        const float w = art.font().widthOf (s) * 2.0f + 20.0f;
        const auto box = juce::Rectangle<float> (12.0f, 12.0f, w, 26.0f);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.fillRect (box);
        art.font().drawFitted (g, s, box.reduced (8.0f, 5.0f), juce::Justification::centredLeft, Palette::bands[b]);
    }

    // Key hint, top right, sized from its text (the band readout can use the left ~530 px).
    {
        const juce::String hint = lastHint.isNotEmpty() ? lastHint : currentHint();
        const float tw = (float) art.font().widthOf (hint);
        const float maxW = juce::jmax (120.0f, r.getWidth() - 24.0f - 540.0f);
        float sc = 2.0f;
        while (sc > 1.0f && tw * sc + 16.0f > maxW)
            sc -= 0.5f;
        const float bw = tw * sc + 16.0f;
        const auto box = juce::Rectangle<float> (r.getRight() - 12.0f - bw, 12.0f, bw, 24.0f);
        g.setColour (juce::Colours::black.withAlpha (commandMode ? 0.7f : 0.5f));
        g.fillRect (box);
        art.font().drawFitted (g, hint, box.reduced (8.0f, 4.0f), juce::Justification::centredRight,
                               commandMode ? Palette::hudYellow : Palette::gold, sc);
    }

    if (showHelp)
        paintHelp (g, r);
}

void EqDisplay::paintHelp (juce::Graphics& g, juce::Rectangle<float> r)
{
    constexpr int rows = 10;
    static const char* const eqRows[rows][2] = {
        { "DRAG A NODE",    "FREQ + GAIN (CUTS: Q)" },
        { "SHIFT + DRAG",   "FINE ADJUST" },
        { "SCROLL ON NODE", "Q (WIDTH)" },
        { "DBL CLICK NODE", "BAND ON / OFF" },
        { "RIGHT CLICK",    "FILTER TYPE" },
        { "DBL CLICK KNOB", "RESET IT" },
        { "CLICK A VALUE",  "TYPE IT IN" },
        { "OVERLAY",        "FADE EQ OVER GAME" },
        { "BYPASS",         "HEAR IT WITHOUT EQ" },
        { "F1 OR ?",        "THIS HELP" },
    };
    static const char* const doomRows[rows][2] = {
        { "` (BACKTICK)",   "TAKE / LEAVE COMMAND" },
        { "ESC / ENTER",    "MENU / SELECT" },
        { "ARROWS / WASD",  "MOVE" },
        { "F (OR CTRL)",    "FIRE" },
        { "SPACE / E",      "OPEN + USE" },
        { "SHIFT / ALT",    "RUN / STRAFE" },
        { "1 - 7",          "WEAPONS" },
        { "LEAVING",        "PAUSES YOUR GAME" },
        { "DOOM SOUND",     "EQ THE GAME AUDIO" },
        { "NO KEYS?",       "DAW HAS THEM: USE APP" },
    };

    const auto panel = r.withSizeKeepingCentre (juce::jmin (r.getWidth() - 40.0f, 960.0f), juce::jmin (r.getHeight() - 30.0f, 360.0f));
    g.setColour (juce::Colours::black.withAlpha (0.9f));
    g.fillRect (panel);
    DoomStyle::bevel (g, panel.toNearestInt(), 3, Palette::bevelLight.withAlpha (0.8f), Palette::bevelDark);

    auto inner = panel.reduced (22.0f, 14.0f);
    const auto footer = inner.removeFromBottom (18.0f);
    const float colW = inner.getWidth() * 0.5f;
    const float rowH = juce::jmin (26.0f, (inner.getHeight() - 30.0f) / (float) rows);

    auto column = [&] (juce::Rectangle<float> area, const char* title, const char* const table[rows][2], juce::Colour titleColour)
    {
        art.font().drawFitted (g, title, area.removeFromTop (26.0f), juce::Justification::centredLeft, titleColour, 2.0f);
        area.removeFromTop (4.0f);
        const float keyW = area.getWidth() * 0.44f;
        for (int i = 0; i < rows; ++i)
        {
            auto row = area.removeFromTop (rowH).reduced (0.0f, 3.0f);
            art.font().drawFitted (g, table[i][0], row.removeFromLeft (keyW), juce::Justification::centredLeft, Palette::hudYellow, 1.5f);
            art.font().drawFitted (g, table[i][1], row, juce::Justification::centredLeft, Palette::text, 1.5f);
        }
    };

    column (inner.removeFromLeft (colW).withTrimmedRight (14.0f), "THE EQ", eqRows, Palette::hudYellow);
    column (inner.withTrimmedLeft (14.0f), "THE DOOM", doomRows, Palette::doomRed);
    art.font().drawFitted (g, "MAC: CTRL+ARROWS SWITCHES DESKTOPS, SO FIRE WITH F     CLICK TO CLOSE", footer,
                           juce::Justification::centred, Palette::textDim, 1.0f);
}

namespace
{
    struct Hasher
    {
        uint64_t h = 1469598103934665603ull;

        template <typename T>
        void add (T v)
        {
            const auto* p = reinterpret_cast<const unsigned char*> (&v);
            for (size_t i = 0; i < sizeof (T); ++i)
            {
                h ^= p[i];
                h *= 1099511628211ull;
            }
        }
    };
}

void EqDisplay::updateLayers (float scale)
{
    const int w = juce::roundToInt ((float) getWidth() * scale), h = juce::roundToInt ((float) getHeight() * scale);
    if (w <= 0 || h <= 0)
        return;

    const auto r = getLocalBounds().toFloat();

    auto redraw = [&] (juce::Image& layer, auto&& draw)
    {
        if (layer.getWidth() != w || layer.getHeight() != h)
            layer = juce::Image (juce::Image::ARGB, w, h, true);
        else
            layer.clear (layer.getBounds());

        juce::Graphics lg (layer);
        lg.addTransform (juce::AffineTransform::scale (scale));
        draw (lg);
    };

    Hasher grid;
    grid.add (w); grid.add (h); grid.add (commandMode);
    if (grid.h != gridLayerKey)
    {
        redraw (gridLayer, [&] (juce::Graphics& lg) { paintGrid (lg, r); });
        gridLayerKey = grid.h;
    }

    Hasher curves;
    curves.add (w); curves.add (h); curves.add (sampleRate); curves.add (hoverBand); curves.add (dragBand);
    for (const auto& v : bandViews)
    {
        curves.add (v.on); curves.add ((int) v.type); curves.add (v.freq); curves.add (v.gain); curves.add (v.q);
    }
    if (curves.h != curveLayerKey)
    {
        redraw (curveLayer, [&] (juce::Graphics& lg) { paintCurves (lg, r); });
        curveLayerKey = curves.h;
    }
}

void EqDisplay::paintGrid (juce::Graphics& g, juce::Rectangle<float> r)
{
    const float a = (commandMode ? 0.12f : 0.22f);

    static const float freqs[] = { 20, 30, 40, 50, 60, 70, 80, 90, 100, 200, 300, 400, 500, 600, 700, 800, 900,
                                   1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 20000 };
    for (float f : freqs)
    {
        const bool major = (f == 100 || f == 1000 || f == 10000);
        g.setColour (Palette::text.withAlpha (major ? a * 1.6f : a * 0.6f));
        g.drawVerticalLine ((int) xForFreq (f), r.getY(), r.getBottom());
    }

    static const float labelled[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    for (float f : labelled)
    {
        const auto s = f >= 1000 ? juce::String ((int) (f / 1000)) + "K" : juce::String ((int) f);
        art.font().draw (g, s, xForFreq (f) + 4.0f, r.getBottom() - 22.0f, 2.0f, Palette::gold.withAlpha (0.9f));
    }

    for (float db = -18.0f; db <= 18.0f; db += 6.0f)
    {
        const float y = yForDb (db);
        g.setColour (Palette::text.withAlpha (db == 0.0f ? a * 1.8f : a * 0.7f));
        g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
        if (db != 0.0f)
        {
            const auto s = (db > 0 ? "+" : "") + juce::String ((int) db);
            art.font().draw (g, s, r.getRight() - 12.0f - art.font().widthOf (s) * 2.0f, y - 16.0f, 2.0f,
                             Palette::gold.withAlpha (0.9f));
        }
    }
}

void EqDisplay::paintSpectrum (juce::Graphics& g, juce::Rectangle<float> r)
{
    const int w = getWidth();
    if (w <= 0)
        return;

    // Bin -> column and tilt tables, rebuilt only when the width or sample rate changes.
    if (spectrumTableWidth != w || spectrumTableRate != sampleRate)
    {
        spectrumTableWidth = w;
        spectrumTableRate = sampleRate;
        binColumn.assign ((size_t) fftSize / 2, -1);
        binTilt.assign ((size_t) fftSize / 2, 0.0f);
        const float binHz = (float) sampleRate / (float) fftSize;
        for (int i = 1; i < fftSize / 2; ++i)
        {
            const float f = (float) i * binHz;
            if (f < minFreq || f > maxFreq)
                continue;
            binColumn[(size_t) i] = juce::jlimit (0, w - 1, (int) xForFreq (f));
            binTilt[(size_t) i] = 4.5f * std::log2 (f / 1000.0f); // so pink noise sits flat
        }
        columns.assign ((size_t) w, -120.0f);
    }

    std::fill (columns.begin(), columns.end(), -120.0f);
    for (int i = 1; i < fftSize / 2; ++i)
        if (const int x = binColumn[(size_t) i]; x >= 0)
            columns[(size_t) x] = juce::jmax (columns[(size_t) x], spectrumDb[(size_t) i] + binTilt[(size_t) i]);

    float last = -120.0f;
    for (auto& c : columns)
    {
        if (c <= -119.0f) c = last;
        else last = c;
    }

    // Chunky 4 px columns, like an old VU: bodies and caps, two fills in all.
    const float top = r.getY() + 10.0f, bottom = r.getBottom() - 4.0f;
    juce::RectangleList<float> bodies, caps;
    for (int x = 0; x + 3 < w; x += 4)
    {
        const float db = juce::jmax (columns[(size_t) x], columns[(size_t) x + 2]);
        const float y = std::round (juce::jmap (juce::jlimit (-84.0f, 0.0f, db), -84.0f, 0.0f, bottom, top));
        if (y >= bottom - 1.0f)
            continue;
        bodies.addWithoutMerging ({ (float) x, y, 3.0f, bottom - y });
        caps.addWithoutMerging ({ (float) x, y, 3.0f, 2.0f });
    }

    g.setColour (Palette::gold.withAlpha (overlay * (commandMode ? 0.12f : 0.26f)));
    g.fillRectList (bodies);
    g.setColour (Palette::hudYellow.withAlpha (overlay * (commandMode ? 0.25f : 0.55f)));
    g.fillRectList (caps);
}

void EqDisplay::paintCurves (juce::Graphics& g, juce::Rectangle<float> r)
{
    const int w = getWidth();
    const float zeroY = yForDb (0.0f);
    std::vector<double> total ((size_t) w + 1, 0.0);

    for (int b = 0; b < DoomEQProcessor::numBands; ++b)
    {
        const auto& v = bandViews[(size_t) b];
        if (! v.on)
            continue;

        juce::Path fill;
        fill.startNewSubPath (0.0f, zeroY);
        for (int x = 0; x <= w; ++x)
        {
            const double db = eq::responseDb (v.coeffs, freqForX ((float) x), sampleRate);
            total[(size_t) x] += db;
            fill.lineTo ((float) x, juce::jlimit (r.getY() - 2.0f, r.getBottom() + 2.0f, yForDb ((float) db)));
        }
        fill.lineTo ((float) w, zeroY);
        fill.closeSubPath();

        const bool focus = (b == hoverBand || b == dragBand);
        g.setColour (Palette::bands[b].withAlpha ((focus ? 0.3f : 0.13f)));
        g.fillPath (fill);
    }

    juce::Path curve;
    for (int x = 0; x <= w; ++x)
    {
        const float y = juce::jlimit (r.getY() - 2.0f, r.getBottom() + 2.0f, yForDb ((float) total[(size_t) x]));
        if (x == 0) curve.startNewSubPath (0.0f, y);
        else        curve.lineTo ((float) x, y);
    }

    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.strokePath (curve, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::square));
    g.setColour (Palette::doomRed.withAlpha (0.9f));
    g.strokePath (curve, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::square));
    g.setColour (Palette::hudYellow.withAlpha (1.0f));
    g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::square));
}

void EqDisplay::paintHandles (juce::Graphics& g)
{
    for (int b = 0; b < DoomEQProcessor::numBands; ++b)
    {
        const auto& v = bandViews[(size_t) b];
        const bool focus = (b == hoverBand || b == dragBand);
        const float a = focus ? 1.0f : overlay;
        if (a <= 0.01f)
            continue;

        const auto p = handlePosition (b).toInt();
        const auto colour = Palette::bands[b];
        const int s = (int) handleRadius * 2;
        const auto box = juce::Rectangle<int> (s, s).withCentre (p);

        // Square pixel nodes with a bevel, like a pickup.
        g.setColour (juce::Colours::black.withAlpha (0.8f * a));
        g.fillRect (box.expanded (2).translated (1, 1));
        g.setColour ((v.on ? colour : juce::Colour (0xff2a2622)).withAlpha (a));
        g.fillRect (box);
        DoomStyle::bevel (g, box, 2, (v.on ? colour.brighter (0.6f) : colour.withMultipliedBrightness (0.6f)).withAlpha (a),
                          colour.darker (0.8f).withAlpha (a));
        if (focus)
        {
            g.setColour (Palette::hudYellow);
            g.drawRect (box.expanded (4), 2);
        }

        art.font().drawFitted (g, juce::String (b + 1), box.toFloat().reduced (2.0f), juce::Justification::centred,
                               (v.on ? juce::Colours::black : colour).withAlpha (a), 2.0f, false);
    }
}

//==============================================================================
void EqDisplay::mouseMove (const juce::MouseEvent& e)
{
    const int b = hitTestHandle (e.position);
    if (b != hoverBand)
    {
        hoverBand = b;
        setMouseCursor (b >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void EqDisplay::mouseExit (const juce::MouseEvent&)
{
    hoverBand = -1;
    repaint();
}

void EqDisplay::mouseDown (const juce::MouseEvent& e)
{
    if (showHelp)
    {
        setHelpVisible (false);
        return;
    }

    if (commandMode)
    {
        // While playing, a click only gives the game the keyboard back: it must not grab a
        // node (they sit across the middle of the picture) or open the type menu on Ctrl-fire.
        grabKeyboardFocus();
        return;
    }

    if (auto* parent = getParentComponent())
        parent->grabKeyboardFocus(); // so ` reaches the editor

    dragBand = hitTestHandle (e.position);
    if (dragBand < 0)
        return;

    if (e.mods.isPopupMenu())
    {
        const int band = dragBand;
        dragBand = -1;

        juce::PopupMenu m;
        m.setLookAndFeel (&getLookAndFeel()); // a top-level menu only uses the LookAndFeel set on it
        const auto& v = bandViews[(size_t) band];
        m.addItem (1000, v.on ? "Turn band off" : "Turn band on");
        m.addSeparator();
        for (int t = 0; t < eq::numTypes; ++t)
            m.addItem (t + 1, eq::typeNames[t], true, (int) v.type == t);
        m.addSeparator();
        m.addItem (2000, "Reset gain");

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                         [safe = juce::Component::SafePointer<EqDisplay> (this), band] (int result)
                         {
                             if (safe == nullptr)
                                 return; // the window closed while the menu was open
                             if (result == 1000)
                                 safe->setParamGesture (band, "on", safe->bandViews[(size_t) band].on ? 0.0f : 1.0f);
                             else if (result == 2000)
                                 safe->setParamGesture (band, "gain", 0.0f);
                             else if (result > 0)
                                 safe->setBandType (band, result - 1);
                         });
        return;
    }

    // The node follows the mouse from where it is, not from where you grabbed it.
    dragVirtual = handlePosition (dragBand);
    dragLast = e.position;

    for (auto* name : { "freq", "gain", "q" })
        if (auto* p = param (dragBand, name))
            p->beginChangeGesture();
}

void EqDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0)
        return;

    // Shift slows the node down for fine moves.
    const auto delta = e.position - dragLast;
    dragLast = e.position;
    dragVirtual += delta * (e.mods.isShiftDown() ? 0.2f : 1.0f);
    dragVirtual.x = juce::jlimit (0.0f, (float) getWidth(), dragVirtual.x);
    dragVirtual.y = juce::jlimit (yForDb (displayDbRange), yForDb (-displayDbRange), dragVirtual.y);

    setParam (dragBand, "freq", freqForX (dragVirtual.x));

    const float db = juce::jlimit (-displayDbRange, displayDbRange, dbForY (dragVirtual.y));
    if (eq::usesGain (bandViews[(size_t) dragBand].type))
        setParam (dragBand, "gain", db);
    else
        setParam (dragBand, "q", juce::jlimit (0.1f, 10.0f, butterworthQ * std::pow (10.0f, db / 20.0f))); // a cut's height is its resonance

    readBands();
    repaint();
}

void EqDisplay::mouseUp (const juce::MouseEvent&)
{
    endDragGestures();
}

void EqDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (commandMode)
        return;
    const int b = hitTestHandle (e.position);
    if (b >= 0)
        setParamGesture (b, "on", bandViews[(size_t) b].on ? 0.0f : 1.0f);
}

void EqDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (commandMode)
        return;
    const int b = hitTestHandle (e.position);
    if (b < 0)
        return;

    auto* p = param (b, "q");
    if (p == nullptr)
        return;

    const float q = bandViews[(size_t) b].q * std::pow (1.6f, wheel.deltaY * (wheel.isReversed ? -1.0f : 1.0f));
    setParamGesture (b, "q", juce::jlimit (0.1f, 10.0f, q));
    readBands();
    repaint();
}

//==============================================================================
bool EqDisplay::keyPressed (const juce::KeyPress& key)
{
    if (! commandMode)
        return false; // let the editor see ` to take command

    if (isTakeCommandKey (key))
    {
        setCommandMode (false);
        if (onCommandChange)
            onCommandChange (false);
        return true;
    }

    // Cmd+S, Cmd+Z, Cmd+Q... belong to the host. F1 shows our help, not DOOM's.
    if (key.getModifiers().isCommandDown() || key.getKeyCode() == juce::KeyPress::F1Key)
        return false;

    const int code = key.getKeyCode();
    if (heldKeys.count (code) == 0)
    {
        const int dk = doomKeyFor (key);
        if (dk == 0)
            return false;
        heldKeys[code] = dk;
        DoomEngine::pushKey (true, dk, typedCharFor (key));
    }
    return true;
}

bool EqDisplay::keyStateChanged (bool)
{
    if (! commandMode)
        return false;

    for (auto it = heldKeys.begin(); it != heldKeys.end();)
    {
        if (! juce::KeyPress::isKeyCurrentlyDown (it->first))
        {
            DoomEngine::pushKey (false, it->second);
            it = heldKeys.erase (it);
        }
        else
            ++it;
    }
    return ! juce::ModifierKeys::getCurrentModifiersRealtime().isCommandDown();
}

void EqDisplay::modifierKeysChanged (const juce::ModifierKeys& mods)
{
    syncModifiers (mods);
}

void EqDisplay::syncModifiers (const juce::ModifierKeys& mods)
{
    if (! commandMode)
        return;

    auto update = [] (bool now, bool& was, int doomKey)
    {
        if (now != was)
        {
            DoomEngine::pushKey (now, doomKey);
            was = now;
        }
    };

    // Releasing Shift can change a held key's code (! back to 1) without a key-up for the
    // shifted one, so let go of held digits/symbols when Shift changes.
    if (mods.isShiftDown() != shiftDown)
    {
        for (auto it = heldKeys.begin(); it != heldKeys.end();)
        {
            if (it->first >= 32 && it->first < 127 && ! juce::CharacterFunctions::isLetter ((juce::juce_wchar) it->first))
            {
                DoomEngine::pushKey (false, it->second);
                it = heldKeys.erase (it);
            }
            else
                ++it;
        }
    }

    update (mods.isCtrlDown(), ctrlDown, KEY_FIRE);
    update (mods.isShiftDown(), shiftDown, KEY_RSHIFT);
    update (mods.isAltDown(), altDown, KEY_RALT);
}

void EqDisplay::focusLost (FocusChangeType)
{
    releaseAllKeys();
    if (commandMode)
        DoomEngine::requestPause (true); // the game can't see the keys: don't let it run on
    needsFullRepaint = true;
}

void EqDisplay::focusGained (FocusChangeType)
{
    if (commandMode)
        DoomEngine::requestPause (false);
    needsFullRepaint = true;
}

void EqDisplay::releaseAllKeys()
{
    for (auto& [code, dk] : heldKeys)
        DoomEngine::pushKey (false, dk);
    heldKeys.clear();

    if (ctrlDown)  DoomEngine::pushKey (false, KEY_FIRE);
    if (shiftDown) DoomEngine::pushKey (false, KEY_RSHIFT);
    if (altDown)   DoomEngine::pushKey (false, KEY_RALT);
    ctrlDown = shiftDown = altDown = false;
}

//==============================================================================
DoomEQEditor::DoomEQEditor (DoomEQProcessor& p)
    : AudioProcessorEditor (p), proc (p), tex (*art), lnf (*art, tex), display (p, *art, tex)
{
    setLookAndFeel (&lnf);
    setWantsKeyboardFocus (true);

    addAndMakeVisible (display);
    display.onCommandChange = [this] (bool inCommand) { setCommandMode (inCommand); };

    auto styleValueBox = [] (juce::Slider& k)
    {
        k.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        k.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        k.setColour (juce::Slider::textBoxTextColourId, Palette::hudYellow);
    };

    for (int b = 0; b < DoomEQProcessor::numBands; ++b)
    {
        auto s = std::make_unique<Strip>();
        const auto colour = Palette::bands[b];

        s->on.setButtonText ("BAND " + juce::String (b + 1));
        s->on.setColour (juce::ToggleButton::tickColourId, colour);
        s->on.setWantsKeyboardFocus (false); // clicking EQ controls mustn't take the keys off the game
        addAndMakeVisible (s->on);
        s->onAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, DoomEQProcessor::pid (b, "on"), s->on);

        for (int t = 0; t < eq::numTypes; ++t)
            s->type.addItem (eq::typeNames[t], t + 1);
        s->type.setWantsKeyboardFocus (false);
        addAndMakeVisible (s->type);
        s->typeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, DoomEQProcessor::pid (b, "type"), s->type);
        s->type.onUserPick = [this, b] (int oldIndex, int newIndex)
        {
            // Picked by hand: a bell or shelf becoming a cut starts as a flat Butterworth.
            if (eq::usesGain ((eq::Type) oldIndex) && eq::isCut ((eq::Type) newIndex))
                if (auto* q = proc.apvts.getParameter (DoomEQProcessor::pid (b, "q")))
                {
                    q->beginChangeGesture();
                    q->setValueNotifyingHost (q->convertTo0to1 (butterworthQ));
                    q->endChangeGesture();
                }
        };

        auto setupKnob = [&] (juce::Slider& k, juce::Label& label, const char* text, const char* id,
                              std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att)
        {
            k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            k.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 18);
            k.setColour (juce::Slider::rotarySliderFillColourId, colour);
            styleValueBox (k);
            addAndMakeVisible (k);
            att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, DoomEQProcessor::pid (b, id), k);

            label.setText (text, juce::dontSendNotification);
            label.setJustificationType (juce::Justification::centred);
            label.setColour (juce::Label::textColourId, Palette::gold);
            label.getProperties().set ("pixelScale", 1.5f);
            addAndMakeVisible (label);
        };

        setupKnob (s->freq, s->freqLabel, "FREQ", "freq", s->freqAttachment);
        s->freq.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 92, 18); // room for "88.8HZ" at 1.5x
        setupKnob (s->gain, s->gainLabel, "GAIN", "gain", s->gainAttachment);
        setupKnob (s->q, s->qLabel, "Q", "q", s->qAttachment);

        // Short readouts so the HUD font can stay big.
        s->freq.textFromValueFunction = [] (double v)
        {
            return v < 1000.0 ? juce::String (v, v < 100.0 ? 1 : 0) + "HZ"
                              : juce::String (v / 1000.0, v < 10000.0 ? 2 : 1) + "K";
        };
        s->gain.textFromValueFunction = [] (double v)
        {
            if (std::abs (v) < 0.05) v = 0.0;
            return (v > 0.0 ? "+" : "") + juce::String (v, 1);
        };
        s->q.textFromValueFunction = [] (double v) { return juce::String (v, 2); };

        // Typed values: "2.5k", "2500", "250hz" all work.
        s->freq.valueFromTextFunction = [] (const juce::String& text)
        {
            const auto t = text.trim().toLowerCase();
            double v = t.getDoubleValue();
            if (t.containsChar ('k'))
                v *= 1000.0;
            return v;
        };
        s->gain.valueFromTextFunction = [] (const juce::String& text) { return text.trim().getDoubleValue(); };
        s->q.valueFromTextFunction    = [] (const juce::String& text) { return text.trim().getDoubleValue(); };

        for (auto* k : { &s->freq, &s->gain, &s->q })
            k->updateText();

        for (auto [slider, id] : { std::pair<juce::Slider*, const char*> { &s->freq, "freq" }, { &s->q, "q" } })
            if (auto* prm = proc.apvts.getParameter (DoomEQProcessor::pid (b, id)))
                slider->setDoubleClickReturnValue (true, (double) prm->convertFrom0to1 (prm->getDefaultValue()));

        const juce::String knobTip = " Double-click to reset, click the number to type a value.";
        s->on.setTooltip ("Turn band " + juce::String (b + 1) + " on or off.");
        s->type.setTooltip ("Filter shape: bell, shelf, or a low/high cut at 12, 24 or 48 dB per octave.");
        s->freq.setTooltip ("Frequency." + knobTip);
        s->gain.setTooltip ("Boost or cut, in dB (not used by the cut filters)." + knobTip);
        s->q.setTooltip ("Q: higher is narrower. On shelves and cuts it adds a resonant bump." + knobTip);
        s->gain.setDoubleClickReturnValue (true, 0.0);

        strips[(size_t) b] = std::move (s);
    }

    output.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    output.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 100, 18);
    output.setColour (juce::Slider::rotarySliderFillColourId, Palette::doomRed);
    output.setDoubleClickReturnValue (true, 0.0);
    styleValueBox (output);
    addAndMakeVisible (output);
    outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "output", output);
    outputLabel.setText ("OUTPUT", juce::dontSendNotification);
    outputLabel.setJustificationType (juce::Justification::centred);
    outputLabel.setColour (juce::Label::textColourId, Palette::gold);
    addAndMakeVisible (outputLabel);

    // Header: DOOM sound, its level, overlay opacity, take command.
    doomSound.setButtonText ("DOOM SOUND");
    doomSound.setColour (juce::ToggleButton::tickColourId, juce::Colour (0xff50e040));
    addAndMakeVisible (doomSound);
    doomSoundAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "doom_sound", doomSound);

    auto setupBar = [&] (juce::Slider& s, juce::Label& l, const char* text, juce::Colour colour)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        s.setColour (juce::Slider::trackColourId, colour);
        addAndMakeVisible (s);
        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centredRight);
        l.setColour (juce::Label::textColourId, Palette::gold);
        addAndMakeVisible (l);
    };

    setupBar (doomLevel, doomLevelLabel, "LEVEL", juce::Colour (0xff50e040));
    doomLevelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "doom_level", doomLevel);
    doomLevel.setDoubleClickReturnValue (true, -6.0);
    doomLevel.textFromValueFunction = [] (double v) { return (v > 0.0 ? "+" : "") + juce::String (v, 1) + " DB"; };

    setupBar (overlaySlider, overlayLabel, "OVERLAY", Palette::hudYellow);
    overlaySlider.setRange (0.0, 100.0, 1.0);
    overlaySlider.setTextValueSuffix ("%");
    for (auto* bar : { &doomLevel, &overlaySlider })
        bar->setRepaintsOnMouseActivity (true); // the value appears on the bar under the mouse

    overlaySlider.setDoubleClickReturnValue (true, 70.0);
    overlaySlider.setValue (proc.getOverlay(), juce::dontSendNotification);
    overlaySlider.onValueChange = [this]
    {
        proc.setOverlay ((float) overlaySlider.getValue());
        display.setOverlayOpacity ((float) overlaySlider.getValue() / 100.0f);
    };
    display.setOverlayOpacity ((float) overlaySlider.getValue() / 100.0f);

    commandButton.setButtonText ("TAKE COMMAND  `");
    commandButton.setClickingTogglesState (false);
    commandButton.onClick = [this] { setCommandMode (! display.isInCommand()); };
    addAndMakeVisible (commandButton);

    helpButton.setButtonText ("?");
    helpButton.onClick = [this]
    {
        if (display.isInCommand())
            setCommandMode (false);
        display.setHelpVisible (! display.isHelpVisible());
    };
    addAndMakeVisible (helpButton);

    bypass.setButtonText ("BYPASS");
    bypass.setColour (juce::ToggleButton::tickColourId, Palette::doomRed);
    addAndMakeVisible (bypass);
    bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "eq_bypass", bypass);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &doomSound, &bypass, &commandButton, &helpButton })
        c->setWantsKeyboardFocus (false);

    // Clicking an EQ control while playing must not move the keyboard off the game (that would
    // pause it). Typing into a value box still takes the keys, on purpose.
    for (auto& st : strips)
        for (juce::Component* c : std::initializer_list<juce::Component*> { &st->on, &st->type, &st->freq, &st->gain, &st->q })
            c->setMouseClickGrabsKeyboardFocus (false);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &output, &doomLevel, &overlaySlider, &doomSound, &bypass })
        c->setMouseClickGrabsKeyboardFocus (false);

    output.setTooltip ("Output level after the EQ. Double-click to reset.");
    bypass.setTooltip ("Hear the signal without the EQ. DOOM's sound still plays.");
    doomSound.setTooltip ("Feed DOOM's sound effects and music into the EQ. Only one DOOM EQ can play them at a time.");
    doomLevel.setTooltip ("How loud DOOM comes in, before the EQ.");
    overlaySlider.setTooltip ("Fade the EQ curve and analyser over the game. At 0% the nodes still appear under the mouse.");
    commandButton.setTooltip ("Give the keyboard to DOOM (` key). Press ` again to hand it back; your game pauses until you return.");
    helpButton.setTooltip ("Show the controls (F1).");

    // HUD art
    skull[0] = art->patch ("M_SKULL1");
    skull[1] = art->patch ("M_SKULL2");
    for (int hurt = 0; hurt < 5; ++hurt)
        for (int look = 0; look < 3; ++look)
            faces[hurt][look] = art->patch ("STFST" + juce::String (hurt) + juce::String (look));
    ouch = art->patch ("STFOUCH0");


    setResizable (true, true);
    setResizeLimits (minWidth, minHeight, 2600, 1800);

    auto [w, h] = proc.getEditorSize();
    const bool saved = w > 0 && h > 0;
    if (! saved)
    {
        w = 1200;
        h = 880;
    }
    if (const auto* screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        // Leave room for the host's own window chrome, and never open bigger than the screen.
        const int maxW = juce::jmax (minWidth, screen->userArea.getWidth() - 60);
        const int maxH = juce::jmax (minHeight, screen->userArea.getHeight() - 120);
        if (! saved && (screen->userArea.getHeight() < 1000 || screen->userArea.getWidth() < 1300))
        {
            w = 1040;
            h = 760;
        }
        w = juce::jmin (w, maxW);
        h = juce::jmin (h, maxH);
    }
    setupPresetBar();

    setSize (juce::jlimit (minWidth, 2600, w), juce::jlimit (minHeight, 1800, h));
    sizeRestored = true;
    startTimerHz (35);
    startUpdateCheckWhenShown();
}

DoomEQEditor::~DoomEQEditor()
{
    // Nothing the editor opened may outlive it: the update check first (so its callback can't
    // run), then the popups, any open menu (it uses our LookAndFeel), and the dialog.
    updateCheck.reset();
    updatePopup.reset();
    about.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    dialog.reset();
    stopTimer();
    tooltips.hideTip();
    setLookAndFeel (nullptr);
}

void DoomEQEditor::setCommandMode (bool inCommand)
{
    display.setCommandMode (inCommand);
    commandButton.setToggleState (inCommand, juce::dontSendNotification);
    commandButton.setButtonText (inCommand ? "LEAVE COMMAND  `" : "TAKE COMMAND  `");
    if (! inCommand)
        grabKeyboardFocus();
}

bool DoomEQEditor::keyPressed (const juce::KeyPress& key)
{
    if (dialog != nullptr || about != nullptr || updatePopup != nullptr)
        return false; // they handle their own keys

    if (key.getKeyCode() == juce::KeyPress::F1Key)
    {
        display.setHelpVisible (! display.isHelpVisible());
        return true;
    }

    if (EqDisplay::isTakeCommandKey (key))
    {
        setCommandMode (! display.isInCommand());
        return true;
    }
    return false;
}

void DoomEQEditor::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    const auto p = e.getPosition();
    if (titleClickArea.contains (p))
        showAbout();
    else if (siteLinkArea.contains (p))
        juce::URL ("https://sweetjusticesound.com").launchInDefaultBrowser();
}

void DoomEQEditor::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    const bool overLink = titleClickArea.contains (p) || siteLinkArea.contains (p);
    setMouseCursor (overLink ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    setTooltip (titleClickArea.contains (p) ? "About SammyJs DOOM EQ, and check for updates."
              : siteLinkArea.contains (p)   ? "Open sweetjusticesound.com"
                                            : juce::String());
}

void DoomEQEditor::visibilityChanged()
{
    juce::AudioProcessorEditor::visibilityChanged();
    startUpdateCheckWhenShown();

    // Hosts that hand the keyboard to plug-in windows then deliver ` straight away.
    if (isShowing())
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<DoomEQEditor> (this)]
                                         {
                                             if (safe != nullptr && safe->isShowing() && ! safe->display.isInCommand())
                                                 safe->grabKeyboardFocus();
                                         });
}

//==============================================================================
// Presets: the Sammy J preset bar and menu, drawn DOOM-style.
//==============================================================================
void DoomEQEditor::setupPresetBar()
{
    presetPrev.setTooltip ("Previous preset");
    presetPrev.onClick = [this] { proc.getPresetManager().loadPrevious(); updatePresetDisplay(); };

    presetNext.setTooltip ("Next preset");
    presetNext.onClick = [this] { proc.getPresetManager().loadNext(); updatePresetDisplay(); };

    presetName.setTooltip ("Choose a preset, or save, delete and find your presets. Also: About and updates.");
    presetName.onClick = [this] { showPresetMenu(); };

    presetSave.setTooltip ("Save the current settings as a preset");
    presetSave.onClick = [this]
    {
        auto& pm = proc.getPresetManager();
        pm.refresh(); // a preset renamed or removed outside the plug-in is not written back blindly
        const auto current = pm.getSnapshot();

        // Editing a user preset that is still in the folder: SAVE writes back to its file.
        if (current.currentIsUser && current.currentIndex >= 0)
            savePresetAs (current.currentName);
        else
            promptSavePreset (current.currentName.equalsIgnoreCase ("Init") ? juce::String() : current.currentName);
    };

    for (auto* b : { &presetPrev, &presetName, &presetNext, &presetSave })
    {
        b->setWantsKeyboardFocus (false);
        b->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    proc.getPresetManager().refresh();
    updatePresetDisplay();
}

void DoomEQEditor::updatePresetDisplay()
{
    const auto& pm = proc.getPresetManager();
    const bool modified = pm.isModified();
    const auto text = pm.getCurrentName().toUpperCase() + (modified ? " *" : "");

    if (presetName.getButtonText() != text)
        presetName.setButtonText (text);
    if (presetSave.getToggleState() != modified)
        presetSave.setToggleState (modified, juce::dontSendNotification); // lit while there's something to save
}

void DoomEQEditor::showPresetMenu()
{
    auto& pm = proc.getPresetManager();
    pm.refresh(); // pick up presets added or removed outside the plug-in

    const auto snapshot = pm.getSnapshot();
    const auto& list = snapshot.presets;
    const auto isCurrentFactory = [&snapshot] (int i) { return snapshot.currentIsFactory && i == snapshot.currentIndex; };

    juce::PopupMenu menu;
    menu.setLookAndFeel (&lnf);
    menu.addSectionHeader ("FACTORY");

    for (int i = 0; i < list.size(); ++i)
        if (list[i].isFactory && list[i].category.isEmpty())
            menu.addItem (i + 1, list[i].name, true, isCurrentFactory (i));

    for (const auto& category : PresetManager::getFactoryCategories())
    {
        juce::PopupMenu sub;
        bool containsCurrent = false;
        for (int i = 0; i < list.size(); ++i)
        {
            if (list[i].isFactory && list[i].category == category)
            {
                sub.addItem (i + 1, list[i].name, true, isCurrentFactory (i));
                containsCurrent = containsCurrent || isCurrentFactory (i);
            }
        }
        if (sub.getNumItems() > 0)
            menu.addSubMenu (category, sub, true, nullptr, containsCurrent);
    }

    // User presets are told apart by their file (two can share a name; menuName adds the file).
    juce::Array<juce::File> userFiles;
    juce::PopupMenu user;
    bool userContainsCurrent = false;
    juce::String currentMenuName = snapshot.currentName;

    for (const auto& preset : list)
    {
        if (preset.isFactory)
            continue;
        const bool isCurrent = snapshot.currentIsUser && preset.file == snapshot.currentFile;
        user.addItem (firstUserItemId + userFiles.size(), preset.menuName, true, isCurrent);
        userFiles.add (preset.file);
        if (isCurrent)
        {
            userContainsCurrent = true;
            currentMenuName = preset.menuName;
        }
    }

    menu.addSectionHeader ("USER");
    if (! userFiles.isEmpty())
        menu.addSubMenu ("USER PRESETS", user, true, nullptr, userContainsCurrent);
    else
        menu.addItem (-1, "(NONE SAVED YET)", false);

    const auto fileToDelete = snapshot.currentIsUser ? snapshot.currentFile : juce::File();

    menu.addSeparator();
    menu.addItem (saveAsItemId, "SAVE AS...");
    menu.addItem (deleteItemId, "DELETE \"" + currentMenuName.toUpperCase() + "\"", fileToDelete != juce::File());
    menu.addItem (revealItemId, "SHOW PRESETS FOLDER");
    menu.addSeparator();
    menu.addItem (aboutItemId, "ABOUT DOOM EQ + UPDATES...");

    menu.showMenuAsync (DoomEQLookAndFeel::menuOptionsFor (presetName, presetName.getWidth()),
                        [safe = juce::Component::SafePointer<DoomEQEditor> (this), userFiles, fileToDelete, nameToDelete = currentMenuName] (int result)
    {
        if (safe == nullptr || result == 0)
            return;

        auto& presetManager = safe->proc.getPresetManager();

        if (result == saveAsItemId)
            safe->promptSavePreset (presetManager.isCurrentUserPreset() ? presetManager.getCurrentName() : juce::String());
        else if (result == deleteItemId)
            safe->confirmDeletePreset (fileToDelete, nameToDelete);
        else if (result == revealItemId)
        {
            const auto dir = PresetManager::getUserPresetDirectory();
            dir.createDirectory();
            dir.startAsProcess(); // opens the folder in Finder / Explorer
        }
        else if (result == aboutItemId)
            safe->showAbout();
        else if (result >= firstUserItemId)
        {
            if (const int i = result - firstUserItemId; i < userFiles.size())
                presetManager.loadUserPreset (userFiles[i]);
            safe->updatePresetDisplay();
        }
        else if (result >= 1 && result <= PresetManager::getNumFactory())
        {
            presetManager.loadPreset (result - 1); // factory presets lead the list, in a fixed order
            safe->updatePresetDisplay();
        }
    });
}

void DoomEQEditor::showDialog (DoomDialogOptions options)
{
    if (display.isInCommand())
        setCommandMode (false); // the dialog needs the keys

    // Wrap the result so the dialog is gone before the next step (which may open another one).
    auto userResult = std::move (options.onResult);
    options.onResult = [safe = juce::Component::SafePointer<DoomEQEditor> (this), userResult] (int button, const juce::String& text)
    {
        if (safe == nullptr)
            return;
        safe->dialog.reset();
        if (userResult)
            userResult (button, text); // may open the next dialog
        if (safe != nullptr && safe->dialog == nullptr)
            safe->overlayClosed();
    };

    dialog = std::make_unique<DoomDialog> (*art, tex, std::move (options));
    addAndMakeVisible (*dialog);
    dialog->setBounds (getLocalBounds());
    dialog->toFront (true);
}

void DoomEQEditor::promptSavePreset (const juce::String& suggestedName)
{
    DoomDialogOptions o;
    o.title = "SAVE PRESET";
    o.message = "NAME THIS PRESET:";
    o.withTextEntry = true;
    o.initialText = suggestedName.toUpperCase();
    o.buttons = { "SAVE", "CANCEL" };
    o.onResult = [safe = juce::Component::SafePointer<DoomEQEditor> (this)] (int button, const juce::String& text)
    {
        if (safe != nullptr && button == 0 && text.trim().isNotEmpty())
            safe->checkNameAndSave (text.trim());
    };
    showDialog (std::move (o));
}

void DoomEQEditor::checkNameAndSave (const juce::String& name)
{
    auto& pm = proc.getPresetManager();

    if (! PresetManager::isValidPresetName (name))
    {
        DoomDialogOptions o;
        o.title = "CAN'T USE THIS NAME";
        o.message = "\"" + name.toUpperCase() + "\" HAS NO CHARACTERS A FILE NAME CAN HOLD. PLEASE USE LETTERS OR NUMBERS.";
        o.buttons = { "OK", "CANCEL" };
        o.onResult = [safe = juce::Component::SafePointer<DoomEQEditor> (this), name] (int button, const juce::String&)
        {
            if (safe != nullptr && button == 0)
                safe->promptSavePreset (name); // try again, with the name as typed
        };
        showDialog (std::move (o));
        return;
    }

    if (pm.userPresetExists (name))
        confirmOverwrite (name, pm.getNameOfPresetSavedAs (name));
    else
        savePresetAs (name);
}

void DoomEQEditor::confirmOverwrite (const juce::String& name, const juce::String& replacedName)
{
    DoomDialogOptions o;
    o.title = "REPLACE PRESET";
    o.message = replacedName.isEmpty() || replacedName == name
              ? "A PRESET CALLED \"" + name.toUpperCase() + "\" ALREADY EXISTS. REPLACE IT?"
              : "SAVING AS \"" + name.toUpperCase() + "\" REPLACES THE PRESET \"" + replacedName.toUpperCase()
                  + "\" (THE TWO NAMES GIVE THE SAME FILE NAME). REPLACE IT?";
    o.buttons = { "REPLACE", "CANCEL" };
    o.onResult = [safe = juce::Component::SafePointer<DoomEQEditor> (this), name, replacedName] (int button, const juce::String&)
    {
        if (safe == nullptr || button != 0)
            return;
        // The folder may have changed while the question was up: ask again if so.
        const auto nowReplaced = safe->proc.getPresetManager().getNameOfPresetSavedAs (name);
        if (nowReplaced.isNotEmpty() && nowReplaced != replacedName)
            safe->confirmOverwrite (name, nowReplaced);
        else
            safe->savePresetAs (name);
    };
    showDialog (std::move (o));
}

void DoomEQEditor::confirmDeletePreset (const juce::File& file, const juce::String& displayName)
{
    DoomDialogOptions o;
    o.title = "DELETE PRESET";
    o.message = "DELETE \"" + displayName.toUpperCase() + "\"? THIS REMOVES THE PRESET FILE.";
    o.buttons = { "DELETE", "CANCEL" };
    o.onResult = [safe = juce::Component::SafePointer<DoomEQEditor> (this), file] (int button, const juce::String&)
    {
        if (safe == nullptr || button != 0)
            return;
        safe->proc.getPresetManager().deleteUserPreset (file); // exactly the file the dialog named
        safe->updatePresetDisplay();
    };
    showDialog (std::move (o));
}

void DoomEQEditor::savePresetAs (const juce::String& name)
{
    if (! proc.getPresetManager().saveUserPreset (name))
    {
        DoomDialogOptions o;
        o.title = "COULDN'T SAVE PRESET";
        o.message = "THE PRESET COULD NOT BE WRITTEN TO: " + PresetManager::getUserPresetDirectory().getFullPathName().toUpperCase();
        o.buttons = { "OK" };
        showDialog (std::move (o));
    }
    updatePresetDisplay();
}

//==============================================================================
// About and updates
//==============================================================================
void DoomEQEditor::showAbout()
{
    if (about != nullptr)
        return;
    if (display.isInCommand())
        setCommandMode (false);

    about = std::make_unique<AboutPanel> (*art, tex, [safe = juce::Component::SafePointer<DoomEQEditor> (this)]
                                                     {
                                                         if (safe != nullptr)
                                                         {
                                                             safe->about.reset();
                                                             safe->overlayClosed();
                                                         }
                                                     });
    addAndMakeVisible (*about);
    about->setBounds (getLocalBounds());
    about->toFront (true);
}

void DoomEQEditor::overlayClosed()
{
    // An update found while something else was open shows now.
    if (dialog == nullptr && about == nullptr && pendingUpdate.has_value())
    {
        const auto info = *pendingUpdate;
        pendingUpdate.reset();
        showUpdatePopup (info);
        return;
    }

    // Give the keys back to whatever is on top.
    for (juce::Component* c : std::initializer_list<juce::Component*> { updatePopup.get(), about.get(), dialog.get() })
        if (c != nullptr && c->isShowing())
        {
            c->grabKeyboardFocus();
            return;
        }
    if (! display.isInCommand() && isShowing())
        grabKeyboardFocus();
}

void DoomEQEditor::parentHierarchyChanged()
{
    juce::AudioProcessorEditor::parentHierarchyChanged();
    startUpdateCheckWhenShown();
}

void DoomEQEditor::startUpdateCheckWhenShown()
{
    // Once per editor, only when it's on screen in a host or the app: an editor that is only
    // built (a test, a snapshot) never goes online. Never from the processor or audio thread.
    if (updateCheck != nullptr || ! isShowing() || proc.wrapperType == juce::AudioProcessor::wrapperType_Undefined)
        return;

    // 0.0.x releases are pre-releases, which the shared check skips: update notices start with
    // the first full release. (About's CHECK FOR UPDATES also sees test builds.)
    updateCheck = std::make_unique<sammyj::UpdateCheck> ("doomeq", "doomeq-v", JucePlugin_VersionString,
                                                          [this] (const sammyj::UpdateInfo& info) { showUpdatePopup (info); });
}

void DoomEQEditor::showUpdatePopup (const sammyj::UpdateInfo& info)
{
    if (updatePopup != nullptr)
        return;
    if (dialog != nullptr || about != nullptr)
    {
        pendingUpdate = info; // don't steal the keys from a dialog in use; show it when that closes
        return;
    }
    if (display.isInCommand())
        setCommandMode (false);

    updatePopup = std::make_unique<UpdatePopup> (*art, tex, info, [safe = juce::Component::SafePointer<DoomEQEditor> (this)]
                                                                  {
                                                                      if (safe != nullptr)
                                                                      {
                                                                          safe->updatePopup.reset();
                                                                          safe->overlayClosed();
                                                                      }
                                                                  });
    addAndMakeVisible (*updatePopup);
    updatePopup->setBounds (getLocalBounds());
    updatePopup->toFront (true);
}

void DoomEQEditor::fixValueBoxScales()
{
    // One text size per value box, picked from the widest value it can show, so the digits
    // don't jump between sizes as the value changes.
    auto apply = [this] (juce::Slider& sl, const char* widest)
    {
        const float inner = (float) juce::jmin (sl.getTextBoxWidth(), sl.getWidth()) - 6.0f;
        float sc = 2.0f;
        while (sc > 1.0f && (float) art->font().widthOf (widest) * sc > inner)
            sc -= 0.5f;
        if (! sl.getProperties().contains ("valueScale") || ! juce::exactlyEqual ((float) sl.getProperties()["valueScale"], sc))
        {
            sl.getProperties().set ("valueScale", sc);
            sl.repaint();
        }
    };

    for (auto& s : strips)
    {
        apply (s->freq, "88.8HZ");
        apply (s->gain, "-18.0");
        apply (s->q, "10.00");
    }
    apply (output, "-18.0 DB");
}

void DoomEQEditor::timerCallback()
{
    display.tick();




    for (int b = 0; b < DoomEQProcessor::numBands; ++b)
    {
        auto& s = *strips[(size_t) b];
        const auto t = (eq::Type) s.type.getSelectedItemIndex();
        if (s.gain.isEnabled() != eq::usesGain (t))
            s.gain.setEnabled (eq::usesGain (t));
    }

    updatePresetDisplay();

    // A session or preset restored with the window open brings its own overlay amount.
    if (! overlaySlider.isMouseButtonDown() && std::abs (proc.getOverlay() - (float) overlaySlider.getValue()) > 0.5f)
    {
        overlaySlider.setValue (proc.getOverlay(), juce::dontSendNotification);
        display.setOverlayOpacity (proc.getOverlay() / 100.0f);
    }

    ++frameCount;

    // DOOM's sound can only come out while the host sends audio through the plug-in. Most
    // DAWs (Pro Tools, for one) do that all the time; some hosts, Soundminer for one, only
    // while playing a file. And only one DOOM EQ plays it at a time. Say so on the button,
    // so silence doesn't look like a fault.
    hostIdleTicks = proc.getBlockCount() != lastIdleBlockCount ? 0 : hostIdleTicks + 1;
    lastIdleBlockCount = proc.getBlockCount();
    if (frameCount % 12 == 0)
    {
        const bool on = doomSound.getToggleState();
        const bool elsewhere = on && proc.doomAudioUsedElsewhere();
        const bool hostIdle = on && hostIdleTicks > 17; // ~0.5 s without a block
        const juce::String text = elsewhere ? "SOUND: OTHER EQ" : hostIdle ? "SOUND: HOST IDLE" : "DOOM SOUND";
        if (doomSound.getButtonText() != text)
        {
            doomSound.setButtonText (text);
            doomSound.setTooltip (hostIdle
                ? "Your host isn't sending audio through DOOM EQ right now, so DOOM can't be heard. Most DAWs keep plug-ins running all the time; Soundminer only while a file plays. The standalone app plays it all the time."
                : elsewhere ? "Another DOOM EQ is playing the game's sound. Turn DOOM SOUND off there to hear it here."
                            : "Feed DOOM's sound effects and music into the EQ. Only one DOOM EQ can play them at a time.");
        }
    }

    // Skull cursor blinks like the menu one.
    if (frameCount % 8 == 0)
        repaint (skullArea);

    // The marine reacts to your output level. If the host has stopped sending audio, the
    // processor's meter is frozen, so let the face calm down here.
    const auto blocks = proc.getBlockCount();
    facePeak = blocks != lastBlockCount ? proc.getOutputPeak() : facePeak * 0.85f;
    lastBlockCount = blocks;
    const float db = juce::Decibels::gainToDecibels (facePeak, -100.0f);
    const int hurt = db < -30.0f ? 0 : db < -18.0f ? 1 : db < -9.0f ? 2 : db < -3.0f ? 3 : 4;
    const bool clip = db > -0.1f;
    if (--nextLookChange <= 0)
    {
        faceLook = juce::Random::getSystemRandom().nextInt (3);
        nextLookChange = 20 + juce::Random::getSystemRandom().nextInt (50);
        repaint (faceArea);
    }
    if (hurt != faceHurt || clip != faceOuch)
    {
        faceHurt = hurt;
        faceOuch = clip;
        repaint (faceArea);
    }
}

void DoomEQEditor::rebuildChrome (float scale)
{
    chromeScale = scale;
    siteLinkArea = {};
    chrome = juce::Image (juce::Image::RGB, juce::jmax (1, juce::roundToInt ((float) getWidth() * scale)),
                          juce::jmax (1, juce::roundToInt ((float) getHeight() * scale)), false);
    juce::Graphics g (chrome);
    g.addTransform (juce::AffineTransform::scale (scale));

    const auto bounds = getLocalBounds();
    DoomStyle::fillTextured (g, bounds.toFloat(), tex.background, 2.0f, 0.42f);

    // Header bar with hazard trim.
    {
        auto h = headerArea.expanded (12, 12).withTop (0);
        DoomStyle::fillTextured (g, h.toFloat(), tex.panel, 2.0f, 0.36f);
        DoomStyle::fillTextured (g, h.removeFromBottom (8).toFloat(), tex.hazard, 1.0f, 0.75f);
        DoomStyle::bevel (g, headerArea.expanded (12, 12).withTop (0).withTrimmedBottom (8), 2, Palette::bevelLight.withAlpha (0.5f), Palette::bevelDark);
    }

    {
        const auto& f = art->font();
        const int wd = f.widthOf ("DOOM"), gap = 4, we = f.widthOf ("EQ");
        float sc = 4.0f;
        while (sc > 1.0f && (float) (wd + gap + we) * sc > (float) titleArea.getWidth())
            sc -= 1.0f;
        const float x = (float) titleArea.getX(), y = (float) titleArea.getCentreY() - (float) f.height * sc * 0.5f;
        f.draw (g, "DOOM", x, y, sc, Palette::doomRed);
        f.draw (g, "EQ", x + (float) (wd + gap) * sc, y, sc, Palette::hudYellow);

        // The family kicker, then the website, as space allows.
        float used = (float) (wd + gap + we) * sc + 18.0f;
        const juce::String kicker = "SAMMYJS  /  6 BAND PARAMETRIC  /";
        const float kickerW = (float) f.widthOf (kicker) * 1.5f;
        if ((float) titleArea.getWidth() - used >= kickerW)
        {
            f.draw (g, kicker, (float) titleArea.getX() + used, (float) titleArea.getCentreY() - (float) f.height * 0.75f, 1.5f, Palette::textDim);
            used += kickerW + 12.0f;
            const juce::String site = "SWEETJUSTICESOUND.COM";
            if ((float) titleArea.getWidth() - used >= (float) f.widthOf (site) * 1.5f)
            {
                f.draw (g, site, (float) titleArea.getX() + used, (float) titleArea.getCentreY() - (float) f.height * 0.75f, 1.5f, Palette::gold.withAlpha (0.75f));
                siteLinkArea = juce::Rectangle<float> ((float) titleArea.getX() + used, (float) titleArea.getY(),
                                                       (float) f.widthOf (site) * 1.5f, (float) titleArea.getHeight()).toNearestInt();
            }
        }
        titleClickArea = juce::Rectangle<int> (skullArea.getX(), titleArea.getY(),
                                               titleArea.getX() + (int) ((float) (wd + gap + we) * sc) - skullArea.getX(), titleArea.getHeight());
    }

    // The DOOM control row: a recessed strip above the screen.
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRect (controlRow);
        DoomStyle::bevel (g, controlRow, 2, Palette::bevelDark, Palette::bevelLight.withAlpha (0.45f));
    }

    // Band panels
    for (int b = 0; b < DoomEQProcessor::numBands; ++b)
    {
        const auto r = strips[(size_t) b]->bounds.reduced (3, 0);
        DoomStyle::fillTextured (g, r.toFloat(), tex.panel, 2.0f, 0.4f, r.getPosition().toFloat());
        DoomStyle::bevel (g, r, 3, Palette::bevelLight.withAlpha (0.75f), Palette::bevelDark);
        DoomStyle::rivets (g, r, 6, 5);

        // Colour stripe
        const auto stripe = r.reduced (16, 0).withHeight (6).translated (0, 6);
        g.setColour (Palette::bands[b].withMultipliedBrightness (0.55f));
        g.fillRect (stripe);
        g.setColour (Palette::bands[b]);
        g.fillRect (stripe.withHeight (3));
    }

    // Output panel with the face.
    {
        const auto r = outputArea.reduced (3, 0);
        DoomStyle::fillTextured (g, r.toFloat(), tex.panel, 2.0f, 0.4f, r.getPosition().toFloat());
        DoomStyle::bevel (g, r, 3, Palette::bevelLight.withAlpha (0.75f), Palette::bevelDark);
        DoomStyle::rivets (g, r, 6, 5);

        g.setColour (juce::Colours::black);
        g.fillRect (faceArea);
        DoomStyle::bevel (g, faceArea, 2, Palette::bevelDark, Palette::bevelLight.withAlpha (0.5f));
    }
}

void DoomEQEditor::paint (juce::Graphics& g)
{
    const float scale = (float) g.getInternalContext().getPhysicalPixelScaleFactor();
    if (! chrome.isValid() || chromeScale != scale)
        rebuildChrome (scale);

    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (chrome, getLocalBounds().toFloat());
    }

    // Blinking skull.
    const auto& sk = skull[(frameCount / 8) % 2];
    if (sk.isValid())
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (sk, skullArea.toFloat(), juce::RectanglePlacement::centred);
    }

    // The face.
    {
        const auto& face = faceOuch && ouch.isValid() ? ouch : faces[faceHurt][faceLook];
        if (face.isValid())
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
            g.drawImage (face, faceArea.reduced (4).toFloat(), juce::RectanglePlacement::centred);
        }
    }
}

void DoomEQEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    // Row 1, the Sammy J header: skull, title (opens About), kicker, presets, help.
    headerArea = area.removeFromTop (headerHeight - 12);
    {
        auto h = headerArea;
        skullArea = h.removeFromLeft (44);
        h.removeFromLeft (8);

        helpButton.setBounds (h.removeFromRight (30).reduced (0, 4));
        h.removeFromRight (8);
        presetSave.setBounds (h.removeFromRight (70).reduced (0, 4));
        h.removeFromRight (8);
        presetNext.setBounds (h.removeFromRight (30).reduced (0, 4));
        h.removeFromRight (4);
        presetName.setBounds (h.removeFromRight (juce::jlimit (180, 320, h.getWidth() - 300)).reduced (0, 4));
        h.removeFromRight (4);
        presetPrev.setBounds (h.removeFromRight (30).reduced (0, 4));
        h.removeFromRight (16);
        titleArea = h;
    }

    // Row 2, the DOOM controls, just above the screen.
    area.removeFromTop (20);
    controlRow = area.removeFromTop (controlRowHeight);
    {
        auto c = controlRow.reduced (6, 3);
        doomSound.setBounds (c.removeFromLeft (170));
        c.removeFromLeft (14);
        doomLevelLabel.setBounds (c.removeFromLeft (70).reduced (0, 3));
        doomLevel.setBounds (c.removeFromLeft (120));
        c.removeFromLeft (14);
        overlayLabel.setBounds (c.removeFromLeft (100).reduced (0, 3));
        overlaySlider.setBounds (c.removeFromLeft (140));
        commandButton.setBounds (c.removeFromRight (220));
    }
    area.removeFromTop (8);
    stripsArea = area.removeFromBottom (stripsHeight);
    area.removeFromBottom (10);
    display.setBounds (area);

    outputArea = stripsArea.removeFromRight (130);
    stripsArea.removeFromRight (4);

    const int stripW = stripsArea.getWidth() / DoomEQProcessor::numBands;
    for (int b = 0; b < DoomEQProcessor::numBands; ++b)
    {
        auto& s = *strips[(size_t) b];
        s.bounds = stripsArea.withX (stripsArea.getX() + b * stripW).withWidth (stripW);

        auto r = s.bounds.reduced (14, 12);
        r.removeFromTop (10);
        s.on.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        s.type.setBounds (r.removeFromTop (26));
        r.removeFromTop (6);

        auto top = r.removeFromTop (r.getHeight() / 2);
        s.freqLabel.setBounds (top.removeFromTop (14));
        s.freq.setBounds (top.withSizeKeepingCentre (juce::jmin (top.getWidth(), 92), top.getHeight()));

        auto left = r.removeFromLeft (r.getWidth() / 2);
        s.gainLabel.setBounds (left.removeFromTop (14));
        s.gain.setBounds (left.reduced (2, 0));
        s.qLabel.setBounds (r.removeFromTop (14));
        s.q.setBounds (r.reduced (2, 0));
    }

    auto o = outputArea.reduced (14, 14);
    o.removeFromTop (4);
    outputLabel.setBounds (o.removeFromTop (20));
    o.removeFromTop (4);
    faceArea = o.removeFromTop (76).withSizeKeepingCentre (64, 76);
    o.removeFromTop (6);
    bypass.setBounds (o.removeFromBottom (26));
    o.removeFromBottom (6);
    output.setBounds (o);

    if (sizeRestored)
        proc.setEditorSize (getWidth(), getHeight());

    for (juce::Component* overlay : std::initializer_list<juce::Component*> { dialog.get(), about.get(), updatePopup.get() })
        if (overlay != nullptr)
            overlay->setBounds (getLocalBounds());

    chrome = {}; // rebuilt at the next paint
    fixValueBoxScales();
}
