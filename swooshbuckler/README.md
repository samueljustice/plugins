# Swooshbuckler

A whoosh-making rig. Drop in up to ten sounds, press play, and Swooshbuckler rides the faders across them so the layers swell, pass by and fall away. You get a steady stream of doppler passes, swells, hits and tails. Arm the tape and it records every one into a single file, skipping the silence. All that's left is to pick out the good ones.

Swooshbuckler is a standalone app for macOS and Windows, and also a VST3 plug-in (both platforms) and an Audio Unit (macOS).

![Swooshbuckler](docs/screenshot.png)

## Download

Get the latest version from the [releases page](https://github.com/samueljustice/plugins/releases?q=swooshbuckler):

| File | What's in it |
|---|---|
| `Swooshbuckler-x.y.z-macOS-Installer.pkg` | The app, the VST3 and the AU. Click **Customize** to choose which. |
| `Swooshbuckler-x.y.z-macOS-App.dmg` | Just the app. Drag it to Applications. |
| `Swooshbuckler-x.y.z-Windows-Installer.exe` | The app and the VST3. |
| `...-VST3.zip`, `...-AU.zip`, `...-Windows-App.zip` | Each part on its own, for installing by hand. |

macOS builds are universal (Apple silicon and Intel), signed and notarised, and need macOS 11 or later. Windows builds need 64-bit Windows 10 or later.

## Quick start

1. **Drop sounds into Tracks**, or click the drop zone to browse. Anything works: cloth, air, engines, animals, metal, voices. Each file gets its own looping track, and Swooshbuckler listens to it, names it and works out what kind of sound it is.
2. **Press PLAY.** With **AUTO RIDE** on (it is by default), every track's fader moves by itself at its own speed. When the layers swell together you get a whoosh.
3. **Shape it** with Energy, Rate and Chaos, or try a preset or **RANDOMISE**.
4. **Arm the tape** (ARM). It records whenever there's sound and skips the silence. Click again to stop and close the file.
5. Missed a good one? **LAST 60s** saves the last minute, even if the tape wasn't armed.

Hold **SWELL**, the space bar or any MIDI note to push every fader up at once.

Every control has a tooltip: hover over it to see what it does.

## How it works

```
 Tracks ─┬─> Bus A (tonal, crackle, rumble, mixed) ─┐
         ├─> Bus B (beds, air)  <── ducked by A ────┼─> Output ─> tape
         └─> Hits (impacts) <── gate opens on A + B ┘
```

- **Tracks**:
  - Each track loops from its start marker (the flag). Click or drag on a waveform to move the marker.
  - With **SKIP SILENCE** on (the default), the gaps between sounds are skipped, so a recording full of pauses becomes a steady source.
- **Sound types**:
  - Each track is sorted into one of seven types, which decide its bus and how it plays: Energy bed, Tonal content, Impact, Air / swish, Crackle, Rumble or Mixed content.
  - Click the type badge to change a wrong guess.
- **Bus A and Bus B** each run a moving-source chain: doppler passes, stereo travel, pitch bend, low end, swirl, an envelope filter and drive. Bus A ducks and darkens Bus B so they don't pile up.
- **Hits**:
  - Impact tracks only play through the hit gate. The gate opens at the loudest moments of the two buses, so the impacts land on the biggest swells.
  - **SYNC HITS** fires all impact tracks on one shared beat, so their transients line up into one big hit. The waveforms slide into line to show it.
- **Output**: echoes and reverb, then loudness and a limiter. The tape records what you hear.

Each section's title bar has a small waveform of the last 3 seconds of what it sends on, so you can see the ducking and the gate working.

## Controls

Double-click any knob to reset it.

### Ride
| Control | Range | What it does |
|---|---|---|
| **Swell** | button | Hold to push every fader up (space bar or any MIDI note also works). |
| **Auto ride** | on / off | The faders move by themselves. Off: they stay where you set them. |
| **Energy** | 0–100% | How far the ride pulls the faders: gentle movement to big swells out of silence. |
| **Rate** | 0–100% | How fast the faders move, and so how long each whoosh and doppler pass lasts. |
| **Fill** | 0–100% | Long echoes (0.5, 1 and 2 s) that fill the gaps in gappy sounds. |
| **Chaos** | 0–100% | How unpredictable the rides and passes are. |
| **Pitch** | ±24 semitones | Plays every track higher or lower. |

### Bus A and Bus B
| Control | What it does |
|---|---|
| **Doppler** | How dramatic each pass is: how close it comes, how much it swells and brightens. |
| **Width** | How far each pass travels across the stereo field. |
| **Bend** | How much the pitch dives as a pass goes by. |
| **Low end** | A sub octave, low overtones and a lift at the loudest moments. |
| **Swirl** | A phaser, then a flanger that sweeps with each pass. |
| **Filter** | An envelope filter that opens as the bus gets louder. |
| **Drive** | Saturation, from warmth to growl. |
| **Ducked** *(B only)* | How much bus A pushes bus B down. |
| **Filtered** *(B only)* | How much bus A darkens bus B with a low-pass filter. |
| **TAKE TURNS** *(B only)* | Only the louder bus speaks and the other ducks under it. Off: A always ducks B. |

### Hits
| Control | Range | What it does |
|---|---|---|
| **Gate** | Peaks / Level / Jumps / Open | What opens the gate: the loudest moments (default), any level over Sensitivity, sudden rises, or always open. |
| **Duck** | Volume / Darken / Mids | How the ducked buses get out of the way: turned down (default), low-passed, or only their middle (250 Hz to 4 kHz) turned down. |
| **Sensitivity** | 0–100% | How easily the gate opens. |
| **Boost** | 0 to +18 dB | How much the hits jump up when the gate opens. |
| **Release** | 60 ms to 1.2 s | How long the hits tail out. |
| **Hold** | 0–500 ms | How long the gate stays open once it opens. |
| **Floor** | Closed, or −40 to −3 dB | How far the gate closes between hits. |
| **Duck** | 0–100% | How far the hits push buses A and B down. |
| **Return** | Fast, or 20 ms to 1 s | How fast ducked buses come back up. |
| **SYNC HITS** | on / off | All impact tracks fire on one shared beat. |

### Output
| Control | Range | What it does |
|---|---|---|
| **Space** | 0–100% | Echoes, reverb and width. |
| **Inflate** | 0–100% | Louder and denser without new peaks. |
| **Push** | 0 to +18 dB | Drive into the limiter. |
| **Ceiling** | −12 to 0 dB | Nothing gets out louder than this. |

### Morph
Click **MORPH** over the sea to show the morph pad, and switch it on with **ON**.

- The morph blends bus A into bus B spectrally, so the two sounds trade tone and pitch rather than just crossfading.
- Drag the dot, or press ▶ to let it wander along a shape.
- There are three modes:
  - **Blend** morphs the tone.
  - **Glide** slides the pitches.
  - **Cross** plays one through the other, like a vocoder.

### Modulation
Four modulators sit along the bottom. Drag a modulator's **jack** onto any knob to wire it; the morph pad and track faders are the exceptions.

Each modulator can be one of four types:
- **LFO**: a repeating wave: Sine, Triangle, Ramp, Square or Steps.
- **Random**: a smooth random wander.
- **Follower**: follows the loudness of bus A, bus B, the hits or the output. Pick which in the box under the scope.
- **Macro**: one knob that turns every knob it's wired to.

To work with a wire's plug on a knob:
- Drag it up or down to set the depth (hold Shift for fine control).
- Double-click it to set +50%.
- Right-click it (or Alt-click) to invert it or pull it out.

**CABLES** shows or hides the cables. Wires only offset the knobs, so your DAW automation stays clean.

### Tracks
| Control | What it does |
|---|---|
| **Fader** | The track's level. With auto ride on, it's how high the ride can take it. |
| **Bus tag** | Where the track plays: BUS A, BUS B or HITS. It follows the sound type. |
| **Type badge** | The sound type it was sorted into, which also picks its bus. Click it to change it. |
| **SKIP SILENCE** | On: only the sounds play. Off: the whole file loops, silence and all. |
| **M / S / x** | Mute, solo, remove (undo with Cmd+Z, or Ctrl+Z on Windows). |
| Double-click the name | Rename the track. |

### Settings
Where recordings go, and their file type (WAV, AIFF, FLAC), bit depth (16, 24 or 32-bit float), sample rate and channels. You can also arm the tape every time Swooshbuckler opens. In the app, this is also where you choose the audio device.

### Presets
Factory presets: Steady energy, Jet passes, Monster swells, Gentle breeze, Fast and furious, Magic dust, Heavy hits. Choosing one glides every control to its new setting while it plays. Your tracks, their levels and the output section stay as they are.

The version and build are in the **About** panel: click the title or the version number under it.

## Building

### Prerequisites
- CMake 3.21 or later
- Xcode command line tools (macOS) or Visual Studio 2022 (Windows)
- JUCE 8.0.8 at `../SDK/JUCE`, relative to this folder

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

This builds the app, the VST3 and the AU (macOS) into `build/Swooshbuckler_artefacts/Release/`, along with three offline test programs: `SwooshbucklerRig`, `SwooshbucklerCargo` and `SwooshbucklerClassify`. Pass `-DSWOOSHBUCKLER_TESTS=OFF` to skip the tests, or `-DSWOOSHBUCKLER_FORMATS="Standalone;VST3"` to choose the formats.

### Version

The version lives in one place, `SWOOSHBUCKLER_VERSION` in [CMakeLists.txt](CMakeLists.txt). Release builds override it from the tag.

### Icon

The icon is drawn in code, in [tools/IconRender.cpp](tools/IconRender.cpp). After changing it, regenerate `Resources/icon.png` and `Resources/icon.ico` and commit them:

```bash
cmake -B build-icon -DSWOOSHBUCKLER_TOOLS=ON -DSWOOSHBUCKLER_TESTS=OFF -DSWOOSHBUCKLER_FORMATS=Standalone
cmake --build build-icon --target SwooshbucklerIcon
./build-icon/SwooshbucklerIcon_artefacts/SwooshbucklerIcon Resources/icon.png
```

## Releasing

Releases are built by [build-release_swooshbuckler.yml](../.github/workflows/build-release_swooshbuckler.yml). Push a tag and it builds, tests, signs and publishes everything:

```bash
git tag swooshbuckler-v1.0.0
git push origin swooshbuckler-v1.0.0
```

The number in the tag becomes the version everywhere: the About panel, the bundles and both installers.

- **macOS**: uses the same signing secrets as the other plug-ins. The p12 must hold both the Developer ID Application and the Developer ID Installer certificates. `APPLE_ID`, `APPLE_APP_PASSWORD` and `APPLE_TEAM_ID` are for notarisation.
- **Windows**: add `WINDOWS_CERTIFICATE_PFX_BASE64` and `WINDOWS_CERTIFICATE_PASSWORD` (a code-signing certificate as base64 .pfx) to sign the app, the VST3 and the installer. Without them the Windows build is unsigned.

You can also run the workflow by hand (Actions → Build and Release Swooshbuckler → Run workflow) to get the files without publishing a release.

## Credits

- Fonts: [Pirata One](https://fonts.google.com/specimen/Pirata+One) by Rodrigo Fuenzalida and Nicolas Massi, and [IBM Plex Sans Condensed](https://github.com/IBM/plex) by IBM, both under the SIL Open Font License 1.1.
- Built with [JUCE](https://juce.com).
- VST is a registered trademark of Steinberg Media Technologies GmbH.

## License

MIT License. See [LICENSE](LICENSE).
