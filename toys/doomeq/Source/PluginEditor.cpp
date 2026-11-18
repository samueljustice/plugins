#include "PluginEditor.h"

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
    constexpr int headerHeight = 58;

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

    /** JUCE key -> doomkeys.h code. Arrows and WASD move, Ctrl/F fire, Space/E use. */
    int doomKeyFor (const juce::KeyPress& k)
    {
        int code = k.getKeyCode();

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
    const float maxScale = label.getProperties().getWithDefault ("pixelScale", 2.0f);
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
                                           bool isHighlighted, bool isTicked, bool, const juce::String& text,
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
    if (isTicked)
        art.font().draw (g, ">", (float) r.getX(), (float) r.getCentreY() - 7.0f, 2.0f, Palette::hudYellow);
    r.removeFromLeft (22);
    art.font().drawFitted (g, text, r.toFloat(), juce::Justification::centredLeft,
                           isHighlighted ? Palette::hudYellow : Palette::gold.withMultipliedAlpha (isActive ? 1.0f : 0.4f));
}

void DoomEQLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int, int& idealWidth, int& idealHeight)
{
    idealHeight = isSeparator ? 8 : 24;
    idealWidth = art.font().widthOf (text) * 2 + 50;
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
    return k.getTextCharacter() == '`' || k.getTextCharacter() == '~' || k.getKeyCode() == '`';
}

bool EqDisplay::isLeaveCommandKey (const juce::KeyPress& k)
{
    return k.getKeyCode() == juce::KeyPress::escapeKey && k.getModifiers().isShiftDown();
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

void EqDisplay::readBands()
{
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
    const float db = eq::usesGain (v.type) ? v.gain : 0.0f;
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
        grabKeyboardFocus();
    else
        releaseAllKeys();
    repaint();
}

void EqDisplay::setOverlayOpacity (float v)
{
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
    readBands();
    updateAnalyser();
    DoomEngine::fetchFrame (doomFrame, doomSerial);
    repaint();
}

void EqDisplay::updateAnalyser()
{
    const int got = proc.pullAnalyserSamples (pullScratch.data(), (int) pullScratch.size());
    for (int i = 0; i < got; ++i)
    {
        ring[(size_t) ringPos] = pullScratch[(size_t) i];
        ringPos = (ringPos + 1) % fftSize;
    }

    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = ring[(size_t) ((ringPos + i) % fftSize)];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

    window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    const float norm = 4.0f / (float) fftSize;
    for (int i = 1; i < fftSize / 2; ++i)
    {
        const float db = juce::Decibels::gainToDecibels (fftData[(size_t) i] * norm, -120.0f);
        auto& s = spectrumDb[(size_t) i];
        s = db > s ? db : s - 1.6f;
    }
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
        // A light wash so the curve reads, scaled by the overlay amount.
        g.setColour (juce::Colours::black.withAlpha (overlay * (commandMode ? 0.12f : 0.3f)));
        g.fillRect (r);

        paintGrid (g, r);
        paintSpectrum (g, r);
        paintCurves (g, r);
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

    // Key hints
    {
        const juce::String hint = commandMode
            ? (hasKeyboardFocus (false) ? "IN COMMAND   SHIFT+ESC TO LEAVE" : "CLICK THE SCREEN TO RESUME COMMAND")
            : "PRESS ` TO TAKE COMMAND";
        const auto box = juce::Rectangle<float> (r.getRight() - 350.0f, 12.0f, 338.0f, 24.0f);
        g.setColour (juce::Colours::black.withAlpha (commandMode ? 0.7f : 0.45f));
        g.fillRect (box);
        art.font().drawFitted (g, hint, box.reduced (8.0f, 4.0f), juce::Justification::centredRight,
                               commandMode ? Palette::hudYellow : Palette::gold.withAlpha (0.8f));
    }
}

void EqDisplay::paintGrid (juce::Graphics& g, juce::Rectangle<float> r)
{
    const float a = overlay * (commandMode ? 0.12f : 0.22f);

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
        art.font().draw (g, s, xForFreq (f) + 4.0f, r.getBottom() - 22.0f, 2.0f, Palette::gold.withAlpha (overlay * 0.9f));
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
                             Palette::gold.withAlpha (overlay * 0.9f));
        }
    }
}

