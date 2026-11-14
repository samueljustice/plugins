// UI test: the design tokens, the fitted text helper, the one rule for capitals, and the real editor drawn offline.
//
//   - Tokens: every text colour reaches 4.5:1 on the surfaces it is drawn on; the modulators' colours are none of
//     the sections' colours or the brass; the corners and the type scale are what the design says.
//   - Hard edges: every corner token is zero, plates are filled square to their corner pixels, and no file in
//     Source/ draws a rounded rectangle or a round-ended line except on a round object (marked "round object:").
//   - Fitted text: a text that fits is left alone, one that does not is set smaller but never under the floor, and
//     only then cut; a name is cut in its middle, a label at its end; nothing comes out wider than its box; a text
//     is laid out once and kept, and lands where JUCE's own justification would put it.
//   - Capitals: the rule itself, then every parameter name and every choice of every parameter.
//   - The editor, with the demo sounds playing, at 0.6, 1.0 and 1.5 times its size and in each of its states (the
//     first run, wires and linked buses, Settings, Takes, Render Variations, About, the update card): every text it
//     draws fits its box, is at least the smallest size, sits on no other text and follows the capitals rule; and
//     every tooltip starts with a heading in Title Case and shouts nowhere.
//   - The three looks: a control that cannot be used is Disabled, one that waits for something else is Resting (and
//     still usable), one that follows another is Linked; Peak Tremolo is a switch that moves its parameter.
//   - The knobs: every one is one of two wheels (its ring's radius is its size's, whatever box it is given), whole
//     in its box with the dial over its caption and its value; the rows of knobs are on two column pitches; the
//     caption and the value are always drawn, and the value follows a drag; the wheel is where the value is while
//     it is held, coasts when it is let go and comes back, and swings in on a spring when the value changes on its
//     own; each wire is a ring outside the value ring in its modulator's colour, dashed when its depth is negative;
//     plugs sit on the rim, clear of the text, and their cables come in beside it; the cables of the modulator
//     under the mouse stand out and the others fade; the still part of the wheels is kept in a few images.
//   - Faders and meters: no slider or scrollbar is drawn by JUCE's own look; the meters and the Trim fader have a
//     number under them; Swell is the biggest thing in its panel and Throw sits under it.
//
// Usage: SwooshbucklerUi [scratch folder] (a folder in the temp directory if none is given). Nothing is read from
// or written to the user's settings, and nothing goes on line.

#include "UiRig.h"
#include "UiProbe.h"
#include "Parameters.h"

#include <cmath>
#include <set>
#include <iostream>

namespace
{
    using namespace UiRig;
    namespace TC = Theme::Colours;
    int g_failures = 0;

    void check(bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
        if (! ok)
            ++g_failures;
    }

