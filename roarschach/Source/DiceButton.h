#pragma once

#include <JuceHeader.h>
#include "RoarschachLookAndFeel.h"
#include "Theme.h"

#include <functional>

/**
 * A die drawn in vectors on the house's pill button (the LookAndFeel's background, outline, hover,
 * pressed, disabled and focus ring), so it sits with the preset bar's buttons and the tiles' chips.
 * The face shows 1..6 pips (setFace: the dice show a new face after every roll).
 *
 * - setPillSize draws the pill smaller than the button, so the button's clickable area can be larger
 *   than the die looks; setCaption adds a caption under the die that is part of the button too
 *   (it clicks, hovers and shows the tooltip with it).
 * - A right click (or a Ctrl-click), Shift+F10 or a screen reader's "show menu" calls onSecondaryClick,
 *   for the undo menu. Cmd+Z / Shift+Cmd+Z (Ctrl+Z / Ctrl+Shift+Z) while the die has the keyboard
 *   focus calls onUndoKey; the editor drops the die's focus as soon as another control is used, so
 *   those keys go to the host again.
 */
class DiceButton : public juce::Button
{
public:
    /** The die's side as a share of its pill: most of a small pill (a module's die), less of a
        large one, so it does not crowd the round pill. */
    static constexpr float c_dieFraction = 0.75f, c_largeDieFraction = 0.62f;
    static constexpr float c_largePill = 20.0f;
    /** Under this many device pixels the die shows at most four pips, larger (five and six pips
        blur into a grid that small). */
    static constexpr float c_smallDiePixels = 12.0f;
    static constexpr int c_captionGap = 6;

    explicit DiceButton(const juce::String& name) : juce::Button(name)
    {
        setWantsKeyboardFocus(true);
    }

    std::function<void()> onSecondaryClick;
    /** Cmd+Z (redo = false) or Shift+Cmd+Z (redo = true) while the die has the focus. Returns true
        if it undid or redid something (otherwise the key goes on to the host). */
    std::function<bool(bool redo)> onUndoKey;

    /** The pips (1..6). */
    void setFace(int face)
    {
        face = juce::jlimit(1, 6, face);
        if (face != m_face)
        {
            m_face = face;
            repaint();
        }
    }
    int getFace() const noexcept { return m_face; }

    /** The pips' colour on hover (the die itself stays in the house's text colours otherwise). */
    void setAccent(juce::Colour accent) { m_accent = accent; repaint(); }

    /** The pill's side (0: the whole button). A smaller pill leaves a margin that still clicks. */
    void setPillSize(int side) { m_pillSize = side; repaint(); }
    int getPillSize() const noexcept { return m_pillSize; }

    /** A caption under the die, inside the button. */
    void setCaption(const juce::String& text, const juce::Font& font, int height)
    {
        m_caption = text;
        m_captionFont = font;
        m_captionHeight = height;
        repaint();
    }

    /** The pill's area in the button. */
    juce::Rectangle<float> getPillBounds() const
    {
        const auto bounds = getLocalBounds().toFloat();
        const float side = m_pillSize > 0 ? static_cast<float>(m_pillSize)
                                          : std::min(bounds.getWidth(), bounds.getHeight());
        auto pill = juce::Rectangle<float>(side, side).withCentre(bounds.getCentre());
        if (m_caption.isNotEmpty())
            pill.setY(bounds.getY());
        return pill.reduced(0.5f);
    }

    juce::Rectangle<float> getCaptionBounds() const
    {
        const auto pill = getPillBounds().expanded(0.5f);
        return getLocalBounds().toFloat().withTop(pill.getBottom() + static_cast<float>(c_captionGap))
                                         .withHeight(static_cast<float>(m_captionHeight));
    }

    /** The die's side (in logical pixels) drawn on a pill of this side. */
    static float dieSideFor(float pillSide)
    {
        return std::floor(pillSide * (pillSide > c_largePill ? c_largeDieFraction : c_dieFraction));
    }

    /** The face drawn for `face` on a die this many device pixels wide. */
    static int drawnFace(int face, float devicePixels)
    {
        face = juce::jlimit(1, 6, face);
        return devicePixels < c_smallDiePixels && face > 4 ? face - 3 : face;
    }

