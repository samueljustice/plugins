#pragma once

// Bringing a flattened sound back to its own timing.
//
// Flattening by speed gives an exactly flat pitch but moves every moment of the sound: where it
// was slowed, things happen later; where it was sped up, earlier. restoreTiming() stretches and
// squeezes the flattened sound, without touching its pitch, so that every moment lands back
// where it was in the original and the length is the original's. The two steps together are a
// pitch change that keeps time: the first sets the pitch exactly, the second only moves time, so
// nothing in the pitch reading can make the result warble.
//
// Three characters (see Character). Harmonic is the stretch as it comes. Transients finds the
// hits in the sound and carries each one through at its own speed, unstretched, making the time
// up in the stretch just after it, so a hit stays sharp and exactly in place. Voice puts the
// formants back: flattening by speed moved the voice's whole spectrum with its pitch (a slowed
// voice sounds bigger), and the stretcher shifts that envelope back by the same amount while it
// stretches, leaving the pitch alone.
//
// The time stretch is the Rubber Band Library's finer engine, run as a stream so the stretch
// can be set block by block from the time map (its own offline time maps were tried and held
// timing only to within 30 ms or so). Allocates and takes a fraction of the sound's length: for
// a background thread.

#include "Flatten.h"

namespace pitchflattener::core
{

/**
 * flattened: the sound after flattening by speed. outputAtSource: its time map (see
 * Flattened::outputAtSource; entry k is where source sample k * mapStep is heard in it).
 * sourceLength: the original's length in samples, which the result will have exactly.
 * hitsInSource: for Character::Transients, the hits found in the ORIGINAL sound (findHits), where
 * they are clearest; the map says where each one is in the flattened sound.
 * Returns an empty Channels if cancelled or if there is nothing to do it with.
 */
Channels restoreTiming(const Channels& flattened, double sampleRate, const std::vector<double>& outputAtSource, int mapStep,
                       std::size_t sourceLength, Character character = Character::Harmonic,
                       const std::vector<std::size_t>& hitsInSource = {}, const Cancel& cancelled = {});

/** The hits in a sound (sample positions of sudden rises in level), for Character::Transients. */
std::vector<std::size_t> findHits(const Channels& sound, double sampleRate);

} // namespace pitchflattener::core
