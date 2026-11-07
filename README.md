# Sweet Justice Audio Plugins

Audio plug-ins and tools by Samuel Justice / [Sweet Justice Sound](https://www.sweetjusticesound.com), most of them built with [JUCE](https://juce.com). Each project lives in its own folder with its own README, and is released on its own from this repository's [releases page](https://github.com/samueljustice/plugins/releases).

## Projects

| Project | What it is | Formats | Latest release |
|---|---|---|---|
| [Swooshbuckler](swooshbuckler/README.md) | Whoosh-making rig | App (macOS, Windows), VST3, AU | Not released yet |
| [Subbertone](subbertone/README.md) | Subharmonic generator | VST3, AU | [v1.2.0](https://github.com/samueljustice/plugins/releases/tag/subbertone-v1.2.0) |
| [Pitch Flattener](pitchflattener/README.md) | Lookahead pitch flattening (Doppler, sirens, vibrato) | VST3, AU | [v2.0.1](https://github.com/samueljustice/plugins/releases/tag/pitchflattener-v2.0.1) |
| [Reversinator](reversinator/README.md) | Real-time reverser, recreating Backwards Machine | VST3, AU | [v2.0.0](https://github.com/samueljustice/plugins/releases/tag/reversinator-v2.0.0) |
| Stretch Armstrong | Real-time varispeed and time stretch | VST3, AU | Not released yet |
| [PTSL Beat Tool](PTSL_beattool/README.md) | Beat markers for Pro Tools | App (macOS) | Not released yet |

### Swooshbuckler
A whoosh-making rig. Drop in up to ten sounds and it rides the faders across them, making doppler passes, swells, hits and tails, while a tape records every one and skips the silence. Two effect buses, a hit gate, a spectral morph between the buses, and modulators you can drag onto any knob. Runs as a standalone app on macOS and Windows, or as a plug-in.

- Location: `/swooshbuckler/`
- [Project README](swooshbuckler/README.md)

### Subbertone
A subharmonic generator. Tracks the pitch of your bass, engine or drone and adds a clean sub one octave below, with a 16-bar harmonic editor, six saturation types, a resonant, envelope-swept filter and a mastering stage (Inflate and a brickwall limiter), for anything from pure sub reinforcement or bass that survives a phone speaker to a growling octave-down bass. Comes with 32 factory presets, including engines.

- Location: `/subbertone/`
- [Project README](subbertone/README.md)
- [Latest release](https://github.com/samueljustice/plugins/releases/tag/subbertone-v1.2.0)

### Pitch Flattener
Removes pitch movement: the Doppler sweep from a pass-by, the wail from a siren, the vibrato from a voice. Pick a Source Type and it uses a pitch detector built for that sound, looks ahead to track the pitch, and flattens it sample-accurately. Built for offline-rendering hosts such as the Soundminer DSP rack.

- Location: `/pitchflattener/`
- [Project README](pitchflattener/README.md)
- [Latest release](https://github.com/samueljustice/plugins/releases/tag/pitchflattener-v2.0.1)

### Reversinator
A real-time reverser: a faithful, modernised recreation of Backwards Machine's three effects (Reverse Playback, Forward Backwards and Reverse Repeat), with a live display of the read heads sweeping backwards through your audio.

- Location: `/reversinator/`
- [Project README](reversinator/README.md)
- [Latest release](https://github.com/samueljustice/plugins/releases/tag/reversinator-v2.0.0)

### Stretch Armstrong
Stretches audio in real time, either varispeed (the pitch follows the speed, like tape) or time stretch (the pitch stays put), using RubberBand. Work in progress.

- Location: `/stretcharmstrong/`

### PTSL Beat Tool
A macOS app that tracks the beats of an audio file and places Pro Tools markers on them, through the Pro Tools Scripting Library (PTSL). Work in progress.

- Location: `/PTSL_beattool/`
- [Project README](PTSL_beattool/README.md)

## Building

Every project builds with CMake and expects its dependencies in `SDK/` at the root of this repository (not committed):

| Dependency | Used by |
|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) 8.0.8, at `SDK/JUCE` | Swooshbuckler, Subbertone, Pitch Flattener, Reversinator, Stretch Armstrong |
| [RubberBand](https://breakfastquay.com/rubberband/) 4.0.0, at `SDK/rubberband-4.0.0` | Pitch Flattener, Stretch Armstrong |
| Beat-and-Tempo-Tracking, at `SDK/Beat-and-Tempo-Tracking`, and [py-ptsl](https://pypi.org/project/py-ptsl/) | PTSL Beat Tool |

Then, from a project's folder:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

See each project's README for its own options.

## Releases

Each project has a GitHub Actions workflow in `.github/workflows/` that builds, signs and notarises it for macOS (and builds it for Windows where supported), then publishes a release. Push a tag named after the project to start one, for example:

```bash
git tag swooshbuckler-v1.0.0
git push origin swooshbuckler-v1.0.0
```

The tag prefixes are `swooshbuckler-v`, `subbertone-v`, `pitchflattener-v`, `reversinator-v`, `stretcharmstrong-v` and `ptsl-beattool-v`.

## License

Each project and dependency has its own license. See the LICENSE file in each folder.

## Developer

Created by Samuel Justice
Website: [www.sweetjusticesound.com](https://www.sweetjusticesound.com)
