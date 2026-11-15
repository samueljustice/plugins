#pragma once

// The text checker, shared by tools/UiShot.cpp (--list) and Tests/UiTest.cpp.
//
// Every text the interface draws goes through Theme::fitText / fitLines, which tell a Theme::TextProbe what was
// drawn, into which box, in which size. This file paints a component into an image through a graphics context
// that keeps track of where it is drawing, so each text can be placed in the window, and then checks the lot:
//
//   does not fit    a label, a value or a hint that had to be cut to fit its box
//   outside its box the ink runs past the box it was given (sideways or up and down)
//   too small       under Theme::c_minTextSize at the window's 1.0 scale
//   overlaps        two texts drawn on top of each other
//   capitals        a word in capitals that is not an acronym; a control name not in Title Case; a sentence that
//                   does not start with a capital
//
// The rule for capitals: control names, captions, buttons, menus and their items, tags and titles are in Title
// Case (short joining words stay small: "Keep the Last", "One File per Whoosh"); sentences are in sentence case;
// nothing is in capitals but single letters (A, B, M, S) and acronyms (GR, LFO, MIDI).

#include <JuceHeader.h>
#include "Theme.h"

#include <vector>

namespace UiProbe
{
    //==========================================================================================================
    /** A software renderer that knows where its origin is: everything is passed on to the real one, and the
        transforms are followed so a rectangle in the coordinates of whatever is painting can be placed in the image. */
    class TrackingContext : public juce::LowLevelGraphicsContext
    {
    public:
        explicit TrackingContext(const juce::Image& image) : m_inner(image) { m_stack.push_back({}); }

        const juce::AffineTransform& transform() const { return m_stack.back(); }

        bool isVectorDevice() const override { return m_inner.isVectorDevice(); }
        void setOrigin(juce::Point<int> o) override
        {
            m_stack.back() = juce::AffineTransform::translation(static_cast<float>(o.x), static_cast<float>(o.y)).followedBy(m_stack.back());
            m_inner.setOrigin(o);
        }
        void addTransform(const juce::AffineTransform& t) override
        {
            m_stack.back() = t.followedBy(m_stack.back());
            m_inner.addTransform(t);
        }
        float getPhysicalPixelScaleFactor() const override { return m_inner.getPhysicalPixelScaleFactor(); }
        bool clipToRectangle(const juce::Rectangle<int>& r) override { return m_inner.clipToRectangle(r); }
        bool clipToRectangleList(const juce::RectangleList<int>& r) override { return m_inner.clipToRectangleList(r); }
        void excludeClipRectangle(const juce::Rectangle<int>& r) override { m_inner.excludeClipRectangle(r); }
        void clipToPath(const juce::Path& p, const juce::AffineTransform& t) override { m_inner.clipToPath(p, t); }
        void clipToImageAlpha(const juce::Image& i, const juce::AffineTransform& t) override { m_inner.clipToImageAlpha(i, t); }
        bool clipRegionIntersects(const juce::Rectangle<int>& r) override { return m_inner.clipRegionIntersects(r); }
        juce::Rectangle<int> getClipBounds() const override { return m_inner.getClipBounds(); }
        bool isClipEmpty() const override { return m_inner.isClipEmpty(); }
        void saveState() override { m_stack.push_back(m_stack.back()); m_inner.saveState(); }
        void restoreState() override { if (m_stack.size() > 1) m_stack.pop_back(); m_inner.restoreState(); }
        void beginTransparencyLayer(float opacity) override { m_stack.push_back(m_stack.back()); m_inner.beginTransparencyLayer(opacity); }
        void endTransparencyLayer() override { if (m_stack.size() > 1) m_stack.pop_back(); m_inner.endTransparencyLayer(); }
        void setFill(const juce::FillType& f) override { m_inner.setFill(f); }
        void setOpacity(float o) override { m_inner.setOpacity(o); }
        void setInterpolationQuality(juce::Graphics::ResamplingQuality q) override { m_inner.setInterpolationQuality(q); }
        void fillRect(const juce::Rectangle<int>& r, bool replace) override { m_inner.fillRect(r, replace); }
        void fillRect(const juce::Rectangle<float>& r) override { m_inner.fillRect(r); }
        void fillRectList(const juce::RectangleList<float>& r) override { m_inner.fillRectList(r); }
        void fillPath(const juce::Path& p, const juce::AffineTransform& t) override { m_inner.fillPath(p, t); }
        void drawImage(const juce::Image& i, const juce::AffineTransform& t) override { m_inner.drawImage(i, t); }
        void drawLine(const juce::Line<float>& l) override { m_inner.drawLine(l); }
        void setFont(const juce::Font& f) override { m_inner.setFont(f); }
        const juce::Font& getFont() override { return m_inner.getFont(); }
        void drawGlyphs(juce::Span<const uint16_t> glyphs, juce::Span<const juce::Point<float>> positions, const juce::AffineTransform& t) override
        {
            ++glyphRuns;
            m_inner.drawGlyphs(glyphs, positions, t);
        }
        std::unique_ptr<juce::ImageType> getPreferredImageTypeForTemporaryImages() const override { return m_inner.getPreferredImageTypeForTemporaryImages(); }
        uint64_t getFrameId() const override { return m_inner.getFrameId(); }

