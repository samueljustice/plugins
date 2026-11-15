#include "PluginEditor.h"
#include "Audition.h"
#include "DiagnosticLog.h"
#include "SammyJUpdateCheck.h"
#include "TonalRecallBinaryData.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

using namespace tonalrecall;

namespace tonalrecall::ui
{

//==============================================================================
// Palette, type and shapes

namespace
{
namespace Palette
{
    const juce::Colour background { 0xff0b1114 };
    const juce::Colour panel      { 0xff0f181c };
    const juce::Colour rule       { 0xff1a2b31 };
    const juce::Colour text       { 0xffe6fffd };
    // Secondary text. On the panel it has to clear 4.5:1 at 9 to 10 points, through the glass.
    const juce::Colour dim        { 0xff74a3a9 };
    const juce::Colour teal       { 0xff3fd6cc };   // music
    const juce::Colour tealText   { 0xff7fe7e0 };
    const juce::Colour tealLine   { 0xff1f5a5e };
    const juce::Colour amber      { 0xffffb454 };   // sound effect
    const juce::Colour amberLine  { 0xff6b4a1e };
}

// Loaded once per process and shared by every editor.
struct Fonts
{
    Fonts()
        : regular(juce::Typeface::createSystemTypefaceFor(TonalRecallBinaryData::AzeretMonoRegular_ttf, TonalRecallBinaryData::AzeretMonoRegular_ttfSize)),
          medium(juce::Typeface::createSystemTypefaceFor(TonalRecallBinaryData::AzeretMonoMedium_ttf, TonalRecallBinaryData::AzeretMonoMedium_ttfSize)),
          bold(juce::Typeface::createSystemTypefaceFor(TonalRecallBinaryData::AzeretMonoBold_ttf, TonalRecallBinaryData::AzeretMonoBold_ttfSize))
    {
    }

    juce::Typeface::Ptr regular, medium, bold;
};

enum class Weight { Regular, Medium, Bold };

// Tracking is a fraction of the height added after every character. Any tracking at all also
// switches the typeface's ligatures off, which a console wants.
juce::Font font(Weight weight, float height, float tracking = 0.08f)
{
    const juce::SharedResourcePointer<Fonts> fonts;
    const auto& face = weight == Weight::Bold ? fonts->bold : weight == Weight::Medium ? fonts->medium : fonts->regular;
    if (face == nullptr)
        return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), height, juce::Font::plain));
    return juce::Font(juce::FontOptions(face).withHeight(height)).withExtraKerningFactor(std::max(0.002f, tracking));
}

// Texts that did not fit their box since the process started (the tests require none).
std::atomic<int> g_overflows { 0 };

void drawText(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Justification just,
              const juce::Font& f, juce::Colour colour)
{
    if (juce::GlyphArrangement::getStringWidth(f, text) > area.getWidth() + 0.5f || f.getHeight() > area.getHeight() + 0.5f)
        g_overflows.fetch_add(1, std::memory_order_relaxed);
    g.setFont(f);
    g.setColour(colour);
    g.drawText(text, area, just, false);
}

// The clipped-corner shape: top-left and bottom-right corners cut at 45 degrees.
juce::Path chamfered(juce::Rectangle<float> r, float cut)
{
    cut = std::min(cut, 0.5f * std::min(r.getWidth(), r.getHeight()));
    juce::Path p;
    p.startNewSubPath(r.getX() + cut, r.getY());
    p.lineTo(r.getRight(), r.getY());
    p.lineTo(r.getRight(), r.getBottom() - cut);
    p.lineTo(r.getRight() - cut, r.getBottom());
    p.lineTo(r.getX(), r.getBottom());
    p.lineTo(r.getX(), r.getY() + cut);
    p.closeSubPath();
    return p;
}

void drawFrame(juce::Graphics& g, juce::Rectangle<float> r, float cut, juce::Colour fill, juce::Colour outline)
{
    const auto shape = chamfered(r.reduced(0.5f), cut);
    if (! fill.isTransparent())
    {
        g.setColour(fill);
        g.fillPath(shape);
    }
    g.setColour(outline);
    g.strokePath(shape, juce::PathStrokeType(1.0f));
}

juce::String upper(const std::string& s) { return juce::String(s).toUpperCase(); }

//==============================================================================
// Motion. Everything that moves is a short eased transition or a slow pulse on flat colour, and
// each component runs its own 60 Hz timer only while it has something to show: an idle console
// repaints nothing.

// Tests draw still frames: with motion off every transition reads as finished and pulses hold.
std::atomic<bool> g_motionOn { true };

double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

float easeOut(float t)
{
    const float u = 1.0f - juce::jlimit(0.0f, 1.0f, t);
    return 1.0f - u * u * u;
}

float easeInOut(float t)
{
    t = juce::jlimit(0.0f, 1.0f, t);
    return t * t * (3.0f - 2.0f * t);
}

// 0..1 through a transition that began at `start` (seconds); 1 when it never began or is over.
float progressSince(double start, double duration)
{
    if (start <= 0.0 || ! g_motionOn.load(std::memory_order_relaxed))
        return 1.0f;
    return juce::jlimit(0.0f, 1.0f, static_cast<float>((nowSeconds() - start) / duration));
}

// A slow 0..1 pulse; holds at its middle when motion is off.
float pulse(double hz)
{
    if (! g_motionOn.load(std::memory_order_relaxed))
        return 0.5f;
    return 0.5f + 0.5f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * hz * nowSeconds()));
}

class Motion : private juce::Timer
{
public:
    explicit Motion(juce::Component& owner) : m_owner(owner) {}

    // Repaint the owner at 60 Hz for this long from now.
    void animateFor(double seconds)
    {
        if (! g_motionOn.load(std::memory_order_relaxed))
            return;
        m_until = std::max(m_until, nowSeconds() + seconds);
        if (! isTimerRunning())
            startTimerHz(60);
    }

    // ... or for as long as a state lasts (a pulse, a sweep).
    void animateWhile(bool on)
    {
        m_continuous = on && g_motionOn.load(std::memory_order_relaxed);
        if (m_continuous && ! isTimerRunning())
            startTimerHz(60);
    }

private:
    void timerCallback() override
    {
        m_owner.repaint();
        if (! m_continuous && nowSeconds() > m_until)
            stopTimer();
    }

    juce::Component& m_owner;
    double m_until = 0.0;
    bool m_continuous = false;
};

juce::String signedNumber(double v, int decimals)
{
    const double scale = std::pow(10.0, decimals);
    const double rounded = std::round(v * scale) / scale + 0.0;   // (+ 0.0: no "-0" for a value just under zero)
    return (rounded >= 0.0 ? "+" : "") + juce::String(rounded, decimals);
}

// "+2 ST 62 C" from 2.62 semitones (whole semitones and cents, sign shown).
// "0", "+1", "-2": octaves.
juce::String octaveText(int octaves)
{
    return octaves == 0 ? juce::String("0") : (octaves > 0 ? "+" : "") + juce::String(octaves);
}

juce::String shiftText(double semitones, bool lower)
{
    const auto totalCents = static_cast<long long>(std::llround(std::abs(semitones) * 100.0));
    const juce::String sign = semitones < 0.0 && totalCents != 0 ? "-" : "+";
    const auto st = juce::String(totalCents / 100);
    const auto c = juce::String(totalCents % 100);
    return lower ? sign + st + " st " + c + " c" : sign + st + " ST " + c + " C";
}

// The least movement of the music's root that counts as a ramp worth following. Sustained
// strings and tremolo wander by tens of cents on their own.
constexpr float kRampCents = 60.0f;

// How far the music's root travels over what was learned, in cents (5th to 95th percentile, so a
// stray frame does not count as a ramp).
float musicRampCents(const core::MusicAnalysis& music)
{
    if (! music.valid || music.rootDriftCents.size() < 8)
        return 0.0f;
    auto sorted = music.rootDriftCents;
    std::sort(sorted.begin(), sorted.end());
    const auto last = sorted.size() - 1;
    return sorted[static_cast<std::size_t>(0.95 * static_cast<double>(last))] - sorted[static_cast<std::size_t>(0.05 * static_cast<double>(last))];
}

const char* methodName(core::SfxMethod m)
{
    switch (m)
    {
        case core::SfxMethod::Auto:      return "AUTO";
        case core::SfxMethod::Tonal:     return "TONAL";
        case core::SfxMethod::Engine:    return "ENGINE";
        case core::SfxMethod::Resonance: return "RESONANCE";
    }
    return "AUTO";
}

} // namespace

//==============================================================================
// Look: tooltips in the console's type and colours.

class ConsoleLook : public juce::LookAndFeel_V4
{
public:
    ConsoleLook()
    {
        setColour(juce::TooltipWindow::backgroundColourId, Palette::panel);
        setColour(juce::TooltipWindow::textColourId, Palette::text);
        setColour(juce::TooltipWindow::outlineColourId, Palette::tealLine);
    }

    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override
    {
        const auto layout = layoutFor(tipText);
        const int w = static_cast<int>(std::ceil(layout.getWidth())) + 20;
        const int h = static_cast<int>(std::ceil(layout.getHeight())) + 14;
        return juce::Rectangle<int>(screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                    screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
            .constrainedWithin(parentArea);
    }

    void drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) override
    {
        const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        g.fillAll(Palette::panel);
        g.setColour(Palette::tealLine);
        g.drawRect(r, 1.0f);
        layoutFor(text).draw(g, r.reduced(10.0f, 7.0f));
    }

private:
    static juce::TextLayout layoutFor(const juce::String& text)
    {
        juce::AttributedString s;
        s.setJustification(juce::Justification::topLeft);
        s.append(text, font(Weight::Regular, 11.0f, 0.01f), Palette::text);
        juce::TextLayout layout;
        layout.createLayoutWithBalancedLineLengths(s, 300.0f);
        return layout;
    }
};

//==============================================================================
// A button drawn in the console's style. Toggled = filled.

class ConsoleButton : public juce::Button
{
public:
    explicit ConsoleButton(const juce::String& text = {}) : juce::Button(text)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        // Never the keyboard: a clicked button that kept it would answer Return, and the host
        // would not get its own key.
        setWantsKeyboardFocus(false);
        setMouseClickGrabsKeyboardFocus(false);
    }

    // Breathes while on (an armed Learn).
    void setPulsing(bool shouldPulse)
    {
        if (shouldPulse != m_pulsing)
        {
            m_pulsing = shouldPulse;
            m_motion.animateWhile(m_pulsing || m_beacon);
            repaint();
        }
    }

    // A soft pulse on an idle button: "this is the next thing to press".
    void setBeacon(bool shouldBeckon)
    {
        if (shouldBeckon != m_beacon)
        {
            m_beacon = shouldBeckon;
            m_motion.animateWhile(m_pulsing || m_beacon);
            repaint();
        }
    }

    // One bright notch runs along the top edge: "this just changed".
    void flash()
    {
        m_flashStart = nowSeconds();
        m_motion.animateFor(0.75);
    }

    void setAccent(juce::Colour bright, juce::Colour textColour, juce::Colour line)
    {
        m_accent = bright;
        m_text = textColour;
        m_line = line;
        repaint();
    }

    void setFontSize(float size, Weight weight = Weight::Medium, float tracking = 0.12f)
    {
        m_size = size;
        m_weight = weight;
        m_tracking = tracking;
    }

    void setSubText(const juce::String& s)
    {
        if (s != m_sub)
        {
            m_sub = s;
            repaint();
        }
    }

    // Text only, no frame (links).
    void setPlain(bool plainText, juce::Justification just = juce::Justification::centred)
    {
        m_plain = plainText;
        m_plainJust = just;
    }
    void setLabel(const juce::String& s)
    {
        if (s != getButtonText())
            setButtonText(s);
    }

    // A transport mark drawn ahead of the text (drawn, not a character, so no font can lack it).
    enum class Icon { None, Play, Stop };
    void setIcon(Icon icon)
    {
        if (icon != m_icon)
        {
            m_icon = icon;
            repaint();
        }
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        const auto r = getLocalBounds().toFloat();
        const bool on = getToggleState();
        const bool enabled = isEnabled();

        if (m_plain)
        {
            // Text only. As a toggle, on is the accent colour with a rule under it.
            const auto f = font(m_weight, m_size, m_tracking);
            drawText(g, getButtonText(), r, m_plainJust, f, on || over ? m_accent : m_text.withMultipliedAlpha(enabled ? 1.0f : 0.5f));
            if (on)
            {
                const float textW = juce::GlyphArrangement::getStringWidth(f, getButtonText());
                const float x = m_plainJust.testFlags(juce::Justification::right) ? r.getRight() - textW
                              : m_plainJust.testFlags(juce::Justification::left) ? r.getX() : r.getCentreX() - 0.5f * textW;
                g.setColour(m_accent);
                g.fillRect(x, r.getBottom() - 2.0f, textW, 1.0f);
            }
            return;
        }

        // On and hover ease in and out instead of switching.
        const double t = nowSeconds();
        const float dt = static_cast<float>(std::min(0.1, t - m_lastPaint));
        m_lastPaint = t;
        const float onTarget = on ? 1.0f : 0.0f, overTarget = over && enabled ? 1.0f : 0.0f;
        if (! m_painted || ! g_motionOn.load(std::memory_order_relaxed))
        {
            m_onAmount = onTarget;
            m_overAmount = overTarget;
            m_painted = true;
        }
        else
        {
            const float step = 1.0f - std::exp(-dt / 0.05f);
            m_onAmount += (onTarget - m_onAmount) * step;
            m_overAmount += (overTarget - m_overAmount) * step;
            if (std::abs(onTarget - m_onAmount) > 0.004f || std::abs(overTarget - m_overAmount) > 0.004f)
                m_motion.animateFor(0.1);
            else
            {
                m_onAmount = onTarget;
                m_overAmount = overTarget;
            }
        }

        const float breath = m_pulsing && on ? 0.7f + 0.3f * pulse(1.1) : 1.0f;
        const float beckon = m_beacon && ! on && enabled ? pulse(0.9) : 0.0f;
        const auto fill = m_onAmount > 0.0f ? m_accent.withAlpha(m_onAmount * breath)
                        : down ? m_line
                        : beckon > 0.0f ? m_accent.withAlpha(0.16f * beckon) : juce::Colours::transparentBlack;
        const auto outline = m_line.interpolatedWith(m_accent, std::max({ m_onAmount, m_overAmount, beckon }));
        const float cut = std::min(6.0f, r.getHeight() * 0.25f);
        drawFrame(g, r, cut, fill, enabled ? outline : outline.withMultipliedAlpha(0.5f));

        const float flashT = progressSince(m_flashStart, 0.7);
        if (flashT < 1.0f)
        {
            const float notch = 30.0f;
            const float x = r.getX() + cut + (r.getWidth() - cut - notch) * easeInOut(flashT);
            g.setColour(Palette::text.withAlpha(1.0f - flashT * flashT));
            g.fillRect(x, r.getY(), notch, 2.0f);
        }

        const auto textColour = (enabled ? m_text : m_text.withMultipliedAlpha(0.45f)).interpolatedWith(Palette::background, m_onAmount);
        if (m_icon != Icon::None)
        {
            // The mark and the text, centred together.
            const auto f = font(m_weight, m_size, m_tracking);
            const float textW = std::ceil(juce::GlyphArrangement::getStringWidth(f, getButtonText()));
            const float mark = 7.0f, gap = 7.0f;
            if (mark + gap + textW > r.getWidth() - 12.0f)
                g_overflows.fetch_add(1, std::memory_order_relaxed);   // too close to the frame to read as inside it
            const float x = std::round(r.getCentreX() - 0.5f * (mark + gap + textW));
            const float cy = r.getCentreY();
            g.setColour(textColour);
            if (m_icon == Icon::Play)
            {
                juce::Path triangle;
                triangle.addTriangle(x, cy - 4.0f, x, cy + 4.0f, x + mark, cy);
                g.fillPath(triangle);
            }
            else
            {
                g.fillRect(x, cy - 3.5f, mark, mark);
            }
            drawText(g, getButtonText(), { x + mark + gap, r.getY(), textW + 1.0f, r.getHeight() }, juce::Justification::centredLeft, f, textColour);
        }
        else if (m_sub.isEmpty())
        {
            drawText(g, getButtonText(), r, juce::Justification::centred, font(m_weight, m_size, m_tracking), textColour);
        }
        else
        {
            auto inner = r.reduced(6.0f, 0.0f);
            const float total = m_size + 6.0f + 10.0f;
            auto block = inner.withSizeKeepingCentre(inner.getWidth(), total);
            drawText(g, getButtonText(), block.removeFromTop(m_size + 2.0f), juce::Justification::centred,
                     font(m_weight, m_size, m_tracking), textColour);
            block.removeFromTop(4.0f);
            drawText(g, m_sub, block, juce::Justification::centred, font(Weight::Medium, 9.0f, 0.1f),
                     Palette::dim.interpolatedWith(Palette::background, m_onAmount));
        }
    }

private:
    juce::Colour m_accent = Palette::teal, m_text = Palette::tealText, m_line = Palette::tealLine;
    float m_size = 10.0f, m_tracking = 0.12f;
    Weight m_weight = Weight::Medium;
    juce::String m_sub;
    bool m_plain = false;
    juce::Justification m_plainJust = juce::Justification::centred;
    Icon m_icon = Icon::None;

    Motion m_motion { *this };
    float m_onAmount = 0.0f, m_overAmount = 0.0f;
    double m_lastPaint = 0.0, m_flashStart = 0.0;
    bool m_painted = false, m_pulsing = false, m_beacon = false;
};

//==============================================================================
// TRIM: a switch that also says what it is doing. A lamp at the left (lit while on, breathing
// while the stamp's level is being measured), the name, and at the right the figure: how far the
// stamp is turned down, or OFF. It is not filled when on, like the other switches, so that it
// does not compete with Stamp above it.

class TrimButton : public juce::Button
{
public:
    TrimButton() : juce::Button("TRIM")
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        setWantsKeyboardFocus(false);
        setMouseClickGrabsKeyboardFocus(false);
    }

    // figure: what the right-hand side says. alert: it is drawn in amber (off, and the stamp would
    // go over full scale).
    void show(bool measuring, const juce::String& figure, bool alert)
    {
        if (measuring != m_measuring || figure != m_figure || alert != m_alert)
        {
            m_measuring = measuring;
            m_figure = figure;
            m_alert = alert;
            m_motion.animateWhile(m_measuring);
            repaint();
        }
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        const auto r = getLocalBounds().toFloat();
        const bool on = getToggleState();
        const float cut = std::min(6.0f, r.getHeight() * 0.25f);
        drawFrame(g, r, cut, down ? Palette::tealLine : juce::Colours::transparentBlack,
                  over ? Palette::teal : on ? Palette::tealLine.brighter(0.25f) : Palette::tealLine);

        auto inner = r.reduced(10.0f, 0.0f);
        // The lamp: the same block as the warning strip's marker.
        const auto lamp = juce::Rectangle<float>(inner.getX(), r.getCentreY() - 5.0f, 5.0f, 10.0f);
        if (on)
        {
            g.setColour(Palette::teal.withAlpha(m_measuring ? 0.3f + 0.7f * pulse(1.6) : 1.0f));
            g.fillRect(lamp);
        }
        else
        {
            g.setColour(Palette::tealLine.brighter(0.4f));
            g.drawRect(lamp, 1.0f);
        }
        inner.removeFromLeft(12.0f);

        const auto f = font(Weight::Medium, 9.5f, 0.12f);
        drawText(g, getButtonText(), inner.removeFromLeft(40.0f), juce::Justification::centredLeft, f, Palette::tealText);
        const auto figureColour = m_alert ? Palette::amber : on && ! m_measuring && m_figure.startsWith("-") ? Palette::text : Palette::dim;
        drawText(g, m_figure, inner, juce::Justification::centredRight, f, figureColour);
    }

