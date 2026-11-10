#pragma once

#include "../common/IrPack.h"
#include "../common/ZeroLatencyConvolver.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <memory>
#include <vector>

namespace roar
{
    /**
     * CHAMBER's eight rooms (Params::chamberRoomNames, in that order), ready for the convolver at
     * one sample rate.
     *
     * Two sources, the same list:
     *   - bundled: the rooms compiled into the plug-in as an encrypted blob (a build with
     *     -DROARSCHACH_IR_DIR and -DROARSCHACH_IR_KEY). Decrypted and decoded in memory only.
     *   - modelled: when no blob is compiled in (or it does not open), each room is synthesised
     *     from the measurements of its capture (decay, clarity, brightness, modes, reflection
     *     density), so presets and sessions still load and sound close.
     *
     * Each room is stored as mid (L + R) / 2 and side (L - R) / 2 (the captures are true stereo:
     * CHAMBER's Width scales the room's own side), resampled once from 48 kHz to the host rate
     * (windowed sinc), and partitioned for the convolver. Built at prepare, off the audio thread,
     * synchronously, and cached per sample rate: every instance and both twin chains share one
     * immutable bank.
     */
    class ChamberBank
    {
    public:
        static constexpr int numRooms = 8;
        enum class Source { bundled, modelled };

        struct Room
        {
            ConvolverIr ir;           // outputs: 0 mid, 1 side
            double rt60 = 0.4;        // seconds, measured on the mid
            double earlyEnergy = 0.0; // the mid's head and level-0 energy (the first ~10.7 ms)
            std::vector<double> lateEnergy;   // the mid's level-1 partitions (from ~10.7 ms)
            std::vector<double> bandPower;    // the mid's power response per bandCentre() (1/6 octave)
            double decayStart = 0.0;  // s: where the late part's (voice-band) energy peaks; captures
                                      // often swell in for 50-130 ms, and Resonance's decay change starts there
            double maxDecayScale = 1.0;   // the longest decay (x rt60, 1..maxResonanceScale) whose
                                          // tail still fades out inside the IR (Resonance's top end)
        };

        /** Resonance's longest decay, x a room's own (rooms whose IR is too short for it get less). */
        static constexpr double maxResonanceScale = 1.6;
        /** How far under its peak a lengthened room's tail must be where the IR's final fade starts
            (85 % of its length), so the fade never cuts off an audible ring. */
        static constexpr double tailEndDb = -40.0;
        /** Resonance's late gain: the level change in dB at time t (s) for a level offset and a decay scale. */
        static double lateGainDb (const Room& room, double t, double lateDb, double scale);

        /** The bands Room::bandPower is measured in: 1/6 octave from 63 Hz, numBands of them (to ~13 kHz). */
        static constexpr int numBands = 47;
        static double bandCentre (int band) { return 63.0 * std::pow (2.0, band / 6.0); }

        /** The shared bank for a rate (bundled rooms when they open, else modelled). Allocates. */
        static std::shared_ptr<const ChamberBank> get (double fs);
        /** A new bank from a given source (tests). bundled falls back to modelled if unavailable. */
        static std::shared_ptr<const ChamberBank> build (double fs, Source source);

        /** True when this build carries the bundled rooms and they open with the compiled-in key. */
        static bool hasBundledRooms();
        /** The bundled rooms at 48 kHz (empty without them). */
        static std::vector<irpack::Ir> bundledRooms();
        /** The modelled rooms at 48 kHz (deterministic). */
        static std::vector<irpack::Ir> modelledRooms();
        /** Tests: make get() use the modelled rooms even when the bundled ones exist. */
        static void setForceModelled (bool force);

        /** A bank from rooms at any rate (resampled to fs). */
        static std::shared_ptr<const ChamberBank> fromRooms (double fs, const std::vector<irpack::Ir>& rooms, Source source);

        double getSampleRate() const { return m_fs; }
        Source getSource() const { return m_source; }
        const ConvolverLayout& getLayout() const { return m_layout; }
        const Room& room (int index) const { return m_rooms[static_cast<size_t> (std::clamp (index, 0, numRooms - 1))]; }

    private:
        double m_fs = 48000.0;
        Source m_source = Source::modelled;
        ConvolverLayout m_layout;
        std::array<Room, numRooms> m_rooms;
    };
}
