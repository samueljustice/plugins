// UI test: the design tokens, the fitted text helper, the one rule for capitals, and the real editor drawn offline.
//
//   - Tokens: every text colour reaches 4.5:1 on the surfaces it is drawn on; the modulators' colours are none of
//     the sections' colours or the brass; the radii and the type scale are what the design says.
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
//
// Usage: SwooshbucklerUi [scratch folder] (a folder in the temp directory if none is given). Nothing is read from
// or written to the user's settings, and nothing goes on line.

#include "UiRig.h"
#include "UiProbe.h"
#include "Parameters.h"

#include <cmath>
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

        const juce::Colour sections[] = { TC::busA, TC::busB, TC::hits, TC::output, TC::brass, TC::space };
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
        check(distance(TC::busA, TC::busB) > 0.3 && distance(TC::busA, TC::hits) > 0.1 && distance(TC::busB, TC::hits) > 0.3,
              "sections: coral, turquoise and cherry are three different colours");
        check(TC::flight == TC::brass && TC::accent == TC::brass, "chrome: one accent (the old gold is the brass)");

        check(Theme::Cut::panel >= 3.0f && Theme::Cut::panel <= 5.0f && Theme::Cut::control >= 2.0f && Theme::Cut::control <= Theme::Cut::panel
                  && Theme::Cut::small > 0.0f && Theme::Cut::small <= Theme::Cut::control,
              "corners: cut, not round - panels 3 to 5, controls no more than panels, the small things no more than controls");
        check(Theme::Plate::bolt >= 6.0f && Theme::Plate::bolt <= 7.0f && Theme::Plate::boltInset >= Theme::Plate::bolt,
              "fasteners: a bolt head is 6 to 7 across (big enough to read as a bolt) and sits clear of its plate's edge");
        check(contrast(TC::bolt, TC::panel) >= 3.0 && contrast(TC::bolt, TC::boltSlot) >= 3.0 && contrast(TC::bolt, TC::raised) >= 3.0,
              "fasteners: a bolt reads on its plate, and its slot reads on the bolt (" + juce::String(contrast(TC::bolt, TC::panel), 2) + ", "
                  + juce::String(contrast(TC::bolt, TC::boltSlot), 2) + ")");
        check(luminance(TC::edgeDark) < luminance(TC::window) && TC::edgeLight.getAlpha() > 0 && TC::edgeLight.getAlpha() < 80,
              "edges: the pressed line is darker than anything it is drawn on, and the light line beside it is faint");
        {
            // The lit plates: the lettering on a plate in a section's colour, the brass or a modulator's colour.
            const juce::Colour lit[] = { TC::brass, TC::busA, TC::busB, TC::hits, TC::output, TC::go, TC::space, TC::filter,
                                         Theme::modColour(0), Theme::modColour(1), Theme::modColour(2), Theme::modColour(3) };
            for (const auto& plate : lit)
                check(plate.getPerceivedBrightness() > 0.5f && contrast(TC::window, plate) >= 4.5,
                      "lit plates: dark lettering reads on " + plate.toDisplayString(false) + " (" + juce::String(contrast(TC::window, plate), 2) + ":1)");
            for (int lane = 0; lane < 10; ++lane)
                check(contrast(TC::window, Theme::trackColour(lane)) >= 4.5, "lit plates: dark lettering reads on track colour " + juce::String(lane + 1));
        }
        {
            // A plate's shape: corners cut at 45 degrees, nothing round. Its path is eight straight lines.
            const auto& shape = Theme::Plate::shape({ 10.0f, 20.0f, 80.0f, 24.0f }, Theme::Cut::control);
            int lines = 0, curves = 0;
            for (juce::Path::Iterator it(shape); it.next();)
            {
                lines += it.elementType == juce::Path::Iterator::lineTo ? 1 : 0;
                curves += it.elementType == juce::Path::Iterator::quadraticTo || it.elementType == juce::Path::Iterator::cubicTo ? 1 : 0;
            }
            check(curves == 0 && lines == 7 && ! shape.contains(10.5f, 20.5f) && shape.contains(10.5f, 32.0f) && shape.contains(14.0f, 21.0f)
                      && shape.getBounds() == juce::Rectangle<float>(10.0f, 20.0f, 80.0f, 24.0f),
                  "plates: a plate is its rectangle with the four corners cut straight (no curves)");
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
    }

    std::cout << (g_failures == 0 ? "ALL PASSED" : juce::String(g_failures) + " FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