    /** The die drawn in `area` (also used for pictures and tests). devicePixelsPerUnit picks the
        small die's pips (see c_smallDiePixels). */
    static void drawDie(juce::Graphics& g, juce::Rectangle<float> area, int face, juce::Colour body, juce::Colour pips,
                        float devicePixelsPerUnit = 2.0f)
    {
        const float side = std::floor(std::min(area.getWidth(), area.getHeight()));
        const bool small = side * devicePixelsPerUnit < c_smallDiePixels;
        face = drawnFace(face, side * devicePixelsPerUnit);
        const auto box = juce::Rectangle<float>(side, side).withCentre(area.getCentre());
        const float stroke = juce::jlimit(0.8f, 1.4f, side * 0.08f);
        const auto inner = box.reduced(stroke * 0.5f);
        const float corner = side * 0.24f;

        g.setColour(body.withMultipliedAlpha(0.18f));
        g.fillRoundedRectangle(inner, corner);
        g.setColour(body);
        g.drawRoundedRectangle(inner, corner, stroke);

        // Pips on a 3 x 3 grid inside the face (larger, and further apart, on a small die).
        const float d = small ? side * 0.22f : juce::jmax(1.6f, side * 0.17f);
        const float step = (side - 2.0f * stroke) * (small ? 0.27f : 0.29f);
        const auto c = box.getCentre();
        const auto pip = [&](int col, int row)
        {
            g.fillEllipse(juce::Rectangle<float>(d, d).withCentre({ c.x + static_cast<float>(col) * step, c.y + static_cast<float>(row) * step }));
        };
        g.setColour(pips);
        if (face % 2 == 1)
            pip(0, 0);
        if (face >= 2)
        {
            pip(-1, -1);
            pip(1, 1);
        }
        if (face >= 4)
        {
            pip(1, -1);
            pip(-1, 1);
        }
        if (face == 6)
        {
            pip(-1, 0);
            pip(1, 0);
        }
    }

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isDown) override
    {
        const bool enabled = isEnabled();
        const auto pill = getPillBounds();

        // The house's pill (as the LookAndFeel draws a button), in the pill's area only.
        {
            const float corner = std::min(pill.getWidth(), pill.getHeight()) * 0.5f;
            auto fill = findColour(juce::TextButton::buttonColourId);
            if (!enabled)
                fill = fill.withMultipliedAlpha(0.5f);
            g.setColour(fill.brighter(isDown ? 0.2f : isMouseOver && enabled ? 0.1f : 0.0f));
            g.fillRoundedRectangle(pill, corner);
            if (RoarschachLookAndFeel::shouldShowFocusRing(*this))
            {
                g.setColour(Theme::Colours::output);
                g.drawRoundedRectangle(pill.reduced(0.25f), corner, 1.5f);
            }
            else
            {
                g.setColour(!enabled ? Theme::Colours::outline.withMultipliedAlpha(0.6f)
                                     : isMouseOver ? Theme::Colours::textDim : Theme::Colours::outline);
                g.drawRoundedRectangle(pill, corner, 1.0f);
            }
        }

        auto body = isDown || isMouseOver ? Theme::Colours::text : Theme::Colours::textDim;
        auto pips = (isDown || isMouseOver) && !m_accent.isTransparent() ? m_accent : body;
        if (!enabled)
        {
            body = Theme::Colours::textFaint;
            pips = body;
        }
        // Pressed: the die sinks a little, the house's pressed state for a glyph.
        const float side = dieSideFor(pill.getWidth() + 1.0f);
        const auto area = juce::Rectangle<float>(side, side).withCentre(pill.getCentre().translated(0.0f, isDown ? 0.5f : 0.0f));
        drawDie(g, area, m_face, body, pips, g.getInternalContext().getPhysicalPixelScaleFactor());

        if (m_caption.isNotEmpty())
        {
            g.setFont(m_captionFont);
            g.setColour(!enabled ? Theme::Colours::textFaint : isDown || isMouseOver ? Theme::Colours::text : Theme::Colours::textDim);
            g.drawText(m_caption, getCaptionBounds(), juce::Justification::centred, false);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onSecondaryClick)
                onSecondaryClick();
            return;
        }
        juce::Button::mouseDown(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        juce::Button::mouseUp(e);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress(juce::KeyPress::F10Key, juce::ModifierKeys::shiftModifier, 0))
        {
            if (onSecondaryClick)
                onSecondaryClick();
            return true;
        }

        const bool isUndo = key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0);
        const bool isRedo = key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0);
        if (isUndo || isRedo)
            return onUndoKey != nullptr && onUndoKey(isRedo);

        return juce::Button::keyPressed(key);
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<DiceAccessibilityHandler>(*this);
    }

private:
    /** A button with a "show menu" action too (the undo menu). */
    class DiceAccessibilityHandler final : public juce::AccessibilityHandler
    {
    public:
        explicit DiceAccessibilityHandler(DiceButton& b)
            : juce::AccessibilityHandler(b, juce::AccessibilityRole::button,
                                         juce::AccessibilityActions()
                                             .addAction(juce::AccessibilityActionType::press, [&b] { b.triggerClick(); })
                                             .addAction(juce::AccessibilityActionType::showMenu,
                                                        [&b] { if (b.onSecondaryClick) b.onSecondaryClick(); })),
              m_button(b)
        {
        }

        juce::String getTitle() const override
        {
            const auto title = juce::AccessibilityHandler::getTitle();
            return title.isNotEmpty() ? title : m_button.getButtonText();
        }

        juce::String getHelp() const override { return m_button.getTooltip(); }

    private:
        DiceButton& m_button;
    };

    int m_face = 5;
    juce::Colour m_accent;
    int m_pillSize = 0;
    juce::String m_caption;
    juce::Font m_captionFont { juce::FontOptions {} };
    int m_captionHeight = 0;
};
