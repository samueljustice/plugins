#include "SpeakerMeters.h"

namespace TC = Theme::Colours;

SpeakerMeters::SpeakerMeters(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts) : m_processor(processor), m_fonts(fonts)
{
    setInterceptsMouseClicks(true, false);
    setTooltip("Speaker Meters\nEach speaker's level, after the limiter and before Master Trim, on the Out meter's scale: the fronts, "
               "the surrounds, the tops, and the LFE set apart at the end. A sound shared between speakers peaks lower in each of "
               "them than it would in one, so the limiter can be at work with no meter near the top: the GR meter beside them "
               "shows it. The bar named GR by the LFE is the LFE's own limiter, which holds the LFE on its own.");
}

float SpeakerMeters::widthFor(room::Width width, float pitch)
{
    const auto& layout = room::layout(width);
    if (width < room::Width::Quad)
        return 0.0f;
    int mains = 0, tops = 0;
    for (int ch = 0; ch < layout.numChannels; ++ch)
    {
        const auto role = room::info(layout.speakers[static_cast<size_t>(ch)]).role;
        mains += role == room::Role::Main ? 1 : 0;
        tops += role == room::Role::Height ? 1 : 0;
    }
    float w = static_cast<float>(mains + tops) * pitch + (tops > 0 ? c_topsGap : 0.0f);
    if (layout.lfe >= 0)
        w += c_lfeGap + 2.0f * pitch; // its plate: its meter and its own reduction
    return w;
}

void SpeakerMeters::setWidth(room::Width width)
{
    if (width == m_width)
        return;
    m_width = width;
    m_level.fill(0.0f);
    m_hold.fill(0.0f);
    m_lfeGrDb = 0.0f;
    place();
    repaint();
}

void SpeakerMeters::place()
{
    m_count = 0;
    m_lfeGr = m_lfeGrName = m_lfePlate = {};
    if (m_width < room::Width::Quad)
        return;
    const auto order = room::displayChannels(m_width);
    const auto& layout = room::layout(m_width);
    const auto r = getLocalBounds().toFloat();
    // As wide a pitch as the strip allows (whole pixels, the names' overhang kept inside), and the bars widening with it.
    const float slots = widthFor(m_width, 1.0f) - widthFor(m_width, 0.0f);
    m_pitch = juce::jlimit(c_minPitch, c_maxPitch, std::floor((r.getWidth() - 8.0f - widthFor(m_width, 0.0f)) / juce::jmax(1.0f, slots)));
    m_barW = juce::jlimit(c_minBarW, c_maxBarW, std::round(m_pitch * 0.42f));
    // Centred in the space it is given, bars over names.
    float x = r.getX() + std::floor((r.getWidth() - widthFor(m_width, m_pitch)) * 0.5f);
    const float barTop = r.getY() + 1.0f, barBottom = r.getBottom() - c_nameH - 1.0f;
    bool wasMain = true;
    for (int i = 0; i < layout.numChannels; ++i)
    {
        const int ch = order[static_cast<size_t>(i)];
        const auto role = room::info(layout.speakers[static_cast<size_t>(ch)]).role;
        if (role == room::Role::Height && wasMain)
            x += c_topsGap;
        if (role == room::Role::Lfe)
            x += c_lfeGap;
        wasMain = role == room::Role::Main;
        const auto i_ = static_cast<size_t>(i);
        m_channels[i_] = ch;
        m_bars[i_] = { x + std::floor((m_pitch - m_barW) * 0.5f), barTop, m_barW, barBottom - barTop };
        m_names[i_] = { x - 2.0f, r.getBottom() - c_nameH, m_pitch + 4.0f, c_nameH };
        if (role == room::Role::Lfe)
        {
            // Its own limiter in the next column, the two on one plate.
            m_lfeGr = m_bars[i_].translated(m_pitch, 0.0f);
            m_lfeGrName = m_names[i_].translated(m_pitch, 0.0f);
            m_lfePlate = juce::Rectangle<float>(x - 4.0f, r.getY(), 2.0f * m_pitch + 8.0f, r.getHeight());
        }
        x += m_pitch;
        ++m_count;
    }
}

void SpeakerMeters::step(float dt)
{
    if (m_count == 0)
        return;
    m_time += static_cast<double>(dt);
    bool changed = false;
    for (int i = 0; i < m_count; ++i)
    {
        const auto ch = static_cast<size_t>(m_channels[static_cast<size_t>(i)]);
        // The highest level since the last look, so nothing between two frames is missed; the processor's own
        // reading falls back as the OUT meter's does.
        const float now = m_processor.takeChannelPeak(static_cast<int>(ch));
        float level = juce::jmax(now, m_processor.getChannelPeak(static_cast<int>(ch)));
        float hold = m_hold[ch];
        if (now >= hold)
        {
            hold = now;
            m_holdTime[ch] = m_time;
        }
        else if (m_time - m_holdTime[ch] > c_holdSeconds)
            hold = juce::jmax(level, hold * std::exp(-dt / 0.15f));
        // Always kept, so a meter falls all the way to nothing (under -80 dB it is nothing); painted again only
        // when it has moved enough to see.
        if (level < 1.0e-4f)
            level = 0.0f;
        if (hold < 1.0e-4f)
            hold = 0.0f;
        changed = changed || std::abs(level - m_paintedLevel[ch]) > 0.002f || std::abs(hold - m_paintedHold[ch]) > 0.002f
                  || ((level == 0.0f) != (m_paintedLevel[ch] == 0.0f)) || ((hold == 0.0f) != (m_paintedHold[ch] == 0.0f));
        m_level[ch] = level;
        m_hold[ch] = hold;
    }
    // The LFE's limiter: its deepest reduction of the last block, falling back at the GR meter's 12 dB a second.
    if (! m_lfeGr.isEmpty())
    {
        const float now = juce::jmin(0.0f, m_processor.getLfeGainReductionDb());
        const float gr = juce::jmin(now, juce::jmin(0.0f, m_lfeGrDb + 12.0f * dt));
        changed = changed || std::abs(gr - m_paintedLfeGrDb) > 0.05f || ((gr < 0.0f) != (m_paintedLfeGrDb < 0.0f));
        m_lfeGrDb = gr;
    }
    if (changed)
        repaint();
}

