#include "OutputEq.h"

// A stub: the settings are kept and nothing is done to the sound. See OutputEq.h for what the filler provides.

bool OutputEq::isFlat(const Params& p)
{
    if (! p.on)
        return true;
    return p.lowCutHz <= 0.0f && p.highCutHz <= 0.0f && p.lowShelfDb == 0.0f && p.lowMidDb == 0.0f && p.highMidDb == 0.0f
        && p.highShelfDb == 0.0f;
}

double OutputEq::responseDb(const Params& p, double hz, double sampleRate, int band)
{
    (void) p;
    (void) hz;
    (void) sampleRate;
    (void) band;
    return 0.0;
}

void OutputEq::prepare(double sampleRate, int numChannels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels = numChannels < 1 ? 1 : (numChannels > c_maxChannels ? c_maxChannels : numChannels);
    reset();
}

void OutputEq::reset() {}

void OutputEq::process(float* const* channels, int numChannels, int numSamples)
{
    (void) channels;
    (void) numChannels;
    (void) numSamples;
}
