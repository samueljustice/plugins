# DOOM EQ

A six-band parametric EQ whose curve display runs DOOM.

## The EQ

- Six bands. Each one can be a Bell, Low/High Shelf, or Low/High Cut at 12, 24 or 48 dB/oct.
- Zero-delay state-variable filters (Simper's SVF), so sweeping and automating stay smooth with no zipper noise. Shelves with Q above 0.71 get a resonant bump, and on a cut, Q sets the resonance at the corner.
- The curve is the filters' exact digital response.
- Post-EQ analyser with a +4.5 dB/oct tilt.

**On the display:** drag a node to set frequency and gain. Scroll over it to change Q, double-click it to turn the band on or off, and right-click it to pick the type.

## The DOOM

- **The screen** shows the whole frame, as big as fits. The window is resizable (drag the corner) and the screen grows with it.
- **OVERLAY** fades the grid, analyser and curve over the game, from 0% (pure DOOM) to 100%. Hovering over a node always shows it.
- **DOOM SOUND** feeds the game's sound effects and music into the plug-in *before* the EQ, so you're EQing DOOM. **LEVEL** sets how loud it comes in. It's off by default.
  - Sound effects are the original 8-bit samples. The music is Freedoom's MIDI, played on a small built-in synth with distorted guitars, synth bass and drums.
  - There's only one game, so only one EQ instance plays its sound at a time: the first one to turn DOOM SOUND on.
- **Take command:** press `` ` `` (or click TAKE COMMAND), then:
  - Arrows/WASD move, Ctrl/F fire, Space/E use, Shift run, Alt strafe.
  - 1-7 pick weapons, Esc opens DOOM's menu.
  - **Shift+Esc** (or `` ` `` again) hands the keyboard back.
- **The look** is built from the Freedoom IWAD's own flats, HUD font, menu skull and status-bar faces. The marine in the OUTPUT panel gets more beaten up the louder your output is, and says ouch when you clip.
- There is one game per process, shared by every instance. It pauses while no editor is open.
- Any `exit()` in the engine is caught, so DOOM can't take the DAW down with it.
- The Freedoom IWAD is embedded and unpacked on first run to `~/Library/Application Support/SammyJs DOOM EQ/`. Config and savegames go there too.

## Downloads

Test builds are pre-releases tagged `doomeq-v*` on the [releases page](https://github.com/samueljustice/plugins/releases). Each one has zips of the VST3, AU and app; the AAX is added once it has been PACE-signed. DOOM EQ isn't in the combined installer or on the website.

## Building

From `toys/doomeq`, with JUCE at `../../SDK/JUCE`:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Add `-DSAMMYJ_AAX=ON` for the AAX. `Resources/freedoom1.wad` is git-ignored. If it's missing, CMake downloads Freedoom 0.13.0 and checks its hash.

To release, bump `project(DoomEQ VERSION ...)` in `CMakeLists.txt` and push a matching `doomeq-v<version>` tag. `.github/workflows/build-release_doomeq.yml` does the rest.

## Licences

- `ThirdParty/doomgeneric` is [doomgeneric](https://github.com/ozkl/doomgeneric) (GPL-2.0), patched so `exit()`, the config dir and sound output are host-controlled. Because it's linked in, the DOOM EQ binary is GPL-2.0.
- [Freedoom](https://freedoom.github.io) is BSD-licensed; see `Resources/FREEDOOM-COPYING.txt`.
