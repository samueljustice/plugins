#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

/**
 * The design tokens: every colour, radius, gap and text size the interface uses is named here, once.
 *
 * Surfaces sit on each other by tone and a soft shadow, never by an outline: the window is the deepest water,
 * panels float on it, raised things (buttons, lanes, menus) float on panels, and wells (scopes, text boxes,
 * meters) are cut into them. A stroke always means a state: hovered, switched on, focused.
 *
 * One chrome accent (brass) for everything that is not a section. The sections keep their own colour from the
 * knobs to the track tags to the comets: coral for Bus A, turquoise for Bus B, cherry for the Hits, driftwood
 * for the Output. The four modulators have a family of their own that is none of those.
 */
namespace Theme
{
    namespace Colours
    {
        // Surfaces, deepest first.
        inline const juce::Colour window      { 0xff081820 }; // deep water
        inline const juce::Colour panel       { 0xff102a35 };
        inline const juce::Colour raised      { 0xff1a3b48 }; // buttons, menus, lanes
        inline const juce::Colour well        { 0xff06131a }; // scopes, text boxes, meters

        // Text. Each reaches 4.5:1 on panel and on raised (UiTest checks it).
        inline const juce::Colour text        { 0xfff2e8d2 }; // sand: names and values
        inline const juce::Colour textDim     { 0xffa9c2c7 }; // secondary: captions
        inline const juce::Colour textFaint   { 0xff86a6ae }; // faint: hints, scales, units

        // The chrome accent.
        inline const juce::Colour brass       { 0xffe6b455 };
        inline const juce::Colour brassDark   { 0xff7d5a26 };

        // The sections.
        inline const juce::Colour busA        { 0xffff8466 }; // coral
        inline const juce::Colour busB        { 0xff3ccbc2 }; // turquoise
        inline const juce::Colour hits        { 0xffff4f6a }; // cherry
        inline const juce::Colour output      { 0xffd3c9b4 }; // driftwood
        inline const juce::Colour space       { 0xff8fd9b0 }; // sea glass (the Space knob)
        inline const juce::Colour go          { 0xff4fd68a }; // switched on and running (the morph)
        inline const juce::Colour filter      { 0xff86b4ff }; // sky blue (the morph pad)

        // States. Strokes are only ever one of these.
        inline const juce::Colour stroke      { 0xff3a6a7c }; // hovered, or the edge of a well that takes typing
        inline const juce::Colour hover       { 0xff224857 }; // a raised thing under the mouse
        inline const juce::Colour selection   { 0x333ccbc2 };
        inline const juce::Colour shadow      { 0xff02090d };
        inline const juce::Colour grid        { 0xff173a46 }; // lines inside a well

        // The older names, kept where the meaning is the same thing.
        inline const juce::Colour background  = window;
        inline const juce::Colour panelRaised = raised;
        inline const juce::Colour scope       = well;
        inline const juce::Colour outline     = stroke;
        inline const juce::Colour outlineSoft { 0xff1c4250 }; // a quiet rule inside a panel
        inline const juce::Colour input       = busB;   // LogoOutline
        inline const juce::Colour harmonics   = busA;   // LogoOutline
        inline const juce::Colour sources     = busB;
        inline const juce::Colour movement    = busA;
        inline const juce::Colour record      = hits;
        inline const juce::Colour master      = output;
        inline const juce::Colour flight      = brass;
        inline const juce::Colour accent      = brass;
        inline const juce::Colour good        = space;
    }

    /** The four modulators: lilac, sky, rose and lime. None is a section's colour or the brass. */
    inline juce::Colour modColour(int source)
    {
        static const juce::uint32 colours[4] = { 0xffb9a2ff, 0xff6fb7ff, 0xfff48fc6, 0xffb7e36b };
        return juce::Colour(colours[((source % 4) + 4) % 4]);
    }

    /** Is a component there to be seen in its window: itself and everything it sits in visible. Unlike
        Component::isShowing it does not ask for the window to be on a screen, so the interface lays its wires and
        ropes out the same way when it is drawn offline (tools/UiShot.cpp, Tests/UiTest.cpp). */
    inline bool isShownInWindow(const juce::Component& component)
    {
        for (const auto* c = &component; c != nullptr; c = c->getParentComponent())
            if (! c->isVisible())
                return false;
        return true;
    }

    /** Corner radii. */
    namespace Radius
    {
        constexpr float panel = 13.0f;   // the big surfaces, the sea, popup menus
        constexpr float control = 7.0f;  // buttons, menus, lanes, wells
        constexpr float small = 4.0f;    // the little things inside a control
    }

