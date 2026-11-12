#include "SeaVisualiser.h"


namespace
{
    constexpr float c_camHeight = 0.62f;
    constexpr float c_halfWidth = 2.4f;
    constexpr float c_arcZ = 2.7f;      // cannonball passes start and end this far out
    constexpr float c_arcX = 1.75f;     // and this far either side

    juce::Colour seaColour(float t) // across the band axis: teal -> violet -> rose
    {
        return t < 0.5f ? Theme::Colours::sources.interpolatedWith(Theme::Colours::filter, t * 2.0f)
                        : Theme::Colours::filter.interpolatedWith(Theme::Colours::movement, (t - 0.5f) * 2.0f);
    }
}

SeaVisualiser::SeaVisualiser(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts)
    : m_processor(processor), m_fonts(fonts)
{
    setOpaque(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setTooltip("Click the sea to fire a whoosh (or press space).");

    juce::Random r(42);
    for (int i = 0; i < 90; ++i)
        m_stars.push_back({ r.nextFloat(), r.nextFloat() * r.nextFloat(), 0.6f + 1.4f * r.nextFloat(), r.nextFloat() * 6.28f });

    m_logo = std::make_unique<LogoOutline>(BinaryData::sjlogo_svg, static_cast<size_t>(BinaryData::sjlogo_svgSize));

    for (auto& row : m_rows)
        row.fill(0.0f);
    startTimerHz(40);
}

SeaVisualiser::~SeaVisualiser()
{
    stopTimer();
}

void SeaVisualiser::mouseDown(const juce::MouseEvent&)
{
    if (onClick)
        onClick();
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

void SeaVisualiser::timerCallback()
{
    const float dt = 1.0f / 40.0f;
    m_time += dt;
    analyse();

    if (++m_frame % 2 == 0)
    {
        m_rowHead = (m_rowHead + 1) % c_rows;
        m_rows[static_cast<size_t>(m_rowHead)] = m_smooth;
    }

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
            env += (v.env - env) * 0.35f;

            // Each pass is a cannonball arcing across the sea, swinging in towards us at the pass.
            const float across = juce::jlimit(-1.0f, 1.0f, v.x);
            const float middle = 1.0f - std::abs(across);
            const float zClose = juce::jmap(v.closeness, 2.4f, 1.2f);
            const float z = juce::jmap(middle, c_arcZ, zClose);
            const float y = 0.12f + (0.22f + 0.2f * env) * std::sin(juce::MathConstants<float>::pi * 0.5f * (across + 1.0f));
            trail.push_back({ across * c_arcX, y, z });


            if (static_cast<int>(trail.size()) > c_trail)
                trail.pop_front();

            // Flash when it crosses in front of us
            const float lastX = m_lastX[static_cast<size_t>(i)];
            if ((lastX < 0.0f) != (v.x < 0.0f) && std::abs(v.x - lastX) < 0.5f)
                m_flash[static_cast<size_t>(i)] = 1.0f;
            m_lastX[static_cast<size_t>(i)] = v.x;
        }
        else
        {
            env *= 0.85f;
            if (! trail.empty())
                trail.pop_front();
            m_lastX[static_cast<size_t>(i)] = -2.0f;
        }
        m_flash[static_cast<size_t>(i)] *= 0.9f;
    }


    // Logo in the sky once the sea has been calm for a while
    const bool quiet = ! anyFlight && m_processor.getOutputPeak() < 0.02f;
    m_quietSeconds = quiet ? m_quietSeconds + dt : 0.0f;
    const bool showLogo = m_quietSeconds > 1.5f && m_logo != nullptr && m_logo->isLoaded();
    if (showLogo && m_logoOpacity <= 0.001f)
        m_logoTime = 0.0f;
    m_logoOpacity += ((showLogo ? 1.0f : 0.0f) - m_logoOpacity) * (showLogo ? 0.04f : 0.15f);
    if (m_logoOpacity > 0.001f)
        m_logoTime += dt;

    repaint();
}

