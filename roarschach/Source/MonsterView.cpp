#include "MonsterView.h"

#include "Params.h"
#include "RoarschachLookAndFeel.h"

#include "NoiseTexture.h"

#include <cmath>
#include <vector>

namespace
{
    constexpr float c_quietLevel = 0.001f;      // -60 dBFS: below this the input counts as silence
    constexpr float c_sleepAfterSeconds = 1.5f;
    // Hosts stop calling processBlock when the transport stops: the features then freeze at their
    // last values. No new chunk for this long counts as silence (longer than any host buffer).
    constexpr float c_staleAfterSeconds = 0.5f;
    constexpr int c_frameHz = 30;                // GL frames per second while awake
    constexpr int c_idleSinglePassHz = 15;       // single pass: once asleep and settled (the clouds still drift; sound coming back is noticed)
    constexpr int c_idleBufferHz = 24;           // the ink buffer's tiers asleep: an even 24 Hz (moving ink steps visibly at 15)
    constexpr float c_stallSeconds = 0.25f;      // a gap this long is a stall (hidden, host busy), not a slow GPU
    constexpr int c_failAfterTicks = 3 * c_frameHz; // 3 s on screen without a GL context
    constexpr float c_maxFrameSeconds = 0.1f;   // a stalled frame doesn't make the animation jump

    // One-pole smoothing coefficient for a time constant, given the frame time.
    float smoothing (float dt, float seconds)
    {
        return 1.0f - std::exp (-dt / std::max (seconds, 1.0e-4f));
    }

    // Features come from another thread: never let a NaN or a wild value reach the shader.
    float safe (float v, float lo, float hi)
    {
        return std::isfinite (v) ? juce::jlimit (lo, hi, v) : 0.0f;
    }

    void follow (float& state, float target, float dt, float attackSeconds, float releaseSeconds)
    {
        state += (target - state) * smoothing (dt, target > state ? attackSeconds : releaseSeconds);

        if (! std::isfinite (state))
            state = 0.0f;
    }

    constexpr float c_logoBoxSize = 0.8f;      // of the view height, centred
}

MonsterView::MonsterView (const roar::MonsterFeatures& features, const roar::CreatureShared* creature)
    : m_features (features),
      m_ownCreature (creature == nullptr ? std::make_unique<roar::CreatureShared>() : nullptr),
      m_creature (creature != nullptr ? *creature : *m_ownCreature)
{
    // Baked once per process here, on the message thread (the GL thread only uploads it).
    roar::NoiseTexture::shared();

    // Not opaque: the overlay (readouts, fallback text) is painted over the GL frame.
    setOpaque (false);
    // Clicks are taken only for the right-click menu; nothing else happens on a click.
    setInterceptsMouseClicks (true, false);

    m_context.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
    m_context.setRenderer (this);
    m_context.setComponentPaintingEnabled (true);
    // No continuous repainting: the shader is a full-screen procedural one, so drawing it at the
    // display rate (up to 120 Hz) costs a lot of GPU for no visible gain. The timer below asks for
    // frames at 30 Hz, and at 15 Hz once the creature is asleep and settled.
    m_context.setContinuousRepainting (false);
    m_context.attachTo (*this);

    // The timer also refreshes the overlay text, which only repaints when it changes.
    startTimerHz (c_frameHz);
}

MonsterView::~MonsterView()
{
    stopTimer();
    m_context.detach();
    m_creature.eggVisible.store (false, std::memory_order_relaxed);
}

void MonsterView::setParams (const Params& params)
{
    m_pitchSt.store (params.pitchSt, std::memory_order_relaxed);
    m_size.store (params.size, std::memory_order_relaxed);
    m_growl.store (params.growl, std::memory_order_relaxed);
    m_liquid.store (params.liquid, std::memory_order_relaxed);
    m_roar.store (params.roar, std::memory_order_relaxed);
    m_sub.store (params.sub, std::memory_order_relaxed);
    m_gravel.store (params.gravel, std::memory_order_relaxed);
    m_detailLow.store (params.detailLow, std::memory_order_relaxed);
    m_detailHigh.store (params.detailHigh, std::memory_order_relaxed);
    m_swirlKnob.store (params.swirl, std::memory_order_relaxed);
    m_ensembleKnob.store (params.ensemble, std::memory_order_relaxed);
    m_overtoneKnob.store (params.overtone, std::memory_order_relaxed);
    m_overtoneTravel.store (params.overtoneTravel, std::memory_order_relaxed);
    m_ringKnob.store (params.ring, std::memory_order_relaxed);
    m_cleanOn.store (params.cleanOn, std::memory_order_relaxed);
    m_clean.store (params.clean, std::memory_order_relaxed);
    for (size_t i = 0; i < m_moodTarget.size(); ++i)
        m_moodTarget[i].store (params.mood[i], std::memory_order_relaxed);
    m_moodFrozen.store (params.moodFrozen, std::memory_order_relaxed);
    if (params.moodKept.has_value())
        for (size_t i = 0; i < m_moodKept.size(); ++i)
            m_moodKept[i].store ((*params.moodKept)[i], std::memory_order_relaxed);
    m_moodHasKept.store (params.moodKept.has_value(), std::memory_order_release);
}

void MonsterView::mouseDown (const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu())
        showCreatureMenu();
}

