#pragma once

#include <JuceHeader.h>

#include "LookAndFeel.h"
#include "Parameters.h"
#include "Theme.h"

#include <memory>

/**
 * Which way a bus's passes fly (Direction), or which side of you they take (Side): a stamped three-way switch
 * beside the bus's Flight Path, each of its three keys showing what it does on that path rather than naming it.
 *
 *   Direction   Random (a two-headed arrow along the path), then the two ways: left / right arrows on Across (and
 *               at Stereo, where every pass flies across), down / up on Through and Overhead (the plan has the
 *               front at the top, so "front to back" points down), a turning arrow each way on Around.
 *   Side        Random (a head with a mark on both sides), then a mark on the left or the right of the head
 *               (Through, Overhead), or in front or behind it (Around: where the pass is loudest). Across has no
 *               sides: the switch rests.
 *
 * It is a modulation target like a knob (a ModTarget): a wire decides pass by pass, so while one is plugged in the
 * key its modulator picks now is marked under the switch in the wire's colour and the switch's own choice is drawn
 * quieter. Bus B's, while Link is on, shows bus A's choice (setShown) in the Linked look, and a click there does
 * nothing: bus B flies bus A's passes. Clicking a key chooses it (one undo step); a double-click goes back to Random.
 *
 * The words for the choice in hand ("Left to Right", "Loudest Behind") are the first line of its tooltip; the
 * window writes the rest of the tooltip. Message thread only.
 */
class PassSwitch final : public ModTarget
{
public:
    enum class Kind { Direction, Side };

    PassSwitch(Kind kind, juce::Colour accent) : m_kind(kind), m_accent(accent)
    {
        setSliderStyle(juce::Slider::LinearHorizontal);
        setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        setRange(0.0, 2.0, 1.0);
        setDoubleClickReturnValue(true, 0.0);
        setWantsKeyboardFocus(false);
        setScrollWheelEnabled(false);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        getProperties().set("howTo", "Click a key to choose it; double-click to go back to Random. Drag a Mod jack onto it to have a "
                                     "modulator decide, pass by pass.");
    }

    /** Follows a choice parameter (Random, then the two ways or sides) and sets it, each click one undo step. */
    void attach(juce::RangedAudioParameter& parameter)
    {
        m_attachment = std::make_unique<juce::ParameterAttachment>(parameter, [this](float value)
        {
            m_syncing = true;
            setValue(value, juce::sendNotificationSync);
            m_syncing = false;
        });
        m_attachment->sendInitialUpdate();
    }

    /** The Flight Path its keys are drawn for (0 Across, 1 Through, 2 Overhead, 3 Around). The window passes Across
        at Mono and Stereo, where every pass flies across. */
    void setPath(int path)
    {
        if (path == m_path)
            return;
        m_path = path;
        updateWords();
        repaint();
    }
    int getPath() const { return m_path; }

    /** Shows this choice instead of its own (bus B while Link is on: bus A's), and takes no clicks; -1 = its own. */
    void setShown(int choice)
    {
        if (choice == m_shown)
            return;
        m_shown = choice;
        updateWords();
        repaint();
    }

    /** The choice it shows: 0 Random, 1 the first way / side, 2 the other. */
    int shownChoice() const { return m_shown >= 0 ? m_shown : juce::jlimit(0, 2, juce::roundToInt(getValue())); }
    /** Its own choice, whatever it shows. */
    int ownChoice() const { return juce::jlimit(0, 2, juce::roundToInt(getValue())); }

    /** The words for a choice on this path ("Left to Right", "Loudest Behind"); empty for a Side on Across. */
    juce::String wordsFor(int choice) const
    {
        if (m_kind == Kind::Direction)
            return Params::directionLabel(m_path, choice);
        const char* words = Params::sideLabel(m_path, choice);
        return words != nullptr ? juce::String(words) : juce::String();
    }
    juce::String words() const { return wordsFor(shownChoice()); }

    /** The key a wire's modulator picks now (1 or 2), or -1 when nothing is wired. */
    int wiredChoice() const
    {
        if (m_modArcs.empty() || m_modLive < 0.0f)
            return -1;
        return m_modLive < 0.75f ? 1 : 2;
    }

    /** The three keys, in this control's coordinates. */
    juce::Rectangle<float> keyArea(int key) const
    {
        const auto r = getLocalBounds().toFloat();
        const float w = r.getWidth() / 3.0f;
        return { r.getX() + w * static_cast<float>(key), r.getY(), w, r.getHeight() - c_wireRow };
    }

