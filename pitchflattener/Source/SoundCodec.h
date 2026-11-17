#pragma once

// The learned sound, compressed for the plug-in's state: FLAC at 24 bits, the learned rate and
// channel count, behind a small header.
//
// 24 bits is what the matching needs: the copy differs from the float original by quantisation
// at about -144 dBFS, far under the -30 dB the residual test allows, so the original playing
// through still matches the copy exactly where it should. A sound louder than full scale (float
// audio can be) is scaled down for the FLAC and back up on decoding.
//
// Header (little endian): "PFS1", float64 sample rate, float32 scale (multiply the decoded audio
// by it), uint32 channels, uint64 frames; then the FLAC stream.
//
// Off the audio thread only (it allocates and can take a while for a long sound).

#include "core/Flatten.h"

#include <vector>

namespace pitchflattener::codec
{

/** Empty when it cannot be encoded. */
std::vector<char> encode(const core::Channels& sound, double sampleRate);

/** False when the bytes are not a sound this codec made. */
bool decode(const std::vector<char>& bytes, core::Channels& sound, double& sampleRate);

} // namespace pitchflattener::codec