void MonsterView::showCreatureMenu()
{
    if (! isCreatureFrozen || ! setCreatureFrozen || ! resetCreature)
        return;

    enum { live = 1, frozen, reset };
    const bool isFrozen = isCreatureFrozen();
    juce::PopupMenu menu;
    menu.addSectionHeader ("Creature");
    menu.addItem (live, "Evolution: Live", true, ! isFrozen);
    menu.addItem (frozen, "Evolution: Frozen", true, isFrozen);
    menu.addSeparator();
    menu.addItem (reset, "Reset creature");
    menu.showMenuAsync (RoarschachLookAndFeel::menuOptionsFor (*this),
                        [safe = juce::Component::SafePointer<MonsterView> (this)] (int item)
                        {
                            if (safe == nullptr || item == 0)
                                return;
                            // (a Frozen creature keeps its colour with the session: the colour on screen
                            // when it is frozen, the settings' colour after a reset)
                            const auto keep = [&safe] (const std::array<std::atomic<float>, roar::CreatureMood::numAxes>& from)
                            {
                                roar::CreatureMood::Axes axes {};
                                for (size_t i = 0; i < axes.size(); ++i)
                                    axes[i] = from[i].load (std::memory_order_relaxed);
                                if (safe->keepCreatureColour)
                                    safe->keepCreatureColour (axes);
                            };
                            if (item == live || item == frozen)
                            {
                                const bool wasFrozen = safe->isCreatureFrozen();
                                safe->setCreatureFrozen (item == frozen);
                                if (item == frozen && ! wasFrozen)
                                    keep (safe->m_moodShown);
                                else if (item == live && safe->keepCreatureColour)
                                    safe->keepCreatureColour (std::nullopt);
                            }
                            else if (item == reset)
                            {
                                safe->resetCreature();
                                safe->m_moodResets.fetch_add (1, std::memory_order_relaxed);
                                if (safe->isCreatureFrozen())
                                    keep (safe->m_moodTarget);
                            }
                        });
}

juce::Image MonsterView::takeCapture()
{
    const juce::ScopedLock lock (m_captureLock);
    auto image = m_capture;
    m_capture = {};
    return image;
}

void MonsterView::setCorner (float radius, juce::Colour outside)
{
    m_cornerRadius.store (std::max (0.0f, radius), std::memory_order_relaxed);
    m_outsideArgb.store (outside.getARGB(), std::memory_order_relaxed);
}

void MonsterView::resized()
{
    m_width.store (getWidth(), std::memory_order_relaxed);
    m_height.store (getHeight(), std::memory_order_relaxed);
}

void MonsterView::timerCallback()
{
    // GL frames: full rate while awake, an even slower rate once asleep and settled (the timer
    // itself is retimed, so the frames stay evenly spaced).
    const bool buffered = m_tier.load (std::memory_order_relaxed) >= 1;
    // (a GPU still slow on its lowest tier is given fewer frames: see QualityGovernor)
    const int awakeHz = juce::jlimit (10, c_frameHz, m_awakeHz.load (std::memory_order_relaxed));
    const int wantHz = m_idle.load (std::memory_order_relaxed) ? std::min (awakeHz, buffered ? c_idleBufferHz : c_idleSinglePassHz) : awakeHz;
    if (wantHz != m_timerHz)
    {
        m_timerHz = wantHz;
        startTimerHz (wantHz);
    }
    if (! m_glGivenUp)
        m_context.triggerRepaint();
    refreshOverlay (1.0f / static_cast<float> (m_timerHz));
}

void MonsterView::setHint (const juce::String& text)
{
    m_hintText = text;
    m_hintQuietSeconds = 0.0f;
    if (m_hintShown)
    {
        m_hintShown = false;
        repaint();
    }
}

void MonsterView::refreshOverlay (float seconds)
{
    // The hint: up after the same 1.5 s of silence that puts the creature to sleep, gone for good
    // with the first sound.
    if (m_hintText.isNotEmpty())
    {
        const float input = m_features.inputLevel.load (std::memory_order_relaxed);
        bool show = m_hintShown;
        if (std::isfinite (input) && input >= c_quietLevel)
        {
            m_hintText.clear();
            show = false;
            if (onAudioHeard != nullptr)
                onAudioHeard();
        }
        else
        {
            m_hintQuietSeconds = std::min (m_hintQuietSeconds + std::max (0.0f, seconds), 60.0f);
            show = m_hintQuietSeconds >= c_sleepAfterSeconds;
        }
        if (show != m_hintShown)
        {
            m_hintShown = show;
            repaint();
        }
    }

    const float f0 = m_features.f0Hz.load (std::memory_order_relaxed);
    const bool voiced = m_features.voiced.load (std::memory_order_relaxed) > 0.5f;
    const float boost = m_features.detailBoostDb.load (std::memory_order_relaxed);
    const float cut = m_features.detailCutDb.load (std::memory_order_relaxed);

    const bool stale = m_stale.load (std::memory_order_relaxed);
    const bool live = ! stale && voiced && std::isfinite (f0) && f0 > 0.0f && f0 < 20000.0f;
    const auto f0Text = live ? "F0 " + juce::String (juce::roundToInt (f0)) + " Hz" : juce::String ("F0 --");
    // Same rule as the strip's DETAIL readout: no sign on a zero, so it never reads "-0.0".
    const auto dbText = [stale] (float db, const char* sign)
    {
        const float v = ! stale && std::isfinite (db) ? std::min (std::abs (db), 99.9f) : 0.0f;
        return v < 0.05f ? juce::String ("0.0") : sign + juce::String (v, 1);
    };
    const auto detailText = "DETAIL " + dbText (boost, "+") + " / " + dbText (cut, "-") + " dB";
    const bool gravel = ! stale && m_features.gravelActive.load (std::memory_order_relaxed) > 0.5f;
    const bool asleep = m_asleep.load (std::memory_order_relaxed);

    // attachTo() succeeds even when the native context can't be made, and then no GL callback ever
    // runs: give it a few seconds on screen before showing the fallback.
    const bool glReady = m_glReady.load (std::memory_order_relaxed);
    m_ticksWithoutGL = (glReady || ! isShowing()) ? 0 : std::min (m_ticksWithoutGL + 1, c_failAfterTicks);
    // No context after all: the view lets go of OpenGL. While it is attached the component is drawn
    // through the context's own cached image, which without a native context draws nothing, and
    // paint() (the still cloud and the line that says why) would never be reached.
    if (! m_glGivenUp && ! glReady && m_ticksWithoutGL >= c_failAfterTicks)
        giveUpOnGL();
    const bool failed = m_shaderFailed.load (std::memory_order_relaxed) || m_glGivenUp || ! m_context.isAttached()
                     || m_ticksWithoutGL >= c_failAfterTicks;

    if (f0Text != m_f0Text || detailText != m_detailText || gravel != m_gravelShown
        || asleep != m_sleepShown || failed != m_failShown)
    {
        m_f0Text = f0Text;
        m_detailText = detailText;
        m_gravelShown = gravel;
        m_sleepShown = asleep;
        m_failShown = failed;
        repaint();
    }
}

