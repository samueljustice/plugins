#pragma once

#include <JuceHeader.h>
#include "ScopeRing.h"
#include "Theme.h"

/**
 * A little rolling waveform for a section's title bar (bus A, bus B, hits, output):
 * the last few seconds of what that section sends on, newest at the right, in the
 * section's colour. Repaints only its own small area, and only when a new point arrived.
 */
class SectionScope : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    SectionScope(const ScopeRing& ring, juce::Colour colour) : m_ring(ring), m_colour(colour)
    {
        setOpaque(false);
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        namespace TC = Theme::Colours;
        const auto r = getLocalBounds().toFloat();
        // A well cut into the title bar: no outline.
        g.setColour(TC::well.withAlpha(0.9f));
        g.fillRoundedRectangle(r, Theme::Radius::control);

        const auto area = r.reduced(4.0f, 3.0f);
        const float mid = area.getCentreY(), half = area.getHeight() * 0.5f;
        g.setColour(TC::grid);
        g.drawHorizontalLine(juce::roundToInt(mid), area.getX(), area.getRight());

        m_ring.read(m_lo, m_hi);
        // Gentle compression so quiet passages still show (and loud ones don't just fill the box).
        auto shape = [](float v) { return std::copysign(std::pow(juce::jlimit(0.0f, 1.0f, std::abs(v)), 0.6f), v); };
        constexpr int n = ScopeRing::c_points;
        auto& p = m_path; // kept between frames, so drawing it allocates nothing
        p.clear();
        for (int k = 0; k < n; ++k)
        {
            const float x = area.getX() + area.getWidth() * static_cast<float>(k) / static_cast<float>(n - 1);
            const float y = mid - shape(juce::jmax(m_hi[static_cast<size_t>(k)], 0.0f)) * half;
            if (k == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
        for (int k = n - 1; k >= 0; --k)
        {
            const float x = area.getX() + area.getWidth() * static_cast<float>(k) / static_cast<float>(n - 1);
            p.lineTo(x, mid - shape(juce::jmin(m_lo[static_cast<size_t>(k)], 0.0f)) * half - 0.5f);
        }
        p.closeSubPath();
        // Older material fades out to the left.
        g.setGradientFill(juce::ColourGradient(m_colour.withAlpha(0.15f), area.getX(), 0.0f, m_colour.withAlpha(0.95f), area.getRight(), 0.0f, false));
        g.fillPath(p);
    }

private:
    void timerCallback() override
    {
        const int pos = m_ring.position();
        if (pos != m_last && isShowing())
        {
            m_last = pos;
            repaint();
        }
    }

    const ScopeRing& m_ring;
    juce::Colour m_colour;
    int m_last = -1;
    std::array<float, ScopeRing::c_points> m_lo {}, m_hi {};
    juce::Path m_path;
};