void SpeakerMeters::paint(juce::Graphics& g)
{
    m_paintedLevel = m_level;
    m_paintedHold = m_hold;
    m_paintedLfeGrDb = m_lfeGrDb;
    const auto& layout = room::layout(m_width);
    // The LFE's plate: a tint in its colour behind its meter and its own reduction.
    if (! m_lfePlate.isEmpty())
    {
        g.setColour(TC::stowaway.withAlpha(0.07f));
        g.fillRect(m_lfePlate);
        g.setColour(TC::stowaway.withAlpha(0.4f));
        Theme::Plate::stroke(g, m_lfePlate, 0.0f);
    }
    for (int i = 0; i < m_count; ++i)
    {
        const auto i_ = static_cast<size_t>(i);
        const int ch = m_channels[i_];
        const bool lfe = ch == layout.lfe;
        const auto bar = m_bars[i_];
        g.setColour(TC::well);
        g.fillRect(bar);
        g.setColour(TC::edgeDark);
        g.drawRect(bar, 1.0f);
        const auto in = bar.reduced(2.0f);
        auto yOf = [&in](float db) { return in.getBottom() - in.getHeight() * position(db); };

        const float db = juce::Decibels::gainToDecibels(m_level[static_cast<size_t>(ch)], -60.0f);
        if (db > -59.5f)
        {
            // As the OUT meter: driftwood up to -6, brass above it, cherry for the last decibel. The LFE in its own colour.
            const float top = yOf(db), y6 = yOf(-6.0f), y1 = yOf(-1.0f);
            g.setColour(lfe ? TC::stowaway : TC::output);
            g.fillRect(in.withTop(juce::jmax(top, y6)));
            if (top < y6)
            {
                g.setColour(TC::brass);
                g.fillRect(in.withTop(juce::jmax(top, y1)).withBottom(y6));
            }
            if (top < y1)
            {
                g.setColour(TC::hits);
                g.fillRect(in.withTop(top).withBottom(y1));
            }
        }
        // The slots, and the held peak.
        g.setColour(TC::well);
        for (float y = in.getBottom() - 3.0f; y > in.getY(); y -= 3.0f)
            g.fillRect(juce::Rectangle<float>(in.getX(), y, in.getWidth(), 1.0f));
        if (const float hold = m_hold[static_cast<size_t>(ch)]; hold > 0.001f)
        {
            const float holdDb = juce::Decibels::gainToDecibels(hold, -60.0f);
            g.setColour(holdDb > -1.0f ? TC::hits : TC::text);
            g.fillRect(juce::Rectangle<float>(in.getX(), juce::jlimit(in.getY(), in.getBottom() - 2.0f, yOf(holdDb) - 1.0f), in.getWidth(), 2.0f));
        }

        // Its name under it: the LFE in its colour.
        g.setColour(lfe ? TC::stowaway.brighter(0.3f) : TC::textDim);
        Theme::fitText(g, room::info(layout.speakers[static_cast<size_t>(ch)]).name, m_names[i_], m_fonts.caption(), juce::Justification::centred);
    }

    // The LFE's own limiter: a bar beside its meter that grows down, 12 dB at the bottom, as GR does, named GR.
    if (! m_lfeGr.isEmpty())
    {
        g.setColour(TC::well);
        g.fillRect(m_lfeGr);
        g.setColour(TC::edgeDark);
        g.drawRect(m_lfeGr, 1.0f);
        const auto in = m_lfeGr.reduced(2.0f);
        if (m_lfeGrDb < -0.05f)
        {
            g.setColour(TC::busA);
            g.fillRect(in.withHeight(in.getHeight() * juce::jlimit(0.0f, 1.0f, -m_lfeGrDb / 12.0f)));
        }
        g.setColour(TC::well);
        for (float y = in.getBottom() - 3.0f; y > in.getY(); y -= 3.0f)
            g.fillRect(juce::Rectangle<float>(in.getX(), y, in.getWidth(), 1.0f));
        g.setColour(TC::stowaway.brighter(0.3f));
        Theme::fitText(g, "GR", m_lfeGrName, m_fonts.caption(), juce::Justification::centred);
    }
}