void MonsterView::giveUpOnGL()
{
    m_glGivenUp = true;
    m_context.detach();
    m_creature.eggVisible.store (false, std::memory_order_relaxed);
    repaint();
}

// Shown again after the view had let go of OpenGL (the editor reopened, or moved to a display
// that may have a context): one more try.
void MonsterView::visibilityChanged()
{
    if (m_glGivenUp && isShowing())
    {
        m_glGivenUp = false;
        m_ticksWithoutGL = 0;
        m_context.attachTo (*this);
    }
}

void MonsterView::paint (juce::Graphics& g)
{
    using namespace Theme::Colours;

    const auto bounds = getLocalBounds().toFloat();
    const float radius = m_cornerRadius.load (std::memory_order_relaxed);

    if (m_failShown)
    {
        // No GL: a still cloud in the creature view's own tones (lit above, shaded below, a few soft
        // banks, mirrored as the cloud folds round the creature) with a line saying why nothing moves.
        juce::Graphics::ScopedSaveState state (g);
        juce::Path shape;
        shape.addRoundedRectangle (bounds, radius);
        g.reduceClipRegion (shape);
        const auto cloudLit = juce::Colour::fromFloatRGBA (0.85f, 0.89f, 0.92f, 1.0f);
        const auto cloudTone = juce::Colour::fromFloatRGBA (0.580f, 0.690f, 0.757f, 1.0f);   // the cloud's tone
        g.setGradientFill (juce::ColourGradient (cloudLit, 0.0f, bounds.getY(), cloudTone, 0.0f, bounds.getBottom(), false));
        g.fillRect (bounds);
        const float h = bounds.getHeight(), cx = bounds.getCentreX();
        struct Bank { float x, y, r; bool pale; };
        const Bank banks[] = { { 0.18f, 0.30f, 0.55f, true }, { 0.52f, 0.62f, 0.70f, false }, { 0.95f, 0.38f, 0.60f, true },
                               { 1.30f, 0.70f, 0.55f, false }, { 0.0f, 0.85f, 0.45f, false } };
        for (const auto& b : banks)
            for (const float side : { -1.0f, 1.0f })
            {
                const juce::Point<float> c (cx + side * b.x * h, bounds.getY() + b.y * h);
                const auto inner = b.pale ? cloudLit.withAlpha (0.55f) : cloudTone.darker (0.25f).withAlpha (0.35f);
                // (a round soft puff squashed to a bank: its edge fades to nothing all the way round)
                juce::Graphics::ScopedSaveState squash (g);
                g.addTransform (juce::AffineTransform::scale (1.0f, 0.6f, c.x, c.y));
                g.setGradientFill (juce::ColourGradient (inner, c, inner.withAlpha (0.0f), c.translated (b.r * h, 0.0f), true));
                g.fillEllipse (juce::Rectangle<float> (2.0f * b.r * h, 2.0f * b.r * h).withCentre (c));
            }
        g.setFont (m_fonts->body (12.0f));
        const juce::String message ("OpenGL not available");
        const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), message) + 24.0f;
        const auto pill = bounds.withSizeKeepingCentre (w, 24.0f);
        g.setColour (scope.withAlpha (0.6f));
        g.fillRoundedRectangle (pill, 6.0f);
        g.setColour (textDim);
        g.drawText (message, pill, juce::Justification::centred, false);
    }

    auto row = bounds.reduced (16.0f, 12.0f).removeFromTop (16.0f);
    g.setFont (m_fonts->label (10.5f));

    // The readouts sit on a soft dark backing: the creature view is a pale, misty sky, and the
    // accent colours alone would not read on it.
    const auto readout = [&g] (const juce::String& label, juce::Rectangle<float> r, bool left, juce::Colour c)
    {
        const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), label) + 12.0f;
        const auto pill = (left ? r.withWidth (w).translated (-6.0f, 0.0f) : r.withLeft (r.getRight() - w).translated (6.0f, 0.0f)).expanded (0.0f, 2.0f);
        g.setColour (scope.withAlpha (0.6f));
        g.fillRoundedRectangle (pill, 5.0f);
        g.setColour (c);
        g.drawText (label, r, left ? juce::Justification::centredLeft : juce::Justification::centredRight, false);
    };

    readout (m_f0Text, row, true, m_f0Text == "F0 --" ? textFaint : pitch.withAlpha (0.9f));
    readout (m_detailText, row, false, Theme::Colours::detail.withAlpha (0.85f));

    auto bottom = bounds.reduced (16.0f, 12.0f).removeFromBottom (16.0f);

    if (m_gravelShown)
    {
        readout ("VOCODER  32 BANDS", bottom, true, vocoder.withAlpha (0.85f));
    }

    if (m_sleepShown)
    {
        readout ("ASLEEP", bottom, false, textDim);
    }

    if (m_hintShown && m_hintText.isNotEmpty())
    {
        // Centred on the bottom row, between the two readouts, on the same dark backing.
        g.setFont (m_fonts->body (12.0f));
        const float w = std::min (bottom.getWidth() - 360.0f, juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), m_hintText) + 20.0f);
        const auto pill = bottom.withSizeKeepingCentre (w, bottom.getHeight()).expanded (0.0f, 3.0f);
        g.setColour (scope.withAlpha (0.6f));
        g.fillRoundedRectangle (pill, 5.0f);
        g.setColour (textDim);
        g.drawText (m_hintText, pill.reduced (8.0f, 0.0f), juce::Justification::centred, true);
    }

    g.setColour (outlineSoft);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
}

