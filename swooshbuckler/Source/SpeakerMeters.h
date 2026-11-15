#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Room.h"
#include "Theme.h"

#include <array>

/**
 * The speaker meters: in a room (Quad and wider), one meter for each speaker in Output, in the room's display
 * order (room::displayChannels: L C R, the surrounds, the tops, and the LFE set apart at the end), each with the
 * speaker's name under it. Each is a slotted window like the OUT meter beside it, on the same scale, with a held
 * peak: the level after the limiter, before Master Trim. The LFE's bar is in Stowaway's colour (its Sub is what
 * Discrete sends it), and beside it a thin bar shows its own limiter's reduction. The mains' reduction stays on
 * the GR meter.
 *
 * Message thread. step() reads the processor's per-speaker levels (it is their one reader of the held peaks).
 */
class SpeakerMeters : public juce::Component, public juce::SettableTooltipClient
{
public:
    SpeakerMeters(SwooshbucklerAudioProcessor& processor, const Theme::Fonts& fonts);

    /** The room it shows. Mono and Stereo show nothing (the OUT meter is theirs). */
    void setWidth(room::Width width);
    room::Width getWidth() const { return m_width; }
    /** One frame (dt seconds): each speaker's level and peak hold, and the LFE's limiter. Repaints what changed. */
    void step(float dt);
    void paint(juce::Graphics&) override;
    void resized() override { place(); }

    /** How many meters it shows, and for each (in display order) its processing channel, its bar and its name's box. */
    int numMeters() const { return m_count; }
    int channelOf(int meter) const { return m_channels[static_cast<size_t>(juce::jlimit(0, room::c_maxChannels - 1, meter))]; }
    juce::Rectangle<float> barArea(int meter) const { return m_bars[static_cast<size_t>(juce::jlimit(0, room::c_maxChannels - 1, meter))]; }
    juce::Rectangle<float> nameArea(int meter) const { return m_names[static_cast<size_t>(juce::jlimit(0, room::c_maxChannels - 1, meter))]; }
    /** The LFE's own reduction bar (empty without an LFE). */
    juce::Rectangle<float> lfeReductionArea() const { return m_lfeGr; }
    /** A meter's level and held peak as shown (gain), by processing channel. */
    float shownLevel(int channel) const { return m_level[static_cast<size_t>(juce::jlimit(0, room::c_maxChannels - 1, channel))]; }
    float shownHold(int channel) const { return m_hold[static_cast<size_t>(juce::jlimit(0, room::c_maxChannels - 1, channel))]; }
    float shownLfeReductionDb() const { return m_lfeGrDb; }

    /** The column a meter takes, its bar's width, and the gaps before the tops and before the LFE. */
    static constexpr float c_pitch = 20.0f, c_barW = 10.0f, c_topsGap = 8.0f, c_lfeGap = 14.0f, c_lfeGrW = 4.0f;
    static constexpr float c_nameH = 13.0f;
    /** The width the meters of a room take (its widest is 7.1.4). */
    static float widthFor(room::Width width);
    /** Where a level sits on a bar, 0 (-60 dB) to 1 (0 dBFS): the OUT meter's scale. */
    static float position(float db)
    {
        const float t = juce::jlimit(0.0f, 1.0f, 1.0f + db / 60.0f);
        return t * t;
    }

private:
    void place();

    SwooshbucklerAudioProcessor& m_processor;
    const Theme::Fonts& m_fonts;
    room::Width m_width = room::Width::Stereo;
    int m_count = 0;
    std::array<int, room::c_maxChannels> m_channels {};
    std::array<juce::Rectangle<float>, room::c_maxChannels> m_bars, m_names;
    juce::Rectangle<float> m_lfeGr;
    std::array<float, room::c_maxChannels> m_level {}, m_hold {};
    std::array<float, room::c_maxChannels> m_paintedLevel {}, m_paintedHold {}; // as last painted: step() repaints when they have moved
    std::array<double, room::c_maxChannels> m_holdTime {};
    float m_lfeGrDb = 0.0f, m_paintedLfeGrDb = 0.0f;
    double m_time = 0.0;
    static constexpr double c_holdSeconds = 1.5;
};
