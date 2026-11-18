#pragma once

#include <JuceHeader.h>

#include <utility>
#include <vector>

/**
 * The factory presets, compiled in. PresetManager lists them first, in this order, and
 * exposes them to the host as programs, so the order is part of saved sessions: add new
 * presets at the end of a category's run, and never rename one (sessions find a factory
 * preset by name).
 *
 * Values are REAL units, by parameter ID:
 *   bN_on    0 / 1
 *   bN_type  choice index: 0 Bell, 1 Low Shelf, 2 High Shelf, 3 Low Cut 12, 4 Low Cut 24,
 *            5 Low Cut 48, 6 High Cut 12, 7 High Cut 24, 8 High Cut 48
 *   bN_freq  Hz (20 - 20000)
 *   bN_gain  dB (-18 - +18; cuts ignore it)
 *   bN_q     0.1 - 10 (0.707 on shelves and cuts)
 *   output   dB (-18 - +18)
 *
 * A preset is a complete look: anything it doesn't list goes back to its default
 * (PresetManager::getValueWhenAbsent()), so an entry only lists what it changes.
 * bypass, eq_bypass, doom_sound and doom_level are never part of a preset.
 *
 * Names and categories are upper case: the WAD pixel font has no lower-case glyphs.
 *
 * The values are a std::vector rather than a std::initializer_list: an initializer_list
 * member would point at a temporary array that dies when the table is copied into the
 * vector below.
 */
struct FactoryPreset
{
    const char* name;
    const char* category;                                // menu group ("" = top level)
    std::vector<std::pair<const char*, float>> values;   // real values; anything missing takes its default
    bool currentDefaults = false;                        // INIT: every parameter at today's default, as a new instance starts
};

/** The menu order of the factory categories (presets with category "" sit above them). */
inline juce::StringArray getFactoryPresetCategoryOrder()
{
    return { "CLEAN UP", "TONE", "CHARACTER" };
}

