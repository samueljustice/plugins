<div align="center">

# Sammy J's Plugins

**Toys I've made for sound designers.**

<!-- get-them-all:start (written by .github/workflows/build-release_bundle.yml; edits here are replaced) -->
### ⬇️ Get them all (last built 10 Nov 2026, 12:34 UTC)

[![Get them all for macOS](https://img.shields.io/badge/Get_them_all-macOS-000000?style=for-the-badge&logo=apple&logoColor=white)](https://github.com/samueljustice/plugins/releases/download/bundle-v2026.11.10.72/SammyJ-Plugins-2026-11-10-1234-macOS.dmg)
[![Get them all for Windows](https://img.shields.io/badge/Get_them_all-Windows-0078D6?style=for-the-badge&logo=windows&logoColor=white)](https://github.com/samueljustice/plugins/releases/download/bundle-v2026.11.10.72/SammyJ-Plugins-2026-11-10-1234-Windows-Installer.exe)

<sub>One installer with the newest version of every plug-in: Swooshbuckler 1.4.1 · Sound Advice 1.0.12 · Subbertone 1.3.1 · Pitch Flattener 2.3.0 · Reversinator 2.0.3 · Roarschach 1.0.0. The apps, the VST3s and (on macOS) the AUs. It removes older copies first, so nothing shows up twice in your host. To remove them again: on macOS, run Uninstall Sammy J's Plugins from the same disk image; on Windows, use Settings &gt; Apps.<br>
Want just one? Each plug-in's own download is below, or browse all <a href="https://github.com/samueljustice/plugins/releases">releases</a>.</sub>
<!-- get-them-all:end -->

![VST3](https://img.shields.io/badge/VST3-222?style=flat-square)
![AU](https://img.shields.io/badge/AU-222?style=flat-square)
![Standalone](https://img.shields.io/badge/Standalone-222?style=flat-square)
[![Built with JUCE](https://img.shields.io/badge/built%20with-JUCE-8DC63F?style=flat-square)](https://juce.com)
![Downloads](https://img.shields.io/github/downloads/samueljustice/plugins/total?style=flat-square&color=d00a35)

<br>

| [<img src="swooshbuckler/docs/screenshot.png" width="400" alt="Swooshbuckler">](#-swooshbuckler) | [<img src="subbertone/docs/screenshot.jpg" width="400" alt="Subbertone">](#-subbertone) |
|:---:|:---:|
| **[Swooshbuckler](#-swooshbuckler)**<br>Generates whooshes from up to ten sounds | **[Subbertone](#-subbertone)**<br>Adds a pitch-tracked sub and harmonics to bass |
| [<img src="pitchflattener/docs/screenshot.png" width="400" alt="Pitch Flattener">](#-pitch-flattener) | [<img src="reversinator/docs/screenshot.png" width="400" alt="Reversinator">](#-reversinator) |
| **[Pitch Flattener](#-pitch-flattener)**<br>Removes pitch movement from Doppler passes, sirens and vibrato | **[Reversinator](#-reversinator)**<br>Real-time reverse effects, recreating Backwards Machine |

[<img src="roarschach/docs/screenshot.jpg" width="812" alt="Roarschach">](#-roarschach)<br>
**[Roarschach](#-roarschach)**<br>Turn a voice into a creature

[<img src="soundadvice/docs/screenshot.png" width="812" alt="Sound Advice">](#-sound-advice)<br>
**[Sound Advice](#-sound-advice)**<br>Advice from film, TV and game sound professionals and other creative people

</div>

---

## ⚔️ Swooshbuckler

> Generates whooshes from up to ten sounds, and records them.

<p align="center">
  <img src="swooshbuckler/docs/screenshot.png" alt="Swooshbuckler" width="820">
</p>

Drop in up to ten sounds (cloth, air, engines, creatures, metal) and Swooshbuckler works out what each one is and sends it to Bus A, Bus B or the Hits. Then it rides every fader by itself, so the layers swell, fly past as Doppler passes and fall away. Hold **Swell** (the space bar, a MIDI note or a click on the sea) to push it all harder. Arm the tape and it records every whoosh into one file and skips the silence. Missed one? **LAST 60s** grabs the last minute anyway.

- 🎚️ **Auto ride** across up to ten looping tracks, each fader on its own path, shaped by Energy, Rate, Fill, Chaos and Pitch
- 🧠 **Smart loading** finds the separate sounds in each file, skips the gaps and guesses what each one is from the audio and the file name
- 🎛️ **Per-track pitch** in semitones and cents, with **VARI** on for tape-style varispeed or off to change the pitch and keep the speed
- 🌀 **Two Doppler buses** with Width, Bend, Low End, Swirl, an envelope filter and Drive. Bus A can duck and darken Bus B, or they **Take Turns**
- 🧬 **Spectral morph pad** blends A into B (Blend, Glide or vocoder-style Cross), and can wander along a Lorenz loop, a rose or a figure eight
- 💥 **Hit gate** fires your impacts at the biggest swells, and **Sync Hits** stacks their transients into one huge hit
- 🔌 **Four drag-a-cable modulators** (LFO, Random, Follower, Macro), plus **7 factory presets** and a RANDOMISE button

<details>
<summary>More screenshots</summary>
<br>
<p align="center">
  <img src="swooshbuckler/docs/images/morph-pad.png" alt="Swooshbuckler spectral morph pad" width="400">
  <img src="swooshbuckler/docs/images/cables.png" alt="Swooshbuckler modulation cables" width="400">
</p>
<p align="center">
  <img src="swooshbuckler/docs/images/tracks.png" alt="Swooshbuckler tracks" width="820">
</p>
</details>

**App** (macOS, Windows) · **VST3** · **AU** &nbsp;&nbsp;
[![Download Swooshbuckler](https://img.shields.io/badge/download-v1.4.1-d00a35?style=flat-square)](https://github.com/samueljustice/plugins/releases/tag/swooshbuckler-v1.4.1) &nbsp;
[Read the manual →](swooshbuckler/README.md)

---

## 🔊 Subbertone

> Adds a pitch-tracked sub an octave down, plus harmonics, saturation, filtering and limiting.

<p align="center">
  <img src="subbertone/docs/screenshot.jpg" alt="Subbertone" width="820">
</p>

Subbertone follows the pitch of your bass, engine or drone and adds a clean sub one octave below. That's only the start: build its harmonics bar by bar, from pure sub reinforcement, to the trick that makes a bass audible on speakers that can't play a sub, to a growling, talking octave-down monster. Saturation, a resonant filter and a mastering stage finish it off, with about 2 ms of latency that your host compensates for.

- 🧲 **Phase Align** lines the new harmonics up with your bass's own, so they add up instead of cancelling. Notes thin out far less, repeat hits land at the same level, and it adds no latency
- 🎹 **16-bar harmonic editor** with a live meter on every bar: H1 is the sub, H2 to H16 its harmonics. Draw them, pick a pattern, or switch the **sub off** and keep only the harmonics for small speakers
- 🔥 **Six saturation types** (Tape, Valve, Console, Transformer, Rectifier, Wavefolder) into a **resonant, envelope-swept filter** that makes the bass talk
- 🧱 **Master stage**: Inflate with a Curve control, then a look-ahead brickwall limiter with a ceiling and a gain-reduction meter
- 🎯 **Steady tracking**: octave jumps are confirmed before the sub follows, dropouts are held, Glide tames wandering engines, and a Lo Cut keeps a very low sub from eating your headroom
- 🚗 **32 factory presets** in five groups, including engines from inline-4 to V12, V-twin and 2-stroke

<details>
<summary>More screenshots</summary>
<br>
<p align="center">
  <img src="subbertone/docs/harmonics.png" alt="Subbertone harmonic editor with Phase Align on" width="787">
</p>
<p align="center">
  <img src="subbertone/docs/output-master.jpg" alt="Subbertone OUTPUT and MASTER sections" width="626">
</p>
<p align="center">
  <img src="subbertone/docs/presets.png" alt="Subbertone presets menu" width="400">
  <img src="subbertone/docs/patterns.png" alt="Subbertone patterns menu" width="400">
</p>
</details>

**VST3** (macOS, Windows) · **AU** (macOS) &nbsp;&nbsp;
[![Download Subbertone](https://img.shields.io/badge/download-v1.3.1-d00a35?style=flat-square)](https://github.com/samueljustice/plugins/releases/tag/subbertone-v1.3.1) &nbsp;
[Read the manual →](subbertone/README.md)

---

## 🚨 Pitch Flattener

> Removes pitch movement from Doppler passes, sirens and vibrato.

<p align="center">
  <img src="pitchflattener/docs/screenshot.png" alt="Pitch Flattener" width="680">
</p>

Pitch Flattener takes the pitch movement out of a sound and keeps its energy and character: the Doppler sweep from a pass-by, the wail from a siren, the vibrato from a voice. Or lock a warbling tone to one note. Pick a Source Type and it loads a pitch detector built for that kind of sound, looks about 2.6 seconds ahead, and lines the correction up with the audio sample-accurately. Made for offline rendering: the Soundminer DSP rack, a DAW bounce or AudioSuite-style processing.

- 🎯 **Eight Source Types** (Vocal, Instrument, Siren, Whistle, Engine / Doppler, Drone, Noisy Field, Custom), each with its own tuned detector; Siren runs two at once, so fast yelps and sirens in traffic both track
- 🔭 **Lookahead tracking** fixes octave errors, bridges gaps and smooths with zero lag, so the very first syllable or rev is already flat
- 🎚️ **Latch, Fixed or Auto**: hold each sound at the pitch it starts on, flatten everything to one note (type `A4`), or take out only the vibrato and keep the melody
- 📈 **Live pitch display**: eight seconds of detection, track, target and corrected output over a spectrogram
- 🚨 **Wobble warning**: a red warning when the output isn't coming out flat, suggesting the Source Type that would do better
- 📏 **Measured, not guessed**: the test suite flattens a siren wail to within 2 cents and an engine pass-by to within 1 cent
- 💾 **Amount** for half-flattened results, **Formant** for natural-sounding voices, and presets you can save

<details>
<summary>More screenshots</summary>
<br>
<p align="center">
  <img src="pitchflattener/docs/display-engine.png" alt="Pitch Flattener display on an engine pass-by" width="400">
  <img src="pitchflattener/docs/display-voice.png" alt="Pitch Flattener display on a voice" width="400">
</p>
</details>

**VST3** (macOS, Windows) · **AU** (macOS) &nbsp;&nbsp;
[![Download Pitch Flattener](https://img.shields.io/badge/download-v2.3.0-d00a35?style=flat-square)](https://github.com/samueljustice/plugins/releases/tag/pitchflattener-v2.3.0) &nbsp;
[Read the manual →](pitchflattener/README.md)

---

## ⏪ Reversinator

> Real-time reverse effects, recreating Backwards Machine's three modes.

<p align="center">
  <img src="reversinator/docs/screenshot.png" alt="Reversinator" width="760">
</p>

A real-time reverser that faithfully recreates Backwards Machine's three effects, **Reverse Playback**, **Forward Backwards** and **Reverse Repeat**, rebuilt from a study of the original's signal flow. It lives in a new resizable interface with a live display of each read head sweeping backwards through your audio, and it fixes the original's glitches, so every change is click-free.

- 🎯 **Checked against the real thing**: nulls against the original's own renders at 0.999+ correlation
- ⏪ **Three classic effects**, with the original's sweeps, windows, vibrato and gain staging
- 👀 **Live snippet display** that draws every read head as it sweeps backwards through your input
- 🤫 **Click-free everywhere**: TIME sweeps, mode switches, the big REVERSER button and host bypass all crossfade, at any buffer size
- ⏳ **Hourglass TIME** from 0.5 to 4 s, plus feedback that builds cascades of echoes, alternating direction
- 🔍 **Resizable from 75% to 200%**, mono or stereo, any sample rate, zero latency

<details>
<summary>More screenshots</summary>
<br>
<p align="center">
  <img src="reversinator/docs/guide/display-reverse-playback.png" alt="Reversinator display in Reverse Playback" width="400">
  <img src="reversinator/docs/guide/display-reverse-repeat.png" alt="Reversinator display in Reverse Repeat" width="400">
</p>
</details>

**VST3** (macOS, Windows) · **AU** (macOS) &nbsp;&nbsp;
[![Download Reversinator](https://img.shields.io/badge/download-v2.0.3-d00a35?style=flat-square)](https://github.com/samueljustice/plugins/releases/tag/reversinator-v2.0.3) &nbsp;
[Read the manual →](reversinator/README.md)

---

## 👹 Roarschach

> *Say the line. The monster says it back.*

<p align="center">
  <img src="roarschach/docs/screenshot.jpg" alt="Roarschach" width="820">
</p>

Roarschach turns a spoken or shouted performance into a big, liquid, gravelly creature voice that still reads as words. Fourteen modules (PITCH, CLEAN, DE-ESS, DETAIL, VOCODER, RING, SHIMMER, GROWL, SWIRL, ROAR, ENSEMBLE, TAME, SUB and OVERTONE) run between an input stage and a level-matched output, in any order, and a creature drawn in OpenGL reacts to every sound and every knob.

- 🗣️ **Pitch and size, separately**: shift the voice up to two octaves down, then give it a bigger throat. Native-rate processing brings a 96 or 192 kHz recording's ultrasonic detail down into the voice
- 🐉 **Texture and weight**: a growling chop, a liquid wobble, a phaser, tube saturation, an ensemble, a tracked sub and overtones that carry the low end on small speakers
- 🤖 **Machines**: a 32-band pulse-carrier vocoder and a ring modulator with a pitch-tracking carrier and a resonant body
- 🧩 **Any order**: drag any module by its header, or use its ••• menu, to anywhere in the chain. Moves are click-free
- 🔊 **A finished output**: Push into a true-peak-safe limiter and an oversampled clipper, with Attack, Release and Auto to choose between clean limiting and low-end crunch
- 👁️ **A living creature** that forms out of drifting cloud, opens its jaw, glows, shakes and drips with the voice, softens back into fog in silence, and slowly evolves the harder you push it. Plus **39 factory presets** in six groups

<details>
<summary>More screenshots</summary>
<br>
<p align="center">
  <img src="roarschach/docs/view-forming.jpg" alt="Roarschach creature forming out of the fog" width="400"> <img src="roarschach/docs/view-crown.jpg" alt="Roarschach creature after a long, loud session" width="400">
</p>
<p align="center">
  <img src="roarschach/docs/panel-out.png" alt="Roarschach output stage" width="300"> <img src="roarschach/docs/tooltip.png" alt="Roarschach tooltip" width="474">
</p>
</details>

**VST3** (macOS, Windows) · **AU** (macOS) &nbsp;&nbsp;
[![Download Roarschach](https://img.shields.io/badge/download-v1.0.0-d00a35?style=flat-square)](https://github.com/samueljustice/plugins/releases/tag/roarschach-v1.0.0) &nbsp;
[Read the manual →](roarschach/README.md)

---

## 🎲 Sound Advice

> Shows a random piece of advice from film, TV and game sound professionals and other creative people.

<p align="center">
  <img src="soundadvice/docs/screenshot.png" alt="Sound Advice, in day and night themes" width="820">
</p>

A page of advice from people who work on film, TV, game and trailer sound: re-recording mixers, sound designers, supervising editors, game audio directors and others, plus a Disney Imagineer on leading creative work. Pick what you're working on (mixing, creatures, dialogue, silence, running a creative team...) and roll the die. It deals you a field note on a real technique or decision, or a quote in their own words, tied to the production it came from.

- 📚 **839 entries from 147 people**, each name linked to their credits
- 🔗 **Every entry traced to its source**: click through to the original interview, article or SoundWorks Collection video
- 🛠️ **Concrete, not fortune-cookie**: mostly field notes on real sources, recording methods and mix decisions
- 🗂️ **17 categories**, from Mixing and Field Recording to Horror & Tension, Silence & Restraint and Creativity & Teams. Each is a shuffled deck, so nothing repeats until you've seen the lot
- 🌗 **Paper by day, ink by night**, with keyboard shortcuts and right-click to copy
- 🔇 **Audio passes straight through**, so it can sit on a mix bus, or run as an app on a second screen

**App** (macOS, Windows) · **VST3** · **AU** &nbsp;&nbsp;
[![Download Sound Advice](https://img.shields.io/badge/download-v1.0.12-9a5a3a?style=flat-square)](https://github.com/samueljustice/plugins/releases/tag/soundadvice-v1.0.12) &nbsp;
[Read the manual →](soundadvice/README.md)

---

## 🛠️ For developers

<details>
<summary><b>Building from source</b></summary>
<br>

Every project builds with CMake and expects its dependencies in `SDK/` at the root of this repository (not committed):

| Dependency | Used by |
|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) 8.0.8, at `SDK/JUCE` | Swooshbuckler, Sound Advice, Subbertone, Pitch Flattener, Reversinator, Roarschach, Stretch Armstrong |
| [RubberBand](https://breakfastquay.com/rubberband/) 4.0.0, at `SDK/rubberband-4.0.0` | Pitch Flattener, Stretch Armstrong |
| Beat-and-Tempo-Tracking, at `SDK/Beat-and-Tempo-Tracking`, and [py-ptsl](https://pypi.org/project/py-ptsl/) | PTSL Beat Tool |

Then, from a project's folder:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

See each project's README for its own options.

**AAX:** add `-DSAMMYJ_AAX=ON` to build the AAX format too (macOS and Windows), and `-DAAX_SDK_PATH=<folder>` to use Avid's AAX SDK instead of the one bundled with JUCE (see [`cmake/AAX.cmake`](cmake/AAX.cmake)). The SDK isn't in this repository: it's Avid's. The release workflows fetch it from a private repository and upload each plug-in's AAX as a build artifact. It's unsigned, so it loads only in Pro Tools Developer until it is signed with PACE's wraptool.

</details>

<details>
<summary><b>Making a release</b></summary>
<br>

Each project has a GitHub Actions workflow in `.github/workflows/` that builds, signs and notarises it for macOS (and builds it for Windows where supported), then publishes a release. Push a tag named after the project to start one, for example:

```bash
git tag swooshbuckler-v1.0.0
git push origin swooshbuckler-v1.0.0
```

The tag prefixes are `swooshbuckler-v`, `soundadvice-v`, `subbertone-v`, `pitchflattener-v`, `reversinator-v`, `roarschach-v`, `stretcharmstrong-v` and `ptsl-beattool-v`.

When a plug-in's release finishes, `build-release_bundle.yml` rebuilds the combined **Sammy J's Plugins** installer for macOS and Windows from every plug-in's newest release, and publishes it as the repository's Latest release (tagged `bundle-v<date>.<run>`, with the build's date and time in the file names, like `SammyJ-Plugins-2026-11-08-1253-macOS-Installer.pkg`). It then points the **Get them all** buttons at the top of this README at those files and says when they were built (it rewrites the block between the `get-them-all` markers and commits it), and deletes the installer releases it replaces. It doesn't rebuild the plug-ins; it repackages what their releases published. The plug-ins it includes are listed in [`bundle/plugins.txt`](bundle/plugins.txt). After publishing, it points the Get them all buttons at the new files and sets every plug-in's download link, here and in each plug-in's own README, to that plug-in's newest release, so those never need editing by hand. On macOS the download is a disk image holding the installer and **Uninstall Sammy J's Plugins**, an app that removes everything again and, if asked, the settings and presets listed in [`bundle/userdata.txt`](bundle/userdata.txt); on Windows the installer's own uninstaller does the same. Run it by hand from the Actions tab to rebuild it now.

</details>

**License:** each project and dependency has its own license; see the LICENSE file in each folder.

---

<div align="center">

### 👋 Made by Samuel Justice

Creative Sound Supervisor at **[Sweet Justice](https://www.sweetjusticesound.com)**

<sub>Found a bug or have an idea? <a href="https://github.com/samueljustice/plugins/issues">Open an issue</a>.</sub>

</div>