    //==========================================================================================================
    void paint(juce::Graphics& g) override
    {
        namespace TC = Theme::Colours;
        const auto look = Theme::getLook(*this);
        const int chosen = shownChoice();
        const int wired = wiredChoice();
        const bool resting = look == Theme::Look::Resting || look == Theme::Look::Disabled;
        const auto accent = m_shown >= 0 ? m_shownAccent : m_accent;
        for (int key = 0; key < 3; ++key)
        {
            const auto area = keyArea(key).reduced(key == 0 ? 0.0f : 0.5f, 0.0f).withTrimmedLeft(key == 0 ? 0.0f : 0.5f);
            const bool on = key == chosen && wired < 0;
            const bool over = m_over == key && m_shown < 0 && isEnabled();
            SwooshLookAndFeel::drawControlPlate(g, area, accent, look, on, over, false);
            // Linked: the plate is flat, so the key that flies is marked by its glyph in the bus's colour.
            juce::Colour ink = on && look == Theme::Look::Normal ? TC::window
                             : resting ? TC::textFaint
                             : look == Theme::Look::Linked ? (key == chosen ? accent : TC::textFaint)
                             : wired >= 0 && key == chosen ? TC::textFaint
                             : TC::text;
            if (look == Theme::Look::Resting && key == chosen)
                ink = accent.withAlpha(0.9f);
            drawGlyph(g, SwooshLookAndFeel::plateFace(area, false).reduced(3.0f, 2.0f), key, ink);
        }

        // A wire decides: the key it picks now is underlined in each wire's colour, under the switch.
        if (wired >= 0)
        {
            const auto key = keyArea(wired);
            float y = static_cast<float>(getHeight()) - 1.0f;
            for (const auto& arc : m_modArcs)
            {
                g.setColour(arc.colour);
                g.fillRect(juce::Rectangle<float>(key.getX() + 2.0f, y - 1.5f, key.getWidth() - 4.0f, 1.5f));
                y -= 2.0f;
            }
        }
        if (! m_wireHover.isTransparent())
        {
            g.setColour(m_wireHover.withAlpha(0.85f));
            Theme::Plate::stroke(g, getLocalBounds().toFloat(), Theme::Cut::small, 2.0f);
        }
    }

    /** Bus B's colour while it shows bus A's choice. */
    void setShownAccent(juce::Colour c) { m_shownAccent = c; }

    //==========================================================================================================
    void mouseMove(const juce::MouseEvent& e) override { setOver(keyAt(e.position)); }
    void mouseEnter(const juce::MouseEvent& e) override { setOver(keyAt(e.position)); }
    void mouseExit(const juce::MouseEvent&) override { setOver(-1); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isCommandDown() || e.mods.isPopupMenu())
        {
            ModTarget::mouseDown(e);
            return;
        }
        if (m_shown >= 0)
            return; // linked: it shows what it flies, and keeps its own choice for when Link goes off
        if (const int key = keyAt(e.position); key >= 0)
            setValue(key, juce::sendNotificationSync);
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (m_modGesture)
            ModTarget::mouseDrag(e);
    }
    void mouseUp(const juce::MouseEvent& e) override
    {
        if (m_modGesture)
            ModTarget::mouseUp(e);
    }
    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.mods.isCommandDown())
        {
            ModTarget::mouseDoubleClick(e);
            return;
        }
        if (m_shown < 0)
            setValue(0.0, juce::sendNotificationSync);
    }

    void valueChanged() override
    {
        if (! m_syncing && m_attachment != nullptr)
            m_attachment->setValueAsCompleteGesture(static_cast<float>(getValue()));
        updateWords();
        repaint();
    }

    juce::Point<float> getDialCentre() const override { return getLocalBounds().toFloat().getCentre(); }
    float getDialRadius() const override { return static_cast<float>(getHeight()) * 0.5f + 2.0f; }
    // A flat strip near the panel's foot: its plugs sit in a row just over it, clear of the keys.
    juce::Point<float> getPlugPosition(int index) const override
    {
        return { static_cast<float>(getWidth()) - 6.0f - 11.0f * static_cast<float>(index), -6.0f };
    }

    static constexpr float c_wireRow = 3.0f; // under the keys: where a wire's pick is marked

