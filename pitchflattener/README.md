# SammyJs Pitch Flattener

A VST3/AU plugin that removes pitch movement from a sound while keeping its energy and character. Use it to take the Doppler sweep out of a pass-by, iron the wail out of a siren, take the vibrato out of a voice or instrument, or lock a warbling tone to one note so the pitch is malleable again.

### 🎉 Latest Release v2.0.1 - [Download Here](https://github.com/samueljustice/plugins/releases/tag/pitchflattener-v2.0.1)

![Pitch Flattener 2.0](docs/screenshot.png)

> **2.0 is a rewrite of the detection and correction engine.** 1.x reacted late and fought itself, so it mostly made "weird cool sounds". 2.0 looks ahead, picks a pitch detector built for your kind of source, and lines the correction up with the audio sample-accurately. See [What changed in 2.0](#what-changed-in-20).

## Designed for offline rendering

Pitch Flattener runs as a normal realtime plugin, so it loads in any VST3/AU host or rack. It is built for hosts that render offline, such as the **Soundminer DSP rack**, a DAW bounce, or AudioSuite-style processing:

- It looks **about 2.6 s ahead**, so it knows where the pitch is going before it shifts anything.
- It reports that delay to the host. With delay compensation, which Soundminer and every major DAW have, the output lines up with the input exactly.
- The latency is the same whatever the settings, so changing Source Type never shifts your audio in time.
- It is not meant for live monitoring.

## Source Type: pick what you're flattening

Each Source Type chooses a pitch detector and tuned defaults. Every setting can still be changed afterwards.

| Source Type | Detector | Use it for |
|---|---|---|
| **Vocal / Speech** | pYIN (probabilistic YIN) | Voices, singing, vocal-like animal calls. Ignores breaths and consonants and follows the voice smoothly. Formant preservation is on by default. |
| **Instrument** | MPM (McLeod Pitch Method) | Solo melodic instruments: vibrato, warble, tape wow. |
| **Siren** | Spectral Peak (reassigned STFT) | Wail, yelp, hi-lo and electronic sweeps, even in traffic noise. Uses the Tight window to follow fast sweeps. |
| **Whistle / Tone** | Spectral Peak, long windows | Whistles, feedback, test tones, theremin. Very stable on near-pure tones. |
| **Engine / Doppler** | Harmonic Sum (subharmonic summation) | Engines, pass-bys, props, motors and trains. Follows the whole harmonic series, so it does not jump octaves and copes with a weak or missing fundamental. |
| **Drone / Hum** | Harmonic Sum, slow tracking | Hums, generators and drones down to 25 Hz. |
| **Noisy Field** | Harmonic Sum, strict voicing | Rough field recordings with lots of broadband noise. |
| **Custom** | Any | Your own settings. Choosing Custom doesn't change anything. |

### How the detectors work

- **pYIN** (Mauch & Dixon 2014) runs YIN's cumulative-mean-normalised difference function, computed with FFTs. It weighs every dip in that function against a Beta-distributed prior over YIN thresholds, so each frame produces several pitch candidates with probabilities instead of one guess.
- **MPM** (McLeod & Wyvill 2005) uses the normalised squared difference function. It picks the first "key maximum" within a fraction of the highest one, which avoids octave errors on bright instruments.
- **Spectral Peak** uses Gaussian-windowed FFTs at several lengths: short windows for fast sweeps, long ones for steady tones. Each peak is refined with quadratic interpolation and time-frequency reassignment, which gives the instantaneous frequency even mid-sweep. Its salience is measured against the local noise floor. A harmonic-support check makes the fundamental win over its overtones.
- **Harmonic Sum** (subharmonic summation, Hermes 1988) works on a whitened, log-frequency spectrum and adds up the energy at every candidate's harmonics. An odd/even-harmonic test settles octave ambiguities, for example when the fundamental is missing.

### Tracking, using lookahead

Every detector feeds the same tracker, which can see well into the future of the signal:

1. **Fixed-lag Viterbi** over the candidates, plus an "unpitched" state. Jumps faster than the Tracking Speed and octave jumps are expensive, so the best continuous path wins. This removes octave errors and glitches.
2. **Octave repair and median filter** against the local median pitch.
3. **Gap fill:** short dropouts are bridged. Before a sound starts, the correction is set up in advance, so the first syllable or rev is already flattened.
4. **Centred Gaussian smoothing** in cents. It is zero-phase, so the curve doesn't lag.
5. **Target and correction:** the shift needed to move the track to the target, limited by Max Shift.

The shifter is **Rubber Band Library R3** (the "finer" engine) in high-consistency mode. Each ~3 ms chunk is shifted by the correction measured at the same moment in the audio.

## Controls

### Target

- **Target Mode**
  - **Latch** (default) locks to the opening pitch of each sound: the median of its first ~300 ms, read with lookahead. It re-latches after a second of silence. **Re-latch** forces a new latch.
  - **Fixed** flattens everything to **Target Pitch**.
  - **Auto** removes movement faster than **Auto Window** and keeps the slower overall contour. Use it to take out vibrato or warble without changing the melody.
- **Flatten Amount** (0–100 %): how much of the pitch deviation is removed. 50 % halves a Doppler sweep.
- **Manual Override**: ignores detection and treats the input as if it were at **Override Frequency**, which applies a constant shift.

### Tracking

- **Sensitivity**: how readily a sound counts as pitched. Raise it for quiet or breathy sources; lower it for noisy recordings.
- **Tracking Speed** (oct/s): the fastest pitch movement the tracker will believe. Lower it to reject glitches on slow sources such as engines and drones. Raise it for yelps.
- **Smoothing** (ms): smooths the detected pitch curve before correcting it.
- **Gap Fill** (ms): the longest dropout that is bridged before the correction relaxes.
- **Min / Max Frequency**: the pitch search range. Bracket your source; a tighter range is more robust.

### Input

- **Gate** (dB): frames quieter than this count as unpitched.
- **Detection High-pass / Low-pass**: filters that apply to the analysis only, to keep rumble or hiss out of detection. They do not change the sound.

### Output

- **Quality**
  - **Smooth** uses R3's standard window and gives the best sound.
  - **Tight** uses the short window and follows very fast sweeps more closely.
- **Formant Preserve**: keeps a voice's character natural when it is shifted. Leave it off for machines and sirens.
- **Max Shift**: a safety limit on how far the audio may be shifted.
- **Mix**: blends in the time-aligned dry signal. Keep it at 100 % for flattening; partial mixes can sound phasey.

### Presets

The preset bar in the header works like Subbertone's. Click the preset name to open the preset menu:

- **FACTORY**: one preset per Source Type, with its tuned detector, range, tracking and target settings.
- **USER**: your own presets, in the **User Presets** submenu.
- **Save As...**, **Delete** (the current user preset, moved to the trash), **Reset to Defaults** (every parameter back to its default) and **Show Presets Folder**.

The **<** and **>** arrows step through the factory presets and then your own, in menu order. **SAVE** stores the current settings as a user preset, and asks before replacing one with the same name. A tick marks the current preset. The name reads "No preset" after Reset to Defaults, or when the preset it came from has been deleted.

User presets are plain `.xml` files, named after the preset, so they are easy to back up, share or rename:

- macOS: `~/Library/SammyJs/PitchFlattener/Presets/User`
- Windows: `%APPDATA%\SammyJs\PitchFlattener\Presets\User`

The current preset is saved with your session. Presets from 1.x still load.

## Measured performance

`tests/FlattenTests.cpp` renders synthetic sources through the exact plugin DSP, at random host block sizes, and measures how flat the output is with an independent pitch meter:

| Test signal | Input pitch range | Output error (median / 90th pct) |
|---|---|---|
| Siren wail, 550–1350 Hz sweep | 1520 cents | 2 / 4 cents |
| Siren yelp, 1 octave triangle at 3 Hz (6 oct/s) | 1060 cents | 18 / 40 cents |
| Voice, 50-cent vibrato + 3-semitone drift, formants, gaps | 350 cents | 7 / 11 cents |
| Engine pass-by, 25 m/s Doppler | 250 cents | 1 / 5 cents |
| Engine with missing fundamental, Doppler | 200 cents | 5 / 11 cents |
| Engine pass-by in heavy noise (Noisy Field) | 300 cents | 2 / 5 cents |
| Instrument, 35-cent vibrato at 6 Hz | 70 cents | 8 / 11 cents |
| Whistle, ±100-cent drift | 200 cents | 0.7 / 1.3 cents |

The suite runs in CI on every release build. To run it locally:

```bash
cmake -B build-tests -S . -DPITCHFLATTENER_BUILD_TESTS=ON -DPITCHFLATTENER_COPY_AFTER_BUILD=OFF
cmake --build build-tests --target PitchFlattenerTests
./build-tests/PitchFlattenerTests_artefacts/Release/PitchFlattenerTests --block 0 --wav /tmp/pf
```

`--wav` writes each test's input and output so you can listen to them.

## Tips

- **Doppler pass-by:** choose Engine / Doppler and Latch. If the approach is quieter than the pass, use **Re-latch** or Fixed with the engine's idle pitch.
- **Siren:** choose Siren and Fixed at the pitch you want. For a wail, Smooth quality sounds cleaner; for a yelp, keep Tight.
- **Vibrato only:** use Auto mode with an Auto Window of 300–600 ms.
- **Half-flattened, more natural result:** set Flatten Amount to 50–70 %.
- **The output jumps or warbles:** narrow Min/Max Frequency around the source, lower Tracking Speed, or try another Source Type.

## What changed in 2.0

- **New detectors:** pYIN, MPM, Spectral Peak and Harmonic Sum, chosen by Source Type. The O(N²) YIN and WORLD DIO are gone. DIO re-analysed a 0.5 s buffer on every audio block and read its answer from the least reliable end of it.
- **Lookahead tracking:** a Viterbi tracker with octave repair, gap fill and zero-phase smoothing, replacing the block-rate exponential smoothing and the 5 %-change threshold.
- **Sample-accurate correction:** the shifter is fed audio whose pitch has already been decided, and each chunk gets the correction measured at that point. In 1.x the correction was applied to audio from a different moment.
- **Fixed in the shifter:** the 1.x RubberBand feeder pushed overlapping, duplicated audio into the stretcher whenever Lookahead was above 1x, which is where most of the old artefacts came from. It also padded underruns by repeating samples. Both are fixed. The engine is now R3 with proper start padding and delay compensation.
- **Reported latency and tail:** the dry/wet mix is time-aligned, and bypass stays time-aligned too.
- **New UI** in the SammyJs design language, with a scrolling pitch display showing raw detection, the final track, the target and the corrected output.
- **Removed parameters:** Detection Rate, Threshold, Hold Time, Jump Limit, Confidence, the DIO settings, Lookahead, Hard Flatten, and RubberBand's R2-only Transients, Phase and Window options. Their jobs are now done by the tracker. 1.x sessions still load: matching settings are kept and the algorithm choice is mapped to a Custom detector.

## Building

Prerequisites: CMake 3.15+, a C++17 compiler (Xcode on macOS, Visual Studio 2022 on Windows), and the SDK folder next to this one containing JUCE 8.0.8 and Rubber Band 4.0.0 built as a static library (see `.github/workflows/build-release_pitchflattener.yml`).

```bash
./build.sh
```

This builds and installs:
- VST3: `/Library/Audio/Plug-Ins/VST3/`
- AU: `/Library/Audio/Plug-Ins/Components/`

## Credits and licences

Created by Samuel Justice, [www.sweetjusticesound.com](https://www.sweetjusticesound.com).

- [JUCE](https://juce.com) framework.
- [Rubber Band Library](https://breakfastquay.com/rubberband/) for pitch shifting (GPL / commercial).
- UI font: [Azeret Mono](https://github.com/displaay/Azeret), SIL Open Font License 1.1.
- Algorithms:
  - YIN, de Cheveigné & Kawahara (2002)
  - pYIN, Mauch & Dixon (2014)
  - MPM, McLeod & Wyvill (2005)
  - Subharmonic summation, Hermes (1988)
  - Time-frequency reassignment, Auger & Flandrin (1995)

## Version history

- **v2.0.1**: Subbertone 1.2.0 styling for every menu, submenu, dialog, tooltip and text field; preset bar and menu on Subbertone's layout; user presets tracked by file; CI stapling retries.
- v2.0.0: new detection and correction engine, Source Types, lookahead tracking, sample-accurate latency, new UI, test suite.
- v1.2.5: help text and About button scaling fixes, FFT visualiser.
- v1.1.0: added FFT pitch tracking.
- v1.0.0: initial release.
