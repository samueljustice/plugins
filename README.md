<div align="center">

# Sammy J's Plugins

**Some of the toys and utilities I've made for us at [Sweet Justice Sound](https://www.sweetjusticesound.com), given for free.**

![VST3](https://img.shields.io/badge/VST3-222?style=flat-square)
![AU](https://img.shields.io/badge/AU-222?style=flat-square)
![Standalone](https://img.shields.io/badge/Standalone-222?style=flat-square)
![AAX](https://img.shields.io/badge/AAX-222?style=flat-square)
[![Built with JUCE](https://img.shields.io/badge/built%20with-JUCE-8DC63F?style=flat-square)](https://juce.com)
![Downloads](https://img.shields.io/github/downloads/samueljustice/plugins/total?style=flat-square&color=d00a35)

<!-- get-them-all:start (written by .github/workflows/build-release_bundle.yml; edits here are replaced) -->
### Get them all

[![Get them all for macOS](https://img.shields.io/badge/Get_them_all-macOS-000000?style=for-the-badge&logo=apple&logoColor=white)](https://github.com/samueljustice/plugins/releases/download/bundle-v2026.11.16.201/SammyJ-Plugins-2026-11-16-1813-macOS.dmg)
[![Get them all for Windows](https://img.shields.io/badge/Get_them_all-Windows-0078D6?style=for-the-badge&logo=windows&logoColor=white)](https://github.com/samueljustice/plugins/releases/download/bundle-v2026.11.16.201/SammyJ-Plugins-2026-11-16-1813-Windows-Installer.exe)

<sub>One installer with the newest version of every plug-in: Swooshbuckler 2.2.0 · Sound Advice 1.0.13 · Subbertone 1.4.0 · Pitch Flattener 3.1.1 · Reversinator 2.1.0 · Roarschach 1.3.0 · Tonal Recall 0.2.3.<br>
Apps, VST3, AU and signed AAX for Pro Tools. It replaces older copies, and comes with an uninstaller (macOS: on the disk image; Windows: Settings &gt; Apps).<br>
Built 16 Nov 2026, 18:13 UTC. Older versions are on the <a href="https://github.com/samueljustice/plugins/releases">releases</a> page.</sub>
<!-- get-them-all:end -->

<sub>If these get some use in your work and you'd like to chip in, you can <a href="https://www.paypal.com/donate/?business=sam%40samueljustice.net&no_recurring=1&item_name=Sammy+J%27s+Plugins">toss a coin in the jar</a> on PayPal. Every bit goes towards more of them.</sub>

<br>

| [Roarschach](#roarschach) | [Swooshbuckler](#swooshbuckler) | [Tonal Recall](#tonal-recall) |
|:---:|:---:|:---:|
| [<img src="roarschach/docs/hero.gif" width="250" alt="Roarschach">](#roarschach) | [<img src="swooshbuckler/docs/screenshot.png" width="250" alt="Swooshbuckler">](#swooshbuckler) | [<img src="tonalrecall/docs/screenshot.png" width="250" alt="Tonal Recall">](#tonal-recall) |
| <sub>A modular voice processor for exploration and fun</sub> | <sub>Generates whooshes from up to ten sounds</sub> | <sub>Pitch-matches sound effects to music in Pro Tools AudioSuite</sub> |

| [Pitch Flattener](#pitch-flattener) | [Reversinator](#reversinator) | [Sound Advice](#sound-advice) | [Subbertone](#subbertone) |
|:---:|:---:|:---:|:---:|
| [<img src="pitchflattener/docs/screenshot.png" width="180" alt="Pitch Flattener">](#pitch-flattener) | [<img src="reversinator/docs/screenshot.png" width="180" alt="Reversinator">](#reversinator) | [<img src="soundadvice/docs/screenshot.png" width="180" alt="Sound Advice">](#sound-advice) | [<img src="subbertone/docs/screenshot.jpg" width="180" alt="Subbertone">](#subbertone) |
| <sub>Learns a sound and plays it back with its pitch flat</sub> | <sub>Real-time reverse effects</sub> | <sub>Advice from sound professionals</sub> | <sub>Sub and harmonics for bass</sub> |

</div>

## Roarschach

<p align="center">
  <img src="roarschach/docs/hero.gif" alt="Roarschach" width="820">
</p>

A modular voice processor. It has fifteen modules (pitch and throat size, a small speaker in a room, a vocoder, a ring modulator, growl, tube saturation, a tracked sub and others) and you can drag them into any order. Each module has a die that randomises it within sensible ranges, and there are 51 presets to start from.

The picture above the chain is an ink blot that forms out of cloud and moves with the voice. It does nothing to the sound.

<details>
<summary>More screenshots</summary>
<br>
<p align="center">
  <img src="roarschach/docs/view-forming.gif" alt="Roarschach ink blot gathering out of the cloud" width="400"> <img src="roarschach/docs/view-dissolving.gif" alt="Roarschach ink blot tearing back into smoke" width="400">
</p>
<p align="center">
  <img src="roarschach/docs/colour-moods.gif" alt="Roarschach cloud settled in three colours: pale blue-grey at Init, ember red and warm stone" width="604">
</p>
<p align="center">
  <img src="roarschach/docs/panel-out.png" alt="Roarschach output stage" width="300"> <img src="roarschach/docs/tooltip.png" alt="Roarschach tooltip" width="474">
</p>
</details>

**VST3** (macOS, Windows) · **AU** (macOS) · **AAX**<br>
[Get it in the installer](https://github.com/samueljustice/plugins/releases/latest) · [Read the manual →](roarschach/README.md)

## Swooshbuckler

<p align="center">
  <img src="swooshbuckler/docs/screenshot.png" alt="Swooshbuckler" width="820">
</p>

A whoosh maker. Drop in up to ten sounds and it sorts them onto two Doppler buses and a hits layer, then moves every fader by itself so the layers swell, pass and fall away. Hold Swell (or the space bar, or a MIDI note) to push it harder.

The quickest way to work is the renderer. Dial in a sound you like, then ask for a hundred variations of it, or a thousand. It renders them in the background, much faster than real time, as separate files or as one long file with a marker on each. You can also record by hand: a tape records every whoosh into one file and leaves out the silence, and Last 60 s saves the last minute if you weren't recording.

It plays in mono, in stereo or in surround up to 7.1.4. In surround every pass flies through the room on a path you choose (across the front, past your head front to back, or overhead), each at a height of its own, and each hit lands where its whoosh is. Recordings and renders come out at any width, laid out as Pro Tools writes them or as one mono file per speaker.

There is more in it: a spectral morph between the two buses, a third bus called Stowaway, and modulators you patch with cables. The manual covers all of it. It runs as an app as well as a plug-in.

<details>
<summary>More screenshots</summary>
<br>
<p align="center">
  <img src="swooshbuckler/docs/images/morph-pad.png" alt="Swooshbuckler spectral morph pad" width="400">
  <img src="swooshbuckler/docs/images/sea.png" alt="Swooshbuckler the sea" width="400">
</p>
<p align="center">
  <img src="swooshbuckler/docs/images/tracks.png" alt="Swooshbuckler tracks" width="820">
</p>
</details>

**App** (macOS, Windows) · **VST3** · **AU** · **AAX**<br>
[Get it in the installer](https://github.com/samueljustice/plugins/releases/latest) · [Read the manual →](swooshbuckler/README.md)

## Tonal Recall

<p align="center">
  <img src="tonalrecall/docs/screenshot.png" alt="Tonal Recall" width="820">
</p>

Pitch-matches a sound effect to the music under it. It's for trailer work, where stings, hits and tails should land on one of the music's notes. You play it the music, then the effect, and it works out a shift, shows it as a curve you can edit, and stamps it onto the clip when you render. The clip keeps its length and its hits stay where they were.

It is an offline tool for Pro Tools AudioSuite, not a live effect. The VST3 and AU versions work the same way in other hosts.

<details>
<summary>More screenshots</summary>
<br>
<p align="center">
  <img src="tonalrecall/docs/music.png" alt="Tonal Recall music panel" width="400"> <img src="tonalrecall/docs/tail.png" alt="Tonal Recall effect scan with a predicted tail" width="400">
</p>
<p align="center">
  <img src="tonalrecall/docs/strip-shift.png" alt="Tonal Recall shift, Bend, Stamp and Trim" width="820">
</p>
<p align="center">
  <img src="tonalrecall/docs/playing.png" alt="Tonal Recall playing the stamped effect, with the playhead on the effect's scan" width="820">
</p>
</details>

**AAX AudioSuite** (macOS, Windows) · **VST3** and **AU** for offline use in other hosts<br>
[Get it in the installer](https://github.com/samueljustice/plugins/releases/latest) · [Read the manual →](tonalrecall/README.md)

## Pitch Flattener

<p align="center">
  <img src="pitchflattener/docs/screenshot.png" alt="Pitch Flattener" width="820">
</p>

Takes the pitch movement out of a sound: the sweep of a pass-by, a siren's wail, an engine's climb, vibrato. Press Learn and play the sound once. It reads the pitch of the whole sound three ways, for voices, engines and tones, uses the reading that flattens it best, and makes a flat version by speeding the playback up and slowing it down from moment to moment, so the pitch stays put however fast it bends. You can let the length change with the speed, or have the sound stretched back to its own timing and length, with a character for tone, hits or voices. Play the same recording again, at the same speed, and you hear the flat version from wherever you start; anything else, even another take of the same sound, passes through untouched.

It can hold the sound at the top of its pitch, the bottom, the middle, where it starts, or a note you type, and a recording of several sounds with short silences between them gets each one flattened to its own pitch. Where a reading goes wrong you can pull the line into place by hand, zoomed in as far as you need. It is made for a rack or a DAW (learn, then play or render), and adds a fixed delay, 0.4 seconds at ordinary block sizes, that the host makes up for. In its tests a 7-semitone pass-by and a siren wailing over 15 semitones both come out with no measurable movement.

<details>
<summary>More screenshots</summary>
<br>
<p align="center">
  <img src="pitchflattener/docs/editor.png" alt="Pitch Flattener SCOPE with a corrected line" width="820">
</p>
<p align="center">
  <img src="pitchflattener/docs/flight.png" alt="Pitch Flattener FLIGHT view" width="820">
</p>
<p align="center">
  <img src="pitchflattener/docs/controls.png" alt="Pitch Flattener TARGET, NOTE, AMOUNT, SOURCE, LENGTH and CHARACTER" width="820">
</p>
</details>

**VST3** (macOS, Windows) · **AU** (macOS) · **AAX**<br>
[Get it in the installer](https://github.com/samueljustice/plugins/releases/latest) · [Read the manual →](pitchflattener/README.md)

## Reversinator

<p align="center">
  <img src="reversinator/docs/screenshot.png" alt="Reversinator" width="820">
</p>

A real-time reverser. It recreates the three effects of Backwards Machine (Reverse Playback, Forward Backwards and Reverse Repeat), rebuilt from a study of the original's signal flow and checked against the original's own renders, which it matches at better than 0.999 correlation. What's different is a resizable window, a display that shows the read heads sweeping back along a road into the past, and no clicks when you change a setting.

**VST3** (macOS, Windows) · **AU** (macOS) · **AAX**<br>
[Get it in the installer](https://github.com/samueljustice/plugins/releases/latest) · [Read the manual →](reversinator/README.md)

## Sound Advice

<p align="center">
  <img src="soundadvice/docs/screenshot.png" alt="Sound Advice, in day and night themes" width="820">
</p>

Not an effect: a page of advice. Pick what you're working on (mixing, creatures, dialogue, silence, running a team) and roll the die, and it shows you something a working sound person said or did about it, with the production it came from and a link to the source. There are 839 entries from 147 people: mixers, sound designers, editors, game audio directors, and a Disney Imagineer on leading creative work.

Audio passes straight through it, so it can sit on a mix bus, or run as an app on another screen.

**App** (macOS, Windows) · **VST3** · **AU** · **AAX**<br>
[Get it in the installer](https://github.com/samueljustice/plugins/releases/latest) · [Read the manual →](soundadvice/README.md)

## Subbertone

<p align="center">
  <img src="subbertone/docs/screenshot.jpg" alt="Subbertone" width="820">
</p>

Made mostly for cars and other engines: it follows the pitch of an engine, and also of a voice, bass or drone, and adds a sub an octave below. You can then draw in its harmonics, up to the sixteenth, and add deeper subs two and three octaves down, which takes it from plain sub reinforcement to a bass you can hear on small speakers, or to a growl. Phase Align lines the added harmonics up with the ones already in the sound, so they add instead of cancelling. After that come saturation, a resonant filter and a limiter. It has about 2 ms of latency, which it reports to the host. For passages that are hard to track as they play, such as engine run-ups, loop the passage in your host and press LEARN: Subbertone works out the pitch of the whole pass with hindsight, and the sub follows what it learned. LEARN is new and works best on close engine and exhaust recordings; the plug-in's [known limitations](subbertone/README.md#known-limitations) say what it doesn't handle well yet.

<details>
<summary>More screenshots</summary>
<br>
<p align="center">
  <img src="subbertone/docs/harmonics.png" alt="Subbertone harmonic editor with Phase Align on" width="701">
</p>
<p align="center">
  <img src="subbertone/docs/output-master.jpg" alt="Subbertone OUTPUT and MASTER sections" width="626">
</p>
<p align="center">
  <img src="subbertone/docs/presets.png" alt="Subbertone presets menu" width="400">
  <img src="subbertone/docs/patterns.png" alt="Subbertone patterns menu" width="400">
</p>
</details>

**VST3** (macOS, Windows) · **AU** (macOS) · **AAX**<br>
[Get it in the installer](https://github.com/samueljustice/plugins/releases/latest) · [Read the manual →](subbertone/README.md)

## For developers

<details>
<summary><b>Building from source</b></summary>
<br>

Every project builds with CMake and expects its dependencies in `SDK/` at the root of this repository (not committed):

| Dependency | Used by |
|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) 8.0.8, at `SDK/JUCE` | Swooshbuckler, Sound Advice, Subbertone, Pitch Flattener, Reversinator, Roarschach, Tonal Recall, Stretch Armstrong |
| [RubberBand](https://breakfastquay.com/rubberband/) 4.0.0, at `SDK/rubberband-4.0.0` | Pitch Flattener, Tonal Recall, Stretch Armstrong |
| Beat-and-Tempo-Tracking, at `SDK/Beat-and-Tempo-Tracking`, and [py-ptsl](https://pypi.org/project/py-ptsl/) | PTSL Beat Tool |

Then, from a project's folder:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

See each project's README for its own options.

**AAX:** add `-DSAMMYJ_AAX=ON` to build the AAX format too (macOS and Windows), and `-DAAX_SDK_PATH=<folder>` to use Avid's AAX SDK instead of the one bundled with JUCE (see [`cmake/AAX.cmake`](cmake/AAX.cmake)). The SDK isn't in this repository: it's Avid's. The release workflows fetch it from a private repository and upload each plug-in's AAX as a build artifact. That build is unsigned: Pro Tools needs it signed with PACE's wraptool, which needs the iLok, so signing is done by hand on the machine the iLok is plugged into. After each release, the **AAX signing** workflow waits at two stages: *Sign on the Mac* (plug the iLok into the Mac and run `sign-aax`; Notarise AAX then publishes `<Name>-<version>-macOS-AAX.zip`) and *Sign on the PC* (move the iLok to the PC and run `sign-aax` there; publishes `<Name>-<version>-Windows-AAX.zip`). Each stage waits 30 minutes; re-run the workflow with the tag to sign later. When both stages are done, the Sammy J's Plugins installer rebuilds with the signed AAX, so it always carries every plug-in's AAX; the per-plug-in pages all link to that installer. `sign-aax` signs every release waiting at its stage, so one iLok swap covers releases made together. `bundle/install-aax-signer.sh` installs it on the Mac, with a Sign AAX app to click (keep it in the Dock), and `bundle/install-aax-signer-windows.ps1` on the PC, with a Sign AAX shortcut to click (on the Desktop and in the Start menu; pin it to the taskbar), each with a prompt that asks when signing is waiting. On the PC, `sign-aax` runs `bundle/sign-aax-windows.ps1`, which signs with a code-signing certificate instead of a Developer ID and uploads the zip straight to the release (there's no notarisation on Windows). The *Sign on the PC* stage only runs while the repository variable `AAX_WINDOWS_SIGNING` is `true`.

</details>

<details>
<summary><b>Making a release</b></summary>
<br>

Each project has a GitHub Actions workflow in `.github/workflows/` that builds, signs and notarises it for macOS (and builds it for Windows where supported), then publishes a release. Push a tag named after the project to start one, for example:

```bash
git tag swooshbuckler-v1.0.0
git push origin swooshbuckler-v1.0.0
```

The tag prefixes are `swooshbuckler-v`, `soundadvice-v`, `subbertone-v`, `pitchflattener-v`, `reversinator-v`, `roarschach-v`, `tonalrecall-v`, `stretcharmstrong-v` and `ptsl-beattool-v`.

**Every update, before tagging:**

1. Bump the version (each project's CMakeLists, and where its workflow says so, its installer script).
2. Say what's new in the plug-in's own docs: its `CHANGELOG.md`, the version history in its `README.md`, or its `RELEASE_NOTES.md` (which becomes the release text, so head it with the new version). Update its README's tour and screenshots for anything that looks or works differently.
3. Update the plug-in's section on this page. This README is also the website, [samueljustice.github.io/plugins](https://samueljustice.github.io/plugins/), which the Pages workflow republishes whenever it changes.
4. Run `bundle/check-release-docs.sh <id> <version>`. The Release docs workflow runs it again on the tag and fails (GitHub emails) if a step was missed.
5. Push the tag, one tag per push (GitHub ignores pushes of more than three tags). Every update must be its own tagged release: installed copies find new versions by their plug-in's own tag (`<prefix>X.Y.Z`), not by the combined installer.
6. Sign the AAX when the AAX signing run asks (iLok in the Mac, **Sign AAX**, then the PC); the combined installer then rebuilds.

When a plug-in's release finishes, `build-release_bundle.yml` rebuilds the combined **Sammy J's Plugins** installer for macOS and Windows from every plug-in's newest release, and publishes it as the repository's Latest release (tagged `bundle-v<date>.<run>`, with the build's date and time in the file names, like `SammyJ-Plugins-2026-11-08-1253-macOS-Installer.pkg`). It then points the **Get them all** buttons at the top of this README at those files and says when they were built (it rewrites the block between the `get-them-all` markers and commits it), and deletes the installer releases it replaces. It doesn't rebuild the plug-ins; it repackages what their releases published. The plug-ins it includes are listed in [`bundle/plugins.txt`](bundle/plugins.txt). Every plug-in's own download link, here and in its README, points to this installer, so those never need editing. On macOS the download is a disk image holding the installer and **Uninstall Sammy J's Plugins**, an app that removes everything again and, if asked, the settings and presets listed in [`bundle/userdata.txt`](bundle/userdata.txt); on Windows the installer's own uninstaller does the same. Run it by hand from the Actions tab to rebuild it now.

</details>

**License:** each project and dependency has its own license; see the LICENSE file in each folder.

---

<div align="center">

### Made by Samuel Justice

Creative Sound Supervisor at **[Sweet Justice](https://www.sweetjusticesound.com)**

I taught myself C++ during the development of SOMA, but my knowledge only goes so far. These plug-ins are experiments: ideas that have been sitting in my head for a while, built with what I know and with help from Claude.

<sub>Found a bug or have an idea? <a href="https://github.com/samueljustice/plugins/issues">Open an issue</a>.</sub>

</div>
