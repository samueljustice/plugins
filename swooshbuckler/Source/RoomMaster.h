#pragma once

#include "MasterProcessor.h"
#include "Room.h"

#include <array>

/**
 * The output stage at any room width: MasterProcessor (Inflate, Push, the true-peak Ceiling limiter) on every
 * channel, with two limiters instead of one.
 *
 *   main channels (every speaker but the LFE) --> MasterProcessor, one gain linked across all of them
 *   LFE                                        --> its own MasterProcessor, the same settings, its own gain
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

    void prepare(double sampleRate, room::Width width); // allocates
    void reset();
    void setParameters(const MasterProcessor::Parameters& p); // both limiters; once per block, audio thread

    /** In place. numChannels is pipeChannels(width) as prepared (fewer is allowed: the missing ones are skipped). */
    void process(float* const* channels, int numChannels, int numSamples);

    /** The same for every channel, every width and every parameter value (MasterProcessor's look-ahead). */
    int getLatencySamples() const { return m_mains.getLatencySamples(); }
    int getNumChannels() const { return m_numChannels; }
    int getLfeChannel() const { return m_lfe; } // -1 when the width has none

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
    std::array<float*, room::c_maxChannels> m_mainPtrs {};
};
