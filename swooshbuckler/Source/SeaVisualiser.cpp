#include "SeaVisualiser.h"

namespace
{
    constexpr float c_camHeight = 0.62f;
    constexpr float c_halfWidth = 2.4f;
    constexpr float c_arcZ = 2.7f;      // cannonball passes start and end this far out
    constexpr float c_arcX = 1.75f;     // and this far either side
    constexpr float c_subLift = 0.17f;  // how far a sub at full level lifts the water (the camera is 0.62 above it)
    constexpr float c_roomNearZ = 1.3f; // in a room, the back wall: about the nearest a close pass ever came
    constexpr float c_roomAcross = 0.65f; // and the side walls this far out of the comets' width: a pass Across a room
                                          // moves evenly, so it is drawn no faster across the sky than a Stereo one

    // The water's own colours. Depth, not loudness, picks them, and none of them is a bus colour (coral,
    // turquoise, cherry): the buses own the comets, the water owns the distance.
    const juce::Colour c_deepWater { 0xff1f5f82 };  // far out: cold deep blue
    const juce::Colour c_nearWater { 0xff79b6c6 };  // under the camera: moonlit steel
    const juce::Colour c_fog       { 0xff0e2a38 };  // what the horizon fades everything into
    const juce::Colour c_hollow    { 0xff071820 };  // the water between two rows
    const juce::Colour c_moonlight { 0xfff3e3c0 };

    // How much of the mark's own shapes shows behind its stars: the grenade is the solid thing in the mark, so it
    // is the heavier of the two; the cherry in front of it cuts a dark line round itself out of the grenade.
    constexpr float c_glowGrenade = 0.15f, c_glowCherry = 0.10f, c_keyline = 1.6f;
    constexpr float c_hazeMargin = 2.0f;

    /** The water at a distance: 1 at the camera, 0 at the horizon. */
    juce::Colour waterColour(float nearness, float height)
    {
        const auto lit = c_deepWater.interpolatedWith(c_nearWater, nearness).interpolatedWith(c_moonlight, 0.30f * height);
        const float d = 1.0f - nearness;
        const float haze = d * d * (3.0f - 2.0f * d); // horizon fog: the far rows go hazy, the near ones do not
        return lit.interpolatedWith(c_fog, haze * 0.82f);
    }
}

