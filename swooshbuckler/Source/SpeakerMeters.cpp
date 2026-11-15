#include "SpeakerMeters.h"

namespace TC = Theme::Colours;

SpeakerMeters::SpeakerMeters(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts) : m_processor(processor), m_fonts(fonts)
{
    setInterceptsMouseClicks(true, false);
    setTooltip("Speaker Meters\nEach speaker's level, after the limiter and before Master Trim, on the Out meter's scale: the fronts, "
               "the surrounds, the tops, and the LFE set apart at the end. A sound shared between speakers peaks lower in each of "
               "them than it would in one, so the limiter can be at work with no meter near the top: GR shows it. The thin bar by "
               "the LFE is its own limiter, which holds the LFE on its own.");
}

float SpeakerMeters::widthFor(room::Width width)
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
    float w = static_cast<float>(mains + tops) * c_pitch + (tops > 0 ? c_topsGap : 0.0f);
    if (layout.lfe >= 0)
        w += c_lfeGap + c_pitch + c_lfeGrW;
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
    m_lfeGr = {};
    if (m_width < room::Width::Quad)
        return;
    const auto order = room::displayChannels(m_width);
    const auto& layout = room::layout(m_width);
    const auto r = getLocalBounds().toFloat();
    // Centred in the space it is given, bars over names.
    float x = r.getX() + std::floor((r.getWidth() - widthFor(m_width)) * 0.5f);
    const float barTop = r.getY(), barBottom = r.getBottom() - c_nameH - 1.0f;
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
        m_bars[i_] = { x + (c_pitch - c_barW) * 0.5f, barTop, c_barW, barBottom - barTop };
        m_names[i_] = { x - 2.0f, r.getBottom() - c_nameH, c_pitch + 4.0f, c_nameH };
        if (role == room::Role::Lfe)
            m_lfeGr = { m_bars[i_].getRight() + 2.0f, barTop, c_lfeGrW, barBottom - barTop };
        x += c_pitch;
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
        const float level = juce::jmax(now, m_processor.getChannelPeak(static_cast<int>(ch)));
        float hold = m_hold[ch];
        if (now >= hold)
        {
            hold = now;
            m_holdTime[ch] = m_time;
        }
        else if (m_time - m_holdTime[ch] > c_holdSeconds)
            hold = juce::jmax(level, hold * std::exp(-dt / 0.15f));
        if (std::abs(level - m_level[ch]) > 0.002f || std::abs(hold - m_hold[ch]) > 0.002f)
        {
            m_level[ch] = level;
            m_hold[ch] = hold;
            changed = true;
        }
    }
    // The LFE's limiter: its deepest reduction of the last block, falling back at the GR meter's 12 dB a second.
    if (! m_lfeGr.isEmpty())
    {
        const float now = juce::jmin(0.0f, m_processor.getLfeGainReductionDb());
        const float gr = juce::jmin(now, juce::jmin(0.0f, m_lfeGrDb + 12.0f * dt));
        if (std::abs(gr - m_lfeGrDb) > 0.05f)
        {
            m_lfeGrDb = gr;
            changed = true;
        }
    }
    if (changed)
        repaint();
}

void SpeakerMeters::paint(juce::Graphics& g)
{
    const auto& layout = room::layout(m_width);
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

    // The LFE's own limiter: a thin bar beside its meter that grows down, 12 dB at the bottom, as GR does.
    if (! m_lfeGr.isEmpty())
    {
        g.setColour(TC::well);
        g.fillRect(m_lfeGr);
        if (m_lfeGrDb < -0.05f)
        {
            const auto in = m_lfeGr.reduced(1.0f);
            g.setColour(TC::busA);
            g.fillRect(in.withHeight(in.getHeight() * juce::jlimit(0.0f, 1.0f, -m_lfeGrDb / 12.0f)));
        }
    }
}
