#include "UpdatePopup.h"

namespace
{
    constexpr int cardWidth = 380, cardHeight = 178;
    constexpr int padding = 26;
    constexpr int buttonHeight = 30;

    juce::Font buttonFont() { return Theme::caps (11.5f, 0.22f); }
}

//==============================================================================
UpdatePopup::TextButton::TextButton (const juce::String& label, bool isPrimary)
    : juce::Button (label), primary (isPrimary)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setWantsKeyboardFocus (true);
}

int UpdatePopup::TextButton::idealWidth() const
{
    return (int) std::ceil (juce::GlyphArrangement::getStringWidth (buttonFont(), getButtonText().toUpperCase()))
           + (primary ? 36 : 16);
}

void UpdatePopup::TextButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat();
    if (down)
        r = r.reduced (0.5f);

    const bool focused = hasKeyboardFocus (false);

    if (primary)
    {
        g.setColour (palette.accent.withAlpha (highlighted || down ? 0.12f : 0.06f));
        g.fillRect (r.reduced (0.5f));
        g.setColour (palette.accent.withAlpha (focused || highlighted ? 1.0f : 0.75f));
        g.drawRect (r.reduced (0.5f), focused ? 1.5f : 1.0f);
    }

    g.setColour (primary ? palette.accent : (highlighted || focused) ? palette.ink : palette.muted);
    g.setFont (buttonFont());
    g.drawText (getButtonText().toUpperCase(), r, juce::Justification::centred, false);

    // Focus on the plain buttons: a short accent rule under the word.
    if (! primary && focused)
    {
        const auto w = juce::GlyphArrangement::getStringWidth (buttonFont(), getButtonText().toUpperCase());
        g.setColour (palette.accent);
        g.fillRect (juce::Rectangle<float> (w, 1.0f).withCentre ({ r.getCentreX(), r.getCentreY() + 9.0f }));
    }
}

//==============================================================================
UpdatePopup::UpdatePopup (sammyj::UpdateInfo updateInfo) : info (std::move (updateInfo))
{
    setWantsKeyboardFocus (true);
    setFocusContainerType (FocusContainerType::keyboardFocusContainer);
    setInterceptsMouseClicks (true, true);

    for (auto* b : { &download, &later, &skip })
        addAndMakeVisible (b);

    download.setExplicitFocusOrder (1);
    later.setExplicitFocusOrder (2);
    skip.setExplicitFocusOrder (3);

    download.onClick = [this]
    {
        info.openDownloadPage();
        dismiss();
    };
    later.onClick = [this] { dismiss(); };
    skip.onClick = [this]
    {
        sammyj::UpdateCheck::skipVersion (info);
        dismiss();
    };
}

void UpdatePopup::setPalette (const Theme::Palette& p)
{
    palette = p;
    for (auto* b : { &download, &later, &skip })
        b->setPalette (p);
    repaint();
}

void UpdatePopup::focusDefaultButton()
{
    if (isShowing())
        download.grabKeyboardFocus();
}

void UpdatePopup::dismiss()
{
    if (onDismiss)
        onDismiss();
}

juce::Rectangle<int> UpdatePopup::cardBounds() const
{
    return juce::Rectangle<int> (juce::jmin (cardWidth, getWidth() - 32), juce::jmin (cardHeight, getHeight() - 32))
        .withCentre (getLocalBounds().getCentre());
}

void UpdatePopup::paint (juce::Graphics& g)
{
    // The page underneath, dimmed: towards the paper's own shade by day, towards black at night.
    const bool night = palette.background.getPerceivedBrightness() < 0.5f;
    g.fillAll (night ? juce::Colours::black.withAlpha (0.5f) : palette.backgroundEdge.darker (0.6f).withAlpha (0.55f));

    const auto card = cardBounds();
    juce::DropShadow (juce::Colours::black.withAlpha (0.22f), 24, { 0, 8 }).drawForRectangle (g, card);

    const auto r = card.toFloat();
    g.setGradientFill (juce::ColourGradient (palette.background, r.getCentreX(), r.getY() + r.getHeight() * 0.4f,
                                             palette.backgroundEdge, r.getX(), r.getBottom() + r.getHeight() * 0.3f, true));
    g.fillRect (r);

    // The same inset hairline frame as the page, like a printed card.
    g.setColour (palette.hairline.withMultipliedAlpha (0.8f));
    g.drawRect (r.reduced (6.0f), 1.0f);

    auto text = card.reduced (padding, 0);
    text.removeFromTop (padding);

    g.setColour (palette.muted.withMultipliedAlpha (0.9f));
    g.setFont (Theme::caps (10.5f, 0.3f));
    g.drawText ("UPDATE", text.removeFromTop (16), juce::Justification::centredLeft, false);

    text.removeFromTop (6);
    g.setColour (palette.ink);
    g.setFont (Theme::text (25.0f));
    g.drawFittedText ("Sound Advice " + info.latestVersion + " is available", text.removeFromTop (30),
                      juce::Justification::centredLeft, 1, 0.85f);

    text.removeFromTop (2);
    g.setColour (palette.muted);
    g.setFont (Theme::italic (18.0f));
    g.drawFittedText ("You have " + info.currentVersion + ".", text.removeFromTop (24),
                      juce::Justification::centredLeft, 1, 0.9f);
}

void UpdatePopup::resized()
{
    auto row = cardBounds().reduced (padding - 6, 0);
    row = row.removeFromBottom (buttonHeight + 22).removeFromTop (buttonHeight);

    download.setBounds (row.removeFromRight (download.idealWidth()));
    row.removeFromRight (8);
    later.setBounds (row.removeFromRight (later.idealWidth()));
    skip.setBounds (row.removeFromLeft (skip.idealWidth()));
}

bool UpdatePopup::keyPressed (const juce::KeyPress& key)
{
    const auto code = key.getKeyCode();

    if (code == juce::KeyPress::escapeKey)
    {
        dismiss();
        return true;
    }

    // A focused button takes Return itself (juce::Button::keyPressed); otherwise Return downloads.
    if (code == juce::KeyPress::returnKey)
    {
        download.triggerClick();
        return true;
    }

    // Keep the page's own keys (another quote, back, categories) from acting behind the card.
    return code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey
        || code == juce::KeyPress::downKey || code == juce::KeyPress::backspaceKey;
}
