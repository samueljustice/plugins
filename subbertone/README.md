# SammyJs Subbertone

A subharmonic generator plugin that creates rich bass harmonics by generating a sine wave one octave below the fundamental frequency, applying distortion, and inverse mixing it back with the original.

## Features

- **Fundamental Detection**: Uses WORLD DIO algorithm for accurate pitch tracking
- **Subharmonic Generation**: Creates a sine wave exactly one octave below the detected fundamental
- **Harmonic Distortion**: Applies distortion to the subharmonic to generate rich overtones
- **Inverse Mixing**: Blends the distorted signal with the clean subharmonic for unique tonal character
- **Dry/Wet Control**: Blend between original and processed signals

## Building

Requires:
- CMake 3.21+
- JUCE Framework
- WORLD vocoder library
- macOS 10.13+ (for macOS builds)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Installation

Copy the plugin to your audio plugins folder:
- VST3: `/Library/Audio/Plug-Ins/VST3/`
- AU: `/Library/Audio/Plug-Ins/Components/`

## License

MIT License - See LICENSE file for details