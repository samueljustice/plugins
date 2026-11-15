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
//   - The room: at Stereo the strip under the sea and the Flight Paths rest, saying why, and nothing else of the room
//     is drawn; at 5.1, Quad and 7.1.4 the right controls light, Output shows a meter per speaker in the room's order
//     with every name whole under it, the sea draws the plan with its lamps lit, a pass's dot keeps to its Flight Path
//     and its comet stays calm; a modulator on Spread moves the sound; the new colours read.
//   - Faders and meters: no slider or scrollbar is drawn by JUCE's own look; the meters and the Trim fader have a
//     number under them; Swell is the biggest thing in its panel and Throw sits under it.
//
// Usage: SwooshbucklerUi [scratch folder] (a folder in the temp directory if none is given). Nothing is read from
// or written to the user's settings, and nothing goes on line.

#include "UiRig.h"
#include "UiProbe.h"
#include "DemoSounds.h"
#include "Parameters.h"

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <tuple>
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
                  "keys: a key given to another action is taken off the one that had it, and Reset to Defaults puts them all back");
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

            // It starts empty: the drop zone is all there is, with no demo sounds offered anywhere on screen.
            {
                int demoMentions = 0;
                std::function<void(juce::Component&)> walk = [&](juce::Component& c)
                {
                    juce::String text;
                    if (auto* button = dynamic_cast<juce::Button*>(&c)) text << button->getButtonText();
                    if (auto* tip = dynamic_cast<juce::SettableTooltipClient*>(&c)) text << " " << tip->getTooltip();
                    if (text.containsIgnoreCase("demo"))
                        ++demoMentions;
                    for (auto* child : c.getChildren())
                        walk(*child);
                };
                walk(*rig.editor);
                check(demoMentions == 0 && proc.getCargo().firstEmptyTrack() == 0,
                      "first run: it starts empty, and nothing on screen offers demo sounds (" + juce::String(demoMentions) + " found)");
            }

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

        DemoSounds::loadInto(proc);
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
            // The knobs: three wheels, whole in their boxes, on one column pitch. The small one (the room strip under
            // the sea) has its caption and value beside the dial, at its right; the others under it.
            {
                std::vector<WheelKnob*> knobs;
                collect(ed, knobs);
                int whole = 0, primary = 0, small = 0;
                for (auto* k : knobs)
                {
                    const auto size = k->getSize();
                    primary += size == WheelKnob::Size::Primary ? 1 : 0;
                    small += size == WheelKnob::Size::Small ? 1 : 0;
                    const float ring = k->getRingRadius(), outer = ring + WheelKnob::c_modRoom;
                    const auto centre = k->getDialCentre();
                    const auto caption = k->getCaptionArea(), value = k->getValueArea();
                    const auto box = k->getLocalBounds().toFloat();
                    const bool clearOfText = k->textBeside() ? centre.x + outer <= caption.getX() + 0.01f && centre.y + outer <= box.getHeight() + 0.01f
                                                             : centre.y + outer <= caption.getY() + 0.01f;
                    const bool ok = std::abs(ring - WheelKnob::ringFor(size)) < 1.0e-3f && k->textBeside() == (size == WheelKnob::Size::Small)
                                    && k->getWidth() >= WheelKnob::cellWidth(size) && k->getHeight() >= WheelKnob::cellHeight(size)
                                    && centre.x - outer >= -0.01f && centre.x + outer <= box.getWidth() + 0.01f && centre.y - outer >= -0.01f
                                    && clearOfText && caption.getBottom() <= value.getY() + 0.01f
                                    && box.contains(caption) && box.contains(value) && caption.getHeight() >= 12.0f && value.getHeight() >= 12.0f
                                    && caption.getWidth() >= 30.0f;
                    whole += ok ? 1 : 0;
                    if (! ok)
                        std::cout << "         " << k->caption << ": box " << k->getBounds().toString() << ", ring " << ring << std::endl;
                }
                const int count = static_cast<int>(knobs.size());
                check(count >= 40 && whole == count && primary >= 20 && small == 4 && primary + small < count,
                      "knobs: all " + juce::String(count) + " are one of three wheels (" + juce::String(primary) + " primary, "
                          + juce::String(count - primary - small) + " secondary, " + juce::String(small)
                          + " small), whole in their boxes with the dial clear of the caption and the value," + at);

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
                    // (a bus's starts after its Flight Path menu and its Direction and Side, which sit at the strip's left end,
                    // in that order, inside the panel and clear of each other)
                    const bool bus = i < ed.m_flightPath.size();
                    const int path = bus ? ed.m_side[i]->getRight() + 10 - (panel.getX() + 12) : 0;
                    strips = strips && panel.contains(strip) && strip.getWidth() >= panel.getWidth() - 32 - path && strip.getBottom() > panel.getBottom() - 12
                             && strip.getHeight() >= 14 && strip.getRight() == panel.getRight() - 12
                             && (! bus || (panel.contains(ed.m_flightPath[i].getBounds()) && panel.contains(ed.m_flightPathLabel[i])
                                           && ed.m_flightPathLabel[i].getRight() < ed.m_flightPath[i].getX()
                                           && ed.m_flightPath[i].getRight() < ed.m_direction[i]->getX() && ed.m_direction[i]->getRight() < ed.m_side[i]->getX()
                                           && ed.m_side[i]->getRight() < strip.getX() && panel.contains(ed.m_side[i]->getBounds())));
                    for (auto* k : knobs)
                        strips = strips && ! (k->getParentComponent() == &ed.m_content && k->getBounds().intersects(strip));
                }
                check(strips && ride.contains(ed.m_rideStripArea) && ed.m_rideStripArea.getWidth() > 200,
                      "strips: a scope strip the width of its panel along the bottom of Bus A, Bus B, Hits and Output, under the knobs (a bus's after its "
                      "Flight Path); the ride's last 8 s in Ride" + at);

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
            if (scale == 1.0f)
            {
                // Render rests while the format chosen can't be made, and a finished batch's line goes when a choice changes.
                auto& v = ed.m_variations;
                auto pick = [](juce::ComboBox& box, int id) { box.setSelectedId(id, juce::sendNotificationSync); };
                const int widthWas = v.m_width.getSelectedId(), layoutWas = v.m_layout.getSelectedId(), typeWas = proc.getFileType();
                pick(v.m_width, static_cast<int>(room::Width::Surround714) + 1);
                pick(v.m_layout, 1);
                const bool litForWav = v.m_render.isEnabled() && ! v.m_getProblem;
                proc.setFileType(2);
                v.readSettings();
                const bool restsForFlac = ! v.m_render.isEnabled() && v.m_getProblem && v.m_render.getTooltip().contains("red line");
                v.m_shown.state = VariationRenderer::State::Finished; // as if a batch had just ended with these choices
                v.m_planChanged = false;
                pick(v.m_layout, 2);
                const bool litForSets = v.m_render.isEnabled() && ! v.m_getProblem;
                check(litForWav && restsForFlac && litForSets,
                      "Render Variations: Render rests (and says why) for 7.1.4 in one FLAC file, and is lit for WAV and for Multi-Mono FLAC");
                check(v.m_planChanged, "Render Variations: a finished batch's Done line goes when a choice changes");
                v.m_shown = {};
                v.m_planChanged = false;
                proc.setFileType(typeWas);
                v.readSettings();
                pick(v.m_layout, layoutWas);
                pick(v.m_width, widthWas);
            }
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
        DemoSounds::loadInto(proc);
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

    // The room ---------------------------------------------------------------------------------------------------
    std::cout << "The room" << std::endl;
    {
        auto base = makeProcessor();
        auto& proc = *dynamic_cast<Proc*>(base.get());
        DemoSounds::loadInto(proc);
        Rig rig(proc, 1.0f);
        rig.open();
        auto& ed = *rig.editor;
        auto& sea = ed.m_sea;
        proc.setPlaying(true);
        rig.run(1.0);

        auto widen = [&](room::Width width)
        {
            auto layout = proc.getBusesLayout();
            layout.outputBuses.getReference(0) = room::channelSet(width);
            proc.setBusesLayout(layout);
            proc.prepareToPlay(c_fs, c_block);
            rig.block.setSize(room::layout(width).numChannels, c_block);
        };
        auto set = [&](const char* id, float plain)
        {
            auto* param = proc.getState().getParameter(id);
            param->setValueNotifyingHost(param->convertTo0to1(plain));
        };
        auto* spread = dynamic_cast<WheelKnob*>(ed.m_controls[ParamIDs::roomSpread]);
        auto* height = dynamic_cast<WheelKnob*>(ed.m_controls[ParamIDs::roomHeight]);
        auto* lfeLevel = dynamic_cast<WheelKnob*>(ed.m_controls[ParamIDs::roomLfeLevel]);
        auto* varies = dynamic_cast<WheelKnob*>(ed.m_controls[ParamIDs::roomHeightVaries]);
        const std::vector<juce::Component*> stripControls { spread, height, varies, &ed.m_roomCentre, &ed.m_lfeMode, lfeLevel };
        auto lookOf = [](juce::Component* c) { return Theme::getLook(*c); };
        auto tipOf = [](juce::Component* c) { auto* t = dynamic_cast<juce::SettableTooltipClient*>(c); return t != nullptr ? t->getTooltip() : juce::String(); };
        const auto& out = ed.m_sections[3].bounds;
        const int stereoKnobY = ed.m_controls[ParamIDs::space]->getY();
        const auto stereoScope = ed.m_sectionScopes[3]->getBounds();

        // Calm: how far a comet's head moves on the screen from one frame of the sea to the next, at its worst.
        auto worstCometStep = [&](double seconds, float& nearest)
        {
            float worst = 0.0f;
            nearest = 100.0f;
            std::array<juce::Point<float>, RigEngine::c_maxFlights> last {};
            std::array<bool, RigEngine::c_maxFlights> had {};
            const int frames = static_cast<int>(seconds * 60.0);
            for (int f = 0; f < frames; ++f)
            {
                rig.run(1.0 / 60.0);
                { UiProbe::Recorder r; r.paint(sea, 1.0f, 1.0f); } // the projection is set in paint
                for (size_t i = 0; i < sea.m_trails.size(); ++i)
                {
                    const auto& trail = sea.m_trails[i];
                    if (trail.size() < 3 || sea.m_flightEnv[i] < 0.05f)
                    {
                        had[i] = false;
                        continue;
                    }
                    const auto head = sea.project(trail.back());
                    nearest = juce::jmin(nearest, trail.back().z);
                    if (had[i])
                        worst = juce::jmax(worst, head.getDistanceFrom(last[i]));
                    last[i] = head;
                    had[i] = true;
                }
            }
            return worst;
        };
        float stereoNearest = 0.0f;
        const float stereoStep = worstCometStep(6.0, stereoNearest);

        // At Stereo the strip and the Flight Paths rest, still usable, each saying why; nothing else of the room is
        // drawn: no meters, no plan, Output as it always was.
        {
            bool resting = true;
            for (auto* c : stripControls)
                resting = resting && lookOf(c) == Theme::Look::Resting && c->isEnabled() && c->isVisible() && tipOf(c).contains("rests at Stereo");
            for (auto& menu : ed.m_flightPath)
                resting = resting && lookOf(&menu) == Theme::Look::Resting && menu.isEnabled() && tipOf(&menu).contains("rests at Stereo");
            check(resting && ed.m_roomStrip.isVisible() && ! ed.m_roomWidth.isVisible() && ed.m_roomStrip.getTooltip().contains("this track is Stereo"),
                  "room: at Stereo the strip and both Flight Paths rest (still usable), each tooltip saying why; in a host the width is a read-out from the track");
            bool dark = sea.roomShown() == 0.0f;
            for (int ch = 0; ch < room::c_maxChannels; ++ch)
                dark = dark && sea.lampLevel(ch) == 0.0f;
            for (int o = 0; o < RigEngine::c_objects; ++o)
            {
                float lit = 0.0f;
                sea.planDot(o, &lit);
                dark = dark && lit == 0.0f;
            }
            check(dark && ! ed.m_speakerMeters->isVisible() && ed.m_speakerMeters->numMeters() == 0
                      && ed.m_controls[ParamIDs::space]->getY() > ed.m_controls[ParamIDs::tremDepth]->getY() - 10
                      && ed.m_sectionScopes[3]->getWidth() == out.getWidth() - 24,
                  "room: at Stereo nothing else of it is drawn: no plan in the sea, no speaker meters, Output's knobs and scope where they were");
        }

        // The strip on the sea's foot: inside the sea, under its water, its controls on it and clear of each other;
        // the morph pad docks above it.
        {
            const auto strip = ed.m_roomStrip.getBounds();
            const auto sea0 = ed.m_sea.getBounds();
            bool inside = sea0.contains(strip) && strip.getBottom() <= sea0.getBottom() - 4 && sea.getFootHeight() == strip.getHeight()
                          && ed.m_morphPad.getBottom() <= strip.getY() - 4;
            std::vector<juce::Rectangle<int>> boxes { ed.m_roomWidthReadout, ed.m_roomWidthCaption, ed.m_roomCentreCaption, ed.m_lfeModeCaption };
            for (auto* c : stripControls)
                boxes.push_back(ed.m_content.getLocalArea(c, c->getLocalBounds()));
            inside = inside && ed.m_roomWidth.getParentComponent() == &ed.m_roomStrip
                     && ed.m_roomWidthReadout.contains(ed.m_content.getLocalArea(&ed.m_roomWidth, ed.m_roomWidth.getLocalBounds()));
            for (auto* c : stripControls)
                inside = inside && c->getParentComponent() == &ed.m_roomStrip;
            for (size_t i = 0; i < boxes.size(); ++i)
            {
                inside = inside && strip.contains(boxes[i]);
                for (size_t j = i + 1; j < boxes.size(); ++j)
                    inside = inside && ! boxes[i].intersects(boxes[j]);
            }
            check(inside, "room: the strip is the sea's foot (" + juce::String(strip.getHeight()) + " px), every control on it and clear of the others, the morph pad docked above it");
            check(lfeLevel->hasMark() && std::abs(lfeLevel->getMark() - ParamIDs::lfeLevelMainsDb) < 1.0e-6
                      && lfeLevel->getTextFromValue(ParamIDs::lfeLevelMainsDb).contains("(Mains)"),
                  "room: LFE Level marks its reference point on the dial and in its read-out (" + lfeLevel->getTextFromValue(ParamIDs::lfeLevelMainsDb) + ")");
            check(ModMatrix::targetIndex(ParamIDs::roomSpread) >= 0 && ModMatrix::targetIndex(ParamIDs::roomHeight) >= 0
                      && ModMatrix::targetIndex(ParamIDs::roomLfeLevel) < 0 && ModMatrix::targetIndex(ParamIDs::flightPath[0]) < 0,
                  "room: Spread and Height take a wire (LFE Level and the Flight Paths do not)");
        }

        // The new colours: every name on the strip reads on its plate, resting or not; the speakers' names on
        // Output's; a lamp lit is plain against one at rest, and every object reads on the plan's well.
        {
            check(contrast(TC::textFaint, TC::raised) >= 4.5 && contrast(TC::textDim, TC::raised) >= 4.5 && contrast(TC::textFaint, TC::well) >= 4.5,
                  "room: the strip's names read on its plate, lit or resting (" + juce::String(contrast(TC::textFaint, TC::raised), 2) + ":1 at rest)");
            check(contrast(TC::textDim, TC::panel) >= 4.5 && contrast(TC::stowaway.brighter(0.3f), TC::panel) >= 4.5,
                  "room: the speakers' names read under their meters, the LFE's in its own colour (" + juce::String(contrast(TC::stowaway.brighter(0.3f), TC::panel), 2) + ":1)");
            const auto unlit = TC::well.overlaidWith(TC::bolt.withAlpha(0.45f)), lit = juce::Colour(0xfff3e3c0);
            bool objects = true;
            for (const auto c : { TC::busA, TC::busB, TC::brass, TC::hits, TC::stowaway })
                objects = objects && contrast(c, TC::well) >= 3.0;
            check(contrast(lit, unlit) >= 4.5 && contrast(unlit, TC::well) >= 1.4 && objects,
                  "room: a lamp at full is " + juce::String(contrast(lit, unlit), 2) + ":1 against one at rest, which still shows on the well ("
                      + juce::String(contrast(unlit, TC::well), 2) + ":1), and every object's colour reads on it");
        }

        // A 5.1 track: the strip lights up (Height rests: no speakers overhead), Output makes room for six meters,
        // the sea draws the plan.
        set(ParamIDs::roomLfeMode, static_cast<float>(RigEngine::LfeMixdown));
        set(ParamIDs::roomCentre, 1.0f);
        widen(room::Width::Surround51);
        rig.run(2.0);
        {
            const bool lit = lookOf(spread) == Theme::Look::Normal && lookOf(&ed.m_roomCentre) == Theme::Look::Normal
                             && lookOf(&ed.m_lfeMode) == Theme::Look::Normal && lookOf(lfeLevel) == Theme::Look::Normal
                             && lookOf(height) == Theme::Look::Resting && tipOf(height).contains("no speakers overhead")
                             && lookOf(&ed.m_flightPath[0]) == Theme::Look::Normal && ! tipOf(spread).contains("rests");
            check(lit && ed.m_roomCentre.getButtonText() == "On", "room: at 5.1 the strip lights up, but for Height (5.1 has no speakers overhead, and its tooltip says so)");
            auto& meters = *ed.m_speakerMeters;
            juce::StringArray names;
            bool sounding = true;
            for (int m = 0; m < meters.numMeters(); ++m)
            {
                const int ch = meters.channelOf(m);
                names.add(room::info(room::layout(room::Width::Surround51).speakers[static_cast<size_t>(ch)]).name);
                sounding = sounding && meters.shownLevel(ch) > 0.0005f;
            }
            check(meters.isVisible() && names.joinIntoString(" ") == "L C R Ls Rs LFE" && sounding,
                  "room: Output shows a meter for each speaker, in the room's order with the LFE last: " + names.joinIntoString(" ")
                      + (sounding ? ", every one moving" : ", NOT all moving"));
            const auto m = meters.getBounds();
            const bool placed = out.contains(m) && ! m.intersects(ed.m_meterArea) && ! m.intersects(ed.m_trim.getBounds()) && ! m.intersects(ed.m_tremGroup)
                                && m.getY() >= ed.m_controls[ParamIDs::space]->getBottom() && ed.m_controls[ParamIDs::space]->getY() < stereoKnobY
                                && ed.m_sectionScopes[3]->getRight() <= ed.m_tremGroup.getRight() && ! ed.m_sectionScopes[3]->getBounds().intersects(m);
            check(placed, "room: the meters sit under Output's four knobs (moved up " + juce::String(stereoKnobY - ed.m_controls[ParamIDs::space]->getY())
                              + " px), clear of GR, OUT, Trim and the tremolo, and the scope keeps to the tremolo's width");
            bool lamps = sea.roomShown() > 0.99f;
            for (int ch = 0; ch < 6; ++ch)
                lamps = lamps && (ch == 3 || sea.lampLevel(ch) > 0.05f);
            check(lamps, "room: the sea draws the plan of the room, every speaker's lamp lit by its level");
            checkTexts("the window at 5.1", ed, 1.0f, 1.0f);

            // Each lamp and each meter is its own speaker's: with Centre Speaker off and LFE Mode Off the centre's and
            // the LFE's go dark and still while the others stay lit, and they come back with the switch and the mode.
            // And LFE Level rests while LFE Mode is Off (there is nothing for it to turn up), saying so.
            const int centre = room::layout(room::Width::Surround51).centre, lfe = room::layout(room::Width::Surround51).lfe;
            set(ParamIDs::roomCentre, 0.0f);
            set(ParamIDs::roomLfeMode, 0.0f);
            rig.run(4.0);
            for (int i = 0; i < 40; ++i)
                meters.step(0.1f); // (the rig's frames take no time: let the held peaks' 1.5 s pass)
            bool own = centre == 2 && lfe == 3;
            for (int ch = 0; ch < 6; ++ch)
            {
                const bool silent = ch == centre || ch == lfe;
                own = own && (silent ? sea.lampLevel(ch) == 0.0f && meters.shownLevel(ch) == 0.0f && meters.shownHold(ch) == 0.0f : sea.lampLevel(ch) > 0.05f && meters.shownLevel(ch) > 0.0005f);
            }
            const bool levelRests = lookOf(lfeLevel) == Theme::Look::Resting && tipOf(lfeLevel).contains("LFE Mode is Off")
                                    && lookOf(&ed.m_lfeMode) == Theme::Look::Normal && ed.m_roomCentre.getButtonText() == "Off";
            set(ParamIDs::roomCentre, 1.0f);
            set(ParamIDs::roomLfeMode, static_cast<float>(RigEngine::LfeMixdown));
            rig.run(2.0);
            check(own && sea.lampLevel(centre) > 0.05f && meters.shownLevel(centre) > 0.0005f && meters.shownLevel(lfe) > 0.0005f,
                  "room: the centre's and the LFE's lamps and meters are their own (dark with Centre Speaker off and LFE Mode Off, their meters and held peaks fallen to nothing, the others lit; back with them)");
            check(levelRests && lookOf(lfeLevel) == Theme::Look::Normal && ! tipOf(lfeLevel).contains("rests"),
                  "room: LFE Level rests while LFE Mode is Off, saying so, and lights with Discrete or Mixdown");
        }

        // Quad: no centre speaker and no LFE, so those rest too, each saying so.
        widen(room::Width::Quad);
        rig.run(0.5);
        check(lookOf(&ed.m_roomCentre) == Theme::Look::Resting && tipOf(&ed.m_roomCentre).contains("no centre speaker")
                  && lookOf(&ed.m_lfeMode) == Theme::Look::Resting && lookOf(lfeLevel) == Theme::Look::Resting && tipOf(lfeLevel).contains("no LFE")
                  && lookOf(spread) == Theme::Look::Normal && ed.m_speakerMeters->numMeters() == 4 && ed.m_speakerMeters->lfeReductionArea().isEmpty(),
              "room: at Quad, Centre Speaker and the LFE rest (no centre speaker, no LFE), and there are four meters");

        // 7.1.4: twelve meters, readable at every size: the names in order, each in its own box, none on another.
        widen(room::Width::Surround714);
        set(ParamIDs::roomSpread, 0.25f);
        rig.run(2.0);
        {
            auto& meters = *ed.m_speakerMeters;
            juce::StringArray names;
            bool apart = true;
            juce::SharedResourcePointer<Theme::Fonts> fonts;
            const auto font = fonts->caption();
            for (int m = 0; m < meters.numMeters(); ++m)
            {
                const auto name = juce::String(room::info(room::layout(room::Width::Surround714).speakers[static_cast<size_t>(meters.channelOf(m))]).name);
                names.add(name);
                apart = apart && Theme::textWidth(font, name) <= meters.nameArea(m).getWidth() && meters.getLocalBounds().toFloat().contains(meters.nameArea(m))
                        && meters.barArea(m).getBottom() <= meters.nameArea(m).getY();
                if (m > 0)
                    apart = apart && meters.barArea(m).getX() - meters.barArea(m - 1).getRight() >= 6.0f
                            && Theme::textWidth(font, names[m - 1]) * 0.5f + Theme::textWidth(font, name) * 0.5f + 2.0f
                                   <= meters.nameArea(m).getCentreX() - meters.nameArea(m - 1).getCentreX();
            }
            const int last = meters.numMeters() - 1;
            const bool lfeApart = last == 11 && meters.barArea(last).getX() - meters.barArea(last - 1).getRight() >= SpeakerMeters::c_lfeGap
                                  && ! meters.lfeReductionArea().isEmpty() && meters.lfeReductionArea().getX() >= meters.barArea(last).getRight();
            check(names.joinIntoString(" ") == "L C R Lss Rss Lsr Rsr Ltf Rtf Ltr Rtr LFE" && apart && lfeApart,
                  "room: at 7.1.4 twelve meters, " + names.joinIntoString(" ") + ", every name whole under its own meter, the tops after a gap, the LFE "
                      "set apart with its own limiter beside it");
            checkTexts("the window at 7.1.4", ed, 1.0f, 1.0f);
        }

        // A modulator on Spread or Height moves the room: with a Macro at full through a full wire on Spread and an
        // upside-down one on Height, the room the engine renders has Spread at 100% and Height at 0% while both knobs
        // stay where they are (0% and 100%).
        {
            auto& engine = proc.getEngine();
            auto play = [&](int blocks)
            {
                for (int i = 0; i < blocks; ++i)
                {
                    rig.block.clear();
                    rig.midi.clear();
                    proc.processBlock(rig.block, rig.midi);
                }
            };
            set(ParamIDs::roomSpread, 0.0f);
            set(ParamIDs::roomHeight, 1.0f);
            play(40);
            const float spreadBefore = engine.m_spreadNow, heightBefore = engine.m_params.roomHeight;
            set(ParamIDs::modType[3], 3.0f); // a Macro
            set(ParamIDs::modKnob[3], 1.0f);
            const int spreadWire = proc.getMods().connect(3, ParamIDs::roomSpread, 1.0f);
            const int heightWire = proc.getMods().connect(3, ParamIDs::roomHeight, -1.0f);
            play(80);
            const float spreadWired = engine.m_spreadNow, heightWired = engine.m_params.roomHeight;
            proc.getMods().disconnect(heightWire);
            proc.getMods().disconnect(spreadWire);
            set(ParamIDs::modType[3], 0.0f);
            play(80);
            check(spreadWire >= 0 && heightWire >= 0 && spreadBefore < 0.01f && heightBefore > 0.99f && spreadWired > 0.99f && heightWired < 0.01f
                      && engine.m_spreadNow < 0.01f && proc.getState().getRawParameterValue(ParamIDs::roomSpread)->load() < 0.01f,
                  "room: wires on Spread and Height reach the room (the engine's Spread " + juce::String(spreadBefore, 2) + " -> " + juce::String(spreadWired, 2)
                      + ", Height " + juce::String(heightBefore, 2) + " -> " + juce::String(heightWired, 2) + "), and the knobs themselves stay put");
        }

        // Each Flight Path on the plan and on the sea. A pass's dot keeps to where its path goes (Across in front,
        // Through from the front to behind, Overhead up into the middle, Around all the way round), and its comet
        // never comes nearer than a comet ever did, nor moves across the screen faster than the Stereo ones do.
        {
            set(ParamIDs::roomSpread, 0.0f);
            const auto box = [&]
            {
                const auto plate = sea.roomPlanArea();
                return plate.withTrimmedLeft(15.0f).withTrimmedRight(15.0f).withTrimmedTop(13.0f).withTrimmedBottom(21.0f);
            }();
            const auto c = box.getCentre();
            const float half = box.getWidth() * 0.5f;
            const char* pathNames[] = { "Across", "Through", "Overhead", "Around" };
            for (int path = 0; path < ParamIDs::numFlightPaths; ++path)
            {
                for (auto* id : ParamIDs::flightPath)
                    set(id, static_cast<float>(path));
                rig.run(1.0);
                float frontMost = 1.0f, backMost = -1.0f, leftMost = 1.0f, rightMost = -1.0f, innerMost = 2.0f, highest = 0.0f;
                std::array<bool, 4> quarters {};
                int comets = 0, onTheirObjects = 0, sided = 0, sameSide = 0;
                const int frames = 7 * 60;
                for (int f = 0; f < frames; ++f)
                {
                    rig.run(1.0 / 60.0);
                    // Each bus's comet flies where its pass is in the room.
                    for (int i = RigEngine::ObjPassA1; i <= RigEngine::ObjPassB2; ++i)
                    {
                        const auto& trail = sea.m_trails[static_cast<size_t>(i)];
                        const auto object = proc.getEngine().getObject(i);
                        if (trail.empty() || ! object.active || sea.m_flightEnv[static_cast<size_t>(i)] < 0.05f)
                            continue;
                        const auto want = SeaVisualiser::roomToSea(object, sea.m_flightEnv[static_cast<size_t>(i)]);
                        const auto& head = trail.back();
                        ++comets;
                        onTheirObjects += std::abs(head.x - want.x) < 0.15f && std::abs(head.y - want.y) < 0.15f && std::abs(head.z - want.z) < 0.15f ? 1 : 0;
                        // Left in the room is left on the sea and left on the plan (the speakers' lamps are placed by the same rule).
                        float lit = 0.0f;
                        const auto dot = sea.planDot(i, &lit);
                        if (std::abs(object.x) > 0.3f && lit > 0.15f)
                        {
                            ++sided;
                            sameSide += head.x * object.x > 0.0f && (dot.x - c.x) * object.x > 0.0f ? 1 : 0;
                        }
                    }
                    for (int o = RigEngine::ObjPassA1; o <= RigEngine::ObjPassB2; ++o)
                    {
                        float lit = 0.0f;
                        const auto p = sea.planDot(o, &lit);
                        if (lit < 0.15f)
                            continue;
                        const float x = (p.x - c.x) / half, y = (c.y - p.y) / half; // y up the plan is the front
                        frontMost = juce::jmin(frontMost, y);
                        backMost = juce::jmax(backMost, y);
                        leftMost = juce::jmin(leftMost, x);
                        rightMost = juce::jmax(rightMost, x);
                        innerMost = juce::jmin(innerMost, juce::jmax(std::abs(x), std::abs(y)));
                        quarters[static_cast<size_t>((x < 0.0f ? 0 : 1) + (y < 0.0f ? 2 : 0))] = true;
                        highest = juce::jmax(highest, proc.getEngine().getObject(o).z);
                    }
                }
                juce::String how;
                bool ok = false;
                const int sides = static_cast<int>(std::count(quarters.begin(), quarters.end(), true));
                // (frontMost and backMost: the least and the most of y, the front of the room at +1)
                // (How high: the others no higher than half an Overhead pass's arch, Height Varies being at the 50% it
                // starts at; Overhead most of the way up its arch, which stops short of the ceiling.)
                const float lowMost = 0.5f * room::c_archTop + 0.01f;
                if (path == 0) { ok = frontMost > 0.2f && leftMost < -0.4f && rightMost > 0.4f && highest < lowMost; how = "in front, from side to side, low"; }
                if (path == 1) { ok = backMost > 0.6f && frontMost < -0.6f && highest < lowMost; how = "from the front wall to the back, low"; }
                if (path == 2) { ok = highest > 0.6f && highest <= room::c_archTop + 0.01f && innerMost < 0.15f && backMost > 0.3f && frontMost < -0.3f; how = "from the front, up into the middle and on behind"; }
                if (path == 3) { ok = sides == 4 && highest < lowMost; how = "through all four corners of the room"; }
                std::cout << "         " << pathNames[path] << ": dots from " << juce::String(frontMost, 2) << " to " << juce::String(backMost, 2) << " front to back, "
                          << juce::String(leftMost, 2) << " to " << juce::String(rightMost, 2) << " across, nearest the middle " << juce::String(innerMost, 2)
                          << ", " << sides << " quarters, as high as " << juce::String(highest, 2) << std::endl;
                check(ok, juce::String("room: ") + pathNames[path] + " on the plan goes " + how);
                check(comets > 100 && onTheirObjects >= comets * 9 / 10,
                      juce::String("room: ") + pathNames[path] + ": the buses' comets fly where their passes are in the room (" + juce::String(onTheirObjects)
                          + " of " + juce::String(comets) + " looks)");
                check(path == 2 || (sided > 50 && sameSide >= sided * 9 / 10),
                      juce::String("room: ") + pathNames[path] + ": a pass at the left of the room is at the left of the sea and of the plan (" + juce::String(sameSide)
                          + " of " + juce::String(sided) + " looks)");
                float nearest = 0.0f;
                const float step = worstCometStep(5.0, nearest);
                check(nearest >= 1.29f && step <= stereoStep * 1.4f,
                      juce::String("room: ") + pathNames[path] + " on the sea is calm: no nearer than " + juce::String(nearest, 2) + " (Stereo's nearest "
                          + juce::String(stereoNearest, 2) + "), at most " + juce::String(step, 1) + " px a frame (Stereo's comets " + juce::String(stereoStep, 1) + ")");
            }
            for (auto* id : ParamIDs::flightPath)
                set(id, 0.0f);
        }

        // The plan's own rule: the front wall at the top, left at the left, a sound higher up nearer the middle.
        {
            const juce::Rectangle<float> box(0.0f, 0.0f, 100.0f, 100.0f);
            const auto front = SeaVisualiser::planPoint(box, 0.0f, 1.0f, 0.0f), backLeft = SeaVisualiser::planPoint(box, -1.0f, -1.0f, 0.0f);
            const auto top = SeaVisualiser::planPoint(box, -1.0f, 1.0f, 1.0f);
            check(front == juce::Point<float>(50.0f, 0.0f) && backLeft == juce::Point<float>(0.0f, 100.0f)
                      && std::abs(top.x - 50.0f * SeaVisualiser::c_planHeightIn) < 1.0e-4f && std::abs(top.y - 50.0f * SeaVisualiser::c_planHeightIn) < 1.0e-4f,
                  "room: on the plan the front is at the top and left at the left, and the tops sit on an inner square");
        }

        // Back to Stereo: the plan fades away, the meters go, Output is as it was, and the strip rests again.
        widen(room::Width::Stereo);
        set(ParamIDs::roomLfeMode, 0.0f);
        rig.run(1.5);
        check(sea.roomShown() == 0.0f && ! ed.m_speakerMeters->isVisible() && ed.m_controls[ParamIDs::space]->getY() == stereoKnobY
                  && ed.m_sectionScopes[3]->getBounds() == stereoScope && lookOf(spread) == Theme::Look::Resting,
              "room: back at Stereo the plan has faded away, the meters are gone, Output is as it was and the strip rests");

        // At 7.1.4, at the smallest size, the design size and half as big again: every text in the window fits.
        rig.editor.reset();
        widen(room::Width::Surround714);
        set(ParamIDs::roomCentre, 1.0f);
        for (const float scale : { 0.85f, 1.0f, 1.5f })
        {
            Rig sized(proc, scale);
            sized.block.setSize(12, c_block);
            sized.open();
            sized.run(1.5);
            checkTexts("the window at 7.1.4, at " + juce::String(scale, 2), *sized.editor, 1.0f, scale);
        }
    }


    // Words and flow ---------------------------------------------------------------------------------------------
    std::cout << "Words and flow" << std::endl;
    {
        // The capitals checker works both ways: a capitalised joining word inside a name is a fault too.
        {
            using UiProbe::capitalsFault;
            const auto label = Theme::Fit::Whole;
            bool both = true;
            for (const char* text : { "One File Per Whoosh", "Date And Time", "Reset To Defaults", "Linked To A", "Left To Right", "Loudest In Front" })
                both = both && capitalsFault(text, label).isNotEmpty();
            for (const char* text : { "One File per Whoosh", "Date and Time", "Reset to Defaults", "Linked to A", "Bus A Doppler", "Morph On / Off",
                                      "Save, Then Change", "Swooshbuckler 2.0 Is Available" })
                both = both && capitalsFault(text, label).isEmpty();
            check(both, "capitals: a small joining word written with a capital inside a name is caught (\"One File Per Whoosh\"), and names, single "
                        "letters and named choices (\"On / Off\") are not");
        }

        auto base = makeProcessor();
        auto& proc = *dynamic_cast<Proc*>(base.get());
        DemoSounds::loadInto(proc);
        Rig rig(proc, 1.0f);
        rig.open();
        auto& ed = *rig.editor;
        proc.setPlaying(true);
        rig.run(0.5);
        auto widen = [&](room::Width width)
        {
            auto layout = proc.getBusesLayout();
            layout.outputBuses.getReference(0) = room::channelSet(width);
            proc.setBusesLayout(layout);
            proc.prepareToPlay(c_fs, c_block);
            rig.block.setSize(room::layout(width).numChannels, c_block);
            rig.run(0.2);
        };
        auto set = [&](const char* id, float plain)
        {
            auto* param = proc.getState().getParameter(id);
            param->setValueNotifyingHost(param->convertTo0to1(plain));
        };
        auto value = [&](const char* id) { return juce::roundToInt(proc.getState().getRawParameterValue(id)->load()); };
        auto textsOf = [](juce::Component& c)
        {
            UiProbe::Recorder r;
            r.paint(c, 1.0f, 1.0f);
            juce::StringArray all;
            for (const auto& t : r.texts)
                all.add(t.text);
            return all;
        };
        std::function<void(juce::Component&, juce::StringArray&)> tipsOf = [&tipsOf](juce::Component& c, juce::StringArray& tips)
        {
            if (auto* t = dynamic_cast<juce::TooltipClient*>(&c))
                tips.add(t->getTooltip());
            for (auto* child : c.getChildren())
                tipsOf(*child, tips);
        };

        // One name for one thing: what you listen on is Speakers, what a file gets is Channels; "Room Width" and
        // "Listening" are on screen nowhere, and Render Variations has no plain "Width" (the bus knobs' word).
        {
            juce::StringArray seen = textsOf(ed);
            ed.m_settings.refresh();
            ed.m_settings.setVisible(true);
            seen.addArray(textsOf(ed.m_settings));
            ed.m_settings.setVisible(false);
            ed.m_variations.setVisible(true);
            rig.run(0.1);
            const auto variations = textsOf(ed.m_variations);
            seen.addArray(variations);
            ed.m_variations.setVisible(false);
            juce::StringArray tips;
            tipsOf(ed, tips);
            bool retired = true;
            for (const auto& text : seen)
                retired = retired && ! text.contains("Room Width") && text != "Listening";
            for (const auto& tip : tips)
                retired = retired && ! tip.contains("Room Width") && ! tip.contains("Listening");
            check(retired && seen.contains("Speakers") && variations.contains("Channels") && ! variations.contains("Width"),
                  "words: what you listen on is called Speakers and what a file gets Channels, everywhere; \"Room Width\" and \"Listening\" appear in no "
                  "text or tooltip, and Render Variations has no \"Width\" (" + juce::String(seen.size()) + " texts, " + juce::String(tips.size()) + " tooltips)");
        }

        // At Stereo the room rests and says so in place: a note in the sea where the plan goes, drawn; a resting
        // control's reason comes with the first tier of its tooltip; Render Variations says a room's files are
        // made with the room as set, not heard.
        {
            auto* spread = ed.m_controls[ParamIDs::roomSpread];
            const auto note = ed.m_sea.getRestNote();
            const auto drawn = textsOf(ed.m_sea);
            const auto first = ed.m_tooltips.summary(*spread, spread->getTooltip()); // the first tier (getTipFor needs a foreground app)
            check(note.contains("rests at Stereo") && note.contains("Render Variations") && drawn.contains("The room strip rests at Stereo.")
                      && first.contains("It rests at Stereo") && first.contains("Render Variations") && ! first.contains("Double-click"),
                  "resting: at Stereo the sea says in place why the strip rests and that Render Variations still uses it; a resting knob says why "
                  "from the first tier of its tooltip");
            ed.m_variations.setVisible(true);
            ed.m_variations.m_width.setSelectedId(static_cast<int>(room::Width::Surround51) + 1, juce::sendNotificationSync);
            rig.run(0.1);
            const auto room = ed.m_variations.roomLine(), said = ed.m_variations.roomNote();
            check(room.contains("Flight Paths A Across") && ! room.contains("Stowaway") && room.contains("Spread 0%") && said.contains("Not heard on your Stereo speakers")
                      && said.contains("C and the LFE will be silent (Centre Speaker Off, LFE Mode Off)") && ed.m_variations.m_showRoom.isVisible(),
                  "render: Channels 5.1 from a Stereo rig says the room as set, that it is not heard, and that C and the LFE will be silent; Set the "
                  "Room is there");
            ed.m_variations.m_width.setSelectedId(static_cast<int>(room::Width::Stereo) + 1, juce::sendNotificationSync);
            rig.run(0.1);
            check(ed.m_variations.roomLine().isEmpty() && ed.m_variations.roomNote().isEmpty() && ! ed.m_variations.m_showRoom.isVisible(),
                  "render: at Stereo Channels there is no room to speak of");
            ed.m_variations.setVisible(false);
        }

        // Direction and Side: a click chooses, the keys follow the path (Across at Stereo), Side rests on Across.
        {
            auto& direction = *ed.m_direction[0];
            auto& side = *ed.m_side[0];
            set(ParamIDs::flightPath[0], 1.0f); // Through: at Stereo still drawn as Across
            rig.run(0.1);
            const auto key = direction.keyArea(2).getCentre();
            direction.mouseDown(mouseAt(direction, key, key, false));
            direction.mouseUp(mouseAt(direction, key, key, false));
            const bool chosen = value(ParamIDs::direction[0]) == ParamIDs::DirectionBackwards && direction.words() == "Right to Left"
                                && ed.m_tooltips.summary(direction, direction.getTooltip()).contains("Right to Left");
            // (Side is not heard at Stereo: it rests, drawn for the Flight Path as set, which a rendered room uses.)
            const bool sideRests = Theme::getLook(side) == Theme::Look::Resting && Theme::getLook(direction) == Theme::Look::Normal
                                   && direction.getPath() == 0 && side.getPath() == 1 && ed.m_tooltips.summary(side, side.getTooltip()).contains("Random");
            proc.getHistory().undo();
            check(chosen && sideRests && value(ParamIDs::direction[0]) == ParamIDs::DirectionRandom,
                  "direction: a click on a key chooses it (\"Right to Left\" at Stereo, where every pass flies across) and Undo takes it back; Side rests");
            widen(room::Width::Surround714);
            const bool through = direction.getPath() == 1 && direction.wordsFor(1) == "Front to Back" && Theme::getLook(side) == Theme::Look::Normal
                                 && side.wordsFor(2) == "Right";
            set(ParamIDs::flightPath[0], 3.0f);
            rig.run(0.1);
            check(through && direction.wordsFor(1) == "Clockwise" && side.wordsFor(1) == "Loudest in Front",
                  "direction: in a room the keys and their words follow the Flight Path (Through: Front to Back, Right; Around: Clockwise, "
                  "Loudest in Front)");

            // Linked: bus B shows (and flies) bus A's path, way and side, and keeps its own for when Link goes off.
            set(ParamIDs::flightPath[1], 0.0f);
            set(ParamIDs::direction[0], 1.0f);
            set(ParamIDs::linkBuses, 1.0f);
            rig.run(0.1);
            auto& bDirection = *ed.m_direction[1];
            const auto bKey = bDirection.keyArea(2).getCentre();
            bDirection.mouseDown(mouseAt(bDirection, bKey, bKey, false));
            const bool shown = ed.m_flightPath[1].getSelectedId() == 4 && bDirection.shownChoice() == 1 && bDirection.getPath() == 3
                               && value(ParamIDs::direction[1]) == 0 && value(ParamIDs::flightPath[1]) == 0
                               && Theme::getLook(bDirection) == Theme::Look::Linked && Theme::getLook(ed.m_flightPath[1]) == Theme::Look::Linked;
            set(ParamIDs::linkBuses, 0.0f);
            rig.run(0.1);
            check(shown && ed.m_flightPath[1].getSelectedId() == 1 && bDirection.shownChoice() == 0 && bDirection.getPath() == 0,
                  "link: bus B's Flight Path and Direction show bus A's (Around, Clockwise) while linked, take no clicks, and go back to its own");

            // A wire decides: the switch takes it like a knob, and marks the key its modulator picks now.
            const int wire = proc.getMods().connect(3, ParamIDs::direction[0], 1.0f);
            ed.m_wires.rebuild();
            rig.run(0.5);
            ed.m_wires.rebuild();
            const int picked = direction.wiredChoice();
            proc.getMods().disconnect(wire);
            ed.m_wires.rebuild();
            rig.run(0.1);
            check(wire >= 0 && (picked == 1 || picked == 2) && direction.wiredChoice() == -1,
                  "direction: a wire lands on it like on a knob and marks the key its modulator picks (" + juce::String(picked) + ")");
            for (auto* id : { ParamIDs::flightPath[0], ParamIDs::direction[0] })
                set(id, 0.0f);
            // On Across a Side has nothing to choose: its tooltip says so in words, at once, never as a number.
            rig.run(0.1);
            const auto across = ed.m_tooltips.summary(side, side.getTooltip());
            check(side.getPath() == 0 && Theme::getLook(side) == Theme::Look::Resting && across.contains("No Sides") && across.contains("It rests: Across has no sides")
                      && ! across.contains("to 2"),
                  "side: on Across it rests and its tooltip says \"No Sides\" and why from the first tier");
        }

        // The LFE: Discrete with Stowaway off is said in place, with the fix; the fix is one undo step.
        {
            widen(room::Width::Surround51);
            set(ParamIDs::roomLfeMode, 1.0f);
            rig.run(0.1);
            auto* level = ed.m_controls[ParamIDs::roomLfeLevel];
            const bool said = ! level->isVisible() && ed.m_lfeFix.isVisible() && ed.m_lfeCue.isNotEmpty()
                              && textsOf(ed.m_roomStrip).contains("Nothing Feeds the LFE") && ed.m_lfeMode.getTooltip().contains("Nothing feeds the LFE");
            ed.m_lfeFix.onClick(); // what a click on it does
            rig.run(0.1);
            const bool fixed = proc.isStowawayOn() && proc.getStowawayMode() == ParamIDs::StowSub && ! ed.m_lfeFix.isVisible() && level->isVisible();
            proc.getHistory().undo();
            rig.run(0.1);
            check(said && fixed && ! proc.isStowawayOn() && ed.m_lfeFix.isVisible(),
                  "lfe: Discrete with Stowaway off says \"Nothing Feeds the LFE\" where LFE Level was, with Use Stowaway Sub; it switches Stowaway on in "
                  "Sub, and one Undo takes it back");
            const auto reads = ed.m_controls[ParamIDs::roomLfeLevel];
            check(dynamic_cast<WheelKnob*>(reads)->getTextFromValue(ParamIDs::lfeLevelMainsDb) == "-10.0 dB (Mains)",
                  "lfe: -10 dB reads as the level of the mains (\"-10.0 dB (Mains)\"), not \"Unity\"");
            set(ParamIDs::roomLfeMode, 0.0f);
        }

        // Stowaway folded on Fly names its Flight Path and way.
        {
            set(ParamIDs::stowMode, static_cast<float>(ParamIDs::StowFly));
            set(ParamIDs::stowFlightPath, 2.0f);
            set(ParamIDs::stowDirection, 1.0f);
            proc.setStowawayOn(true);
            proc.setStowawayPanel(0);
            rig.run(0.5);
            const auto note = ed.stowawayNote(ParamIDs::StowFly, true);
            const auto flying = proc.describeRoom(room::Width::Surround51);
            proc.setStowawayOn(false);
            check(note == "A third mover, flying Overhead, front to back.", "stowaway: folded on Fly, its note names the path and the way (\"" + note + "\")");
            check(flying.contains(", Stowaway Overhead (Front to Back). Spread") && ! proc.describeRoom(room::Width::Surround51).contains("Stowaway"),
                  "render: the room as set names Stowaway's Flight Path only while Stowaway flies (on, in Fly)");
        }

        // A change of Speakers asks first only when the Last 60 s memory holds something.
        check(! ed.asksBeforeChangingSpeakers() || proc.hasRetro(), "speakers: asks before a change only when the memory holds something");

        // A panel over the window: with the rig playing, nothing under it repaints (the sea, the lanes, the scopes,
        // the meters and the ride hold still behind the dimming), and it all moves again once the panel closes. Every
        // repaint asked for in the window passes through the content component on its way up, so a counter in its
        // place (a cached image that keeps nothing) sees each one: those inside the open panel, and the footer's
        // toast line, are the panel's and the toast's own; any other is something under the panel repainting.
        {
            struct RepaintCounter final : juce::CachedComponentImage
            {
                std::vector<juce::Rectangle<int>> areas;
                void paint(juce::Graphics&) override {}
                bool invalidateAll() override { areas.push_back({ 0, 0, Editor::c_width, Editor::c_height }); return true; }
                bool invalidate(const juce::Rectangle<int>& r) override { areas.push_back(r); return true; }
                void releaseResources() override {}
            };
            widen(room::Width::Stereo);
            proc.setPlaying(true);
            rig.run(0.5);
            auto behind = [&](juce::Component* panel, juce::Rectangle<int> plate, double seconds)
            {
                if (panel != nullptr)
                {
                    panel->setVisible(true);
                    panel->toFront(false);
                }
                rig.run(0.2); // the frame that notices the panel
                auto* counter = new RepaintCounter();
                ed.m_content.setCachedComponentImage(counter); // (the content owns it now)
                rig.run(seconds);
                int under = 0;
                for (const auto& r : counter->areas)
                    if (! plate.contains(r) && ! ed.m_footer.contains(r) && ! ed.m_toastPlate.getBounds().contains(r) && r != ed.m_content.getLocalBounds())
                        ++under;
                ed.m_content.setCachedComponentImage(nullptr);
                if (panel != nullptr)
                    panel->setVisible(false);
                rig.run(0.2);
                return under;
            };
            juce::String counts;
            bool held = true;
            int least = 1 << 30; // the fewest repaints the window asked for in a second with no panel, just before each
            for (const auto& [panel, plate, name] : { std::tuple<juce::Component*, juce::Rectangle<int>, const char*> { &ed.m_variations, ed.m_variations.m_panel, "Render Variations" },
                                                      { &ed.m_settings, ed.m_settings.m_panel, "Settings" }, { &ed.m_takes, ed.m_takes.m_panel, "Takes" },
                                                      { &ed.m_about, ed.m_about.m_panel, "About" } })
            {
                least = juce::jmin(least, behind(nullptr, {}, 1.0));
                const int under = behind(panel, plate, 1.0);
                held = held && under == 0;
                counts << name << " " << under << ", ";
            }
            check(held && least > 60 && ! Theme::isCovered(ed.m_sea),
                  "panels: over a playing rig nothing under Render Variations, Settings, Takes or About repaints in a second (" + counts
                      + "with none open at least " + juce::String(least) + "), and it all moves again when the panel closes");
        }
    }

    // The sea ---------------------------------------------------------------------------------------------------
    std::cout << "The sea" << std::endl;
    {
        auto base = makeProcessor();
        auto& proc = *dynamic_cast<Proc*>(base.get());
        DemoSounds::loadInto(proc);
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

        // The mark in the stars: a few dozen stars on the logo's corners and round its curves, joined by lines. They sit
        // on the artwork (on the ring and the stems they run down the middle, half a line's width off the outline).
        // Few enough to be a constellation and not a dotted drawing, and only a handful of them heavy: the star the
        // stems part from, the cap's corners, the ring's eyelet, the cherry's dimple.
        {
            const auto outline = sea.m_logo->createOutlinePath();
            float furthest = 0.0f;
            int heavy = 0, light = 0, furthestStar = -1;
            for (const auto& s : sea.m_constellation)
            {
                juce::Point<float> on;
                outline.getNearestPoint(s.at, on);
                if (on.getDistanceFrom(s.at) > furthest)
                {
                    furthest = on.getDistanceFrom(s.at);
                    furthestStar = (int) (&s - sea.m_constellation.data());
                }
                heavy += s.weight >= 0.45f ? 1 : 0;
                light += s.weight <= 0.3f ? 1 : 0;
            }
            std::set<int> joined;
            for (const auto& line : sea.m_constellationLines)
            {
                joined.insert(line.first);
                joined.insert(line.second);
            }
            const int stars = (int) sea.m_constellation.size();
            check(stars >= 28 && stars <= 45 && (int) sea.m_constellationLines.size() >= stars
                      && (int) joined.size() == stars && furthest < 0.012f && heavy >= 3 && heavy <= 8 && light * 2 >= stars,
                  "sea: the constellation is " + juce::String(stars) + " stars on the mark (the furthest, star "
                      + juce::String(furthestStar) + ", " + juce::String(furthest, 4) + " of a unit off its outline), every one joined by a line; "
                      + juce::String(heavy) + " are heavy and " + juce::String(light) + " light");
        }

        // The pull ring is a ring: its own stars, eight or more, all one distance from their middle and evenly spaced
        // round it, joined in the figure's strongest line. The outlines of the body and the cherry are the next
        // strongest; nothing else is drawn as strongly as those.
        {
            std::set<int> onRing;
            int ringLines = 0, outlineLines = 0, quietLines = 0;
            for (const auto& line : sea.m_constellationLines)
            {
                if (line.tier == LogoConstellation::ring)
                {
                    onRing.insert(line.first);
                    onRing.insert(line.second);
                    ++ringLines;
                }
                outlineLines += line.tier == LogoConstellation::silhouette ? 1 : 0;
                quietLines += line.tier < LogoConstellation::silhouette ? 1 : 0;
            }
            juce::Point<float> middle;
            for (int i : onRing)
                middle += sea.m_constellation[(size_t) i].at / (float) juce::jmax(1, (int) onRing.size());
            float nearest = 1.0e6f, farthest = 0.0f;
            for (int i : onRing)
            {
                const float d = sea.m_constellation[(size_t) i].at.getDistanceFrom(middle);
                nearest = juce::jmin(nearest, d);
                farthest = juce::jmax(farthest, d);
            }
            const float markWidth = sea.m_logo->createOutlinePath().getBounds().getWidth();
            check(onRing.size() >= 8 && ringLines == (int) onRing.size() && farthest - nearest < 0.02f * farthest
                      && 2.0f * farthest > 0.2f * markWidth && 2.0f * farthest < 0.35f * markWidth
                      && outlineLines >= 16 && quietLines >= 8 && quietLines < outlineLines + ringLines,
                  "sea: the constellation's pull ring is a circle of " + juce::String((int) onRing.size()) + " stars ("
                      + juce::String(juce::roundToInt(200.0f * farthest / markWidth)) + "% of the mark across, round to within "
                      + juce::String(100.0f * (farthest - nearest) / juce::jmax(1.0e-6f, farthest), 1) + "%) in the strongest line; "
                      + juce::String(outlineLines) + " outline lines, " + juce::String(quietLines) + " quieter ones");
        }

        // It is the whole mark, not a corner of it: the stars reach to every side of the logo, the cherry is drawn all
        // the way round in its own stars, and the grenade's round body has stars along the bottom of it.
        {
            auto boundsOf = [](const std::vector<juce::Point<float>>& points)
            {
                if (points.empty())
                    return juce::Rectangle<float>();
                juce::Point<float> low = points.front(), high = points.front();
                for (const auto& p : points)
                {
                    low = { juce::jmin(low.x, p.x), juce::jmin(low.y, p.y) };
                    high = { juce::jmax(high.x, p.x), juce::jmax(high.y, p.y) };
                }
                return juce::Rectangle<float>(low, high);
            };
            const auto mark = sea.m_logo->createOutlinePath().getBounds();
            const auto cherry = sea.m_logo->createOutlinePath(true).getBounds();
            const auto grenade = sea.m_logo->createOutlinePath(false).getBounds();

            std::vector<juce::Point<float>> all, onCherry, onBody;
            for (const auto& s : sea.m_constellation)
            {
                all.push_back(s.at);
                if (s.cherry)
                    onCherry.push_back(s.at);
                else if (s.at.y < grenade.getY() + 0.35f * grenade.getHeight() && s.at.x < cherry.getX())   // y is up: the bottom of the grenade
                    onBody.push_back(s.at);
            }
            const auto starsBounds = boundsOf(all), cherryStars = boundsOf(onCherry), bodyStars = boundsOf(onBody);
            const float acrossMark = starsBounds.getWidth() / mark.getWidth(), upMark = starsBounds.getHeight() / mark.getHeight();
            const float acrossCherry = cherryStars.getWidth() / cherry.getWidth(), upCherry = cherryStars.getHeight() / cherry.getHeight();
            const float acrossBody = bodyStars.getWidth() / grenade.getWidth();
            check(acrossMark > 0.93f && upMark > 0.93f && onCherry.size() >= 10 && acrossCherry > 0.9f && upCherry > 0.9f
                      && onBody.size() >= 5 && acrossBody > 0.4f,
                  "sea: the constellation is the whole mark (its stars span " + juce::String(juce::roundToInt(acrossMark * 100.0f)) + "% by "
                      + juce::String(juce::roundToInt(upMark * 100.0f)) + "% of the logo; the cherry has " + juce::String((int) onCherry.size())
                      + " of its own, spanning " + juce::String(juce::roundToInt(acrossCherry * 100.0f)) + "% by "
                      + juce::String(juce::roundToInt(upCherry * 100.0f)) + "% of it; the bottom of the grenade's body has "
                      + juce::String((int) onBody.size()) + ", across " + juce::String(juce::roundToInt(acrossBody * 100.0f)) + "% of the grenade)");

            // And it is big enough to read: a sixth of the sea's width or more, filling the sky's height.
            const auto area = sea.constellationArea();
            check(area.getWidth() >= sea.getWidth() * 0.16f && area.getHeight() >= sea.getHeight() * 0.30f * 0.8f,
                  "sea: the constellation is " + juce::String(juce::roundToInt(area.getWidth())) + " by " + juce::String(juce::roundToInt(area.getHeight()))
                      + " in a sea " + juce::String(sea.getWidth()) + " wide with " + juce::String(juce::roundToInt(sea.getHeight() * 0.30f)) + " of sky");
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
