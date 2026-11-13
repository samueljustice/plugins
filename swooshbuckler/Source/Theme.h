#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

#include <unordered_map>

/**
 * The design tokens: every colour, corner cut, gap and text size the interface uses is named here, once.
 *
 * THE VOCABULARY. The window is ship's ironwork with brass: flat plates with things stamped into them. Four
 * things, and nothing is drawn that is not one of them (the round things are the ones that are round on a ship:
 * the wheel, bolts, lamps, jacks and the moon).
 *
 *   1. Panel plate   A big plate bolted to the window (Theme::Colours::panel). Square corners, a pressed edge -
 *                    a dark line with a faint light line inside it - and a slotted bolt in each corner.
 *                    SwooshLookAndFeel::drawPanel; drawSurface is the same plate without the bolts.
 *   2. Control plate A button, menu, chip or tag (Colours::raised), square corners, its label stamped into it
 *                    (Theme::stampText). Its states:
 *                      idle      iron plate, dark edge, a light line along its top, a dark lip under it
 *                      hover     lighter plate, the edge in Colours::stroke
 *                      pressed   the plate sits down on its lip
 *                      on        the whole plate is brass (or its section's colour), its label dark
 *                      disabled  flat and faint: no edge, no lip
 *                      resting   hollow, with a dashed edge: it can be set, and waits for something else
 *                      linked    as disabled, with a chain link: it follows another control
 *                    SwooshLookAndFeel::drawControlPlate (every button style, menus, the tape button).
 *   3. Well          A window cut into a plate (Colours::well): dark, with a shadow line under its top edge and a
 *                    light line along its bottom. Scopes, waveforms, meters, text boxes, slots a fader or a switch
 *                    slides in. SwooshLookAndFeel::drawWell.
 *   4. Nameplate     A panel's title: a small dark plate riveted to the panel head, a tally in the section's colour
 *                    at its left with the section's mark or letter, the name in the blackletter face.
 *
 * HARD EDGES ONLY. No pills and no round corners, not even a small radius: every plate, well, chip, cap and frame
 * has sharp 90 degree corners (Theme::Cut is zero everywhere; a small cut reads as a round corner at this size, so
 * the only 45 degree cuts are big ones that read as ironwork: the gussets of the sea's frame). Lines around plates
 * are mitered with butt ends. The only round things are the ones that are round on a ship (above), ropes and
 * cables, and the dots on ride lines and cursors. UiTest fails if a corner token is not zero, and if a rounded
 * rectangle or a round-ended stroke appears in Source/ without the words "round object:" beside it. Stamping
 * is one or two thin tonal lines, never a gradient or a gloss, and never at the cost of reading the label: text
 * keeps its full colour and 4.5:1 on its plate (UiTest checks it).
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

        // The ironwork: what a plate's pressed edge and its fasteners are drawn in.
        inline const juce::Colour edgeDark    { 0xff040e13 }; // the pressed line around a plate, the lip under a key
        inline const juce::Colour edgeLight   { 0x24f2e8d2 }; // the faint light line beside it (sand, mostly clear)
        inline const juce::Colour bolt        { 0xff7e99a1 }; // gunmetal: bolt heads and rivets
        inline const juce::Colour boltSlot    { 0xff0b1c24 }; // the slot in a bolt's head

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

    /** Corners: how much of a plate's corner is taken off. Nothing: every corner is a hard 90 degrees (a cut of a
        few pixels reads as a round corner). The names stay so that one place decides; UiTest holds them at zero. */
    namespace Cut
    {
        constexpr float panel = 0.0f;    // the panel plates, the overlays, popup menus
        constexpr float control = 0.0f;  // buttons, menus, tags, lanes, cards, wells
        constexpr float small = 0.0f;    // chips, text boxes, the little things inside a control
    }

    /** Plates: the hard-edged shape everything is made of (a rectangle; a `cut` takes its corners off straight),
        its pressed edge and its fasteners. Message thread only (the shape is built in one kept path, so drawing a
        plate allocates nothing after the first). */
    namespace Plate
    {
        constexpr float bolt = 6.5f;       // a bolt head's diameter: big enough to read as a bolt, slot and all
        constexpr float boltInset = 8.0f;  // from a plate's corner to its bolt's centre
        constexpr float rivet = 3.0f;      // the small fastener on a nameplate

        /** The rectangle with its four corners cut by `cut`. The reference is good until the next call. */
        inline const juce::Path& shape(juce::Rectangle<float> r, float cut)
        {
            static juce::Path p;
            p.clear();
            const float c = juce::jmax(0.0f, juce::jmin(cut, r.getWidth() * 0.5f, r.getHeight() * 0.5f));
            if (c < 0.05f)
            {
                p.addRectangle(r);
                return p;
            }
            const float x0 = r.getX(), y0 = r.getY(), x1 = r.getRight(), y1 = r.getBottom();
            p.startNewSubPath(x0 + c, y0);
            p.lineTo(x1 - c, y0);
            p.lineTo(x1, y0 + c);
            p.lineTo(x1, y1 - c);
            p.lineTo(x1 - c, y1);
            p.lineTo(x0 + c, y1);
            p.lineTo(x0, y1 - c);
            p.lineTo(x0, y0 + c);
            p.closeSubPath();
            return p;
        }

        /** Fills the plate in the current colour. */
        inline void fill(juce::Graphics& g, juce::Rectangle<float> r, float cut)
        {
            if (cut < 0.05f)
                g.fillRect(r);
            else
                g.fillPath(shape(r, cut));
        }

        /** A line around the plate, inside r, in the current colour: mitered, with butt ends. */
        inline void stroke(juce::Graphics& g, juce::Rectangle<float> r, float cut, float thickness = 1.0f)
        {
            g.strokePath(shape(r.reduced(thickness * 0.5f), juce::jmax(0.0f, cut - thickness * 0.3f)),
                         juce::PathStrokeType(thickness, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
        }

        /** The pressed edge of a plate: a dark line around it and a faint light line just inside that. */
        inline void edge(juce::Graphics& g, juce::Rectangle<float> r, float cut, juce::Colour dark = Colours::edgeDark,
                         juce::Colour light = Colours::edgeLight)
        {
            g.setColour(light);
            stroke(g, r.reduced(1.0f), cut - 0.4f);
            g.setColour(dark);
            stroke(g, r, cut);
        }

        /** The edge of a window cut into a plate: a dark line around it, a shadow under its top edge and a light
            line along its bottom. */
        inline void wellEdge(juce::Graphics& g, juce::Rectangle<float> r, float cut)
        {
            g.setColour(Colours::shadow.withAlpha(0.55f));
            g.fillRect(juce::Rectangle<float>(r.getX() + cut + 1.0f, r.getY() + 1.0f, r.getWidth() - 2.0f * cut - 2.0f, 1.0f));
            g.setColour(Colours::edgeLight);
            g.fillRect(juce::Rectangle<float>(r.getX() + cut, r.getBottom() - 1.0f, r.getWidth() - 2.0f * cut, 1.0f));
            g.setColour(Colours::edgeDark);
            stroke(g, r.withTrimmedBottom(1.0f), cut);
        }

        /** A slotted bolt head: a dark seat, a gunmetal head lit along its top, and the slot, turned a little
            differently on every bolt (from where it is, so it never moves). */
        inline void drawBolt(juce::Graphics& g, juce::Point<float> centre, float diameter = bolt)
        {
            const auto head = juce::Rectangle<float>(diameter, diameter).withCentre(centre);
            g.setColour(Colours::edgeDark);
            g.fillEllipse(head.expanded(1.0f));
            g.setColour(Colours::bolt.darker(0.35f));
            g.fillEllipse(head);
            g.setColour(Colours::bolt);
            g.fillEllipse(head.reduced(0.6f).translated(0.0f, -0.5f));
            const float turn = 0.55f + 0.45f * std::sin(centre.x * 0.37f + centre.y * 0.91f);
            const juce::Point<float> dir { std::cos(turn), std::sin(turn) };
            g.setColour(Colours::boltSlot);
            g.drawLine(juce::Line<float>(centre - dir * (diameter * 0.42f), centre + dir * (diameter * 0.42f)), juce::jmax(1.0f, diameter * 0.2f));
        }

        /** A rivet: the small round fastener on a nameplate. */
        inline void drawRivet(juce::Graphics& g, juce::Point<float> centre, float diameter = rivet)
        {
            const auto head = juce::Rectangle<float>(diameter, diameter).withCentre(centre);
            g.setColour(Colours::edgeDark);
            g.fillEllipse(head.expanded(0.8f));
            g.setColour(Colours::bolt);
            g.fillEllipse(head);
        }
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

    //==============================================================================
    // Measured text. Laying a line of text out (shaping it) is by far the dearest part of drawing it, and the
    // interface draws the same few hundred labels over and over, many of them thirty times a second. So a text
    // is laid out once for each font it is drawn in and kept: measuring it and drawing it again cost a lookup.
    // Readouts that change (a time, a level) are laid out once per value. Message thread only, like all painting.

    /** A line of text laid out from the origin (the left end of its baseline). */
    struct ShapedText
    {
        juce::GlyphArrangement glyphs;
        juce::Rectangle<float> bounds, ink; // with and without the white space at its ends
    };

    namespace detail
    {
        struct TextCache final : public juce::DeletedAtShutdown
        {
            struct Key
            {
                juce::Font font;
                juce::String text;
                bool operator==(const Key& other) const noexcept { return text == other.text && font == other.font; }
            };
            struct Hash
            {
                size_t operator()(const Key& k) const noexcept
                {
                    return static_cast<size_t>(k.text.hashCode64()) ^ (std::hash<float>()(k.font.getHeight()) * 31u);
                }
            };
            static constexpr size_t c_most = 1024; // more than the window ever shows at once; emptied when full

            ~TextCache() override { slot() = nullptr; }
            static TextCache*& slot()
            {
                static TextCache* cache = nullptr;
                return cache;
            }
            std::unordered_map<Key, ShapedText, Hash> map;
        };
    }

    /** The text laid out in the font, from the cache. The reference is good until the next call. */
    inline const ShapedText& shaped(const juce::Font& font, const juce::String& text)
    {
        auto*& cache = detail::TextCache::slot();
        if (cache == nullptr)
            cache = new detail::TextCache(); // deleted with JUCE (DeletedAtShutdown), so it holds no font past it
        detail::TextCache::Key key { font, text };
        if (const auto found = cache->map.find(key); found != cache->map.end())
            return found->second;
        if (cache->map.size() >= detail::TextCache::c_most)
            cache->map.clear();
        auto& made = cache->map[std::move(key)];
        made.glyphs.addLineOfText(font, text, 0.0f, 0.0f);
        made.bounds = made.glyphs.getBoundingBox(0, -1, true);
        made.ink = made.glyphs.getBoundingBox(0, -1, false);
        return made;
    }

    /** How wide a line of text is in a font (what juce::GlyphArrangement::getStringWidth says, kept). */
    inline float textWidth(const juce::Font& font, const juce::String& text)
    {
        return text.isEmpty() ? 0.0f : shaped(font, text).bounds.getWidth();
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
        return fitMiddleBy(text, [&font](const juce::String& t) { return textWidth(font, t); }, maxWidth);
    }

    /** Colours for the tracks: lagoon, coral, gold, sea glass, sky, hibiscus, harbour, rope, lilac, palm. */
    inline juce::Colour trackColour(int lane)
    {
        static const juce::uint32 colours[10] = { 0xff3ccbc2, 0xffff8466, 0xfff2b84b, 0xff8fd9b0, 0xff86b4ff,
                                                  0xffe889b8, 0xff5fa8d3, 0xffd9a35f, 0xffb59cf2, 0xff7fd1a0 };
        return juce::Colour(colours[((lane % 10) + 10) % 10]);
    }

    /** A lane's colour: a shade of its bus's colour. Lanes on the same bus step through lightness (the first is
        the bus colour itself, then lighter, a touch darker, lighter still...), so two of them are told apart and both still
        read as their bus. Every shade keeps 4.5:1 under dark lettering (UiTest checks it). */
    inline juce::Colour laneShade(juce::Colour bus, int step)
    {
        static const float mix[5] = { 0.0f, 0.42f, -0.12f, 0.66f, 0.21f }; // towards sand (+) or towards the deep (-)
        const float m = mix[((step % 5) + 5) % 5];
        return m >= 0.0f ? bus.interpolatedWith(Colours::text, m) : bus.interpolatedWith(Colours::panel, -m);
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
        if (textWidth(font, text) <= maxWidth)
            return text;
        int lo = 1, hi = text.length() - 1;
        juce::String best;
        while (lo <= hi)
        {
            const int mid = (lo + hi) / 2;
            const auto cut = text.substring(0, mid).trimEnd() + "...";
            if (textWidth(font, cut) <= maxWidth)
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
        Fitted result { text, font, textWidth(font, text), false, false };
        if (result.width <= maxWidth || text.isEmpty() || maxWidth <= 0.0f)
            return result;
        result.shrunk = true;
        result.font = font.withHeight(juce::jmax(c_minFontHeight, juce::jmin(font.getHeight(), font.getHeight() * maxWidth / result.width * 0.985f)));
        result.width = textWidth(result.font, text);
        if (result.width <= maxWidth)
            return result;
        result.cut = true;
        result.text = fit == Fit::Name ? fitMiddle(text, result.font, maxWidth) : juce::String();
        if (result.text.isEmpty())
            result.text = fitEnd(text, result.font, maxWidth);
        result.width = textWidth(result.font, result.text);
        return result;
    }

    namespace detail
    {
        /** fitText and stampText: `stamp` (when it is not clear) is drawn first, the same text moved by stampDy. */
        inline float drawFitted(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> box, const juce::Font& font,
                                juce::Justification justification, Fit fit, const juce::Colour* colour, juce::Colour stamp, float stampDy)
        {
            if (text.isEmpty() || box.getWidth() <= 0.0f || box.getHeight() <= 0.0f)
                return 0.0f;
            const auto fitted = fitToWidth(text, font, box.getWidth(), fit);
            if (fitted.text.isEmpty())
                return 0.0f;
            // Placed in the box as juce::GlyphArrangement::justifyGlyphs places it (centred text is centred by its
            // ink, the rest by its whole width), by moving the kept layout rather than laying it out again.
            const auto& shape = shaped(fitted.font, fitted.text);
            const auto& bb = justification.testFlags(juce::Justification::horizontallyCentred) ? shape.ink : shape.bounds;
            float dx = box.getX(), dy = box.getY();
            if (justification.testFlags(juce::Justification::horizontallyCentred))  dx += (box.getWidth() - bb.getWidth()) * 0.5f - bb.getX();
            else if (justification.testFlags(juce::Justification::right))           dx += box.getWidth() - bb.getRight();
            else                                                                    dx -= bb.getX();
            if (justification.testFlags(juce::Justification::top))                  dy -= bb.getY();
            else if (justification.testFlags(juce::Justification::bottom))          dy += box.getHeight() - bb.getBottom();
            else                                                                    dy += (box.getHeight() - bb.getHeight()) * 0.5f - bb.getY();
            if (colour != nullptr)
            {
                if (! stamp.isTransparent())
                {
                    g.setColour(stamp);
                    shape.glyphs.draw(g, juce::AffineTransform::translation(dx, dy + stampDy));
                }
                g.setColour(*colour);
            }
            shape.glyphs.draw(g, juce::AffineTransform::translation(dx, dy));
            if (auto* probe = textProbe())
                probe->textDrawn(g, text, box, shape.bounds.translated(dx, dy), fitted.font.getHeight(), fit, fitted.shrunk, fitted.cut);
            return fitted.width;
        }
    }

    /** Draws one line of text inside box in the current colour, fitted (see above). Returns the width drawn. */
    inline float fitText(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> box, const juce::Font& font,
                         juce::Justification justification, Fit fit = Fit::Whole)
    {
        return detail::drawFitted(g, text, box, font, justification, fit, nullptr, {}, 0.0f);
    }

    /** A label stamped into its plate: fitted like fitText, in `colour` at full strength, with one thin line of
        tone beside each stroke. Light lettering on a dark plate gets a dark line above it (pressed in); dark
        lettering on a lit plate gets a light line under it. The stamping never dims the lettering itself. */
    inline float stampText(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> box, const juce::Font& font,
                           juce::Justification justification, juce::Colour colour, Fit fit = Fit::Whole)
    {
        const bool darkLettering = colour.getPerceivedBrightness() < 0.4f;
        return detail::drawFitted(g, text, box, font, justification, fit, &colour,
                                  darkLettering ? juce::Colours::white.withAlpha(0.28f * colour.getFloatAlpha())
                                                : Colours::edgeDark.withAlpha(0.75f * colour.getFloatAlpha()),
                                  darkLettering ? 1.0f : -1.0f);
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