    /** Spacing: everything is a multiple of the 4 px unit; panels are a gutter apart. */
    namespace Space
    {
        constexpr int unit = 4;
        constexpr int gutter = 12;
        constexpr int panelPadding = 16;
        constexpr int titleBar = 40;     // a panel's title row
    }

    /** How a control that is not simply "on" or "off" reads. Three different looks, never just a fade:
        Disabled cannot be used now; Resting can be set but does nothing until something else is switched on;
        Linked follows another control and shows whose value it is using. */
    enum class Look { Normal = 0, Disabled, Resting, Linked };
    inline const juce::Identifier lookProperty { "look" };
    inline void setLook(juce::Component& c, Look look)
    {
        if (static_cast<int>(c.getProperties().getWithDefault(lookProperty, 0)) == static_cast<int>(look))
            return;
        c.getProperties().set(lookProperty, static_cast<int>(look));
        c.setEnabled(look != Look::Disabled && look != Look::Linked);
        c.repaint();
    }
    /** The look a control is drawn in: the one it was given, or Disabled when it is simply switched off. */
    inline Look getLook(const juce::Component& c)
    {
        const auto look = static_cast<Look>(static_cast<int>(c.getProperties().getWithDefault(lookProperty, 0)));
        return look == Look::Normal && ! c.isEnabled() ? Look::Disabled : look;
    }

    /** Text the user reads names Mac shortcuts ("Cmd+Z"); on Windows and Linux that key is Ctrl. */
    inline juce::String localiseShortcuts(const juce::String& text)
    {
       #if JUCE_MAC
        return text;
       #else
        return text.replace("Shift+Cmd+", "Ctrl+Shift+").replace("Cmd+", "Ctrl+").replace("Finder / Explorer", "Explorer");
       #endif
    }

    /** A long name cut in the middle to at most maxChars characters, keeping its start and its end (where file
        names carry their take numbers): "ANMLFarm_Pig...al_Close_01". Short names come back as they are. */
    inline juce::String middleTruncated(const juce::String& text, int maxChars)
    {
        if (text.length() <= maxChars)
            return text;
        const int keep = maxChars - 3; // the three dots
        if (keep < 2)
            return {};
        const int head = (keep + 1) / 2, tail = keep - head;
        return text.substring(0, head).trimEnd() + "..." + text.substring(text.length() - tail).trimStart();
    }

    /** The name, cut in the middle as far as it takes to fit maxWidth, as `measure` says how wide a text is: whole
        when it fits (however short), else the longest cut of six characters or more that does, else empty. A handful
        of measurements, not one per character: it is called while painting. */
    template <typename Measure>
    juce::String fitMiddleBy(const juce::String& text, Measure&& measure, float maxWidth)
    {
        if (text.isEmpty() || maxWidth <= 0.0f)
            return {};
        if (measure(text) <= maxWidth)
            return text;
        int lo = 6, hi = text.length() - 1; // the shortest cut worth showing, and the longest there is
        juce::String best;
        while (lo <= hi)
        {
            const int mid = (lo + hi) / 2;
            const auto cut = middleTruncated(text, mid);
            if (measure(cut) <= maxWidth)
            {
                best = cut;
                lo = mid + 1;
            }
            else
                hi = mid - 1;
        }
        return best;
    }

    /** fitMiddleBy, measured in a font. */
    inline juce::String fitMiddle(const juce::String& text, const juce::Font& font, float maxWidth)
    {
        return fitMiddleBy(text, [&font](const juce::String& t) { return juce::GlyphArrangement::getStringWidth(font, t); }, maxWidth);
    }

    /** Colours for the tracks: lagoon, coral, gold, sea glass, sky, hibiscus, harbour, rope, lilac, palm. */
    inline juce::Colour trackColour(int lane)
    {
        static const juce::uint32 colours[10] = { 0xff3ccbc2, 0xffff8466, 0xfff2b84b, 0xff8fd9b0, 0xff86b4ff,
                                                  0xffe889b8, 0xff5fa8d3, 0xffd9a35f, 0xffb59cf2, 0xff7fd1a0 };
        return juce::Colour(colours[((lane % 10) + 10) % 10]);
    }

