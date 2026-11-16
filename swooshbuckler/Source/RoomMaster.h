#pragma once

#include "MasterProcessor.h"
#include "Room.h"

#include <array>

/**
 * The output stage at any room width: MasterProcessor (Inflate, Push, the true-peak Ceiling limiter) on every
 * channel, with two limiters instead of one.
 *
 *   main channels (every speaker but the LFE) --> MasterProcessor, one gain linked across all of them
 *   LFE                                        --> its own MasterProcessor: Push and the Ceiling as the mains
 *                                                  have them, no Inflate (it would add harmonics above the LFE's
 *                                                  low-pass), its own gain, which holds for 60 ms and lets go
 *                                                  over 300 ms or more, so it never bends a low note (a limiter
 *                                                  that follows a 45 Hz note cycle by cycle puts its third
 *                                                  harmonic 37 dB under it, above the low-pass)
 *
 * THE SAME AT EVERY WIDTH. Inflate (a curve) and the limiter depend on level, and in a room one pass is shared
 * between speakers: each carries less of it than a Stereo channel would, so shaping and limiting speaker by
 * speaker would do less the wider the room and the more a pass is spread. In a room (Quad and wider) the mains
 * are therefore worked BY SIDE (MasterProcessor::setSides): the left speakers together are treated as the Stereo
 * left channel they stand in for, the right speakers as the right, the centre speaker half to each. The peak the
 * limiter reads is the side's speakers summed by power, which for a pass shared between them is the pass itself;
 * the level that drives Inflate in a speaker is the same sum over the speakers that carry the SAME SOUND as it
 * does (measured: see MasterProcessor::inflateSides), which for a shared pass is again the pass itself, while a
 * speaker carrying something else (another sound, its own share of the reverb) is shaped by its own level and
 * is not pulled about by a loud sound in a neighbour. The room's panner shares power between left and right exactly as a
 * stereo pan does, wherever the pass is in depth and height and whatever the Spread, so a pass gets the Inflate,
 * Push and limiting it gets in Stereo. Stereo (and Mono, carried as Stereo) is a channel a side already and runs
 * as it always did.
 *
 * What follows from it: a pass spread over four speakers of one side reaches the Ceiling together, as its sum,
 * not when one speaker does, so each speaker's own peak then sits below the Ceiling (6 dB for four equal
 * speakers) and the room is as loud as the Stereo render, not louder. No channel is ever above the Ceiling: a
 * channel's peak is never more than its side's, and the centre speaker's own peak is read as well (a sound in
 * the centre speaker alone is therefore limited 3 dB sooner than the same sound as a phantom centre: one
 * speaker carries all of it, and a channel cannot go over the Ceiling whatever it stands in for).
 *
 * The main channels share one gain, so a pass that moves from speaker to speaker is never pulled about by the
 * limiter: the room keeps its balance. The LFE has its own, so a loud sub never pulls the room down (and the room
 * never pulls the sub down). Both run at the same rate with the same look-ahead, so every channel is delayed by
 * exactly the same number of samples whatever the width.
 *
 * The channels arrive in the width's processing order (room::layout). Mono is carried as Stereo (the engine
 * makes stereo; Mono is folded from it at the very end), so prepare() with Width::Mono prepares two channels.
 *
 * No allocation after prepare(): safe for the audio thread.
 */
class RoomMaster
{
public:
    /** The width the stage carries: Mono is carried as Stereo. */
    static room::Width pipeWidth(room::Width width) noexcept { return width == room::Width::Mono ? room::Width::Stereo : width; }
    /** How many channels that is. */
    static int pipeChannels(room::Width width) noexcept { return room::layout(pipeWidth(width)).numChannels; }
    /** The LFE's limiter: how long it holds a reduction before letting go, and how slowly it then lets go at
        the least (see setParameters). */
    static constexpr double c_lfeHoldSeconds = 0.06;
    static constexpr float c_lfeReleaseMs = 300.0f;

    void prepare(double sampleRate, room::Width width); // allocates
    void reset();
    void setParameters(const MasterProcessor::Parameters& p); // both limiters (the LFE's without Inflate); once per block, audio thread

    /** In place. numChannels is pipeChannels(width) as prepared (fewer is allowed: the missing ones are skipped). */
    void process(float* const* channels, int numChannels, int numSamples);

    /** The same for every channel, every width and every parameter value (MasterProcessor's look-ahead). */
    int getLatencySamples() const { return m_mains.getLatencySamples(); }
    int getNumChannels() const { return m_numChannels; }
    int getLfeChannel() const { return m_lfe; } // -1 when the width has none
    /** A room: the mains are shaped and limited by side (see the class comment). False at Mono and Stereo. */
    bool isSided() const { return m_sided; }

    /** The main channels' limiter: deepest reduction of the last block, and since the last call (see MasterProcessor). */
    float getGainReductionDb() const { return m_mains.getGainReductionDb(); }
    float takeGainReductionDb() { return m_mains.takeGainReductionDb(); }
    /** The LFE's own limiter: deepest reduction of the last block (0 when there is no LFE). */
    float getLfeGainReductionDb() const { return m_lfe >= 0 ? m_lfeStage.getGainReductionDb() : 0.0f; }
    std::uint64_t getSafetyClampCount() const { return m_mains.getSafetyClampCount() + m_lfeStage.getSafetyClampCount(); }

private:
    MasterProcessor m_mains, m_lfeStage;
    room::Width m_width = room::Width::Stereo;
    int m_numChannels = 2;
    int m_lfe = -1;
    bool m_sided = false;
    std::array<float*, room::c_maxChannels> m_mainPtrs {};
};
