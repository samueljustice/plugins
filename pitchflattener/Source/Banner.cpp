#include "Banner.h"

#include <cmath>

namespace
{
    using Kind = BannerLogic::Kind;
    using State = pitchflattener::Engine::State;

    // Blinks at 2 Hz: lit for the first 60% of each half second, so a blink reads as a flash, not a flicker.
    bool blinkLit (double t) { return t - 0.5 * std::floor (t / 0.5) < 0.3; }
}

//==============================================================================
void BannerLogic::update (const Input& in, double now)
{
    if (! m_started)
    {
        // Opening the editor is not a change: a sound learned before stays unannounced, and a
        // playback already running does not flash.
        m_started = true;
        m_announcedId = in.learnedId;
        if (in.playing)
            m_lastPlaying = now;
        m_last = in;
    }

    const bool capturing = in.state == State::Armed || in.state == State::Listening || in.state == State::Analysing;
    if (capturing)
        m_oneShot = Kind::None;   // the user started something new: what came before is over

    // A new sound learned: announced once, whatever renders of it follow.
    if (in.learnedId != 0 && in.learnedId != m_announcedId && ! capturing)
    {
        if (in.state == State::Ready)
        {
            m_oneShot = Kind::Learned;
            m_oneShotStart = now;
            m_announcedId = in.learnedId;
        }
        else if (in.state == State::NothingToFlatten)
        {
            m_oneShot = Kind::NoSteadyPitch;
            m_oneShotStart = now;
            m_announcedId = in.learnedId;
        }
    }

    // The flattened sound starts playing (and had not been playing a moment ago).
    if (in.playing && ! m_last.playing && in.state == State::Ready && now - m_lastPlaying > kPlayGapSeconds)
    {
        m_oneShot = Kind::Flattened;
        m_oneShotStart = now;
    }
    if (in.playing)
        m_lastPlaying = now;

    // Forgotten.
    if (in.learnedId == 0 && m_last.learnedId != 0 && in.state == State::Idle)
    {
        m_oneShot = Kind::Cleared;
        m_oneShotStart = now;
        m_announcedId = 0;
    }

    // Which one-shot is still showing.
    Kind oneShot = Kind::None;
    double oneShotSince = m_oneShotStart;
    const double age = now - m_oneShotStart;
    switch (m_oneShot)
    {
        case Kind::Learned:
            if (age < kLearnedSeconds)
                oneShot = Kind::Learned;
            else if (age < kLearnedSeconds + kPlayItAgainSeconds)
            {
                oneShot = Kind::PlayItAgain;
                oneShotSince = m_oneShotStart + kLearnedSeconds;
            }
            break;
        case Kind::Flattened:     if (age < kFlattenedSeconds) oneShot = Kind::Flattened; break;
        case Kind::NoSteadyPitch: if (age < kNoSteadySeconds)  oneShot = Kind::NoSteadyPitch; break;
        case Kind::Cleared:       if (age < kClearedSeconds)   oneShot = Kind::Cleared; break;
        case Kind::None:
        case Kind::LearnFirst:
        case Kind::PlayTheSound:
        case Kind::Listening:
        case Kind::Analysing:
        case Kind::PlayItAgain:
        case Kind::WrongSampleRate: break;   // not one-shots
    }
    if (oneShot == Kind::None)
        m_oneShot = Kind::None;

    Kind persistent = Kind::None;
    switch (in.state)
    {
        case State::Idle:      persistent = Kind::LearnFirst; break;
        case State::Armed:     persistent = Kind::PlayTheSound; break;
        case State::Listening: persistent = Kind::Listening; break;
        case State::Analysing: persistent = Kind::Analysing; break;
        case State::Ready:     persistent = in.rateMismatch ? Kind::WrongSampleRate : Kind::None; break;
        case State::NothingToFlatten: break;
    }

    // What the user is doing now outranks what just happened; a sound that cannot play outranks its
    // announcement; otherwise the one-shot shows, and the persistent banner after it.
    Kind next = persistent;
    double nextSince = now;
    if (! capturing && persistent != Kind::WrongSampleRate && oneShot != Kind::None)
    {
        next = oneShot;
        nextSince = oneShotSince;
    }

    if (next != m_kind)
    {
        m_kind = next;
        m_since = next == oneShot && next != Kind::None ? nextSince : now;
        if (next != Kind::None)
            ++m_raised;
    }

    m_flatTag = in.playing && in.state == State::Ready && m_kind != Kind::Flattened;
    m_last = in;
}

