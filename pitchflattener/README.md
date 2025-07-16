# SammyJs Pitch Flattener
A VST3/AU audio plugin that flattens pitch variations in real-time, perfect for removing sweeping Doppler effects, stabilizing warbling tones, or creating unique pitch-locked effects. When you want the energy of a recording but the pitch to be mallaeable again. Works a bit like z-noise, give it a little bit to warm up. TLDR a crazy toy that sometimes flattens pitch and mostly makes weird cool sounds

### 🎉 Latest Release v1.2.0 - [Download Here](https://github.com/samueljustice/plugins/releases/tag/pitchflattener-v1.2.0)

  • Enhanced UI with About Dialog - Added version info, update checking,
  and quick access to support email/website. Improved tooltip system with
  better scaling and help text for all parameters.

  • Streamlined Preset Management - Simplified preset system with "Reset
  All" button that now properly resets all parameters including latched
  pitch. All controls support double-click to reset to default values.

  • Better DAW Integration - All 28 audio parameters are now exposed for
  automation, including pitch algorithm selection, DIO settings, and
  advanced detection controls. Fixed interface scaling issues for
  consistent appearance across different screen sizes.



<img width="995" height="893" alt="Screenshot 2025-07-16 at 11 24 23" src="https://github.com/user-attachments/assets/11f673f5-f635-4d8f-9b40-62466e57eddb" />

## Features

- **Pitch flattening** - Tracks and neutralizes pitch movement to maintain a constant frequency, not just transposing
- **Base pitch latching** - Automatically locks onto stable pitches and flattens variations around them
- **Dual pitch detection algorithms**:
  - **WORLD DIO** (default) - FFT-based algorithm optimized for noisy field recordings and complex audio
  - **YIN** - Fast autocorrelation-based algorithm for clean, simple sources
- **High-quality pitch shifting** using the RubberBand library for artifact-free processing
- **Visual pitch meter** showing detected pitch, target pitch, and pitch deviation
- **Manual override mode** for locking to specific frequencies
- **Volume gating** to prevent false pitch detection from noise
- **Comprehensive controls** for fine-tuning detection behavior

## Building

### Prerequisites
- macOS (tested on macOS 12+)
- CMake 3.15 or higher
- Xcode Command Line Tools

### Build Instructions

1. Clone or download this repository
2. Run the build script:
   ```bash
   ./build.sh
   ```

The plugin will be automatically built and installed to:
- VST3: `/Library/Audio/Plug-Ins/VST3/`
- AU: `/Library/Audio/Plug-Ins/Components/`

## Usage

1. Load the plugin in your DAW as a VST3 or AU effect
2. Play audio through the plugin - the pitch meter will show the detected pitch
3. Choose your flattening mode:
   - **Normal Mode**: Set the **Flatten To** target frequency - all detected pitches will be shifted to this frequency
   - **Base Pitch Latch Mode**: Enable **Base Pitch Latch** - the plugin will automatically lock onto the first stable pitch and flatten all variations back to that base frequency
   - **Manual Override Mode**: Enable **Manual Override** to force flattening to a specific frequency
4. Adjust parameters as needed for your source material

## Controls

### Main Controls

- **Flatten To** (50-2000 Hz): The target frequency that all pitches will be flattened to. For example, set to 1000 Hz to make all sounds maintain a constant 1000 Hz pitch regardless of their original frequency.

- **Smoothing Time** (5-200 ms): Controls how quickly the plugin responds to pitch changes
  - 10-50 ms: Fast response for quick pitch changes (good for vibrato removal)
  - 50-100 ms: Balanced response for Doppler effects
  - 100-200 ms: Slow, smooth response for gradual pitch drifts

- **Mix** (0-100%): Blend between dry (original) and wet (pitch-flattened) signal
  - 0%: Original signal only
  - 100%: Fully processed signal
  - 50-70%: Retains some original character while flattening pitch

### Manual Override

- **Manual Override**: When enabled, ignores pitch detection and flattens everything to the Override Frequency
- **Override Freq** (50-2000 Hz): The frequency to use when Manual Override is enabled

### Base Pitch Latch

- **Base Pitch Latch**: When enabled, the plugin uses delta inversion to flatten pitch variations
  - Automatically detects and locks onto the first stable pitch it hears
  - Inverts any pitch variations to maintain the locked base frequency
  - Example: If base is 1000 Hz and pitch rises to 1100 Hz (10% up), the plugin shifts down by 10% to maintain 1000 Hz

- **Reset Base**: Click to clear the locked base pitch and wait for a new stable pitch

- **Latch Status**: Shows whether a base pitch is locked and at what frequency

- **Flatten Sensitivity** (0-50%): Sets the minimum pitch variation required before flattening occurs
  - 0%: Flattens even tiny pitch variations
  - 1-5%: Normal operation - ignores minor pitch fluctuations
  - 10-50%: Only flattens large pitch variations

- **Hard Flatten Mode**: When enabled, forces output to exactly match the latched pitch for complete pitch neutralization

### Pitch Detection

- **Algorithm** (WORLD DIO/YIN): Choose pitch detection method
  - **WORLD DIO** (default): FFT-based, handles noise well, requires buffer time
  - **YIN**: Autocorrelation-based, fast and responsive for clean signals

- **Detection Rate** (64-1024 samples): How often pitch detection runs (YIN only)
  - Lower values = more responsive but higher CPU usage
  - 64-128 samples recommended for fast tracking