    /**
     * Typefaces, loaded once and shared by every editor instance (SharedResourcePointer), and the type scale.
     * A text's size comes from its role, never from the height of the box it sits in:
     *
     *   caption   10.5  Plex Medium, lightly tracked   knob captions, chips, units, scales, small tags
     *   body      12    Plex Regular                   values, hints, subtitles, tooltips, messages
     *   control   12    Plex Medium                    buttons, menus and their items, text in a control
     *   title     14    Plex SemiBold                  names (a track, a take), headings inside a panel
     *   display   21    Pirata One                     panel titles
     *   wordmark  36    Pirata One                     the name of the thing
     *
     * Everything is written in Title Case (control names) or sentence case (sentences); nothing in capitals.
     * art() is for lettering that is part of a drawing and grows with it (the word on the Swell button).
     */
    struct Fonts
    {
        static constexpr float c_caption = 10.5f, c_body = 12.0f, c_title = 14.0f, c_display = 21.0f, c_wordmark = 36.0f;

        Fonts()
            : pirata  (juce::Typeface::createSystemTypefaceFor(BinaryData::PirataOneRegular_ttf, BinaryData::PirataOneRegular_ttfSize))
            , regular (juce::Typeface::createSystemTypefaceFor(BinaryData::IBMPlexSansCondensedRegular_ttf, BinaryData::IBMPlexSansCondensedRegular_ttfSize))
            , medium  (juce::Typeface::createSystemTypefaceFor(BinaryData::IBMPlexSansCondensedMedium_ttf, BinaryData::IBMPlexSansCondensedMedium_ttfSize))
            , bold    (juce::Typeface::createSystemTypefaceFor(BinaryData::IBMPlexSansCondensedSemiBold_ttf, BinaryData::IBMPlexSansCondensedSemiBold_ttfSize))
        {
        }

        juce::Font caption() const  { return make(medium, c_caption * c_em, 0.03f); }
        juce::Font body() const     { return make(regular, c_body * c_em); }
        juce::Font control() const  { return make(medium, c_body * c_em, 0.02f); }
        juce::Font title() const    { return make(bold, c_title * c_em, 0.02f); }
        juce::Font display() const  { return make(pirata, c_display, 0.02f); }
        juce::Font wordmark() const { return make(pirata, c_wordmark, 0.02f); }
        juce::Font art(float height) const { return make(pirata, height, 0.02f); }

        /** A size in the scale's units is this much taller as a font height (Plex has tall line gaps). */
        static constexpr float c_em = 1.1f;

        juce::Typeface::Ptr pirata, regular, medium, bold;

    private:
        juce::Font make(const juce::Typeface::Ptr& face, float height, float kerning = 0.0f) const
        {
            if (face == nullptr)
                return juce::Font(juce::FontOptions(height));
            return juce::Font(juce::FontOptions(face).withHeight(height)).withExtraKerningFactor(kerning);
        }
    };

    //==============================================================================
    // Fitted text. Every text drawn into a box goes through fitText (one line) or fitLines (a paragraph), so it
    // is measured first: it is drawn as it is when it fits, set smaller (never under the floor) when it does not,
    // and only then cut with dots. A label or a hint that had to be cut is a fault in the layout, and the checker
    // (tools/UiProbe.h, run by UiTest and UiShot --list) reports it; a name the user chose (a file, a preset, a
    // track) may be cut, in the middle so its start and its take number both stay, and so may a status message.

    /** The smallest text there is: 10 in the scale's units. */
    constexpr float c_minTextSize = 10.0f;
    constexpr float c_minFontHeight = c_minTextSize * Fonts::c_em;

    /** What a text is, which says both how it may be fitted and how it is written. */
    enum class Fit
    {
        Whole,     // a control's name, a caption, a value: must fit whole; written in Title Case
        Sentence,  // a subtitle or a hint of ours: must fit whole; written in sentence case
        Message,   // a status line of any length (it may quote a name): may be cut at its end; sentence case
        Name       // a name the user chose (a file, a preset, a track): may be cut in its middle; written as they wrote it
    };
    inline bool mayBeCut(Fit fit) { return fit == Fit::Message || fit == Fit::Name; }

    /** Told about every text as it is drawn (the checker). Not set in the app. */
    struct TextProbe
    {
        virtual ~TextProbe() = default;
        /** box: where the text had to fit; ink: where it was drawn; both in the coordinates of the graphics
            context it was drawn into. lines > 1 for a paragraph. */
        virtual void textDrawn(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> box, juce::Rectangle<float> ink,
                               float fontHeight, Fit fit, bool shrunk, bool cut) = 0;
    };
    inline TextProbe*& textProbe()
    {
        static TextProbe* probe = nullptr;
        return probe;
    }