juce::String BannerLogic::textFor (Kind k)
{
    switch (k)
    {
        case Kind::LearnFirst:      return "LEARN FIRST";
        case Kind::PlayTheSound:    return "PLAY THE SOUND";
        case Kind::Listening:       return "LISTENING";
        case Kind::Analysing:       return "ANALYSING";
        case Kind::Learned:         return "LEARNED";
        case Kind::PlayItAgain:     return "PLAY IT AGAIN";
        case Kind::Flattened:       return "FLATTENED";
        case Kind::NoSteadyPitch:   return "NO STEADY PITCH";
        case Kind::WrongSampleRate: return "WRONG SAMPLE RATE";
        case Kind::Cleared:         return "CLEARED";
        case Kind::None:            break;
    }
    return {};
}

juce::Colour BannerLogic::colourFor (Kind k)
{
    using namespace Theme::Colours;
    switch (k)
    {
        case Kind::LearnFirst:      return orange;    // the control to press
        case Kind::Analysing:       return cyan;      // reading what was heard
        case Kind::Listening:
        case Kind::NoSteadyPitch:
        case Kind::WrongSampleRate: return magenta;   // recording, or a problem
        case Kind::PlayTheSound:
        case Kind::Learned:
        case Kind::PlayItAgain:
        case Kind::Flattened:       return lime;      // go, success, the flattened sound
        case Kind::Cleared:
        case Kind::None:            break;
    }
    return text;
}

//==============================================================================
Banner::Banner (const Hud::Clock& clock)
    : m_clock (clock)
{
    setInterceptsMouseClicks (false, false);
    setOpaque (false);
}

void Banner::show (BannerLogic::Kind kind, double since, double captureSeconds)
{
    const bool changed = kind != m_kind || std::abs (since - m_since) > 1.0e-9;
    if (kind != m_kind)
    {
        m_outgoing = m_kind;
        m_outSince = m_clock.now();
    }
    const bool repaintNeeded = changed || (kind == BannerLogic::Kind::Listening && std::abs (captureSeconds - m_captureSeconds) >= 0.05);
    m_kind = kind;
    m_since = since;
    m_captureSeconds = captureSeconds;
    if (repaintNeeded)
        repaint();
}

bool Banner::isAnimating() const noexcept
{
    return m_kind != BannerLogic::Kind::None || m_clock.now() - m_outSince < kWipeSeconds + 0.05;
}

void Banner::tick()
{
    if (! isAnimating())
        return;
    // Only the band (with its shadow and rails) and, under LEARN FIRST, the chevrons above it move.
    const auto b = getLocalBounds();
    const int bandHeight = juce::roundToInt (juce::jlimit (56.0f, 92.0f, (float) b.getHeight() * 0.34f));
    const int top = juce::roundToInt ((float) b.getCentreY() - (float) bandHeight * 0.5f - 6.0f);
    auto area = juce::Rectangle<int> (b.getX(), top - 10, b.getWidth(), bandHeight + 24);
    if (m_kind == BannerLogic::Kind::LearnFirst || m_outgoing == BannerLogic::Kind::LearnFirst)
        area = area.getUnion ({ juce::roundToInt (m_learnX) - 12, 0, 24, top });
    repaint (area);
}

