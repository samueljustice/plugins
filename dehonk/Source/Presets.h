#pragma once

/** The factory presets, as data. Values are in the parameters' own units (percent, dBFS, dB, choice
    index, on/off). These are first guesses; they are to be set by measurement against the corpus
    (SPEC 8.1, stage 5). The first entry is the defaults, and new instances start on it. Every factory
    preset uses the default module edges (dehonk::kDefaultEdgesHz).

    Lookahead index: 0 Off, 1 5 ms, 2 10 ms, 3 20 ms. Modules: Woof, Tub, Fog, Bark; then Click. */
namespace FactoryPresets
{
    struct Values
    {
        const char* name;
        float threshold;    // dBFS (-60 = always)
        float depth;        // %
        float sharpness;    // % (0 broad, 100 narrow)
        float speed;        // % (0 slow, 100 fast): how quickly a cut goes in
        float release;      // ms: how slowly a cut lets go
        float maxCut;       // dB: no cut goes deeper
        float impactGuard;  // %: how far the cuts are kept out of a broadband impact while it sounds
        bool modules[4];    // Woof, Tub, Fog, Bark
        bool click;         // the Click module
        float link;         // %
        int lookahead;      // choice index
        float mix;          // %
        float trim;         // dB
    };

    inline constexpr Values list[]
    {
        // The defaults: one instance on the master, before the limiter.
        { "Master bus",             -16.0f, 50.0f,  20.0f, 50.0f, 400.0f, 6.0f, 100.0f, { false, true, true, false }, false, 100.0f, 0, 100.0f, 0.0f },

        // Directly after a psychoacoustic bass enhancer: its harmonics of the upper bass land between
        // about 120 and 300 Hz (SPEC 11.1). It acts on quieter material than on the master.
        { "After RBass",            -24.0f, 50.0f,  20.0f, 50.0f, 400.0f, 6.0f, 100.0f, { false, true, true, false }, false, 100.0f, 0, 100.0f, 0.0f },

        // A printed stem: lower in level than a mix, and latency does not matter, so lookahead
        // catches the fronts of hits.
        { "Printed stem",           -30.0f, 60.0f,  20.0f, 50.0f, 400.0f, 6.0f, 100.0f, { false, true, true, false }, false, 100.0f, 2, 100.0f, 0.0f },

        // Only single ringing resonances; the overall balance is left alone. A resonance stays
        // where it is, so its cut is held longer.
        { "Narrow resonances only", -16.0f, 50.0f, 100.0f, 50.0f, 700.0f, 6.0f, 100.0f, { false, true, true, false }, false, 100.0f, 0, 100.0f, 0.0f },

        // Everything from the bass to the nasal midrange, cut deeper: more Depth under a higher ceiling.
        { "Heavy",                  -16.0f, 80.0f,  20.0f, 50.0f, 400.0f, 9.0f, 100.0f, { true, true, true, true },   false, 100.0f, 0, 100.0f, 0.0f },
    };

    inline constexpr int count = static_cast<int> (sizeof (list) / sizeof (list[0]));
}