private:
    juce::String m_figure;
    bool m_measuring = false, m_alert = false;
    Motion m_motion { *this };
};

//==============================================================================
// The bend slider: centre detent at 0, double-click resets.

class BendSlider : public juce::Slider
{
public:
    BendSlider() : juce::Slider(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox)
    {
        setRange(-1.0, 1.0, 0.0);
        setDoubleClickReturnValue(true, 0.0);
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        setVelocityBasedMode(false);
    }

    double snapValue(double attempted, DragMode) override
    {
        return std::abs(attempted) < 0.06 ? 0.0 : attempted;
    }

    void paint(juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced(7.0f, 0.0f);
        const float cy = r.getCentreY();
        const auto xFor = [&](double v) { return r.getX() + static_cast<float>((v + 1.0) * 0.5) * r.getWidth(); };

        g.setColour(Palette::amberLine);
        g.fillRect(r.getX(), cy - 0.5f, r.getWidth(), 1.0f);
        for (int i = -2; i <= 2; ++i)
        {
            const float tick = i == 0 ? 7.0f : 3.0f;
            g.fillRect(xFor(0.5 * i) - 0.5f, cy - tick, 1.0f, 2.0f * tick);
        }

        const double v = getValue();
        const float x0 = xFor(0.0), x1 = xFor(v);
        g.setColour(Palette::amber);
        g.fillRect(std::min(x0, x1), cy - 1.5f, std::abs(x1 - x0), 3.0f);

        const auto thumb = juce::Rectangle<float>(10.0f, 16.0f).withCentre({ x1, cy });
        drawFrame(g, thumb, 3.0f, isMouseOverOrDragging() ? Palette::amber : Palette::panel, Palette::amber);
    }
};

//==============================================================================
// A scan display: a dark well with a cached picture and a message when there is nothing to show.

class ScanDisplay : public juce::Component
{
public:
    enum class Activity { Idle, Armed, Capturing, Analysing };

    ScanDisplay(juce::Colour line, juce::Colour bright) : m_line(line), m_bright(bright) { setInterceptsMouseClicks(false, false); }

    // immediate: no cross-fade (the picture is following a drag).
    void setPicture(juce::Image image, bool immediate = false)
    {
        const double t = nowSeconds();
        if (immediate)
            m_previous = {};
        else if (m_image.isValid() && m_message.isEmpty() && t - m_revealStart > 0.1)
        {
            // Already showing a picture: the new one fades in over it.
            m_previous = m_image;
            m_fadeStart = t;
            m_motion.animateFor(0.3);
        }
        m_image = std::move(image);
        repaint();
    }

    void setMessage(const juce::String& text, juce::Colour colour)
    {
        if (text != m_message || colour != m_messageColour)
        {
            if (m_message.isNotEmpty() && text.isEmpty())
            {
                // From a message to the picture: it wipes in, and the corner brackets lock on.
                m_revealStart = nowSeconds();
                m_previous = {};
                m_motion.animateFor(0.8);
            }
            m_message = text;
            m_messageColour = colour;
            repaint();
        }
    }

    // progress: 0..1 through the selection while capturing, or negative when unknown.
    void setActivity(Activity activity, float progress = -1.0f)
    {
        m_progress = progress;
        if (activity != m_activity)
        {
            m_activity = activity;
            m_motion.animateWhile(activity != Activity::Idle);
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        const float w = r.getWidth(), h = r.getHeight();
        g.setColour(Palette::background);
        g.fillRect(r);

        const bool showPicture = m_image.isValid() && m_message.isEmpty();
        if (! showPicture)
        {
            // An empty scope: a faint graticule.
            g.setColour(Palette::rule.withAlpha(0.55f));
            for (int i = 1; i < 12; ++i)
                g.fillRect(std::floor(w * static_cast<float>(i) / 12.0f), 0.0f, 1.0f, h);
            for (int i = 1; i < 6; ++i)
                g.fillRect(0.0f, std::floor(h * static_cast<float>(i) / 6.0f), w, 1.0f);
        }
        else
        {
            const float reveal = easeOut(progressSince(m_revealStart, 0.65));
            const float fade = progressSince(m_fadeStart, 0.25);
            if (m_previous.isValid() && fade < 1.0f)
            {
                g.drawImageAt(m_previous, 0, 0);
                g.setOpacity(easeInOut(fade));
                g.drawImageAt(m_image, 0, 0);
                g.setOpacity(1.0f);
            }
            else if (reveal < 1.0f)
            {
                const float edge = w * reveal;
                {
                    const juce::Graphics::ScopedSaveState state(g);
                    g.reduceClipRegion(0, 0, static_cast<int>(std::ceil(edge)), getHeight());
                    g.drawImageAt(m_image, 0, 0);
                }
                g.setColour(m_bright);
                g.fillRect(edge - 1.0f, 0.0f, 2.0f, h);
            }
            else
            {
                m_previous = {};
                g.drawImageAt(m_image, 0, 0);
            }
        }

        if (m_activity == Activity::Capturing)
        {
            // A sweep line with a stepped trail: where Learn has got to in the selection.
            const float at = m_progress >= 0.0f ? m_progress : static_cast<float>(std::fmod(nowSeconds() / 2.4, 1.0));
            const float x = w * juce::jlimit(0.0f, 1.0f, at);
            for (int i = 0; i < 6; ++i)
            {
                g.setColour(m_bright.withAlpha(0.12f * (1.0f - static_cast<float>(i) / 6.0f)));
                g.fillRect(juce::Rectangle<float>(x - 12.0f * static_cast<float>(i + 1), 0.0f, 12.0f, h).getIntersection(r));
            }
            g.setColour(m_bright);
            g.fillRect(x - 1.0f, 0.0f, 2.0f, h);
        }
        else if (m_activity == Activity::Analysing)
        {
            // A block pacing the scope while the analysis runs.
            const float span = 72.0f;
            const float x = (w - span) * (g_motionOn.load(std::memory_order_relaxed)
                                              ? 0.5f - 0.5f * static_cast<float>(std::cos(juce::MathConstants<double>::twoPi * nowSeconds() / 1.5))
                                              : 0.5f);
            g.setColour(m_bright.withAlpha(0.08f));
            g.fillRect(x, 0.0f, span, h);
            g.setColour(m_bright);
            g.fillRect(x, 0.0f, span, 2.0f);
            g.fillRect(x, h - 2.0f, span, 2.0f);
        }

        g.setColour(m_line);
        g.drawRect(r, 1.0f);

        // Corner brackets: quiet at rest, breathing while armed, and snapping out to the corners
        // when a picture lands.
        const float lock = easeOut(progressSince(m_revealStart, 0.5));
        const float inset = 1.0f + (1.0f - lock) * 14.0f;
        const float strength = m_activity == Activity::Armed ? 0.45f + 0.55f * pulse(1.1)
                             : lock < 1.0f ? 1.0f
                             : showPicture ? 0.75f : 0.4f;
        g.setColour(m_bright.withAlpha(strength));
        const float arm = 11.0f, thick = 2.0f;
        const auto corner = [&](float cx, float cy, float dx, float dy)
        {
            g.fillRect(juce::Rectangle<float>(cx, cy, dx * arm, dy * thick).withPosition(std::min(cx, cx + dx * arm), std::min(cy, cy + dy * thick))
                           .withSize(arm, thick));
            g.fillRect(juce::Rectangle<float>(arm, thick).withSize(thick, arm)
                           .withPosition(std::min(cx, cx + dx * thick), std::min(cy, cy + dy * arm)));
        };
        corner(inset, inset, 1.0f, 1.0f);
        corner(w - inset, inset, -1.0f, 1.0f);
        corner(inset, h - inset, 1.0f, -1.0f);
        corner(w - inset, h - inset, -1.0f, -1.0f);

        if (m_message.isNotEmpty())
            drawText(g, m_message, r.reduced(8.0f, 0.0f), juce::Justification::centred, font(Weight::Medium, 10.0f, 0.12f), m_messageColour);
    }

private:
    juce::Colour m_line, m_bright;
    juce::Image m_image, m_previous;
    juce::String m_message;
    juce::Colour m_messageColour = Palette::dim;
    Activity m_activity = Activity::Idle;
    float m_progress = -1.0f;
    double m_revealStart = 0.0, m_fadeStart = 0.0;
    Motion m_motion { *this };
};

//==============================================================================
// A playhead: a line that moves across a scan while what it shows is being heard. It lies over the
// scan (and takes no clicks), maps time to x exactly as the picture under it does, and repaints
// only the strip it leaves and the strip it moves to.
//
// The audio thread reports a position once a block. Blocks arrive a few tens of milliseconds
// apart (and in bursts in some hosts), so the line is not drawn where the last block left it: it
// runs on by the clock between blocks and is drawn gently back towards the reports, which keeps
// it gliding instead of stepping.

class PlayheadLayer : public juce::Component, private juce::Timer
{
public:
    // How the picture under it maps time to x (in this component's coordinates).
    struct Map
    {
        float x0 = 0.0f, width = 0.0f, top = 0.0f, height = 0.0f;
        double duration = 0.0;   // seconds across the width
        bool valid = false;
        bool operator!=(const Map& o) const noexcept
        {
            const auto moved = [](double a, double b) { return std::abs(a - b) > 1.0e-6; };
            return moved(x0, o.x0) || moved(width, o.width) || moved(top, o.top) || moved(height, o.height) || moved(duration, o.duration)
                || valid != o.valid;
        }
    };

    struct Reading
    {
        bool show = false;
        double seconds = 0.0;
        bool moving = false;          // advancing in real time
        std::int64_t atNs = 0;        // when it was reported (steady clock)
        juce::Colour colour;
        bool strong = true;           // a bright, heavier line (false: a quiet one)
    };

    PlayheadLayer() { setInterceptsMouseClicks(false, false); }

    std::function<Reading()> read;
    // Where the line is now, for a readout (negative seconds: no line).
    std::function<void(double seconds)> onMoved;

    void setMap(const Map& map)
    {
        if (map != m_map)
        {
            m_map = map;
            repaint();
        }
    }

    // The editor's timer calls this; while there is a line to move, the layer runs its own
    // 60 Hz timer, and stops it when the line goes.
    void poll()
    {
        if (! isTimerRunning())
            tick();
    }

    void paint(juce::Graphics& g) override
    {
        if (! m_showing)
            return;
        const float x = m_x;
        const float top = m_map.top, h = m_map.height;
        // A faint glow either side, the line, and a small notch at its head.
        g.setColour(m_colour.withAlpha(m_strong ? 0.14f : 0.08f));
        g.fillRect(x - 4.0f, top, 8.0f, h);
        g.setColour(m_strong ? m_colour : m_colour.withAlpha(0.75f));
        const float w = m_strong ? 2.0f : 1.0f;
        g.fillRect(x - 0.5f * w, top, w, h);
        juce::Path notch;
        notch.addTriangle(x - 4.0f, top, x + 4.0f, top, x, top + 5.0f);
        g.fillPath(notch);
    }

private:
    void timerCallback() override { tick(); }

    static std::int64_t steadyNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void tick()
    {
        const auto r = read ? read() : Reading {};
        const bool show = r.show && m_map.valid && m_map.duration > 0.0 && m_map.width > 0.0f;
        const bool moving = g_motionOn.load(std::memory_order_relaxed);
        const double t = nowSeconds();
        const double dt = std::clamp(t - m_lastTick, 0.0, 0.1);
        m_lastTick = t;

        if (show)
        {
            // Where the report says the sound is now: as reported, moved on by the time since
            // (never by more than about a block, in case reports stop coming).
            double target = r.seconds;
            if (r.moving && moving)
                target += std::clamp(static_cast<double>(steadyNs() - r.atNs) * 1.0e-9, 0.0, 0.05);
            if (! m_showing || ! moving || ! r.moving || std::abs(target - m_seconds) > 0.25)
                m_seconds = target;   // a new start, a jump, or a held moment: straight there
            else
            {
                m_seconds += dt;
                m_seconds += (target - m_seconds) * 0.2;
            }
            m_seconds = std::clamp(m_seconds, 0.0, m_map.duration);
        }

        const float x = m_map.x0 + m_map.width * static_cast<float>(m_seconds / std::max(1.0e-9, m_map.duration));
        if (show != m_showing || (show && (std::abs(x - m_x) >= 0.2f || r.colour != m_colour || r.strong != m_strong)))
        {
            const auto strip = [this](float at)
            {
                return juce::Rectangle<float>(at - 6.0f, m_map.top - 1.0f, 12.0f, m_map.height + 2.0f).getSmallestIntegerContainer();
            };
            if (m_showing)
                repaint(strip(m_x));
            m_showing = show;
            m_x = x;
            m_colour = r.colour;
            m_strong = r.strong;
            if (m_showing)
                repaint(strip(m_x));
        }
        if (onMoved)
            onMoved(m_showing ? m_seconds : -1.0);

        if (show && moving)
        {
            if (! isTimerRunning())
                startTimerHz(60);
        }
        else
        {
            stopTimer();
        }
    }

    Map m_map;
    bool m_showing = false, m_strong = true;
    double m_seconds = 0.0, m_lastTick = 0.0;
    float m_x = 0.0f;
    juce::Colour m_colour = Palette::text;
};

//==============================================================================
// Editing tools the user picked, kept for the life of the process so a reopened window agrees.

struct EditTools
{
    bool draw = false;   // freehand: the line is drawn with the mouse
    int snap = 0;        // 0 off, 1 the music's notes, 2 the notes of its scale
    bool hear = true;    // audition while editing
};
EditTools g_tools;

//==============================================================================
// Sits over the effect's scan: marks the stretch the music was not heard under, and lets the
// user shape the shift along the effect, by pulling points or by drawing the line freehand,
// optionally snapped to the music's notes.

class CurveEditor : public juce::Component, public juce::SettableTooltipClient
{
public:
    // How the scan's picture maps time and pitch to pixels.
    struct Axis
    {
        double lo = 0.0, hi = 1.0;                 // MIDI at the bottom and top of the plot
        float x0 = 0.0f, plotW = 1.0f, top = 0.0f, plotH = 1.0f;
        double duration = 0.0;                     // seconds across the plot
        bool valid = false;
    };

    CurveEditor()
    {
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        setWantsKeyboardFocus(true);   // for undo and redo by key, once the scan has been clicked
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        // Cmd+Z undoes, Shift+Cmd+Z (or Cmd+Y) redoes. Everything else goes back to the host.
        const auto mods = key.getModifiers();
        if (! mods.isCommandDown() || onUndoKey == nullptr)
            return false;
        if (key.getKeyCode() == 'Z' || key.getKeyCode() == 'z')
        {
            onUndoKey(mods.isShiftDown());
            return true;
        }
        if (key.getKeyCode() == 'Y' || key.getKeyCode() == 'y')
        {
            onUndoKey(true);
            return true;
        }
        return false;
    }

    std::function<void(const std::vector<core::CurvePoint>&)> onChange;
    std::function<void(double seconds, double semitones)> onHold;   // sustain the sound here at this shift
    std::function<void(double seconds)> onRelease;                  // play on from here
    std::function<void(const std::vector<core::CurvePoint>& before)> onEdited;   // an edit finished: what it replaced
    std::function<void(int style)> onStyle;                         // one of the offered curves was picked
    std::function<void(bool redo)> onUndoKey;

    void setData(const Axis& axis, const std::vector<core::CurvePoint>& edits, const std::vector<float>& afterMidi, double hopSeconds,
                 int coveredFrom, int coveredTo, bool following, bool showCoverage, double centreMidi, const core::MusicAnalysis& music)
    {
        m_following = following;
        m_showCoverage = showCoverage;
        m_axis = axis;
        m_after = afterMidi;
        m_hop = hopSeconds;
        m_coveredFrom = coveredFrom;
        m_coveredTo = coveredTo;
        m_centre = centreMidi;
        // Only what snapping needs: the tuning, the root and the mode.
        m_scale = {};
        m_scale.valid = music.valid;
        m_scale.tuningCents = music.tuningCents;
        m_scale.rootPitchClass = music.rootPitchClass;
        m_scale.minor = music.minor;
        if (! m_gesture)
            m_points = edits;   // during a gesture the user's own points lead
        repaint();
    }

    // The ready-made curves on offer (where each would put the effect's pitch), and which is
    // in use (-1: none of them, the curve has been set by hand).
    void setStyles(const std::array<std::vector<float>, MatchOptions::kNumStyles>& curves, int active)
    {
        m_styles = curves;
        m_activeStyle = active;
        repaint();
    }

    bool isDragging() const noexcept { return m_gesture; }

    void paint(juce::Graphics& g) override
    {
        if (! m_axis.valid || m_after.empty())
            return;
        const auto plot = juce::Rectangle<float>(m_axis.x0, m_axis.top, m_axis.plotW, m_axis.plotH);
        const auto frames = static_cast<int>(m_after.size());

        // Where the music was not heard: hatched, with its edge marked.
        const auto predicted = [&](int from, int to)
        {
            if (to <= from)
                return;
            const float xa = xForFrame(from), xb = xForFrame(to);
            const auto band = juce::Rectangle<float>(xa, plot.getY(), xb - xa, plot.getHeight());
            if (band.getWidth() < 3.0f)
                return;
            {
                const juce::Graphics::ScopedSaveState state(g);
                g.reduceClipRegion(band.toNearestInt());
                g.setColour(Palette::amber.withAlpha(0.16f));
                for (float x = band.getX() - band.getHeight(); x < band.getRight(); x += 7.0f)
                    g.drawLine(x, band.getBottom(), x + band.getHeight(), band.getY(), 1.0f);
            }
            g.setColour(Palette::amber.withAlpha(0.55f));
            const float edge = from == 0 ? band.getRight() : band.getX();
            for (float y = band.getY(); y < band.getBottom(); y += 6.0f)
                g.fillRect(edge - 0.5f, y, 1.0f, 3.0f);
            // Following the music, the bend here is a prediction; otherwise it is simply unheard.
            const juce::String text = m_following ? "PREDICTED" : "NO MUSIC LEARNED HERE";
            const float textW = m_following ? 76.0f : 168.0f;
            if (band.getWidth() > textW + 6.0f)
                drawText(g, text, { band.getX() + 5.0f, band.getY() + 2.0f, textW, 11.0f }, juce::Justification::centredLeft,
                         font(Weight::Medium, 9.0f, 0.1f), Palette::amber);
        };
        // (Only when the effect is moving with the music: locked to one note, where the music was
        // heard makes no difference to it.)
        if (m_showCoverage)
            predicted(0, m_coveredFrom);
        if (m_showCoverage)
            predicted(m_coveredTo, frames);

        // The curves on offer. The one in use is named in the effect's colour; the others are
        // drawn grey with their names, and a click on a grey name switches to that curve.
        static constexpr const char* styleNames[MatchOptions::kNumStyles] { "LOCKED", "SUBTLE", "MOVING" };
        for (int style = 0; style < MatchOptions::kNumStyles; ++style)
        {
            const auto& curve = m_styles[static_cast<std::size_t>(style)];
            if (curve.size() < 2)
                continue;
            const bool active = style == m_activeStyle;
            const bool hot = style == m_hoverStyle && ! active;
            const auto grey = Palette::dim.withAlpha(hot ? 1.0f : 0.75f);
            if (! active)
            {
                juce::Path line;
                const std::size_t stride = std::max<std::size_t>(1, curve.size() / 400);
                for (std::size_t i = 0; i < curve.size(); i += stride)
                {
                    const juce::Point<float> pt { plot.getX() + plot.getWidth() * static_cast<float>(i) / static_cast<float>(curve.size() - 1),
                                                  std::clamp(yFor(curve[i]), plot.getY(), plot.getBottom()) };
                    if (i == 0)
                        line.startNewSubPath(pt);
                    else
                        line.lineTo(pt);
                }
                g.setColour(grey);
                g.strokePath(line, juce::PathStrokeType(hot ? 1.6f : 1.0f));
            }
            const auto label = styleLabelArea(style);
            drawText(g, styleNames[style], label, juce::Justification::centredLeft, font(Weight::Medium, 9.5f, 0.1f),
                     active ? Palette::amber : hot ? Palette::text : Palette::dim);
            if (active)
            {
                g.setColour(Palette::amber);
                g.fillRect(label.getX(), label.getBottom() - 1.0f, 44.0f, 1.0f);
            }
        }

        // A marker on the line under the mouse: "pull here" (a ring), or "draw here" (a cross).
        if (m_hoverX >= plot.getX() && m_hoverX <= plot.getRight() && m_hover < 0 && ! m_gesture)
        {
            g.setColour(Palette::amber.withAlpha(0.7f));
            if (g_tools.draw)
            {
                g.fillRect(m_hoverX - 5.0f, m_hoverY - 0.5f, 10.0f, 1.0f);
                g.fillRect(m_hoverX - 0.5f, m_hoverY - 5.0f, 1.0f, 10.0f);
            }
            else
            {
                g.drawEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre({ m_hoverX, yFor(afterAt(secondsFor(m_hoverX))) }), 1.0f);
            }
        }