void Banner::drawBand (juce::Graphics& g, BannerLogic::Kind kind, double age, float wipeIn, float wipeOut) const
{
    using namespace Theme::Colours;
    const auto bounds = getLocalBounds().toFloat();
    const float bandHeight = std::round (juce::jlimit (56.0f, 92.0f, bounds.getHeight() * 0.34f));
    const float lean = bandHeight * Hud::kShear * 1.4f;
    const auto band = juce::Rectangle<float> (bounds.getX() + 28.0f, std::round (bounds.getCentreY() - bandHeight * 0.5f - 6.0f),
                                              bounds.getWidth() - 56.0f, bandHeight);
    const auto colour = BannerLogic::colourFor (kind);

    // Wipes: in from the left edge, out towards the right, along the band's own slant.
    juce::Graphics::ScopedSaveState save (g);
    {
        const float span = band.getWidth() + 2.0f * lean;
        const float from = band.getX() - lean + span * wipeOut;
        const float to = band.getX() - lean + span * wipeIn;
        juce::Path clip;
        clip.startNewSubPath (from + lean, band.getY() - 12.0f);
        clip.lineTo (to + lean, band.getY() - 12.0f);
        clip.lineTo (to - lean, band.getBottom() + 12.0f);
        clip.lineTo (from - lean, band.getBottom() + 12.0f);
        clip.closeSubPath();
        g.reduceClipRegion (clip);
    }

    juce::Path shape;
    shape.startNewSubPath (band.getX() + lean, band.getY());
    shape.lineTo (band.getRight(), band.getY());
    shape.lineTo (band.getRight() - lean, band.getBottom());
    shape.lineTo (band.getX(), band.getBottom());
    shape.closeSubPath();

    // What it covers is chunky again, whatever was marked fine or raw under it.
    Hud::markCovered (g, shape);
    {
        auto shadow = shape;
        shadow.applyTransform (juce::AffineTransform::translation (6.0f, 6.0f));
        Hud::markCovered (g, shadow);
    }

    // A dark shadow band under it, offset, so the coloured band lifts off a busy picture behind it.
    g.setColour (space.withAlpha (0.82f));
    g.fillPath (shape, juce::AffineTransform::translation (6.0f, 6.0f));
    g.setColour (colour);
    g.fillPath (shape);

    // Thin rails above and below, as on a broadcast strap.
    g.fillRect (juce::Rectangle<float> (band.getX() + lean + 10.0f, band.getY() - 6.0f, band.getWidth() * 0.42f, 2.0f));
    g.fillRect (juce::Rectangle<float> (band.getRight() - lean - band.getWidth() * 0.42f - 10.0f, band.getBottom() + 4.0f, band.getWidth() * 0.42f, 2.0f));

    // Speed stripes trailing at both ends, cut out of the band in the window colour; the words sit
    // between them.
    const auto stripeRow = juce::Rectangle<float> (0.0f, band.getY() + bandHeight * 0.2f, 0.0f, bandHeight * 0.6f);
    const float leftStripes = Hud::speedStripes (g, stripeRow.withX (band.getX() + lean + 6.0f).withWidth (100.0f), space.withAlpha (0.75f), 3);
    const float rightWidth = 2.0f * std::max (2.0f, stripeRow.getHeight() * 0.28f) + std::max (2.0f, stripeRow.getHeight() * 0.22f) + stripeRow.getHeight() * Hud::kShear * 1.6f;
    Hud::speedStripes (g, stripeRow.withX (band.getRight() - lean - 6.0f - rightWidth).withWidth (rightWidth), space.withAlpha (0.45f), 2);

    juce::String words = BannerLogic::textFor (kind);
    if (kind == BannerLogic::Kind::Listening)
        words << "  " << juce::String (m_captureSeconds, 1) << " S";

    // Lit: steady after the first second; LEARN FIRST blinks for one second in every four.
    const double phase = kind == BannerLogic::Kind::LearnFirst ? age - 4.0 * std::floor (age / 4.0) : age;
    const bool lit = phase >= 1.0 || blinkLit (phase);

    auto textArea = band.withLeft (band.getX() + lean + 6.0f + leftStripes + 14.0f)
                        .withRight (band.getRight() - lean - 6.0f - rightWidth - 14.0f)
                        .reduced (0.0f, bandHeight * 0.1f);
    // The recording mark gets its own room before the words.
    const float mark = bandHeight * 0.26f;
    if (kind == BannerLogic::Kind::Listening)
        textArea.removeFromLeft (mark + 16.0f);
    const float h = Hud::fittedHeight (*m_fonts, true, words, textArea.getWidth(), bandHeight * 0.66f, 12.0f);
    const auto font = m_fonts->display (h);
    Hud::drawSheared (g, words, textArea, font, lit ? space : colour.darker (0.45f), juce::Justification::centred);

    // Recording mark: a square that blinks the whole time the sound is being captured.
    if (kind == BannerLogic::Kind::Listening && blinkLit (age))
    {
        const float textLeft = textArea.getCentreX() - Hud::shearedWidth (font, words) * 0.5f;
        g.setColour (space);
        g.fillRect (juce::Rectangle<float> (textLeft - mark - 12.0f, band.getCentreY() - mark * 0.5f, mark, mark));
    }

    // Analysing: a bright bar sweeping along the band, as a scanner reading it.
    if (kind == BannerLogic::Kind::Analysing)
    {
        const double sweep = age * 0.9 - std::floor (age * 0.9);
        const float x = band.getX() + lean + (float) sweep * (band.getWidth() - lean);
        juce::Path bar;
        bar.startNewSubPath (x + lean * 0.5f, band.getY());
        bar.lineTo (x + lean * 0.5f + 10.0f, band.getY());
        bar.lineTo (x - lean * 0.5f + 10.0f, band.getBottom());
        bar.lineTo (x - lean * 0.5f, band.getBottom());
        bar.closeSubPath();
        g.setColour (text.withAlpha (0.55f));
        g.fillPath (bar);
    }
}

