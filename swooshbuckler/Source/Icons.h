#pragma once

#include <JuceHeader.h>

/**
 * The drawn icons: every glyph the interface uses in place of a letter or a word, drawn as strokes in one weight
 * so they sit together. Used by IconButton (an icon alone), by text buttons that carry one at their left
 * (Icons::set, drawn by the look and feel) and by the panels' nameplates.
 */
namespace Icons
{
    enum class Icon
    {
        None = 0,
        Undo, Redo, Back, Forward, Star, Die, Lock,
        Remove,             // a cross: take this away
        Previous, Next,     // chevrons: step through a list
        Play, Stop,
        Settings,           // three sliders
        New,                // a blank page
        Clock,              // keep the last minute
        Cables,             // a wire between two plugs
        Clear,              // a wire pulled out
        Takes,              // a stack of takes
        Variations,         // a fan of copies
        Link,               // two links of a chain: this follows something else
        // The marks on the panels' nameplates.
        Tracks, Ride, Hits, Output, Modulation, LetterA, LetterB,
        LetterS             // Stowaway: the extra bus, by the letter its tracks' tags carry
    };

    inline const juce::Identifier property { "icon" };
    /** Gives a text button an icon at the left of its text. */
    inline void set(juce::Component& c, Icon icon)
    {
        if (static_cast<int>(c.getProperties().getWithDefault(property, 0)) == static_cast<int>(icon))
            return;
        c.getProperties().set(property, static_cast<int>(icon));
        c.repaint();
    }
    inline Icon get(const juce::Component& c) { return static_cast<Icon>(static_cast<int>(c.getProperties().getWithDefault(property, 0))); }

    /** The face a die shows. Randomise sets it while the die tumbles and puts it back to five afterwards; it is
        the only thing that changes it, so the die is still five everywhere else. */
    inline const juce::Identifier pipsProperty { "diePips" };
    inline void setPips(juce::Component& c, int pips)
    {
        if (static_cast<int>(c.getProperties().getWithDefault(pipsProperty, 5)) == pips)
            return;
        c.getProperties().set(pipsProperty, pips);
        c.repaint();
    }
    inline int getPips(const juce::Component& c) { return juce::jlimit(1, 6, static_cast<int>(c.getProperties().getWithDefault(pipsProperty, 5))); }

