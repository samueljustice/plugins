#pragma once

/** The factory presets, as data. Values are in the parameters' own units (percent, dBFS, dB, choice
    index, on/off). These are first guesses; they are to be set by measurement against the corpus
    (SPEC 8.1, stage 5). The first entry is the defaults, and new instances start on it. Every factory
    preset uses the default module edges (dehonk::kDefaultEdgesHz).

    Lookahead index: 0 Off, 1 5 ms, 2 10 ms, 3 20 ms. Link index: 0 Linked, 1 Grouped, 2 Unlinked.
    Detection index: 0 Fast, 1 Steady.
    Modules: Woof, Tub, Fog, Bark; then Click. Every factory preset leaves each range's timing
    locked to Speed and Release (the parameters' defaults). */
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
        int linkMode;       // choice index
        bool dialogue;      // listen for the dialogue on the sidechain
        int lookahead;      // choice index
        float mix;          // %
        float trim;         // dB
        int detection;      // choice index: 0 Fast, 1 Steady
    };

    inline constexpr Values list[]
    {
        // The defaults: one instance on the master, before the limiter.
        { "Master bus",             -16.0f, 50.0f,  20.0f, 50.0f, 400.0f, 6.0f, 100.0f, { false, true, true, false }, false, 0, false, 0, 100.0f, 0.0f, 0 },

        // The same with the dialogue stem on the sidechain: bands the dialogue accounts for are left alone.
        { "Master bus, dialogue keyed", -16.0f, 50.0f, 20.0f, 50.0f, 400.0f, 6.0f, 100.0f, { false, true, true, false }, false, 0, true, 0, 100.0f, 0.0f, 0 },

        // Directly after a psychoacoustic bass enhancer: its harmonics of the upper bass land between
        // about 120 and 300 Hz (SPEC 11.1). It acts on quieter material than on the master.
        { "After a bass enhancer",  -24.0f, 50.0f,  20.0f, 50.0f, 400.0f, 6.0f, 100.0f, { false, true, true, false }, false, 0, false, 0, 100.0f, 0.0f, 0 },

        // A printed stem: lower in level than a mix, and latency does not matter, so lookahead
        // catches the fronts of hits.
        { "Printed stem",           -30.0f, 60.0f,  20.0f, 50.0f, 400.0f, 6.0f, 100.0f, { false, true, true, false }, false, 0, false, 2, 100.0f, 0.0f, 0 },

        // Only single ringing resonances; the overall balance is left alone. A resonance stays
        // where it is, so its cut is held longer.
        { "Narrow resonances only", -16.0f, 50.0f, 100.0f, 50.0f, 700.0f, 6.0f, 100.0f, { false, true, true, false }, false, 0, false, 0, 100.0f, 0.0f, 0 },

        // Everything from the bass to the nasal midrange, cut deeper: more Depth under a higher ceiling.
        { "Heavy",                  -16.0f, 80.0f,  20.0f, 50.0f, 400.0f, 9.0f, 100.0f, { true, true, true, true },   false, 0, false, 0, 100.0f, 0.0f, 0 },

        // The master bus with Detection at Steady (SPEC 13.19): a peak must hold for about a second
        // before it is cut, and a broad lift is left alone. About half as much cutting, fewer cuts
        // at once, less taken from dense, noisy layers; a new build-up takes about two to four
        // seconds to be caught.
        { "Master bus, steady",     -16.0f, 50.0f,  20.0f, 50.0f, 400.0f, 6.0f, 100.0f, { false, true, true, false }, false, 0, false, 0, 100.0f, 0.0f, 1 },
    };

    inline constexpr int count = static_cast<int> (sizeof (list) / sizeof (list[0]));
}
