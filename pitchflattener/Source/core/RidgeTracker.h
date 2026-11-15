#pragma once

// The ridge tracker: follows the strongest continuous partial of a sound through its whole
// length.
//
// Flattening only needs to know how a sound's pitch MOVES, and every partial of a sound moves
// like the rest, so the line does not have to be the fundamental: it has to stay on one partial.
// That makes this the tracker for sounds the harmonic tracker cannot read: sirens, whistles and
// feedback above its reach, and a ricochet's whine in a burst of noise.
//
// Each moment is looked at through a short window and a long one. A fast sweep (a ricochet can
// fall more than 100 semitones a second) only holds together in the short window; a steady or
// noisy tone only stands clear in the long one. The best path through all those peaks is then
// found over the whole sound: loud and continuous wins, a sweep is followed, a leap to another
// partial is not, and frames with nothing worth following are left empty.
//
// Plain C++20, no JUCE. Allocates; for a background thread.

#include <vector>

namespace pitchflattener::core
{

struct Ridge
{
    double hop = 0.005;           // seconds between frames
    std::vector<float> hz;        // 0 = nothing followed
    std::vector<float> strength;  // how the followed peak scored
};

Ridge trackRidge(const float* mono, int numSamples, double sampleRate, double loHz = 120.0, double hiHz = 6000.0);

} // namespace pitchflattener::core
