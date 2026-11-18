# DOOM EQ

A six-band parametric EQ whose curve display runs DOOM.

## The EQ

- Six bands. Each one can be a Bell, Low/High Shelf, or Low/High Cut at 12, 24 or 48 dB/oct.
- Zero-delay state-variable filters (Simper's SVF), so sweeping and automating stay smooth with no zipper noise. Shelves with Q above 0.71 get a resonant bump, and on a cut, Q sets the resonance at the corner (0.71 is a flat Butterworth).
- Bells use a matched design (Vicanek), so a high bell keeps its analogue shape near Nyquist instead of narrowing: a 16 kHz air boost sounds like one.
- Changing a band's type, turning a band off and bypass all crossfade, so nothing clicks, even under automation.
- The curve is the filters' exact digital response.
- Post-EQ analyser with a +4.5 dB/oct tilt.

**On the display:**
- Drag a node to set frequency and gain. On a cut, dragging up and down sets its resonance; the 0 dB line is flat.
- Hold Shift while dragging for fine adjustment.
- Scroll over a node to change Q, double-click it to turn the band on or off, and right-click it to pick the type.

**Elsewhere:**
- Double-click a knob to reset it, or click its number to type a value (`2.5k` works).
- **BYPASS** (under OUTPUT) takes the EQ out so you can compare, with DOOM's sound still playing. The host's own bypass also silences DOOM.
- **F1** or **?** shows every control.

## Presets and About

- The preset bar works like the other Sammy J plug-ins: `<` and `>` step through the presets, the name opens the menu, and SAVE stores your own.
- Factory presets come in three groups: Clean Up, Tone and Character.
- User presets live in `~/Library/Audio/Presets/SammyJs/DOOM EQ` (macOS) or `%APPDATA%\SammyJs\DOOM EQ\Presets` (Windows). The menu's SHOW PRESETS FOLDER opens it.
- Presets never change DOOM SOUND, its level or bypass.
- Click the DOOM EQ title, or pick ABOUT DOOM EQ in the preset menu, for the version, credits, licences and CHECK FOR UPDATES.
- The automatic update notice, shared with the other plug-ins, starts with DOOM EQ's first full release. 0.0.x test builds are pre-releases, which it skips; About's CHECK FOR UPDATES does see newer test builds.

## The DOOM

- **The screen** shows the whole frame, as big as fits. The window is resizable (drag the corner) and the screen grows with it.
- **OVERLAY** fades the grid, analyser and curve over the game, from 0% (pure DOOM) to 100%. Hovering over a node always shows it.
- **DOOM SOUND** feeds the game's sound effects and music into the plug-in *before* the EQ, so you're EQing DOOM. **LEVEL** sets how loud it comes in. It's off by default.
  - Sound effects are the original 8-bit samples. The music is Freedoom's MIDI, played on a small built-in synth with distorted guitars, synth bass and drums.
  - There's only one game, so only one EQ instance plays its sound at a time: the first one to turn DOOM SOUND on.
- **Take command:** press `` ` `` (or click TAKE COMMAND), then:
  - Arrows/WASD move, F (or Ctrl) fires, Space/E use, Shift runs, Alt strafes.
  - 1-7 pick weapons, even while running. Esc opens DOOM's menu and Enter selects.
  - Cheats and savegame names type normally.
  - **`` ` ``** again hands the keyboard back, and pauses your game until you return.
  - On a Mac, Ctrl+arrows switches desktops, so fire with F.
  - While you play, clicks on the screen only give the game the keys back; use the knobs to EQ meanwhile.
  - Some DAWs keep keys for themselves (Pro Tools, for one). If keys don't reach the game, try the standalone app.
- DOOM's own Quit returns to the title screen: it can't close your DAW.
- **The look** is built from the Freedoom IWAD's own flats, HUD font, menu skull and status-bar faces. The marine in the OUTPUT panel gets more beaten up the louder your output is, and says ouch when you clip.
- There is one game per process, shared by every instance. It pauses while no editor is open.
- Any `exit()` in the engine is caught, so DOOM can't take the DAW down with it.
- The Freedoom IWAD is embedded and unpacked on first run to `~/Library/Application Support/SammyJs DOOM EQ/`. Config and savegames go there too.

## Downloads

Test builds are pre-releases tagged `doomeq-v*` on the [releases page](https://github.com/samueljustice/plugins/releases). Each one has a macOS and a Windows installer, plus the plug-ins as zips. Once the AAX is PACE-signed, `doomeq-test-installers.yml` rebuilds both installers with it in. DOOM EQ isn't in the combined installer or on the website.

## Building

From `toys/doomeq`, with JUCE at `../../SDK/JUCE`:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Add `-DSAMMYJ_AAX=ON` for the AAX. `Resources/freedoom1.wad` is git-ignored. If it's missing, CMake downloads Freedoom 0.13.0 and checks its hash.

`DoomEQHostCheck` runs the processor offline and checks transparency, click-free type changes and bypass, oversized blocks, mono to stereo, extreme settings, presets and state recall. CI runs it on every build:

```bash
cmake --build build --target DoomEQHostCheck && ./build/DoomEQHostCheck_artefacts/DoomEQHostCheck
```

To release, bump `project(DoomEQ VERSION ...)` in `CMakeLists.txt` and push a matching `doomeq-v<version>` tag. `.github/workflows/build-release_doomeq.yml` does the rest.

## Licences

- `ThirdParty/doomgeneric` is [doomgeneric](https://github.com/ozkl/doomgeneric) (GPL-2.0), patched so `exit()`, the config dir and sound output are host-controlled. Because it's linked in, the DOOM EQ binary is GPL-2.0.
- [Freedoom](https://freedoom.github.io) is BSD-licensed; see `Resources/FREEDOOM-COPYING.txt`.