private:
    int keyAt(juce::Point<float> p) const
    {
        for (int key = 0; key < 3; ++key)
            if (keyArea(key).expanded(0.0f, c_wireRow).contains(p))
                return key;
        return -1;
    }
    void setOver(int key)
    {
        if (key == m_over)
            return;
        m_over = key;
        repaint();
    }
    // (A Side on Across has no words of its own: its tooltip says so, not the bare number of its choice.)
    void updateWords() { getProperties().set("tipLine", words().isNotEmpty() ? words() : juce::String("No Sides")); }

    // The glyphs: drawn in strokes with mitered, square ends, and filled arrowheads.
    static void arrowHead(juce::Path& p, juce::Point<float> tip, juce::Point<float> from, float size)
    {
        const auto d = (tip - from) / juce::jmax(0.001f, tip.getDistanceFrom(from));
        const juce::Point<float> n { -d.y, d.x };
        p.addTriangle(tip, tip - d * size + n * size * 0.62f, tip - d * size - n * size * 0.62f);
    }
    static void line(juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, float thickness)
    {
        juce::Path p;
        p.startNewSubPath(a);
        p.lineTo(b);
        g.strokePath(p, juce::PathStrokeType(thickness, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    }

    void drawGlyph(juce::Graphics& g, juce::Rectangle<float> box, int key, juce::Colour ink) const
    {
        g.setColour(ink);
        const auto c = box.getCentre();
        const float s = juce::jmin(box.getWidth(), box.getHeight()) * 0.5f; // half the glyph's size
        constexpr float stroke = 1.5f;
        const float head = juce::jmax(3.2f, s * 0.62f);
        if (m_kind == Kind::Direction)
        {
            if (m_path == 3)
            {
                // Around: most of a turn, its head at the end it goes to (clockwise: the plan seen from above).
                const float r = s * 0.78f;
                const float gap = 0.9f;                    // radians left open at the top, where the head sits
                const float start = gap * 0.5f, end = juce::MathConstants<float>::twoPi - gap * 0.5f;
                juce::Path arc;
                arc.addCentredArc(c.x, c.y, r, r, 0.0f, start, end, true);
                g.strokePath(arc, juce::PathStrokeType(stroke, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
                auto at = [c, r](float a) { return juce::Point<float>(c.x + r * std::sin(a), c.y - r * std::cos(a)); };
                juce::Path heads;
                // Clockwise ends at `end` heading on clockwise; anticlockwise ends at `start` heading back.
                if (key != 2)
                    arrowHead(heads, at(end) + juce::Point<float>(head * 0.55f, 0.0f), at(end - 0.35f), head);
                if (key != 1)
                    arrowHead(heads, at(start) - juce::Point<float>(head * 0.55f, 0.0f), at(start + 0.35f), head);
                g.fillPath(heads);
                return;
            }
            // Across: left and right. Through and Overhead: front (up) and back (down).
            const bool across = m_path == 0;
            const auto axis = across ? juce::Point<float>(s, 0.0f) : juce::Point<float>(0.0f, s);
            const auto a = c - axis, b = c + axis; // a: left / front, b: right / back
            juce::Path heads;
            if (key == 0)
            {
                line(g, a + (b - a) * 0.18f, b - (b - a) * 0.18f, stroke);
                arrowHead(heads, a, b, head);
                arrowHead(heads, b, a, head);
            }
            else
            {
                const auto from = key == 1 ? a : b, to = key == 1 ? b : a; // Forwards: left to right, front to back
                line(g, from, to - (to - from) * 0.2f, stroke);
                arrowHead(heads, to, from, head);
            }
            g.fillPath(heads);
            return;
        }

        // Side: a head (a small ring, round object: the listener) with a mark where the pass goes by.
        const float headR = juce::jmax(2.2f, s * 0.36f);
        g.drawEllipse(juce::Rectangle<float>(2.0f * headR, 2.0f * headR).withCentre(c), 1.2f);
        if (m_path == 0)
            return; // Across: no sides
        const bool around = m_path == 3;
        const float off = s * 0.92f, mark = juce::jmax(3.0f, s * 0.5f);
        auto markAt = [&](float sign, bool solid)
        {
            const auto p = around ? juce::Point<float>(c.x, c.y - sign * off) : juce::Point<float>(c.x + sign * off, c.y);
            const auto r = juce::Rectangle<float>(mark, mark).withCentre(p);
            if (solid)
                g.fillRect(r);
            else
                g.drawRect(r, 1.0f);
        };
        // Left (or, going Around, loudest in front) is the first key; right (or behind) the second.
        if (key == 0)
        {
            markAt(-1.0f, false);
            markAt(1.0f, false);
        }
        else
            markAt(around ? (key == 1 ? 1.0f : -1.0f) : (key == 1 ? -1.0f : 1.0f), true);
    }

    Kind m_kind;
    juce::Colour m_accent, m_shownAccent { Theme::Colours::busA };
    int m_path = 0, m_shown = -1, m_over = -1;
    bool m_syncing = false;
    std::unique_ptr<juce::ParameterAttachment> m_attachment;
};