- **Threshold** (0.05-0.5): Pitch detection sensitivity (YIN only)
  - Lower values detect weaker pitches but may get false readings
  - 0.10-0.15 works well for most sources

- **Min/Max Freq** (20-4000 Hz): Frequency range for pitch detection
  - Set these to bracket your expected pitch range
  - For high pitches: 600-2000 Hz
  - For vocals: 80-1000 Hz

- **Volume Gate** (-60 to 0 dB): Minimum volume level for pitch detection
  - Signal must be louder than this to trigger pitch detection
  - Prevents false detection from background noise
  - Current volume level shown below the slider

### Advanced Detection

#### YIN Algorithm Controls
- **Hold Time** (0-2000 ms): Minimum time before accepting a new pitch
  - Prevents rapid jumping between pitches
  - 200-500 ms recommended for stable tracking

- **Jump Limit** (10-500 Hz): Maximum allowed pitch change between detections
  - Prevents octave errors and false jumps
  - 50-200 Hz recommended

- **Confidence** (0-1): Minimum confidence level to accept a pitch
  - Higher values = more stable but may miss quick changes
  - 0.35-0.5 recommended for most sources

- **Smoothing** (0-0.99): Additional smoothing for detected pitch values
  - 0 = no smoothing
  - 0.8-0.9 = reduces pitch jitter

#### WORLD DIO Algorithm Controls
- **DIO Speed** (1-12): Processing speed vs accuracy tradeoff
  - 1 = Fastest (best for real-time)
  - 12 = Most accurate (higher latency)

- **Frame Period** (1-15 ms): How often DIO analyzes pitch
  - Lower = more responsive tracking
  - 5 ms default for good balance

- **Allowed Range** (0.1-0.5): F0 contour smoothing threshold
  - Lower = stricter pitch tracking
  - Higher = allows more variation

- **Channels** (1-8): Frequency analysis resolution
  - More channels = better accuracy, more CPU
  - 2-4 recommended

- **Buffer Time** (0.05-1.5 s): Analysis buffer duration
  - Larger = better accuracy but more latency
  - You'll hear silence for this duration when switching to DIO

#### Detection Filters (Both Algorithms)
- **Detection HP** (20-2000 Hz): High-pass filter for pitch detection signal
  - Filters out low frequencies before pitch detection
  - Does NOT affect the audio output
  - 600 Hz default cuts out rumble

- **Detection LP** (1000-20000 Hz): Low-pass filter for pitch detection signal
  - Filters out high frequencies before pitch detection
  - Does NOT affect the audio output
  - 6000 Hz default reduces noise and harmonics

## Visual Feedback

- **Pitch Meter**: Shows the currently detected frequency and note
- **Target Indicator**: Green line shows the target frequency
- **Deviation Meter**: Shows how far the detected pitch is from the target
- **Flattening To**: Displays the current target frequency the plugin is flattening to
- **Status**: Shows processing state and detected frequency

## Tips

### For Experimental Effects
- Try Manual Override with automated frequency changes
- Use very slow smoothing (150-200 ms) for gliding effects
- Mix at 50% to create harmonies with the original pitch
- Use Base Pitch Latch with high sensitivity (20-50%) for selective flattening

## Troubleshooting

- **No pitch detection**: Check that input volume is above the Volume Gate threshold
- **Erratic pitch jumping**: Increase Hold Time and Jump Limit, decrease Threshold
- **Slow response**: Decrease Detection Rate and Smoothing Time
- **CPU usage high**: Increase Detection Rate value (processes less frequently)

## Developer

Created by Samuel Justice  
Website: [www.sweetjusticesound.com](https://www.sweetjusticesound.com)

## License

This plugin uses the JUCE framework, RubberBand library, and WORLD vocoder system. Please refer to their respective licenses for terms of use.

## Technical Details

### How Pitch Flattening Works

This plugin uses advanced pitch tracking and neutralization to flatten pitch variations:

1. **Pitch Detection**: 
   - **WORLD DIO** (default): Uses FFT-based analysis with multiple frequency channels to accurately track pitch even in noisy recordings
   - **YIN**: Uses autocorrelation to quickly identify the fundamental frequency in cleaner signals

2. **Movement Tracking**: The plugin tracks pitch movement (delta) between audio blocks to identify sweeps and variations

3. **Slope Detection**: Calculates the rate of pitch change (Hz/ms) to differentiate between:
   - Static tones that need no correction
   - Moving pitches that need flattening
   - Rapid sweeps that need aggressive correction

4. **Dynamic Correction**: Instead of simply transposing to a fixed pitch, the plugin:
   - Detects when pitch is rising or falling
   - Applies inverse correction to neutralize the movement
   - Maintains the original tonal character while removing pitch variation

5. **Result**: The output maintains a constant frequency regardless of input pitch variations, effectively removing Doppler effects and pitch sweeps

### WORLD DIO Algorithm

The WORLD DIO (Distributed Inline-filter Operation) algorithm is a sophisticated FFT-based pitch detection method that:
- Analyzes multiple frequency channels simultaneously
- Uses distributed processing to handle noise and interference
- Provides stable pitch tracking even with complex harmonic content
- Requires a buffer period for initial analysis but then provides continuous real-time tracking

## Version History

- v1.1.0 - Added FFT pitch tracking
- v1.0.0 - Initial release
