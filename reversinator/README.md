# SammyJs Reversinator

A VST3/AU real-time reverser for macOS and Windows. Reversinator is a faithful, modernised recreation of **Backwards Machine** (The Sound Guy, 2007), the classic plugin that "reverses continuous audio in real time". Its three effects, Reverse Playback, Forward Backwards and Reverse Repeat, were rebuilt from a study of the original's signal flow, so they behave and sound like it. They run inside a new interface that shows the reverser at work.

### 🎉 Latest Release v2.0.0 - [Download Here](https://github.com/samueljustice/plugins/releases/tag/reversinator-v2.0.0)

![Reversinator](docs/screenshot.png)

## Features

- **The three Backwards Machine effects**, with the original's sweeps, windows, vibrato, feedback routing, gain staging, parameter ranges and control tapers.
- **Checked against the real thing.** The vendor's demo clip, run through Reversinator, nulls against the vendor's own Backwards Machine renders of it. Correlation is 0.983 for Reverse Playback, 0.992 for Forward Backwards and 0.998 for Reverse Repeat; what remains is MP3 coding noise.
- **Click-free everywhere.** REVERSER switches with a 10 ms crossfade, as on the original. TIME changes crossfade between the old and new read positions. Switching effects crossfades straight into the new one, because every effect shares the same input history. Wet, dry and feedback changes are smoothed. All of this is independent of the host's buffer size.
- **Live snippet display**: a scrolling view of the recent input with each read head drawn as it sweeps backwards through it, so you can see what each effect is doing.
- **Modern interface** in the spirit of the original: mode lamps, the big REVERSER button, an hourglass TIME control and a feedback loop. The window is resizable from 75% to 200%.
- Mono or stereo at any sample rate. The effect itself is a delay, so no latency is reported to the host.

## The effects

Every effect is built from the same voice. A delay line's delay time is swept by a sawtooth from 0 up to 2 × TIME over each TIME period, so the read head runs backwards through the input at normal speed: each period plays the previous TIME seconds of audio in reverse. A `0.5 + 0.5·sin` window fades each snippet in and out, and is silent at the exact moment the sweep jumps back. That is why there are no clicks at the snippet boundaries.

| Effect | What you hear |
|---|---|
| **Reverse Playback** | Two voices half a period apart, so their windows always add up to one. You hear a continuous, overlapping backwards stream. |
| **Forward Backwards** | One backwards voice crossfaded against the dry input on the opposite window. The sound smoothly alternates between playing forwards and backwards, once per TIME period. |
| **Reverse Repeat** | One backwards voice, plus a copy of it played again half a period later with a slight 5 Hz vibrato. Each snippet is heard backwards twice. |

**Feedback** sends each voice's output back into its own delay line, so repeats are re-reversed into cascades that alternate direction. As on the original, each delay line's input is halved and the output doubled, so FEEDBACK DEPTH is the true loop gain. At the 75% maximum, the feedback dies away rather than running away.

## Controls

| Control | Range | Default | Description |
|---|---|---|---|
| **Mode** | 3 effects | Reverse Playback | Reverse Playback, Forward Backwards or Reverse Repeat. |
| **REVERSER** | Reverse / Forward | Reverse | Whether you hear the effect or the original forwards audio, at the wet level. The effect keeps running either way, so switching back lands in a live effect. |
| **TIME** | 0.5 – 4 s | 2.00 s | The length of each snippet that is played backwards. 2 seconds tends to sound best. Same square-law taper as the original. |
| **FEEDBACK DEPTH** | 0 – 75 % | 0 % | How much of the effect is fed back into itself. Usually not needed. |
| **WET MIX** | 0 – 100 % | 100 % | Level of the effect. Squared law: 50% is -12 dB. |
| **DRY MIX** | 0 – 100 % | 0 % | Mixes in the original input. Usually 0%, but useful for hearing forwards and backwards together. |

## Tips

- **Matching the tempo.** The effect often sounds just as interesting out of time, but to sync it use `TIME = N × 60 / BPM` with N = 1, 2, 4 and so on. For example, at 120 BPM, N = 4 gives 2 seconds, which is one bar of 4/4.
- **Reverse reverb.** Put Reversinator before a reverb, then print and reverse the result, or simply use Reverse Playback on a reverb return.
- **Transitions.** Automate REVERSER to flip a phrase backwards and return cleanly on the downbeat.
- **Ambience.** Forward Backwards with a long TIME and some DRY MIX turns a loop into a slowly breathing palindrome.
- **Cascades.** Reverse Playback with 40–75% feedback builds layered, alternating-direction echoes.

## Upgrading from 1.x

Version 2 rebuilds the effect to match Backwards Machine, so some settings changed:

- **Envelope** and **Crossfade** are gone. The original's windows are fixed, and the new engine uses them.
- **TIME** is now 0.5–4 s (was 30 ms–5 s), and **FEEDBACK DEPTH** is now 0–75% (was 0–100%), as on the original.
- **REVERSER** now defaults to on.
- Sessions saved with 1.x still open. Values outside the new ranges are clamped, and the removed controls are ignored.
- macOS builds now run on macOS 11 and later. Previous builds required the macOS version they were built on.

## Building

### Prerequisites

- CMake 3.22 or higher
- macOS: Xcode or the Command Line Tools. Windows: Visual Studio 2022.
- JUCE 8.0.8 in `../SDK/JUCE`, relative to this folder

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The plugins are written to `build/Reversinator_artefacts/Release/VST3/` and `.../AU/`. To install, copy them to `~/Library/Audio/Plug-Ins/VST3/` and `~/Library/Audio/Plug-Ins/Components/`.

Options:

- `-DREVERSINATOR_FORMATS="AU;VST3;Standalone"` also builds a standalone app.
- `-DREVERSINATOR_BUILD_TESTS=ON` builds `ReversinatorTests`, an offline test runner:
  - `ReversinatorTests selftest` checks stability across sample rates, layouts and block sizes, block-size independence, click-free automation and mode switching, and state save/restore.
  - `ReversinatorTests render in.wav out.wav --mode 1 --time 1.5` renders a file through the effect.
  - `ReversinatorTests screenshot out.png` renders the editor to an image.
  - `ReversinatorTests hostcheck "build/.../SammyJs Reversinator.vst3"` loads and exercises the built plugin.

## Developer

Created by Samuel Justice
Website: [www.sweetjusticesound.com](https://www.sweetjusticesound.com)
Email: sam@sweetjusticesound.com

Backwards Machine is a trademark of The Sound Guy, Inc. Reversinator is an independent recreation and is not affiliated with or endorsed by them.

## License

See [LICENSE](LICENSE). The plugin uses the JUCE framework; see JUCE's license for its terms.

## Version History

- **v2.0.0**: Rebuilt to match Backwards Machine: the original's three effects, ranges, tapers, defaults and gain staging. New interface with a live snippet display. Click-free TIME, mode and REVERSER changes. Fixes the macOS minimum-version problem, a crash risk in the update check, and the build break.
- **v1.0.0**: Initial release with a grain-based overlap-add engine, three reverse modes and a resizable UI.