        // The user's points: diamonds on the line. A freehand line has many: draw those small.
        const bool dense = m_points.size() > 24;
        for (int i = 0; i < static_cast<int>(m_points.size()); ++i)
        {
            const auto centre = pointPosition(m_points[static_cast<std::size_t>(i)]);
            const bool hot = ! m_drawing && (i == m_hover || i == m_drag);
            if (dense && ! hot)
            {
                g.setColour(Palette::amber.withAlpha(0.8f));
                g.fillRect(centre.x - 1.0f, centre.y - 1.0f, 2.0f, 2.0f);
                continue;
            }
            juce::Path diamond;
            const float s = hot ? 6.0f : 4.5f;
            diamond.addQuadrilateral(centre.x, centre.y - s, centre.x + s, centre.y, centre.x, centre.y + s, centre.x - s, centre.y);
            g.setColour(hot ? Palette::amber : Palette::background);
            g.fillPath(diamond);
            g.setColour(Palette::amber);
            g.strokePath(diamond, juce::PathStrokeType(1.2f));
        }

        // What the gesture is doing, in words: the note it is on and the cents it has added.
        if (m_gesture && m_readoutSeconds >= 0.0)
        {
            const double after = m_readoutAfter;
            int cents = 0;
            const auto name = upper(core::noteName(after, &cents));
            const auto text = name + " " + signedNumber(cents, 0) + "C  /  " + signedNumber(m_readoutValue * 100.0, 0) + " C";
            const float x = xFor(m_readoutSeconds), y = std::clamp(yFor(after), plot.getY(), plot.getBottom());
            const float tw = 132.0f;
            const float tx = std::clamp(x + 10.0f, plot.getX(), plot.getRight() - tw);
            const float ty = y > plot.getY() + 18.0f ? y - 17.0f : y + 8.0f;
            g.setColour(Palette::background.withAlpha(0.85f));
            g.fillRect(tx - 3.0f, ty - 1.0f, tw + 6.0f, 12.0f);
            drawText(g, text, { tx, ty, tw, 10.0f }, juce::Justification::centredLeft, font(Weight::Medium, 8.5f, 0.06f), Palette::text);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        m_hoverX = e.position.x;
        m_hoverY = e.position.y;
        m_hover = g_tools.draw ? -1 : hit(e.position);
        m_hoverStyle = m_hover < 0 ? styleAt(e.position) : -1;
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        m_hoverX = -1.0f;
        m_hover = -1;
        m_hoverStyle = -1;
        repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (! m_axis.valid || m_after.empty())
            return;
        // A click on one of the curves on offer (or its name) picks it. A point already on the
        // line comes first, so an edit is never mistaken for a pick.
        if (hit(e.position) < 0)
        {
            const int style = styleAt(e.position);
            if (style >= 0 && style != m_activeStyle)
            {
                if (onStyle)
                    onStyle(style);
                return;
            }
        }
        const bool inPlot = e.position.x >= m_axis.x0 && e.position.x <= m_axis.x0 + m_axis.plotW;

        // The line without the hand adjustments: what a gesture's values are measured from.
        m_base.resize(m_after.size());
        for (std::size_t i = 0; i < m_after.size(); ++i)
            m_base[i] = m_after[i] - static_cast<float>(core::editOffsetAt(m_points, static_cast<double>(i) * m_hop, m_axis.duration));
        m_before = m_points;
        m_moved = false;
        m_created = false;
        m_dragFrom = e.position;

        if (g_tools.draw)
        {
            if (! inPlot)
                return;
            m_gesture = m_drawing = true;
            m_stroke.clear();
            m_strokeFrom = secondsFor(e.position.x);
            addStrokeSample(e);
            return;
        }

        int index = hit(e.position);
        if (index < 0)
        {
            if (! inPlot || m_points.size() >= MatchOptions::kMaxEdits)
                return;
            m_points.push_back({ secondsFor(e.position.x), core::editOffsetAt(m_before, secondsFor(e.position.x), m_axis.duration) });
            index = static_cast<int>(m_points.size()) - 1;
            m_created = true;
        }
        grabKeyboardFocus();
        m_gesture = true;
        m_drag = index;
        m_dragStart = m_points[static_cast<std::size_t>(index)];
        showReadout(m_dragStart.seconds, m_dragStart.semitones);
        if (onHold)
            onHold(m_dragStart.seconds, baseAt(m_dragStart.seconds) + m_dragStart.semitones - m_centre);
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (! m_gesture)
            return;
        if (e.position.getDistanceFrom(m_dragFrom) > 2.5f)
            m_moved = true;

        if (m_drawing)
        {
            addStrokeSample(e);
            return;
        }
        if (m_drag < 0 || ! m_moved)
            return;

        auto& p = m_points[static_cast<std::size_t>(m_drag)];
        // Sideways only once the mouse has clearly moved that way, so a pitch nudge stays put in time.
        const double fine = e.mods.isShiftDown() ? 0.2 : 1.0;
        const float dx = e.position.x - m_dragFrom.x;
        if (std::abs(dx) > 4.0f)
            p.seconds = std::clamp(m_dragStart.seconds + static_cast<double>(dx) / static_cast<double>(m_axis.plotW) * m_axis.duration * fine,
                                   0.0, m_axis.duration);
        // Up is sharper. Shift slows the move down for fine work; Alt lets go of the snap.
        const double perPixel = (m_axis.hi - m_axis.lo) / static_cast<double>(m_axis.plotH);
        double value = m_dragStart.semitones + static_cast<double>(m_dragFrom.y - e.position.y) * perPixel * fine;
        const double base = baseAt(p.seconds);
        if (g_tools.snap != 0 && ! e.mods.isAltDown())
            value = core::snapToMusic(m_scale, base + value, g_tools.snap == 2) - base;
        else if (std::abs(value) < 0.02)
            value = 0.0;
        p.semitones = std::clamp(value, -24.0, 24.0);

        showReadout(p.seconds, p.semitones);
        if (onChange)
            onChange(m_points);
        if (onHold)
            onHold(p.seconds, base + p.semitones - m_centre);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (! m_gesture)
            return;
        double from = m_drawing ? m_strokeFrom : m_points[static_cast<std::size_t>(m_drag)].seconds;
        if (! m_drawing && m_created && ! m_moved)
            m_points = m_before;   // a plain click on the line: just listen from there, add nothing
        else
        {
            if (onChange)
                onChange(m_points);
            if (onEdited && (m_moved || m_drawing))
                onEdited(m_before);
        }
        m_gesture = m_drawing = false;
        m_drag = -1;
        m_readoutSeconds = -1.0;
        if (onRelease)
            onRelease(from);
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (g_tools.draw)
            return;
        const int index = hit(e.position);
        if (index < 0)
            return;
        const auto before = m_points;
        m_points.erase(m_points.begin() + index);
        m_hover = -1;
        if (onChange)
            onChange(m_points);
        if (onEdited)
            onEdited(before);
        repaint();
    }

private:
    // Where a style's name is written: a little above its own curve, each a step further along so
    // that names do not sit on top of one another where the curves run together.
    juce::Rectangle<float> styleLabelArea(int style) const
    {
        const auto& curve = m_styles[static_cast<std::size_t>(style)];
        const float x = m_axis.x0 + 8.0f + 66.0f * static_cast<float>(style);
        float y = m_axis.top + 14.0f;
        if (curve.size() >= 2)
        {
            const auto at = static_cast<std::size_t>(std::clamp((x + 20.0f - m_axis.x0) / m_axis.plotW, 0.0f, 1.0f) * static_cast<float>(curve.size() - 1));
            y = yFor(curve[at]);
        }
        // Stacked upwards in style order, so three curves on one note still read as three names.
        y = std::clamp(y - 16.0f - 13.0f * static_cast<float>(MatchOptions::kNumStyles - 1 - style), m_axis.top + 1.0f,
                       m_axis.top + m_axis.plotH - 13.0f);
        return { x, y, 58.0f, 12.0f };
    }

    // The style whose name is under a point, or -1. (Only the name picks a style: a click on a
    // curve is always an edit or a listen, so reaching for the line never changes the style.)
    int styleAt(juce::Point<float> at) const
    {
        for (int style = 0; style < MatchOptions::kNumStyles; ++style)
            if (m_styles[static_cast<std::size_t>(style)].size() >= 2 && styleLabelArea(style).expanded(3.0f, 3.0f).contains(at))
                return style;
        return -1;
    }

    // Freehand: the mouse position is the pitch. The stroke replaces the points under it, and the
    // curve either side of the stroke is pinned where it was.
    void addStrokeSample(const juce::MouseEvent& e)
    {
        const double seconds = secondsFor(e.position.x);
        double pitch = m_axis.hi - static_cast<double>((std::clamp(e.position.y, m_axis.top, m_axis.top + m_axis.plotH) - m_axis.top) / m_axis.plotH)
                                       * (m_axis.hi - m_axis.lo);
        if (g_tools.snap != 0 && ! e.mods.isAltDown())
            pitch = core::snapToMusic(m_scale, pitch, g_tools.snap == 2);
        const double value = std::clamp(pitch - baseAt(seconds), -24.0, 24.0);

        // One sample per 1/240 of the effect: a newer sample replaces a neighbour that close.
        const double spacing = std::max(1.0e-4, m_axis.duration / 240.0);
        m_stroke.erase(std::remove_if(m_stroke.begin(), m_stroke.end(),
                                      [&](const core::CurvePoint& p) { return std::abs(p.seconds - seconds) < spacing; }),
                       m_stroke.end());
        m_stroke.push_back({ seconds, value });

        double lo = seconds, hi = seconds;
        for (const auto& p : m_stroke)
        {
            lo = std::min(lo, p.seconds);
            hi = std::max(hi, p.seconds);
        }
        const double eps = std::max(2.0e-4, m_axis.duration / 400.0);
        std::vector<core::CurvePoint> points;
        for (const auto& p : m_before)
            if (p.seconds < lo - eps || p.seconds > hi + eps)
                points.push_back(p);
        if (lo - eps > 0.0)
            points.push_back({ lo - eps, core::editOffsetAt(m_before, lo - eps, m_axis.duration) });
        if (hi + eps < m_axis.duration)
            points.push_back({ hi + eps, core::editOffsetAt(m_before, hi + eps, m_axis.duration) });
        points.insert(points.end(), m_stroke.begin(), m_stroke.end());
        if (points.size() > MatchOptions::kMaxEdits)
            points.resize(MatchOptions::kMaxEdits);
        m_points = std::move(points);

        showReadout(seconds, value);
        if (onChange)
            onChange(m_points);
        if (onHold)
            onHold(seconds, baseAt(seconds) + value - m_centre);
        repaint();
    }

    void showReadout(double seconds, double value)
    {
        m_readoutSeconds = seconds;
        m_readoutValue = value;
        m_readoutAfter = baseAt(seconds) + value;
    }

    float xForFrame(int frame) const
    {
        const auto last = std::max<std::size_t>(1, m_after.size() - 1);
        return m_axis.x0 + m_axis.plotW * static_cast<float>(std::clamp(frame, 0, static_cast<int>(last))) / static_cast<float>(last);
    }
    float xFor(double seconds) const
    {
        return m_axis.x0 + m_axis.plotW * static_cast<float>(m_axis.duration > 0.0 ? std::clamp(seconds / m_axis.duration, 0.0, 1.0) : 0.0);
    }
    double secondsFor(float x) const
    {
        return std::clamp(static_cast<double>((x - m_axis.x0) / m_axis.plotW), 0.0, 1.0) * m_axis.duration;
    }
    float yFor(double midi) const
    {
        return m_axis.top + static_cast<float>((m_axis.hi - midi) / (m_axis.hi - m_axis.lo)) * m_axis.plotH;
    }
    static double valueAt(const std::vector<float>& curve, double hop, double seconds)
    {
        if (curve.empty() || hop <= 0.0)
            return 0.0;
        return curve[static_cast<std::size_t>(std::clamp(seconds / hop, 0.0, static_cast<double>(curve.size() - 1)))];
    }
    double afterAt(double seconds) const { return valueAt(m_after, m_hop, seconds); }
    double baseAt(double seconds) const { return valueAt(m_base, m_hop, seconds); }
    juce::Point<float> pointPosition(const core::CurvePoint& p) const
    {
        // During a gesture the picture lags the mouse by a frame or two: place points from the
        // gesture's own numbers so they stay under the hand.
        const double pitch = m_gesture ? baseAt(p.seconds) + core::editOffsetAt(m_points, p.seconds, m_axis.duration) : afterAt(p.seconds);
        return { xFor(p.seconds), std::clamp(yFor(pitch), m_axis.top, m_axis.top + m_axis.plotH) };
    }
    int hit(juce::Point<float> at) const
    {
        for (int i = static_cast<int>(m_points.size()) - 1; i >= 0; --i)
            if (pointPosition(m_points[static_cast<std::size_t>(i)]).getDistanceFrom(at) <= 8.0f)
                return i;
        return -1;
    }

    Axis m_axis;
    std::vector<core::CurvePoint> m_points, m_before, m_stroke;
    std::vector<float> m_after, m_base;
    core::MusicAnalysis m_scale;
    double m_hop = 0.005, m_centre = 0.0;
    int m_coveredFrom = 0, m_coveredTo = 0;
    int m_hover = -1, m_drag = -1;
    float m_hoverX = -1.0f, m_hoverY = 0.0f;
    std::array<std::vector<float>, MatchOptions::kNumStyles> m_styles;
    int m_activeStyle = -1, m_hoverStyle = -1;
    bool m_showCoverage = false;
    bool m_following = false, m_gesture = false, m_drawing = false, m_moved = false, m_created = false;
    core::CurvePoint m_dragStart;
    juce::Point<float> m_dragFrom;
    double m_strokeFrom = 0.0;
    double m_readoutSeconds = -1.0, m_readoutValue = 0.0, m_readoutAfter = 0.0;
};

//==============================================================================
// Shared panel scaffolding: frame, title, LEARN / CLEAR.

namespace PanelLayout
{
    constexpr int pad = 12;
    constexpr int titleY = 12, titleH = 24;
    constexpr int scanY = 46, scanH = 210;
    constexpr int transportY = 262, transportH = 24;   // the play buttons and the time, under the scan
    constexpr int bigY = 294, bigH = 30;
    constexpr int dimY = 326, dimH = 14;
    constexpr int labelY = 350, labelH = 12;
    constexpr int chipsY = 366, chipsH = 24;
    constexpr int gutter = 30;   // note labels at the left of a scan
}

class SlotPanel : public juce::Component
{
public:
    SlotPanel(juce::String title, juce::Colour bright, juce::Colour textColour, juce::Colour line)
        : m_title(std::move(title)), m_bright(bright), m_textColour(textColour), m_lineColour(line), m_scan(line, bright)
    {
        m_learn.setAccent(bright, textColour, line);
        m_clear.setAccent(bright, textColour, line);
        m_clear.setFontSize(9.0f);
        m_learn.onClick = [this] { if (onLearn) onLearn(); };
        m_clear.onClick = [this] { if (onClear) onClear(); };
        addAndMakeVisible(m_learn);
        addAndMakeVisible(m_clear);
        addAndMakeVisible(m_scan);
        addAndMakeVisible(m_playhead);
        m_playhead.onMoved = [this](double seconds) { showPlayTime(seconds); };
    }

    std::function<void()> onLearn, onClear;

    // Marks Learn as the next thing to press.
    void setBeacon(bool shouldBeckon) { m_learn.setBeacon(shouldBeckon); }

    // What a play button can do now.
    enum class PlayState { Unavailable, Idle, Waiting, Playing };

    // The editor's timer: moves the playhead on.
    void pollPlayhead() { m_playhead.poll(); }

    void paint(juce::Graphics& g) override
    {
        using namespace PanelLayout;
        // The outline breathes while Learn is armed and waiting for Preview.
        const auto outline = m_waiting ? m_lineColour.interpolatedWith(m_bright, 0.25f + 0.6f * pulse(1.1)) : m_lineColour;
        drawFrame(g, getLocalBounds().toFloat(), 10.0f, Palette::panel, outline);
        const float w = static_cast<float>(getWidth());
        const float bigIn = easeOut(progressSince(m_bigChanged, 0.3));
        drawText(g, m_title, { static_cast<float>(pad) + 4.0f, static_cast<float>(titleY), 200.0f, static_cast<float>(titleH) },
                 juce::Justification::centredLeft, font(Weight::Bold, 12.0f, 0.16f), m_bright);
        // New readings ease in: the big line slides a few pixels into place as it brightens.
        drawText(g, m_big, { static_cast<float>(pad) - (1.0f - bigIn) * 8.0f, static_cast<float>(bigY), w - 2.0f * pad, static_cast<float>(bigH) },
                 juce::Justification::centredLeft, font(Weight::Bold, 25.0f, 0.06f), m_bigColour.withMultipliedAlpha(bigIn));
        drawText(g, m_dim, { static_cast<float>(pad), static_cast<float>(dimY), w - 2.0f * pad, static_cast<float>(dimH) },
                 juce::Justification::centredLeft, font(Weight::Regular, 10.0f, 0.08f), Palette::dim.withMultipliedAlpha(bigIn));
        drawText(g, m_chipLabel, { static_cast<float>(pad), static_cast<float>(labelY), w - 2.0f * pad, static_cast<float>(labelH) },
                 juce::Justification::centredLeft, font(Weight::Medium, 9.0f, 0.2f), Palette::dim);
        paintTransport(g);
    }

