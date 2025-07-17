# SammyJs Reversinator

A VST3/AU audio plugin that provides real-time audio reversal effects, inspired by the classic Backwards Machine plugin. Create psychedelic reverse effects, experimental soundscapes, and unique time-based audio manipulations.

### 🎉 Latest Release v1.0.0 - [Download Here](https://github.com/samueljustice/plugins/releases/tag/reversinator-v1.0.0)

<img width="600" alt="Reversinator Plugin" src="https://github.com/user-attachments/assets/reversinator-screenshot.png" />

## Features

- **Three Reverse Modes**:
  - **Reverse Playback** - Continuous reverse effect with smooth transitions
  - **Forward Backwards** - Alternating forward and reverse playback with crossfading
  - **Reverse Repeat** - Double playback with vibrato effect on the reversed portion
  
- **Adjustable Window Time** (30ms - 2s) - Control the size of the reverse buffer
- **Feedback Control** - Add echo-like effects to the reversed signal
- **Wet/Dry Mix Controls** - Blend reversed and original signals independently
- **Crossfade Control** - Smooth transitions in Forward Backwards mode
- **Low Latency Processing** - Optimized for real-time performance
- **Click-Free Operation** - Advanced anti-click algorithms for smooth audio

## Building

### Prerequisites
- macOS (tested on macOS 12+) or Windows 10/11
- CMake 3.15 or higher
- Xcode Command Line Tools (macOS) or Visual Studio 2022 (Windows)

### Build Instructions

#### macOS
```bash
cd reversinator
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

#### Windows
```cmd
cd reversinator
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

The plugin will be built to:
- VST3: `build/Reversinator_artefacts/Release/VST3/`
- AU (macOS only): `build/Reversinator_artefacts/Release/AU/`

## Installation

### macOS
- Copy the `.vst3` file to `/Library/Audio/Plug-Ins/VST3/`
- Copy the `.component` file to `/Library/Audio/Plug-Ins/Components/`

### Windows
- Copy the `.vst3` folder to `C:\Program Files\Common Files\VST3\`

## Usage

1. Load the plugin in your DAW as a VST3 or AU effect
2. Enable the **Reverser** button to activate the effect
3. Select your desired **Effect Mode**:
   - **Reverse Playback**: Classic tape-reverse effect
   - **Forward Backwards**: Ping-pong style forward/reverse
   - **Reverse Repeat**: Adds vibrato to the reversed signal
4. Adjust the **Window Time** to control the reverse buffer size
5. Set **Wet Mix** and **Dry Mix** levels to taste

## Controls

### Main Controls

- **Reverser** - Master on/off switch for the effect

- **Effect Mode** - Choose between three reverse algorithms:
  - **Reverse Playback**: Continuously reverses audio in chunks
  - **Forward Backwards**: Alternates between forward and reverse playback
  - **Reverse Repeat**: Plays forward then reverses with vibrato

### Time & Modulation

- **Window Time** (30ms - 2s): Size of the reverse window
  - Smaller values (30-200ms): Granular, glitchy effects
  - Medium values (200-500ms): Classic reverse sounds
  - Larger values (500ms-2s): Long, sweeping reverses

- **Crossfade** (0-100%): Smoothness of transitions (Forward Backwards mode only)
  - 0%: Sharp transitions
  - 50%: Balanced blend
  - 100%: Maximum smoothing

### Feedback & Mix

- **Feedback Depth** (0-100%): Amount of reversed signal fed back
  - Creates echo and delay-like effects
  - Higher values create more complex textures

- **Wet Mix** (0-100%): Level of the reversed signal
  - 0%: No reversed signal
  - 100%: Full reversed signal

- **Dry Mix** (0-100%): Level of the original signal
  - 0%: No original signal
  - 100%: Full original signal

## Tips & Tricks

### Creative Uses

1. **Reverse Reverb**: Place before a reverb for classic reverse reverb effects
2. **Rhythmic Effects**: Sync window time to tempo for rhythmic reversing
3. **Ambient Textures**: Long window times with high feedback for soundscapes
4. **Glitch Effects**: Short window times (30-100ms) for stuttering effects
5. **Transition Effects**: Automate the Reverser button for dramatic transitions

### Recommended Settings

**Subtle Reverse Effect**
- Mode: Reverse Playback
- Window: 500ms
- Feedback: 20%
- Wet: 50%, Dry: 50%

**Psychedelic Sweep**
- Mode: Forward Backwards
- Window: 1s
- Crossfade: 70%
- Feedback: 40%
- Wet: 80%, Dry: 20%

**Glitch Stutter**
- Mode: Reverse Repeat
- Window: 50ms
- Feedback: 60%
- Wet: 100%, Dry: 0%

## Troubleshooting

- **Clicking or popping**: Window time is automatically limited to 30ms minimum to prevent clicks
- **No sound**: Check that Reverser is enabled and Wet Mix is above 0%
- **CPU usage**: Larger window times use more memory but not necessarily more CPU

## Technical Details

### Anti-Click Technology

Reversinator uses several techniques to ensure click-free operation:
- Dynamic fade lengths that scale with window size
- Hann window fading for smooth transitions
- Circular buffer with 3x overallocation to prevent wrap errors
- Anti-denormal noise injection
- Minimum 30ms window time enforcement

### Buffer Management

The plugin uses efficient circular buffers for each channel with:
- Real-time safe memory allocation
- Lock-free audio processing
- Automatic buffer resizing when parameters change
- Separate buffers for each effect mode

## Developer

Created by Samuel Justice  
Website: [www.sweetjusticesound.com](https://www.sweetjusticesound.com)  
Email: sam@sweetjusticesound.com

## License

Copyright (c) 2025 Samuel Justice. All rights reserved.

This plugin is released under the MIT License. See LICENSE file for details.

Uses the JUCE framework. Please refer to JUCE's license for terms of use.

## Version History

- v1.0.0 - Initial release with three reverse modes and anti-click improvements