    /** The text cut at its end with three dots to fit maxWidth (empty when not even a few characters do). */
    inline juce::String fitEnd(const juce::String& text, const juce::Font& font, float maxWidth)
    {
        if (juce::GlyphArrangement::getStringWidth(font, text) <= maxWidth)
            return text;
        int lo = 1, hi = text.length() - 1;
        juce::String best;
        while (lo <= hi)
        {
            const int mid = (lo + hi) / 2;
            const auto cut = text.substring(0, mid).trimEnd() + "...";
            if (juce::GlyphArrangement::getStringWidth(font, cut) <= maxWidth)
            {
                best = cut;
                lo = mid + 1;
            }
            else
                hi = mid - 1;
        }
        return best;
    }

    /** What fitText works out: what to draw, in which font, and how wide it comes out. */
    struct Fitted
    {
        juce::String text;
        juce::Font font { juce::FontOptions() };
        float width = 0.0f;
        bool shrunk = false, cut = false;
    };

    /** Fits one line of text to a width: as it is, else smaller (down to the floor), else cut. */
    inline Fitted fitToWidth(const juce::String& text, const juce::Font& font, float maxWidth, Fit fit = Fit::Whole)
    {
        Fitted result { text, font, juce::GlyphArrangement::getStringWidth(font, text), false, false };
        if (result.width <= maxWidth || text.isEmpty() || maxWidth <= 0.0f)
            return result;
        result.shrunk = true;
        result.font = font.withHeight(juce::jmax(c_minFontHeight, juce::jmin(font.getHeight(), font.getHeight() * maxWidth / result.width * 0.985f)));
        result.width = juce::GlyphArrangement::getStringWidth(result.font, text);
        if (result.width <= maxWidth)
            return result;
        result.cut = true;
        result.text = fit == Fit::Name ? fitMiddle(text, result.font, maxWidth) : juce::String();
        if (result.text.isEmpty())
            result.text = fitEnd(text, result.font, maxWidth);
        result.width = juce::GlyphArrangement::getStringWidth(result.font, result.text);
        return result;
    }

    /** Draws one line of text inside box in the current colour, fitted (see above). Returns the width drawn. */
    inline float fitText(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> box, const juce::Font& font,
                         juce::Justification justification, Fit fit = Fit::Whole)
    {
        if (text.isEmpty() || box.getWidth() <= 0.0f || box.getHeight() <= 0.0f)
            return 0.0f;
        const auto fitted = fitToWidth(text, font, box.getWidth(), fit);
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText(fitted.font, fitted.text, 0.0f, 0.0f);
        glyphs.justifyGlyphs(0, glyphs.getNumGlyphs(), box.getX(), box.getY(), box.getWidth(), box.getHeight(), justification);
        glyphs.draw(g);
        if (auto* probe = textProbe())
            probe->textDrawn(g, text, box, glyphs.getBoundingBox(0, -1, true), fitted.font.getHeight(), fit, fitted.shrunk, fitted.cut);
        return fitted.width;
    }
    inline float fitText(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> box, const juce::Font& font,
                         juce::Justification justification, Fit fit = Fit::Whole)
    {
        return fitText(g, text, box.toFloat(), font, justification, fit);
    }

    /** Draws a paragraph inside box, wrapped by word, in the given colour. One that needs more room than the box
        has is set smaller (down to the floor); the checker reports one that still runs past the bottom. */
    inline void fitLines(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> box, const juce::Font& font,
                         juce::Colour colour, juce::Justification justification = juce::Justification::topLeft)
    {
        if (text.isEmpty() || box.getWidth() <= 0.0f || box.getHeight() <= 0.0f)
            return;
        juce::Font used = font;
        juce::TextLayout layout;
        bool shrunk = false;
        for (;;)
        {
            juce::AttributedString s;
            s.setJustification(justification);
            s.setWordWrap(juce::AttributedString::byWord);
            s.append(text, used, colour);
            layout.createLayout(s, box.getWidth());
            if (layout.getHeight() <= box.getHeight() + 0.5f || used.getHeight() <= c_minFontHeight + 0.01f)
                break;
            used = used.withHeight(juce::jmax(c_minFontHeight, used.getHeight() - 0.5f));
            shrunk = true;
        }
        layout.draw(g, box);
        if (auto* probe = textProbe())
            probe->textDrawn(g, text, box, box.withHeight(layout.getHeight()), used.getHeight(), Fit::Sentence, shrunk,
                             layout.getHeight() > box.getHeight() + 0.5f);
    }
    inline void fitLines(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> box, const juce::Font& font,
                         juce::Colour colour, juce::Justification justification = juce::Justification::topLeft)
    {
        fitLines(g, text, box.toFloat(), font, colour, justification);
    }
}