    void resized() override
    {
        using namespace PanelLayout;
        auto top = juce::Rectangle<int>(pad, titleY, getWidth() - 2 * pad, titleH);
        m_clear.setBounds(top.removeFromRight(58));
        top.removeFromRight(6);
        m_learn.setBounds(top.removeFromRight(92));
        m_scan.setBounds(pad, scanY, getWidth() - 2 * pad, scanH);
        m_playhead.setBounds(m_scan.getBounds());
    }

protected:
    static void showPlayButton(ConsoleButton& b, PlayState state, const juce::String& label, const juce::String& tip)
    {
        b.setEnabled(state != PlayState::Unavailable);
        b.setToggleState(state == PlayState::Waiting || state == PlayState::Playing, juce::dontSendNotification);
        b.setPulsing(state == PlayState::Waiting);
        b.setIcon(state == PlayState::Playing ? ConsoleButton::Icon::Stop : ConsoleButton::Icon::Play);
        b.setLabel(state == PlayState::Playing ? juce::String("STOP") : state == PlayState::Waiting ? juce::String("ARMED") : label);
        if (b.getTooltip() != tip)
            b.setTooltip(tip);
    }

    // The transport row under the scan: the play buttons at the left (laid out by each panel, which
    // says where they end), then a track that fills as the clip plays, and its time at the right.
    juce::Rectangle<int> transportArea() const
    {
        using namespace PanelLayout;
        return { m_transportLeft, transportY, getWidth() - pad - m_transportLeft, transportH };
    }

    void setClipLength(double seconds)
    {
        if (std::abs(seconds - m_clipSeconds) > 1.0e-6)
        {
            m_clipSeconds = seconds;
            repaint(transportArea());
        }
    }

    void setPlayColour(juce::Colour colour)
    {
        if (colour != m_playColour)
        {
            m_playColour = colour;
            repaint(transportArea());
        }
    }

    void showPlayTime(double seconds)
    {
        // Repainted when what it shows changes: the time to the hundredth, or the track by a pixel.
        const auto shown = seconds < 0.0 ? -1 : static_cast<int>(std::floor(seconds * 100.0));
        const auto trackW = static_cast<double>(std::max(1, transportArea().getWidth() - 140));
        const auto pixel = seconds < 0.0 || m_clipSeconds <= 0.0 ? -1 : static_cast<int>(trackW * std::clamp(seconds / m_clipSeconds, 0.0, 1.0));
        if (shown != m_shownTime || pixel != m_shownPixel)
        {
            m_shownTime = shown;
            m_shownPixel = pixel;
            m_playSeconds = seconds;
            repaint(transportArea());
        }
    }

    static juce::String timeText(double seconds)
    {
        return juce::String(std::floor(std::max(0.0, seconds) * 100.0) / 100.0, 2);
    }

    void paintTransport(juce::Graphics& g)
    {
        if (m_clipSeconds <= 0.0)
            return;
        const auto area = transportArea().toFloat();
        const bool playing = m_playSeconds >= 0.0;
        const auto f = font(Weight::Medium, 10.0f, 0.1f);
        const juce::String text = playing ? timeText(m_playSeconds) + " / " + timeText(m_clipSeconds) + " S" : timeText(m_clipSeconds) + " S";
        const float textW = std::ceil(juce::GlyphArrangement::getStringWidth(f, text)) + 1.0f;
        const auto textArea = juce::Rectangle<float>(area.getRight() - textW, area.getY(), textW, area.getHeight());
        drawText(g, text, textArea, juce::Justification::centredRight, f, playing ? m_playColour : Palette::dim);

        // The track: a hairline from the buttons to the time, filled as far as the clip has played.
        const float x0 = area.getX() + 12.0f, x1 = textArea.getX() - 12.0f, cy = std::round(area.getCentreY());
        if (x1 - x0 < 24.0f)
            return;
        g.setColour(Palette::rule.brighter(0.15f));
        g.fillRect(x0, cy, x1 - x0, 1.0f);
        g.fillRect(x1 - 1.0f, cy - 3.0f, 1.0f, 7.0f);
        if (playing)
        {
            const float at = x0 + (x1 - x0) * static_cast<float>(std::clamp(m_playSeconds / m_clipSeconds, 0.0, 1.0));
            g.setColour(m_playColour);
            g.fillRect(x0, cy, at - x0, 1.0f);
            g.fillRect(at - 1.0f, cy - 3.0f, 2.0f, 7.0f);
        }
        else
        {
            g.fillRect(x0, cy - 3.0f, 1.0f, 7.0f);
        }
    }

    // Learn button and scan message from the store's state; true if the scan shows a message.
    bool showSlotState(const SlotSnapshot& s, bool busy, bool learned, const juce::String& instruction)
    {
        m_learn.setLabel(s.capturing ? "LEARNING" : s.armed ? "ARMED" : "LEARN");
        m_learn.setToggleState(s.armed || s.capturing, juce::dontSendNotification);
        m_learn.setPulsing(s.armed && ! s.capturing);
        m_clear.setEnabled(s.samplesCaptured > 0 || learned);

        const bool waiting = s.armed && ! s.capturing;
        if (waiting != m_waiting)
        {
            m_waiting = waiting;
            m_motion.animateWhile(waiting);
            repaint();
        }
        m_scan.setActivity(s.capturing ? ScanDisplay::Activity::Capturing
                           : busy      ? ScanDisplay::Activity::Analysing
                           : s.armed   ? ScanDisplay::Activity::Armed
                                       : ScanDisplay::Activity::Idle,
                           s.capturing && s.fractionHeard ? static_cast<float>(*s.fractionHeard) : -1.0f);

        if (s.capturing)
        {
            const double secs = s.sampleRate > 0.0 ? static_cast<double>(s.samplesSinceArm) / s.sampleRate : 0.0;
            m_scan.setMessage("LEARNING  " + juce::String(secs, 1) + " S", m_bright);
            return true;
        }
        if (busy)
        {
            m_scan.setMessage("ANALYSING", m_textColour);
            return true;
        }
        if (! learned)
        {
            m_scan.setMessage(s.armed ? "ARMED: PRESS PREVIEW" : instruction, s.armed ? m_textColour : Palette::dim);
            return true;
        }
        m_scan.setMessage({}, Palette::dim);
        return false;
    }

    void setTexts(const juce::String& big, juce::Colour bigColour, const juce::String& dim, const juce::String& chipLabel)
    {
        if (big != m_big || dim != m_dim || bigColour != m_bigColour || chipLabel != m_chipLabel)
        {
            if (big != m_big)
            {
                m_bigChanged = nowSeconds();
                m_motion.animateFor(0.35);
            }
            m_big = big;
            m_dim = dim;
            m_bigColour = bigColour;
            m_chipLabel = chipLabel;
            repaint();
        }
    }

    juce::Rectangle<int> chipArea() const
    {
        using namespace PanelLayout;
        return { pad, chipsY, getWidth() - 2 * pad, chipsH };
    }

    juce::String m_title;
    juce::Colour m_bright, m_textColour, m_lineColour;
    ConsoleButton m_learn { "LEARN" }, m_clear { "CLEAR" };
    ScanDisplay m_scan;
    juce::String m_big, m_dim, m_chipLabel;
    juce::Colour m_bigColour = Palette::text;
    PlayheadLayer m_playhead;
    int m_transportLeft = PanelLayout::pad;   // where the play buttons end
    double m_clipSeconds = -1.0, m_playSeconds = -1.0;
    int m_shownTime = -1, m_shownPixel = -1;
    juce::Colour m_playColour = Palette::text;

    Motion m_motion { *this };
    bool m_waiting = false;
    double m_bigChanged = 0.0;
};

//==============================================================================
// 01 MUSIC

class MusicPanel : public SlotPanel
{
public:
    struct Chip
    {
        core::Target target = core::Target::Root;
        int pitchClass = 0;
    };

    MusicPanel() : SlotPanel("01 MUSIC", Palette::teal, Palette::tealText, Palette::tealLine)
    {
        m_learn.setTooltip("Arm, then press Preview in the AudioSuite window with the music selected. The selection is "
                           "learned as it plays, and Learn switches itself off at its end. Click again to disarm.");
        m_clear.setTooltip("Forget the learned music.");

        m_play.setAccent(Palette::teal, Palette::tealText, Palette::tealLine);
        m_play.setFontSize(9.5f, Weight::Medium, 0.12f);
        m_play.onClick = [this] { if (onPlay) onPlay(Playback::Music); };
        addAndMakeVisible(m_play);
        m_playhead.read = []
        {
            PlayheadLayer::Reading r;
            auto& bus = AuditionBus::instance();
            const auto p = bus.playhead();
            if (! bus.previewRunning() || bus.playing() != Playback::Music || p.what != Playback::Music)
                return r;
            r.show = true;
            r.seconds = p.seconds;
            r.moving = p.moving;
            r.atNs = p.atNs;
            r.colour = Palette::teal;
            return r;
        };
        setPlayColour(Palette::tealText);

        for (std::size_t i = 0; i < m_chips.size(); ++i)
        {
            auto& chip = m_chips[i];
            chip.setAccent(Palette::teal, Palette::tealText, Palette::tealLine);
            chip.setFontSize(9.5f, Weight::Medium, 0.1f);
            chip.onClick = [this, i] { if (onTarget && i < m_chipDefs.size()) onTarget(m_chipDefs[i].target, m_chipDefs[i].pitchClass); };
            addChildComponent(chip);
        }
    }

    std::function<void(core::Target, int)> onTarget;
    std::function<void(Playback)> onPlay;

    void showPlayback(PlayState state, const juce::String& tip) { showPlayButton(m_play, state, "PLAY MUSIC", tip); }

    void setData(const HubSnapshot& hub, const SlotSnapshot& slot)
    {
        const auto& m = hub.music;
        const bool learned = hub.hasMusic;
        // What is played back, and how long it is.
        AuditionBus::instance().setMusic(hub.musicChannels, hub.musicRate);
        setClipLength(learned && hub.musicChannels != nullptr && ! hub.musicChannels->empty() && hub.musicRate > 0.0
                          ? static_cast<double>((*hub.musicChannels)[0].size()) / hub.musicRate
                          : -1.0);
        const bool message = showSlotState(slot, hub.musicBusy, learned, "ARM LEARN, SELECT THE MUSIC, PRESS PREVIEW");

        if (learned && ! m.valid && ! message)
            m_scan.setMessage("NO NOTES FOUND", m_textColour);

        if (learned && m.valid)
        {
            const double rootHz = core::midiToHz(m.rootMidi);
            setTexts(upper(m.keyName()), Palette::text,
                     "ROOT " + upper(core::noteName(m.rootMidi)) + " / " + juce::String(rootHz, 1) + " HZ / KEY "
                         + juce::String(juce::roundToInt(m.keyConfidence * 100.0f)) + "% SURE / TUNING "
                         + signedNumber(static_cast<double>(m.tuningCents), 0) + " C",
                     "TARGET NOTE");
        }
        else
        {
            setTexts("--", Palette::dim, learned ? "NOTHING TO MATCH TO" : "NOTHING LEARNED YET", "TARGET NOTE");
        }

        updateChips(hub);

        // The lane picture, rebuilt only when the analysis itself changed.
        const auto signature = juce::String(m.laneFrames) + "/" + juce::String(m.lanePitches) + "/" + juce::String(m.durationSeconds, 6)
                             + "/" + juce::String(m.rootPitchClass) + "/" + (learned && m.valid ? "1" : "0");
        if (signature != m_signature || ! m_haveImage)
        {
            m_signature = signature;
            m_haveImage = true;
            m_laneMap = {};
            m_scan.setPicture(learned && m.valid ? renderLane(m, hub.match.valid ? hub.match.targetPitchClass : -1) : juce::Image());
            m_playhead.setMap(m_laneMap);
        }
    }

    void resized() override
    {
        SlotPanel::resized();
        m_play.setBounds(PanelLayout::pad, PanelLayout::transportY, kPlayW, PanelLayout::transportH);
        m_transportLeft = m_play.getRight();
        layoutChips();
    }

    static constexpr int kPlayW = 118;   // as wide as the effect's PLAY INPUT beside it

private:
    void updateChips(const HubSnapshot& hub)
    {
        const auto& m = hub.music;
        const bool valid = hub.hasMusic && m.valid;
        const int root = m.rootPitchClass;
        const int fifth = (root + 7) % 12;
        const int third = (root + (m.minor ? 3 : 4)) % 12;

        std::vector<Chip> defs { { core::Target::Follow, 0 }, { core::Target::Root, root }, { core::Target::Fifth, fifth },
                                 { core::Target::Third, third }, { core::Target::Nearest, 0 } };
        std::vector<juce::String> labels { "FOLLOW",
                                           valid ? "ROOT " + juce::String(core::pitchClassName(root)) : juce::String("ROOT"),
                                           valid ? "5TH " + juce::String(core::pitchClassName(fifth)) : juce::String("5TH"),
                                           valid ? "3RD " + juce::String(core::pitchClassName(third)) : juce::String("3RD"),
                                           "NEAREST" };
        std::vector<juce::String> tips {
            "Rests the effect on the nearest note the music is sounding and keeps it there while the music holds that note. "
            "When the note stops, the effect moves to the nearest note sounding then. It does not play the tune. Where the "
            "music holds one note, this is the same as Nearest.",
            "Shift the effect onto the music's root note, in the octave nearest the effect.",
            "Shift the effect onto the fifth above the music's root.",
            "Shift the effect onto the music's third (minor or major, as the music is).",
            "Shift the effect onto whichever clear note of the music needs the smallest move." };

        const auto addNote = [&](int pc)
        {
            if (defs.size() >= m_chips.size() || pc == root || pc == fifth || pc == third)
                return;
            for (const auto& d : defs)
                if (d.target == core::Target::PitchClass && d.pitchClass == pc)
                    return;
            defs.push_back({ core::Target::PitchClass, pc });
            labels.push_back(core::pitchClassName(pc));
            tips.push_back("Shift the effect onto " + juce::String(core::pitchClassName(pc)) + ", one of the strong notes in the music.");
        };
        // A chosen note always keeps its chip, so the selection stays visible.
        if (hub.options.target == core::Target::PitchClass)
            addNote(hub.options.pitchClass);
        if (valid)
            for (const auto& n : m.notes)
                addNote(n.pitchClass);

        m_chipDefs = defs;
        for (std::size_t i = 0; i < m_chips.size(); ++i)
        {
            auto& chip = m_chips[i];
            const bool used = i < defs.size();
            chip.setVisible(used);
            if (! used)
                continue;
            chip.setLabel(labels[i]);
            chip.setTooltip(tips[i]);
            const auto& d = defs[i];
            const bool selected = hub.options.target == d.target
                                  && (d.target != core::Target::PitchClass || hub.options.pitchClass == d.pitchClass);
            chip.setToggleState(selected, juce::dontSendNotification);
        }
        layoutChips();
    }

    void layoutChips()
    {
        auto area = chipArea();
        for (std::size_t i = 0; i < m_chips.size(); ++i)
        {
            auto& chip = m_chips[i];
            if (! chip.isVisible())
                continue;
            const int width = chip.getButtonText().length() <= 2 ? 36 : chip.getButtonText().length() <= 4 ? 46 : 70;
            chip.setBounds(area.removeFromLeft(width));
            area.removeFromLeft(6);
        }
    }

    // The music as a short list of the notes it holds, strongest eight, highest at the top. Each
    // row is one note: its name, when it sounds across the learned passage (brighter = stronger),
    // and how strong it is overall. The root and the note being matched to are marked.
    juce::Image renderLane(const core::MusicAnalysis& m, int targetPitchClass)
    {
        const int w = m_scan.getWidth(), h = m_scan.getHeight();
        if (w <= 140 || h <= 40 || m.laneFrames <= 0 || m.lanePitches <= 0)
            return {};

        struct Row { int pitch; float total; };
        std::vector<Row> rows;
        float strongest = 0.0f;
        for (int p = 0; p < m.lanePitches; ++p)
        {
            float total = 0.0f;
            for (int f = 0; f < m.laneFrames; ++f)
                total += m.lane[static_cast<std::size_t>(f * m.lanePitches + p)];
            strongest = std::max(strongest, total);
            rows.push_back({ p, total });
        }
        if (strongest <= 0.0f)
            return {};
        std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y)
        {
            if (x.total > y.total) return true;
            if (x.total < y.total) return false;
            return x.pitch < y.pitch;
        });
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& r) { return r.total < 0.12f * strongest; }), rows.end());
        if (rows.size() > 8)
            rows.resize(8);
        std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.pitch > y.pitch; });

        juce::Image image(juce::Image::ARGB, w, h, true);
        juce::Graphics g(image);

        const float labelW = 40.0f, tagW = 64.0f, left = 10.0f;
        const float stripX = left + labelW, stripW = static_cast<float>(w) - stripX - tagW - 8.0f;
        const float rowH = std::min(30.0f, static_cast<float>(h - 12) / static_cast<float>(std::max<std::size_t>(rows.size(), 5)));
        const float top = 0.5f * (static_cast<float>(h) - rowH * static_cast<float>(rows.size()));
        const int columns = static_cast<int>(stripW);

        // The playhead runs down the note rows, on the same time scale as their strips.
        m_laneMap.x0 = stripX;
        m_laneMap.width = static_cast<float>(columns);
        // The whole height of the view, like the effect's: with only a few notes the rows are a
        // short band, and a playhead that long would be easy to miss.
        m_laneMap.top = 6.0f;
        m_laneMap.height = static_cast<float>(h) - 12.0f;
        m_laneMap.duration = static_cast<double>(m.laneFrames) * m.hopSeconds;
        m_laneMap.valid = m_laneMap.duration > 0.0 && ! rows.empty();

        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows[i];
            const int midi = m.laneLowMidi + row.pitch;
            const int pitchClass = ((midi % 12) + 12) % 12;
            const bool isRoot = pitchClass == m.rootPitchClass;
            const bool isTarget = pitchClass == targetPitchClass;
            const float y = top + static_cast<float>(i) * rowH;
            const float barH = std::max(4.0f, rowH - 10.0f), barY = y + 0.5f * (rowH - barH);

            if (isTarget)
            {
                g.setColour(Palette::tealLine.withAlpha(0.35f));
                g.fillRect(2.0f, y + 1.0f, static_cast<float>(w) - 4.0f, rowH - 2.0f);
            }

            const auto name = juce::String(core::pitchClassName(pitchClass)) + juce::String(midi / 12 - 1);
            drawText(g, name, { left, y, labelW - 4.0f, rowH }, juce::Justification::centredLeft, font(Weight::Bold, 11.0f, 0.04f),
                     isTarget ? Palette::text : isRoot ? Palette::tealText : Palette::tealText.withAlpha(0.7f));

            // When the note sounds: a track, and on it the note's strength over time.
            g.setColour(Palette::rule);
            g.fillRect(stripX, barY + 0.5f * barH - 0.5f, stripW, 1.0f);
            for (int x = 0; x < columns; ++x)
            {
                const int fa = x * m.laneFrames / columns;
                const int fb = std::max(fa + 1, (x + 1) * m.laneFrames / columns);
                float v = 0.0f;
                for (int f = fa; f < fb && f < m.laneFrames; ++f)
                    v = std::max(v, m.lane[static_cast<std::size_t>(f * m.lanePitches + row.pitch)]);
                if (v < 0.05f)
                    continue;
                const float level = std::clamp(std::pow(v, 0.7f), 0.0f, 1.0f);
                const float hNow = barH * (0.35f + 0.65f * level);
                g.setColour(Palette::teal.withAlpha(0.35f + 0.65f * level));
                g.fillRect(stripX + static_cast<float>(x), barY + 0.5f * (barH - hNow), 1.0f, hNow);
            }

            // Overall strength, and what the note is to the match.
            const float tagX = stripX + stripW + 8.0f;
            const juce::String tag = isTarget ? "TARGET" : isRoot ? "ROOT" : juce::String(juce::roundToInt(100.0f * row.total / strongest)) + "%";
            drawText(g, tag, { tagX, y, tagW - 6.0f, rowH }, juce::Justification::centredLeft, font(Weight::Medium, 9.5f, 0.08f),
                     isTarget ? Palette::text : isRoot ? Palette::tealText : Palette::dim);
        }
        return image;
    }

    std::array<ConsoleButton, 6> m_chips;
    std::vector<Chip> m_chipDefs;
    juce::String m_signature;
    bool m_haveImage = false;
    ConsoleButton m_play { "PLAY MUSIC" };
    PlayheadLayer::Map m_laneMap;
};