inline const std::vector<FactoryPreset>& getFactoryPresetTable()
{
    static const std::vector<FactoryPreset> presets
    {
        { "INIT", "", {}, true },

        // CLEAN UP
        { "LOW CUT CLEANUP", "CLEAN UP",
          { { "b1_type", 4.0f }, { "b1_freq", 80.0f },   { "b1_q", 0.707f },
            { "b2_type", 0.0f }, { "b2_freq", 250.0f },  { "b2_gain", -1.5f }, { "b2_q", 1.2f } } },

        { "MUD REMOVER", "CLEAN UP",
          { { "b1_type", 3.0f }, { "b1_freq", 40.0f },   { "b1_q", 0.707f },
            { "b2_type", 0.0f }, { "b2_freq", 300.0f },  { "b2_gain", -4.0f }, { "b2_q", 1.5f },
            { "b3_type", 0.0f }, { "b3_freq", 500.0f },  { "b3_gain", -2.0f }, { "b3_q", 2.0f } } },

        // Narrow, deep notches on the mains fundamental and its next two harmonics, with a
        // low cut under them for the rumble. The top two bands are off so nothing else colours it.
        { "HUM NOTCH 50", "CLEAN UP",
          { { "b1_type", 4.0f }, { "b1_freq", 30.0f },   { "b1_q", 0.707f },
            { "b2_type", 0.0f }, { "b2_freq", 50.0f },   { "b2_gain", -18.0f }, { "b2_q", 10.0f },
            { "b3_type", 0.0f }, { "b3_freq", 100.0f },  { "b3_gain", -12.0f }, { "b3_q", 10.0f },
            { "b4_type", 0.0f }, { "b4_freq", 150.0f },  { "b4_gain", -9.0f },  { "b4_q", 10.0f },
            { "b5_on", 0.0f },
            { "b6_on", 0.0f } } },

        { "HUM NOTCH 60", "CLEAN UP",
          { { "b1_type", 4.0f }, { "b1_freq", 30.0f },   { "b1_q", 0.707f },
            { "b2_type", 0.0f }, { "b2_freq", 60.0f },   { "b2_gain", -18.0f }, { "b2_q", 10.0f },
            { "b3_type", 0.0f }, { "b3_freq", 120.0f },  { "b3_gain", -12.0f }, { "b3_q", 10.0f },
            { "b4_type", 0.0f }, { "b4_freq", 180.0f },  { "b4_gain", -9.0f },  { "b4_q", 10.0f },
            { "b5_on", 0.0f },
            { "b6_on", 0.0f } } },

        // TONE
        { "VOCAL PRESENCE", "TONE",
          { { "b1_type", 4.0f }, { "b1_freq", 100.0f },   { "b1_q", 0.707f },
            { "b2_type", 0.0f }, { "b2_freq", 300.0f },   { "b2_gain", -2.5f }, { "b2_q", 1.4f },
            { "b4_type", 0.0f }, { "b4_freq", 3000.0f },  { "b4_gain", 3.0f },  { "b4_q", 1.0f },
            { "b5_type", 0.0f }, { "b5_freq", 6500.0f },  { "b5_gain", -2.0f }, { "b5_q", 4.0f },
            { "b6_type", 2.0f }, { "b6_freq", 12000.0f }, { "b6_gain", 2.5f },  { "b6_q", 0.707f } } },

        { "KICK PUNCH", "TONE",
          { { "b1_type", 0.0f }, { "b1_freq", 60.0f },    { "b1_gain", 4.0f },  { "b1_q", 1.2f },
            { "b2_type", 0.0f }, { "b2_freq", 350.0f },   { "b2_gain", -4.0f }, { "b2_q", 1.5f },
            { "b4_type", 0.0f }, { "b4_freq", 3500.0f },  { "b4_gain", 3.0f },  { "b4_q", 1.4f },
            { "b6_type", 2.0f }, { "b6_freq", 12000.0f }, { "b6_gain", 0.0f },  { "b6_q", 0.707f } } },

        { "AIR AND SPARKLE", "TONE",
          { { "b2_type", 0.0f }, { "b2_freq", 250.0f },   { "b2_gain", -1.5f }, { "b2_q", 1.0f },
            { "b5_type", 0.0f }, { "b5_freq", 5000.0f },  { "b5_gain", 1.5f },  { "b5_q", 0.8f },
            { "b6_type", 2.0f }, { "b6_freq", 10000.0f }, { "b6_gain", 4.0f },  { "b6_q", 0.707f } } },

        { "WARM BLANKET", "TONE",
          { { "b1_type", 1.0f }, { "b1_freq", 120.0f },   { "b1_gain", 3.0f },  { "b1_q", 0.707f },
            { "b4_type", 0.0f }, { "b4_freq", 2500.0f },  { "b4_gain", -1.5f }, { "b4_q", 0.8f },
            { "b6_type", 2.0f }, { "b6_freq", 8000.0f },  { "b6_gain", -4.0f }, { "b6_q", 0.707f } } },

        // CHARACTER
        { "SMILE CURVE", "CHARACTER",
          { { "b1_type", 1.0f }, { "b1_freq", 100.0f },   { "b1_gain", 4.0f },  { "b1_q", 0.707f },
            { "b3_type", 0.0f }, { "b3_freq", 800.0f },   { "b3_gain", -2.0f }, { "b3_q", 0.7f },
            { "b6_type", 2.0f }, { "b6_freq", 10000.0f }, { "b6_gain", 4.0f },  { "b6_q", 0.707f },
            { "output", -3.0f } } },

        // A steep band-pass with a nasal bump: only the bands that make the shape stay on.
        { "TELEPHONE", "CHARACTER",
          { { "b1_type", 5.0f }, { "b1_freq", 400.0f },   { "b1_q", 0.707f },
            { "b2_on", 0.0f },
            { "b3_type", 0.0f }, { "b3_freq", 1500.0f },  { "b3_gain", 6.0f },  { "b3_q", 1.0f },
            { "b4_on", 0.0f },
            { "b5_on", 0.0f },
            { "b6_type", 8.0f }, { "b6_freq", 3400.0f },  { "b6_q", 0.707f },
            { "output", -3.0f } } },

        { "IMP RADIO", "CHARACTER",
          { { "b1_type", 4.0f }, { "b1_freq", 300.0f },   { "b1_q", 0.707f },
            { "b2_on", 0.0f },
            { "b3_type", 0.0f }, { "b3_freq", 2200.0f },  { "b3_gain", 5.0f },  { "b3_q", 2.0f },
            { "b4_on", 0.0f },
            { "b5_on", 0.0f },
            { "b6_type", 7.0f }, { "b6_freq", 5000.0f },  { "b6_q", 0.707f },
            { "output", -2.0f } } },
    };

    return presets;
}
