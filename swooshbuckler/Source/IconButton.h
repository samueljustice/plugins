#pragma once

#include <JuceHeader.h>
#include "Theme.h"

/**
 * A small button with a drawn icon instead of text (undo, redo, the roll history, keep, the section dice and
 * locks). It uses the same plate as the text buttons; a toggled one lights in its accent colour.
 */
class IconButton : public juce::Button
{
public:
    enum class Icon { Undo, Redo, Back, Forward, Star, Die, Lock };

    IconButton(Icon icon, juce::Colour accent) : juce::Button({}), m_icon(icon)
    {
        setColour(juce::TextButton::buttonOnColourId, accent);
        setWantsKeyboardFocus(false);
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        namespace TC = Theme::Colours;
        getLookAndFeel().drawButtonBackground(g, *this, findColour(juce::TextButton::buttonColourId), over, down);
        const auto accent = findColour(juce::TextButton::buttonOnColourId);
        const bool on = getToggleState();
        const auto colour = ! isEnabled() ? TC::textFaint.withAlpha(0.5f) : (on ? accent.brighter(0.4f) : (over ? TC::text : TC::textDim));
        const float side = juce::jmin(static_cast<float>(getWidth()), static_cast<float>(getHeight())) - 6.0f;
        const auto box = juce::Rectangle<float>(side, side).withCentre(getLocalBounds().toFloat().getCentre());
        auto at = [&box](float x, float y) { return juce::Point<float>(box.getX() + x * box.getWidth(), box.getY() + y * box.getHeight()); };
        const float line = juce::jmax(1.2f, side * 0.11f);
        const juce::PathStrokeType stroke(line, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.setColour(colour);

        switch (m_icon)
        {
            case Icon::Undo:
            case Icon::Redo:
            {
                // A line back along the top, round the end and home: the arrowhead is where it comes out.
                const bool redo = m_icon == Icon::Redo;
                auto mirrored = [&at, redo](float x, float y) { return at(redo ? 1.0f - x : x, y); };
                juce::Path p;
                p.startNewSubPath(mirrored(0.18f, 0.36f));
                p.lineTo(mirrored(0.60f, 0.36f));
                p.cubicTo(mirrored(0.92f, 0.36f), mirrored(0.92f, 0.80f), mirrored(0.60f, 0.80f));
                p.lineTo(mirrored(0.34f, 0.80f));
                p.startNewSubPath(mirrored(0.38f, 0.16f));
                p.lineTo(mirrored(0.18f, 0.36f));
                p.lineTo(mirrored(0.38f, 0.56f));
                g.strokePath(p, stroke);
                break;
            }
            case Icon::Back:
            case Icon::Forward:
            {
                const bool forward = m_icon == Icon::Forward;
                auto mirrored = [&at, forward](float x, float y) { return at(forward ? 1.0f - x : x, y); };
                juce::Path p;
                p.addTriangle(mirrored(0.24f, 0.5f), mirrored(0.74f, 0.18f), mirrored(0.74f, 0.82f));
                g.fillPath(p);
                break;
            }
            case Icon::Star:
            {
                juce::Path p;
                p.addStar(box.getCentre(), 5, box.getWidth() * 0.22f, box.getWidth() * 0.48f, 0.0f);
                if (on || down)
                    g.fillPath(p);
                g.strokePath(p, juce::PathStrokeType(juce::jmax(1.0f, line * 0.8f), juce::PathStrokeType::mitered));
                break;
            }
            case Icon::Die:
            {
                const auto face = box.reduced(box.getWidth() * 0.08f);
                g.drawRoundedRectangle(face, face.getWidth() * 0.22f, juce::jmax(1.0f, line * 0.85f));
                const float pip = juce::jmax(1.6f, face.getWidth() * 0.17f);
                for (const auto& c : { juce::Point<float>(0.28f, 0.28f), juce::Point<float>(0.72f, 0.28f), juce::Point<float>(0.5f, 0.5f),
                                       juce::Point<float>(0.28f, 0.72f), juce::Point<float>(0.72f, 0.72f) })
                    g.fillEllipse(juce::Rectangle<float>(pip, pip).withCentre({ face.getX() + c.x * face.getWidth(), face.getY() + c.y * face.getHeight() }));
                break;
            }
            case Icon::Lock:
            {
                // Closed (on): the shackle sits in the body. Open (off): it is lifted and swung clear on one side.
                const auto body = juce::Rectangle<float>(at(0.18f, 0.48f), at(0.82f, 0.96f));
                juce::Path shackle;
                const float lift = on ? 0.0f : 0.12f;
                shackle.startNewSubPath(at(0.32f, 0.48f));
                shackle.lineTo(at(0.32f, 0.36f - lift));
                shackle.cubicTo(at(0.32f, 0.04f - lift), at(0.68f, 0.04f - lift), at(0.68f, 0.36f - lift));
                if (on)
                    shackle.lineTo(at(0.68f, 0.48f));
                g.strokePath(shackle, juce::PathStrokeType(juce::jmax(1.0f, line * 0.9f), juce::PathStrokeType::curved, juce::PathStrokeType::butt));
                if (on)
                    g.fillRoundedRectangle(body, body.getWidth() * 0.15f);
                else
                    g.drawRoundedRectangle(body.reduced(line * 0.4f), body.getWidth() * 0.15f, juce::jmax(1.0f, line * 0.8f));
                break;
            }
        }
    }

private:
    Icon m_icon;
};