void MonsterView::newOpenGLContextCreated()
{
    createRenderer (m_context.isCoreProfile());
}

void MonsterView::createRenderer (bool coreProfile)
{
    m_firstFrame = true;
    const int w = m_width.load (std::memory_order_relaxed), h = m_height.load (std::memory_order_relaxed);
    const float aspect = w > 0 && h > 0 ? (float) w / (float) h : 3.27f;
    const auto& noise = roar::NoiseTexture::shared();

    if (! m_renderer.create (coreProfile, aspect, m_logo.rgba, LogoSdf::c_size, noise.rgba, noise.size))
    {
        m_shaderFailed.store (true, std::memory_order_relaxed);
        return;
    }

    m_renderer.setFixedStep (m_testFixedStep.load (std::memory_order_relaxed));
    m_tier.store (m_renderer.getTier(), std::memory_order_relaxed);

    // The egg can be seen only while this context has the logo to draw.
    m_creature.eggVisible.store (m_renderer.hasLogo(), std::memory_order_relaxed);

    m_jawMotion.reset();
    m_head.reset();
    m_audioForces.reset();
    m_ripples.reset();
    m_evoSnap = true;
    m_shaderFailed.store (false, std::memory_order_relaxed);
    m_glReady.store (true, std::memory_order_relaxed);
}

void MonsterView::openGLContextClosing()
{
    m_creature.eggVisible.store (false, std::memory_order_relaxed);
    m_glReady.store (false, std::memory_order_relaxed);
    releaseGL();
}

void MonsterView::releaseGL()
{
    m_renderer.release();
    m_jawMotion.reset();
    m_head.reset();
    m_audioForces.reset();
    m_ripples.reset();
    m_tier.store (-1, std::memory_order_relaxed);
}