    // WCAG relative luminance and contrast.
    double luminance(juce::Colour c)
    {
        auto channel = [](juce::uint8 v)
        {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(c.getRed()) + 0.7152 * channel(c.getGreen()) + 0.0722 * channel(c.getBlue());
    }

    double contrast(juce::Colour a, juce::Colour b)
    {
        const double la = luminance(a), lb = luminance(b);
        return (juce::jmax(la, lb) + 0.05) / (juce::jmin(la, lb) + 0.05);
    }

    /** How far apart two colours are to the eye, roughly: 0 the same, 1 black against white. */
    double distance(juce::Colour a, juce::Colour b)
    {
        const double dr = (a.getRed() - b.getRed()) / 255.0, dg = (a.getGreen() - b.getGreen()) / 255.0, db = (a.getBlue() - b.getBlue()) / 255.0;
        return std::sqrt((2.0 * dr * dr + 4.0 * dg * dg + 3.0 * db * db) / 9.0);
    }

    /** Checks one drawing of a component and says what is wrong in it. Returns the number of texts seen. */
    int checkTexts(const juce::String& name, juce::Component& component, float paintScale, float contentScale)
    {
        UiProbe::Recorder recorder;
        recorder.paint(component, paintScale, contentScale);
        const auto faults = UiProbe::check(recorder.texts);
        for (size_t i = 0; i < faults.size() && i < 12; ++i)
            std::cout << "         " << faults[i].what << ": " << UiProbe::describe(faults[i].text) << " - " << faults[i].detail << std::endl;
        check(faults.empty() && ! recorder.texts.empty(),
              name + ": " + juce::String(static_cast<int>(recorder.texts.size())) + " texts, every one fits, is big enough, clear of the others and capitalised by the rule"
                  + (faults.empty() ? juce::String() : " (" + juce::String(static_cast<int>(faults.size())) + " faults)"));
        return static_cast<int>(recorder.texts.size());
    }

    template <typename Kind>
    void collect(juce::Component& c, std::vector<Kind*>& found)
    {
        if (auto* k = dynamic_cast<Kind*>(&c); k != nullptr && Theme::isShownInWindow(c))
            found.push_back(k);
        for (auto* child : c.getChildren())
            collect(*child, found);
    }

    /** A mouse event on a component, as the main mouse would send it. */
    juce::MouseEvent mouseAt(juce::Component& c, juce::Point<float> position, juce::Point<float> down, bool dragged)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                                juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &c, &c, now, down, now, 1, dragged);
    }

    /** How much of a ring around a knob's dial is drawn in a colour, between two positions of its travel. */
    float ringCoverage(const juce::Image& image, float imageScale, juce::Point<float> centre, float radius, float lo, float hi, juce::Colour colour)
    {
        const float start = juce::MathConstants<float>::pi * -0.75f, end = juce::MathConstants<float>::pi * 0.75f;
        int hits = 0;
        const int samples = 80;
        for (int i = 0; i < samples; ++i)
        {
            const float p = lo + (hi - lo) * (0.06f + 0.88f * static_cast<float>(i) / static_cast<float>(samples - 1));
            const float a = start + p * (end - start);
            const int x = juce::roundToInt((centre.x + std::sin(a) * radius) * imageScale - 0.5f), y = juce::roundToInt((centre.y - std::cos(a) * radius) * imageScale - 0.5f);
            if (image.getBounds().contains(x, y) && image.getPixelAt(x, y).getAlpha() > 128 && distance(image.getPixelAt(x, y), colour) < 0.16)
                ++hits;
        }
        return static_cast<float>(hits) / static_cast<float>(samples);
    }

    /** Every tooltip under a component: its heading line in Title Case, and no word in capitals anywhere in it. */
    void collectTooltips(juce::Component& c, juce::StringArray& tips)
    {
        if (auto* client = dynamic_cast<juce::SettableTooltipClient*>(&c))
            if (const auto tip = client->getTooltip(); tip.isNotEmpty())
                tips.addIfNotAlreadyThere(tip);
        for (auto* child : c.getChildren())
            collectTooltips(*child, tips);
    }
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File folder = argc > 1 ? juce::File(argv[1]) : juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SwooshbucklerUiTest");
    Sandbox sandbox(folder, "uitest");

    // Tokens -----------------------------------------------------------------------------------------------------
    std::cout << "Tokens" << std::endl;
    {
        struct Pair { const char* name; juce::Colour text, surface; };
        const Pair pairs[] = {
            { "text on the window", TC::text, TC::window },         { "text on a panel", TC::text, TC::panel },
            { "text on a raised plate", TC::text, TC::raised },     { "text in a well", TC::text, TC::well },
            { "secondary text on a panel", TC::textDim, TC::panel }, { "secondary text on a raised plate", TC::textDim, TC::raised },
            { "faint text on the window", TC::textFaint, TC::window }, { "faint text on a panel", TC::textFaint, TC::panel },
            { "faint text on a raised plate", TC::textFaint, TC::raised }, { "faint text in a well", TC::textFaint, TC::well },
            { "brass on a panel", TC::brass, TC::panel },           { "dark text on brass (Play)", TC::window, TC::brass },
            { "dark text on a coral chip", TC::window, TC::busA },  { "dark text on a turquoise chip", TC::window, TC::busB },
            { "dark text on a cherry chip", TC::window, TC::hits },
        };
        for (const auto& p : pairs)
            check(contrast(p.text, p.surface) >= 4.5, juce::String("contrast: ") + p.name + " is " + juce::String(contrast(p.text, p.surface), 2) + ":1 (4.5 or more)");

        check(luminance(TC::window) < luminance(TC::panel) && luminance(TC::panel) < luminance(TC::raised) && luminance(TC::well) < luminance(TC::panel),
              "surfaces: the window is the deepest, panels float on it, raised plates on them, and wells are cut in (tone alone tells them apart)");
        check(contrast(TC::panel, TC::window) >= 1.15 && contrast(TC::raised, TC::panel) >= 1.15,
              "surfaces: each step of tone is big enough to see without an outline (" + juce::String(contrast(TC::panel, TC::window), 2) + ", "
                  + juce::String(contrast(TC::raised, TC::panel), 2) + ")");

        const juce::Colour sections[] = { TC::busA, TC::busB, TC::hits, TC::stowaway, TC::output, TC::brass, TC::space };
        double nearest = 1.0;
        for (int m = 0; m < 4; ++m)
        {
            for (const auto& s : sections)
                nearest = juce::jmin(nearest, distance(Theme::modColour(m), s));
            for (int other = m + 1; other < 4; ++other)
                nearest = juce::jmin(nearest, distance(Theme::modColour(m), Theme::modColour(other)));
            check(contrast(Theme::modColour(m), TC::raised) >= 4.5, "modulators: colour " + juce::String(m + 1) + " reads on its card");
        }
        check(nearest > 0.14, "modulators: none of the four is close to a section's colour, the brass or each other (nearest " + juce::String(nearest, 2) + ")");
        {
            // Stowaway's plate sits beside the modulators and their cables land on its knobs: its kelp, and every
            // shade a lane of it takes, is well away from all four.
            double plate = 1.0, lanes = 1.0;
            for (int m = 0; m < 4; ++m)
            {
                plate = juce::jmin(plate, distance(TC::stowaway, Theme::modColour(m)));
                for (int step = 0; step < 5; ++step)
                    lanes = juce::jmin(lanes, distance(Theme::laneShade(TC::stowaway, step), Theme::modColour(m)));
            }
            check(plate > 0.2 && lanes > 0.14, "modulators: none of the four is close to Stowaway's kelp (nearest " + juce::String(plate, 2) + ") or to a lane in a shade of it (nearest "
                                                   + juce::String(lanes, 2) + ")");
            check(distance(TC::stowaway, TC::busA) > 0.2 && distance(TC::stowaway, TC::busB) > 0.2 && distance(TC::stowaway, TC::hits) > 0.2 && distance(TC::stowaway, TC::brass) > 0.12,
                  "sections: Stowaway's kelp is none of the buses' colours, the hits' or the brass");
        }
        check(distance(TC::busA, TC::busB) > 0.3 && distance(TC::busA, TC::hits) > 0.1 && distance(TC::busB, TC::hits) > 0.3,
              "sections: coral, turquoise and cherry are three different colours");
        check(TC::flight == TC::brass && TC::accent == TC::brass, "chrome: one accent (the old gold is the brass)");

        check(Theme::Cut::panel == 0.0f && Theme::Cut::control == 0.0f && Theme::Cut::small == 0.0f,
              "corners: hard - every corner token is zero (panels, controls and the small things are square)");
        {
            // Square to the pixel: a panel plate, a control plate (idle, lit, resting, disabled) and a well each
            // fill the four corner pixels of their box.
            const juce::Rectangle<float> box(4.0f, 4.0f, 60.0f, 24.0f);
            auto squareCorners = [&box](std::function<void(juce::Graphics&)> draw)
            {
                juce::Image image(juce::Image::ARGB, 68, 40, true);
                {
                    juce::Graphics g(image);
                    draw(g);
                }
                return image.getPixelAt(4, 4).getAlpha() > 40 && image.getPixelAt(63, 4).getAlpha() > 40
                       && image.getPixelAt(4, 25).getAlpha() > 40 && image.getPixelAt(63, 25).getAlpha() > 40
                       && image.getPixelAt(3, 3).getAlpha() == 0 && image.getPixelAt(64, 3).getAlpha() == 0;
            };
            using L = SwooshLookAndFeel;
            bool square = squareCorners([&](juce::Graphics& g) { L::drawSurface(g, box, Theme::Cut::panel, TC::panel, 0.0f); })
                          && squareCorners([&](juce::Graphics& g) { L::drawWell(g, box); })
                          && squareCorners([&](juce::Graphics& g) { L::drawWell(g, box, Theme::Cut::small); });
            for (const auto look : { Theme::Look::Normal, Theme::Look::Resting, Theme::Look::Disabled })
                for (const bool on : { false, true })
                    square = square && squareCorners([&](juce::Graphics& g) { L::drawControlPlate(g, box, TC::brass, look, on, false, false); });
            check(square, "corners: a panel plate, a well and a control plate in each of its looks fill their box to its four corner pixels");
        }
        {
            // The sources: no rounded rectangle, no corner radius, and no round-ended or round-joined line unless
            // it is on a round object and says so on its line ("round object: the rope").
            const juce::File sources(SWOOSH_SOURCE_DIR);
            int files = 0, faults = 0;
            for (const auto& entry : juce::RangedDirectoryIterator(sources, false, "*.h;*.cpp"))
            {
                ++files;
                juce::StringArray lines;
                entry.getFile().readLines(lines);
                for (int i = 0; i < lines.size(); ++i)
                {
                    const auto& raw = lines[i];
                    const auto code = raw.upToFirstOccurrenceOf("//", false, false);
                    const bool roundedBox = code.contains("Rounded") || code.contains("cornerSize") || code.contains("Radius::")
                                            || code.contains("addBubble") || code.contains("drawBubble");
                    const bool roundedLine = (code.contains("PathStrokeType::curved") || code.contains("PathStrokeType::rounded")) && ! raw.contains("round object:");
                    if (roundedBox || roundedLine)
                    {
                        ++faults;
                        std::cout << "         " << entry.getFile().getFileName() << ":" << (i + 1) << " " << raw.trim().substring(0, 110) << std::endl;
                    }
                }
            }
            check(files > 40 && faults == 0, "corners: none of the " + juce::String(files) + " source files draws a rounded rectangle, or a round-ended line that is not on a round object");
        }
        check(Theme::Plate::bolt >= 6.0f && Theme::Plate::bolt <= 7.0f && Theme::Plate::boltInset >= Theme::Plate::bolt,
              "fasteners: a bolt head is 6 to 7 across (big enough to read as a bolt) and sits clear of its plate's edge");
        check(contrast(TC::bolt, TC::panel) >= 3.0 && contrast(TC::bolt, TC::boltSlot) >= 3.0 && contrast(TC::bolt, TC::raised) >= 3.0,
              "fasteners: a bolt reads on its plate, and its slot reads on the bolt (" + juce::String(contrast(TC::bolt, TC::panel), 2) + ", "
                  + juce::String(contrast(TC::bolt, TC::boltSlot), 2) + ")");
        check(luminance(TC::edgeDark) < luminance(TC::window) && TC::edgeLight.getAlpha() > 0 && TC::edgeLight.getAlpha() < 80,
              "edges: the pressed line is darker than anything it is drawn on, and the light line beside it is faint");
        {
            // The lit plates: the lettering on a plate in a section's colour, the brass or a modulator's colour.
            const juce::Colour lit[] = { TC::brass, TC::busA, TC::busB, TC::hits, TC::stowaway, TC::output, TC::go, TC::space, TC::filter,
                                         Theme::modColour(0), Theme::modColour(1), Theme::modColour(2), Theme::modColour(3) };
            for (const auto& plate : lit)
                check(plate.getPerceivedBrightness() > 0.5f && contrast(TC::window, plate) >= 4.5,
                      "lit plates: dark lettering reads on " + plate.toDisplayString(false) + " (" + juce::String(contrast(TC::window, plate), 2) + ":1)");
            for (int lane = 0; lane < 10; ++lane)
                check(contrast(TC::window, Theme::trackColour(lane)) >= 4.5, "lit plates: dark lettering reads on track colour " + juce::String(lane + 1));
            // A lane is a shade of its bus's colour: every shade takes dark lettering, reads on a well, and
            // neighbouring shades of one bus can be told apart.
            for (const auto& bus : { TC::busA, TC::busB, TC::hits, TC::stowaway })
            {
                bool reads = Theme::laneShade(bus, 0) == bus, apart = true;
                for (int step = 0; step < 5; ++step)
                {
                    const auto shade = Theme::laneShade(bus, step);
                    reads = reads && contrast(TC::window, shade) >= 4.5 && contrast(shade, TC::well) >= 4.5;
                    // The next lane on the bus is a clear step away; any two differ.
                    for (int other = step + 1; other < 5; ++other)
                        apart = apart && contrast(shade, Theme::laneShade(bus, other)) >= (other == step + 1 ? 1.15 : 1.06);
                }
                check(reads && apart, "lanes: the five shades of " + bus.toDisplayString(false) + " all take dark lettering, read in a well and differ from each other");
            }
        }
        {
            // A plate's shape, when it is given a cut: corners taken off at 45 degrees, nothing round. Its path
            // is eight straight lines. With no cut (every token) it is its rectangle.
            const juce::Rectangle<float> whole(10.0f, 20.0f, 80.0f, 24.0f);
            const auto& plain = Theme::Plate::shape(whole, Theme::Cut::control);
            check(plain.contains(10.2f, 20.2f) && plain.contains(89.8f, 43.8f) && plain.getBounds() == whole, "plates: a plate is its whole rectangle");
            const auto& shape = Theme::Plate::shape(whole, 3.0f);
            int lines = 0, curves = 0;
            for (juce::Path::Iterator it(shape); it.next();)
            {
                lines += it.elementType == juce::Path::Iterator::lineTo ? 1 : 0;
                curves += it.elementType == juce::Path::Iterator::quadraticTo || it.elementType == juce::Path::Iterator::cubicTo ? 1 : 0;
            }
            check(curves == 0 && lines == 7 && ! shape.contains(10.5f, 20.5f) && shape.contains(10.5f, 32.0f) && shape.contains(14.0f, 21.0f)
                      && shape.getBounds() == juce::Rectangle<float>(10.0f, 20.0f, 80.0f, 24.0f),
                  "plates: a cut, where one is asked for, is straight (no curves)");
        }
        check(Theme::Space::unit == 4 && Theme::Space::gutter == 12 && Theme::Space::gutter % Theme::Space::unit == 0 && Theme::Space::panelPadding % Theme::Space::unit == 0,
              "spacing: a 4 px unit, a 12 px gutter");
        using F = Theme::Fonts;
        check(F::c_caption >= Theme::c_minTextSize && F::c_caption < F::c_body && F::c_body < F::c_title && F::c_title < F::c_display && F::c_display < F::c_wordmark,
              "type: four steps and the wordmark, each larger than the last, none under the floor of " + juce::String(Theme::c_minTextSize, 0));
    }

    // Fitted text ------------------------------------------------------------------------------------------------
    std::cout << "Fitted text" << std::endl;
    {
        juce::SharedResourcePointer<Theme::Fonts> fonts;
        const auto font = fonts->control();
        const juce::String label = "Follow Master";
        const float natural = juce::GlyphArrangement::getStringWidth(font, label);

        const auto fits = Theme::fitToWidth(label, font, natural + 2.0f);
        check(! fits.shrunk && ! fits.cut && fits.text == label && std::abs(fits.font.getHeight() - font.getHeight()) < 1.0e-4f, "fit: a text that fits is left as it is");

        const auto tight = Theme::fitToWidth(label, font, natural * 0.93f);
        check(tight.shrunk && ! tight.cut && tight.text == label && tight.width <= natural * 0.93f && tight.font.getHeight() < font.getHeight()
                  && tight.font.getHeight() >= Theme::c_minFontHeight,
              "fit: a little too wide is set smaller, whole (" + juce::String(tight.font.getHeight() / Theme::Fonts::c_em, 1) + ")");

        const auto narrow = Theme::fitToWidth(label, font, natural * 0.5f);
        check(narrow.cut && narrow.text.endsWith("...") && narrow.width <= natural * 0.5f && std::abs(narrow.font.getHeight() - Theme::c_minFontHeight) < 1.0e-3f,
              "fit: far too wide stops at the floor of " + juce::String(Theme::c_minTextSize, 0) + " and is cut at its end (\"" + narrow.text + "\")");

        const juce::String file = "ANMLFarm_Pig_Squeal_Angry_Close_Short_Animal_Close_01.wav";
        const auto name = Theme::fitToWidth(file, font, 150.0f, Theme::Fit::Name);
        check(name.cut && name.text.startsWith("ANML") && name.text.endsWith("01.wav") && name.text.contains("...") && name.width <= 150.0f,
              "fit: a name is cut in its middle, keeping its start and its end (\"" + name.text + "\")");

        bool neverWider = true;
        for (float width = 4.0f; width < natural + 40.0f; width += 3.0f)
            for (const auto fit : { Theme::Fit::Whole, Theme::Fit::Sentence, Theme::Fit::Message, Theme::Fit::Name })
                neverWider = neverWider && Theme::fitToWidth(label, font, width, fit).width <= width + 0.01f;
        check(neverWider, "fit: whatever the width and the kind of text, what comes out is never wider than the box");
        check(Theme::mayBeCut(Theme::Fit::Name) && Theme::mayBeCut(Theme::Fit::Message) && ! Theme::mayBeCut(Theme::Fit::Whole) && ! Theme::mayBeCut(Theme::Fit::Sentence),
              "fit: names and messages may be cut; labels and hints may not (the checker reports one that was)");

        // A text is laid out once and kept: measuring and drawing it again is a lookup, not another layout (laying
        // text out is what costs, and the interface redraws the same labels many times a second).
        {
            auto& cache = Theme::detail::TextCache::slot();
            bool sameWidths = true;
            for (const auto* text : { "Follow Master", "Throw Length", "14.82 s   8 sounds", "A", "Drop More Sounds Here" })
                for (const auto& f : { fonts->caption(), fonts->control(), fonts->title(), fonts->display() })
                    sameWidths = sameWidths && std::abs(Theme::textWidth(f, text) - juce::GlyphArrangement::getStringWidth(f, text)) < 1.0e-3f;
            check(sameWidths, "kept text: a kept width is the width JUCE measures");

            const juce::String once = "Laid Out Once";
            Theme::textWidth(font, once);
            const auto before = cache != nullptr ? cache->map.size() : 0;
            juce::Image image(juce::Image::ARGB, 200, 40, true);
            juce::Graphics g(image);
            for (int i = 0; i < 50; ++i)
            {
                Theme::fitToWidth(once, font, 180.0f);
                Theme::fitText(g, once, juce::Rectangle<float>(4.0f, 4.0f, 180.0f, 20.0f), font, juce::Justification::centred);
            }
            check(cache != nullptr && cache->map.size() == before, "kept text: fifty measurements and drawings of one label lay it out once");

            // It lands where JUCE's own justification puts it.
            struct Catch : Theme::TextProbe
            {
                juce::Rectangle<float> ink;
                void textDrawn(juce::Graphics&, const juce::String&, juce::Rectangle<float>, juce::Rectangle<float> i, float, Theme::Fit, bool, bool) override { ink = i; }
            } caught;
            Theme::textProbe() = &caught;
            bool samePlace = true;
            const juce::Rectangle<float> box(12.0f, 6.0f, 170.0f, 26.0f);
            for (const auto j : { juce::Justification::centredLeft, juce::Justification::centred, juce::Justification::centredRight,
                                  juce::Justification::topLeft, juce::Justification::bottomRight, juce::Justification::centredTop })
            {
                Theme::fitText(g, " Auto Ride ", box, font, j);
                juce::GlyphArrangement reference;
                reference.addLineOfText(font, " Auto Ride ", 0.0f, 0.0f);
                reference.justifyGlyphs(0, reference.getNumGlyphs(), box.getX(), box.getY(), box.getWidth(), box.getHeight(), j);
                const auto want = reference.getBoundingBox(0, -1, true);
                samePlace = samePlace && std::abs(want.getX() - caught.ink.getX()) < 0.01f && std::abs(want.getY() - caught.ink.getY()) < 0.01f
                            && std::abs(want.getWidth() - caught.ink.getWidth()) < 0.01f;
            }
            Theme::textProbe() = nullptr;
            check(samePlace, "kept text: it is placed in its box where JUCE's justification places it (left, centre, right, top, bottom)");

            for (int i = 0; i < 3000; ++i)
                Theme::textWidth(font, "Rec " + juce::String(i));
            check(cache != nullptr && cache->map.size() <= Theme::detail::TextCache::c_most,
                  "kept text: readouts that keep changing do not pile up (at most " + juce::String(static_cast<int>(Theme::detail::TextCache::c_most)) + " kept)");
        }

        // The checker sees a cut label, a text under the floor, two texts on top of each other and capitals.
        struct Canvas : juce::Component
        {
            std::function<void(juce::Graphics&)> draw;
            void paint(juce::Graphics& g) override { draw(g); }
        };
        auto faultsOf = [](std::function<void(juce::Graphics&)> draw)
        {
            Canvas canvas;
            canvas.draw = std::move(draw);
            canvas.setSize(300, 60);
            UiProbe::Recorder recorder;
            recorder.paint(canvas, 1.0f);
            juce::StringArray kinds;
            for (const auto& f : UiProbe::check(recorder.texts))
                kinds.addIfNotAlreadyThere(f.what);
            return kinds.joinIntoString(", ");
        };
        const auto caption = fonts->caption();
        check(faultsOf([&](juce::Graphics& g) { Theme::fitText(g, "Skip Silence", juce::Rectangle<float>(10, 10, 120, 16), caption, juce::Justification::centredLeft); }).isEmpty(),
              "checker: a label with room is fine");
        check(faultsOf([&](juce::Graphics& g) { Theme::fitText(g, "Skip Silence", juce::Rectangle<float>(10, 10, 30, 16), caption, juce::Justification::centredLeft); }) == "does not fit",
              "checker: a label cut to fit its box is reported");
        check(faultsOf([&](juce::Graphics& g) { Theme::fitText(g, "Skip Silence", juce::Rectangle<float>(10, 10, 120, 16), caption.withHeight(8.0f), juce::Justification::centredLeft); }) == "too small",
              "checker: a text under the smallest size is reported");
        check(faultsOf([&](juce::Graphics& g)
                       {
                           Theme::fitText(g, "Doppler", juce::Rectangle<float>(10, 10, 120, 16), caption, juce::Justification::centredLeft);
                           Theme::fitText(g, "Width", juce::Rectangle<float>(30, 12, 120, 16), caption, juce::Justification::centredLeft);
                       }) == "overlaps",
              "checker: one text drawn over another is reported");
        check(faultsOf([&](juce::Graphics& g) { Theme::fitText(g, "SKIP SILENCE", juce::Rectangle<float>(10, 10, 200, 16), caption, juce::Justification::centredLeft); }) == "capitals",
              "checker: a label in capitals is reported");
        check(faultsOf([&](juce::Graphics& g)
                       {
                           g.addTransform(juce::AffineTransform::scale(0.5f).translated(40.0f, 10.0f));
                           Theme::fitText(g, "Depth", juce::Rectangle<float>(0, 0, 120, 16), caption, juce::Justification::centredLeft);
                       }) == "too small",
              "checker: sizes are measured where the text lands (a text drawn at half size is too small)");
    }

    // Capitals ---------------------------------------------------------------------------------------------------
    std::cout << "Capitals" << std::endl;
    {
        using UiProbe::capitalsFault;
        const auto label = Theme::Fit::Whole;
        bool good = true;
        for (const char* text : { "Low End", "Skip Silence", "Energy Bed", "Take Turns", "Keep the Last", "One File per Whoosh", "Match Pitch to Master",
                                  "Auto (Bus A, by Type)", "GR", "LFO", "Learn MIDI CC", "Last 60 s", "44.1 kHz", "32-Bit Float", "A / B", "Fly-By",
                                  "3 wires", "4 of 10", "+12 st", "-0.3 dB", "Buses A + B", "Type a Value...", "Swooshbuckler 2.0 Is Available" })
            if (const auto fault = capitalsFault(text, label); fault.isNotEmpty())
            {
                good = false;
                std::cout << "         \"" << text << "\": " << fault << std::endl;
            }
        check(good, "rule: control names in Title Case pass, with small joining words, units, acronyms and readouts as they are");
        bool bad = true;
        for (const char* text : { "LOW END", "Low end", "skip Silence", "RANDOMISE", "Peak tremolo", "AUTO RIDE", "Hit gate" })
            if (capitalsFault(text, label).isEmpty())
            {
                bad = false;
                std::cout << "         \"" << text << "\" passed and should not" << std::endl;
            }
        check(bad, "rule: capitals, and a name with a small second word, are faults in a label");
        check(capitalsFault("The faders move by themselves", Theme::Fit::Sentence).isEmpty() && capitalsFault("the faders move", Theme::Fit::Sentence).isNotEmpty()
                  && capitalsFault("Press PLAY to hear it", Theme::Fit::Message).isNotEmpty() && capitalsFault("Learn MIDI CC cancelled.", Theme::Fit::Message).isEmpty(),
              "rule: a sentence starts with a capital and shouts nowhere (acronyms aside)");
        check(capitalsFault("ANMLFarm_PIG_squeal.wav", Theme::Fit::Name).isEmpty(), "rule: a name of the user's is left as they wrote it");

        // Every parameter's name, and every choice it offers (what a host shows in its automation lanes).
        auto base = makeProcessor();
        int checked = 0, wrong = 0;
        for (auto* p : base->getParameters())
        {
            juce::StringArray texts { p->getName(100) };
            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(p))
                texts.addArray(choice->choices);
            for (const auto& text : texts)
            {
                ++checked;
                if (const auto fault = capitalsFault(text, label); fault.isNotEmpty())
                {
                    ++wrong;
                    std::cout << "         \"" << text << "\": " << fault << std::endl;
                }
            }
        }
        check(wrong == 0 && checked > 150, "parameters: all " + juce::String(checked) + " names and choices are in Title Case");
    }

    // Motion -----------------------------------------------------------------------------------------------------
    std::cout << "Motion" << std::endl;
    {
        // A fade eases to its target in its own time and never goes past it.
        {
            Motion::Fade fade;
            fade.snap(0.0f);
            fade.to(1.0f, 120.0f);
            float highest = 0.0f;
            int frames = 0;
            while (fade.step(1.0f / 60.0f) && frames < 100)
            {
                highest = juce::jmax(highest, fade.value);
                ++frames;
            }
            const bool quick = fade.value > 0.4f; // ease-out: well past half way after the first third
            Motion::Fade third;
            third.snap(0.0f);
            third.to(1.0f, 120.0f);
            third.step(0.04f);
            check(std::abs(fade.value - 1.0f) < 1.0e-5f && highest <= 1.0f && frames > 2 && frames < 20 && third.value > 0.6f && quick,
                  "motion: a fade reaches its target in " + juce::String(frames) + " frames, eases out (" + juce::String(third.value, 2)
                      + " of the way at a third of the time) and never overshoots");
        }
        // A spring overshoots a little and settles; a very late frame catches up instead of flying apart.
        {
            Motion::Spring helm;
            helm.snap(0.0f);
            helm.to(1.0f);
            float highest = 0.0f;
            int frames = 0;
            while (helm.stepHelm(1.0f / 60.0f) && frames < 600)
            {
                highest = juce::jmax(highest, helm.value);
                ++frames;
            }
            Motion::Spring late;
            late.snap(0.0f);
            late.to(1.0f);
            for (int i = 0; i < 20; ++i)
                late.stepHelm(2.0f); // two whole seconds a frame: clamped to Motion::c_maxStep
            check(std::abs(helm.value - 1.0f) < 1.0e-3f && highest > 1.0f && highest < 1.2f && frames > 10 && frames < 90
                      && std::isfinite(late.value) && std::abs(late.value - 1.0f) < 0.05f,
                  "motion: the Helm spring overshoots " + juce::String(juce::roundToInt((highest - 1.0f) * 100.0f)) + "% and settles in "
                      + juce::String(frames) + " frames; a 2 s frame lands it, not launches it");
        }
        // One clock: every listener gets the same frame, the step is clamped, and one that goes mid-frame is safe.
        {
            struct Counter : Motion::FrameClock::Listener
            {
                int frames = 0;
                float last = 0.0f;
                std::function<void()> alsoDo;
                void frame(float dt) override { ++frames; last = dt; if (alsoDo) alsoDo(); }
            };
            Motion::FrameClock clock;
            Counter a, b, c;
            clock.addListener(&a);
            clock.addListener(&b);
            clock.addListener(&c);
            clock.addListener(&a); // twice is once
            a.alsoDo = [&clock, &c] { clock.removeListener(&c); }; // c goes while the frame is being handed out
            clock.tick(5.0f);
            const bool clamped = std::abs(a.last - Motion::c_maxStep) < 1.0e-6f;
            const int afterOne = clock.listenerCount();
            clock.tick(1.0f / 60.0f);
            check(a.frames == 2 && b.frames == 2 && afterOne == 2 && clock.listenerCount() == 2 && clamped
                      && std::abs(b.last - 1.0f / 60.0f) < 1.0e-6f,
                  "motion: one clock hands every listener the same frame, clamps a 5 s step to "
                      + juce::String(Motion::c_maxStep, 3) + " s, and lets a listener leave inside its own frame");
        }
    }

    // The keys ---------------------------------------------------------------------------------------------------
    std::cout << "The keys" << std::endl;
    {
        // Every action is named in Title Case, no two share a name, and every default key is one of its own.
        {
            std::set<juce::String> names, keys;
            int wrong = 0;
            for (const auto& info : Shortcuts::all())
            {
                const juce::String name(info.name);
                if (UiProbe::capitalsFault(name, Theme::Fit::Whole).isNotEmpty() || ! names.insert(name).second)
                    ++wrong;
            }
            Shortcuts::Registry registry;
            for (int i = 0; i < Shortcuts::numActions; ++i)
            {
                const auto key = registry.key(static_cast<Shortcuts::Action>(i));
                if (! key.isValid() || ! keys.insert(Shortcuts::describe(key)).second)
                    ++wrong;
            }
            check(wrong == 0 && names.size() == static_cast<size_t>(Shortcuts::numActions) && keys.size() == names.size(),
                  "keys: all " + juce::String(Shortcuts::numActions) + " actions have a Title Case name of their own and a default key of its own");
        }
        // A key is written the same way every time, and reads back as itself.
        {
            Shortcuts::Registry registry;
            bool roundTrips = true;
            for (int i = 0; i < Shortcuts::numActions; ++i)
            {
                const auto action = static_cast<Shortcuts::Action>(i);
                const auto key = registry.key(action);
                roundTrips = roundTrips && registry.find(key) == action;
            }
            const auto undo = registry.keyText(Shortcuts::Action::Undo);
            const auto redo = registry.keyText(Shortcuts::Action::Redo);
           #if JUCE_MAC
            const bool written = undo == "Cmd+Z" && redo == "Shift+Cmd+Z";
           #else
            const bool written = undo == "Ctrl+Z" && redo == "Ctrl+Shift+Z";
           #endif
            check(roundTrips && written && registry.keyText(Shortcuts::Action::Swell) == "Space",
                  "keys: every key finds its own action again, and is written one way (Undo is " + undo + ")");
        }
        // Giving a key away: the action that had it loses it, and nothing else moves.
        {
            Shortcuts::Registry registry;
            const auto randomiseKey = registry.key(Shortcuts::Action::Randomise);
            const auto taken = registry.set(Shortcuts::Action::Throw, randomiseKey);
            const bool moved = taken == Shortcuts::Action::Randomise
                               && ! registry.key(Shortcuts::Action::Randomise).isValid()
                               && registry.find(randomiseKey) == Shortcuts::Action::Throw
                               && ! registry.isDefault();
            registry.resetToDefaults();
            check(moved && registry.isDefault() && registry.find(randomiseKey) == Shortcuts::Action::Randomise,
                  "keys: a key given to another action is taken off the one that had it, and Reset To Defaults puts them all back");
        }
        // The keys are kept in the settings file.
        {
            const auto file = folder.getChildFile("keys.settings");
            file.deleteFile();
            juce::PropertiesFile::Options options;
            options.applicationName = "keys";
            options.filenameSuffix = ".settings";
            bool kept = false, emptyKept = false;
            {
                juce::PropertiesFile settings(file, options);
                Shortcuts::Registry registry;
                registry.attach(&settings);
                registry.set(Shortcuts::Action::Randomise, juce::KeyPress('Q'));
                registry.set(Shortcuts::Action::Throw, juce::KeyPress());
                settings.saveIfNeeded();
            }
            {
                juce::PropertiesFile settings(file, options);
                Shortcuts::Registry registry;
                registry.attach(&settings);
                kept = registry.find(juce::KeyPress('Q')) == Shortcuts::Action::Randomise;
                emptyKept = ! registry.key(Shortcuts::Action::Throw).isValid();
            }
            check(kept && emptyKept, "keys: a key you change, and one you take away, are still yours the next time");
        }
    }

    // The editor -------------------------------------------------------------------------------------------------
    std::cout << "The editor" << std::endl;
    {
        auto base = makeProcessor();
        auto& proc = *dynamic_cast<Proc*>(base.get());
        const auto recordings = folder.getChildFile("Recordings");
        recordings.deleteRecursively();
        recordings.createDirectory();
        proc.setRecordFolder(recordings);

        // The first run.
        {
            Rig rig(proc);
            rig.open();
            rig.run(0.2);
            checkTexts("first run (no sounds)", *rig.editor, 1.0f, 1.0f);
            check(rig.editor->m_playButton.getButtonText() == "Play" && Icons::get(rig.editor->m_playButton) == Icons::Icon::Play
                      && SwooshLookAndFeel::getStyle(rig.editor->m_playButton) == SwooshLookAndFeel::ButtonStyle::Primary,
                  "header: Play is the one filled button, with its glyph");

            // The window that asks for a preset's name is a plate like the rest: square, filled to its corners
            // (its window is opaque), with a bolt in each corner.
            {
                auto& ed = *rig.editor;
                std::unique_ptr<juce::AlertWindow> dialog(ed.makeNameDialog("Save Preset", "The settings as they are now are saved under this name.", "My Preset", "Save", true));
                UiProbe::Recorder recorder;
                const auto image = recorder.paint(*dialog, 1.0f);
                const int w = image.getWidth(), h = image.getHeight();
                bool corners = true, bolts = true;
                const int inset = juce::roundToInt(Theme::Plate::boltInset);
                for (const auto& c : { juce::Point<int>(0, 0), juce::Point<int>(w - 1, 0), juce::Point<int>(0, h - 1), juce::Point<int>(w - 1, h - 1) })
                {
                    corners = corners && image.getPixelAt(c.x, c.y).getAlpha() == 255 && luminance(image.getPixelAt(c.x, c.y)) < luminance(TC::panel);
                    const juce::Point<int> b(c.x == 0 ? inset : w - inset, c.y == 0 ? inset - 2 : h - inset - 2); // on the head, above its slot
                    bolts = bolts && contrast(image.getPixelAt(b.x, b.y), TC::panel) >= 2.0;
                }
                check(corners && bolts, "name window: a square plate with a pressed edge to its very corners and a bolt in each");
                dialog->removeCustomComponent(0);
                dialog.reset();
                ed.m_dialogToggle.reset();
            }
        }

        proc.loadDemoSounds();
        for (const float scale : { 0.6f, 1.0f, 1.5f })
        {
            Rig rig(proc, scale);
            rig.open();
            auto& ed = *rig.editor;
            proc.setPlaying(true);
            rig.run(1.2);
            const juce::String at = " at " + juce::String(scale, 1);
            const int texts = checkTexts("the window, playing," + at, ed, 1.0f, scale);
            if (scale == 1.0f)
            {
                check(texts > 120, "the window: the checker saw its texts (" + juce::String(texts) + ")");
                check(ed.m_playButton.getButtonText() == "Stop" && Icons::get(ed.m_playButton) == Icons::Icon::Stop, "header: playing, the button reads Stop with the stop glyph");

                // Tooltips: a heading in Title Case, then what it does; no word in capitals.
                juce::StringArray tips;
                collectTooltips(ed, tips);
                int badHeadings = 0, shouted = 0, noBody = 0;
                for (const auto& tip : tips)
                {
                    const auto heading = tip.upToFirstOccurrenceOf("\n", false, false);
                    if (! tip.containsChar('\n'))
                    {
                        ++noBody;
                        std::cout << "         no heading line: \"" << tip.substring(0, 70) << "\"" << std::endl;
                    }
                    else if (const auto fault = UiProbe::capitalsFault(heading, Theme::Fit::Whole); fault.isNotEmpty())
                    {
                        ++badHeadings;
                        std::cout << "         heading \"" << heading << "\": " << fault << std::endl;
                    }
                    if (const auto word = UiProbe::shoutedWord(tip); word.isNotEmpty())
                    {
                        ++shouted;
                        std::cout << "         \"" << word << "\" in the tip \"" << heading << "\"" << std::endl;
                    }
                }
                check(tips.size() > 90 && noBody == 0 && badHeadings == 0 && shouted == 0,
                      "tooltips: all " + juce::String(tips.size()) + " start with a heading in Title Case and none has a word in capitals");

                // The how-to that is the same on every knob is written once, in the tooltip window, not forty times.
                {
                    int repeated = 0;
                    for (const auto& tip : tips)
                        if (tip.contains("Double-click to reset it"))
                            ++repeated;
                    check(repeated == 0, "tooltips: the how-to line that is the same on every control is written once, in the tooltip window, not on each of them");
                }

                // The keys go through the registry: move Randomise to a key of its own and that key rolls, while
                // the key it used to have does nothing. Hard-coded keys cannot pass this.
                {
                    const auto energyId = juce::String(ParamIDs::energy);
                    auto* energy = proc.getState().getParameter(energyId);
                    const auto was = ed.m_keys.key(Shortcuts::Action::Randomise);
                    ed.m_keys.set(Shortcuts::Action::Randomise, juce::KeyPress('Q'));
                    ed.m_morph.finish();
                    const int before = static_cast<int>(ed.m_morph.items.size());
                    const bool oldKeyDead = ! ed.keyPressed(was) && ed.m_morph.items.empty();
                    const bool newKeyRolls = ed.keyPressed(juce::KeyPress('Q')) && ! ed.m_morph.items.empty();
                    ed.m_morph.finish();
                    ed.m_keys.resetToDefaults();
                    const bool backAgain = ed.keyPressed(was) && ! ed.m_morph.items.empty();
                    check(before == 0 && oldKeyDead && newKeyRolls && backAgain && energy != nullptr,
                          "keys: Randomise rolls from whatever key it has been given, and from no other");

                    // The glide passes through the window: the knobs set off left to right, row by row, inside a
                    // quarter of a second, and the one furthest on waits longest.
                    // Every knob with a place in the window, paired with when it sets off: sorting by place must
                    // sort by delay too, and nobody waits longer than the wave is long.
                    std::vector<std::pair<int, float>> wave;
                    float longest = 0.0f, shortest = 1.0f;
                    for (const auto& item : ed.m_morph.items)
                    {
                        if (item.param == nullptr)
                            continue;
                        longest = juce::jmax(longest, item.delay);
                        shortest = juce::jmin(shortest, item.delay);
                        const auto found = ed.m_controls.find(item.param->getParameterID());
                        if (found == ed.m_controls.end() || found->second == nullptr)
                            continue;
                        const auto box = found->second->getBounds();
                        wave.push_back({ (box.getCentreY() / 80) * 10000 + box.getCentreX(), item.delay });
                    }
                    std::sort(wave.begin(), wave.end());
                    bool ordered = wave.size() > 3;
                    for (size_t i = 1; i < wave.size(); ++i)
                        ordered = ordered && wave[i].second >= wave[i - 1].second - 1.0e-6f;
                    check(ed.m_morph.items.size() > 5 && shortest <= 1.0e-6f && longest > 0.0f
                              && longest <= Editor::Morph::c_wave + 1.0e-6f && ordered,
                          "motion: a roll passes through the knobs as a wave - the first sets off at once, the last after "
                              + juce::String(juce::roundToInt(longest * 1000.0f)) + " ms, and it is the one furthest down and right");
                    ed.m_morph.finish();
                }

                // The tape's reels spin up when it starts writing and coast down when it stops.
                {
                    auto& tape = ed.m_tape;
                    tape.m_speed = 0.0f;
                    for (int i = 0; i < 20; ++i)
                        tape.frame(1.0f / 60.0f);   // not capturing: they stay still
                    const float stopped = tape.m_speed;
                    tape.m_speed = 1.0f;
                    const float wasAngle = tape.m_angle;
                    for (int i = 0; i < 6; ++i)
                        tape.frame(1.0f / 60.0f);   // coasting down, still turning
                    check(stopped <= 1.0e-3f && tape.m_speed > 0.3f && tape.m_speed < 1.0f && tape.m_angle != wasAngle,
                          "motion: the tape's reels coast down rather than stopping dead (" + juce::String(tape.m_speed, 2) + " of speed after a tenth of a second)");

                    // Last 60 s goes back over what has just been played: the reels turn the other way for it.
                    tape.m_speed = 0.0f;
                    tape.m_rewind = 0.0f;
                    for (int i = 0; i < 40; ++i)
                        tape.frame(1.0f / 60.0f);       // let the coast die away
                    const float still = tape.m_angle;
                    tape.m_speed = 1.0f;
                    tape.frame(1.0f / 60.0f);           // one frame of playing: the angle winds down
                    const float forward = tape.m_angle - still;
                    tape.m_speed = 0.0f;
                    const float before = tape.m_angle;
                    tape.rewind();
                    for (int i = 0; i < 60; ++i)
                        tape.frame(1.0f / 60.0f);
                    const float back = tape.m_angle - before;
                    check(forward < 0.0f && back > 0.0f && std::abs(back) > 2.5f && std::abs(back) < 3.3f,
                          "motion: Last 60 s flicks the reels back half a turn - the other way from playing ("
                              + juce::String(back, 2) + " against " + juce::String(forward, 3) + " a frame)");
                }

                // A key is named in one place only: the registry. No control writes its own key into its words,
                // or the tooltip window would say one thing and the control's own sentence another.
                {
                    const char* spelledOut[] = { "The R key", "The T key", "The L key", "The F key", "the Return key",
                                                 "The space bar", "The spacebar", "the space bar", "key does the same",
                                                 "key rolls too", "key arms", "key saves it too", "Cmd+Z does the same",
                                                 "Shift+Cmd+Z does the same", "(Cmd+Z)", "Cmd+Z brings", "Cmd+Z takes" };
                    int named = 0;
                    for (const auto& tip : tips)
                        for (const auto* phrase : spelledOut)
                            if (tip.contains(phrase))
                            {
                                ++named;
                                std::cout << "         \"" << phrase << "\" in \""
                                          << tip.upToFirstOccurrenceOf("\n", false, false) << "\"" << std::endl;
                            }
                    check(named == 0, "keys: no control spells a key out in its own words - the key comes from the registry, so it follows a change");
                }

                // The two keys that are drawn rather than asked for follow the registry as well.
                {
                    const auto wasSwell = ed.m_keys.key(Shortcuts::Action::Swell);
                    ed.m_keys.set(Shortcuts::Action::Swell, juce::KeyPress('Q'));
                    ed.refreshShortcuts();
                    const bool moved = ed.m_swell.m_keyHint == "Q" && ed.m_sea.getTooltip().contains("press Q");
                    ed.m_keys.set(Shortcuts::Action::Swell, wasSwell);
                    ed.refreshShortcuts();
                    check(moved && ed.m_swell.m_keyHint == "Space" && ed.m_sea.getTooltip().contains("press Space"),
                          "keys: the hint under Swell and the sea's own tip name the key Swell has now, not the one it started with");
                }
            }

            // Wires, linked buses and the tape armed: the three looks.
            proc.getMods().connect(0, ParamIDs::bus[0][ParamIDs::Doppler], 0.6f);
            proc.getMods().connect(2, ParamIDs::hitBoost, 0.5f);
            proc.getState().getParameter(ParamIDs::linkBuses)->setValueNotifyingHost(1.0f);
            ed.m_wires.rebuild();
            rig.run(0.3);
            checkTexts("wires and linked buses," + at, ed, 1.0f, scale);
            if (scale == 1.0f)
            {
                auto* linked = ed.m_controls[ParamIDs::bus[1][ParamIDs::Doppler]];
                auto* resting = ed.m_controls[ParamIDs::tremDepth];
                auto* disabled = ed.m_controls[ParamIDs::hitRelease];
                check(Theme::getLook(*linked) == Theme::Look::Linked && ! linked->isEnabled() && Theme::getLook(ed.m_passMode[1]) == Theme::Look::Linked,
                      "looks: with Link on, bus B's Doppler and its Pass menu are Linked (and cannot be moved)");
                check(Theme::getLook(*resting) == Theme::Look::Resting && resting->isEnabled() && Theme::getLook(ed.m_tremShape) == Theme::Look::Resting,
                      "looks: with Peak Tremolo off, its knobs and menus are Resting (and can still be set)");
                check(Theme::getLook(*disabled) == Theme::Look::Disabled && ! disabled->isEnabled(), "looks: with Aim on, the gate's Release is Disabled");
                check(Theme::getLook(*ed.m_controls[ParamIDs::bus[0][ParamIDs::Doppler]]) == Theme::Look::Normal, "looks: bus A's Doppler is as usual");

                ed.m_tremOn.setToggleState(true, juce::sendNotificationSync);
                rig.run(0.1);
                check(proc.getState().getRawParameterValue(ParamIDs::tremOn)->load() > 0.5f && Theme::getLook(*resting) == Theme::Look::Normal,
                      "Peak Tremolo: its switch turns the tremolo on, and its knobs wake up");
                ed.m_tremOn.setToggleState(false, juce::sendNotificationSync);

                check(dynamic_cast<IconButton*>(&ed.m_presetPrev) != nullptr && ed.m_presetPrev.getTooltip().startsWith("Previous Preset")
                          && SwooshLookAndFeel::getStyle(ed.m_presetName) == SwooshLookAndFeel::ButtonStyle::Menu,
                      "header: the preset arrows are drawn icons, and the preset box is a menu");
            }
            // The knobs: two wheels, whole in their boxes, on one column pitch.
            {
                std::vector<WheelKnob*> knobs;
                collect(ed, knobs);
                int whole = 0, primary = 0;
                for (auto* k : knobs)
                {
                    const auto size = k->getSize();
                    primary += size == WheelKnob::Size::Primary ? 1 : 0;
                    const float ring = k->getRingRadius(), outer = ring + WheelKnob::c_modRoom;
                    const auto centre = k->getDialCentre();
                    const auto caption = k->getCaptionArea(), value = k->getValueArea();
                    const auto box = k->getLocalBounds().toFloat();
                    const bool ok = std::abs(ring - WheelKnob::ringFor(size)) < 1.0e-3f
                                    && k->getWidth() >= WheelKnob::cellWidth(size) && k->getHeight() >= WheelKnob::cellHeight(size)
                                    && centre.x - outer >= -0.01f && centre.x + outer <= box.getWidth() + 0.01f && centre.y - outer >= -0.01f
                                    && centre.y + outer <= caption.getY() + 0.01f && caption.getBottom() <= value.getY() + 0.01f
                                    && box.contains(caption) && box.contains(value) && caption.getHeight() >= 12.0f && value.getHeight() >= 12.0f;
                    whole += ok ? 1 : 0;
                    if (! ok)
                        std::cout << "         " << k->caption << ": box " << k->getBounds().toString() << ", ring " << ring << std::endl;
                }
                const int count = static_cast<int>(knobs.size());
                check(count >= 40 && whole == count && primary >= 20 && primary < count,
                      "knobs: all " + juce::String(count) + " are one of two wheels (" + juce::String(primary) + " primary, " + juce::String(count - primary)
                          + " secondary), whole in their boxes with the dial over the caption and the value," + at);

                // Bus A, bus B and the Hits: each a row on the one shared column pitch, and the Hits' knobs sit on
                // the very columns of bus A's, the panel above them.
                std::vector<int> pitches;
                std::array<std::vector<int>, 3> columns;
                bool even = true;
                for (int section = 0; section < 3; ++section)
                {
                    auto& xs = columns[static_cast<size_t>(section)];
                    for (auto* k : knobs)
                        if (k->getParentComponent() == &ed.m_content && k->getSize() == WheelKnob::Size::Primary
                            && ed.m_sections[static_cast<size_t>(section)].bounds.contains(k->getBounds().getCentre()))
                            xs.push_back(k->getBounds().getCentreX());
                    std::sort(xs.begin(), xs.end());
                    even = even && xs.size() >= 7;
                    for (size_t i = 1; i < xs.size(); ++i)
                    {
                        even = even && xs[i] - xs[i - 1] == xs[1] - xs[0];
                        if (std::find(pitches.begin(), pitches.end(), xs[i] - xs[i - 1]) == pitches.end())
                            pitches.push_back(xs[i] - xs[i - 1]);
                    }
                }
                check(even && pitches.size() == 1, "knobs: bus A, bus B and the Hits are rows on one shared column pitch (" + juce::String(pitches.empty() ? 0 : pitches[0]) + " px)" + at);
                check(columns[2] == columns[0], "knobs: the Hits' seven knobs are on the same columns as bus A's, the panel above them" + at);

                // Nothing is drawn by JUCE's own look: every slider and scrollbar has ours.
                std::vector<juce::Slider*> sliders;
                std::vector<juce::ScrollBar*> bars;
                collect(ed, sliders);
                collect(ed, bars);
                int ours = 0, faders = 0;
                for (auto* slider : sliders)
                {
                    const bool drawnByItself = dynamic_cast<ModTarget*>(slider) != nullptr;
                    faders += drawnByItself ? 0 : 1;
                    ours += (drawnByItself || (dynamic_cast<SwooshLookAndFeel*>(&slider->getLookAndFeel()) != nullptr && slider->getTextBoxPosition() == juce::Slider::NoTextBox
                                               && (slider->getSliderStyle() == juce::Slider::LinearHorizontal || slider->getSliderStyle() == juce::Slider::LinearVertical))) ? 1 : 0;
                }
                for (auto* bar : bars)
                    ours += dynamic_cast<SwooshLookAndFeel*>(&bar->getLookAndFeel()) != nullptr ? 1 : 0;
                check(faders >= 4 && ours == static_cast<int>(sliders.size() + bars.size()),
                      "faders: all " + juce::String(faders) + " faders and " + juce::String(static_cast<int>(bars.size())) + " scrollbars are drawn in the house style" + at);
            }

            // The layout: the panels bolted to the window, in design pixels (the same at every scale).
            {
                using E = Editor;
                std::vector<WheelKnob*> knobs;
                collect(ed, knobs);
                const auto panels = ed.panelBounds();
                const juce::Rectangle<int> window(0, 0, E::c_width, E::c_height);
                int touching = 0, outside = 0;
                for (size_t i = 0; i < panels.size(); ++i)
                {
                    if (panels[i].second.isEmpty() || ! window.contains(panels[i].second))
                    {
                        ++outside;
                        std::cout << "         " << panels[i].first << " " << panels[i].second.toString() << " is not whole in the window" << std::endl;
                    }
                    for (size_t j = i + 1; j < panels.size(); ++j)
                        if (panels[i].second.intersects(panels[j].second))
                        {
                            ++touching;
                            std::cout << "         " << panels[i].first << " " << panels[i].second.toString() << " runs into " << panels[j].first << " "
                                      << panels[j].second.toString() << std::endl;
                        }
                }
                check(panels.size() == 10 && touching == 0 && outside == 0,
                      "layout: the " + juce::String(static_cast<int>(panels.size())) + " panels are whole in the window and none runs into another" + at);

                auto of = [&panels](const char* name)
                {
                    for (const auto& p : panels)
                        if (p.first == name)
                            return p.second;
                    return juce::Rectangle<int>();
                };
                const auto tracks = of("Tracks"), sea = of("Sea"), ride = of("Ride"), busA = of("Bus A"), busB = of("Bus B"), hits = of("Hits"),
                           output = of("Output"), mod = of("Modulation"), stow = of("Stowaway");
                const int gutter = Theme::Space::gutter;
                check(E::c_width >= 1480 && E::c_width <= 1560 && E::c_height == 924, "layout: a landscape window, " + juce::String(E::c_width) + " x " + juce::String(E::c_height));
                check(tracks.getY() == sea.getY() && tracks.getHeight() == sea.getHeight() && sea.getX() - tracks.getRight() == gutter
                          && tracks.getX() == gutter && sea.getRight() == E::c_width - gutter,
                      "layout: the tracks and the sea share the top row, a gutter apart, a gutter in from the window's edges" + at);
                const double seaShape = static_cast<double>(sea.getWidth()) / sea.getHeight();
                const double topShare = static_cast<double>(sea.getHeight()) / E::c_height;
                check(std::abs(seaShape - 1.8) < 0.02 && topShare >= 0.38 && topShare <= 0.44 && sea.getWidth() * sea.getHeight() > 3 * 352 * 196,
                      "layout: the sea is 1.8 : 1 (" + juce::String(seaShape, 2) + "), the top row is about two fifths of the window (" + juce::String(topShare * 100.0, 0)
                          + "%), and the sea is over three times the size it was" + at);
                check(ride.getY() == busA.getY() && busA.getY() == busB.getY() && ride.getHeight() == busA.getHeight() && busA.getHeight() == busB.getHeight()
                          && busA.getX() - ride.getRight() == gutter && busB.getX() - busA.getRight() == gutter && ride.getX() == gutter
                          && busB.getRight() == E::c_width - gutter && ride.getY() - tracks.getBottom() == gutter,
                      "layout: Ride, Bus A and Bus B are one row under the tracks, a gutter apart" + at);
                check(hits.getY() == output.getY() && hits.getHeight() == output.getHeight() && hits.getY() - busA.getBottom() == gutter
                          && output.getX() - hits.getRight() == gutter && hits.getX() == ride.getX() && hits.getRight() == busA.getRight()
                          && output.getX() == busB.getX() && output.getRight() == busB.getRight(),
                      "layout: Hits (under Ride and Bus A) and Output (under Bus B) are the next row, on the same seams" + at);
                check(mod.getY() - hits.getBottom() == gutter && stow.getX() == gutter && stow.getY() == mod.getY() && stow.getHeight() == mod.getHeight()
                          && mod.getX() - stow.getRight() == gutter && mod.getRight() == E::c_width - gutter && of("Footer").getY() >= mod.getBottom(),
                      "layout: Stowaway and Modulation share the bottom row, a gutter apart, with the hint line under them" + at);
                check(! proc.isStowawayOn() && proc.getStowawayPanel() == -1 && stow.getWidth() <= 200 && mod.getWidth() >= 1290,
                      "stowaway: in a new session its plate is folded (" + juce::String(stow.getWidth()) + " px wide) and Modulation has the rest of the row" + at);

                // The strips: what each section sends on, along the bottom of its panel; the ride's last 8 s in Ride.
                bool strips = true;
                for (size_t i = 0; i < ed.m_sectionScopes.size(); ++i)
                {
                    const auto strip = ed.m_sectionScopes[i]->getBounds();
                    const auto& panel = ed.m_sections[i].bounds;
                    strips = strips && panel.contains(strip) && strip.getWidth() >= panel.getWidth() - 32 && strip.getBottom() > panel.getBottom() - 12 && strip.getHeight() >= 14;
                    for (auto* k : knobs)
                        strips = strips && ! (k->getParentComponent() == &ed.m_content && k->getBounds().intersects(strip));
                }
                check(strips && ride.contains(ed.m_rideStripArea) && ed.m_rideStripArea.getWidth() > 200,
                      "strips: a scope strip the width of its panel along the bottom of Bus A, Bus B, Hits and Output, under the knobs; the ride's last 8 s in Ride" + at);

                // The five demo sounds: four whole lanes in view, never a half-cut one, and the fifth a scroll away;
                // the drop zone under them is one slim line.
                auto& hold = ed.m_hold;
                int lanes = 0, whole = 0, laneHeight = 0;
                for (auto* lane : hold.m_laneHolder.getChildren())
                    if (lane->isVisible())
                    {
                        ++lanes;
                        laneHeight = lane->getHeight();
                        whole += hold.m_viewport.getBounds().contains(hold.getLocalArea(lane, lane->getLocalBounds())) ? 1 : 0;
                    }
                check(lanes == 5 && whole == 4 && laneHeight >= 64 && hold.m_viewport.getHeight() == 4 * laneHeight
                          && hold.m_laneHolder.getHeight() == 5 * laneHeight,
                      "lanes: five tracks, four whole lanes in view (" + juce::String(laneHeight) + " high), the list exactly four lanes tall and the fifth a scroll away" + at);
                check(hold.m_dropZone.getHeight() > 0 && hold.m_dropZone.getHeight() <= 24 && hold.m_dropZone.getY() >= hold.m_viewport.getBottom()
                          && hold.getLocalBounds().contains(hold.m_dropZone),
                      "lanes: with tracks loaded the drop zone is one slim line under them (" + juce::String(hold.m_dropZone.getHeight()) + " px)" + at);

                // Everything in the header and the title bars sits whole in the window, and no two controls overlap.
                {
                    std::vector<juce::Component*> controls;
                    for (auto* child : ed.m_content.getChildren())
                        if (child->isVisible() && child != &ed.m_wires && child != &ed.m_hold && child != &ed.m_sea && child != &ed.m_morphPad
                            && child != &ed.m_modPanel && child != &ed.m_morphButton && ! child->getBounds().isEmpty()
                            && child->getBounds() != juce::Rectangle<int>(0, 0, E::c_width, E::c_height))
                            controls.push_back(child);
                    int clashes = 0, outOfWindow = 0;
                    for (size_t i = 0; i < controls.size(); ++i)
                    {
                        outOfWindow += window.contains(controls[i]->getBounds()) ? 0 : 1;
                        for (size_t j = i + 1; j < controls.size(); ++j)
                            if (controls[i]->getBounds().intersects(controls[j]->getBounds()))
                            {
                                ++clashes;
                                std::cout << "         " << controls[i]->getBounds().toString() << " and " << controls[j]->getBounds().toString() << " overlap" << std::endl;
                            }
                    }
                    check(controls.size() > 80 && clashes == 0 && outOfWindow == 0,
                          "layout: none of the window's " + juce::String(static_cast<int>(controls.size())) + " controls overlaps another or leaves the window" + at);
                }
            }

            if (scale == 1.0f)
            {
                // The first open: the design size where the screen has room, else the largest that is whole on it,
                // never under the smallest size (where the smallest text is still 8.5 px on screen).
                using E = Editor;
                const int smallest = juce::roundToInt(E::c_width * E::c_minScale);
                check(E::firstOpenWidth({ 0, 0, 1920, 1055 }) == E::c_width && E::firstOpenWidth({}) == E::c_width, "first open: the design size on a screen with room");
                const int laptop = E::firstOpenWidth({ 0, 25, 1440, 840 });
                check(laptop < E::c_width && laptop >= smallest && laptop <= 1440 - 16 && laptop * E::c_height / E::c_width <= 840 - 48,
                      "first open: on a 1440 x 900 laptop the window opens whole on the screen (" + juce::String(laptop) + " wide)");
                check(E::firstOpenWidth({ 0, 0, 1024, 700 }) == smallest && Theme::c_minTextSize * E::c_minScale >= 8.5f,
                      "first open: never under the smallest size, where the smallest text is " + juce::String(Theme::c_minTextSize * E::c_minScale, 1) + " px on screen");
            }

            if (scale == 1.0f)
            {
                auto& wires = ed.m_wires;
                auto* doppler = dynamic_cast<WheelKnob*>(ed.m_controls[ParamIDs::bus[0][ParamIDs::Doppler]]);
                auto* width = dynamic_cast<WheelKnob*>(ed.m_controls[ParamIDs::bus[0][ParamIDs::Width]]);

                // The caption and the value are always drawn, and the value is the control's own text.
                {
                    auto textsOf = [](WheelKnob& k)
                    {
                        UiProbe::Recorder recorder;
                        recorder.paint(k, 1.0f);
                        juce::StringArray texts;
                        for (const auto& t : recorder.texts)
                            texts.add(t.text);
                        return texts;
                    };
                    const auto before = textsOf(*width);
                    const bool shown = before.size() == 2 && before[0] == "Width" && before[1] == width->getTextFromValue(width->getValue()) && before[1].isNotEmpty();
                    width->setValue(width->getMinimum() + 0.25 * (width->getMaximum() - width->getMinimum()), juce::sendNotificationSync);
                    const auto after = textsOf(*width);
                    check(shown && after.size() == 2 && after[1] == width->getTextFromValue(width->getValue()) && after[1] != before[1],
                          "knobs: the caption and the value are always drawn (\"" + before.joinIntoString(" / ") + "\", then \"" + after.joinIntoString(" / ") + "\")");
                }

                // A value that changes on its own swings the wheel in on a spring; under the hand the wheel is where
                // the value is; let go while turning, it coasts on past the value and comes back to it.
                {
                    for (int i = 0; i < 400; ++i) width->timerCallback();
                    const float start = width->getShownPosition();
                    width->setValue(width->getMinimum() + 0.6 * (width->getMaximum() - width->getMinimum()), juce::sendNotificationSync);
                    const float target = static_cast<float>(width->valueToProportionOfLength(width->getValue()));
                    const bool lags = std::abs(width->getShownPosition() - start) < 1.0e-4f && std::abs(target - start) > 0.2f;
                    width->timerCallback();
                    const bool moving = std::abs(width->getShownPosition() - start) > 1.0e-4f && std::abs(width->getShownPosition() - target) > 0.05f;
                    for (int i = 0; i < 400; ++i) width->timerCallback();
                    check(lags && moving && std::abs(width->getShownPosition() - target) < 1.0e-4f, "wheel: a value that changes on its own swings in on the spring");

                    const auto centre = width->getDialCentre();
                    width->mouseDown(mouseAt(*width, centre, centre, false));
                    bool oneToOne = true;
                    float last = target;
                    for (int step = 1; step <= 6; ++step)
                    {
                        juce::Thread::sleep(4);
                        width->mouseDrag(mouseAt(*width, centre.translated(0.0f, -8.0f * static_cast<float>(step)), centre, true));
                        const float now = static_cast<float>(width->valueToProportionOfLength(width->getValue()));
                        oneToOne = oneToOne && now > last + 0.01f && std::abs(width->getShownPosition() - now) < 1.0e-6f;
                        last = now;
                    }
                    width->mouseUp(mouseAt(*width, centre.translated(0.0f, -48.0f), centre, true));
                    float furthest = 0.0f;
                    for (int i = 0; i < 400; ++i)
                    {
                        width->timerCallback();
                        furthest = juce::jmax(furthest, width->getShownPosition() - last);
                    }
                    const bool settled = std::abs(width->getShownPosition() - last) < 1.0e-4f;
                    check(oneToOne, "wheel: under the hand it is where the value is at every step of a drag, with no lag");
                    // An Alt-click (it belongs to typing a value) never turns the wheel, so it is not a hold: a value set
                    // after one, even with no mouse-up seen, still swings in on the spring.
                    {
                        for (int i = 0; i < 400; ++i) width->timerCallback();
                        const auto now = juce::Time::getCurrentTime();
                        const juce::MouseEvent alt(juce::Desktop::getInstance().getMainMouseSource(), centre,
                                                   juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier),
                                                   juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                                   juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                                   juce::MouseInputSource::defaultTiltY, width, width, now, centre, now, 1, false);
                        width->mouseDown(alt);
                        const float from = width->getShownPosition();
                        width->setValue(width->getMinimum() + 0.2 * (width->getMaximum() - width->getMinimum()), juce::sendNotificationSync);
                        const float to = static_cast<float>(width->valueToProportionOfLength(width->getValue()));
                        const bool sprung = std::abs(width->getShownPosition() - from) < 1.0e-4f && std::abs(to - from) > 0.2f;
                        width->mouseUp(alt);
                        for (int i = 0; i < 400; ++i) width->timerCallback();
                        check(sprung && std::abs(width->getShownPosition() - to) < 1.0e-4f, "wheel: an Alt-click or a right-click is not a hold (a value set after it swings in on the spring)");
                    }
                    check(furthest > 0.003f && furthest < 0.08f && settled,
                          "wheel: let go while turning, it coasts on (" + juce::String(furthest * 100.0f, 1) + "% of its travel) and settles on the value");
                }

                // Rings: one for each wire outside the value ring, in the modulator's colour; a negative depth is dashed.
                proc.getMods().connect(1, ParamIDs::bus[0][ParamIDs::Doppler], -0.4f);
                wires.rebuild();
                rig.run(0.1);
                {
                    const float imageScale = 3.0f;
                    const auto image = doppler->createComponentSnapshot(doppler->getLocalBounds(), false, imageScale);
                    const float ring = doppler->getRingRadius(), first = ring + 3.5f, second = first + 2.4f;
                    const float p = static_cast<float>(doppler->valueToProportionOfLength(doppler->getValue()));
                    const auto& arcs = doppler->m_modArcs;
                    bool rings = arcs.size() == 2;
                    float solid = 0.0f, dashed = 0.0f, inside = 0.0f;
                    if (rings)
                    {
                        auto range = [p](const ModTarget::ModArc& m)
                        {
                            const float lo = m.bipolar ? p - 0.5f * std::abs(m.depth) : juce::jmin(p, p + m.depth), hi = m.bipolar ? p + 0.5f * std::abs(m.depth) : juce::jmax(p, p + m.depth);
                            return std::pair<float, float>(juce::jmax(0.0f, lo), juce::jmin(1.0f, hi));
                        };
                        const auto r0 = range(arcs[0]), r1 = range(arcs[1]);
                        solid = ringCoverage(image, imageScale, doppler->getDialCentre(), first, r0.first, r0.second, Theme::modColour(0));
                        dashed = ringCoverage(image, imageScale, doppler->getDialCentre(), second, r1.first, r1.second, Theme::modColour(1));
                        inside = ringCoverage(image, imageScale, doppler->getDialCentre(), ring, r0.first, r0.second, Theme::modColour(0));
                    }
                    check(rings && solid > 0.8f && dashed > 0.25f && dashed < 0.8f && inside < 0.05f,
                          "rings: each wire is a ring outside the value ring in its modulator's colour (" + juce::String(solid, 2) + " of the first), dashed when its depth is negative ("
                              + juce::String(dashed, 2) + ")");
                }

                // Plugs on the rim, clear of the caption and the value; cables come in beside the text.
                {
                    int plugs = 0, onRim = 0, clear = 0;
                    const float plugRadius = 5.5f;
                    for (const auto& plug : wires.m_plugs)
                        if (auto* knob = dynamic_cast<WheelKnob*>(plug.knob))
                        {
                            ++plugs;
                            const auto local = knob->getLocalPoint(&wires, plug.pos);
                            const float d = local.getDistanceFrom(knob->getDialCentre());
                            onRim += std::abs(d - (knob->getRingRadius() + 3.0f)) < 0.6f ? 1 : 0;
                            const auto text = wires.getLocalArea(knob, knob->getCaptionArea().getUnion(knob->getValueArea()).getSmallestIntegerContainer()).toFloat();
                            bool off = plug.hasGate && plug.pos.y + plugRadius <= text.getY() && ! text.contains(plug.gate);
                            const auto jack = wires.getLocalPoint(wires.m_panel, wires.m_panel->getJackCentre(plug.route.source));
                            const auto path = wires.cablePath(jack, plug.pos, &plug.gate);
                            juce::PathFlatteningIterator it(path);
                            while (it.next())
                                off = off && ! text.reduced(1.0f).intersects(juce::Line<float>(it.x1, it.y1, it.x2, it.y2));
                            clear += off ? 1 : 0;
                        }
                    check(plugs == 3 && onRim == plugs && clear == plugs && wires.m_plugs[0].pos.getDistanceFrom(wires.m_plugs.back().pos) > 2.0f * plugRadius - 1.5f,
                          "plugs: all " + juce::String(plugs) + " sit on the wheel's rim, clear of the caption and the value, and their cables come in beside the text");
                }

                // The modulator under the mouse: its cables at full strength, the others faded.
                {
                    const auto* lilac = &wires.m_plugs.front(); // Mod 1 -> bus A's Doppler
                    for (const auto& plug : wires.m_plugs)
                        if (plug.route.source == 0)
                            lilac = &plug;
                    const auto jack = wires.getLocalPoint(wires.m_panel, wires.m_panel->getJackCentre(0));
                    const auto path = wires.cablePath(jack, lilac->pos, lilac->hasGate ? &lilac->gate : nullptr);
                    const auto mid = path.getPointAlongPath(path.getLength() * 0.3f);
                    auto strength = [&]
                    {
                        int most = 0;
                        for (int dx = -2; dx <= 2; ++dx)
                            for (int dy = -2; dy <= 2; ++dy)
                                most = juce::jmax(most, static_cast<int>(wires.m_cables.getPixelAt(juce::roundToInt(mid.x * wires.m_scale) + dx, juce::roundToInt(mid.y * wires.m_scale) + dy).getAlpha()));
                        return most;
                    };
                    const int plain = strength();
                    wires.setHoverCard(0);
                    const int own = strength();
                    const bool alphas = wires.cableAlpha(0) > 0.9f && wires.cableAlpha(1) < 0.3f;
                    wires.setHoverCard(2);
                    const int other = strength();
                    wires.setHoverCard(-1);
                    check(alphas && own > plain + 20 && other < plain - 80 && strength() == plain && wires.cableAlpha(0) == ModWires::c_cableAlpha,
                          "cables: the modulator under the mouse stands out (" + juce::String(own) + " of 255) and the others fade (" + juce::String(other) + "), from " + juce::String(plain));
                }

                // The still part of the wheels is kept: a few images, and drawing again makes no more.
                {
                    UiProbe::Recorder first;
                    first.paint(ed, 1.0f, scale);
                    const int kept = WheelKnob::keptLayerCount();
                    UiProbe::Recorder again;
                    again.paint(ed, 1.0f, scale);
                    check(kept >= 2 && kept <= 24 && WheelKnob::keptLayerCount() == kept, "wheel: the still part of all the knobs is " + juce::String(kept) + " kept images, and drawing again adds none");
                }

                // The meters and the Trim fader each have a number; Swell is the biggest thing in Ride; Throw is under its knob.
                {
                    ed.m_trim.setValue(-12.0, juce::sendNotificationSync);
                    const bool numbers = ed.m_grReadout.isNotEmpty() && ed.m_outReadout.isNotEmpty() && ed.m_trimReadout == "-12.0";
                    ed.m_trim.setValue(0.0, juce::sendNotificationSync);
                    check(numbers && ed.m_trimReadout == "0.0" && ed.m_trimValue.getWidth() >= 40 && ! ed.m_trimValue.intersects(ed.m_trim.getBounds()),
                          "meters: GR, Out and Trim each have a number under them (\"" + ed.m_grReadout + "\", \"" + ed.m_outReadout + "\", \"" + ed.m_trimReadout + "\")");
                    const auto swell = ed.m_swell.getBounds(), thrown = ed.m_throwButton.getBounds();
                    const auto length = ed.m_controls[ParamIDs::throwLength]->getBounds();
                    const auto autoRide = ed.m_rideButton.getBounds(), follow = ed.m_followMasterButton.getBounds();
                    check(swell.getWidth() >= 2 * WheelKnob::cellWidth(WheelKnob::Size::Secondary) && swell.getWidth() > WheelKnob::cellWidth(WheelKnob::Size::Primary)
                              && ed.m_performPanel.contains(swell) && ed.m_performPanel.contains(thrown)
                              && std::abs(thrown.getY() - length.getBottom()) <= 8 && thrown.getRight() == length.getRight()
                              && follow.getY() == autoRide.getY() && follow.getX() - autoRide.getRight() <= 8 && follow.getX() >= autoRide.getRight(),
                          "Ride: Swell is the biggest thing in it, Throw is right under its Throw Length knob, and Follow Master is beside Auto Ride");
                }
            }

            proc.getState().getParameter(ParamIDs::linkBuses)->setValueNotifyingHost(0.0f);
            proc.getMods().clear();

            // The panels that open over the window (each checked on its own: the window under it is not its text).
            ed.toggleLog();
            rig.run(0.5);
            ed.toggleLog();
            proc.timerCallback();
            ed.m_settings.refresh();
            ed.m_settings.setVisible(true);
            checkTexts("Settings," + at, ed.m_settings, scale, 1.0f);
            ed.m_settings.setVisible(false);
            ed.showTakes();
            rig.run(0.1);
            checkTexts("Takes," + at, ed.m_takes, scale, 1.0f);
            ed.m_takes.setVisible(false);
            ed.showVariations();
            rig.run(0.1);
            checkTexts("Render Variations," + at, ed.m_variations, scale, 1.0f);
            ed.m_variations.setVisible(false);
            ed.m_about.setVisible(true);
            checkTexts("About," + at, ed.m_about, scale, 1.0f);
            {
                // The version line names the version, and nothing else: an editor made without a wrapper (this test,
                // the picture tool) has no format to name, and used to print the word "Undefined" beside the number.
                UiProbe::Recorder about;
                about.paint(ed.m_about, scale, 1.0f);
                bool named = false, undefined = false;
                for (const auto& t : about.texts)
                {
                    named |= t.text.startsWith("Version ");
                    undefined |= t.text.contains("Undefined");
                }
                check(named && ! undefined, "About," + at + ": the version line names the version and nothing that isn't a format");
            }
            ed.m_about.setVisible(false);
            sammyj::UpdateInfo info;
            info.pluginId = "swooshbuckler";
            info.currentVersion = "1.0.0";
            info.latestVersion = "10.10.10";
            ed.showUpdatePopup(info);
            checkTexts("the update card," + at, ed.m_updatePopup, scale, 1.0f);
            ed.m_updatePopup.setVisible(false);

            // The morph pad over the sea.
            proc.getState().getParameter(ParamIDs::morphOn)->setValueNotifyingHost(1.0f);
            ed.m_morphButton.setToggleState(true, juce::dontSendNotification);
            ed.syncMorphPad();
            rig.run(0.3);
            checkTexts("the morph pad open," + at, ed, 1.0f, scale);
            proc.getState().getParameter(ParamIDs::morphOn)->setValueNotifyingHost(0.0f);
            proc.setPlaying(false);
        }

        {
            // Fewer than four tracks: the drop line stays directly under the last lane. (Given the whole of the
            // room the four lanes would have taken, it floated at the panel's foot with a hole above it.)
            Rig rig(proc, 1.0f);
            rig.open();
            auto& ed = *rig.editor;
            auto& hold = ed.m_hold;
            for (int track = 1; track < CargoHold::c_numTracks; ++track)
                proc.getCargo().clear(track);
            hold.changeListenerCallback(nullptr);
            hold.resized();
            int lanes = 0, laneHeight = 0;
            for (auto* lane : hold.m_laneHolder.getChildren())
                if (lane->isVisible())
                {
                    ++lanes;
                    laneHeight = lane->getHeight();
                }
            const int hole = hold.m_dropZone.getY() - hold.m_viewport.getBottom();
            check(lanes == 1 && hold.m_viewport.getHeight() == laneHeight && hole >= 0 && hole <= 8 && hold.m_dropZone.getHeight() <= 24
                      && hold.getLocalBounds().contains(hold.m_dropZone),
                  "lanes: with one track the drop line sits right under it (" + juce::String(hole) + " px below the lane), not adrift at the panel's foot");
        }
    }

    // Stowaway's plate -------------------------------------------------------------------------------------------
    std::cout << "Stowaway's plate" << std::endl;
    {
        using E = Editor;
        auto base = makeProcessor();
        auto& proc = *dynamic_cast<Proc*>(base.get());
        proc.loadDemoSounds();
        Rig rig(proc, 1.0f);
        rig.open();
        auto& ed = *rig.editor;
        proc.setPlaying(true);
        rig.run(0.3);
        auto knob = [&ed](int k) { return dynamic_cast<WheelKnob*>(ed.m_controls[ParamIDs::stow[k]]); };
        auto shownKnobs = [&]
        {
            int n = 0;
            for (int k = 0; k < ParamIDs::numStowControls; ++k)
                n += knob(k)->isVisible() ? 1 : 0;
            return n;
        };
        // Nothing that is showing may overlap anything else, stand outside the window, or (on the bottom row) stand
        // outside its own plate. Returns how many faults, and prints them.
        auto faults = [&](const juce::String& state)
        {
            std::vector<juce::Component*> controls;
            for (auto* child : ed.m_content.getChildren())
                if (child->isVisible() && child != &ed.m_wires && child != &ed.m_hold && child != &ed.m_sea && child != &ed.m_morphPad
                    && child != &ed.m_morphButton && ! child->getBounds().isEmpty()
                    && child->getBounds() != juce::Rectangle<int>(0, 0, E::c_width, E::c_height))
                    controls.push_back(child);
            int found = 0;
            const juce::Rectangle<int> window(0, 0, E::c_width, E::c_height);
            const auto stow = ed.m_stowPanel, mod = ed.m_modSection;
            for (size_t i = 0; i < controls.size(); ++i)
            {
                const auto b = controls[i]->getBounds();
                bool bad = ! window.contains(b);
                if (b.getY() >= stow.getY() && b.getBottom() <= stow.getBottom())
                    bad = bad || ! (stow.contains(b) || mod.contains(b));
                for (size_t j = i + 1; j < controls.size(); ++j)
                    bad = bad || b.intersects(controls[j]->getBounds());
                if (bad)
                {
                    ++found;
                    std::cout << "         " << state << ": " << b.toString() << " overlaps something or is out of its plate" << std::endl;
                }
            }
            found += stow.intersects(mod) || mod.getX() - stow.getRight() != Theme::Space::gutter || mod.getRight() != E::c_width - Theme::Space::gutter ? 1 : 0;
            return found;
        };
        auto settle = [&](float open)
        {
            ed.m_stowOpen.snap(open);
            ed.layoutBottomRow();
        };

        // Off, and folded: the switch, the Mode, the chevron and a window that says Off; no wheels, no dice.
        const int foldedW = ed.m_stowPanel.getWidth();
        check(! ed.m_stowOn.getToggleState() && ed.m_stowOn.isVisible() && ed.m_stowMode.isVisible() && ed.m_stowFold.isVisible() && shownKnobs() == 0
                  && ! ed.m_sectionDice[Presets::Stowaway]->isVisible() && ! ed.m_sectionLocks[Presets::Stowaway]->isVisible() && ! ed.m_stowScope->isVisible()
                  && ed.m_stowPanel.contains(ed.m_stowOn.getBounds()) && ed.m_stowPanel.contains(ed.m_stowMode.getBounds())
                  && ed.m_stowPanel.contains(ed.m_stowFold.getBounds()) && ed.m_stowPanel.contains(ed.m_stowStrip) && faults("off, folded") == 0,
              "plate: off and folded it is its nameplate, its switch, its Mode, its chevron and its scope's window, each whole in the plate");
        check(Theme::getLook(ed.m_stowMode) == Theme::Look::Resting && Theme::getLook(*knob(ParamIDs::StowFollow)) == Theme::Look::Resting
                  && knob(ParamIDs::StowFollow)->getTooltip().contains("Stowaway is off"),
              "plate: off, its Mode and its wheels rest (they can still be set), and a wheel's tooltip says Stowaway is off");
        int paintedOff = 0;
        {
            UiProbe::Recorder recorder;
            recorder.paint(ed, 1.0f, 1.0f);
            for (const auto& t : recorder.texts)
                paintedOff += t.text == "Off" && ed.m_stowStrip.toFloat().expanded(2.0f).contains(t.box) ? 1 : 0;
        }
        check(paintedOff == 1, "plate: off, the scope's window on the plate reads Off");
        check(ed.m_stowOn.getButtonText() == "Off", "plate: off, the switch says Off in a word beside it");

        // Switched on for the first time: the plate opens by itself, and the wheels are there.
        proc.setStowawayOn(true);
        rig.run(0.1);
        check(proc.getStowawayPanel() == 1 && ed.m_stowOpen.target() == 1.0f && ed.m_stowOn.getToggleState(),
              "plate: switching Stowaway on for the first time opens its plate");
        check(ed.m_stowOn.getButtonText() == "On", "plate: on, the switch says On");
        settle(1.0f);
        rig.run(0.1);
        bool onPitch = true, inside = true;
        for (int k = 0; k < ParamIDs::numStowControls; ++k)
        {
            onPitch = onPitch && knob(k)->getX() - knob(0)->getX() == 64 * k && knob(k)->getWidth() == 64 && knob(k)->getSize() == WheelKnob::Size::Secondary;
            inside = inside && ed.m_stowPanel.contains(knob(k)->getBounds()) && ! knob(k)->getBounds().intersects(ed.m_stowStrip);
        }
        check(ed.m_stowPanel.getWidth() > foldedW + 150 && shownKnobs() == 5 && onPitch && inside && ed.m_stowScope->isVisible()
                  && ed.m_sectionDice[Presets::Stowaway]->isVisible() && ed.m_sectionLocks[Presets::Stowaway]->isVisible() && faults("on, open") == 0,
              "plate: open it shows its five wheels on the shared column pitch (64 px), its scope, its die and its lock, and Modulation gives up the width ("
                  + juce::String(ed.m_stowPanel.getWidth()) + " px, Modulation " + juce::String(ed.m_modSection.getWidth()) + ")");
        check(Theme::getLook(ed.m_stowMode) == Theme::Look::Normal && Theme::getLook(*knob(ParamIDs::StowFollow)) == Theme::Look::Normal
                  && Theme::getLook(*knob(ParamIDs::StowWidth)) == Theme::Look::Resting && knob(ParamIDs::StowWidth)->getTooltip().contains("Not used in Sub"),
              "plate: on, in Sub, Width rests and its tooltip says why; the others are live");
        // The modulators still fit their cards at the narrower width.
        {
            const int texts = checkTexts("the window with Stowaway open", ed, 1.0f, 1.0f);
            check(texts > 120, "plate: every text in the window still fits with the plate open (" + juce::String(texts) + " texts)");
        }
        // Each Mode: which wheels rest, and what a wheel's tooltip says.
        bool rests = true, tips = true;
        for (int mode = 0; mode < ParamIDs::numStowModes; ++mode)
        {
            auto* param = proc.getState().getParameter(ParamIDs::stowMode);
            param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(mode)));
            rig.run(0.1);
            for (int k = 0; k < ParamIDs::numStowControls; ++k)
            {
                rests = rests && (Theme::getLook(*knob(k)) == Theme::Look::Resting) == ! Params::stowControlInUse(mode, k);
                tips = tips && knob(k)->getTooltip().startsWith(juce::String("Stowaway: ") + ParamIDs::stowNames[k] + "\nIn " + ParamIDs::stowModeNames[mode] + ": ");
            }
        }
        check(rests && tips, "plate: in every Mode the wheels that do nothing rest, and each wheel's tooltip says what it does in that Mode");

        // On the way open and shut nothing stands outside the plate or on a neighbour.
        int moving = 0;
        for (const float open : { 0.1f, 0.3f, 0.49f, 0.5f, 0.6f, 0.75f, 0.9f, 0.99f })
        {
            settle(open);
            moving += faults("at " + juce::String(open, 2) + " open");
        }
        check(moving == 0, "plate: on its way open and shut, nothing overlaps, leaves the plate or runs into Modulation");
        settle(1.0f);

        // Folding: the chevron and the nameplate both do it; it is remembered, and switching on again leaves it folded.
        ed.m_stowFold.onClick();
        rig.run(0.1);
        const bool foldedByChevron = proc.getStowawayPanel() == 0 && ed.m_stowOpen.target() == 0.0f;
        settle(0.0f);
        check(foldedByChevron && shownKnobs() == 0 && ed.m_stowPanel.getWidth() == foldedW && ed.m_stowScope->isVisible() && faults("on, folded") == 0,
              "plate: the chevron folds it: the wheels go, the switch, the Mode and the scope stay, and Modulation takes the width back");
        ed.m_content.mouseDown(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), ed.m_stowNameplate.getCentre().toFloat(), {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                &ed.m_content, &ed.m_content, {}, ed.m_stowNameplate.getCentre().toFloat(), {}, 1, false));
        const bool openedByName = proc.getStowawayPanel() == 1;
        ed.m_stowFold.onClick();
        proc.setStowawayOn(false);
        rig.run(0.1);
        proc.setStowawayOn(true);
        rig.run(0.1);
        check(openedByName && proc.getStowawayPanel() == 0, "plate: the nameplate opens and folds it too, and a plate folded by hand stays folded when Stowaway is switched on again");

        // The lane: sending a track to Stowaway from its bus menu while it is off switches it on, as one undo step.
        proc.setStowawayOn(false);
        rig.run(0.1);
        Component* firstLane = nullptr;
        for (auto* lane : ed.m_hold.m_laneHolder.getChildren())
            if (lane->isVisible() && firstLane == nullptr)
                firstLane = lane;
        juce::String said;
        ed.m_hold.onStatus = [&said](const juce::String& s) { said = s; };
        check(firstLane != nullptr && ed.m_hold.applyBusChoice(0, CargoHold::c_stowBus) && proc.isStowawayOn() && proc.getCargo().getBus(0) == CargoHold::c_stowBus
                  && said.contains("Stowaway is switched on"),
              "lane: choosing Stowaway in a track's bus menu while it is off switches it on, and the status line says so (\"" + said + "\")");
        juce::String what;
        check(proc.undoEdit(&what) && what == "Track Bus" && ! proc.isStowawayOn() && proc.getCargo().getBusOverride(0) != CargoHold::c_stowBus,
              "lane: and one Undo takes back both the move and the switch");
        // By hand while it stays off (the processor's own call does not switch it on): the tag rests and its tooltip says so.
        proc.setTrackBus(0, CargoHold::c_stowBus);
        ed.m_hold.historyStepped();
        rig.run(0.1);
        check(! proc.isStowawayOn() && ed.m_hold.busTagTooltip(0).contains("Stowaway, which is off"),
              "lane: a track on Stowaway while it is off waits, and its tag's tooltip says Stowaway is off");
        // And the lane rests, so it is plain why it is silent: with the rig playing, nothing in it moves (its ride
        // line lies still, no playhead runs) and its colour is dimmed. Switched on, it is lit and rides again.
        {
            auto picture = [&] { return firstLane->createComponentSnapshot(firstLane->getLocalBounds(), false, 1.0f); };
            auto differ = [](const juce::Image& a, const juce::Image& b)
            {
                int count = 0;
                for (int y = 0; y < a.getHeight(); ++y)
                    for (int x = 0; x < a.getWidth(); ++x)
                        count += a.getPixelAt(x, y) != b.getPixelAt(x, y) ? 1 : 0;
                return count;
            };
            // How much of the lane is in a colour: pixels as lit as the lane's own shade (well above the plates).
            auto lit = [&](const juce::Image& image, juce::Colour shade)
            {
                int count = 0;
                for (int y = 0; y < image.getHeight(); ++y)
                    for (int x = 0; x < image.getWidth(); ++x)
                        count += distance(image.getPixelAt(x, y), shade) < 0.08 ? 1 : 0;
                return count;
            };
            const bool wasPlaying = proc.isPlaying();
            proc.setPlaying(true);
            rig.run(0.5);
            const auto shade = ed.m_hold.laneColour(0);
            const auto restA = picture();
            rig.run(0.7);
            const auto restB = picture();
            const int restMoved = differ(restA, restB), restLit = lit(restB, shade);
            proc.setStowawayOn(true);
            rig.run(0.5);
            const auto onA = picture();
            rig.run(0.7);
            const auto onB = picture();
            const int onMoved = differ(onA, onB), onLit = lit(onB, shade);
            proc.setStowawayOn(false);
            rig.run(0.3);
            proc.setPlaying(wasPlaying);
            const auto dim = Theme::laneResting(shade);
            std::cout << "         a lane on Stowaway, the rig playing: off, " << restMoved << " pixels change in 0.7 s and " << restLit << " are in its colour; on, " << onMoved
                      << " change and " << onLit << " are in its colour. Its colour at rest is " << juce::String(contrast(dim, TC::raised), 2) << ":1 on the plate (lit: "
                      << juce::String(contrast(shade, TC::raised), 2) << ":1)" << std::endl;
            check(ed.m_hold.trackWaits(0) && restMoved == 0 && onMoved > 50, "lane: a track waiting on a switched-off Stowaway lies still (no ride line moving, no playhead), and moves again once Stowaway is on");
            check(restLit == 0 && onLit > 50 && contrast(dim, TC::raised) < 0.5 * contrast(shade, TC::raised) && contrast(dim, TC::raised) >= 1.3,
                  "lane: waiting, its colour is dimmed (still told from the plate), and lit again once Stowaway is on");
        }
        proc.setTrackBus(0, -1);

        // Presets never move a track. One that switches Stowaway on while nothing is sent to it says so (and how to
        // put something there); with a track there already, or a rumble for a sub, it has nothing to say.
        {
            auto preset = [](const char* name) -> const Presets::Preset&
            {
                return *std::find_if(Presets::factory().begin(), Presets::factory().end(), [name](const Presets::Preset& p) { return p.name == name; });
            };
            auto choose = [&](const char* name)
            {
                ed.m_toast.clear();
                const auto& p = preset(name);
                ed.applyLook(p, Presets::Unlisted::Default, p.name, p.name);
                rig.run(0.1);
                ed.m_morph.finish();
                return ed.m_toast;
            };
            // (The demo sounds have a rumble, which a sub takes by itself: it is called a bed while "nothing there" is tried.)
            const int roleBefore = proc.getCargo().getRoleOverride(0);
            std::vector<std::pair<int, int>> rumbles;
            for (int t = 0; t < ParamIDs::numCargoTracks; ++t)
                if (proc.getCargo().get(t) != nullptr && proc.getCargo().getRole(t) == CargoRole::Rumble)
                {
                    rumbles.emplace_back(t, proc.getCargo().getRoleOverride(t));
                    proc.getCargo().setRoleOverride(t, static_cast<int>(CargoRole::Bed));
                }
            const auto dryEmpty = choose("Dry Rider"), subEmpty = choose("Sub Under Jets"), plain = choose("Jet Passes");
            for (const auto& [t, role] : rumbles)
                proc.getCargo().setRoleOverride(t, role);
            const bool fits = Theme::textWidth(ed.m_fonts->body(), dryEmpty) + 30.0f <= 560.0f && Theme::textWidth(ed.m_fonts->body(), subEmpty) + 30.0f <= 560.0f;
            proc.setTrackBus(0, CargoHold::c_stowBus);
            const auto dryFed = choose("Dry Rider"), flyFed = choose("Three Movers");
            proc.setTrackBus(0, -1);
            proc.getCargo().setRoleOverride(0, static_cast<int>(CargoRole::Rumble));
            const auto subFed = choose("Sub Under Jets"), wakeRumble = choose("Debris Wake");
            proc.getCargo().setRoleOverride(0, roleBefore);
            check(dryEmpty.contains("nothing on it") && dryEmpty.contains("bus tag") && subEmpty.contains("nothing on it") && subEmpty.contains("Rumble") && fits,
                  "presets: one that switches Stowaway on with nothing sent to it says so, and how to put a track there (\"" + dryEmpty + "\"; \"" + subEmpty + "\")");
            check(plain.isEmpty() && dryFed.isEmpty() && flyFed.isEmpty() && subFed.isEmpty() && wakeRumble.contains("nothing on it"),
                  "presets: with a track on Stowaway (or a rumble, for a sub) it has nothing to say; a rumble on Auto does not count for the other modes, where it plays on bus A");
            const Presets::Preset defaults { "Default", {} };
            ed.applyLook(defaults, Presets::Unlisted::Default, "Back to Defaults", "Default");
            rig.run(0.1);
            ed.m_morph.finish();
            proc.setStowawayOn(false);
            ed.m_toast.clear();
            rig.run(0.2);
        }
    }

    // The sea ---------------------------------------------------------------------------------------------------
    std::cout << "The sea" << std::endl;
    {
        auto base = makeProcessor();
        auto& proc = *dynamic_cast<Proc*>(base.get());
        proc.loadDemoSounds();
        Rig rig(proc, 1.0f);
        rig.open();
        auto& ed = *rig.editor;
        auto& sea = ed.m_sea;
        proc.setPlaying(true);
        rig.run(1.0);

        // Stowaway in the sea: nothing while it is off; on, it shows in the Mode it is in and in no other.
        {
            proc.getCargo().setBusOverride(1, CargoHold::c_stowBus); // the wind bed
            rig.run(1.5);
            bool nothing = sea.m_wakeTrail.empty() && sea.m_stowTrails[0].empty() && sea.m_stowTrails[1].empty() && sea.m_stowLabelAlpha == 0.0f;
            for (int mode = 0; mode < ParamIDs::numStowModes; ++mode)
                nothing = nothing && sea.stowawayShown(mode) == 0.0f;
            check(nothing, "sea: with Stowaway off (a track waiting on it), the sea shows nothing of it");
            proc.setStowawayOn(true);
            auto* modeParam = proc.getState().getParameter(ParamIDs::stowMode);
            bool onlyItsMode = true, subLifts = false, wakeTrails = false, flyComet = false, dryLantern = false, named = true;
            for (int mode = 0; mode < ParamIDs::numStowModes; ++mode)
            {
                modeParam->setValueNotifyingHost(modeParam->convertTo0to1(static_cast<float>(mode)));
                float most = 0.0f;
                size_t longestWake = 0, longestTrail = 0;
                bool labelled = false;
                for (int i = 0; i < 40; ++i)
                {
                    rig.run(0.15);
                    UiProbe::Recorder recorder;
                    recorder.paint(ed.m_sea, 1.0f, 1.0f);
                    most = juce::jmax(most, sea.stowawayShown(mode));
                    longestWake = juce::jmax(longestWake, sea.m_wakeTrail.size());
                    if (i >= 20) // (the comet of the Mode before takes a moment to burn out)
                        longestTrail = juce::jmax(longestTrail, sea.m_stowTrails[0].size(), sea.m_stowTrails[1].size());
                    for (const auto& t : recorder.texts)
                        labelled = labelled || t.text.startsWith("Stowaway");
                }
                for (int other = 0; other < ParamIDs::numStowModes; ++other)
                    if (other != mode)
                        onlyItsMode = onlyItsMode && sea.stowawayShown(other) < 0.02f;
                std::cout << "  " << ParamIDs::stowModeNames[mode] << ": shown up to " << most << ", wake trail " << longestWake << ", own comet trail " << longestTrail
                          << (labelled ? ", named" : ", not named") << std::endl;
                named = named && labelled;
                if (mode == ParamIDs::StowSub)  subLifts = most > 0.02f && longestTrail == 0;
                if (mode == ParamIDs::StowWake) wakeTrails = most > 0.02f && longestWake > 8 && longestTrail == 0;
                if (mode == ParamIDs::StowFly)  flyComet = longestTrail > 8;
                if (mode == ParamIDs::StowDry)  dryLantern = most > 0.02f && longestTrail == 0;
            }
            check(subLifts, "sea: a Sub lifts the swell with its level (and is no comet)");
            check(wakeTrails, "sea: a Wake lays a trail on the water behind bus A's comet");
            check(flyComet, "sea: Fly is a comet of its own");
            check(dryLantern, "sea: Dry shows its level as a lantern on the water (and is no comet)");
            check(onlyItsMode && named, "sea: it shows in the Mode it is in and no other, with its name (\"" + sea.m_busLabels[SeaVisualiser::c_stowLabel] + "\")");
            check(sea.m_busLabels[SeaVisualiser::c_stowLabel].startsWith("Stowaway") && sea.m_busLabels[SeaVisualiser::c_stowLabel].contains("Energy Bed"),
                  "sea: its label is Stowaway and, with one track on it, that track's name");
            proc.setStowawayOn(false);
            rig.run(3.0);
            {
                UiProbe::Recorder recorder;
                recorder.paint(ed.m_sea, 1.0f, 1.0f);
            }
            bool gone = sea.m_stowLabelAlpha == 0.0f && sea.m_stowTrails[0].empty() && sea.m_stowTrails[1].empty() && sea.m_wakeTrail.empty();
            for (int mode = 0; mode < ParamIDs::numStowModes; ++mode)
                gone = gone && sea.stowawayShown(mode) < 0.02f;
            check(gone, "sea: switched off, it leaves the sea");
            proc.getCargo().setBusOverride(1, -1);
            auto* reset = proc.getState().getParameter(ParamIDs::stowMode);
            reset->setValueNotifyingHost(reset->getDefaultValue());
            rig.run(0.5);
        }

        // Comets: every flight belongs to a bus or to the throws, and none reaches past the end of the labels.
        {
            int worst = -1;
            for (int flight = 0; flight < RigEngine::c_maxFlights; ++flight)
                worst = juce::jmax(worst, SeaVisualiser::cometGroup(flight));
            check(worst == RigEngine::c_maxBuses && worst < SeaVisualiser::c_cometGroups,
                  "sea: every comet belongs to a bus or to the throws (the highest is " + juce::String(worst) + " of "
                      + juce::String(SeaVisualiser::c_cometGroups) + ", so a thrown whoosh cannot label past the end)");
        }

        // The mark in the stars: brighter stars taken off the logo's own outline, joined into closed shapes.
        {
            const auto outline = sea.m_logo->createOutlinePath();
            float furthest = 0.0f;
            for (const auto& s : sea.m_constellation)
            {
                juce::Point<float> on;
                outline.getNearestPoint(s.at, on);
                furthest = juce::jmax(furthest, on.getDistanceFrom(s.at));
            }
            std::set<int> joined;
            for (const auto& line : sea.m_constellationLines)
            {
                joined.insert(line.first);
                joined.insert(line.second);
            }
            check(sea.m_constellation.size() >= 12 && sea.m_constellationLines.size() >= sea.m_constellation.size()
                      && joined.size() == sea.m_constellation.size() && furthest < 0.01f,
                  "sea: the constellation is " + juce::String((int) sea.m_constellation.size()) + " stars on the mark's outline (the furthest "
                      + juce::String(furthest, 4) + " of a unit off it), every one joined by a line");
        }

        // It shows in open sky: clear of the moon, of the panel's name and of the Morph button, and above the water.
        {
            const auto area = sea.constellationArea();
            const auto morph = sea.getLocalArea(&ed.m_morphButton, ed.m_morphButton.getLocalBounds()).toFloat();
            const auto title = juce::Rectangle<float>(20.0f, 8.0f, 72.0f, 16.0f);
            const auto moon = juce::Rectangle<float>(92.0f, 92.0f).withCentre({ sea.getWidth() * 0.26f, sea.getHeight() * 0.30f * 0.52f });
            check(! area.intersects(morph) && ! area.intersects(title) && ! area.intersects(moon)
                      && area.getBottom() <= sea.getHeight() * 0.30f + 2.0f && sea.getLocalBounds().toFloat().contains(area),
                  "sea: the constellation sits in open sky, clear of the moon, the name and the Morph button");
        }

        // A take put away shows it; it fades away again on its own.
        {
            sea.m_constellationLeft = 0.0f;
            sea.m_constellationOpacity = 0.0f;
            sea.m_constellationHeld = false;
            ed.toggleLog();
            rig.run(0.4);
            ed.toggleLog();           // a take is written
            proc.timerCallback();
            rig.run(0.6);
            const float afterTake = sea.m_constellationOpacity;
            sea.m_constellationLeft = 0.0f;
            rig.run(8.0);
            check(afterTake > 0.05f && sea.m_constellationOpacity < afterTake * 0.5f,
                  "sea: saving a take brings the mark out (" + juce::String(afterTake, 2) + "), and it fades away again ("
                      + juce::String(sea.m_constellationOpacity, 2) + ")");
        }

        // A label never sits on the name, the recording lamp or the Morph button, nor on another label.
        {
            const juce::Rectangle<float> keepOut[] = { { 20.0f, 8.0f, 72.0f, 16.0f }, { 94.0f, 8.0f, 44.0f, 16.0f },
                                                       { 528.0f, 12.0f, 100.0f, 22.0f } };
            juce::Rectangle<float> placed[3];
            int numPlaced = 0;
            bool clear = true;
            // Three labels all wanting the same spot, right under the Morph button.
            for (int i = 0; i < 3; ++i)
            {
                const auto want = juce::Rectangle<float>(100.0f, 14.0f).withCentre({ 560.0f, 20.0f });
                const auto got = SeaVisualiser::clearOf(want, keepOut, 3, placed, numPlaced);
                for (const auto& k : keepOut)
                    clear = clear && ! got.intersects(k);
                for (int p = 0; p < numPlaced; ++p)
                    clear = clear && ! got.intersects(placed[p]);
                placed[numPlaced++] = got;
            }
            check(clear, "sea: comet labels move down off the name, the recording lamp and the Morph button, and off each other");
        }

        // The morph pad is a dock over a corner of the sea, not a lid on it.
        {
            ed.m_morphButton.setToggleState(true, juce::dontSendNotification);
            ed.syncMorphPad();
            rig.run(0.2);
            const auto dock = sea.getLocalArea(&ed.m_morphPad, ed.m_morphPad.getLocalBounds());
            const double covered = (double) dock.getWidth() * dock.getHeight() / ((double) sea.getWidth() * sea.getHeight());
            check(sea.getLocalBounds().contains(dock) && covered < 0.45 && dock.getBottom() > sea.getHeight() * 0.6
                      && dock.getRight() > sea.getWidth() * 0.6 && ! sea.m_dock.isEmpty(),
                  "sea: the morph pad docks over the lower right and leaves " + juce::String(juce::roundToInt(100.0 - covered * 100.0))
                      + "% of the sea in view, dimmed behind it");
            ed.m_morphButton.setToggleState(false, juce::dontSendNotification);
            ed.syncMorphPad();
        }

        // A rope is tied under its handle's plate: it is drawn nowhere on the plate, so the letter and the name
        // on it stay readable. The window is painted with the rope layer and without it, and the two must be the
        // same inside both plates - and different somewhere else, or there was no rope to begin with.
        {
            ed.m_morphButton.setToggleState(true, juce::dontSendNotification);
            ed.syncMorphPad();
            proc.setMorphSource(0, 0); // the A side roped to the first track
            ed.syncMorphPad();
            ed.m_morphPad.ropesMoved();
            rig.run(0.2);

            auto* layer = ed.m_morphPad.ropeLayer();
            juce::Image roped, bare;
            {
                UiProbe::Recorder shot;
                roped = shot.paint(ed, 1.0f);
                if (layer != nullptr)
                    layer->setVisible(false);
                bare = shot.paint(ed, 1.0f);
                if (layer != nullptr)
                    layer->setVisible(true);
            }

            const auto differs = [&roped, &bare](juce::Rectangle<int> area)
            {
                int n = 0;
                for (int y = area.getY(); y < area.getBottom(); ++y)
                    for (int x = area.getX(); x < area.getRight(); ++x)
                        if (roped.getPixelAt(x, y) != bare.getPixelAt(x, y))
                            ++n;
                return n;
            };

            int onPlates = 0;
            for (int slot = 0; slot < 2; ++slot)
                onPlates += differs(ed.getLocalArea(&ed.m_morphPad, ed.m_morphPad.handlePlate(slot)).getSmallestIntegerContainer()
                                        .getIntersection(ed.getLocalBounds()));
            const int anywhere = differs(ed.getLocalBounds());

            check(layer != nullptr && layer->isVisible() && anywhere > 200 && onPlates == 0,
                  "morph: the rope is tied under its handle's plate (" + juce::String(anywhere) + " pixels of rope in the window, "
                      + juce::String(onPlates) + " of them on a handle)");

            proc.setMorphSource(0, -1);
            ed.syncMorphPad();
            ed.m_morphPad.ropesMoved();
            ed.m_morphButton.setToggleState(false, juce::dontSendNotification);
            ed.syncMorphPad();
            rig.run(0.2);
        }

        // The sea keeps time. Busy it steps on every frame, quiet it steps a third as often - but either way the
        // same number of frames must carry it the same distance, or the swell rolls fast whenever nothing plays.
        {
            proc.setPlaying(true);
            rig.run(1.0);
            const double busyFrom = sea.m_time;
            rig.run(4.0);
            const double busy = sea.m_time - busyFrom;
            const bool wasBusy = sea.m_quietSeconds < 0.5f;

            proc.setPlaying(false);
            rig.run(9.0);                       // the tails run out, and five seconds of quiet pass
            const bool wentQuiet = sea.m_quietSeconds > 5.0f;
            const double idleFrom = sea.m_time;
            rig.run(4.0);
            const double idle = sea.m_time - idleFrom;

            check(wasBusy && wentQuiet && busy > 1.0 && std::abs(idle - busy) < busy * 0.05,
                  "sea: the idle rate steps less often without running fast (the same frames move it "
                      + juce::String(busy, 2) + " s busy and " + juce::String(idle, 2) + " s quiet)");
            proc.setPlaying(false);
        }
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
