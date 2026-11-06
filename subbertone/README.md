# SammyJs Subbertone

A VST3/AU subharmonic generator. Subbertone tracks the pitch of your bass, synthesises a clean sine one octave below it, and can push that sub through saturation and a resonant, envelope-swept filter for anything from a pure sub reinforcement to a growling, talking octave-down bass.

### 🎉 Latest Release v1.1.0 - [Download Here](https://github.com/samueljustice/plugins/releases/tag/subbertone-v1.1.0)

https://github.com/user-attachments/assets/fccdc149-fdf8-4f65-bdc2-e9ddc40fdcec

![Subbertone](docs/screenshot.jpg)

## Features

- **Pitch-tracked sub, one octave down**: a phase-continuous sine that follows your playing, glides smoothly between notes and fades with the input. All processing is per sample, so it is click-free at any buffer size.
- **Six saturation types**, each with a distinct harmonic recipe:
  - **Tape**: smooth, odd harmonics
  - **Valve**: warm, strong 2nd harmonic at every drive level
  - **Console**: harder knee, brighter
  - **Transformer**: soft, mostly odd with a touch of 2nd
  - **Rectifier**: even harmonics only, so every partial lines up with the input note's own harmonics (the cleanest "octave" sound)
  - **Wavefolder**: harmonics sweep upward as drive rises
- **Driver-style resonant filter** on the harmonics: lowpass, bandpass or notch, with resonance driven into saturation and an envelope follower that sweeps the cutoff on every note.
- **Constant-loudness drive**: the Drive knob crossfades the clean sub into its harmonics at matched level, so turning it up changes the tone, not the volume.
- **3D OpenGL visualiser** showing input, harmonics and sub output.
- **Zero latency**, full DAW automation.

## How it works

1. The input is low-passed to the bass band and analysed with a streaming YIN pitch detector, decimated to about 11 kHz.
2. A sine is generated at half the detected pitch. Its level follows the bass band's RMS and a voicing gate.
3. The sine is shaped by the chosen saturation curve. The DC and fundamental components are then subtracted exactly, leaving only the harmonics. Because the input to the shaper is always a pure sine, each curve acts as a fixed harmonic recipe.
4. The harmonics go through the resonant multimode filter. Its resonant peak is pushed into a soft clipper, and the envelope follower sweeps its cutoff.
5. The clean sub and the filtered harmonics are blended by Drive, low-passed by Hi Cut, then mixed with the dry signal.

A note on harmonics: odd harmonics of the sub land a fifth and a third above the input note, while even harmonics land exactly on the input's own harmonic series. Use Rectifier or Valve when you want the sub to stay consonant with the bass; use Tape, Console or Wavefolder for more edge.

## Controls

### Detect
| Control | Range | Description |
|---|---|---|
| **Threshold** | -60 to -20 dB | Input level needed before the sub is generated. Lower catches quieter notes. |
| **Max Pitch** | 100 to 800 Hz | Highest input fundamental that is tracked. Notes above this are ignored. |

### Drive
| Control | Range | Description |
|---|---|---|
| **Type** | 6 types | Saturation character (see above). |
| **Drive** | 0 to 100% | Crossfades the clean sub into its saturated harmonics while increasing drive. |
| **Hi Cut** | 20 Hz to 20 kHz | Low-pass on the whole sub signal. |

### Filter
| Control | Range | Description |
|---|---|---|
| **Mode** | Lowpass / Bandpass / Notch | Filter response for the harmonics. |
| **Cutoff** | 20 Hz to 20 kHz | Filter cutoff. The envelope sweeps up or down from here. |
| **Reso** | 0 to 100% | Resonance (up to Q 20). The peak is driven into saturation, from subtle overtones to full overdrive. |
| **Env** | -4 to +4 oct | How far each note's envelope sweeps the cutoff. Negative sweeps down. |
| **Release** | 5 ms to 2 s | How quickly the filter envelope falls back after each note. |

### Output
| Control | Range | Description |
|---|---|---|
| **Mix** | 0 to 100% | Blend between the dry input and the generated sub. |
| **Output** | -24 to +24 dB | Final output level. |

Double-click any knob to reset it to its default. Click a row in the visualiser to show or hide that trace.

## Quick start

- **Clean sub reinforcement:** Drive 0%, Mix around 30–50%.
- **Warm octave-down bass:** Valve or Rectifier, Drive 40–60%, Cutoff around 800 Hz.
- **Talking growl:** Tape or Console, Drive 70%+, Filter Bandpass, Reso 50–70%, Env +2 to +3 oct, Release 100–200 ms.

## Building

### Prerequisites
- CMake 3.21 or higher
- Xcode Command Line Tools (macOS) or Visual Studio 2022 (Windows)
- JUCE, expected at `../SDK/JUCE` relative to this folder

### Build instructions

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The plugin is built to:
- VST3: `build/Subbertone_artefacts/Release/VST3/`
- AU: `build/Subbertone_artefacts/Release/AU/`

To also build a standalone app (handy for UI work), configure with:

```bash
cmake -B build-standalone -DCMAKE_BUILD_TYPE=Release -DSUBBERTONE_FORMATS=Standalone
```

### Installation

- Copy the `.vst3` bundle to `~/Library/Audio/Plug-Ins/VST3/`
- Copy the `.component` bundle to `~/Library/Audio/Plug-Ins/Components/`

## Credits

- UI font: [Azeret Mono](https://github.com/displaay/Azeret), SIL Open Font License 1.1.
- The filter section is inspired by the distortion-filter concept of effects such as Native Instruments Driver. No code from it is used.

## License

MIT License. See [LICENSE](LICENSE) for details.