void MonsterView::updateAnimation (float dt)
{
    const auto& f = m_features;

    const int generation = f.generation.load (std::memory_order_relaxed);

    if (generation != m_lastGeneration)
    {
        m_lastGeneration = generation;
        m_staleSeconds = 0.0f;
    }
    else
    {
        m_staleSeconds = std::min (m_staleSeconds + dt, 60.0f);
    }

    // Stale features read as silence: the creature closes its mouth and drifts off to sleep.
    const bool stale = m_staleSeconds >= c_staleAfterSeconds;
    m_stale.store (stale, std::memory_order_relaxed);
    const float live = stale ? 0.0f : 1.0f;
    const auto load = [live] (const std::atomic<float>& a) { return a.load (std::memory_order_relaxed) * live; };

    const float input = safe (load (f.inputLevel), 0.0f, 4.0f);

    // Asleep after 1.5 s of silence; wakes quickly when sound comes back.
    m_quietSeconds = input < c_quietLevel ? std::min (m_quietSeconds + dt, 60.0f) : 0.0f;
    const bool asleep = m_quietSeconds >= c_sleepAfterSeconds;
    follow (m_sleep, asleep ? 1.0f : 0.0f, dt, 0.9f, 0.15f);
    m_asleep.store (asleep, std::memory_order_relaxed);

    // (m_jaw drives the formation and the egg's shout detector; the drawn jaw is the spring below)
    const float jawIn = safe (load (f.jaw), 0.0f, 1.0f);
    m_jawFast = safe (load (f.jawFast), 0.0f, 1.0f);
    follow (m_jaw, jawIn, dt, 0.012f, 0.060f);
    follow (m_growlMod, safe (load (f.growlMod), -1.0f, 1.0f), dt, 0.008f, 0.008f);
    follow (m_roarDrive, safe (load (f.roarHeat), 0.0f, 1.0f), dt, 0.060f, 0.25f);
    follow (m_subLevel, safe (load (f.subLevel), 0.0f, 1.0f), dt, 0.030f, 0.20f);
    follow (m_level, safe (load (f.outputLevel), 0.0f, 1.0f), dt, 0.030f, 0.20f);
    {
        // how loud the voice coming in is (the ink creature swells and darkens with it)
        const float t = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (input, -100.0f) + 30.0f) / 22.0f);
        follow (m_inputLoud, t * t * (3.0f - 2.0f * t), dt, 0.12f, 0.9f);
    }
    follow (m_gravelOn, load (f.gravelActive) > 0.5f ? 1.0f : 0.0f, dt, 0.15f, 0.30f);

    for (size_t i = 0; i < m_bands.size(); ++i)
        follow (m_bands[i], safe (load (f.vocoderBands[i]), 0.0f, 1.0f), dt, 0.015f, 0.12f);

    const auto knob = [dt] (float& state, const std::atomic<float>& target, float lo, float hi)
    {
        follow (state, safe (target.load (std::memory_order_relaxed), lo, hi), dt, 0.12f, 0.12f);
    };
    knob (m_knobs.pitchSt, m_pitchSt, -24.0f, 12.0f);
    knob (m_knobs.size, m_size, 0.0f, 1.0f);
    knob (m_knobs.growl, m_growl, 0.0f, 1.0f);
    knob (m_knobs.liquid, m_liquid, 0.0f, 1.0f);
    knob (m_knobs.roar, m_roar, 0.0f, 1.0f);
    knob (m_knobs.sub, m_sub, 0.0f, 1.0f);
    knob (m_knobs.gravel, m_gravel, 0.0f, 1.0f);
    knob (m_knobs.detailLow, m_detailLow, 0.0f, 1.0f);
    knob (m_knobs.detailHigh, m_detailHigh, 0.0f, 1.0f);
    knob (m_knobs.swirl, m_swirlKnob, 0.0f, 1.0f);
    knob (m_knobs.ensemble, m_ensembleKnob, 0.0f, 1.0f);
    knob (m_knobs.overtone, m_overtoneKnob, 0.0f, 1.0f);
    knob (m_knobs.overtoneTravel, m_overtoneTravel, 0.0f, 1.0f);
    knob (m_knobs.ring, m_ringKnob, 0.0f, 1.0f);

    // The other modules (each feature already fades to 0 with its module's On switch).
    follow (m_swirlNotch, safe (load (f.swirlNotch), 0.0f, 1.0f), dt, 0.05f, 0.05f);
    follow (m_ensWidth, safe (load (f.ensembleWidth), 0.0f, 1.0f), dt, 0.15f, 0.30f);
    follow (m_deess, std::abs (safe (load (f.deessGrDb), -24.0f, 24.0f)) / 8.0f, dt, 0.01f, 0.12f);
    follow (m_tame, std::abs (safe (load (f.tameGrDb), -24.0f, 24.0f)) / 6.0f, dt, 0.05f, 0.30f);
    const float gate = safe (load (f.gateOpen), 0.0f, 1.0f);
    const float cleanOn = safe (m_cleanOn.load (std::memory_order_relaxed), 0.0f, 1.0f);
    follow (m_gate, 1.0f - cleanOn * (1.0f - gate), dt, 0.02f, 0.25f);
    follow (m_ovLevel, safe (load (f.overtoneLevel), 0.0f, 1.0f), dt, 0.03f, 0.20f);
    for (size_t k = 0; k < m_ovHarm.size(); ++k)
        follow (m_ovHarm[k], safe (load (f.overtoneHarm[k]), 0.0f, 1.0f), dt, 0.02f, 0.15f);
    follow (m_ovReplace, safe (load (f.overtoneReplace), 0.0f, 1.0f), dt, 0.2f, 0.2f);
    follow (m_ring, safe (load (f.ringAmount), 0.0f, 1.0f), dt, 0.1f, 0.2f);
    follow (m_ringBody, safe (load (f.ringBodyMix), 0.0f, 1.0f), dt, 0.1f, 0.2f);
    {
        // The ripples move at the carrier, halved until it is slow enough to watch (8 Hz or slower).
        float rate = safe (load (f.ringHz), 0.0f, 20000.0f);
        while (rate > 8.0f)
            rate *= 0.5f;
        m_ringPhase = std::fmod (m_ringPhase + dt * rate, 1.0f);
    }
    follow (m_strain, std::abs (safe (load (f.limiterGrDb), -48.0f, 48.0f)) / 9.0f, dt, 0.03f, 0.35f);
    follow (m_clip, std::abs (safe (load (f.clipDb), -48.0f, 48.0f)) / 4.0f, dt, 0.01f, 0.25f);

    // CHAMBER: how much the room rings, for about as long as its decay (more with Resonance); Boom
    // adds a slow pulse that swells with the level.
    {
        const float decay = safe (load (f.chamberDecay), 0.0f, 2.0f);
        const float ring = safe (load (f.chamberRing), 0.0f, 1.0f) * (0.3f + 0.7f * safe (load (f.chamberResonance), 0.0f, 1.0f));
        follow (m_chamberEcho, ring, dt, 0.05f, std::max (0.15f, decay));
        follow (m_chamberBoom, safe (load (f.chamberBoom), 0.0f, 1.0f), dt, 0.30f, 0.60f);
        m_chamberPhase = std::fmod (m_chamberPhase + dt * 0.7f, 1.0f);
    }

    // Rings travel faster while the sub is loud.
    m_subPhase = std::fmod (m_subPhase + dt * (0.30f + 0.9f * m_subLevel), 1.0f);

    // The colour mood: drifts slowly to the settings' (a spring of several seconds), from where it
    // is when the view opens; held while the creature is Frozen.
    {
        roar::CreatureMood::Axes target {};
        for (size_t i = 0; i < target.size(); ++i)
            target[i] = safe (m_moodTarget[i].load (std::memory_order_relaxed), 0.0f, 1.0f);
        if (m_testMood[0].load (std::memory_order_relaxed) >= 0.0f)
            for (size_t i = 0; i < target.size(); ++i)
                target[i] = safe (m_testMood[i].load (std::memory_order_relaxed), 0.0f, 1.0f);
        const int resets = m_moodResets.load (std::memory_order_relaxed);
        const bool reset = resets != m_moodResetsSeen;
        m_moodResetsSeen = resets;
        const bool frozen = m_moodFrozen.load (std::memory_order_relaxed);
        roar::CreatureMood::Axes kept {};
        const bool hasKept = m_moodHasKept.load (std::memory_order_acquire);
        for (size_t i = 0; i < kept.size(); ++i)
            kept[i] = safe (m_moodKept[i].load (std::memory_order_relaxed), 0.0f, 1.0f);
        if (m_moodSnap)
            m_mood.snap (target, frozen, hasKept ? &kept : nullptr);
        m_mood.step (m_moodSnap ? 0.0f : dt, target, frozen, reset, hasKept ? &kept : nullptr);
        for (size_t i = 0; i < m_moodShown.size(); ++i)
            m_moodShown[i].store (m_mood.value[i], std::memory_order_relaxed);
        // the colour on screen: follows the mood's tone, its hue turning at a bounded speed
        const auto tone = roar::CreatureMood::tone (m_mood.value);
        if (m_moodSnap)
            m_tone.snap (tone);
        else
            m_tone.step (dt, tone);
        m_moodSnap = false;
    }

    // The evolved form: the processor publishes it ten times a second; smooth it so it never steps.
    {
        using E = roar::CreatureEvolution;
        const auto target = m_creature.read();
        const float k = m_evoSnap ? 1.0f : smoothing (dt, 0.15f);
        // The seeds the blots grow from are not smoothed. When they move (a reset, or a queued seed
        // taking over) the glide between them is taken as published too: the published glide
        // already moves smoothly, and an old glide value with new endpoints would show the new blot
        // for a moment and swing back.
        const auto moved = [&target, this] (int i) { return std::abs (target[(size_t) i] - m_evo[(size_t) i]) > 1.0e-6f; };
        const bool endpointsMoved = moved (E::anatomyALo) || moved (E::anatomyAHi) || moved (E::anatomyBLo) || moved (E::anatomyBHi);
        for (size_t i = 0; i < m_evo.size(); ++i)
        {
            const bool seed = i >= (size_t) E::anatomyALo && i <= (size_t) E::anatomyBHi;   // (never smoothed)
            const bool snap = seed || (endpointsMoved && i == (size_t) E::seedGlide);
            m_evo[i] = snap ? target[i] : m_evo[i] + (target[i] - m_evo[i]) * k;
        }

        // Formation: a roar pulls the creature out of the cloud now, sleep disperses it again.
        const float loud = juce::jmap (juce::jlimit (-36.0f, -10.0f, juce::Decibels::gainToDecibels (m_level, -100.0f)), -36.0f, -10.0f, 0.0f, 1.0f);
        const float loudS = loud * loud * (3.0f - 2.0f * loud);
        if (m_evoSnap)
        {
            // (the ink creature opens on silence as it sleeps: cloud, not a blot that then dissolves)
            if (input < c_quietLevel)
            {
                m_quietSeconds = c_sleepAfterSeconds;
                m_sleep = 1.0f;
            }
            m_formation.formation = m_evo[roar::CreatureEvolution::formBase] * (1.0f - 0.85f * m_sleep);
            m_formation.fast = 0.0f;
            m_formation.ink = 1.0f - m_sleep;
        }
        m_formation.step (dt, m_jaw, loudS, m_roarDrive, m_sleep, m_evo[roar::CreatureEvolution::formBase],
                          m_evo[roar::CreatureEvolution::egg]);
        m_evoSnap = false;

        // Slow frames only once asleep and the cloud has finished dispersing (the formation has
        // settled on its sleeping target and no loud moment is still fading).
        const float base = m_evo[roar::CreatureEvolution::formBase];
        const float sleepTarget = juce::jlimit (0.0f, 1.0f, base + 0.35f * m_formation.fast * (1.0f - base)) * (1.0f - 0.85f * m_sleep);
        const bool settled = m_formation.fast < 0.01f && std::abs (m_formation.formation - sleepTarget) < 0.01f;
        m_idle.store (asleep && m_sleep > 0.98f && settled, std::memory_order_relaxed);
    }

    // The jaw and the head: a heavy spring on the phrase, the lips carry the syllables.
    {
        const float sizeJ = juce::jlimit (0.0f, 1.0f, m_knobs.size + m_evo[roar::CreatureEvolution::jawMass]);
        m_jawMotion.step (dt, jawIn, m_jawFast, m_roarDrive, sizeJ);

        const int mask = f.moduleOnMask.load (std::memory_order_relaxed);
        const float swirlOn = ((mask >> static_cast<int> (::Params::ModuleId::swirl)) & 1) != 0 ? m_knobs.swirl : 0.0f;
        roar::AudioForces::Inputs in;
        in.roarHeat = m_roarDrive;
        in.growlMod = m_growlMod * m_knobs.growl;
        in.subLevel = m_subLevel;
        in.swirlOn = swirlOn * live;
        in.ensemble = m_ensWidth;
        in.clip = juce::jlimit (0.0f, 1.0f, m_clip);
        in.strain = juce::jlimit (0.0f, 1.0f, m_strain);
        in.jawVel = m_jawMotion.v;
        in.awake = 1.0f - m_sleep;
        m_audioForces.step (dt, in);

        // hits ripple the picture; a sustained low end trembles it (stale features: neither)
        // (a voice alone, a faint shimmer)
        const float shimmer = load (f.rumble) + 0.12f * juce::jlimit (0.0f, 1.0f, m_level * 2.0f);
        m_ripples.step (dt, f.hits.load (std::memory_order_relaxed), load (f.hitStrength), shimmer, in.awake * live, m_audioForces);

        m_head.step (dt, m_jawMotion.y, m_jawMotion.P, m_jawMotion.justEngaged, m_jawMotion.dPdt, m_jawMotion.roarHold,
                     m_audioForces.growl, m_sleep);
    }
}