//==============================================================================
// 02 SOUND EFFECT

class SfxPanel : public SlotPanel
{
public:
    SfxPanel() : SlotPanel("02 SOUND EFFECT", Palette::amber, Palette::amber, Palette::amberLine)
    {
        m_learn.setTooltip("Arm, then press Preview in the AudioSuite window with the sound effect selected. The selection is "
                           "learned as it plays, and Learn switches itself off at its end. Click again to disarm.");
        m_clear.setTooltip("Forget the learned sound effect.");
        const char* tips[] {
            "Chooses the pitch tracking that suits the effect: Tonal, or Engine where that fits better, and Resonance when neither finds a pitch.",
            "For voices, single tones and other sounds with one clear pitch.",
            "For sounds whose overtones all move together, such as engines and rich drones.",
            "For sounds with no pitch to follow: uses the strongest ring in the sound." };
        const core::SfxMethod methods[] { core::SfxMethod::Auto, core::SfxMethod::Tonal, core::SfxMethod::Engine, core::SfxMethod::Resonance };
        for (std::size_t i = 0; i < m_methods.size(); ++i)
        {
            auto& chip = m_methods[i];
            chip.setLabel(methodName(methods[i]));
            chip.setTooltip(tips[i]);
            chip.setAccent(Palette::amber, Palette::amber, Palette::amberLine);
            chip.setFontSize(9.5f, Weight::Medium, 0.1f);
            const auto method = methods[i];
            chip.onClick = [this, method] { if (onMethod) onMethod(method); };
            addAndMakeVisible(chip);
        }

        m_editor.setTooltip("Click anywhere to listen from that moment. Drag to raise or lower the pitch there: each place "
                    "you pull stays as a diamond, and double-clicking a diamond removes it. Shift moves finely, Alt "
                    "ignores the snap. LOCKED, SUBTLE and MOVING are three ready-made curves: click a grey name to "
                    "switch to it (it sets the target and Bend). Hatched stretches lie outside the learned music.");
        m_editor.onChange = [this](const std::vector<core::CurvePoint>& points) { if (onEdits) onEdits(points); };
        m_editor.onHold = [this](double seconds, double semitones)
        {
            if (g_tools.hear)
            {
                AuditionBus::instance().hold(seconds, semitones);
                noteAudition();
            }
        };
        m_editor.onRelease = [this](double seconds)
        {
            // With the effect playing as stamped, it carries on from here whatever HEAR says.
            if (g_tools.hear || AuditionBus::instance().playing() == Playback::Output)
            {
                AuditionBus::instance().playFrom(seconds);
                noteAudition();
            }
        };
        m_editor.onEdited = [this](const std::vector<core::CurvePoint>& before) { remember(before); };
        m_editor.onStyle = [this](int style) { if (onStyle) onStyle(style); };
        m_editor.onUndoKey = [this](bool redo) { stepHistory(redo); };

        for (auto* b : { &m_undoButton, &m_redoButton })
        {
            b->setAccent(Palette::amber, Palette::amber, Palette::amberLine);
            b->setFontSize(9.0f, Weight::Medium, 0.14f);
            b->setPlain(true, juce::Justification::centredRight);
            addChildComponent(*b);
        }
        m_undoButton.setTooltip("Takes back the last hand edit to the curve (a point, a drawn line, or Reset Curve). Cmd+Z does "
                        "the same after clicking the scan, where the host passes the key on.");
        m_redoButton.setTooltip("Puts back the change that was just undone.");
        m_undoButton.onClick = [this] { stepHistory(false); };
        m_redoButton.onClick = [this] { stepHistory(true); };
        addChildComponent(m_editor);

        // Editing tools, as text toggles on the label row.
        for (auto* b : { &m_draw, &m_snap, &m_hear })
        {
            b->setAccent(Palette::amber, Palette::dim, Palette::amberLine);
            b->setFontSize(9.0f, Weight::Medium, 0.14f);
            b->setPlain(true, juce::Justification::centredRight);
            addChildComponent(*b);
        }
        m_smooth.setAccent(Palette::amber, Palette::dim, Palette::amberLine);
        m_smooth.setFontSize(9.0f, Weight::Medium, 0.14f);
        m_smooth.setPlain(true, juce::Justification::centredRight);
        m_smooth.setTooltip("How gently the effect moves between the notes it rests on. Off: quick steps. Some: short slides. "
                    "Full: long ramps from one note to the next. Hand edits are not smoothed: they come out as drawn.");
        m_smooth.onClick = [this]
        {
            if (onSmooth)
                onSmooth(m_smoothValue < 0.2 ? 0.4 : m_smoothValue < 0.75 ? 1.0 : 0.0);
        };
        addChildComponent(m_smooth);

        m_draw.setTooltip("Freehand: draw the pitch line with the mouse. Off: pull points on the line.");
        m_draw.onClick = [this] { g_tools.draw = ! g_tools.draw; showTools(); };
        m_snap.setTooltip("Snaps edits to the music's notes, or only to the notes of its scale. Hold Alt while editing to ignore it.");
        m_snap.onClick = [this] { g_tools.snap = (g_tools.snap + 1) % 3; showTools(); };
        m_hear.setTooltip("With Preview running in the AudioSuite window, the effect sounds as you edit, through the same "
                  "engine Stamp renders with: held at the pitch you are dragging, then on from there along the "
                  "curve when you let go. Click without dragging to listen from that point. The session must be at "
                  "the sample rate the effect was learned at.");
        m_hear.onClick = [this]
        {
            g_tools.hear = ! g_tools.hear;
            auto& bus = AuditionBus::instance();
            if (! g_tools.hear && bus.playing() == Playback::Output && ! bus.startedByPlay())
                bus.stop();   // an edit's sound (PLAY OUTPUT carries on)
            showTools();
        };
        showTools();

        // Playing the effect as learned and as it will be stamped.
        for (auto* b : { &m_playInput, &m_playOutput })
        {
            b->setAccent(Palette::amber, Palette::amber, Palette::amberLine);
            b->setFontSize(9.5f, Weight::Medium, 0.12f);
            addAndMakeVisible(*b);
        }
        m_playInput.onClick = [this] { if (onPlay) onPlay(Playback::Input); };
        m_playOutput.onClick = [this] { if (onPlay) onPlay(Playback::Output); };
        // The line on the scan: quiet for the effect as learned, amber for the effect as stamped
        // (played from here, or by Preview itself with Stamp on).
        m_playhead.read = []
        {
            PlayheadLayer::Reading r;
            auto& bus = AuditionBus::instance();
            const auto role = bus.playing();
            auto p = bus.playhead();
            if (role == Playback::Input || role == Playback::Output)
            {
                if (! bus.previewRunning() || p.what != role)
                    return r;
            }
            else
            {
                p = bus.stampPlayhead();
                if (p.what == Playback::None)
                    return r;
            }
            r.show = true;
            r.seconds = p.seconds;
            r.moving = p.moving;
            r.atNs = p.atNs;
            r.strong = p.what == Playback::Output;
            r.colour = r.strong ? Palette::amber : Palette::text;
            return r;
        };
        m_playhead.toFront(false);   // over the curve editor

        m_resetEdits.setAccent(Palette::amber, Palette::amber, Palette::amberLine);
        m_resetEdits.setFontSize(9.0f, Weight::Medium, 0.14f);
        m_resetEdits.setPlain(true, juce::Justification::centredRight);
        m_resetEdits.setTooltip("Removes every hand adjustment from the curve.");
        m_resetEdits.onClick = [this]
        {
            remember(m_edits);
            if (onEdits)
                onEdits({});
        };
        addChildComponent(m_resetEdits);
    }

    std::function<void(core::SfxMethod)> onMethod;
    std::function<void(const std::vector<core::CurvePoint>&)> onEdits;
    std::function<void(double)> onSmooth;
    std::function<void(int)> onStyle;
    std::function<void(Playback)> onPlay;

    void showPlayback(PlayState input, const juce::String& inputTip, PlayState output, const juce::String& outputTip)
    {
        showPlayButton(m_playInput, input, "PLAY INPUT", inputTip);
        showPlayButton(m_playOutput, output, "PLAY OUTPUT", outputTip);
        // The time reads in the colour of the line it goes with.
        const auto role = AuditionBus::instance().playing();
        setPlayColour(role == Playback::Input ? Palette::text : Palette::amber);
    }

    void setData(const HubSnapshot& hub, const SlotSnapshot& slot, bool hubChanged)
    {
        const auto& s = hub.sfx;
        const bool learned = hub.hasSfx;
        setClipLength(learned && hub.sfxChannels != nullptr && ! hub.sfxChannels->empty() && hub.sfxRate > 0.0
                          ? static_cast<double>((*hub.sfxChannels)[0].size()) / hub.sfxRate
                          : -1.0);
        const bool message = showSlotState(slot, hub.sfxBusy, learned, "ARM LEARN, SELECT THE EFFECT, PRESS PREVIEW");

        if (learned && s.valid && s.pitched)
        {
            int cents = 0;
            const auto name = upper(core::noteName(s.centreMidi, &cents));
            setTexts(name + " " + signedNumber(cents, 0) + "C", Palette::text,
                     "DRIFT " + juce::String(s.driftSemitones, 1) + " ST / PITCH " + juce::String(juce::roundToInt(s.tonalness * 100.0f))
                         + "% CLEAR / " + methodName(s.usedMethod) + " TRACKING",
                     "PITCH TRACKING");
        }
        else if (learned)
        {
            if (! message)
                m_scan.setMessage("NO CLEAR PITCH", m_textColour);
            setTexts("NO PITCH", Palette::dim, s.valid ? "PITCH " + juce::String(juce::roundToInt(s.tonalness * 100.0f)) + "% CLEAR / " + methodName(s.usedMethod) + " TRACKING"
                                                       : juce::String("TOO SHORT OR SILENT"),
                     "PITCH TRACKING");
        }
        else
        {
            setTexts("--", Palette::dim, "NOTHING LEARNED YET", "PITCH TRACKING");
        }

        for (std::size_t i = 0; i < m_methods.size(); ++i)
            m_methods[i].setToggleState(static_cast<int>(hub.options.method) == static_cast<int>(i), juce::dontSendNotification);

        const bool drawable = learned && s.valid && s.pitched;
        m_edits = hub.options.edits;
        if (hub.sfxChannels != m_historyFor)
        {
            // Another effect: the old one's history means nothing here.
            m_historyFor = hub.sfxChannels;
            m_undo.clear();
            m_redo.clear();
        }
        if (hubChanged || ! m_haveImage)
        {
            m_haveImage = true;
            m_axis.valid = m_axis.valid && m_editor.isDragging();   // the pitch axis holds still under a drag
            m_scan.setPicture(drawable ? renderCurves(hub) : juce::Image(), m_editor.isDragging());
            PlayheadLayer::Map map;
            if (drawable && m_axis.valid)
                map = { m_axis.x0, m_axis.plotW, m_axis.top, m_axis.plotH, m_axis.duration, true };
            m_playhead.setMap(map);
            m_editor.setData(m_axis, hub.options.edits, hub.match.afterMidi, hub.match.hopSeconds, hub.match.coveredFrom, hub.match.coveredTo,
                             hub.options.bend > 1.0e-6, hub.options.target == core::Target::Follow || hub.options.bend > 1.0e-6,
                             s.centreMidi, hub.music);
            m_editor.setStyles(hub.styleAfterMidi, hub.options.style());
            AuditionBus::instance().setSource(hub.sfxChannels, hub.sfxRate, hub.options.preserveFormants);
            AuditionBus::instance().setCurve(hub.match.curve, hub.match.hopSeconds);
        }
        const bool editable = drawable && hub.match.valid && m_axis.valid && ! message;
        // Nothing to hear the curve on: the effect as stamped stops (as learned plays on).
        if (! editable && AuditionBus::instance().playing() == Playback::Output)
            AuditionBus::instance().stop();
        m_editor.setVisible(editable);
        m_resetEdits.setVisible(editable && ! hub.options.edits.empty());
        for (auto* b : { &m_draw, &m_snap, &m_hear })
            b->setVisible(editable);
        m_smoothValue = hub.options.glide;
        m_smooth.setLabel(m_smoothValue < 0.2 ? "SMOOTH OFF" : m_smoothValue < 0.75 ? "SMOOTH SOME" : "SMOOTH FULL");
        m_smooth.setToggleState(m_smoothValue >= 0.2, juce::dontSendNotification);
        m_smooth.setVisible(editable);
        m_undoButton.setVisible(editable && ! m_undo.empty());
        m_redoButton.setVisible(editable && ! m_redo.empty());
    }

    void resized() override
    {
        SlotPanel::resized();
        auto area = chipArea();
        const int widths[] { 52, 58, 62, 92 };
        for (std::size_t i = 0; i < m_methods.size(); ++i)
        {
            m_methods[i].setBounds(area.removeFromLeft(widths[i]));
            area.removeFromLeft(6);
        }
        m_editor.setBounds(m_scan.getBounds());
        auto transport = juce::Rectangle<int>(PanelLayout::pad, PanelLayout::transportY, getWidth() - 2 * PanelLayout::pad, PanelLayout::transportH);
        m_playInput.setBounds(transport.removeFromLeft(kInputW));
        transport.removeFromLeft(6);
        m_playOutput.setBounds(transport.removeFromLeft(kOutputW));
        m_transportLeft = m_playOutput.getRight();
        // Reset, redo and undo sit at the right of the readout line, above the tools.
        auto history = juce::Rectangle<int>(PanelLayout::pad, PanelLayout::dimY - 2, getWidth() - 2 * PanelLayout::pad, 18);
        m_resetEdits.setBounds(history.removeFromRight(92));
        history.removeFromRight(14);
        m_redoButton.setBounds(history.removeFromRight(38));
        history.removeFromRight(14);
        m_undoButton.setBounds(history.removeFromRight(38));
        // Right to left along the label row: SMOOTH, HEAR, SNAP, DRAW.
        auto row = juce::Rectangle<int>(PanelLayout::pad, PanelLayout::labelY - 3, getWidth() - 2 * PanelLayout::pad, 18);
        m_smooth.setBounds(row.removeFromRight(90));
        row.removeFromRight(10);
        m_hear.setBounds(row.removeFromRight(62));
        row.removeFromRight(10);
        m_snap.setBounds(row.removeFromRight(84));
        row.removeFromRight(10);
        m_draw.setBounds(row.removeFromRight(62));
    }

private:
    // The curve's history: every finished edit can be taken back, and put back again.
    void remember(const std::vector<core::CurvePoint>& before)
    {
        m_undo.push_back(before);
        if (m_undo.size() > 100)
            m_undo.erase(m_undo.begin());
        m_redo.clear();
        m_undoButton.setVisible(true);
        m_redoButton.setVisible(false);
    }

    void stepHistory(bool redo)
    {
        auto& from = redo ? m_redo : m_undo;
        auto& to = redo ? m_undo : m_redo;
        if (from.empty())
            return;
        to.push_back(m_edits);
        const auto points = from.back();
        from.pop_back();
        if (onEdits)
            onEdits(points);
    }

    // An audition was asked for. Without Preview running there is nowhere for it to sound: note
    // that, so the editor can say to press Preview.
    void noteAudition()
    {
        if (! AuditionBus::instance().previewRunning())
            m_auditionUnheardAt = nowSeconds();
    }

public:
    /** True for a few seconds after an edit that could not be heard because Preview is not running. */
    bool auditionNeedsPreview() const
    {
        return g_tools.hear && m_auditionUnheardAt > 0.0 && nowSeconds() - m_auditionUnheardAt < 6.0
               && ! AuditionBus::instance().previewRunning();
    }

private:
    void showTools()
    {
        m_draw.setToggleState(g_tools.draw, juce::dontSendNotification);
        m_draw.setLabel(g_tools.draw ? "DRAW ON" : "DRAW OFF");
        m_hear.setLabel(g_tools.hear ? "HEAR ON" : "HEAR OFF");
        m_snap.setToggleState(g_tools.snap != 0, juce::dontSendNotification);
        m_snap.setLabel(g_tools.snap == 0 ? "SNAP OFF" : g_tools.snap == 1 ? "SNAP NOTES" : "SNAP SCALE");
        m_hear.setToggleState(g_tools.hear, juce::dontSendNotification);
        m_editor.repaint();
    }

