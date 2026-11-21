#include "SwirlFx.h"

#include <cmath>

// A stub: nothing is done to the sound. See SwirlFx.h for what the filler provides.

float SwirlFx::mixFor(float knob)
{
    return 0.5f * std::pow(sj::clamp(knob, 0.0f, 1.0f), 0.7f);
}

void SwirlFx::prepare(double sampleRate)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_drySamples = static_cast<int>(std::lround(c_dryMs * 0.001 * m_sampleRate));
    reset();
}

void SwirlFx::reset()
{
    m_hand = 0.0f;
    m_recentre = false;
    m_startPhase = 0.0f;
}

void SwirlFx::processTape(const Tape& settings, float knob, const Lanes& lanes, int frames)
{
    (void) settings;
    (void) knob;
    (void) lanes;
    (void) frames;
}

void SwirlFx::processDoubler(const Doubler& settings, float knob, const Lanes& lanes, int frames)
{
    (void) settings;
    (void) knob;
    (void) lanes;
    (void) frames;
}

void SwirlFx::delaySides(float* left, float* right, int frames)
{
    (void) left;
    (void) right;
    (void) frames;
}