void MonsterView::renderOpenGL()
{
    using namespace juce::gl;
    using roar::U;

    const auto now = Clock::now();
    const float raw = m_firstFrame ? 0.0f : std::chrono::duration<float> (now - m_lastFrame).count();
    const float dt = juce::jlimit (0.0f, c_maxFrameSeconds, raw);
    m_lastInterval = m_firstFrame ? 1.0f / (float) c_frameHz : raw;
    // (a stall: only a long gap; a slow GPU's 50-100 ms frames are real measurements)
    m_lastClamped = raw > c_stallSeconds;
    m_lastFrame = now;
    m_firstFrame = false;

    const float scale = (float) m_context.getRenderingScale();
    const int width = juce::roundToInt (scale * (float) m_width.load (std::memory_order_relaxed));
    const int height = juce::roundToInt (scale * (float) m_height.load (std::memory_order_relaxed));

    glViewport (0, 0, width, height);
    juce::OpenGLHelpers::clear (Theme::Colours::scope);

    if (! m_renderer.isCreated() || width <= 0 || height <= 0)
        return;

    drawFrame (dt, scale, width, height);
}

void MonsterView::renderOffline (float dt, int width, int height, bool coreProfile)
{
    m_width.store (width, std::memory_order_relaxed);
    m_height.store (height, std::memory_order_relaxed);
    if (! m_renderer.isCreated())
        createRenderer (coreProfile);
    if (! m_renderer.isCreated() || width <= 0 || height <= 0)
        return;
    m_lastInterval = dt;
    m_lastClamped = false;
    drawFrame (dt, 1.0f, width, height);
}

