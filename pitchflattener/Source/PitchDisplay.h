#pragma once

#include <JuceHeader.h>

#include "Engine.h"
#include "Theme.h"

#include <array>
#include <functional>
#include <memory>

/**
 * The SCOPE view: the learned sound across its whole length, both pitch readings over time on a
 * note axis (the one in use cyan, the other dim), the pitch it is flattened to (orange), a level
 * strip along the bottom so timing can be read, and a lime play head while the flattened sound
 * plays, with a trail of lime ticks behind it over the part already heard flattened.
 *
 * Where a reading found no pitch and the line only bridges the gap, the line is drawn dashed and
 * dim. A click on the dim line (or its label) asks to flatten by it instead (onLineClicked).
 */
class PitchDisplay final : public juce::Component, public juce::TooltipClient
{
public:
    PitchDisplay();

    struct View
    {
        std::shared_ptr<const pitchflattener::Result> result;
        pitchflattener::Engine::State state = pitchflattener::Engine::State::Idle;
        double captureSeconds = 0.0;
        std::int64_t playPosition = -1;   // in the flattened sound
        std::uint64_t playGeneration = 0;
        bool rendering = false;
        bool autoLine = true;             // the line parameter is Auto

        bool operator== (const View&) const noexcept;
    };

    void setView (const View&);

    /** A line was clicked: 0 harmonic, 1 ridge. */
    std::function<void (int)> onLineClicked;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

private:
    struct Geometry
    {
        juce::Rectangle<float> plot, strip, timeAxis;
        double lowMidi = 36.0, highMidi = 84.0, seconds = 1.0;

        float xFor (double s) const noexcept { return plot.getX() + (float) (s / seconds) * plot.getWidth(); }
        float yFor (double midi) const noexcept
        {
            return plot.getBottom() - (float) ((midi - lowMidi) / (highMidi - lowMidi)) * plot.getHeight();
        }
    };

    void rebuild();
    int lineAt (juce::Point<float>) const;   // a line or its label under the point, or -1
    void drawLine (juce::Graphics&, int index, bool inUse) const;

    juce::SharedResourcePointer<Theme::Fonts> m_fonts;
    View m_view;
    Geometry m_geometry;
    std::vector<float> m_peaks;               // one per strip column
    std::array<juce::Rectangle<float>, 2> m_labels;
    int m_hoverLine = -1;
};