void EqDisplay::paintSpectrum (juce::Graphics& g, juce::Rectangle<float> r)
{
    const float binHz = (float) sampleRate / (float) fftSize;
    const int w = getWidth();
    if (w <= 0)
        return;

    std::vector<float> cols ((size_t) w, -120.0f);
    for (int i = 1; i < fftSize / 2; ++i)
    {
        const float f = (float) i * binHz;
        if (f < minFreq || f > maxFreq)
            continue;
        const int x = juce::jlimit (0, w - 1, (int) xForFreq (f));
        const float tilted = spectrumDb[(size_t) i] + 4.5f * std::log2 (f / 1000.0f);
        cols[(size_t) x] = juce::jmax (cols[(size_t) x], tilted);
    }

    float last = -120.0f;
    for (auto& c : cols)
    {
        if (c <= -119.0f) c = last;
        else last = c;
    }

    // Chunky 4px columns, like an old VU.
    const float top = r.getY() + 10.0f, bottom = r.getBottom() - 4.0f;
    for (int x = 0; x + 3 < w; x += 4)
    {
        const float db = juce::jmax (cols[(size_t) x], cols[(size_t) x + 2]);
        const float y = juce::jmap (juce::jlimit (-84.0f, 0.0f, db), -84.0f, 0.0f, bottom, top);
        if (y >= bottom - 1.0f)
            continue;
        g.setColour (Palette::gold.withAlpha (overlay * (commandMode ? 0.12f : 0.26f)));
        g.fillRect ((float) x, y, 3.0f, bottom - y);
        g.setColour (Palette::hudYellow.withAlpha (overlay * (commandMode ? 0.25f : 0.55f)));
        g.fillRect ((float) x, y, 3.0f, 2.0f);
    }
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
        g.setColour (Palette::bands[b].withAlpha (overlay * (focus ? 0.3f : 0.13f)));
        g.fillPath (fill);
    }

    juce::Path curve;
    for (int x = 0; x <= w; ++x)
    {
        const float y = juce::jlimit (r.getY() - 2.0f, r.getBottom() + 2.0f, yForDb ((float) total[(size_t) x]));
        if (x == 0) curve.startNewSubPath (0.0f, y);
        else        curve.lineTo ((float) x, y);
    }

    g.setColour (juce::Colours::black.withAlpha (overlay * 0.7f));
    g.strokePath (curve, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::square));
    g.setColour (Palette::doomRed.withAlpha (overlay * 0.9f));
    g.strokePath (curve, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::square));
    g.setColour (Palette::hudYellow.withAlpha (overlay));
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
    if (commandMode)
        grabKeyboardFocus();
    else if (auto* parent = getParentComponent())
        parent->grabKeyboardFocus(); // so ` reaches the editor

    dragBand = hitTestHandle (e.position);
    if (dragBand < 0)
        return;

    if (e.mods.isPopupMenu())
    {
        const int band = dragBand;
        dragBand = -1;

        juce::PopupMenu m;
        const auto& v = bandViews[(size_t) band];
        m.addItem (1000, v.on ? "Turn band off" : "Turn band on");
        m.addSeparator();
        for (int t = 0; t < eq::numTypes; ++t)
            m.addItem (t + 1, eq::typeNames[t], true, (int) v.type == t);
        m.addSeparator();
        m.addItem (2000, "Reset gain");

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                         [this, band] (int result)
                         {
                             if (result == 1000)
                                 setParam (band, "on", bandViews[(size_t) band].on ? 0.0f : 1.0f);
                             else if (result == 2000)
                                 setParam (band, "gain", 0.0f);
                             else if (result > 0)
                                 setParam (band, "type", (float) (result - 1));
                         });
        return;
    }

    for (auto* name : { "freq", "gain" })
        if (auto* p = param (dragBand, name))
            p->beginChangeGesture();
}

void EqDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0)
        return;

    const auto pos = e.position;
    setParam (dragBand, "freq", freqForX (juce::jlimit (0.0f, (float) getWidth(), pos.x)));

    if (eq::usesGain (bandViews[(size_t) dragBand].type))
        setParam (dragBand, "gain", juce::jlimit (-displayDbRange, displayDbRange, dbForY (pos.y)));

    readBands();
    repaint();
}

void EqDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragBand >= 0)
        for (auto* name : { "freq", "gain" })
            if (auto* p = param (dragBand, name))
                p->endChangeGesture();
    dragBand = -1;
}

void EqDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int b = hitTestHandle (e.position);
    if (b >= 0)
        setParam (b, "on", bandViews[(size_t) b].on ? 0.0f : 1.0f);
}

void EqDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int b = hitTestHandle (e.position);
    if (b < 0)
        return;

    auto* p = param (b, "q");
    if (p == nullptr)
        return;

    const float q = bandViews[(size_t) b].q * std::pow (1.6f, wheel.deltaY * (wheel.isReversed ? -1.0f : 1.0f));
    p->beginChangeGesture();
    setParam (b, "q", juce::jlimit (0.1f, 10.0f, q));
    p->endChangeGesture();
    readBands();
    repaint();
}

//==============================================================================
bool EqDisplay::keyPressed (const juce::KeyPress& key)
{
    if (! commandMode)
        return false; // let the editor see ` to take command

    if (isLeaveCommandKey (key) || isTakeCommandKey (key))
    {
        setCommandMode (false);
        if (onCommandChange)
            onCommandChange (false);
        return true;
    }

    const int code = key.getKeyCode();
    if (heldKeys.count (code) == 0)
    {
        const int dk = doomKeyFor (key);
        if (dk == 0)
            return false;
        heldKeys[code] = dk;
        DoomEngine::pushKey (true, dk);
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
    return true;
}

void EqDisplay::modifierKeysChanged (const juce::ModifierKeys& mods)
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

    update (mods.isCtrlDown(), ctrlDown, KEY_FIRE);
    update (mods.isShiftDown(), shiftDown, KEY_RSHIFT);
    update (mods.isAltDown(), altDown, KEY_RALT);
}

void EqDisplay::focusLost (FocusChangeType)
{
    releaseAllKeys();
    repaint();
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
        addAndMakeVisible (s->on);
        s->onAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, DoomEQProcessor::pid (b, "on"), s->on);

        for (int t = 0; t < eq::numTypes; ++t)
            s->type.addItem (eq::typeNames[t], t + 1);
        addAndMakeVisible (s->type);
        s->typeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, DoomEQProcessor::pid (b, "type"), s->type);

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
        for (auto* k : { &s->freq, &s->gain, &s->q })
            k->updateText();
        s->gain.setDoubleClickReturnValue (true, 0.0);

        strips[(size_t) b] = std::move (s);
    }

    output.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    output.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
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
    doomLevel.setPopupDisplayEnabled (true, true, this);

    setupBar (overlaySlider, overlayLabel, "OVERLAY", Palette::hudYellow);
    overlaySlider.setRange (0.0, 100.0, 1.0);
    overlaySlider.setTextValueSuffix ("%");
    overlaySlider.setPopupDisplayEnabled (true, true, this);
    overlaySlider.setDoubleClickReturnValue (true, 70.0);
    overlaySlider.setValue ((double) proc.apvts.state.getProperty ("overlay", 70.0), juce::dontSendNotification);
    overlaySlider.onValueChange = [this]
    {
        proc.apvts.state.setProperty ("overlay", overlaySlider.getValue(), nullptr);
        display.setOverlayOpacity ((float) overlaySlider.getValue() / 100.0f);
    };
    display.setOverlayOpacity ((float) overlaySlider.getValue() / 100.0f);

    commandButton.setButtonText ("TAKE COMMAND  `");
    commandButton.setClickingTogglesState (false);
    commandButton.onClick = [this] { setCommandMode (! display.isInCommand()); };
    addAndMakeVisible (commandButton);

    // HUD art
    skull[0] = art->patch ("M_SKULL1");
    skull[1] = art->patch ("M_SKULL2");
    for (int hurt = 0; hurt < 5; ++hurt)
        for (int look = 0; look < 3; ++look)
            faces[hurt][look] = art->patch ("STFST" + juce::String (hurt) + juce::String (look));
    ouch = art->patch ("STFOUCH0");

    glContext.setComponentPaintingEnabled (true);
    glContext.attachTo (*this);

    setResizable (true, true);
    setResizeLimits (1000, 760, 2600, 1800);
    setSize (1200, 880);
    startTimerHz (35);
}