void MonsterView::releaseOffline()
{
    openGLContextClosing();
}

// One frame: the animation stepped by dt, the frame's uniforms, the renderer, the capture.
void MonsterView::drawFrame (float dt, float scale, int width, int height)
{
    using namespace juce::gl;
    using roar::U;

    updateAnimation (dt);

    auto& fr = m_frame;
    const auto rgb = [&fr] (U u, juce::Colour c) { fr.set (u, c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue()); };

    fr.set (U::uRes, (float) width, (float) height);
    fr.set (U::uPxScale, scale);
    fr.set (U::uRadius, m_cornerRadius.load (std::memory_order_relaxed) * scale);
    const float testJaw = m_testJaw.load (std::memory_order_relaxed);
    fr.set (U::uJaw, testJaw >= 0.0f ? testJaw : m_jawMotion.y);
    fr.set (U::uJawVel, m_jawMotion.v);
    fr.set (U::uLip, m_jawMotion.lip);
    {
        // the head's motion moves the blot up and down and breathes its scale; CHAMBER's Boom pulses in it
        const float pulse = 0.5f - 0.5f * std::cos (m_chamberPhase * juce::MathConstants<float>::twoPi);
        const float boom = juce::jlimit (0.0f, 1.0f, m_chamberBoom) * juce::jlimit (0.0f, 1.0f, m_level * 2.0f) * (1.0f - m_sleep);
        fr.set (U::uHead, m_head.offY, m_head.scaleAdd + 0.008f * boom * pulse, 0.0f, m_head.breath + 0.6f * boom * pulse);
    }
    // (CHAMBER: the room's ringing, how much room there is, the ring's slow clock)
    fr.set (U::uChamber, juce::jlimit (0.0f, 1.0f, m_chamberEcho), juce::jlimit (0.0f, 1.0f, m_chamberBoom), m_chamberPhase, 0.0f);
    fr.set (U::uGrowl, m_knobs.growl);
    fr.set (U::uGrowlMod, juce::jlimit (0.0f, 1.0f, m_audioForces.growl));   // rectified and slow: a shiver, never a 20-80 Hz flicker
    fr.set (U::uLiquid, m_knobs.liquid);
    fr.set (U::uRoar, m_knobs.roar);
    fr.set (U::uRoarDrive, m_roarDrive);
    fr.set (U::uSub, m_knobs.sub);
    fr.set (U::uSubLevel, m_subLevel);
    fr.set (U::uSubPhase, m_subPhase);
    fr.set (U::uPitch, m_knobs.pitchSt);
    fr.set (U::uSize, m_knobs.size);
    fr.set (U::uGravelOn, m_gravelOn * (0.35f + 0.65f * m_knobs.gravel));
    fr.set (U::uDetailLow, m_knobs.detailLow);
    fr.set (U::uDetailHigh, m_knobs.detailHigh);
    fr.set (U::uSleep, m_sleep);
    fr.set (U::uLevel, juce::jlimit (0.0f, 1.0f, m_level * 2.0f));
    fr.setArray (U::uBands, m_bands.data(), c_numBands);
    fr.set (U::uSwirlNotch, m_swirlNotch);
    fr.set (U::uSwirlAmt, m_knobs.swirl * juce::jlimit (0.0f, 1.0f, m_audioForces.swirl * 4.0f));
    fr.set (U::uEnsWidth, juce::jlimit (0.0f, 1.0f, m_ensWidth * (0.4f + 0.6f * m_knobs.ensemble)));
    fr.set (U::uDeess, juce::jlimit (0.0f, 1.0f, m_deess));
    fr.set (U::uTame, juce::jlimit (0.0f, 1.0f, m_tame));
    fr.set (U::uGateClosed, juce::jlimit (0.0f, 1.0f, 1.0f - m_gate));
    fr.set (U::uClean, safe (m_clean.load (std::memory_order_relaxed), 0.0f, 1.0f));
    fr.set (U::uOvLevel, juce::jlimit (0.0f, 1.0f, m_ovLevel * (0.5f + m_knobs.overtone)));
    fr.setArray (U::uOvHarm, m_ovHarm.data(), (int) m_ovHarm.size());
    fr.set (U::uOvReplace, m_ovReplace);
    fr.set (U::uRing, juce::jlimit (0.0f, 1.0f, m_ring));
    fr.set (U::uRingFlicker, 0.5f + 0.5f * std::sin (m_ringPhase * juce::MathConstants<float>::twoPi));
    fr.set (U::uRingBody, m_ringBody);
    fr.set (U::uStrain, juce::jlimit (0.0f, 1.0f, m_strain));
    {
        // the ripples: each ring's radius, amplitude and middle (above or below the body's), the tremble
        const auto& rp = m_ripples;
        const float on = m_testRipplesOff.load (std::memory_order_relaxed) ? 0.0f : 1.0f;
        fr.set (U::uRippleR, rp.radius (0), rp.radius (1), rp.radius (2), rp.radius (3));
        fr.set (U::uRippleA, on * rp.amplitude (0), on * rp.amplitude (1), on * rp.amplitude (2), on * rp.amplitude (3));
        fr.set (U::uRippleY, rp.rings[0].offsetY, rp.rings[1].offsetY, rp.rings[2].offsetY, rp.rings[3].offsetY);
        fr.set (U::uRippleT, m_testTrembleOff.load (std::memory_order_relaxed) ? 0.0f : rp.tremble, rp.trembleClock, 0.0f, 0.0f);
    }

    rgb (U::uColOuter, juce::Colour (m_outsideArgb.load (std::memory_order_relaxed)));

    {
        // the evolved creature: its stage and characters (the anatomy), the cloud's light, the mark
        using E = roar::CreatureEvolution;
        fr.set (U::uStage, juce::jlimit (0.0f, 1.0f, m_evo[E::stageOut]));
        fr.set (U::uEvoChar, m_evo[E::charFluid], m_evo[E::charBeast], m_evo[E::charEldritch], m_evo[E::charDeep]);
        fr.set (U::uColCloudLit, m_evo[E::cloudLitR], m_evo[E::cloudLitG], m_evo[E::cloudLitB]);
        // No texture, no logo (the egg is skipped rather than drawn against an empty field).
        fr.set (U::uEgg, m_renderer.hasLogo() ? m_evo[E::egg] : 0.0f);
        fr.set (U::uLogoBox, 0.0f, 0.0f, c_logoBoxSize, 0.0f);
    }

    {
        const auto turn = roar::CreatureMood::shaderMood (m_tone.value);
        fr.set (U::uMood, turn[0], turn[1], turn[2], 0.0f);
    }

    const bool idle = m_idle.load (std::memory_order_relaxed);
    fr.frameDt = dt;
    fr.interval = m_lastInterval;
    fr.dtClamped = m_lastClamped;
    fr.asleep = idle;
    {
        const int awakeHz = juce::jlimit (10, c_frameHz, m_awakeHz.load (std::memory_order_relaxed));
        const int idleHz = m_tier.load (std::memory_order_relaxed) >= 1 ? c_idleBufferHz : c_idleSinglePassHz;
        fr.targetInterval = 1.0f / (float) (idle ? std::min (awakeHz, idleHz) : awakeHz);
    }
    fr.forces = m_audioForces;
    {
        // the seeds the anatomy grows from, and the creature's formation
        using E = roar::CreatureEvolution;
        const auto seedOf = [this] (int lo, int hi)
        {
            return (std::uint32_t) juce::jlimit (0.0f, 65535.0f, m_evo[(size_t) lo]) | ((std::uint32_t) juce::jlimit (0.0f, 65535.0f, m_evo[(size_t) hi]) << 16);
        };
        fr.seedFrom = seedOf (E::anatomyALo, E::anatomyAHi);
        fr.seedTo = seedOf (E::anatomyBLo, E::anatomyBHi);
        fr.seedGlide = juce::jlimit (0.0f, 1.0f, m_evo[E::seedGlide]);
        const float testFormation = m_testFormation.load (std::memory_order_relaxed);
        fr.inputLoud = m_inputLoud;
        fr.overtoneAmount = m_knobs.overtoneTravel;
        fr.inkFormation = testFormation >= 0.0f ? juce::jlimit (0.0f, 1.0f, testFormation)
                                                : m_formation.inkFormation();
    }

    m_renderer.setFixedStep (m_testFixedStep.load (std::memory_order_relaxed));
    m_renderer.prepare (fr);

    GLint hostFbo = 0;
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &hostFbo);
    const int viewport[4] = { 0, 0, width, height };
    m_renderer.render (fr, (unsigned int) hostFbo, viewport);
    m_tier.store (m_renderer.getTier(), std::memory_order_relaxed);
    m_awakeHz.store (m_renderer.awakeHz(), std::memory_order_relaxed);

    if (m_captureRequested.exchange (false, std::memory_order_relaxed))
    {
        std::vector<juce::uint8> pixels ((size_t) (width * height * 4));
        glPixelStorei (GL_PACK_ALIGNMENT, 1);
        glReadPixels (0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        juce::Image image (juce::Image::RGB, width, height, false);
        {
            juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const auto* px = pixels.data() + ((size_t) (height - 1 - y) * (size_t) width + (size_t) x) * 4;
                    data.setPixelColour (x, y, juce::Colour (px[0], px[1], px[2]));
                }
        }
        const juce::ScopedLock lock (m_captureLock);
        m_capture = image;
    }
}