void Banner::paint (juce::Graphics& g)
{
    using namespace Theme::Colours;
    if (Hud::skipOnScreen (*this))
        return;
    const double now = m_clock.now();

    // The banner leaving, wiping out under the one arriving.
    const double outAge = now - m_outSince;
    if (m_outgoing != BannerLogic::Kind::None && outAge < kWipeSeconds)
        drawBand (g, m_outgoing, outAge, 1.0f, (float) (outAge / kWipeSeconds));

    if (m_kind != BannerLogic::Kind::None)
    {
        const double age = std::max (0.0, now - m_since);
        const float wipe = (float) juce::jlimit (0.0, 1.0, age / kWipeSeconds);
        drawBand (g, m_kind, age, wipe, 0.0f);

        // LEARN FIRST: chevrons climbing towards the Learn button above the display.
        if (m_kind == BannerLogic::Kind::LearnFirst && m_learnX > 0.0f)
        {
            const auto bounds = getLocalBounds().toFloat();
            const float bandTop = std::round (bounds.getCentreY() - juce::jlimit (56.0f, 92.0f, bounds.getHeight() * 0.34f) * 0.5f - 6.0f);
            const float step = 15.0f;
            const int count = std::max (1, std::min (3, (int) ((bandTop - 18.0f) / step)));
            for (int i = 0; i < count; ++i)
            {
                // A light runs up the stack, so they read as pointing upwards even when still.
                const int litIndex = (int) std::floor (now * 4.0) % (count + 1);
                const float alpha = (count - 1 - i) == litIndex ? 1.0f : 0.45f;
                const float y = bandTop - 16.0f - (float) i * step;
                Hud::chevron (g, { m_learnX, y }, 7.0f, -juce::MathConstants<float>::halfPi, 2.5f, orange.withAlpha (alpha));
            }
        }
    }
}
