#include "SeaVisualiser.h"

namespace
{
    constexpr float c_camHeight = 0.62f;
    constexpr float c_halfWidth = 2.4f;
    constexpr float c_arcZ = 2.7f;      // cannonball passes start and end this far out
    constexpr float c_arcX = 1.75f;     // and this far either side

    // The water's own colours. Depth, not loudness, picks them, and none of them is a bus colour (coral,
    // turquoise, cherry): the buses own the comets, the water owns the distance.
    const juce::Colour c_deepWater { 0xff1f5f82 };  // far out: cold deep blue
    const juce::Colour c_nearWater { 0xff79b6c6 };  // under the camera: moonlit steel
    const juce::Colour c_fog       { 0xff0e2a38 };  // what the horizon fades everything into
    const juce::Colour c_hollow    { 0xff071820 };  // the water between two rows
    const juce::Colour c_moonlight { 0xfff3e3c0 };

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
        startTimerHz(4); // the timer is only the watch now: enough to notice if the screen's clock stops
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
    startTimerHz(60);  // no screen clock (or it stopped): this is the clock again
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
    // After five seconds of quiet the sea runs at a third of the rate: a big picture that costs little when
    // there is nothing to show.
    if (m_quietSeconds > 5.0f && ++m_idleSkip < 3)
        return;
    dt *= static_cast<float>(1 + m_idleSkip);
    m_idleSkip = 0;