    /** Draws the icon inside a square box in one colour. filled: the lit form (a kept star, a closed lock).
        pips: the face a die shows (five by default). */
    inline void draw(juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour, bool filled = false, int pips = 5)
    {
        const float side = juce::jmin(area.getWidth(), area.getHeight());
        const auto box = juce::Rectangle<float>(side, side).withCentre(area.getCentre());
        auto at = [&box](float x, float y) { return juce::Point<float>(box.getX() + x * box.getWidth(), box.getY() + y * box.getHeight()); };
        const float line = juce::jmax(1.3f, side * 0.11f);
        const juce::PathStrokeType stroke(line, juce::PathStrokeType::mitered, juce::PathStrokeType::butt); // hard: square ends, sharp joins
        g.setColour(colour);
        juce::Path p;

        switch (icon)
        {
            case Icon::None:
                break;
            case Icon::Undo:
            case Icon::Redo:
            {
                // A line back along the top, round the end and home: the arrowhead is where it comes out.
                const bool redo = icon == Icon::Redo;
                auto mirrored = [&at, redo](float x, float y) { return at(redo ? 1.0f - x : x, y); };
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
                const bool forward = icon == Icon::Forward;
                auto mirrored = [&at, forward](float x, float y) { return at(forward ? 1.0f - x : x, y); };
                p.addTriangle(mirrored(0.24f, 0.5f), mirrored(0.74f, 0.18f), mirrored(0.74f, 0.82f));
                g.fillPath(p);
                break;
            }
            case Icon::Previous:
            case Icon::Next:
            {
                const bool next = icon == Icon::Next;
                auto mirrored = [&at, next](float x, float y) { return at(next ? 1.0f - x : x, y); };
                p.startNewSubPath(mirrored(0.60f, 0.20f));
                p.lineTo(mirrored(0.32f, 0.50f));
                p.lineTo(mirrored(0.60f, 0.80f));
                g.strokePath(p, stroke);
                break;
            }
            case Icon::Star:
            {
                p.addStar(box.getCentre(), 5, box.getWidth() * 0.22f, box.getWidth() * 0.48f, 0.0f);
                if (filled)
                    g.fillPath(p);
                g.strokePath(p, juce::PathStrokeType(juce::jmax(1.0f, line * 0.8f), juce::PathStrokeType::mitered));
                break;
            }
            case Icon::Die:
            {
                const auto face = box.reduced(box.getWidth() * 0.08f);
                g.drawRect(face, juce::jmax(1.0f, line * 0.85f));
                const float pip = juce::jmax(1.6f, face.getWidth() * 0.17f);
                // The six faces, as pips are laid out on a die. Round objects: the pips.
                static const char* faces[6] = { "....X....", "X.......X", "X...X...X", "X.X...X.X", "X.X.X.X.X", "X.XX.XX.X" };
                const char* layout = faces[juce::jlimit(1, 6, pips) - 1];
                for (int cell = 0; cell < 9; ++cell)
                {
                    if (layout[cell] != 'X')
                        continue;
                    const float x = 0.28f + 0.22f * static_cast<float>(cell % 3);
                    const float y = 0.28f + 0.22f * static_cast<float>(cell / 3);
                    g.fillEllipse(juce::Rectangle<float>(pip, pip).withCentre({ face.getX() + x * face.getWidth(),
                                                                               face.getY() + y * face.getHeight() }));
                }
                break;
            }
            case Icon::Lock:
            {
                // Closed (filled): the shackle sits in the body. Open: it is lifted and swung clear on one side.
                const auto body = juce::Rectangle<float>(at(0.18f, 0.48f), at(0.82f, 0.96f));
                const float lift = filled ? 0.0f : 0.12f;
                p.startNewSubPath(at(0.32f, 0.48f));
                p.lineTo(at(0.32f, 0.36f - lift));
                p.cubicTo(at(0.32f, 0.04f - lift), at(0.68f, 0.04f - lift), at(0.68f, 0.36f - lift));
                if (filled)
                    p.lineTo(at(0.68f, 0.48f));
                g.strokePath(p, juce::PathStrokeType(juce::jmax(1.0f, line * 0.9f), juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
                if (filled)
                    g.fillRect(body);
                else
                    g.drawRect(body, juce::jmax(1.0f, line * 0.8f));
                break;
            }
            case Icon::Remove:
            {
                p.startNewSubPath(at(0.26f, 0.26f));
                p.lineTo(at(0.74f, 0.74f));
                p.startNewSubPath(at(0.74f, 0.26f));
                p.lineTo(at(0.26f, 0.74f));
                g.strokePath(p, stroke);
                break;
            }
            case Icon::Play:
            {
                p.addTriangle(at(0.26f, 0.14f), at(0.86f, 0.5f), at(0.26f, 0.86f));
                g.fillPath(p);
                break;
            }
            case Icon::Stop:
            {
                g.fillRect(juce::Rectangle<float>(at(0.2f, 0.2f), at(0.8f, 0.8f)));
                break;
            }
            case Icon::Settings:
            {
                // Three sliders, each with its thumb somewhere else.
                const float thumbs[3] = { 0.66f, 0.34f, 0.58f };
                for (int i = 0; i < 3; ++i)
                {
                    const float y = 0.22f + 0.28f * static_cast<float>(i);
                    p.startNewSubPath(at(0.12f, y));
                    p.lineTo(at(0.88f, y));
                }
                g.strokePath(p, juce::PathStrokeType(juce::jmax(1.0f, line * 0.8f), juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
                for (int i = 0; i < 3; ++i)
                    g.fillRect(juce::Rectangle<float>(side * 0.16f, side * 0.26f).withCentre(at(thumbs[i], 0.22f + 0.28f * static_cast<float>(i)))); // block caps
                break;
            }
            case Icon::New:
            {
                // A page with its corner folded.
                p.startNewSubPath(at(0.24f, 0.10f));
                p.lineTo(at(0.58f, 0.10f));
                p.lineTo(at(0.78f, 0.30f));
                p.lineTo(at(0.78f, 0.90f));
                p.lineTo(at(0.24f, 0.90f));
                p.closeSubPath();
                p.startNewSubPath(at(0.58f, 0.10f));
                p.lineTo(at(0.58f, 0.30f));
                p.lineTo(at(0.78f, 0.30f));
                g.strokePath(p, juce::PathStrokeType(juce::jmax(1.0f, line * 0.85f), juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
                break;
            }
            case Icon::Clock:
            {
                // A clock face whose rim runs backwards into an arrow: what has just been.
                p.addCentredArc(box.getCentreX(), box.getCentreY(), side * 0.38f, side * 0.38f, 0.0f,
                                juce::MathConstants<float>::pi * -0.35f, juce::MathConstants<float>::pi * 1.45f, true);
                p.startNewSubPath(at(0.5f, 0.28f));
                p.lineTo(at(0.5f, 0.52f));
                p.lineTo(at(0.66f, 0.62f));
                p.startNewSubPath(at(0.04f, 0.26f));
                p.lineTo(at(0.14f, 0.44f));
                p.lineTo(at(0.32f, 0.36f));
                g.strokePath(p, juce::PathStrokeType(juce::jmax(1.0f, line * 0.85f), juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
                break;
            }
            case Icon::Cables:
            case Icon::Clear:
            {
                // A sagging wire between two plugs; Clear draws it broken in the middle.
                const float r = side * 0.13f;
                if (icon == Icon::Cables)
                {
                    p.startNewSubPath(at(0.2f, 0.3f));
                    p.cubicTo(at(0.2f, 0.95f), at(0.8f, 0.95f), at(0.8f, 0.3f));
                }
                else
                {
                    p.startNewSubPath(at(0.2f, 0.3f));
                    p.quadraticTo(at(0.2f, 0.74f), at(0.4f, 0.78f));
                    p.startNewSubPath(at(0.8f, 0.3f));
                    p.quadraticTo(at(0.8f, 0.74f), at(0.6f, 0.78f));
                }
                g.strokePath(p, juce::PathStrokeType(juce::jmax(1.0f, line * 0.85f), juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
                g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(at(0.2f, 0.26f)));
                g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(at(0.8f, 0.26f)));
                break;
            }
            case Icon::Takes:
            {
                // Three takes stacked: lines of different lengths.
                const float ends[3] = { 0.86f, 0.62f, 0.76f };
                for (int i = 0; i < 3; ++i)
                {
                    const float y = 0.26f + 0.24f * static_cast<float>(i);
                    g.fillRect(juce::Rectangle<float>(side * 0.14f, side * 0.14f).withCentre(at(0.16f, y)));
                    p.startNewSubPath(at(0.34f, y));
                    p.lineTo(at(ends[i], y));
                }
                g.strokePath(p, juce::PathStrokeType(juce::jmax(1.0f, line * 0.85f), juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
                break;
            }
            case Icon::Variations:
            {
                // One page, with two more behind it.
                const float w = juce::jmax(1.0f, line * 0.85f);
                g.drawRect(juce::Rectangle<float>(at(0.10f, 0.34f), at(0.62f, 0.90f)).expanded(w * 0.5f), w);
                p.startNewSubPath(at(0.26f, 0.22f));
                p.lineTo(at(0.76f, 0.22f));
                p.lineTo(at(0.76f, 0.76f));
                p.startNewSubPath(at(0.42f, 0.10f));
                p.lineTo(at(0.90f, 0.10f));
                p.lineTo(at(0.90f, 0.62f));
                g.strokePath(p, juce::PathStrokeType(w, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
                break;
            }
            case Icon::Link:
            {
                // Two links of a chain, one through the other (square links: forged, not bent).
                const float w = juce::jmax(1.0f, line * 0.85f);
                juce::Path a, b;
                a.addRectangle(juce::Rectangle<float>(at(0.08f, 0.34f), at(0.58f, 0.66f)));
                b.addRectangle(juce::Rectangle<float>(at(0.42f, 0.34f), at(0.92f, 0.66f)));
                g.strokePath(a, juce::PathStrokeType(w));
                g.strokePath(b, juce::PathStrokeType(w));
                break;
            }
            case Icon::Tracks:
            {
                // A waveform as bars.
                const float heights[5] = { 0.24f, 0.56f, 0.84f, 0.44f, 0.28f };
                for (int i = 0; i < 5; ++i)
                {
                    const float x = 0.18f + 0.16f * static_cast<float>(i);
                    p.startNewSubPath(at(x, 0.5f - heights[i] * 0.5f));
                    p.lineTo(at(x, 0.5f + heights[i] * 0.5f));
                }
                g.strokePath(p, stroke);
                break;
            }
            case Icon::Ride:
            case Icon::Modulation:
            {
                // A swell (Ride: one slow wave) or an oscillation (Modulation: two quick ones).
                const int waves = icon == Icon::Ride ? 1 : 2;
                for (int i = 0; i <= 24; ++i)
                {
                    const float t = static_cast<float>(i) / 24.0f;
                    const auto pt = at(0.14f + 0.72f * t, 0.5f - 0.24f * std::sin(t * juce::MathConstants<float>::twoPi * static_cast<float>(waves)));
                    if (i == 0) p.startNewSubPath(pt); else p.lineTo(pt);
                }
                g.strokePath(p, stroke);
                break;
            }
            case Icon::Hits:
            {
                // A burst: a hit, not a letter (the Hits are not a third bus).
                p.addStar(box.getCentre(), 8, side * 0.18f, side * 0.44f, 0.2f);
                g.fillPath(p);
                break;
            }
            case Icon::Output:
            {
                // A speaker with two waves leaving it.
                p.startNewSubPath(at(0.14f, 0.38f));
                p.lineTo(at(0.30f, 0.38f));
                p.lineTo(at(0.48f, 0.20f));
                p.lineTo(at(0.48f, 0.80f));
                p.lineTo(at(0.30f, 0.62f));
                p.lineTo(at(0.14f, 0.62f));
                p.closeSubPath();
                g.fillPath(p);
                juce::Path waves;
                waves.addCentredArc(at(0.48f, 0.5f).x, box.getCentreY(), side * 0.20f, side * 0.20f, 0.0f, 0.6f, 2.54f, true);
                waves.addCentredArc(at(0.48f, 0.5f).x, box.getCentreY(), side * 0.36f, side * 0.36f, 0.0f, 0.6f, 2.54f, true);
                g.strokePath(waves, juce::PathStrokeType(juce::jmax(1.0f, line * 0.85f), juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
                break;
            }
            case Icon::LetterA:
            case Icon::LetterB:
            case Icon::LetterS:
                break; // letters are set in type by whoever draws the nameplate
        }
    }
}