DoomEQEditor::~DoomEQEditor()
{
    stopTimer();
    glContext.detach();
    setLookAndFeel (nullptr);
}

void DoomEQEditor::setCommandMode (bool inCommand)
{
    display.setCommandMode (inCommand);
    commandButton.setToggleState (inCommand, juce::dontSendNotification);
    commandButton.setButtonText (inCommand ? "LEAVE  SHIFT+ESC" : "TAKE COMMAND  `");
    if (! inCommand)
        grabKeyboardFocus();
}

bool DoomEQEditor::keyPressed (const juce::KeyPress& key)
{
    if (EqDisplay::isTakeCommandKey (key))
    {
        setCommandMode (! display.isInCommand());
        return true;
    }
    if (EqDisplay::isLeaveCommandKey (key) && display.isInCommand())
    {
        setCommandMode (false);
        return true;
    }
    return false;
}

void DoomEQEditor::mouseDown (const juce::MouseEvent&)
{
    grabKeyboardFocus();
}

void DoomEQEditor::timerCallback()
{
    display.tick();


    for (int b = 0; b < DoomEQProcessor::numBands; ++b)
    {
        auto& s = *strips[(size_t) b];
        const auto t = (eq::Type) s.type.getSelectedItemIndex();
        s.gain.setEnabled (eq::usesGain (t));
    }

    ++frameCount;

    // Skull cursor blinks like the menu one.
    if (frameCount % 8 == 0)
        repaint (skullArea);

    // The marine reacts to your output level.
    const float db = juce::Decibels::gainToDecibels (proc.getOutputPeak(), -100.0f);
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

void DoomEQEditor::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    DoomStyle::fillTextured (g, bounds.toFloat(), tex.background, 2.0f, 0.42f);

    // Header bar with hazard trim.
    {
        auto h = headerArea.expanded (12, 12).withTop (0);
        DoomStyle::fillTextured (g, h.toFloat(), tex.panel, 2.0f, 0.36f);
        DoomStyle::fillTextured (g, h.removeFromBottom (8).toFloat(), tex.hazard, 1.0f, 0.75f);
        DoomStyle::bevel (g, headerArea.expanded (12, 12).withTop (0).withTrimmedBottom (8), 2, Palette::bevelLight.withAlpha (0.5f), Palette::bevelDark);
    }

    // Blinking skull + title.
    const auto& sk = skull[(frameCount / 8) % 2];
    if (sk.isValid())
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (sk, skullArea.toFloat(), juce::RectanglePlacement::centred);
    }
    art->font().drawFitted (g, "DOOM", titleArea.toFloat().withWidth (150.0f), juce::Justification::centredLeft, Palette::doomRed, 4.0f);
    art->font().drawFitted (g, "EQ", titleArea.toFloat().withTrimmedLeft (150.0f).withWidth (80.0f), juce::Justification::centredLeft, Palette::hudYellow, 4.0f);
    if (titleArea.getWidth() > 420)
        art->font().drawFitted (g, "6 BAND PARAMETRIC", titleArea.toFloat().withTrimmedLeft (236.0f),
                                juce::Justification::centredLeft, Palette::textDim, 1.5f);

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

    headerArea = area.removeFromTop (headerHeight - 12);
    {
        auto h = headerArea;
        skullArea = h.removeFromLeft (44);
        h.removeFromLeft (8);

        commandButton.setBounds (h.removeFromRight (210).reduced (0, 4));
        h.removeFromRight (14);
        overlaySlider.setBounds (h.removeFromRight (120).reduced (0, 4));
        overlayLabel.setBounds (h.removeFromRight (100).reduced (0, 6));
        h.removeFromRight (14);
        doomLevel.setBounds (h.removeFromRight (110).reduced (0, 4));
        doomLevelLabel.setBounds (h.removeFromRight (70).reduced (0, 6));
        h.removeFromRight (8);
        doomSound.setBounds (h.removeFromRight (170).reduced (0, 4));
        h.removeFromRight (8);
        titleArea = h;
    }

    area.removeFromTop (20);
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
    o.removeFromTop (8);
    output.setBounds (o);
}