    m_time += dt;
    analyse();

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
    for (int i = 0; i < RigEngine::c_maxFlights; ++i)
    {
        const auto v = engine.getFlightView(i);
        auto& trail = m_trails[static_cast<size_t>(i)];
        auto& env = m_flightEnv[static_cast<size_t>(i)];
        if (v.active)
        {
            anyFlight = true;
            env += (v.env - env) * juce::jmin(1.0f, 21.0f * dt);

            // Each pass is a cannonball arcing across the sea, swinging in towards us at the pass.
            const float across = juce::jlimit(-1.0f, 1.0f, v.x);
            const float middle = 1.0f - std::abs(across);
            const float zClose = juce::jmap(v.closeness, 2.4f, 1.2f);
            const float z = juce::jmap(middle, c_arcZ, zClose);
            const float y = 0.12f + (0.22f + 0.2f * env) * std::sin(juce::MathConstants<float>::pi * 0.5f * (across + 1.0f));
            trail.push_back({ across * c_arcX, y, z });

            if (static_cast<int>(trail.size()) > c_trail)
                trail.pop_front();

            // It splashes: a bump in the water under it, which then rolls away with the swell.
            const float u = juce::jlimit(0.0f, 1.0f, (across * c_arcX / c_halfWidth + 1.0f) * 0.5f);
            const int band = juce::jlimit(0, c_bands - 1, juce::roundToInt(u * static_cast<float>(c_bands - 1)));
            for (int d = -3; d <= 3; ++d)
            {
                const int b = band + d;
                if (b >= 0 && b < c_bands)
                    m_wake[static_cast<size_t>(b)] = juce::jmax(m_wake[static_cast<size_t>(b)],
                                                               0.55f * env * (1.0f - std::abs(static_cast<float>(d)) / 4.0f));
            }

            // Flash when it crosses in front of us
            const float lastX = m_lastX[static_cast<size_t>(i)];
            if ((lastX < 0.0f) != (v.x < 0.0f) && std::abs(v.x - lastX) < 0.5f)
                m_flash[static_cast<size_t>(i)] = 1.0f;
            m_lastX[static_cast<size_t>(i)] = v.x;
        }
        else
        {
            env *= std::pow(0.03f, dt);
            if (! trail.empty())
                trail.pop_front();
            m_lastX[static_cast<size_t>(i)] = -2.0f;
        }
        m_flash[static_cast<size_t>(i)] *= std::pow(0.02f, dt);
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

    // The mark's outline, as a handful of brighter stars on it joined by faint lines. The outline has thousands
    // of segments; a constellation wants a couple of dozen points, so each contour is walked and a star is
    // dropped every so many units of its length.
    const auto path = m_logo->createOutlinePath();

    struct Contour { int first = 0, count = 0; float length = 0.0f; };
    std::vector<juce::Point<float>> points;
    std::vector<float> lengths;   // arc length to each point
    std::vector<Contour> contours;

    juce::PathFlatteningIterator it(path, {}, 0.004f);
    float runningLength = 0.0f;
    bool startNew = true;
    while (it.next())
    {
        if (startNew)
        {
            contours.push_back({ static_cast<int>(points.size()), 0, 0.0f });
            runningLength = 0.0f;
            points.push_back({ it.x1, it.y1 });
            lengths.push_back(0.0f);
        }
        startNew = it.isLastInSubpath();
        runningLength += juce::Point<float>(it.x1, it.y1).getDistanceFrom({ it.x2, it.y2 });
        points.push_back({ it.x2, it.y2 });
        lengths.push_back(runningLength);
        auto& c = contours.back();
        c.count = static_cast<int>(points.size()) - c.first;
        c.length = runningLength;
    }

    float longest = 0.0f;
    for (const auto& c : contours)
        longest = juce::jmax(longest, c.length);
    if (longest <= 0.0f)
        return;

    for (const auto& c : contours)
    {
        if (c.length < longest * 0.18f || c.count < 4)
            continue;
        const int stars = juce::jlimit(4, 9, juce::roundToInt(4.0f + 5.0f * c.length / longest));
        const int first = static_cast<int>(m_constellation.size());
        for (int s = 0; s < stars; ++s)
        {
            const float want = c.length * static_cast<float>(s) / static_cast<float>(stars);
            int index = c.first;
            for (int k = c.first; k < c.first + c.count; ++k, index = k)
                if (lengths[static_cast<size_t>(k)] >= want)
                    break;
            const auto p = points[static_cast<size_t>(juce::jlimit(c.first, c.first + c.count - 1, index))];
            m_constellation.push_back({ p, 1.5f + 1.1f * m_random.nextFloat(), m_random.nextFloat() * 6.28f });
        }
        const int added = static_cast<int>(m_constellation.size()) - first;
        for (int s = 0; s < added; ++s)
            m_constellationLines.push_back({ first + s, first + (s + 1) % added }); // the contour closes
        if (m_constellation.size() >= 30)
            break;
    }
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
    const float horizon = static_cast<float>(getHeight()) * 0.30f;
    const float span = juce::jmax(40.0f, horizon - 22.0f);
    return juce::Rectangle<float>(span, span).withCentre({ static_cast<float>(getWidth()) * 0.55f, horizon * 0.50f });
}

void SeaVisualiser::paintConstellation(juce::Graphics& g, float w)
{
    if (m_constellationOpacity < 0.01f || m_constellation.empty())
        return;

    const auto area = constellationArea();
    const juce::Point<float> centre = area.getCentre();
    const float span = area.getWidth();
    juce::ignoreUnused(w);
    auto place = [&](juce::Point<float> p) { return juce::Point<float>(centre.x + p.x * span, centre.y - p.y * span); };

    const float alpha = juce::jlimit(0.0f, 1.0f, m_constellationOpacity);
    const float t = m_constellationTime;

    g.setColour(c_moonlight.withAlpha(0.14f * alpha));
    for (const auto& line : m_constellationLines)
    {
        const auto a = place(m_constellation[static_cast<size_t>(line.first)].at);
        const auto b = place(m_constellation[static_cast<size_t>(line.second)].at);
        g.drawLine({ a, b }, 0.8f);
    }
    for (const auto& s : m_constellation)
    {
        const float twinkle = 0.55f + 0.45f * std::sin(t * 2.1f + s.phase);
        const auto p = place(s.at);
        g.setColour(c_moonlight.withAlpha(0.20f * alpha * twinkle));
        g.fillEllipse(juce::Rectangle<float>(s.size * 3.2f, s.size * 3.2f).withCentre(p));
        g.setColour(c_moonlight.withAlpha(juce::jmin(1.0f, 0.55f + 0.35f * twinkle) * alpha));
        g.fillEllipse(juce::Rectangle<float>(s.size, s.size).withCentre(p));
    }
}

//==============================================================================
void SeaVisualiser::buildSky(float pixelScale)
{
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
void SeaVisualiser::paintWater(juce::Graphics& g, float h)
{
    juce::Path line, fill;
    std::array<juce::Point<float>, c_bands> p {};

    for (int k = c_rows - 1; k >= 0; --k)
    {
        const auto& row = m_rows[static_cast<size_t>((m_rowHead - k + c_rows) % c_rows)];
        // The row sits between two steps: it rolls away smoothly instead of jumping once per step.
        const float depth = static_cast<float>(k) + m_rowPhase;
        const float z = m_zNear + depth * m_zStep;
        const float nearness = juce::jlimit(0.0f, 1.0f, 1.0f - depth / static_cast<float>(c_rows));
        const float t = static_cast<float>(m_time);

        const float zNext = juce::jmax(m_zNear, z - m_zStep);
        const float fillBottom = juce::jmin(h, project({ 0.0f, -0.05f, zNext }).y + 2.0f);
        const float nearRow = depth < 1.0f ? h : fillBottom;

        float loudest = 0.0f;
        const float swellGain = 1.0f + 1.8f * m_swell;
        for (int b = 0; b < c_bands; ++b)
        {
            const float u = static_cast<float>(b) / (c_bands - 1);
            const float x = (u * 2.0f - 1.0f) * c_halfWidth;
            const float swell = swellGain * (0.022f * std::sin(x * 2.6f + t * 1.4f + depth * 0.55f)
                                           + 0.012f * std::sin(x * 5.3f - t * 0.9f + depth * 0.9f));
            const float height = row[static_cast<size_t>(b)];
            loudest = juce::jmax(loudest, height);
            p[static_cast<size_t>(b)] = project({ x, height * 0.42f + swell, z });
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
        g.setColour(c_hollow.interpolatedWith(c_fog, (1.0f - nearness) * 0.9f));
        g.fillPath(fill);
        g.setColour(waterColour(nearness, juce::jmin(1.0f, loudest)));
        g.strokePath(line, juce::PathStrokeType(depth < 1.0f ? 1.7f : 1.1f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    }
}

//==============================================================================
void SeaVisualiser::paintComets(juce::Graphics& g, float w)
{
    namespace TC = Theme::Colours;

    std::array<juce::String, c_cometGroups> busLabels;
    {
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
            busLabels[static_cast<size_t>(b)] = juce::String("Bus ") + (b == 0 ? "A" : "B")
                                                + (count == 1 ? (" " + juce::String::charToString(0x00b7) + " ") + cargo.getDisplayName(only) : juce::String());
        }
        busLabels[RigEngine::c_maxBuses] = "Throw";
    }

    struct Label { float env = -1.0f, alpha = 0.0f; juce::Point<float> at; };
    std::array<Label, c_cometGroups> labels; // one per bus, on its louder pass, and one for the throws
    for (size_t i = 0; i < m_trails.size(); ++i)
    {
        const auto& trail = m_trails[i];
        if (trail.size() < 2)
            continue;

        // Two passes per bus, then the thrown whooshes: a throw is the same comet in brass, and brighter.
        const size_t group = static_cast<size_t>(cometGroup(static_cast<int>(i)));
        const bool thrown = static_cast<int>(group) == RigEngine::c_maxBuses;
        const juce::Colour colour = thrown ? TC::brass : (group == 0 ? TC::busA : TC::busB);
        const float bright = thrown ? 1.3f : 1.0f;
        const float env = m_flightEnv[i];
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
        g.setColour(juce::Colour(0xfffff1d6).interpolatedWith(colour, thrown ? 0.45f : 0.35f));
        g.fillEllipse(juce::Rectangle<float>(radius * 0.9f, radius * 0.9f).withCentre(p));

        // Its label goes just above the glow (drawn below, once per group, on its louder pass).
        if (env > labels[group].env)
            labels[group] = { env, juce::jlimit(0.0f, 1.0f, (env - 0.04f) * 6.0f), { p.x, p.y - radius * 2.3f - 9.0f } };

        if (m_flash[i] > 0.02f)
        {
            const float ring = (1.0f - m_flash[i]) * m_focal * 0.5f / head.z;
            g.setColour(colour.withAlpha(0.5f * m_flash[i]));
            g.drawEllipse(juce::Rectangle<float>(ring * 2.0f, ring * 0.7f).withCentre(project({ head.x, 0.0f, head.z })), 1.5f);
        }
    }

    // The labels, fading in and out with the passes: kept apart from each other, and off the readouts and the
    // buttons the window puts over the sea.
    if (! m_labelsShown)
        return;

    const auto labelFont = m_fonts.caption();
    std::array<juce::Rectangle<float>, c_cometGroups> placed;
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
        const juce::Rectangle<float> keepOut[] = { m_titleBox, m_recBox, m_reserved };
        area = clearOf(area, keepOut, 3, placed.data(), static_cast<int>(numPlaced));
        if (numPlaced < placed.size())
            placed[numPlaced++] = area;

        const juce::Colour colour = b == 0 ? TC::busA : (b == 1 ? TC::busB : TC::brass);
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
    const float w = bounds.getWidth(), h = bounds.getHeight();
    m_focal = juce::jmax(w * 0.36f, h * 0.42f);
    m_horizon = h * 0.30f;
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
        constexpr int rows = 26;
        for (int k = 0; k < rows; ++k)
        {
            const float depth = static_cast<float>(k) + m_rowPhase;
            const float z = m_zNear + depth * m_zStep;
            const float nearness = 1.0f - depth / static_cast<float>(rows);
            const float worldX = (moonAt.x - m_cx) * z / m_focal;
            const float u = juce::jlimit(0.0f, 1.0f, (worldX / c_halfWidth + 1.0f) * 0.5f);
            const int band = juce::jlimit(0, c_bands - 1, juce::roundToInt(u * static_cast<float>(c_bands - 1)));
            const float height = m_rows[static_cast<size_t>((m_rowHead - k + c_rows) % c_rows)][static_cast<size_t>(band)];
            // Two or three flecks on each row, scattered across a path that widens towards the camera: the
            // offsets are in world units scaled by depth, so a fleck is the same few pixels wherever it is.
            const int flecks = nearness > 0.45f ? 3 : 2;
            for (int f = 0; f < flecks; ++f)
            {
                const float seed = static_cast<float>(k) * 1.7f + static_cast<float>(f) * 4.3f;
                const float side = std::sin(seed * 2.11f) * (0.05f + 0.22f * nearness);
                const float sway = 0.04f * std::sin(t * 1.9f + seed);
                const auto at = project({ worldX + (side + sway) * z, height * 0.42f, z });
                const float len = 3.0f + 11.0f * nearness * (0.6f + 0.4f * std::abs(std::sin(seed)));
                const float shine = 0.3f + 0.7f * std::abs(std::sin(t * 2.7f + seed * 1.3f));
                g.setColour(c_moonlight.withAlpha(0.10f + 0.34f * nearness * shine));
                g.fillRect(at.x - len * 0.5f, at.y - 0.8f, len, 1.5f);
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
        const float z = 2.6f;
        const float rx = (1.0f - m_hitRing) * 1.7f * m_focal / z;
        g.setColour(TC::hits.withAlpha(0.30f * m_hitRing));
        g.drawEllipse(juce::Rectangle<float>(rx * 2.0f, rx * 0.42f).withCentre(project({ 0.0f, 0.0f, z })), 1.2f);
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

    paintComets(g, w);

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

    // The morph pad is docked over the sea: the water behind it is dimmed so the pad reads.
    if (! m_dock.isEmpty())
    {
        g.setColour(TC::window.withAlpha(0.55f));
        g.fillRect(bounds);
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

void SeaVisualiser::resized()
{
    const auto bounds = getLocalBounds().toFloat();
    m_sky = juce::Image();
    // The window the sea shows through: the frame's inner edge, its corners cut off by the gussets.
    const float band = c_frame, cut = c_gusset - band;
    const auto window = bounds.reduced(band);
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