    juce::Image renderCurves(const HubSnapshot& hub)
    {
        const auto& s = hub.sfx;
        const auto& r = hub.match;
        const int w = m_scan.getWidth(), h = m_scan.getHeight();
        const std::size_t n = s.hz.size();
        if (w <= PanelLayout::gutter + 4 || h <= 8 || n < 2)
            return {};

        // The pitch axis: everything that is drawn, plus a margin, at least an octave.
        double lo = s.centreMidi, hi = s.centreMidi;
        const auto include = [&](double midi) { lo = std::min(lo, midi); hi = std::max(hi, midi); };
        for (float hz : s.hz)
            if (hz > 0.0f)
                include(core::hzToMidi(hz));
        if (r.valid)
        {
            include(r.targetMidi);
            for (float hz : r.afterHz)
                if (hz > 0.0f)
                    include(core::hzToMidi(hz));
            // Room for the curves on offer too, so every one of them can be seen and picked.
            for (const auto& curve : hub.styleAfterMidi)
                for (float midi : curve)
                    include(midi);
        }
        lo = std::floor(lo - 1.5);
        hi = std::ceil(hi + 1.5);
        if (hi - lo < 12.0)
        {
            const double mid = 0.5 * (lo + hi);
            lo = mid - 6.0;
            hi = mid + 6.0;
        }
        if (m_axis.valid)
        {
            // Mid-drag: keep the axis the drag started on, or the line would move under the mouse.
            lo = m_axis.lo;
            hi = m_axis.hi;
        }

        const float x0 = static_cast<float>(PanelLayout::gutter), plotW = static_cast<float>(w) - x0 - 4.0f;
        const float top = 6.0f, plotH = static_cast<float>(h) - 12.0f;
        const auto yFor = [&](double midi) { return top + static_cast<float>((hi - midi) / (hi - lo)) * plotH; };
        const auto xFor = [&](std::size_t i) { return x0 + plotW * static_cast<float>(i) / static_cast<float>(n - 1); };

        m_axis.lo = lo;
        m_axis.hi = hi;
        m_axis.x0 = x0;
        m_axis.plotW = plotW;
        m_axis.top = top;
        m_axis.plotH = plotH;
        m_axis.duration = static_cast<double>(n - 1) * s.hopSeconds;
        m_axis.valid = true;

        juce::Image image(juce::Image::ARGB, w, h, true);
        juce::Graphics g(image);

        // Semitone grid, with the octave lines a little stronger.
        for (int midi = static_cast<int>(std::ceil(lo)); midi <= static_cast<int>(std::floor(hi)); ++midi)
        {
            g.setColour(midi % 12 == 0 ? Palette::rule.brighter(0.25f) : Palette::rule);
            g.fillRect(x0, std::round(yFor(midi)), plotW, 1.0f);
        }

        const auto curve = [&](const std::vector<float>& hz)
        {
            juce::Path p;
            bool open = false;
            for (std::size_t i = 0; i < n && i < hz.size(); ++i)
            {
                if (hz[i] <= 0.0f)
                {
                    open = false;
                    continue;
                }
                const juce::Point<float> pt { xFor(i), yFor(core::hzToMidi(hz[i])) };
                if (open)
                    p.lineTo(pt);
                else
                    p.startNewSubPath(pt);
                open = true;
            }
            return p;
        };

        const auto label = [&](const juce::String& text, double midi, juce::Colour colour)
        {
            const float th = 9.0f;
            const float y = std::clamp(yFor(midi) - 0.5f * th, 0.0f, static_cast<float>(h) - th);
            drawText(g, text, { 3.0f, y, x0 - 5.0f, th }, juce::Justification::centredLeft, font(Weight::Medium, 8.5f, 0.04f), colour);
        };

        // Before: the tracked pitch, dashed.
        {
            juce::Path dashed;
            const float dashes[] { 4.0f, 3.0f };
            juce::PathStrokeType(1.2f).createDashedStroke(dashed, curve(s.hz), dashes, 2);
            g.setColour(Palette::amber.withAlpha(0.6f));
            g.fillPath(dashed);
        }
        label(upper(core::noteName(s.centreMidi)), s.centreMidi, Palette::amber.withAlpha(0.7f));

        if (r.valid)
        {
            // The target: one note, or when the effect moves with the music, the line it moves along.
            g.setColour(Palette::teal);
            if (r.notesFollowed >= 2 && r.afterMidi.size() == n)
            {
                juce::Path steps;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const juce::Point<float> pt { xFor(i), yFor(r.afterMidi[i]) };
                    if (i == 0)
                        steps.startNewSubPath(pt);
                    else
                        steps.lineTo(pt);
                }
                g.strokePath(steps, juce::PathStrokeType(1.2f));
            }
            else
            {
                g.fillRect(x0, std::round(yFor(r.targetMidi)) - 0.5f, plotW, 1.0f);
            }
            g.setColour(Palette::amber);
            g.strokePath(curve(r.afterHz), juce::PathStrokeType(1.8f, juce::PathStrokeType::curved));
            if (std::abs(r.targetMidi - s.centreMidi) > 0.9)
                label(upper(core::noteName(r.targetMidi)), r.targetMidi, Palette::tealText);
        }
        return image;
    }

    std::array<ConsoleButton, 4> m_methods;
    CurveEditor m_editor;
    CurveEditor::Axis m_axis;
    double m_auditionUnheardAt = 0.0;
    ConsoleButton m_resetEdits { "RESET CURVE" }, m_draw { "DRAW" }, m_snap { "SNAP OFF" }, m_hear { "HEAR" };
    ConsoleButton m_undoButton { "UNDO" }, m_redoButton { "REDO" }, m_smooth { "SMOOTH OFF" };
    ConsoleButton m_playInput { "PLAY INPUT" }, m_playOutput { "PLAY OUTPUT" };
    static constexpr int kInputW = 118, kOutputW = 130;
    double m_smoothValue = 0.0;
    std::vector<core::CurvePoint> m_edits;
    std::vector<std::vector<core::CurvePoint>> m_undo, m_redo;
    std::shared_ptr<const AuditionBus::Channels> m_historyFor;
    bool m_haveImage = false;
};

//==============================================================================

class WarningStrip : public juce::Component
{
public:
    WarningStrip() { setInterceptsMouseClicks(false, false); }

    void setLines(const juce::StringArray& lines)
    {
        if (lines != m_lines)
        {
            m_lines = lines;
            m_changed = nowSeconds();
            m_motion.animateFor(0.4);
            m_motion.animateWhile(! lines.isEmpty());   // the marker blinks while there is something to read
            repaint();
        }
    }

    // What to do next, shown when there is nothing to warn about.
    void setHint(const juce::String& hint)
    {
        if (hint != m_hint)
        {
            m_hint = hint;
            if (m_lines.isEmpty())
            {
                m_changed = nowSeconds();
                m_motion.animateFor(0.4);
            }
            repaint();
        }
    }

    int wantedHeight() const { return m_lines.size() > 1 ? 40 : 24; }

    void paint(juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        const bool ready = m_lines.isEmpty();
        drawFrame(g, r, 6.0f, Palette::panel, ready ? Palette::amberLine.withMultipliedAlpha(0.7f) : Palette::amberLine);
        auto text = r.reduced(12.0f, 5.0f);
        const auto f = font(Weight::Medium, 10.0f, 0.06f);

        // New text slides in from the left as it brightens.
        const float in = easeOut(progressSince(m_changed, 0.35));
        const float slide = (1.0f - in) * 12.0f;

        // A marker block ahead of the text: steady when ready, blinking for a message.
        const auto marker = juce::Rectangle<float>(text.getX(), text.getY() + 2.5f, 5.0f, 10.0f);
        text.removeFromLeft(13.0f);
        if (ready)
        {
            g.setColour(Palette::tealLine);
            g.fillRect(marker);
            drawText(g, m_hint.isEmpty() ? juce::String("READY") : m_hint, text.translated(-slide, 0.0f), juce::Justification::centredLeft, f,
                     Palette::tealText.withMultipliedAlpha(0.75f * in));
            return;
        }
        g.setColour(Palette::amber.withAlpha(pulse(1.6) > 0.5f ? 1.0f : 0.25f));
        g.fillRect(marker);
        // Two lines at most; if more are waiting, the second line says how many.
        for (int i = 0; i < std::min(2, m_lines.size()); ++i)
        {
            auto line = text.removeFromTop(15.0f).translated(-slide, 0.0f);
            if (i == 1 && m_lines.size() > 2)
                drawText(g, "+" + juce::String(m_lines.size() - 2) + " MORE", line.removeFromRight(70.0f), juce::Justification::centredRight, f,
                         Palette::amber.withMultipliedAlpha(in));
            drawText(g, m_lines[i], line, juce::Justification::centredLeft, f, Palette::amber.withMultipliedAlpha(in));
        }
    }

private:
    juce::StringArray m_lines;
    juce::String m_hint;
    double m_changed = 0.0;
    Motion m_motion { *this };
};

//==============================================================================
// The bottom panel: the shift, how it behaves, and Stamp.

class ShiftPanel : public juce::Component
{
public:
    ShiftPanel()
    {
        m_bend.setTooltip("Left flattens the effect's own pitch wobble so it holds its note. Right adds the inflection of "
                  "the music's lead voice: where the voice slides into a note or drifts, the effect leans the same "
                  "way, and while the voice holds a note the effect stays on its own. Centre leaves the effect "
                  "alone. Double-click for the centre.");
        m_bend.onValueChange = [this] { if (! m_updating && onBend) onBend(m_bend.getValue()); repaint(); };
        addAndMakeVisible(m_bend);

        for (auto* b : { &m_down, &m_up })
        {
            b->setAccent(Palette::amber, Palette::amber, Palette::amberLine);
            b->setFontSize(12.0f, Weight::Bold, 0.0f);
            addAndMakeVisible(*b);
        }
        m_down.setTooltip("Move the target an octave down.");
        m_up.setTooltip("Move the target an octave up.");
        m_down.onClick = [this] { if (onOctave) onOctave(-1); };
        m_up.onClick = [this] { if (onOctave) onOctave(1); };

        m_formant.setAccent(Palette::amber, Palette::amber, Palette::amberLine);
        m_formant.setFontSize(9.5f);
        m_formant.setTooltip("Keeps the effect's tone colour when it is shifted, so it does not sound smaller or bigger. "
                             "Best on voices and creature sounds.");
        m_formant.onClick = [this] { if (onFormant) onFormant(! m_formant.getToggleState()); };
        addAndMakeVisible(m_formant);

        m_stamp.setAccent(Palette::teal, Palette::tealText, Palette::tealLine);
        m_stamp.setFontSize(18.0f, Weight::Bold, 0.2f);
        m_stamp.setTooltip("On: the Preview button at the bottom left of the AudioSuite window plays the effect with the shift applied, "
                           "and Render writes it to the clip. Off: audio passes through untouched.");
        m_stamp.onClick = [this] { if (onStamp) onStamp(! m_stamp.getToggleState()); };
        addAndMakeVisible(m_stamp);

        m_trim.setTooltip("Shifting pitch can raise a sound's peaks. On: the stamp is turned down by just enough to keep it "
                          "under full scale; the figure is how much. Off: the level is left alone and a limiter catches any peaks.");
        m_trim.onClick = [this] { if (onTrim) onTrim(! m_trim.getToggleState()); };
        addAndMakeVisible(m_trim);

        m_copy.setAccent(Palette::teal, Palette::tealText, Palette::tealLine);
        m_copy.setFontSize(9.5f);
        m_copy.setTooltip("Copies the shift, the speed and the target note to the clipboard as a line of text. For a curve "
                  "that moves, the values are those of its first note.");
        m_copy.onClick = [this] { if (onCopy) onCopy(); };
        addAndMakeVisible(m_copy);
    }

    // Marks Stamp as the next thing to press.
    void setStampBeacon(bool shouldBeckon) { m_stamp.setBeacon(shouldBeckon); }

    std::function<void(double)> onBend;
    std::function<void(int)> onOctave;
    std::function<void(bool)> onFormant, onStamp, onTrim;
    std::function<void()> onCopy;

    void setData(const HubSnapshot& hub)
    {
        // The readout counts from the value it shows to the new one.
        const bool changed = hub.match.valid != m_hub.match.valid
                             || std::abs(hub.match.semitones - m_hub.match.semitones) > 1.0e-6;
        if (changed)
        {
            m_countFrom = hub.match.valid && m_hub.match.valid ? shownSemitones() : hub.match.semitones;
            m_countStart = nowSeconds();
            m_motion.animateFor(0.5);
            if (hub.match.valid && hub.options.stampEnabled)
                m_stamp.flash();
        }
        if (hub.options.stampEnabled && ! m_hub.options.stampEnabled)
            m_stamp.flash();

        m_hub.options = hub.options;
        m_hub.match.valid = hub.match.valid;
        m_hub.match.semitones = hub.match.semitones;
        m_hub.match.speedRatio = hub.match.speedRatio;
        m_hub.match.targetMidi = hub.match.targetMidi;
        m_hub.match.notesFollowed = hub.match.notesFollowed;
        m_hub.trimKnown = hub.trimKnown;
        m_hub.trimMeasuring = hub.trimMeasuring;
        m_hub.trimDb = hub.trimDb;

        m_updating = true;
        if (! m_bend.isMouseButtonDown())
            m_bend.setValue(hub.options.bend, juce::dontSendNotification);
        m_updating = false;
        m_formant.setToggleState(hub.options.preserveFormants, juce::dontSendNotification);
        m_stamp.setToggleState(hub.options.stampEnabled, juce::dontSendNotification);
        // Until there is a match Stamp has nothing to do: it is drawn back, so the Learn button
        // that is asking to be pressed is the brightest thing in the window.
        m_stamp.setSubText(! hub.match.valid ? "NOTHING TO STAMP YET" : hub.options.stampEnabled ? "ON: PREVIEW, THEN RENDER" : "OFF: AUDIO UNTOUCHED");
        m_stamp.setAlpha(hub.match.valid ? 1.0f : 0.4f);
        m_down.setEnabled(hub.options.octaveShift > -2);
        m_up.setEnabled(hub.options.octaveShift < 2);
        m_copy.setEnabled(hub.match.valid);

        // TRIM reads at a glance: the figure while on, OFF while off, and nothing to show until
        // there is a stamp to measure.
        const bool trimOn = hub.options.trimEnabled;
        m_trim.setToggleState(trimOn, juce::dontSendNotification);
        const bool measuring = hub.match.valid && hub.trimMeasuring;
        juce::String figure;
        if (! hub.match.valid)
            figure = trimOn ? "ON" : "OFF";
        else if (measuring)
            figure = "MEASURING";
        else if (! trimOn)
            figure = "OFF";
        else
            figure = trimText(hub.trimDb);
        m_trim.show(measuring && trimOn, figure, ! trimOn && hub.match.valid && hub.trimKnown && hub.trimDb < -0.05f);
        m_trim.setAlpha(hub.match.valid ? 1.0f : 0.4f);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        drawFrame(g, r, 10.0f, Palette::panel, Palette::rule.brighter(0.3f));

        g.setColour(Palette::rule.brighter(0.2f));
        g.fillRect(kLeftW, 14.0f, 1.0f, r.getHeight() - 28.0f);
        g.fillRect(kStampX - 16.0f, 14.0f, 1.0f, r.getHeight() - 28.0f);

        const auto label = font(Weight::Medium, 9.0f, 0.2f);
        const auto& m = m_hub.match;
        drawText(g, "SHIFT TO MATCH", { 18.0f, 14.0f, kLeftW - 30.0f, 12.0f }, juce::Justification::centredLeft, label, Palette::dim);
        const double shown = shownSemitones();
        drawText(g, m.valid ? shiftText(shown, false) : juce::String("--"), { 16.0f, 30.0f, kLeftW - 28.0f, 46.0f },
                 juce::Justification::centredLeft, font(Weight::Bold, 36.0f, 0.02f), m.valid ? Palette::text : Palette::dim);
        // A thin bar under the readout: how far the shift is, out of a tritone either way.
        {
            const float trackX = 18.0f, trackW = kLeftW - 48.0f, trackY = 78.0f, mid = trackX + 0.5f * trackW;
            g.setColour(Palette::rule.brighter(0.2f));
            g.fillRect(trackX, trackY, trackW, 1.0f);
            g.fillRect(mid - 0.5f, trackY - 3.0f, 1.0f, 7.0f);
            if (m.valid)
            {
                const float reach = static_cast<float>(juce::jlimit(-1.0, 1.0, shown / 6.0)) * 0.5f * trackW;
                g.setColour(Palette::teal);
                g.fillRect(std::min(mid, mid + reach), trackY - 1.0f, std::abs(reach), 3.0f);
            }
        }
        const auto small = font(Weight::Medium, 10.5f, 0.1f);
        drawText(g, m.valid ? "SPEED " + juce::String(std::exp2(shown / 12.0) * 100.0, 1) + "%" : juce::String("SPEED --"),
                 { 18.0f, 86.0f, 140.0f, 14.0f }, juce::Justification::centredLeft, small, Palette::text.withAlpha(0.8f));
        drawText(g, m.valid ? "TO " + upper(core::noteName(m.targetMidi)) : juce::String("TO --"), { 162.0f, 86.0f, 100.0f, 14.0f },
                 juce::Justification::centredLeft, small, Palette::tealText);
        {
            const auto summaryFont = font(Weight::Regular, 9.0f, 0.1f);
            const juce::Rectangle<float> box { 18.0f, 104.0f, kLeftW - 30.0f, 12.0f };
            auto summary = (m.valid && m.notesFollowed >= 2 ? "MOVES THROUGH " + juce::String(m.notesFollowed) + " NOTES, FIRST SHOWN"
                                                            : "OCTAVE " + octaveText(m_hub.options.octaveShift))
                           + " / BEND " + bendText(m_hub.options.bend);
            // The trim, when one is applied and there is room for it.
            if (m.valid && m_hub.options.trimEnabled && m_hub.trimKnown && m_hub.trimDb < -0.05f)
            {
                const auto withTrim = summary + " / TRIM " + trimText(m_hub.trimDb);
                if (juce::GlyphArrangement::getStringWidth(summaryFont, withTrim) <= box.getWidth())
                    summary = withTrim;
            }
            drawText(g, summary, box, juce::Justification::centredLeft, summaryFont, Palette::dim);
        }

        // Bend.
        const float bx = kLeftW + 18.0f, bw = kStampX - 34.0f - bx;
        drawText(g, "BEND", { bx, 14.0f, 60.0f, 12.0f }, juce::Justification::centredLeft, label, Palette::dim);
        drawText(g, bendText(m_bend.getValue()), { bx + 60.0f, 14.0f, bw - 60.0f, 12.0f }, juce::Justification::centredRight,
                 font(Weight::Medium, 9.5f, 0.12f), Palette::amber);
        const auto ticks = font(Weight::Medium, 9.5f, 0.1f);
        drawText(g, "FLAT", { bx, 58.0f, 60.0f, 11.0f }, juce::Justification::centredLeft, ticks, Palette::dim);
        drawText(g, "AS IS", { bx + 0.5f * bw - 30.0f, 58.0f, 60.0f, 11.0f }, juce::Justification::centred, ticks, Palette::dim);
        drawText(g, "MUSIC", { bx + bw - 60.0f, 58.0f, 60.0f, 11.0f }, juce::Justification::centredRight, ticks, Palette::dim);

        drawText(g, "OCTAVE", { bx, 84.0f, 56.0f, 24.0f }, juce::Justification::centredLeft, label, Palette::dim);
        drawText(g, octaveText(m_hub.options.octaveShift), { bx + 56.0f + 24.0f, 84.0f, 28.0f, 24.0f }, juce::Justification::centred,
                 font(Weight::Bold, 12.0f, 0.0f), Palette::amber);
    }

    void resized() override
    {
        const int bx = static_cast<int>(kLeftW) + 18, bw = static_cast<int>(kStampX) - 34 - bx;
        m_bend.setBounds(bx, 30, bw, 26);
        m_down.setBounds(bx + 56, 84, 24, 24);
        m_up.setBounds(bx + 56 + 24 + 28, 84, 24, 24);
        m_formant.setBounds(bx + bw - 86, 84, 86, 24);
        const int sx = static_cast<int>(kStampX);
        const int sw = getWidth() - 16 - sx;
        m_stamp.setBounds(sx, 14, sw, 66);
        // Under Stamp: what is done to its level, and the copy of its values.
        const int trimW = 132;
        m_trim.setBounds(sx, 88, trimW, 26);
        m_copy.setBounds(sx + trimW + 8, 88, sw - trimW - 8, 26);
    }

private:
    // The shift as drawn right now: on its way from the last value to the current one.
    double shownSemitones() const
    {
        const float t = easeOut(progressSince(m_countStart, 0.45));
        return m_countFrom + (m_hub.match.semitones - m_countFrom) * static_cast<double>(t);
    }

    // "-1.8 DB", or "0 DB" when nothing is taken off.
    static juce::String trimText(float db)
    {
        return db < -0.05f ? juce::String(db, 1) + " DB" : juce::String("0 DB");
    }

    static juce::String bendText(double bend)
    {
        const int percent = juce::roundToInt(std::abs(bend) * 100.0);
        if (percent == 0)
            return "AS IS";
        return (bend < 0.0 ? "FLAT " : "MUSIC ") + juce::String(percent) + "%";
    }

    static constexpr float kLeftW = 330.0f;
    static constexpr float kStampX = 684.0f;

    HubSnapshot m_hub;   // only the options and the match's numbers are kept
    BendSlider m_bend;
    ConsoleButton m_down { "-" }, m_up { "+" }, m_formant { "FORMANT" }, m_stamp { "STAMP" }, m_copy { "COPY VALUES" };
    TrimButton m_trim;
    bool m_updating = false;
    Motion m_motion { *this };
    double m_countFrom = 0.0, m_countStart = 0.0;
};

//==============================================================================