        int glyphRuns = 0; // every run of glyphs drawn, fitted or not

    private:
        juce::LowLevelGraphicsSoftwareRenderer m_inner;
        std::vector<juce::AffineTransform> m_stack;
    };

    //==========================================================================================================
    struct Text
    {
        juce::String text;
        juce::Rectangle<float> box, ink; // in the window's own (1.0 scale) coordinates
        float size = 0.0f;               // in the type scale's units at 1.0 scale
        Theme::Fit fit = Theme::Fit::Whole;
        bool shrunk = false, cut = false, clipped = false;
    };

    struct Fault
    {
        juce::String what; // "does not fit", "overlaps", ...
        juce::String detail;
        Text text;
    };

    /** Collects every fitted text drawn while it is the probe. */
    class Recorder : public Theme::TextProbe
    {
    public:
        Recorder() { Theme::textProbe() = this; }
        ~Recorder() override { Theme::textProbe() = nullptr; }

        /** Paints the component and its children into a new image, recording its texts in the window's own (1.0
            scale) coordinates. paintScale enlarges the picture; contentScale is the scale the component is already
            laid out at (an editor resized to 1.5 times its design size has contentScale 1.5). */
        juce::Image paint(juce::Component& component, float paintScale, float contentScale = 1.0f)
        {
            juce::Image image(juce::Image::ARGB, juce::jmax(1, juce::roundToInt(static_cast<float>(component.getWidth()) * paintScale)),
                              juce::jmax(1, juce::roundToInt(static_cast<float>(component.getHeight()) * paintScale)), true);
            TrackingContext context(image);
            juce::Graphics g(context);
            g.addTransform(juce::AffineTransform::scale(paintScale));
            m_context = &context;
            m_scale = paintScale * contentScale;
            component.paintEntireComponent(g, true);
            m_context = nullptr;
            glyphRuns += context.glyphRuns;
            return image;
        }

        void textDrawn(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> box, juce::Rectangle<float> ink,
                       float fontHeight, Theme::Fit fit, bool shrunk, bool cut) override
        {
            ++fitted;
            if (m_context == nullptr)
                return; // drawn into an image of the component's own (a cached layer): not placed
            const auto t = m_context->transform();
            auto place = [&](juce::Rectangle<float> r) { return r.transformedBy(t) / m_scale; };
            const auto clip = g.getClipBounds().toFloat();
            if (! clip.intersects(ink.expanded(0.5f)))
                return; // not visible at all (scrolled away, or outside what was painted)
            const float zoom = std::sqrt(std::abs(t.getDeterminant())) / m_scale; // a component with a transform of its own
            Text record { text, place(box), place(ink), fontHeight * zoom / Theme::Fonts::c_em, fit, shrunk, cut, ! clip.contains(ink.reduced(1.0f)) };
            texts.push_back(record);
        }

        std::vector<Text> texts;
        int fitted = 0;    // texts drawn through the fitting helper
        int glyphRuns = 0; // runs of glyphs drawn in all (a fitted text is one run; more than `fitted` means unfitted text)

    private:
        TrackingContext* m_context = nullptr;
        float m_scale = 1.0f;
    };

    //==========================================================================================================
    // The capitalisation rule.

    inline bool isAcronym(const juce::String& word)
    {
        static const juce::StringArray known { "GR", "LFO", "MIDI", "CC", "WAV", "AIFF", "FLAC", "DAW", "VST", "VST3", "AU", "AAX", "JUCE", "IBM",
                                               "SIL", "MIT", "UTC", "CD", "OK", "MB", "GB", "AB", "RMS", "EQ", "FX", "URL", "ID", "USB", "CPU", "II", "III" };
        return known.contains(word);
    }

    inline bool isSmallWord(const juce::String& word)
    {
        static const juce::StringArray small { "a", "an", "the", "and", "or", "of", "to", "in", "on", "at", "by", "for", "per", "as", "with", "from", "is" };
        return small.contains(word);
    }

    /** Units and marks that stay as they are in a label ("60 s", "kHz", "st"). */
    inline bool isUnit(const juce::String& word)
    {
        static const juce::StringArray units { "s", "ms", "st", "c", "ct", "dB", "dBFS", "Hz", "kHz", "min", "x", "v", "px", "bit" };
        return units.contains(word);
    }

    inline juce::StringArray wordsOf(const juce::String& text)
    {
        juce::StringArray words;
        juce::String word;
        for (const auto ch : text)
        {
            if (juce::CharacterFunctions::isLetterOrDigit(ch) || ch == '\'')
                word << juce::String::charToString(ch);
            else if (word.isNotEmpty())
            {
                words.add(word);
                word.clear();
            }
        }
        if (word.isNotEmpty())
            words.add(word);
        return words;
    }