juce::Point<float> SeaVisualiser::project(P3 p) const
{
    const float z = juce::jmax(0.2f, p.z);
    return { m_cx + p.x / z * m_focal, m_horizon + (c_camHeight - p.y) / z * m_focal };
}

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

    // Night sky
    juce::ColourGradient sky(juce::Colour(0xff061219), 0.0f, 0.0f, juce::Colour(0xff10303c), 0.0f, m_horizon + 30.0f, false);
    g.setGradientFill(sky);
    g.fillRect(bounds);

    for (const auto& s : m_stars)
    {
        const float twinkle = 0.5f + 0.5f * std::sin(static_cast<float>(m_time) * 1.3f + s.phase);
        g.setColour(Theme::Colours::text.withAlpha(0.12f + 0.35f * twinkle * s.size / 2.0f));
        g.fillEllipse(s.x * w, s.y * m_horizon, s.size, s.size);
    }

    // Crescent moon
    {
        const auto moon = juce::Rectangle<float>(26.0f, 26.0f).withCentre({ w * 0.86f, m_horizon * 0.36f });
        g.setColour(Theme::Colours::flight.withAlpha(0.06f));
        g.fillEllipse(moon.expanded(18.0f));
        g.setColour(juce::Colour(0xfff3e3c0).withAlpha(0.85f));
        g.fillEllipse(moon);
        g.setColour(juce::Colour(0xff0a1f29));
        g.fillEllipse(moon.translated(7.0f, -4.0f));
    }

    // Logo in the sky when idle
    if (m_logoOpacity > 0.001f && m_logo != nullptr)
    {
        // Rendered into an image a few times a second (the outline is thousands of segments).
        const auto area = juce::Rectangle<float>(w * 0.5f - m_horizon * 1.0f, 4.0f, m_horizon * 2.0f, m_horizon * 1.9f);
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        const int iw = juce::jmax(1, juce::roundToInt(area.getWidth() * scale)), ih = juce::jmax(1, juce::roundToInt(area.getHeight() * scale));
        if (m_skyLogo.getWidth() != iw || m_skyLogo.getHeight() != ih || ++m_skyLogoAge > 4)
        {
            m_skyLogo = juce::Image(juce::Image::ARGB, iw, ih, true);
            juce::Graphics lg(m_skyLogo);
            m_logo->renderPreview(lg, { 0.0f, 0.0f, static_cast<float>(iw), static_cast<float>(ih) }, m_logoTime, 1.0f);
            m_skyLogoAge = 0;
        }
        g.setOpacity(juce::jlimit(0.0f, 1.0f, m_logoOpacity * 0.75f));
        g.drawImage(m_skyLogo, area);
        g.setOpacity(1.0f);
    }

    // The sea, back to front
    juce::Path line;
    for (int k = c_rows - 1; k >= 0; --k)
    {
        const auto& row = m_rows[static_cast<size_t>((m_rowHead - k + c_rows) % c_rows)];
        const float z = m_zNear + static_cast<float>(k) * m_zStep;
        const float fade = std::pow(1.0f - static_cast<float>(k) / c_rows, 1.3f);
        const float t = static_cast<float>(m_time);

        line.clear();
        juce::Path fill;
        // Occlusion only needs the strip down to the next nearer row (plus its tallest wave), not the whole panel.
        const float zNext = juce::jmax(m_zNear, z - m_zStep);
        const float fillBottom = juce::jmin(h, project({ 0.0f, -0.05f, zNext }).y + 2.0f);
        for (int b = 0; b < c_bands; ++b)
        {
            const float u = static_cast<float>(b) / (c_bands - 1);
            const float x = (u * 2.0f - 1.0f) * c_halfWidth;
            const float swell = 0.022f * std::sin(x * 2.6f + t * 1.4f + static_cast<float>(k) * 0.55f)
                              + 0.012f * std::sin(x * 5.3f - t * 0.9f + static_cast<float>(k) * 0.9f);
            const float y = row[static_cast<size_t>(b)] * 0.42f + swell;
            const auto p = project({ x, y, z });
            if (b == 0) { line.startNewSubPath(p); fill.startNewSubPath(p.x, k == 0 ? h : fillBottom); fill.lineTo(p); }
            else { line.lineTo(p); fill.lineTo(p); }
        }
        fill.lineTo(project({ c_halfWidth, 0.0f, z }).x, k == 0 ? h : fillBottom);
        fill.closeSubPath();

        g.setColour(juce::Colour(0xff071820).interpolatedWith(Theme::Colours::scope, 1.0f - fade));
        g.fillPath(fill);

        juce::ColourGradient grad(seaColour(0.0f).withAlpha(0.85f * fade), project({ -c_halfWidth, 0, z }).x, 0.0f,
                                  seaColour(1.0f).withAlpha(0.85f * fade), project({ c_halfWidth, 0, z }).x, 0.0f, false);
        grad.addColour(0.5, seaColour(0.5f).withAlpha(0.85f * fade));
        g.setGradientFill(grad);
        g.strokePath(line, juce::PathStrokeType(k == 0 ? 2.0f : 1.2f));

    }

    // Cannonballs: each in its bus's colour (Bus A coral, Bus B turquoise, as their knobs and track tags), labelled
    // with the bus, and the track when only one track plays on it.
    std::array<juce::String, RigEngine::c_maxBuses> busLabels;
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
            busLabels[static_cast<size_t>(b)] = juce::String("BUS ") + (b == 0 ? "A" : "B")
                                                + (count == 1 ? (" " + juce::String::charToString(0x00b7) + " ") + cargo.getDisplayName(only) : juce::String());
        }
    }
    struct Label { float env = -1.0f, alpha = 0.0f; juce::Point<float> at; };
    std::array<Label, RigEngine::c_maxBuses> labels; // one per bus, on its louder pass
    for (size_t i = 0; i < m_trails.size(); ++i)
    {
        const auto& trail = m_trails[i];
        if (trail.size() < 2)
            continue;

        const size_t bus = i / 2; // two passes per bus, then the thrown whooshes
        const juce::Colour colour = bus == 0 ? Theme::Colours::movement : (bus == 1 ? Theme::Colours::sources : Theme::Colours::flight);
        const float env = m_flightEnv[i];
        for (size_t j = 1; j < trail.size(); ++j)
        {
            const float a = static_cast<float>(j) / static_cast<float>(trail.size());
            const auto p0 = project(trail[j - 1]);
            const auto p1 = project(trail[j]);
            const float width = (1.0f + 7.0f * env) * a * (m_focal / (trail[j].z * 260.0f));
            g.setColour(colour.withAlpha(0.55f * a * a));
            g.drawLine({ p0, p1 }, width);

            // Reflection on the water
            const auto r0 = project({ trail[j - 1].x, -trail[j - 1].y * 0.35f, trail[j - 1].z });
            const auto r1 = project({ trail[j].x, -trail[j].y * 0.35f, trail[j].z });
            g.setColour(colour.withAlpha(0.12f * a * a));
            g.drawLine({ r0, r1 }, width * 0.7f);
        }

        const auto head = trail.back();
        const auto p = project(head);
        const float radius = (0.05f + 0.11f * env) * m_focal / head.z;
        juce::ColourGradient glow(colour.withAlpha(0.9f), p.x, p.y, colour.withAlpha(0.0f), p.x + radius * 2.2f, p.y, true);
        g.setGradientFill(glow);
        g.fillEllipse(juce::Rectangle<float>(radius * 4.4f, radius * 4.4f).withCentre(p));
        g.setColour(juce::Colour(0xfffff1d6).interpolatedWith(colour, 0.35f));
        g.fillEllipse(juce::Rectangle<float>(radius * 0.9f, radius * 0.9f).withCentre(p));

        // Its label goes just above the glow (drawn below, once per bus, on its louder pass).
        if (env > labels[bus].env)
            labels[bus] = { env, juce::jlimit(0.0f, 1.0f, (env - 0.04f) * 6.0f), { p.x, p.y - radius * 2.3f - 9.0f } };

        if (m_flash[i] > 0.02f)
        {
            const float ring = (1.0f - m_flash[i]) * m_focal * 0.5f / head.z;
            g.setColour(colour.withAlpha(0.5f * m_flash[i]));
            g.drawEllipse(juce::Rectangle<float>(ring * 2.0f, ring * 0.7f).withCentre(project({ head.x, 0.0f, head.z })), 1.5f);
        }
    }

    // Bus labels, fading in and out with the passes, kept apart when they meet.
    {
        g.setFont(m_fonts.label(10.5f));
        std::vector<juce::Rectangle<float>> placed;
        for (size_t b = 0; b < labels.size(); ++b)
        {
            const auto& label = labels[b];
            if (label.alpha <= 0.01f)
                continue;
            const auto& text = busLabels[b];
            const float textW = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), text) + 4.0f;
            auto area = juce::Rectangle<float>(textW, 14.0f).withCentre(label.at);
            area = area.withX(juce::jlimit(4.0f, juce::jmax(4.0f, w - textW - 4.0f), area.getX())).withY(juce::jmax(4.0f, area.getY()));
            for (const auto& other : placed)
                if (area.intersects(other))
                    area = area.withY(other.getY() - area.getHeight() - 1.0f);
            placed.push_back(area);
            const juce::Colour colour = b == 0 ? Theme::Colours::movement : Theme::Colours::sources;
            g.setColour(juce::Colour(0xff061219).withAlpha(0.55f * label.alpha));
            g.drawText(text, area.translated(1.0f, 1.0f), juce::Justification::centred, false);
            g.setColour(colour.withAlpha(0.95f * label.alpha));
            g.drawText(text, area, juce::Justification::centred, false);
        }
    }

    // HUD
    g.setFont(m_fonts.label(12.0f));
    g.setColour(Theme::Colours::textFaint);
    g.drawText("OUTPUT", juce::Rectangle<float>(14.0f, 10.0f, 200.0f, 16.0f), juce::Justification::centredLeft);

    if (m_processor.isRecording())
    {
        const bool blink = std::fmod(m_time, 1.0) < 0.6;
        const auto dot = juce::Rectangle<float>(9.0f, 9.0f).withCentre({ w - 112.0f, 18.0f });
        g.setColour(Theme::Colours::record.withAlpha(blink ? 1.0f : 0.35f));
        g.fillEllipse(dot);
        const int secs = static_cast<int>(m_processor.getRecordedSeconds());
        g.setColour(Theme::Colours::text);
        g.drawText("Rec " + juce::String::formatted("%02d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60),
                   juce::Rectangle<float>(w - 102.0f, 10.0f, 96.0f, 16.0f), juce::Justification::centredLeft);
    }

    g.setColour(Theme::Colours::outline);
    g.drawRect(bounds, 1.0f);
}