class AboutOverlay : public juce::Component
{
public:
    AboutOverlay()
    {
        m_site.setPlain(true, juce::Justification::centredLeft);
        m_site.setAccent(Palette::teal, Palette::tealText, Palette::tealLine);
        m_site.setFontSize(11.0f, Weight::Medium, 0.04f);
        m_site.setTooltip("Opens sweetjusticesound.com in your browser.");
        m_site.onClick = [] { juce::URL("https://sweetjusticesound.com").launchInDefaultBrowser(); };
        addAndMakeVisible(m_site);

        m_close.setAccent(Palette::teal, Palette::tealText, Palette::tealLine);
        m_close.setFontSize(9.5f);
        m_close.setTooltip("Closes this panel.");
        m_close.onClick = [this] { setVisible(false); };
        addAndMakeVisible(m_close);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(Palette::background.withAlpha(0.88f));
        const auto box = boxBounds().toFloat();
        drawFrame(g, box, 12.0f, Palette::panel, Palette::tealLine);
        auto r = box.reduced(24.0f, 20.0f);
        drawText(g, "ABOUT", r.removeFromTop(12.0f), juce::Justification::centredLeft, font(Weight::Medium, 9.0f, 0.2f), Palette::dim);
        r.removeFromTop(8.0f);
        drawText(g, "SammyJs Tonal Recall", r.removeFromTop(26.0f), juce::Justification::centredLeft, font(Weight::Bold, 20.0f, 0.02f), Palette::text);
        drawText(g, "Version " PLUGIN_VERSION, r.removeFromTop(16.0f), juce::Justification::centredLeft, font(Weight::Regular, 11.0f, 0.02f), Palette::tealText);
        r.removeFromTop(10.0f);
        drawText(g, "Copyright " COPYRIGHT_YEAR " Samuel Justice", r.removeFromTop(16.0f), juce::Justification::centredLeft,
                 font(Weight::Regular, 11.0f, 0.02f), Palette::text);
        r.removeFromTop(26.0f);   // the link
        drawText(g, "Pitch tracking with thanks to Ben", r.removeFromTop(16.0f), juce::Justification::centredLeft,
                 font(Weight::Regular, 11.0f, 0.02f), Palette::dim);
    }

    void resized() override
    {
        const auto box = boxBounds();
        m_site.setBounds(box.getX() + 24, box.getY() + 20 + 12 + 8 + 26 + 16 + 10 + 16 + 5, 186, 16);
        m_close.setBounds(box.getRight() - 24 - 70, box.getBottom() - 20 - 24, 70, 24);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (! boxBounds().contains(e.getPosition()))
            setVisible(false);
    }

private:
    juce::Rectangle<int> boxBounds() const { return getLocalBounds().withSizeKeepingCentre(440, 214); }

    ConsoleButton m_site { "sweetjusticesound.com" }, m_close { "CLOSE" };
};

//==============================================================================
// "Tonal Recall x.y.z is available": shown over the editor when the shared update check finds a
// newer release. A dim backdrop takes every click; the card has DOWNLOAD (Return), LATER
// (Escape) and SKIP THIS VERSION. The editor deletes it from onClose, which is called after the
// button's own callback has returned.

class UpdateOverlay : public juce::Component
{
public:
    UpdateOverlay(sammyj::UpdateInfo info, std::function<void()> onClose)
        : m_info(std::move(info)), m_onClose(std::move(onClose))
    {
        setWantsKeyboardFocus(true);
        m_download.setAccent(Palette::teal, Palette::tealText, Palette::tealLine);
        m_download.setToggleState(true, juce::dontSendNotification);   // drawn filled: the main action
        m_download.setTooltip("Opens the download page for the Sammy J's Plugins installer in your browser.");
        m_download.onClick = [this] { m_info.openDownloadPage(); close(); };
        m_later.setAccent(Palette::teal, Palette::tealText, Palette::tealLine);
        m_later.setTooltip("Closes this for now. It is offered again another day.");
        m_later.onClick = [this] { close(); };
        m_skip.setAccent(Palette::teal, Palette::tealText, Palette::tealLine);
        m_skip.setTooltip("Does not offer this version again. A newer one is still offered.");
        m_skip.onClick = [this] { sammyj::UpdateCheck::skipVersion(m_info); close(); };
        for (auto* b : { &m_download, &m_later, &m_skip })
        {
            b->setFontSize(9.5f);
            addAndMakeVisible(*b);
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(Palette::background.withAlpha(0.88f));
        const auto box = boxBounds().toFloat();
        drawFrame(g, box, 12.0f, Palette::panel, Palette::tealLine);
        auto r = box.reduced(24.0f, 20.0f);
        drawText(g, "UPDATE", r.removeFromTop(12.0f), juce::Justification::centredLeft, font(Weight::Medium, 9.0f, 0.2f), Palette::dim);
        r.removeFromTop(8.0f);
        drawText(g, "Tonal Recall " + m_info.latestVersion + " is available", r.removeFromTop(26.0f), juce::Justification::centredLeft,
                 font(Weight::Bold, 18.0f, 0.02f), Palette::text);
        r.removeFromTop(2.0f);
        drawText(g, "You have " + m_info.currentVersion + ".", r.removeFromTop(16.0f), juce::Justification::centredLeft,
                 font(Weight::Regular, 11.0f, 0.02f), Palette::tealText);
    }

    void resized() override
    {
        auto row = boxBounds().reduced(24, 20).removeFromBottom(26);
        m_download.setBounds(row.removeFromLeft(110));
        row.removeFromLeft(10);
        m_later.setBounds(row.removeFromLeft(80));
        row.removeFromLeft(10);
        m_skip.setBounds(row.removeFromLeft(160));
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.isKeyCode(juce::KeyPress::returnKey))
        {
            m_info.openDownloadPage();
            close();
            return true;
        }
        if (key.isKeyCode(juce::KeyPress::escapeKey))
        {
            close();
            return true;
        }
        return false;
    }

    void mouseDown(const juce::MouseEvent&) override {}   // the backdrop takes the click; nothing happens

private:
    juce::Rectangle<int> boxBounds() const { return getLocalBounds().withSizeKeepingCentre(440, 150); }

    void close()
    {
        if (m_closing)
            return;
        m_closing = true;
        setVisible(false);
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<UpdateOverlay>(this)]
        {
            if (safe != nullptr && safe->m_onClose)
                safe->m_onClose();
        });
    }

    const sammyj::UpdateInfo m_info;
    const std::function<void()> m_onClose;
    ConsoleButton m_download { "DOWNLOAD" }, m_later { "LATER" }, m_skip { "SKIP THIS VERSION" };
    bool m_closing = false;
};

//==============================================================================
// The glass in front of the console: faint scan lines and darkened corners, as on a monitor. It
// is one picture, made when the window is sized and drawn over everything; it never animates and
// takes no clicks. Kept light on purpose: the text under it has to stay easy to read.

class MonitorOverlay : public juce::Component
{
public:
    MonitorOverlay()
    {
        setInterceptsMouseClicks(false, false);
        setOpaque(false);
    }

    void resized() override
    {
        const int w = getWidth(), h = getHeight();
        if (w <= 0 || h <= 0)
        {
            m_glass = {};
            return;
        }
        // Built at twice the size so the lines stay fine on a high-resolution display.
        constexpr int scale = 2;
        m_glass = juce::Image(juce::Image::ARGB, w * scale, h * scale, true);
        juce::Graphics g(m_glass);

        // Scan lines: one soft dark line in every three (in points).
        g.setColour(juce::Colours::black.withAlpha(0.10f));
        for (int y = 0; y < h * scale; y += 3 * scale)
            g.fillRect(0, y, w * scale, scale);

        // Darkened corners and edges: the middle is left clear.
        juce::ColourGradient vignette(juce::Colours::transparentBlack, static_cast<float>(w * scale) * 0.5f, static_cast<float>(h * scale) * 0.5f,
                                      juce::Colours::black.withAlpha(0.22f), 0.0f, 0.0f, true);
        vignette.addColour(0.62, juce::Colours::transparentBlack);
        g.setGradientFill(vignette);
        g.fillRect(0, 0, w * scale, h * scale);

        // A trace of light along the top edge of the glass.
        juce::ColourGradient sheen(juce::Colours::white.withAlpha(0.035f), 0.0f, 0.0f, juce::Colours::transparentWhite, 0.0f,
                                   static_cast<float>(h * scale) * 0.22f, false);
        g.setGradientFill(sheen);
        g.fillRect(0, 0, w * scale, static_cast<int>(static_cast<float>(h * scale) * 0.22f));
    }

    void paint(juce::Graphics& g) override
    {
        if (m_glass.isValid())
            g.drawImage(m_glass, getLocalBounds().toFloat());
    }

private:
    juce::Image m_glass;
};

//==============================================================================
// Shown over everything when the plug-in has been loaded as a real-time insert in Pro Tools. It
// is an AudioSuite tool: as an insert it passes audio through and does nothing else, and this
// says where to find it. It cannot be dismissed (there is nothing behind it to use).

class InsertNotice : public juce::Component
{
public:
    void paint(juce::Graphics& g) override
    {
        g.fillAll(Palette::background.withAlpha(0.93f));
        const auto box = getLocalBounds().withSizeKeepingCentre(560, 176).toFloat();
        drawFrame(g, box, 12.0f, Palette::panel, Palette::amberLine);
        auto r = box.reduced(26.0f, 22.0f);
        drawText(g, "AUDIOSUITE ONLY", r.removeFromTop(12.0f), juce::Justification::centredLeft, font(Weight::Medium, 9.0f, 0.2f), Palette::amber);
        r.removeFromTop(10.0f);
        drawText(g, "Tonal Recall works offline, on clips", r.removeFromTop(26.0f), juce::Justification::centredLeft,
                 font(Weight::Bold, 18.0f, 0.02f), Palette::text);
        r.removeFromTop(10.0f);
        const auto body = font(Weight::Regular, 11.5f, 0.02f);
        drawText(g, "Remove this insert and open Tonal Recall from the AudioSuite menu,", r.removeFromTop(17.0f), juce::Justification::centredLeft, body, Palette::tealText);
        drawText(g, "under Pitch Shift.", r.removeFromTop(17.0f), juce::Justification::centredLeft, body, Palette::tealText);
        r.removeFromTop(8.0f);
        drawText(g, "As an insert it passes audio through untouched.", r.removeFromTop(17.0f), juce::Justification::centredLeft, body, Palette::dim);
    }

    void mouseDown(const juce::MouseEvent&) override {}   // takes the click; nothing happens
};

} // namespace tonalrecall::ui

//==============================================================================

using namespace tonalrecall::ui;

namespace
{
    constexpr int kMargin = 16;
    constexpr int kPanelsY = 74, kPanelsH = 414;
    constexpr int kStripY = 496, kStripBand = 40;
    constexpr int kShiftY = 544, kShiftH = 128;
    constexpr int kFooterY = 680;
}

TonalRecallEditor::TonalRecallEditor(TonalRecallProcessor& p)
    : AudioProcessorEditor(&p), m_processor(p), m_look(std::make_unique<ConsoleLook>())
{
    setLookAndFeel(m_look.get());

    m_music = std::make_unique<MusicPanel>();
    m_sfx = std::make_unique<SfxPanel>();
    m_strip = std::make_unique<WarningStrip>();
    m_shift = std::make_unique<ShiftPanel>();
    m_about = std::make_unique<AboutOverlay>();

    m_music->onLearn = [this] { toggleLearn(Slot::Music); };
    m_music->onClear = [this] { clearSlot(Slot::Music); };
    m_music->onTarget = [this](core::Target target, int pc)
    {
        changeOptions([&](MatchOptions& o) { o.target = target; o.pitchClass = pc; });
    };
    m_sfx->onLearn = [this] { toggleLearn(Slot::Sfx); };
    m_sfx->onClear = [this] { clearSlot(Slot::Sfx); };
    m_sfx->onMethod = [this](core::SfxMethod method) { changeOptions([&](MatchOptions& o) { o.method = method; }); };
    m_sfx->onEdits = [this](const std::vector<core::CurvePoint>& points) { changeOptions([&](MatchOptions& o) { o.edits = points; }); };
    m_sfx->onSmooth = [this](double amount) { changeOptions([&](MatchOptions& o) { o.glide = amount; }); };
    m_sfx->onStyle = [this](int style) { changeOptions([&](MatchOptions& o) { o = o.withStyle(static_cast<MatchOptions::Style>(style)); }); };
    m_music->onPlay = [this](Playback what) { togglePlay(what); };
    m_sfx->onPlay = [this](Playback what) { togglePlay(what); };

    m_shift->onBend = [this](double bend) { changeOptions([&](MatchOptions& o) { o.bend = bend; }); };
    m_shift->onOctave = [this](int step) { changeOptions([&](MatchOptions& o) { o.octaveShift = std::clamp(o.octaveShift + step, -2, 2); }); };
    m_shift->onFormant = [this](bool on) { changeOptions([&](MatchOptions& o) { o.preserveFormants = on; }); };
    m_shift->onStamp = [this](bool on) { changeOptions([&](MatchOptions& o) { o.stampEnabled = on; }); };
    m_shift->onTrim = [this](bool on) { changeOptions([&](MatchOptions& o) { o.trimEnabled = on; }); };
    m_shift->onCopy = [this] { copyValues(); };

    addAndMakeVisible(*m_music);
    addAndMakeVisible(*m_sfx);
    addAndMakeVisible(*m_strip);
    addAndMakeVisible(*m_shift);

    m_help = std::make_unique<ConsoleButton>("?");
    m_help->setFontSize(13.0f, Weight::Bold, 0.0f);
    m_help->setTooltip("About Tonal Recall.");
    m_help->onClick = [this] { showAbout(true); };
    addAndMakeVisible(*m_help);

    m_log = std::make_unique<ConsoleButton>("LOG");
    m_log->setPlain(true);
    m_log->setAccent(Palette::teal, Palette::dim, Palette::tealLine);
    m_log->setFontSize(9.5f, Weight::Medium, 0.15f);
    m_log->setTooltip("Shows the diagnostic log file, which records what the host asked of the plug-in.");
    m_log->onClick = []
    {
        const juce::File file(juce::String::fromUTF8(DiagnosticLog::instance().filePath().c_str()));
        if (file.existsAsFile())
            file.revealToUser();
        else if (file.getParentDirectory().createDirectory())
            file.getParentDirectory().revealToUser();
    };
    addAndMakeVisible(*m_log);

    m_link = std::make_unique<ConsoleButton>("SWEETJUSTICESOUND.COM");
    m_link->setPlain(true);
    m_link->setAccent(Palette::teal, Palette::dim, Palette::tealLine);
    m_link->setFontSize(9.5f, Weight::Medium, 0.15f);
    m_link->setTooltip("Opens sweetjusticesound.com in your browser.");
    m_link->onClick = [] { juce::URL("https://sweetjusticesound.com").launchInDefaultBrowser(); };
    addAndMakeVisible(*m_link);

    addChildComponent(*m_about);

    m_insertNotice = std::make_unique<InsertNotice>();
    addChildComponent(*m_insertNotice);

    // The glass sits in front of everything, popups included.
    m_monitor = std::make_unique<MonitorOverlay>();
    addAndMakeVisible(*m_monitor);
    m_monitor->setAlwaysOnTop(true);
    m_tooltips = std::make_unique<juce::TooltipWindow>(this, 700);

    setSize(kWidth, kHeight);
    m_processor.logEditor(true);
    refresh(true);
    m_openedAt = nowSeconds();
    startTimerHz(60);   // for the opening second; then 20 Hz (see timerCallback)
}

TonalRecallEditor::~TonalRecallEditor()
{
    // The check first: its callback uses the editor, and must not run once this has begun.
    m_updateCheck.reset();
    m_update.reset();
    stopTimer();
    m_tooltips.reset();
    setLookAndFeel(nullptr);
    m_processor.logEditor(false);
}

void TonalRecallEditor::parentHierarchyChanged()
{
    juce::AudioProcessorEditor::parentHierarchyChanged();
    startUpdateCheckWhenShown();
}

void TonalRecallEditor::visibilityChanged()
{
    juce::AudioProcessorEditor::visibilityChanged();
    startUpdateCheckWhenShown();
}

void TonalRecallEditor::startUpdateCheckWhenShown()
{
    // Once per editor, and only when it is on screen in a host: an editor that is only built (a
    // test, a snapshot) never goes online. Never from the processor or the audio thread.
    if (m_updateCheck != nullptr || ! isShowing())
        return;
    m_updateCheck = std::make_unique<sammyj::UpdateCheck>("tonalrecall", "tonalrecall-v", JucePlugin_VersionString,
        [this](const sammyj::UpdateInfo& info) { showUpdate(info); });
}

void TonalRecallEditor::showUpdate(const sammyj::UpdateInfo& info)
{
    if (m_update != nullptr)
        return;
    m_update = std::make_unique<UpdateOverlay>(info, [this] { m_update.reset(); });
    addAndMakeVisible(*m_update);
    m_update->setBounds(getLocalBounds());
    m_update->toFront(false);
    m_monitor->toFront(false);
    // Return and Escape answer it. The keys go back to the host as soon as it closes.
    if (m_update->isShowing())
        m_update->grabKeyboardFocus();
}

void TonalRecallEditor::showInsertNotice(bool shouldShow)
{
    m_insertNotice->setVisible(shouldShow);
    if (shouldShow)
    {
        m_insertNotice->toFront(false);
        m_monitor->toFront(false);
    }
}

int TonalRecallEditor::textOverflowCount() noexcept
{
    return g_overflows.load(std::memory_order_relaxed);
}

void TonalRecallEditor::setMotionEnabled(bool shouldMove) noexcept
{
    g_motionOn.store(shouldMove, std::memory_order_relaxed);
}

void TonalRecallEditor::showAbout(bool shouldShow)
{
    m_about->setVisible(shouldShow);
    if (shouldShow)
        m_about->toFront(false);
}

void TonalRecallEditor::toggleLearn(Slot slot)
{
    auto& store = CaptureStore::instance();
    auto& log = DiagnosticLog::instance();
    const auto index = static_cast<std::size_t>(slot);
    if (store.isArmed(slot))
    {
        SlotSnapshot snap;
        store.snapshot(slot, snap);
        store.disarm(slot);
        m_nothingCaptured[index] = snap.samplesSinceArm == 0;
        log.post(m_processor.getInstanceId(), ThreadKind::Message, "%s: Learn disarmed by user (%s)", slotName(slot),
                 m_nothingCaptured[index] ? "nothing captured while armed" : "audio was captured");
    }
    else
    {
        // One Learn at a time: with both armed, one Preview would be learned as both.
        const auto other = slot == Slot::Music ? Slot::Sfx : Slot::Music;
        if (store.isArmed(other))
            store.disarm(other);
        store.arm(slot);
        m_nothingCaptured[index] = false;
        log.post(m_processor.getInstanceId(), ThreadKind::Message, "%s: Learn armed by user", slotName(slot));
    }
    refresh(true);
}

void TonalRecallEditor::clearSlot(Slot slot)
{
    CaptureStore::instance().clear(slot);
    m_nothingCaptured[static_cast<std::size_t>(slot)] = false;
    DiagnosticLog::instance().post(m_processor.getInstanceId(), ThreadKind::Message, "%s: cleared by user", slotName(slot));
    refresh(true);
}

void TonalRecallEditor::togglePlay(Playback what)
{
    auto& bus = AuditionBus::instance();
    // Playing, or waiting for Preview: a second click stops it.
    const bool stopping = bus.playing() == what;
    if (stopping)
        bus.stop();
    else
        bus.play(what);
    const char* names[] { "", "music", "effect as learned", "effect as stamped" };
    DiagnosticLog::instance().post(m_processor.getInstanceId(), ThreadKind::Message, "play %s: %s (Preview %s)",
                                   names[static_cast<int>(what)], stopping ? "stopped by user" : "started by user",
                                   bus.previewRunning() ? "running" : "not running: waiting for it");
    updatePlayback();
    refresh(true);
}