    /** A word written all in capitals that is not a known acronym (or empty if there is none). */
    inline juce::String shoutedWord(const juce::String& text)
    {
        for (const auto& word : wordsOf(text))
        {
            if (word.length() < 2 || word.containsAnyOf("0123456789") || isAcronym(word))
                continue;
            if (word == word.toUpperCase() && word != word.toLowerCase())
                return word;
        }
        return {};
    }

    /** What is wrong with how a text is capitalised, or empty. */
    inline juce::String capitalsFault(const juce::String& text, Theme::Fit fit)
    {
        if (fit == Theme::Fit::Name)
            return {};
        if (const auto shouted = shoutedWord(text); shouted.isNotEmpty())
            return "\"" + shouted + "\" is in capitals";
        const auto words = wordsOf(text);
        if (words.isEmpty())
            return {};
        const auto startsLower = [](const juce::String& w) { return juce::CharacterFunctions::isLowerCase(w[0]); };
        if (fit == Theme::Fit::Sentence || fit == Theme::Fit::Message)
            return startsLower(words[0]) && ! isUnit(words[0]) ? "a sentence that does not start with a capital" : juce::String();
        // A label. A readout that starts with a number or a sign ("3 wires", "14.8 s", "+12 st", "4 of 10") is not a name.
        const auto first = text.trimStart()[0];
        if (juce::CharacterFunctions::isDigit(first) || first == '+' || first == '-')
            return {};
        for (int i = 0; i < words.size(); ++i)
        {
            const auto& w = words[i];
            if (! startsLower(w) || isUnit(w) || w.containsAnyOf("0123456789"))
                continue;
            if (isSmallWord(w) && i > 0 && i < words.size() - 1)
                continue;
            return "\"" + w + "\" is not in Title Case";
        }
        return {};
    }

    //==========================================================================================================
    /** Everything wrong with a set of recorded texts. */
    inline std::vector<Fault> check(const std::vector<Text>& texts)
    {
        std::vector<Fault> faults;
        for (const auto& t : texts)
        {
            if (t.cut && ! Theme::mayBeCut(t.fit))
                faults.push_back({ "does not fit", "it had to be cut to fit " + juce::String(t.box.getWidth(), 0) + " x " + juce::String(t.box.getHeight(), 0), t });
            if (t.ink.getX() < t.box.getX() - 0.75f || t.ink.getRight() > t.box.getRight() + 0.75f)
                faults.push_back({ "outside its box", "its ink is " + juce::String(t.ink.getWidth(), 1) + " wide in a box of " + juce::String(t.box.getWidth(), 1), t });
            if (t.ink.getHeight() > t.box.getHeight() + 1.5f)
                faults.push_back({ "outside its box", "its ink is " + juce::String(t.ink.getHeight(), 1) + " tall in a box of " + juce::String(t.box.getHeight(), 1), t });
            if (t.size < Theme::c_minTextSize - 0.05f)
                faults.push_back({ "too small", juce::String(t.size, 1) + " at 1.0 scale (the floor is " + juce::String(Theme::c_minTextSize, 0) + ")", t });
            if (const auto fault = capitalsFault(t.text, t.fit); fault.isNotEmpty())
                faults.push_back({ "capitals", fault, t });
        }
        // Two texts on top of each other (the same text drawn twice in the same place is one text with an effect).
        for (size_t i = 0; i < texts.size(); ++i)
            for (size_t j = i + 1; j < texts.size(); ++j)
            {
                const auto& a = texts[i];
                const auto& b = texts[j];
                if (a.text == b.text && a.ink.getCentre().getDistanceFrom(b.ink.getCentre()) < 2.0f)
                    continue;
                const auto overlap = a.ink.reduced(1.0f, 2.5f).getIntersection(b.ink.reduced(1.0f, 2.5f));
                if (overlap.getWidth() > 1.0f && overlap.getHeight() > 1.0f)
                    faults.push_back({ "overlaps", "it sits on \"" + b.text + "\"", a });
            }
        return faults;
    }

    inline juce::String describe(const Text& t)
    {
        auto r = [](juce::Rectangle<float> b) { return juce::String(juce::roundToInt(b.getX())) + "," + juce::String(juce::roundToInt(b.getY())) + " "
                                                       + juce::String(juce::roundToInt(b.getWidth())) + "x" + juce::String(juce::roundToInt(b.getHeight())); };
        const char* kinds[] = { "label", "sentence", "message", "name" };
        return "\"" + t.text + "\"  box " + r(t.box) + "  ink " + juce::String(t.ink.getWidth(), 1) + " wide  size " + juce::String(t.size, 1)
               + "  " + kinds[static_cast<int>(t.fit)] + (t.shrunk ? "  shrunk" : "") + (t.cut ? "  cut" : "") + (t.clipped ? "  clipped" : "");
    }
}
