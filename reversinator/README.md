# SammyJs Reversinator
A VST3/AU audio plugin that provides real-time audio reversal effects with grain-based overlap-add processing, inspired by the classic Backwards Machine plugin. Create smooth reverse effects, experimental soundscapes, and unique time-based audio manipulations without clicks or pops.

### 🎉 Latest Release v1.0.0 - [Download Here](https://github.com/samueljustice/plugins/releases/tag/reversinator-v1.0.0)

<img width="748" height="377" alt="Screenshot 2025-07-18 at 16 45 05" src="https://github.com/user-attachments/assets/b23d33bf-f564-4057-8600-baf14f932dc4" />

## Features

- **Grain-based overlap-add architecture** - Click-free reversal using phase-continuous windowing
- **Three reverse modes**:
  - **Reverse Playback** - Continuous reverse effect with smooth grain transitions
  - **Forward Backwards** - Palindromic playback (forward then reverse from midpoint)
  - **Reverse Repeat** - Plays snippets backwards twice, with vibrato on the second repeat
- **Adjustable window time** (30ms - 5s) - Control the size of each reverse grain
- **Envelope control** (10-100ms) - Fine-tune fade in/out for each grain
- **Enhanced feedback system** - More extreme feedback effects with soft saturation
- **Independent wet/dry mix** - Blend reversed and original signals precisely
- **Crossfade control** - Smooth transitions in Forward Backwards mode
- **Zero-latency processing** - Real-time performance with minimal CPU usage
- **Full DAW automation** - All parameters can be automated

## Building

### Prerequisites
- macOS (tested on macOS 12+)
- CMake 3.15 or higher
- Xcode Command Line Tools

### Build Instructions

1. Clone or download this repository
2. Navigate to the reversinator folder and run:
   ```bash
   mkdir build
   cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   cmake --build . --config Release
   ```

The plugin will be built to:
- VST3: `build/Reversinator_artefacts/Release/VST3/`
- AU: `build/Reversinator_artefacts/Release/AU/`

To install:
- Copy the `.vst3` file to `~/Library/Audio/Plug-Ins/VST3/`
- Copy the `.component` file to `~/Library/Audio/Plug-Ins/Components/`

## Usage

1. Load the plugin in your DAW as a VST3 or AU effect
2. Click the **Enable** button to activate the effect
3. Select your desired **Effect Mode**:
   - **Reverse Playback**: Classic continuous reverse effect
   - **Forward Backwards**: Plays forward to midpoint, then reverses back
   - **Reverse Repeat**: Plays backwards twice with vibrato on second repeat
4. Adjust parameters to taste

## Controls

### Main Controls

- **Reverser Enable**: Master on/off switch for the effect
  - Click to toggle between enabled/disabled states
  - When disabled, audio passes through unprocessed

- **Effect Mode**: Choose between three reverse algorithms
  - **Reverse Playback**: Continuously reverses audio in overlapping grains
  - **Forward Backwards**: Creates a palindromic effect - forward then reverse
  - **Reverse Repeat**: Plays audio backwards twice - second time with vibrato effect

### Time & Window Controls

- **Window Time** (30ms - 5s): Size of each reverse grain
  - 30-100ms: Granular, metallic textures
  - 100-500ms: Classic reverse effects
  - 500ms-5s: Long, sweeping reverses
  - Directly affects the character of the reversal

- **Envelope** (10-100ms): Fade in/out time for each grain
  - Lower values (10-30ms): Sharper transitions, more definition
  - Higher values (50-100ms): Smoother blending between grains
  - Automatically limited to 50% of window time to prevent overlap issues

- **Crossfade** (0-100%): Blend amount between forward/reverse sections
  - Only visible in Forward Backwards mode
  - 0%: Hard switch between directions
  - 100%: Maximum crossfade for seamless transitions

### Feedback & Mix

- **Feedback** (0-100%): Amount of reversed signal fed back into the input
  - 0%: No feedback
  - 50%: Moderate echo/delay effects
  - 100%: Extreme feedback with soft saturation for wild effects
  - Different scaling per mode for optimal results

- **Wet Mix** (0-100%): Level of the reversed/processed signal
  - 0%: No effect signal
  - 100%: Only reversed signal
  - Use with Dry Mix for parallel processing

- **Dry Mix** (0-100%): Level of the original unprocessed signal
  - 0%: No original signal (full effect)
  - 100%: Full original signal
  - Combine with Wet Mix for blended effects

## Tips & Creative Uses

### For Smooth Reverses
- Use longer window times (500ms+) with moderate envelope (50ms)
- Keep feedback below 50% for clarity
- Balance wet/dry mix around 70/30 for natural effect

### For Glitch Effects
- Short window times (30-100ms) with minimal envelope (10-20ms)
- High feedback (80-100%) for chaos
- 100% wet mix for full transformation

### For Rhythmic Patterns
- Sync window time to your tempo (e.g., 250ms = 16th note at 120 BPM)
- Use Reverse Repeat mode for triplet-like patterns
- Moderate feedback (40-60%) maintains rhythm while adding texture

### For Ambient Soundscapes
- Maximum window time (5s) with high envelope (80-100ms)
- Low feedback (20-40%) for subtle evolution
- Equal wet/dry mix (50/50) for spacious effect

### Production Techniques
- **Reverse reverb**: Place before a reverb for classic effect
- **Vocal effects**: Short windows on vocals for robotic textures
- **Transition tool**: Automate the Enable button for dramatic sweeps
- **Sidechain**: Use on a bus with rhythmic material for pumping effects

## Technical Details

### Grain-Based Overlap-Add System

Reversinator uses an advanced grain-based architecture for click-free operation:

1. **Grain Buffering**: Audio is captured into overlapping grains (4 concurrent grains per channel)
2. **50% Overlap**: Grains overlap by 50% ensuring continuous coverage
3. **Hann Windowing**: Each grain uses a Hann window for smooth fade in/out
4. **Phase Continuity**: Grains maintain phase alignment preventing discontinuities
5. **Output Accumulation**: Overlapping grains are summed in an accumulation buffer

### Processing Modes

**Reverse Playback**
- Continuously captures and reverses audio grains
- Each grain plays backwards while maintaining smooth transitions
- Feedback is applied before grain capture for echo effects

**Forward Backwards**
- First half of grain plays forward (0% to 50%)
- Second half plays reverse from midpoint back to start
- Creates a palindromic "there and back again" effect
- Includes grain spawn offset variation to prevent feedback loops

**Reverse Repeat**
- Plays each grain backwards twice
- Second repeat includes subtle vibrato modulation (0.5% depth)
- Creates unique doubled reverse effect

### Feedback System

The feedback path includes:
- Mode-specific gain scaling (0.5x, 0.3x, 0.5x)
- Soft saturation above 90% for harmonic richness
- Tanh limiting to prevent hard clipping
- Separate feedback sample per channel

## Troubleshooting

- **No sound**: Ensure Reverser is enabled and Wet Mix is above 0%
- **Clicks or pops**: Should not occur with grain system - try increasing Envelope time
- **Distortion**: Lower Feedback or reduce input level
- **CPU spikes**: Increase buffer size in your DAW settings

## Developer

Created by Samuel Justice  
Website: [www.sweetjusticesound.com](https://www.sweetjusticesound.com)  
Email: sam@sweetjusticesound.com

## License

This plugin uses the JUCE framework. Please refer to JUCE's license for terms of use.

## Version History

- v1.0.0 - Initial release with grain-based overlap-add system, three reverse modes, and modern UI