SeaVisualiser::SeaVisualiser(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_fonts(fonts)
{
    setOpaque(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setTooltip("The Sea\nClick it (or press Space) to swell every track up.");

    juce::Random r(42);
    for (int i = 0; i < 26; ++i) // the twinkling few: the still ones are painted into the sky image
        m_stars.push_back({ r.nextFloat(), r.nextFloat() * r.nextFloat(), 0.9f + 1.3f * r.nextFloat(), r.nextFloat() * 6.28f });

    m_logo = std::make_unique<LogoOutline>(BinaryData::sjlogo_svg, static_cast<size_t>(BinaryData::sjlogo_svgSize));
    buildConstellation();

    for (auto& row : m_rows)
        row.fill(0.0f);

    // One frame clock: the screen's own when there is a window, a timer when there is not (the offline rigs).
    m_vblank = juce::VBlankAttachment(this, [this](double timestamp)
    {
        const float dt = m_lastVBlank > 0.0 ? juce::jlimit(1.0f / 240.0f, 1.0f / 20.0f, static_cast<float>(timestamp - m_lastVBlank))
                                            : 1.0f / 60.0f;
        m_lastVBlank = timestamp;
        m_vblankSeenMs = juce::Time::getMillisecondCounter();
        if (! m_timerIsWatch)
        {
            // The timer is only the watch now: enough to notice if the screen's clock stops. Put down once, not
            // on every frame - restarting a timer takes the timer thread's lock.
            m_timerIsWatch = true;
            startTimerHz(4);
        }
        step(dt);
    });
    startTimerHz(60);
}

SeaVisualiser::~SeaVisualiser()
{
    m_vblank = {};
    stopTimer();
}

void SeaVisualiser::timerCallback()
{
    // The screen's clock has it in hand: nothing to do. (Offline there is no vblank, so this is the clock.)
    if (m_vblankSeenMs != 0 && juce::Time::getMillisecondCounter() - m_vblankSeenMs < 500)
        return;
    if (m_timerIsWatch)
    {
        m_timerIsWatch = false;
        startTimerHz(60);  // no screen clock (or it stopped): this is the clock again
    }
    step(1.0f / 60.0f);
}

void SeaVisualiser::mouseDown(const juce::MouseEvent& e)
{
    // A ripple where it landed on the water, and the swell it has always given.
    const auto p = e.position;
    if (m_focal > 0.0f && p.y > m_horizon + 2.0f)
    {
        const float z = juce::jlimit(m_zNear, 6.0f, c_camHeight * m_focal / juce::jmax(1.0f, p.y - m_horizon));
        m_ripples[static_cast<size_t>(m_rippleNext)] = { (p.x - m_cx) * z / m_focal, z, 0.0f };
        m_rippleNext = (m_rippleNext + 1) % static_cast<int>(m_ripples.size());
    }
    if (onClick)
        onClick();
}

void SeaVisualiser::setDimmedUnder(juce::Rectangle<int> dock)
{
    if (m_dock == dock.toFloat())
        return;
    m_dock = dock.toFloat();
    repaint();
}

void SeaVisualiser::showConstellation(bool holdOn)
{
    m_constellationHeld = holdOn;
    m_constellationLeft = juce::jmax(m_constellationLeft, 6.0f);
    m_constellationOpacity = juce::jmax(m_constellationOpacity, holdOn ? 1.0f : 0.02f);
}

void SeaVisualiser::analyse()
{
    m_processor.copyScope(m_fftData.data(), c_fftSize);
    std::fill(m_fftData.begin() + c_fftSize, m_fftData.end(), 0.0f);
    m_window.multiplyWithWindowingTable(m_fftData.data(), c_fftSize);
    m_fft.performFrequencyOnlyForwardTransform(m_fftData.data(), true);

    const float rate = static_cast<float>(m_processor.getSampleRate() > 0.0 ? m_processor.getSampleRate() : 48000.0);
    const float lo = 35.0f, hi = juce::jmin(18000.0f, rate * 0.45f);
    for (int b = 0; b < c_bands; ++b)
    {
        const float f0 = lo * std::pow(hi / lo, static_cast<float>(b) / c_bands);
        const float f1 = lo * std::pow(hi / lo, static_cast<float>(b + 1) / c_bands);
        int k0 = static_cast<int>(f0 / rate * c_fftSize);
        int k1 = juce::jmax(k0 + 1, static_cast<int>(f1 / rate * c_fftSize));
        k0 = juce::jlimit(1, c_fftSize / 2 - 1, k0);
        k1 = juce::jlimit(k0 + 1, c_fftSize / 2, k1);
        float mag = 0.0f;
        for (int k = k0; k < k1; ++k)
            mag = juce::jmax(mag, m_fftData[static_cast<size_t>(k)]);
        // Tilt +3 dB/oct so noise reads flat, then map -84..-6 dB to 0..1
        const float db = juce::Decibels::gainToDecibels(mag * 4.0f / c_fftSize, -120.0f) + 3.0f * std::log2(f0 / 1000.0f);
        const float v = juce::jlimit(0.0f, 1.0f, (db + 84.0f) / 78.0f);
        auto& s = m_smooth[static_cast<size_t>(b)];
        s += (v - s) * (v > s ? 0.55f : 0.18f);
    }
}

void SeaVisualiser::step(float dt)
{
    // Under a panel that covers the window the sea holds still: a dimmed picture, not 60 repaints a second behind it.
    if (Theme::isCovered(*this))
        return;
    // The sea keeps its own rate: 60 a second however fast the screen is, and after five seconds of quiet 20 a
    // second - a big picture that costs little when there is nothing to show. The time between steps is gathered
    // up and handed on whole, so the swell rolls at the same speed whichever rate it is running at.
    m_sinceStep += juce::jlimit(0.0f, 0.25f, dt);
    const float want = m_quietSeconds > 5.0f ? c_idleStepSeconds : c_stepSeconds;
    if (m_sinceStep < want)
        return;
    dt = m_sinceStep;
    m_sinceStep = 0.0f;

    m_time += dt;
    analyse();
    refreshBusLabels();

    // The swell rolls away smoothly: the rows are drawn between steps, not only on them.
    m_rowPhase += dt * c_rowsPerSecond;
    while (m_rowPhase >= 1.0f)
    {
        m_rowPhase -= 1.0f;
        m_rowHead = (m_rowHead + 1) % c_rows;
        auto& row = m_rows[static_cast<size_t>(m_rowHead)];
        for (int b = 0; b < c_bands; ++b)   // the new row takes the wake a pass left, and rolls away with it
            row[static_cast<size_t>(b)] = m_smooth[static_cast<size_t>(b)] + m_wake[static_cast<size_t>(b)];
    }
    for (auto& w : m_wake)
        w *= std::pow(0.02f, dt);

    bool anyFlight = false;
    const auto& engine = m_processor.getEngine();
    stepRoom(dt);
    // In a room each comet is an object: a bus's two passes are its first two, the throws the next four, in the
    // order the flights are numbered (RigEngine::Object).
    static_assert(RigEngine::ObjPassA1 == 0 && RigEngine::ObjThrow1 == RigEngine::c_maxBuses * 2
                  && RigEngine::ObjThrow1 + RigEngine::c_throws == RigEngine::ObjHits, "the flights and the objects are numbered alike");
    for (int i = 0; i < RigEngine::c_maxFlights; ++i)
    {
        const auto ii = static_cast<size_t>(i);
        const auto object = engine.getObject(i);
        anyFlight = stepFlight(engine.getFlightView(i), m_inRoom ? &object : nullptr, m_trails[ii], m_flightEnv[ii], m_flash[ii], m_lastX[ii], dt)
                    || anyFlight;
    }

    // Stowaway: nothing while it is off. Its level shows in the Mode it is in; Fly's passes are comets of its own;
    // a Wake remembers where bus A's comet has been over the water.
    {
        const bool on = m_processor.isStowawayOn();
        const int mode = m_processor.getStowawayMode();
        const float level = on ? engine.getStowLevel() : 0.0f;
        for (int k = 0; k < ParamIDs::numStowModes; ++k)
        {
            auto& shown = m_stowShown[static_cast<size_t>(k)];
            const float want = k == mode ? level : 0.0f;
            shown += (want - shown) * juce::jmin(1.0f, (want > shown ? 14.0f : 5.0f) * dt);
            if (shown < 1.0e-4f)
                shown = 0.0f;
        }
        for (int h = 0; h < 2; ++h)
        {
            const auto hi = static_cast<size_t>(h);
            auto view = engine.getStowFlight(h);
            view.active = view.active && on && mode == ParamIDs::StowFly;
            const auto object = engine.getObject(RigEngine::ObjStow1 + h);
            anyFlight = stepFlight(view, m_inRoom ? &object : nullptr, m_stowTrails[hi], m_stowEnv[hi], m_stowFlash[hi], m_stowLastX[hi], dt)
                        || anyFlight;
        }
        // The wake: bus A's louder comet lays it down as it goes, and it lingers behind the comet.
        const float wake = m_stowShown[ParamIDs::StowWake];
        int lead = -1;
        for (int i = 0; i < 2; ++i)
            if (! m_trails[static_cast<size_t>(i)].empty() && engine.getFlightView(i).active
                && (lead < 0 || m_flightEnv[static_cast<size_t>(i)] > m_flightEnv[static_cast<size_t>(lead)]))
                lead = i;
        if (wake > 0.0f && lead >= 0)
        {
            const auto& at = m_trails[static_cast<size_t>(lead)].back();
            m_wakeTrail.push_back({ at.x, at.z, wake });
            if (static_cast<int>(m_wakeTrail.size()) > c_wakePoints)
                m_wakeTrail.pop_front();
        }
        else if (! m_wakeTrail.empty())
            m_wakeTrail.pop_front();
        anyFlight = anyFlight || level > 0.01f;
    }

    // Hits: a flash on the horizon and a ring on the water when the gate opens.
    const float gate = engine.getHitGate();
    if (gate > 0.35f && m_lastGate <= 0.35f)
        m_hitFlash = m_hitRing = 1.0f;
    m_lastGate = gate;
    m_hitFlash *= std::pow(0.0005f, dt);   // about 120 ms
    m_hitRing  *= std::pow(0.08f, dt);     // about 400 ms

    for (auto& r : m_ripples)
        if (r.age < 1.0f)
            r.age = juce::jmin(1.0f, r.age + dt * 1.6f);

    // The moon's halo warms and widens with the limiter, and the swell rides up while Swell is held.
    const float gr = juce::jlimit(0.0f, 1.0f, m_processor.getGainReductionDb() / 8.0f);
    m_limiterGlow += (gr - m_limiterGlow) * juce::jmin(1.0f, (gr > m_limiterGlow ? 18.0f : 4.0f) * dt);
    m_swell += ((m_processor.isSwellHeld() ? 1.0f : 0.0f) - m_swell) * juce::jmin(1.0f, 4.0f * dt);

    // The mark in the stars: every so often, and whenever a take is put away.
    const int takes = m_processor.getTakesGeneration();
    const bool takeSaved = m_takesSeen >= 0 && takes != m_takesSeen;
    m_takesSeen = takes;
    if (takeSaved)
        m_constellationLeft = juce::jmax(m_constellationLeft, 5.0f);

    if (m_constellationLeft > 0.0f || m_constellationHeld)
    {
        m_constellationLeft = juce::jmax(0.0f, m_constellationLeft - dt);
        m_constellationTime += dt;
        m_constellationOpacity += (1.0f - m_constellationOpacity) * juce::jmin(1.0f, 1.1f * dt);
    }
    else
    {
        m_constellationOpacity += (0.0f - m_constellationOpacity) * juce::jmin(1.0f, 0.9f * dt);
        if (m_constellationOpacity < 0.01f)
        {
            m_constellationTime = 0.0f;
            m_constellationNext -= dt;
            if (m_constellationNext <= 0.0f)
            {
                m_constellationNext = 40.0f + m_random.nextFloat() * 50.0f;
                m_constellationLeft = 6.0f + m_random.nextFloat() * 3.0f;
            }
        }
    }

    const bool quiet = ! anyFlight && m_processor.getOutputPeak() < 0.02f;
    m_quietSeconds = quiet ? m_quietSeconds + dt : 0.0f;

    repaint();
}

SeaVisualiser::P3 SeaVisualiser::roomToSea(const RigEngine::ObjectView& object, float env)
{
    // Seen from the ship: across is across, the front of the room is out towards the horizon and the back is near
    // the ship (never nearer than a comet ever came, so nothing rushes at the camera), and height is height. A
    // pass at ear level still rides a little over the water, higher the louder it is, as the comets always have.
    const float x = juce::jlimit(-1.0f, 1.0f, object.x), y = juce::jlimit(-1.0f, 1.0f, object.y), z = juce::jlimit(0.0f, 1.0f, object.z);
    return { x * c_arcX * c_roomAcross, 0.14f + 0.2f * env + 0.75f * z, juce::jmap(y, -1.0f, 1.0f, c_roomNearZ, c_arcZ + 0.2f) };
}

bool SeaVisualiser::stepFlight(const RigEngine::FlightView& v, const RigEngine::ObjectView* object, std::deque<P3>& trail, float& env,
                               float& flash, float& lastX, float dt)
{
    if (v.active)
    {
        env += (v.env - env) * juce::jmin(1.0f, 21.0f * dt);

        // Each pass is a cannonball arcing across the sea, swinging in towards us at the pass. In a room it flies
        // where its object is.
        const float across = juce::jlimit(-1.0f, 1.0f, v.x);
        P3 at;
        if (object != nullptr)
            at = roomToSea(*object, env);
        else
        {
            const float middle = 1.0f - std::abs(across);
            const float zClose = juce::jmap(v.closeness, 2.4f, 1.2f);
            at = { across * c_arcX, 0.12f + (0.22f + 0.2f * env) * std::sin(juce::MathConstants<float>::pi * 0.5f * (across + 1.0f)),
                   juce::jmap(middle, c_arcZ, zClose) };
        }
        trail.push_back(at);

        if (static_cast<int>(trail.size()) > c_trail)
            trail.pop_front();

        // It splashes: a bump in the water under it, which then rolls away with the swell.
        const float u = juce::jlimit(0.0f, 1.0f, (at.x / c_halfWidth + 1.0f) * 0.5f);
        const int band = juce::jlimit(0, c_bands - 1, juce::roundToInt(u * static_cast<float>(c_bands - 1)));
        for (int d = -3; d <= 3; ++d)
        {
            const int b = band + d;
            if (b >= 0 && b < c_bands)
                m_wake[static_cast<size_t>(b)] = juce::jmax(m_wake[static_cast<size_t>(b)],
                                                           0.55f * env * (1.0f - std::abs(static_cast<float>(d)) / 4.0f));
        }

        // Flash when it crosses in front of us
        if ((lastX < 0.0f) != (v.x < 0.0f) && std::abs(v.x - lastX) < 0.5f)
            flash = 1.0f;
        lastX = v.x;
    }
    else
    {
        env *= std::pow(0.03f, dt);
        if (! trail.empty())
            trail.pop_front();
        lastX = -2.0f;
    }
    flash *= std::pow(0.02f, dt);
    return v.active;
}

juce::Point<float> SeaVisualiser::project(P3 p) const
{
    const float z = juce::jmax(0.2f, p.z);
    return { m_cx + p.x / z * m_focal, m_horizon + (c_camHeight - p.y) / z * m_focal };
}

//==============================================================================
void SeaVisualiser::buildConstellation()
{
    m_constellation.clear();
    m_constellationLines.clear();
    if (m_logo == nullptr || ! m_logo->isLoaded())
        return;

    // The mark as stars: a few heavy ones on the corners the shape hangs on, light ones to carry the curves round.
    const auto mark = LogoConstellation::build(*m_logo);
    juce::Random r(1337);
    juce::Point<float> low { 1.0e6f, 1.0e6f }, high { -1.0e6f, -1.0e6f };
    for (const auto& s : mark.stars)
    {
        m_constellation.push_back({ s.at, 1.15f + 1.9f * s.weight + 0.2f * r.nextFloat(), r.nextFloat() * 6.28f, s.weight, s.cherry });
        low = { juce::jmin(low.x, s.at.x), juce::jmin(low.y, s.at.y) };
        high = { juce::jmax(high.x, s.at.x), juce::jmax(high.y, s.at.y) };
    }
    m_constellationBounds = m_constellation.empty() ? juce::Rectangle<float>() : juce::Rectangle<float>(low, high);
    m_constellationLines = mark.lines;
}

void SeaVisualiser::buildConstellationHaze(float pixelScale)
{
    m_constellationHaze = juce::Image();
    if (m_constellation.empty() || m_logo == nullptr)
        return;

    const auto area = constellationArea().expanded(c_hazeMargin);
    const float unit = (area.getWidth() - 2.0f * c_hazeMargin) / juce::jmax(0.1f, m_constellationBounds.getWidth());
    const auto middle = m_constellationBounds.getCentre();
    m_constellationHaze = juce::Image(juce::Image::ARGB, juce::jmax(1, juce::roundToInt(area.getWidth() * pixelScale)),
                                      juce::jmax(1, juce::roundToInt(area.getHeight() * pixelScale)), true);
    juce::Graphics g(m_constellationHaze);
    const auto toImage = juce::AffineTransform::translation(-middle.x, -middle.y).scaled(unit, -unit)
                             .translated(0.5f * area.getWidth(), 0.5f * area.getHeight()).scaled(pixelScale);
    const auto grenade = m_logo->createOutlinePath(false), cherry = m_logo->createOutlinePath(true);

    // What the cherry hides: itself and a keyline round it.
    juce::Image hidden(juce::Image::SingleChannel, m_constellationHaze.getWidth(), m_constellationHaze.getHeight(), true);
    {
        juce::Graphics mask(hidden);
        mask.setColour(juce::Colours::white);
        mask.fillPath(cherry, toImage);
        mask.strokePath(cherry, juce::PathStrokeType(2.0f * c_keyline / juce::jmax(0.1f, unit)), toImage);
    }

    g.setColour(juce::Colour(0xffa9c6dc).withAlpha(c_glowGrenade));
    g.fillPath(grenade, toImage);
    {
        juce::Image::BitmapData haze(m_constellationHaze, juce::Image::BitmapData::readWrite);
        const juce::Image::BitmapData cut(hidden, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < haze.height; ++y)
            for (int x = 0; x < haze.width; ++x)
                if (const auto covered = *cut.getPixelPointer(x, y))
                    reinterpret_cast<juce::PixelARGB*>(haze.getPixelPointer(x, y))->multiplyAlpha(255 - static_cast<int>(covered));
    }
    g.setColour(juce::Colour(0xffe8455c).withAlpha(c_glowCherry));
    g.fillPath(cherry, toImage);
}

juce::Rectangle<float> SeaVisualiser::clearOf(juce::Rectangle<float> area, const juce::Rectangle<float>* keepOut, int numKeepOut,
                                              const juce::Rectangle<float>* placed, int numPlaced)
{
    for (int pass = 0; pass < 6; ++pass)
    {
        bool moved = false;
        for (int k = 0; k < numKeepOut; ++k)
            if (! keepOut[k].isEmpty() && area.expanded(3.0f, 2.0f).intersects(keepOut[k]))
            {
                area = area.withY(keepOut[k].getBottom() + 3.0f);
                moved = true;
            }
        for (int k = 0; k < numPlaced; ++k)
            if (! placed[k].isEmpty() && area.intersects(placed[k]))
            {
                area = area.withY(placed[k].getBottom() + 2.0f);
                moved = true;
            }
        if (! moved)
            break;
    }
    return area;
}

juce::Rectangle<float> SeaVisualiser::constellationArea() const
{
    // Open sky: right of the moon, left of whatever the window puts in the sea's top right, above the horizon.
    // Taken from the sea's shape, not from the last frame, so it can be asked for before anything is painted.
    // It is as tall as the sky lets it be: the mark has to be big enough to be known for what it is.
    const float horizon = static_cast<float>(getHeight()) * 0.30f; // (the sky is as tall with the room strip under the water)
    const float top = c_frame + 4.0f, bottom = horizon - 7.0f;   // under the frame, over the water
    const float shapeW = juce::jmax(0.1f, m_constellationBounds.getWidth()), shapeH = juce::jmax(0.1f, m_constellationBounds.getHeight());
    const float unit = juce::jmax(40.0f, juce::jmin((bottom - top) / shapeH, static_cast<float>(getWidth()) * 0.28f / shapeW));
    return juce::Rectangle<float>(unit * shapeW, unit * shapeH)
        .withCentre({ static_cast<float>(getWidth()) * 0.57f, 0.5f * (top + bottom) });
}

void SeaVisualiser::paintConstellation(juce::Graphics& g, float w)
{
    if (m_constellationOpacity < 0.01f || m_constellation.empty())
        return;

    const auto area = constellationArea();
    const juce::Point<float> centre = area.getCentre(), middle = m_constellationBounds.getCentre();
    const float unit = area.getWidth() / juce::jmax(0.1f, m_constellationBounds.getWidth());
    juce::ignoreUnused(w);
    auto place = [&](juce::Point<float> p) { return juce::Point<float>(centre.x + (p.x - middle.x) * unit, centre.y - (p.y - middle.y) * unit); };

    const float alpha = juce::jlimit(0.0f, 1.0f, m_constellationOpacity);
    const float t = m_constellationTime;
    // The grenade is cold starlight, the cherry is rosy: the two colours of the mark, as far as a night sky has them.
    const juce::Colour colours[2] = { juce::Colour(0xffdbe9f6), juce::Colour(0xffff8a86) };

    // The haze first: the two shapes themselves, barely there, so the stars read as a grenade and a cherry and
    // not as rings of wire. It is a picture made when the sky is (filling the shapes every frame would cost).
    if (m_constellationHaze.isValid())
    {
        g.setOpacity(alpha);
        g.drawImage(m_constellationHaze, area.expanded(c_hazeMargin));
    }

    // The outlines are the strong lines and the ring the strongest; the cap and stems are quieter, the lever and
    // the cherry's dimple quieter still.
    static constexpr float lineAlpha[4] = { 0.20f, 0.27f, 0.38f, 0.46f }, lineWidth[4] = { 0.7f, 0.85f, 1.0f, 1.1f };
    for (const auto& line : m_constellationLines)
    {
        const auto& from = m_constellation[static_cast<size_t>(line.first)];
        const auto& to = m_constellation[static_cast<size_t>(line.second)];
        const auto tier = static_cast<size_t>(juce::jlimit(0, 3, line.tier));
        g.setColour(colours[from.cherry && to.cherry ? 1 : 0].withAlpha(lineAlpha[tier] * alpha));
        g.drawLine({ place(from.at), place(to.at) }, lineWidth[tier]);
    }
    for (const auto& s : m_constellation)
    {
        const float twinkle = 0.55f + 0.45f * std::sin(t * 2.1f + s.phase);
        const auto p = place(s.at);
        const auto colour = colours[s.cherry ? 1 : 0];
        const float lit = 0.6f + 0.4f * s.weight;
        g.setColour(colour.withAlpha(0.22f * alpha * twinkle * lit));
        g.fillEllipse(juce::Rectangle<float>(s.size * 3.0f, s.size * 3.0f).withCentre(p));
        g.setColour(colour.withAlpha(juce::jmin(1.0f, 0.55f + 0.35f * twinkle) * alpha * lit));
        g.fillEllipse(juce::Rectangle<float>(s.size, s.size).withCentre(p));
    }
}

//==============================================================================
void SeaVisualiser::buildSky(float pixelScale)
{
    buildConstellationHaze(pixelScale);

    const auto bounds = getLocalBounds().toFloat();
    const int iw = juce::jmax(1, juce::roundToInt(bounds.getWidth() * pixelScale));
    const int ih = juce::jmax(1, juce::roundToInt(bounds.getHeight() * pixelScale));
    m_sky = juce::Image(juce::Image::RGB, iw, ih, false);
    m_skyScale = pixelScale;

    juce::Graphics g(m_sky);
    g.addTransform(juce::AffineTransform::scale(pixelScale));
    juce::ColourGradient sky(juce::Colour(0xff061219), 0.0f, 0.0f, juce::Colour(0xff10303c), 0.0f, m_horizon + 30.0f, false);
    g.setGradientFill(sky);
    g.fillRect(bounds);

    // The still stars. The twinkling few are painted live, over this.
    juce::Random r(7);
    for (int i = 0; i < 110; ++i)
    {
        const float x = r.nextFloat() * bounds.getWidth();
        const float y = r.nextFloat() * r.nextFloat() * m_horizon;
        const float size = 0.6f + 1.1f * r.nextFloat();
        g.setColour(Theme::Colours::text.withAlpha(0.10f + 0.22f * r.nextFloat()));
        g.fillEllipse(x, y, size, size);
    }
}

//==============================================================================
float SeaVisualiser::rowHeight(int k, int band) const
{
    // Line k, between two steps: its own row, with the row arriving from nearer the camera blended in as the
    // step goes by. The nearest line's arriving row is the sound as it is now.
    const auto b = static_cast<size_t>(band);
    const float own = m_rows[static_cast<size_t>((m_rowHead - k + c_rows) % c_rows)][b];
    const float arriving = k > 0 ? m_rows[static_cast<size_t>((m_rowHead - k + 1 + c_rows) % c_rows)][b] : m_smooth[b] + m_wake[b];
    return own + (arriving - own) * m_rowPhase;
}

void SeaVisualiser::paintWater(juce::Graphics& g, float h)
{
    juce::Path line, fill;
    std::array<juce::Point<float>, c_bands> p {};

    for (int k = c_rows - 1; k >= 0; --k)
    {
        // The lines of the sea stay where they are: only the waves travel along them. (Lines that slide away to
        // the horizon all the time make the whole picture rush backwards, which is hard to look at.) Between two
        // steps a line shows a blend of its own row and the one arriving from nearer the camera, so the waves
        // still roll away smoothly instead of jumping once per step.
        const float depth = static_cast<float>(k);
        const float z = m_zNear + depth * m_zStep;
        const float nearness = juce::jlimit(0.0f, 1.0f, 1.0f - depth / static_cast<float>(c_rows));
        const float t = static_cast<float>(m_time);

        const float zNext = juce::jmax(m_zNear, z - m_zStep);
        const float fillBottom = juce::jmin(h, project({ 0.0f, -0.05f, zNext }).y + 2.0f);
        const float nearRow = depth < 1.0f ? h : fillBottom;

        float loudest = 0.0f;
        // A sub lifts the whole swell: every row rides higher and rolls deeper with Stowaway's level.
        // (A quiet sub still shows: the lift goes by the root of its level.)
        const float sub = std::sqrt(m_stowShown[ParamIDs::StowSub]);
        const float lift = c_subLift * sub;
        const float swellGain = 1.0f + 1.8f * m_swell + 1.6f * sub;
        for (int b = 0; b < c_bands; ++b)
        {
            const float u = static_cast<float>(b) / (c_bands - 1);
            const float x = (u * 2.0f - 1.0f) * c_halfWidth;
            const float swell = swellGain * (0.022f * std::sin(x * 2.6f + t * 1.4f + depth * 0.55f)
                                           + 0.012f * std::sin(x * 5.3f - t * 0.9f + depth * 0.9f));
            const float height = rowHeight(k, b);
            loudest = juce::jmax(loudest, height);
            p[static_cast<size_t>(b)] = project({ x, height * 0.42f + swell + lift, z });
        }

        // The crest is one smooth curve through the row rather than eighty straight segments. The shape that
        // hides the rows behind is never seen, so it stays straight: curves cost, and this one would not show.
        line.clear();
        fill.clear();
        line.startNewSubPath(p[0]);
        fill.startNewSubPath(p[0].x, nearRow);
        fill.lineTo(p[0]);
        // Smooth where it shows: the near rows are drawn as curves, the far ones (a couple of pixels tall,
        // where no curve would read) as straight segments.
        if (depth < 8.0f)
            for (int b = 1; b < c_bands - 1; ++b)
            {
                line.quadraticTo(p[static_cast<size_t>(b)], (p[static_cast<size_t>(b)] + p[static_cast<size_t>(b + 1)]) * 0.5f);
                fill.lineTo(p[static_cast<size_t>(b)]);
            }
        else
            for (int b = 1; b < c_bands - 1; ++b)
            {
                line.lineTo(p[static_cast<size_t>(b)]);
                fill.lineTo(p[static_cast<size_t>(b)]);
            }
        line.lineTo(p[c_bands - 1]);
        fill.lineTo(p[c_bands - 1]);
        fill.lineTo(p[c_bands - 1].x, nearRow);
        fill.closeSubPath();

        // Each row fills over the rows behind it, so near waves hide far ones; both fill and line take their
        // colour from how far out the row is, so the sea has depth and the horizon fogs away.
        g.setColour(c_hollow.interpolatedWith(c_fog, (1.0f - nearness) * 0.9f).interpolatedWith(Theme::Colours::stowaway, 0.30f * sub * nearness));
        g.fillPath(fill);
        // (and the near crests take Stowaway's colour while it does, so it is plain what is lifting them)
        g.setColour(waterColour(nearness, juce::jmin(1.0f, loudest)).interpolatedWith(Theme::Colours::stowaway, 0.8f * sub * nearness));
        g.strokePath(line, juce::PathStrokeType(depth < 1.0f ? 1.7f : 1.1f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    }
}

//==============================================================================
void SeaVisualiser::refreshBusLabels()
{
    // The names over the comets are made here, in the step, and not in paint: building them is string work, and
    // paint does none. They only change when a track moves bus or is renamed, so they are looked at four times a
    // second and rebuilt only when what they are made of has changed.
    if (m_busLabelsAt >= 0.0 && m_time - m_busLabelsAt < 0.25)
        return;
    m_busLabelsAt = m_time;

    auto& cargo = m_processor.getCargo();
    for (int b = 0; b < RigEngine::c_maxBuses; ++b)
    {
        int count = 0, only = -1;
        for (int t = 0; t < CargoHold::c_numTracks; ++t)
            if (cargo.get(t) != nullptr && cargo.getBus(t) == b)
            {
                ++count;
                only = t;
            }

        const auto name = count == 1 ? cargo.getDisplayName(only) : juce::String();
        const auto want = juce::String("Bus ") + (b == 0 ? "A" : "B")
                          + (name.isNotEmpty() ? (" " + juce::String::charToString(0x00b7) + " ") + name : juce::String());
        m_busLabels[static_cast<size_t>(b)] = want;
    }
    m_busLabels[RigEngine::c_maxBuses] = "Throw";
    {
        int count = 0, only = -1;
        for (int t = 0; t < CargoHold::c_numTracks; ++t)
            if (cargo.get(t) != nullptr && cargo.getBus(t) == CargoHold::c_stowBus)
            {
                ++count;
                only = t;
            }
        const auto name = count == 1 ? cargo.getDisplayName(only) : juce::String();
        m_busLabels[c_stowLabel] = juce::String("Stowaway")
                                   + (name.isNotEmpty() ? (" " + juce::String::charToString(0x00b7) + " ") + name : juce::String());
    }
}

void SeaVisualiser::paintStowaway(juce::Graphics& g, float h)
{
    namespace TC = Theme::Colours;
    const auto colour = TC::stowaway;
    m_stowLabelAlpha = 0.0f;

    // Sub: the swell itself is lifted (see paintWater). Its name sits low over the near water it lifts.
    // (and the body of the water takes its colour). Its name sits low, on the near water it lifts.
    if (const float sub = m_stowShown[ParamIDs::StowSub]; sub > 0.01f)
    {
        m_stowLabelAt = { m_cx, h - 30.0f };
        m_stowLabelAlpha = juce::jlimit(0.0f, 1.0f, (sub - 0.03f) * 6.0f);
    }

    // Wake: a trail on the water where bus A's comet has been, widest and brightest just behind it.
    if (m_wakeTrail.size() >= 2)
    {
        const float count = static_cast<float>(m_wakeTrail.size());
        for (size_t j = 1; j < m_wakeTrail.size(); ++j)
        {
            const auto& a = m_wakeTrail[j - 1];
            const auto& b = m_wakeTrail[j];
            const float age = static_cast<float>(j) / count; // 1 at the comet, 0 at the trail's far end
            const float strength = juce::jmin(1.0f, 1.6f * b.strength);
            const auto p0 = project({ a.x, 0.02f, a.z }), p1 = project({ b.x, 0.02f, b.z });
            const float width = (3.0f + 16.0f * strength) * (0.35f + 0.65f * age) * (m_focal / (b.z * 260.0f));
            g.setColour(colour.withAlpha(juce::jmin(1.0f, (0.18f + 0.55f * strength) * age)));
            g.drawLine({ p0, p1 }, width);
            // Foam along it: a short bright fleck every few points.
            if (j % 4 == 0)
            {
                g.setColour(juce::Colour(0xfffff1d6).interpolatedWith(colour, 0.5f).withAlpha(0.6f * strength * age));
                g.fillRect(juce::Rectangle<float>(width * 0.9f + 2.0f, 1.4f).withCentre(p1));
            }
        }
        const auto& last = m_wakeTrail.back();
        const auto at = project({ last.x, 0.02f, last.z });
        m_stowLabelAt = { at.x, at.y + 14.0f };
        m_stowLabelAlpha = juce::jlimit(0.0f, 1.0f, (m_stowShown[ParamIDs::StowWake] - 0.03f) * 6.0f);
    }

    // Dry does not move: a lantern floating in the middle of the water, glowing with its level.
    if (const float dry = m_stowShown[ParamIDs::StowDry]; dry > 0.01f)
    {
        const float z = 1.7f, t = static_cast<float>(m_time);
        const float bob = 0.012f * std::sin(t * 1.4f);
        const auto base = project({ 0.0f, 0.03f + bob, z });
        const float unit = m_focal / (z * 260.0f); // a pixel-ish at this distance
        const float glow = (26.0f + 70.0f * dry) * unit;
        juce::ColourGradient light(colour.withAlpha(0.20f + 0.50f * dry), base.x, base.y - 9.0f * unit,
                                   colour.withAlpha(0.0f), base.x + glow, base.y - 9.0f * unit, true);
        g.setGradientFill(light);
        g.fillEllipse(juce::Rectangle<float>(glow * 2.0f, glow * 2.0f).withCentre({ base.x, base.y - 9.0f * unit }));
        // Its light on the water under it.
        g.setColour(colour.withAlpha(0.10f + 0.30f * dry));
        g.fillEllipse(juce::Rectangle<float>(glow * 1.8f, glow * 0.22f).withCentre({ base.x, base.y + 3.0f * unit }));
        // The lantern: a float, a square glass and a cap, hard-edged.
        const float s = juce::jmax(1.0f, unit);
        g.setColour(TC::edgeDark);
        g.fillRect(juce::Rectangle<float>(16.0f * s, 3.0f * s).withCentre({ base.x, base.y }));
        g.fillRect(juce::Rectangle<float>(10.0f * s, 3.0f * s).withCentre({ base.x, base.y - 17.0f * s }));
        g.setColour(juce::Colour(0xfffff1d6).interpolatedWith(colour, 0.55f - 0.35f * dry).withAlpha(0.55f + 0.45f * dry));
        g.fillRect(juce::Rectangle<float>(8.0f * s, 12.0f * s).withCentre({ base.x, base.y - 8.5f * s }));
        g.setColour(TC::edgeDark);
        g.drawRect(juce::Rectangle<float>(8.0f * s, 12.0f * s).withCentre({ base.x, base.y - 8.5f * s }), juce::jmax(1.0f, 0.8f * s));
        m_stowLabelAt = { base.x, base.y - 19.0f * s - glow * 0.35f - 9.0f };
        m_stowLabelAlpha = juce::jlimit(0.0f, 1.0f, (dry - 0.03f) * 6.0f);
    }
}

void SeaVisualiser::paintComets(juce::Graphics& g, float w)
{
    namespace TC = Theme::Colours;

    const auto& busLabels = m_busLabels;

    struct Label { float env = -1.0f, alpha = 0.0f; juce::Point<float> at; };
    std::array<Label, c_labels> labels; // one per bus, on its louder pass, one for the throws, and Stowaway's
    auto comet = [&](const std::deque<P3>& trail, float env, float flash, juce::Colour colour, float bright, float whiteness, size_t group)
    {
        if (trail.size() < 2)
            return;
        // The tail fades as the square of its age, so its first third is below a tone anyone can see: it is not
        // drawn, and the reflection only follows the half of the trail that is bright enough to cast one.
        const size_t firstSeen = trail.size() / 3 < 1 ? 1 : trail.size() / 3;
        for (size_t j = firstSeen; j < trail.size(); ++j)
        {
            const float a = static_cast<float>(j) / static_cast<float>(trail.size());
            const auto p0 = project(trail[j - 1]);
            const auto p1 = project(trail[j]);
            const float width = bright * (1.0f + 7.0f * env) * a * (m_focal / (trail[j].z * 260.0f));
            g.setColour(colour.withAlpha(juce::jmin(1.0f, 0.55f * bright * a * a)));
            g.drawLine({ p0, p1 }, width);

            // Reflection on the water
            if (a > 0.5f)
            {
                const auto r0 = project({ trail[j - 1].x, -trail[j - 1].y * 0.35f, trail[j - 1].z });
                const auto r1 = project({ trail[j].x, -trail[j].y * 0.35f, trail[j].z });
                g.setColour(colour.withAlpha(0.12f * bright * a * a));
                g.drawLine({ r0, r1 }, width * 0.7f);
            }
        }

        const auto head = trail.back();
        const auto p = project(head);
        const float radius = (0.05f + 0.11f * env) * m_focal / head.z;
        juce::ColourGradient glow(colour.withAlpha(0.9f), p.x, p.y, colour.withAlpha(0.0f), p.x + radius * 2.2f, p.y, true);
        g.setGradientFill(glow);
        g.fillEllipse(juce::Rectangle<float>(radius * 4.4f, radius * 4.4f).withCentre(p));
        g.setColour(juce::Colour(0xfffff1d6).interpolatedWith(colour, whiteness));
        g.fillEllipse(juce::Rectangle<float>(radius * 0.9f, radius * 0.9f).withCentre(p));

        // Its label goes just above the glow (drawn below, once per group, on its louder pass).
        if (env > labels[group].env)
            labels[group] = { env, juce::jlimit(0.0f, 1.0f, (env - 0.04f) * 6.0f), { p.x, p.y - radius * 2.3f - 9.0f } };

        if (flash > 0.02f)
        {
            const float ring = (1.0f - flash) * m_focal * 0.5f / head.z;
            g.setColour(colour.withAlpha(0.5f * flash));
            g.drawEllipse(juce::Rectangle<float>(ring * 2.0f, ring * 0.7f).withCentre(project({ head.x, 0.0f, head.z })), 1.5f);
        }
    };
    for (size_t i = 0; i < m_trails.size(); ++i)
    {
        // Two passes per bus, then the thrown whooshes: a throw is the same comet in brass, and brighter.
        const size_t group = static_cast<size_t>(cometGroup(static_cast<int>(i)));
        const bool thrown = static_cast<int>(group) == RigEngine::c_maxBuses;
        comet(m_trails[i], m_flightEnv[i], m_flash[i], thrown ? TC::brass : (group == 0 ? TC::busA : TC::busB), thrown ? 1.3f : 1.0f,
              thrown ? 0.45f : 0.35f, group);
    }
    // Stowaway in Fly: a third comet, in its own colour.
    for (size_t i = 0; i < m_stowTrails.size(); ++i)
        comet(m_stowTrails[i], m_stowEnv[i], m_stowFlash[i], TC::stowaway, 1.0f, 0.35f, static_cast<size_t>(c_stowLabel));
    // In its other modes its name goes where it shows (see paintStowaway).
    if (labels[c_stowLabel].env < 0.0f && m_stowLabelAlpha > 0.01f)
        labels[c_stowLabel] = { 0.0f, m_stowLabelAlpha, m_stowLabelAt };

    // The labels, fading in and out with the passes: kept apart from each other, and off the readouts and the
    // buttons the window puts over the sea.
    if (! m_labelsShown)
        return;

    const auto labelFont = m_fonts.caption();
    std::array<juce::Rectangle<float>, c_labels> placed;
    size_t numPlaced = 0;
    for (size_t b = 0; b < labels.size(); ++b)
    {
        const auto& label = labels[b];
        if (label.alpha <= 0.01f)
            continue;
        const auto& text = busLabels[b];
        const float textW = juce::jmin(w - 16.0f, Theme::textWidth(labelFont, text) + 4.0f);
        auto area = juce::Rectangle<float>(textW, 14.0f).withCentre(label.at);
        area = area.withX(juce::jlimit(8.0f, juce::jmax(8.0f, w - textW - 8.0f), area.getX())).withY(juce::jmax(8.0f, area.getY()));

        // Down out of the way of anything it lands on, and then of anything already placed.
        const juce::Rectangle<float> keepOut[] = { m_titleBox, m_recBox, m_reserved, m_roomShown > 0.01f ? roomPlanArea() : juce::Rectangle<float>(),
                                                   m_restNote.isNotEmpty() && m_roomShown < 0.99f ? restNoteArea() : juce::Rectangle<float>() };
        area = clearOf(area, keepOut, 5, placed.data(), static_cast<int>(numPlaced));
        if (numPlaced < placed.size())
            placed[numPlaced++] = area;

        const juce::Colour colour = b == 0 ? TC::busA : (b == 1 ? TC::busB : (static_cast<int>(b) == c_stowLabel ? TC::stowaway : TC::brass));
        // A dark plate behind it, so it reads over the stars and the waves.
        g.setColour(TC::well.withAlpha(0.72f * label.alpha));
        Theme::Plate::fill(g, area.expanded(4.0f, 0.0f), Theme::Cut::small);
        g.setColour(colour.brighter(0.15f).withAlpha(label.alpha));
        Theme::fitText(g, text, area, labelFont, juce::Justification::centred, Theme::Fit::Name);
    }
}

//==============================================================================
void SeaVisualiser::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto scene = bounds.withTrimmedBottom(static_cast<float>(m_foot)); // the water and the sky, over the frame's foot
    const float w = scene.getWidth(), h = scene.getHeight();
    m_focal = juce::jmax(w * 0.36f, h * 0.42f);
    m_horizon = bounds.getHeight() * 0.30f; // the sky keeps its height over the room strip: the water gives it the room
    m_cx = w * 0.5f;
    // The nearest wave row lands on the bottom edge, so the sea fills the whole panel at any shape.
    m_zNear = juce::jlimit(0.25f, 1.2f, c_camHeight * m_focal / juce::jmax(1.0f, h * 1.02f - m_horizon));
    m_zStep = (6.2f - m_zNear) / static_cast<float>(c_rows);

    namespace TC = Theme::Colours;

    // The sky: a kept image (its gradient and the still stars), redrawn only when the view changes shape.
    const float pixelScale = juce::jlimit(0.5f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (! m_sky.isValid() || std::abs(pixelScale - m_skyScale) > 0.01f)
        buildSky(pixelScale);
    g.drawImage(m_sky, bounds);

    // The few stars that twinkle.
    for (const auto& s : m_stars)
    {
        const float twinkle = 0.5f + 0.5f * std::sin(static_cast<float>(m_time) * 1.3f + s.phase);
        g.setColour(TC::text.withAlpha(0.12f + 0.35f * twinkle * s.size / 2.0f));
        g.fillEllipse(s.x * w, s.y * m_horizon, s.size, s.size);
    }

    paintConstellation(g, w);

    // Crescent moon, well clear of the Morph button and the readouts. Its halo warms and widens while the
    // limiter is holding the output down.
    const juce::Point<float> moonAt { w * 0.26f, m_horizon * 0.52f };
    {
        const float glow = m_limiterGlow;
        const auto moon = juce::Rectangle<float>(26.0f, 26.0f).withCentre(moonAt);
        const float halo = 20.0f + 26.0f * glow;
        const auto warm = TC::brass.interpolatedWith(juce::Colour(0xffffb169), glow);
        juce::ColourGradient ring(warm.withAlpha(0.14f + 0.20f * glow), moonAt.x, moonAt.y,
                                  warm.withAlpha(0.0f), moonAt.x + 13.0f + halo, moonAt.y, true);
        g.setGradientFill(ring);
        g.fillEllipse(moon.expanded(halo));
        g.setColour(c_moonlight.withAlpha(0.85f));
        g.fillEllipse(moon);
        g.setColour(juce::Colour(0xff0a1f29));
        g.fillEllipse(moon.translated(7.0f, -4.0f));
    }

    paintWater(g, h);

    // The moon's glitter path: flecks of light on the near rows, under the moon.
    {
        const float t = static_cast<float>(m_time);
        constexpr int rows = c_rows;
        for (int k = 0; k < rows; ++k)
        {
            const float depth = static_cast<float>(k);
            const float z = m_zNear + depth * m_zStep;
            const float nearness = juce::jlimit(0.0f, 1.0f, 1.0f - depth / static_cast<float>(rows));
            const float worldX = (moonAt.x - m_cx) * z / m_focal;
            const float u = juce::jlimit(0.0f, 1.0f, (worldX / c_halfWidth + 1.0f) * 0.5f);
            const int band = juce::jlimit(0, c_bands - 1, juce::roundToInt(u * static_cast<float>(c_bands - 1)));
            const float height = rowHeight(k, band);
            // A few flecks on each row, scattered across a path that widens towards the camera: the offsets are
            // in world units scaled by depth, so a fleck is the same few pixels wherever it is. They are lights
            // on the water, so they brighten quickly with nearness and sit on the lit crests: far out they fade
            // to nothing rather than littering the horizon with specks.
            const int flecks = 2 + juce::roundToInt(3.0f * nearness);
            for (int f = 0; f < flecks; ++f)
            {
                const float seed = static_cast<float>(k) * 1.7f + static_cast<float>(f) * 4.3f;
                const float side = std::sin(seed * 2.11f) * (0.04f + 0.20f * nearness);
                const float sway = 0.035f * std::sin(t * 1.9f + seed);
                const auto at = project({ worldX + (side + sway) * z, height * 0.42f + c_subLift * std::sqrt(m_stowShown[ParamIDs::StowSub]), z });
                const float len = 2.0f + 10.0f * nearness * (0.6f + 0.4f * std::abs(std::sin(seed)));
                const float shine = 0.35f + 0.65f * std::abs(std::sin(t * 2.7f + seed * 1.3f));
                const float lit = nearness * std::sqrt(nearness); // the far ones fade out rather than litter the horizon
                g.setColour(c_moonlight.withAlpha(0.58f * lit * shine * (0.6f + 0.4f * juce::jmin(1.0f, height))));
                g.fillRect(at.x - len * 0.5f, at.y - 0.7f, len, 1.4f);
            }
        }
    }

    // Hits: a flash along the horizon, and a ring widening on the water.
    if (m_hitFlash > 0.01f)
    {
        // A short glow sitting on the horizon, brightest in the middle of it.
        const float band = 14.0f + 12.0f * m_hitFlash;
        juce::ColourGradient flash(TC::hits.withAlpha(0.42f * m_hitFlash), m_cx, m_horizon,
                                   TC::hits.withAlpha(0.0f), m_cx + w * 0.42f, m_horizon, true);
        g.setGradientFill(flash);
        g.fillRect(juce::Rectangle<float>(0.0f, m_horizon - band, w, band * 1.6f));
    }
    if (m_hitRing > 0.01f)
    {
        // Nearer than the horizon and bright enough to read against the water: it is the hit's own mark.
        const float z = 1.9f;
        const float rx = (1.0f - m_hitRing) * 2.2f * m_focal / z;
        g.setColour(TC::hits.withAlpha(0.50f * m_hitRing));
        g.drawEllipse(juce::Rectangle<float>(rx * 2.0f, rx * 0.42f).withCentre(project({ 0.0f, 0.0f, z })), 1.6f);
    }

    // A ripple where the sea was clicked.
    for (const auto& r : m_ripples)
    {
        if (r.age >= 1.0f)
            continue;
        const auto centre = project({ r.x, 0.0f, r.z });
        const float rx = r.age * 0.9f * m_focal / r.z;
        g.setColour(c_moonlight.withAlpha(0.32f * (1.0f - r.age)));
        g.drawEllipse(juce::Rectangle<float>(rx * 2.0f, rx * 0.44f).withCentre(centre), 1.1f);
        if (r.age > 0.3f)
        {
            const float inner = (r.age - 0.3f) * 0.9f * m_focal / r.z;
            g.setColour(c_moonlight.withAlpha(0.18f * (1.0f - r.age)));
            g.drawEllipse(juce::Rectangle<float>(inner * 2.0f, inner * 0.44f).withCentre(centre), 1.0f);
        }
    }

    paintStowaway(g, h);
    paintComets(g, w);
    paintRoom(g);
    paintRestNote(g);

    // The panel's name, and the recording lamp. The time itself is on the tape's own button in the header, so
    // there is one of it in the window.
    m_titleBox = { 20.0f, 8.0f, 72.0f, 16.0f };
    g.setColour(TC::textDim);
    Theme::fitText(g, "The Sea", m_titleBox, m_fonts.control(), juce::Justification::centredLeft);

    m_recBox = {};
    if (m_processor.isRecording())
    {
        const bool blink = std::fmod(m_time, 1.0) < 0.6;
        m_recBox = { 98.0f, 8.0f, 36.0f, 16.0f };
        g.setColour(TC::record.withAlpha(blink ? 1.0f : 0.35f));
        g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre({ 102.0f, 16.0f }));
        g.setColour(TC::text);
        Theme::fitText(g, "Rec", m_recBox.withTrimmedLeft(12.0f), m_fonts.control(), juce::Justification::centredLeft);
        m_recBox = m_recBox.withX(94.0f).withWidth(44.0f);
    }

    // Nothing loaded: a calm sea and one line saying what to do.
    {
        auto& cargo = m_processor.getCargo();
        bool any = false;
        for (int t = 0; t < CargoHold::c_numTracks && ! any; ++t)
            any = cargo.get(t) != nullptr;
        if (! any)
        {
            const auto hint = juce::Rectangle<float>(w * 0.5f - 170.0f, m_horizon + 22.0f, 340.0f, 18.0f);
            g.setColour(TC::well.withAlpha(0.55f));
            Theme::Plate::fill(g, hint.expanded(6.0f, 1.0f), Theme::Cut::small);
            g.setColour(TC::textDim);
            Theme::fitText(g, "Drop a sound into the tracks and the sea comes alive.", hint, m_fonts.control(),
                           juce::Justification::centred, Theme::Fit::Sentence);
        }
    }

    // The morph pad is docked over a corner of the sea: everything behind it is dimmed so the pad reads, and the
    // sky, the moon and the comets stay in view through it.
    if (! m_dock.isEmpty())
    {
        g.setColour(TC::window.withAlpha(0.55f));
        g.fillRect(scene);
    }

    // The frame: the sea is a rectangular window in an iron frame, a gusset plate with a bolt in each corner.
    // Painted over the opaque sea (cheap to repaint): the band, its two edges, the gussets and their bolts.
    g.setColour(TC::raised);
    g.fillPath(m_ironFrame);
    g.setColour(TC::edgeDark);
    g.strokePath(m_frameInner, juce::PathStrokeType(1.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    g.drawRect(bounds, 1.0f);
    g.setColour(TC::edgeLight);
    g.drawRect(bounds.reduced(1.0f), 1.0f);
    const float in = c_gusset * 0.36f;
    for (const auto& c : { bounds.getTopLeft().translated(in, in), bounds.getTopRight().translated(-in, in),
                           bounds.getBottomLeft().translated(in, -in), bounds.getBottomRight().translated(-in, -in) })
        Theme::Plate::drawBolt(g, c);
}

void SeaVisualiser::setFootHeight(int height)
{
    if (height == m_foot)
        return;
    m_foot = juce::jmax(0, height);
    resized();
    repaint();
}

void SeaVisualiser::resized()
{
    const auto bounds = getLocalBounds().toFloat();
    m_sky = juce::Image();
    // The window the sea shows through: the frame's inner edge, its corners cut off by the gussets. Under it, the
    // frame's foot (the room strip) when there is one.
    const float band = c_frame, cut = c_gusset - band;
    const auto window = bounds.reduced(band).withTrimmedBottom(static_cast<float>(m_foot));
    m_frameInner.clear();
    m_frameInner.startNewSubPath(window.getX() + cut, window.getY());
    m_frameInner.lineTo(window.getRight() - cut, window.getY());
    m_frameInner.lineTo(window.getRight(), window.getY() + cut);
    m_frameInner.lineTo(window.getRight(), window.getBottom() - cut);
    m_frameInner.lineTo(window.getRight() - cut, window.getBottom());
    m_frameInner.lineTo(window.getX() + cut, window.getBottom());
    m_frameInner.lineTo(window.getX(), window.getBottom() - cut);
    m_frameInner.lineTo(window.getX(), window.getY() + cut);
    m_frameInner.closeSubPath();
    m_ironFrame.clear();
    m_ironFrame.addRectangle(bounds);
    m_ironFrame.addPath(m_frameInner);
    m_ironFrame.setUsingNonZeroWinding(false);
}

//==============================================================================
// The room: a plan of it in the water's lower left corner.
juce::Rectangle<float> SeaVisualiser::roomPlanArea() const
{
    // Square, in the corner of the water clear of the frame's gusset, and well under the horizon.
    const float h = static_cast<float>(getHeight() - m_foot);
    const float side = juce::jlimit(80.0f, 124.0f, h * 0.38f);
    return { c_frame + 11.0f, h - c_frame - 9.0f - side, side, side };
}

void SeaVisualiser::setRestNote(const juce::String& note)
{
    if (note == m_restNote)
        return;
    m_restNote = note;
    repaint();
}

juce::Rectangle<float> SeaVisualiser::restNoteArea() const
{
    // In the plan's corner, as wide as three lines need and clear of the morph pad's dock at the lower right.
    const float h = static_cast<float>(getHeight() - m_foot);
    constexpr float noteW = 236.0f, noteH = 50.0f;
    return { c_frame + 11.0f, h - c_frame - 9.0f - noteH, noteW, noteH };
}

void SeaVisualiser::paintRestNote(juce::Graphics& g)
{
    // A quiet plate in the water, fading out as the plan of a room fades in: its first line, then the rest.
    const float alpha = 1.0f - m_roomShown;
    if (m_restNote.isEmpty() || alpha < 0.01f)
        return;
    namespace TC = Theme::Colours;
    const auto plate = restNoteArea();
    g.setColour(TC::well.withAlpha(0.78f * alpha));
    Theme::Plate::fill(g, plate, Theme::Cut::control);
    g.setColour(TC::edgeDark.withAlpha(alpha));
    Theme::Plate::stroke(g, plate, Theme::Cut::control);
    auto inner = plate.reduced(8.0f, 5.0f);
    const auto head = m_restNote.upToFirstOccurrenceOf("\n", false, false);
    const auto rest = m_restNote.fromFirstOccurrenceOf("\n", false, false);
    g.setColour(TC::textDim.withAlpha(alpha));
    Theme::fitText(g, head, inner.removeFromTop(14.0f), m_fonts.caption(), juce::Justification::centredLeft, Theme::Fit::Sentence);
    Theme::fitLines(g, rest, inner.toNearestInt(), m_fonts.caption(), TC::textFaint.withAlpha(alpha));
}

juce::String SeaVisualiser::getTooltip()
{
    // The plan of the room explains itself when the mouse is over it.
    if (m_roomShown > 0.5f && roomPlanArea().contains(getMouseXYRelative().toFloat()))
        return "The Room From Above\nThe front of the room is at the top and you are the hull in the middle. Each speaker is a lamp on "
               "the walls (the ones overhead on the inner square, the LFE the square under the room), lit as it plays. Each sound is a dot: "
               "the buses, throws, hits and Stowaway in their colours, a higher one drawn nearer the middle. The speaker meters in "
               "Output give the same levels in numbers.";
    return juce::SettableTooltipClient::getTooltip();
}

juce::Point<float> SeaVisualiser::planPoint(juce::Rectangle<float> roomBox, float x, float y, float z)
{
    // Looking down on the room: the front wall at the top. A sound higher up is drawn nearer the middle, as far in
    // as the tops at the ceiling, so the tops are an inner square and height reads at a glance.
    const float in = 1.0f - c_planHeightIn * juce::jlimit(0.0f, 1.0f, z);
    const auto c = roomBox.getCentre();
    return { c.x + 0.5f * roomBox.getWidth() * juce::jlimit(-1.0f, 1.0f, x) * in,
             c.y - 0.5f * roomBox.getHeight() * juce::jlimit(-1.0f, 1.0f, y) * in };
}

namespace
{
    /** The room's box inside the plan's plate: room round it for the lamps on the walls and, under it, the LFE's. */
    juce::Rectangle<float> planRoomBox(juce::Rectangle<float> plate)
    {
        return plate.withTrimmedLeft(15.0f).withTrimmedRight(15.0f).withTrimmedTop(13.0f).withTrimmedBottom(21.0f);
    }
}

juce::Point<float> SeaVisualiser::planDot(int object, float* brightness) const
{
    const auto& dot = m_dots[static_cast<size_t>(juce::jlimit(0, RigEngine::c_objects - 1, object))];
    if (brightness != nullptr)
        *brightness = dot.count > 0 ? dot.level * m_roomShown : 0.0f;
    if (dot.count == 0)
        return {};
    const auto& p = dot.trail[static_cast<size_t>(dot.head)];
    return planPoint(planRoomBox(roomPlanArea()), p.x, p.y, p.z);
}

void SeaVisualiser::stepRoom(float dt)
{
    // The plan shows in a room and fades away at Mono and Stereo (keeping the last room's shape while it goes).
    const auto width = m_processor.getRoomWidth();
    m_inRoom = width >= room::Width::Quad;
    if (m_inRoom)
        m_planWidth = width;
    m_roomShown += ((m_inRoom ? 1.0f : 0.0f) - m_roomShown) * juce::jmin(1.0f, 6.0f * dt);
    if (std::abs(m_roomShown - (m_inRoom ? 1.0f : 0.0f)) < 0.002f)
        m_roomShown = m_inRoom ? 1.0f : 0.0f;

    // Each speaker's lamp: up at once with its level, down over about a third of a second (the meter itself falls
    // back as the OUT meter does), from dark at -40 dB to full at 0 dBFS, on a curve, so the loud speakers stand
    // out from the ones a sound only touches. No flicker: it never jumps down.
    const int channels = room::layout(m_planWidth).numChannels;
    for (int ch = 0; ch < room::c_maxChannels; ++ch)
    {
        auto& lamp = m_lamps[static_cast<size_t>(ch)];
        const float db = juce::Decibels::gainToDecibels(ch < channels ? m_processor.getChannelPeak(ch) : 0.0f, -100.0f);
        const float t = juce::jlimit(0.0f, 1.0f, (db + 40.0f) / 40.0f);
        const float want = m_inRoom ? t * t : 0.0f;
        lamp = want > lamp ? want : lamp + (want - lamp) * juce::jmin(1.0f, 7.0f * dt);
        if (lamp < 1.0e-3f)
            lamp = 0.0f;
    }

    // The objects: where each is, with a short trail, and a level that eases in and out.
    const auto& engine = m_processor.getEngine();
    for (int i = 0; i < RigEngine::c_objects; ++i)
    {
        auto& dot = m_dots[static_cast<size_t>(i)];
        const auto view = engine.getObject(i);
        const bool on = m_inRoom && view.active;
        dot.level += ((on ? juce::jlimit(0.0f, 1.0f, view.level) : 0.0f) - dot.level) * juce::jmin(1.0f, (on ? 12.0f : 4.0f) * dt);
        if (on)
        {
            dot.head = (dot.head + 1) % c_dotTrail;
            dot.trail[static_cast<size_t>(dot.head)] = { view.x, view.y, view.z };
            dot.count = juce::jmin(c_dotTrail, dot.count + 1);
        }
        else if (dot.level < 0.01f)
        {
            dot.level = 0.0f;
            dot.count = 0;
        }
    }
}

void SeaVisualiser::paintRoom(juce::Graphics& g)
{
    if (m_roomShown < 0.01f)
        return;
    namespace TC = Theme::Colours;
    const float alpha = m_roomShown;
    const auto plate = roomPlanArea();
    const auto box = planRoomBox(plate);
    const auto& layout = room::layout(m_planWidth);

    // The plate: a dark well with a pressed edge, so the water shows faintly through it.
    g.setColour(TC::well.withAlpha(0.84f * alpha));
    Theme::Plate::fill(g, plate, Theme::Cut::control);
    g.setColour(TC::edgeDark.withAlpha(alpha));
    Theme::Plate::stroke(g, plate, Theme::Cut::control);
    g.setColour(TC::stroke.withAlpha(0.55f * alpha));
    Theme::Plate::stroke(g, plate.reduced(1.0f), Theme::Cut::control);

    // The room's walls, and the ceiling (an inner square) when it has speakers overhead.
    bool tops = false;
    for (int ch = 0; ch < layout.numChannels; ++ch)
        tops = tops || room::info(layout.speakers[static_cast<size_t>(ch)]).role == room::Role::Height;
    g.setColour(TC::grid.brighter(0.35f).withAlpha(alpha));
    g.drawRect(box, 1.0f);
    if (tops)
    {
        g.setColour(TC::grid.brighter(0.2f).withAlpha(alpha));
        const float in = 1.0f - c_planHeightIn;
        g.drawRect(box.withSizeKeepingCentre(box.getWidth() * in, box.getHeight() * in), 1.0f);
    }

    // The listener: a small hull in the middle, its bow to the front.
    {
        const auto c = box.getCentre();
        juce::Path hull;
        hull.startNewSubPath(c.x, c.y - 6.5f);
        hull.lineTo(c.x + 3.5f, c.y - 1.5f);
        hull.lineTo(c.x + 3.0f, c.y + 5.0f);
        hull.lineTo(c.x - 3.0f, c.y + 5.0f);
        hull.lineTo(c.x - 3.5f, c.y - 1.5f);
        hull.closeSubPath();
        g.setColour(TC::textDim.withAlpha(0.8f * alpha));
        g.fillPath(hull);
    }

    // A lamp at each speaker that glows with its level: round lamps on the walls, smaller on the ceiling, and the
    // LFE a square lamp of its own under the room.
    const juce::Colour lampLight { 0xfff3e3c0 };
    for (int ch = 0; ch < layout.numChannels; ++ch)
    {
        const auto& speaker = room::info(layout.speakers[static_cast<size_t>(ch)]);
        const float level = m_lamps[static_cast<size_t>(ch)];
        if (speaker.role == room::Role::Lfe)
        {
            const auto at = juce::Point<float>(box.getCentreX(), box.getBottom() + 11.0f);
            const auto lamp = juce::Rectangle<float>(8.0f, 8.0f).withCentre(at);
            g.setColour(TC::stowaway.withAlpha((0.10f + 0.35f * level) * alpha));
            g.fillRect(lamp.expanded(1.0f + 3.0f * level));
            g.setColour(TC::edgeDark.withAlpha(alpha));
            g.fillRect(lamp);
            g.setColour(TC::stowaway.withAlpha((0.22f + 0.78f * level) * alpha));
            g.fillRect(lamp.reduced(1.5f));
            continue;
        }
        const bool high = speaker.role == room::Role::Height;
        const auto at = planPoint(box, speaker.x, speaker.y, speaker.z);
        const float r = high ? 3.2f : 4.2f;
        if (level > 0.01f)
        {
            const float halo = r + 1.0f + 6.0f * level;
            juce::ColourGradient glow(lampLight.withAlpha(0.55f * level * alpha), at.x, at.y, lampLight.withAlpha(0.0f), at.x + halo, at.y, true);
            g.setGradientFill(glow);
            g.fillEllipse(juce::Rectangle<float>(halo * 2.0f, halo * 2.0f).withCentre(at));
        }
        g.setColour(TC::edgeDark.withAlpha(alpha));
        g.fillEllipse(juce::Rectangle<float>(r * 2.0f + 2.0f, r * 2.0f + 2.0f).withCentre(at));
        g.setColour(TC::bolt.withAlpha(0.45f * alpha).interpolatedWith(lampLight.withAlpha(alpha), level));
        g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(at));
    }

    // The objects: a dot in its colour where it is, with a short fading trail behind it.
    for (int i = 0; i < RigEngine::c_objects; ++i)
    {
        const auto& dot = m_dots[static_cast<size_t>(i)];
        if (dot.count == 0 || dot.level < 0.01f)
            continue;
        const auto colour = i <= RigEngine::ObjPassA2 ? TC::busA
                          : i <= RigEngine::ObjPassB2 ? TC::busB
                          : i <= RigEngine::ObjThrow4 ? TC::brass
                          : i == RigEngine::ObjHits   ? TC::hits
                                                      : TC::stowaway;
        const float level = dot.level * alpha;
        for (int k = 1; k < dot.count; ++k)
        {
            const auto& a = dot.trail[static_cast<size_t>((dot.head - k + c_dotTrail) % c_dotTrail)];
            const auto& b = dot.trail[static_cast<size_t>((dot.head - k + 1 + c_dotTrail) % c_dotTrail)];
            const float fade = 1.0f - static_cast<float>(k) / static_cast<float>(c_dotTrail);
            g.setColour(colour.withAlpha(0.5f * fade * level));
            g.drawLine({ planPoint(box, a.x, a.y, a.z), planPoint(box, b.x, b.y, b.z) }, 1.4f);
        }
        const auto& p = dot.trail[static_cast<size_t>(dot.head)];
        const auto at = planPoint(box, p.x, p.y, p.z);
        const float r = 2.2f + 1.6f * dot.level;
        g.setColour(TC::edgeDark.withAlpha(0.8f * level));
        g.fillEllipse(juce::Rectangle<float>(r * 2.0f + 2.0f, r * 2.0f + 2.0f).withCentre(at));
        g.setColour(colour.withAlpha(juce::jmin(1.0f, 0.35f + 0.9f * dot.level) * alpha));
        g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(at));
    }
}
