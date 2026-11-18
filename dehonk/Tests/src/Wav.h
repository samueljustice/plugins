// Minimal WAV reader and writer for the harness.
// Reads PCM 8/16/24/32-bit and IEEE float 32/64-bit, plain or WAVE_FORMAT_EXTENSIBLE.
#pragma once

#include "Audio.h"

#include <string>

namespace dh
{

struct WavFormat
{
    int bitsPerSample = 32;
    bool isFloat = true;
    bool extensible = false;
};

/** Returns false and fills err on failure. */
bool readWav (const std::string& path, Audio& out, std::string& err, WavFormat* formatOut = nullptr);

/** Writes 16, 24 or 32-bit PCM, or 32-bit float. Float output is never clipped;
    integer output is clipped to full scale. */
bool writeWav (const std::string& path, const Audio& a, const WavFormat& format, std::string& err);

inline bool writeWavFloat (const std::string& path, const Audio& a, std::string& err)
{
    return writeWav (path, a, WavFormat { 32, true, false }, err);
}

} // namespace dh
