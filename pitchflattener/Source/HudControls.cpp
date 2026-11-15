#include "HudControls.h"

#include "PitchFlattenerLookAndFeel.h"

namespace
{
    using namespace Theme::Colours;
    constexpr float c_disabledAlpha = 0.35f;
}

//==============================================================================
HudButton::HudButton (const juce::String& label, Style style, juce::Colour accent)
    : juce::Button (label)
    , m_style (style)
    , m_accent (accent)
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void HudButton::paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown)
{
    if (Hud::skipOnScreen (*this))
        return;
    const auto bounds = getLocalBounds().toFloat();
    const bool enabled = isEnabled();
    const bool on = getToggleState();
    const bool hot = enabled && (isMouseOver || isButtonDown);
    const float alpha = enabled ? 1.0f : c_disabledAlpha;
    const float cut = m_style == Style::Tab ? 5.0f : std::min (9.0f, bounds.getHeight() * 0.3f);

    juce::Colour fill = panel, edge = m_accent.withAlpha (0.55f), label = m_accent;
    switch (m_style)
    {
        case Style::Action:
            fill = hot ? m_accent.withAlpha (isButtonDown ? 0.3f : 0.14f) : panel;
            edge = hot ? amber : m_accent;
            label = hot ? amber : m_accent;
            break;
        case Style::Segment:
        case Style::Tab:
            if (on && ! enabled)
            {
                // Selected but doing nothing now: an outline in the accent, the name dim but legible.
                fill = panel;
                edge = m_accent.withAlpha (0.55f);
                label = textDim;
            }
            else if (on)
            {
                fill = hot ? amber : m_accent;
                edge = fill;
                label = space;
            }
            else
            {
                fill = hot ? m_accent.withAlpha (0.12f) : panel;
                edge = hot ? m_accent : panelEdge.brighter (0.25f);
                label = hot ? amber : textDim;
            }
            break;
    }

    // (A dimmed selected segment keeps full strength: its outline and text already say "dimmed".)
    const float strength = on && ! enabled ? 1.0f : alpha;
    Hud::frame (g, bounds, cut, fill.withMultipliedAlpha (strength), edge.withMultipliedAlpha (strength));
    if (PitchFlattenerLookAndFeel::shouldShowFocusRing (*this))
        Hud::frame (g, bounds.expanded (0.0f), cut, juce::Colours::transparentBlack, amber, 2.0f);

    const float h = std::min (13.0f, std::round (bounds.getHeight() * 0.42f));
    const auto area = bounds.reduced (m_style == Style::Action ? 8.0f : 4.0f, 2.0f);
    const auto words = getButtonText().toUpperCase();
    const float fitted = Hud::fittedHeight (*m_fonts, true, words, area.getWidth(), h, 10.0f);
    Hud::drawSheared (g, words, area, m_fonts->display (fitted), label.withMultipliedAlpha (strength), juce::Justification::centred);
}

//==============================================================================
LearnButton::LearnButton (const Hud::Clock& clock)
    : juce::Button ("Learn")
    , m_clock (clock)
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void LearnButton::setContent (const juce::String& word, const juce::String& hint, juce::Colour accent, bool filled, bool pulsing)
{
    if (word == m_word && hint == m_hint && accent == m_accent && filled == m_filled && pulsing == m_pulsing)
        return;
    m_word = word;
    m_hint = hint;
    m_accent = accent;
    m_filled = filled;
    m_pulsing = pulsing;
    repaint();
}

void LearnButton::paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown)
{
    if (Hud::skipOnScreen (*this))
        return;
    const auto bounds = getLocalBounds().toFloat();
    const bool hot = isEnabled() && (isMouseOver || isButtonDown);
    const float cut = 14.0f;

    juce::Colour accent = m_accent, fill = panel, ink = m_accent;
    if (m_filled)
    {
        fill = accent;
        ink = space;
    }
    else if (m_pulsing)
    {
        // The next thing to press: a lime glow that beats, its edge always lit.
        const float p = Hud::pulse (m_clock.now(), 1.1);
        accent = lime;
        fill = lime.withAlpha (0.08f + 0.22f * p);
        ink = lime;
    }
    if (hot && ! m_filled)
        fill = fill.interpolatedWith (accent, isButtonDown ? 0.3f : 0.12f);

    Hud::frame (g, bounds, cut, fill, hot && ! m_filled ? amber.interpolatedWith (accent, 0.5f) : accent, 2.0f);
    if (PitchFlattenerLookAndFeel::shouldShowFocusRing (*this))
        Hud::frame (g, bounds.reduced (3.0f), cut - 2.0f, juce::Colours::transparentBlack, amber, 1.5f);

    // Speed stripes at the left, then the word and the hint.
    auto area = bounds.reduced (16.0f, 8.0f);
    const float stripes = Hud::speedStripes (g, area.withWidth (60.0f).withSizeKeepingCentre (60.0f, 22.0f).withX (area.getX()),
                                             ink.withMultipliedAlpha (m_filled ? 0.8f : 0.9f), 3);
    area.removeFromLeft (stripes + 10.0f);
    auto top = area.removeFromTop (area.getHeight() * 0.6f);
    const float wordHeight = Hud::fittedHeight (*m_fonts, true, m_word, top.getWidth(), 26.0f, 12.0f);
    Hud::drawSheared (g, m_word, top, m_fonts->display (wordHeight), ink, juce::Justification::bottomLeft);
    const auto hintFont = m_fonts->label (Hud::fitHeight ([this] (float h) { return m_fonts->label (h); }, m_hint, area.getWidth(), 11.5f, 10.0f, false));
    Hud::drawText (g, m_hint, area.withTrimmedTop (1.0f), hintFont, m_filled ? space.withAlpha (0.85f) : ink.withAlpha (0.8f), juce::Justification::topLeft);
}