int TonalRecallEditor::playbackSignature() const
{
    auto& bus = AuditionBus::instance();
    return static_cast<int>(bus.playing()) | (bus.previewRunning() ? 8 : 0) | (bus.startedByPlay() ? 16 : 0);
}

void TonalRecallEditor::updatePlayback()
{
    auto& bus = AuditionBus::instance();
    // Learn hears the selection untouched: nothing plays while it is armed or listening.
    const bool learning = m_slots[0].armed || m_slots[0].capturing || m_slots[1].armed || m_slots[1].capturing;
    if (learning && bus.playing() != Playback::None)
        bus.stopNow();

    const auto role = bus.playing();
    const bool running = bus.previewRunning();
    const bool byPlay = bus.startedByPlay();
    const double rate = m_processor.getSampleRate();
    const auto stateFor = [&](Playback what, bool available)
    {
        if (! available)
            return SlotPanel::PlayState::Unavailable;
        if (role == what && running)
            return SlotPanel::PlayState::Playing;
        if (role == what && byPlay)
            return SlotPanel::PlayState::Waiting;
        return SlotPanel::PlayState::Idle;
    };
    // Why a button cannot be pressed, in the order it would have to be put right; empty if it can.
    const auto blocked = [&](bool learned, double learnedAt, const juce::String& what) -> juce::String
    {
        if (learning)
            return "Learn is listening: nothing is played until it has finished.";
        if (! learned)
            return "Learn the " + what + " first.";
        if (rate > 0.0 && learnedAt > 0.0 && std::abs(learnedAt - rate) > 0.5)
            return "The " + what + " was learned at " + juce::String(juce::roundToInt(learnedAt)) + " Hz: the session has to be at that rate "
                   "for it to play at the right speed.";
        return {};
    };

    const bool musicLearned = m_hub.hasMusic && m_hub.musicChannels != nullptr && ! m_hub.musicChannels->empty();
    const bool sfxLearned = m_hub.hasSfx && m_hub.sfxChannels != nullptr && ! m_hub.sfxChannels->empty();
    const auto musicWhy = blocked(musicLearned, m_hub.musicRate, "music");
    const auto inputWhy = blocked(sfxLearned, m_hub.sfxRate, "sound effect");
    auto outputWhy = inputWhy;
    if (outputWhy.isEmpty() && ! (m_hub.match.valid && m_hub.planReady))
        outputWhy = "Needs a match: the music and a sound effect with a clear pitch, both learned.";

    m_music->showPlayback(stateFor(Playback::Music, musicWhy.isEmpty()),
                          musicWhy.isNotEmpty() ? musicWhy
                                                : juce::String("Plays the learned music from its start, as it was learned, through this window's "
                                                               "Preview: Preview has to be running for it to be heard. The line across the notes "
                                                               "shows where it is. Click again to stop."));
    m_sfx->showPlayback(stateFor(Playback::Input, inputWhy.isEmpty()),
                        inputWhy.isNotEmpty() ? inputWhy
                                              : juce::String("Plays the sound effect from its start, as it was learned, through Preview. The pale "
                                                             "line on the scan shows where it is. Click again to stop."),
                        stateFor(Playback::Output, outputWhy.isEmpty()),
                        outputWhy.isNotEmpty() ? outputWhy
                                               : juce::String("Plays the sound effect from its start as it will be stamped: the shift, the curve, "
                                                              "formants and trim, through Preview. Changes to the curve are heard as it plays, and "
                                                              "a click on the scan plays on from there. Click again to stop."));
    m_music->pollPlayhead();
    m_sfx->pollPlayhead();
}

void TonalRecallEditor::changeOptions(const std::function<void(MatchOptions&)>& change)
{
    auto& hub = MatchHub::instance();
    auto o = hub.options();
    change(o);
    hub.setOptions(o);
    refresh(true);
}

void TonalRecallEditor::copyValues()
{
    const auto& m = m_hub.match;
    if (! m.valid)
        return;
    juce::String line;
    line << "Tonal Recall: " << signedNumber(m.semitones, 2) << " st (" << shiftText(m.semitones, true) << "), speed "
         << juce::String(m.speedRatio * 100.0, 1) << "%, to " << core::noteName(m.targetMidi);
    if (m_hub.music.valid)
        line << " (" << m_hub.music.keyName() << ")";
    if (m_hub.options.trimEnabled && m_hub.trimKnown && m_hub.trimDb < -0.05f)
        line << ", trim " << juce::String(m_hub.trimDb, 1) << " dB";
    juce::SystemClipboard::copyTextToClipboard(line);
}

void TonalRecallEditor::timerCallback()
{
    if (m_openedAt > 0.0)
    {
        // The title rule is drawing in.
        repaint(0, 60, 120, 12);
        if (nowSeconds() - m_openedAt > 1.0)
        {
            m_openedAt = 0.0;
            startTimerHz(20);
        }
    }
    // Loaded as an insert in Pro Tools: say so, over everything.
    if (m_processor.isAaxInsert() != m_insertNotice->isVisible())
        showInsertNotice(m_processor.isAaxInsert());

    // The "press Preview to hear edits" line comes and goes with time, not with data, and so do
    // playing and waiting for Preview.
    const bool needsPreview = m_sfx->auditionNeedsPreview();
    const int playback = playbackSignature();
    const bool changed = needsPreview != m_auditionHintShown || playback != m_playbackShown;
    m_auditionHintShown = needsPreview;
    refresh(changed);
    updatePlayback();
}

void TonalRecallEditor::refresh(bool force)
{
    auto& store = CaptureStore::instance();
    auto& hub = MatchHub::instance();

    bool storeChanged = force;
    for (int s = 0; s < kNumSlots; ++s)
    {
        const auto slot = static_cast<Slot>(s);
        const auto index = static_cast<std::size_t>(s);
        const auto generation = store.generation(slot);
        if (! force && generation == m_storeGenerations[index])
            continue;
        store.snapshot(slot, m_slots[index]);
        m_storeGenerations[index] = m_slots[index].generation;
        storeChanged = true;
    }

    const bool hubChanged = force || hub.generation() != m_hubGeneration;
    if (hubChanged)
    {
        hub.snapshot(m_hub);
        m_hubGeneration = m_hub.generation;
    }

    const auto run = m_processor.lastStampRun();
    const bool runChanged = force || run.runIndex != m_runIndex || run.ended != m_runEnded || run.aligned != m_run.aligned;
    m_run = run;
    m_runIndex = run.runIndex;
    m_runEnded = run.ended;

    if (storeChanged || hubChanged)
    {
        m_music->setData(m_hub, m_slots[0]);
        m_sfx->setData(m_hub, m_slots[1], hubChanged);
    }
    if (hubChanged)
        m_shift->setData(m_hub);

    if (force)
        updatePlayback();

    if (storeChanged || hubChanged || runChanged)
    {
        m_playbackShown = playbackSignature();
        // Walk the user through it: the next button to press beckons, and the strip says why.
        const bool busy = m_slots[0].armed || m_slots[0].capturing || m_slots[1].armed || m_slots[1].capturing
                          || m_hub.musicBusy || m_hub.sfxBusy;
        const bool haveMusic = m_hub.hasMusic && m_hub.music.valid;
        const bool haveSfx = m_hub.hasSfx && m_hub.sfx.valid;
        const int step = busy ? 0 : ! haveMusic ? 1 : ! haveSfx ? 2 : (m_hub.match.valid && ! m_hub.options.stampEnabled) ? 3 : 4;
        m_music->setBeacon(step == 1);
        m_sfx->setBeacon(step == 2);
        m_shift->setStampBeacon(step == 3);
        // While something plays, the strip says what (unless something needs saying more).
        const auto playing = AuditionBus::instance().previewRunning() ? AuditionBus::instance().playing() : Playback::None;
        m_strip->setHint(playing == Playback::Music    ? juce::String("PLAYING THE MUSIC AS LEARNED")
                         : playing == Playback::Input  ? juce::String("PLAYING THE SOUND EFFECT AS LEARNED")
                         : playing == Playback::Output ? juce::String("PLAYING THE SOUND EFFECT AS IT WILL BE STAMPED")
                         : step == 1 ? "START HERE: ARM LEARN ON 01 MUSIC, SELECT THE MUSIC, PRESS PREVIEW"
                         : step == 2 ? "NEXT: ARM LEARN ON 02 SOUND EFFECT, SELECT THE EFFECT, PRESS PREVIEW"
                         : step == 3 ? "SWITCH STAMP ON TO AUDITION AND RENDER THE SHIFT"
                         : step == 4 && m_hub.match.valid && m_hub.options.style() == static_cast<int>(MatchOptions::Style::Locked)
                               && musicRampCents(m_hub.music) >= kRampCents
                               ? juce::String("THE MUSIC'S PITCH MOVES: CLICK SUBTLE OR MOVING ON THE EFFECT'S SCAN TO HAVE THE EFFECT MOVE WITH IT")
                         : step == 4 && m_hub.match.valid ? "READY: PRESS PREVIEW (THE SPEAKER, BOTTOM LEFT) TO HEAR THE STAMP, THEN RENDER TO WRITE IT"
                                                          : "");

        auto warnings = buildWarnings();
        if (warnings != m_warnings || force)
        {
            m_warnings = std::move(warnings);
            m_strip->setLines(m_warnings);
            resized();
        }
    }
}

juce::StringArray TonalRecallEditor::buildWarnings() const
{
    juce::StringArray lines;

    // In order of consequence: first whatever means a Render did not, or will not, do what the
    // window says; then what is being learned; then doubts about what was learned.
    const double rate = m_processor.getSampleRate();
    if (m_hub.hasSfx && m_hub.sfxRate > 0.0 && rate > 0.0 && std::abs(m_hub.sfxRate - rate) > 0.5)
        lines.add("THE EFFECT WAS LEARNED AT " + juce::String(juce::roundToInt(m_hub.sfxRate)) + " HZ: STAMP DOES NOTHING UNTIL THE SESSION IS AT THAT RATE");
    if (m_run.ended && m_run.mode == StampRunInfo::Mode::Stamp && m_run.stampOn && m_run.bendActive && ! m_run.aligned)
        lines.add("THE CURVE WAS NOT APPLIED: THE LEARNED EFFECT WAS NOT FOUND IN WHAT WAS PLAYED. ONLY THE FIXED SHIFT WAS APPLIED.");
    if (const float held = m_processor.lastRenderLimiterDb(); held > 0.5f)
        lines.add("THE LAST RENDER WENT OVER FULL SCALE: THE LIMITER HELD ITS PEAKS DOWN BY " + juce::String(held, 1) + " DB");
    if (m_hub.match.valid && ! m_hub.options.trimEnabled && m_hub.trimKnown && m_hub.trimDb < -0.05f)
        lines.add("TRIM IS OFF AND THE STAMP GOES " + juce::String(-m_hub.trimDb, 1) + " DB OVER FULL SCALE: THE LIMITER WILL HOLD ITS PEAKS");

    if (m_hub.hasMusic && m_hub.musicRate > 0.0 && rate > 0.0 && std::abs(m_hub.musicRate - rate) > 0.5)
        lines.add("THE MUSIC WAS LEARNED AT " + juce::String(juce::roundToInt(m_hub.musicRate)) + " HZ: IT PLAYS BACK ONLY WITH THE SESSION AT THAT RATE");

    if (m_processor.learnSkippedInRender() && (m_slots[0].armed || m_slots[1].armed))
        lines.add("LEARN LISTENS TO PREVIEW, NOT RENDER: PRESS PREVIEW");
    {
        // A play button was pressed with Preview stopped: it is waiting for it.
        auto& bus = AuditionBus::instance();
        if (bus.playing() != Playback::None && bus.startedByPlay() && ! bus.previewRunning())
            lines.add(m_processor.isOtherHost() ? "PLAY THE TRACK IN THE HOST: PLAYBACK USES THIS PLUG-IN'S OUTPUT"
                                                : "PRESS PREVIEW (THE SPEAKER, BOTTOM LEFT): PLAYBACK USES ITS OUTPUT");
    }
    if (m_sfx->auditionNeedsPreview())
        lines.add("TO HEAR EDITS AS YOU MAKE THEM, PRESS PREVIEW IN THE AUDIOSUITE WINDOW");

    for (int s = 0; s < kNumSlots; ++s)
    {
        const auto& snap = m_slots[static_cast<std::size_t>(s)];
        const juce::String what = s == 0 ? "MUSIC" : "EFFECT";
        if (snap.capturing)
            lines.add("LEARNING THE " + what + ": LET PREVIEW PLAY THE WHOLE SELECTION");
        else if (snap.armed)
            lines.add("LEARN ARMED FOR THE " + what + ": SELECT IT AND PRESS PREVIEW IN THE AUDIOSUITE WINDOW");
        if (m_nothingCaptured[static_cast<std::size_t>(s)] && ! snap.armed && snap.samplesCaptured == 0)
            lines.add("NOTHING WAS LEARNED FOR THE " + what + ": WAS PREVIEW PRESSED?");
        if (snap.capturing)
            continue;
        if (snap.truncated)
            lines.add("CAPTURE STOPPED AT " + juce::String(juce::roundToInt(CaptureStore::kMaxSeconds)) + " S");
        if (snap.samplesCaptured > 0 && snap.peak < 1.0e-5f)
            lines.add("CAPTURED AUDIO IS SILENT");
        else if (snap.samplesCaptured > 0 && snap.lastEnd != PassEnd::SelectionEnd && snap.fractionHeard && *snap.fractionHeard < 0.995)
            lines.add("ONLY " + juce::String(juce::roundToInt(*snap.fractionHeard * 100.0)) + "% OF THE SELECTION WAS HEARD: LEARN AGAIN AND LET PREVIEW PLAY IT THROUGH");
    }

    if (m_hub.hasMusic && ! m_hub.musicBusy && ! m_hub.music.valid && m_slots[0].peak >= 1.0e-5f)
        lines.add("NO NOTES FOUND IN THE MUSIC: LEARN A LONGER OR LOUDER PASSAGE");
    if (m_hub.hasSfx && ! m_hub.sfxBusy && m_hub.sfx.valid && ! m_hub.sfx.pitched)
        lines.add(m_hub.options.method == core::SfxMethod::Auto
                      ? "NO PITCH FOUND IN THE EFFECT (EVERY TRACKING MODE WAS TRIED): THERE IS NOTHING TO MATCH"
                      : "NO PITCH FOUND IN THE EFFECT WITH THIS TRACKING MODE: TRY AUTO");
    else if (m_hub.hasSfx && ! m_hub.sfxBusy && m_hub.sfx.valid && m_hub.sfx.usedMethod != core::SfxMethod::Resonance && m_hub.sfx.tonalness < 0.3f)
        lines.add("THE EFFECT'S PITCH IS UNCLEAR: CHECK THE RESULT BY EAR");
    if (m_hub.hasMusic && m_hub.music.valid && m_hub.music.keyConfidence < 0.5f)
        lines.add("KEY IS UNCERTAIN: CHECK THE TARGET NOTE");
    if (m_hub.hasSfx && ! m_slots[1].capturing && m_slots[1].peak >= 0.999f)
        lines.add("THE EFFECT REACHES FULL SCALE AS LEARNED: IT MAY HAVE BEEN CLIPPED BEFORE IT GOT HERE");

    lines.removeDuplicates(false);
    return lines;
}

void TonalRecallEditor::paint(juce::Graphics& g)
{
    g.fillAll(Palette::background);

    // The console's backplate: a field of fine dots, and a tick at each corner of the window.
    g.setColour(Palette::rule.withAlpha(0.7f));
    for (int y = 12; y < kHeight; y += 20)
        for (int x = 12; x < kWidth; x += 20)
            g.fillRect(x, y, 1, 1);
    g.setColour(Palette::tealLine);
    for (const auto corner : { juce::Point<int>(6, 6), juce::Point<int>(kWidth - 6, 6), juce::Point<int>(6, kHeight - 6), juce::Point<int>(kWidth - 6, kHeight - 6) })
    {
        const int dx = corner.x < kWidth / 2 ? 1 : -1, dy = corner.y < kHeight / 2 ? 1 : -1;
        g.fillRect(std::min(corner.x, corner.x + dx * 12), corner.y - (dy < 0 ? 1 : 0), 12, 1);
        g.fillRect(corner.x - (dx < 0 ? 1 : 0), std::min(corner.y, corner.y + dy * 12), 1, 12);
    }

    ui::drawText(g, "SAMMYJS  /  PITCH MATCHER  /", { static_cast<float>(kMargin), 14.0f, 400.0f, 12.0f }, juce::Justification::centredLeft,
                 font(Weight::Medium, 10.0f, 0.16f), Palette::dim);
    // Outside Pro Tools it works, but it is the same offline tool: say what the words mean here.
    if (m_processor.isOtherHost())
        ui::drawText(g, "OFFLINE TOOL  /  IN THIS HOST, PREVIEW = PLAY THE CLIP, RENDER = BOUNCE OR FREEZE THE TRACK",
                     { 330.0f, 14.0f, static_cast<float>(kWidth) - 330.0f - 60.0f, 12.0f }, juce::Justification::centredRight,
                     font(Weight::Medium, 9.5f, 0.08f), Palette::amber);
    ui::drawText(g, "TONAL RECALL", { static_cast<float>(kMargin) - 1.0f, 30.0f, 500.0f, 34.0f }, juce::Justification::centredLeft,
                 font(Weight::Bold, 30.0f, 0.14f), Palette::text);

    // A short teal / amber rule under the title: the two sides of the match.
    // They draw themselves in when the window opens, teal first.
    g.setColour(Palette::teal);
    g.fillRect(static_cast<float>(kMargin), 66.0f, 28.0f * easeOut(progressSince(m_openedAt, 0.45)), 2.0f);
    g.setColour(Palette::amber);
    g.fillRect(static_cast<float>(kMargin) + 32.0f, 66.0f, 28.0f * easeOut(progressSince(m_openedAt + 0.2, 0.45)), 2.0f);

    g.setColour(Palette::rule);
    g.fillRect(static_cast<float>(kMargin), static_cast<float>(kFooterY), static_cast<float>(kWidth - 2 * kMargin), 1.0f);
    ui::drawText(g, "V" PLUGIN_VERSION, { static_cast<float>(kMargin), static_cast<float>(kFooterY) + 6.0f, 60.0f, 14.0f },
                 juce::Justification::centredLeft, font(Weight::Medium, 9.5f, 0.15f), Palette::dim);
}

void TonalRecallEditor::resized()
{
    const int panelW = (kWidth - 2 * kMargin - 12) / 2;
    m_music->setBounds(kMargin, kPanelsY, panelW, kPanelsH);
    m_sfx->setBounds(kMargin + panelW + 12, kPanelsY, panelW, kPanelsH);
    // The strip is as tall as its text, centred in the band between the panels.
    const int stripH = m_strip->wantedHeight();
    m_strip->setBounds(kMargin, kStripY + (kStripBand - stripH) / 2, kWidth - 2 * kMargin, stripH);
    m_shift->setBounds(kMargin, kShiftY, kWidth - 2 * kMargin, kShiftH);
    m_help->setBounds(kWidth - kMargin - 30, 24, 30, 30);
    m_log->setBounds(kMargin + 64, kFooterY + 4, 40, 18);
    m_link->setBounds(kWidth - kMargin - 200, kFooterY + 4, 200, 18);
    m_about->setBounds(getLocalBounds());
    if (m_update != nullptr)
        m_update->setBounds(getLocalBounds());
    m_insertNotice->setBounds(getLocalBounds());
    m_monitor->setBounds(getLocalBounds());
}
