# Swooshbuckler v2 redesign brief

User feedback on v1: "the interface is cool but where's the multitrack editor? the font and colouring is far
too posh, it's not intuitive how to use; it needs to be almost DAW-like with a multitrack view to drop
samples into that can be viewed, trimmed etc." Also: silence-skipping must tail out nicely and not hop;
"Throw It" (draw a flight path, dice icon throws a random one) must be visually represented on the interface.
Research plans live in `docs/experiments/` (01 UX, 02 auto-classify, 03 Throw It).

## Principles
1. The multitrack IS the app. Open it and you see tracks you can drop samples onto.
2. Few controls on the front: transport, 6 macro knobs, the flight pad. Everything else in one "More" drawer.
3. Plain, DAW-like look (think Ableton / Reaper / Logic dark theme): system sans-serif font, sentence case,
   no letter-spacing, neutral greys, colour only for track colours, the SWASH button and record.
   The pirate flavour lives in names/presets/icons only, never in typography.
4. Under the hood: auto role detection + auto naming, auto gain, silence skipping, macros that offset many
   raw parameters, parameters that glide (springs).

## Theme (Source/Theme.h, Source/LookAndFeel.*)
- `Theme::Fonts` keeps its API (`body(h)`, `label(h)`, `heading(h)`, `make`), but uses the system UI
  sans-serif (`juce::FontOptions(h)` default sans; `heading` = bold), no extra kerning. Remove the Azeret
  embedding from fonts usage (BinaryData may keep the files; not used).
- Palette (keep these names; LogoOutline uses `text`, `input`, `harmonics`):
  background 0xff1b1c1f, panel 0xff232428, panelRaised 0xff2c2d32, outline 0xff3a3b41, outlineSoft 0xff2f3035,
  text 0xffe6e6e6, textDim 0xffa0a2a8, textFaint 0xff6b6d74,
  flight (accent, SWASH) 0xffffb347, record 0xffff5252, good 0xff6fcf97,
  input/sources 0xff4fc3f7, harmonics/movement 0xffef6f8e, filter 0xffb39dff, space 0xff6fd6b4, master 0xffc9cdd8,
  scope (waveform background) 0xff141518, grid 0xff2a2b30.
- `Theme::trackColour(int lane)`: 12 distinct muted-but-clear colours (built-ins greyish-blue tints,
  sample tracks: orange, teal, purple, green, pink, yellow...).
- Knobs (`WheelKnob` class, keep the name/API: `caption`, `setAccent`, spring-animated): plain DAW knob: dark
  filled circle, coloured value arc, a pointer line; caption under it in `label(12)`, value shown on hover/drag.
  No ship's wheel. `CrewFader` keep API, plain fader look (may become unused).
- Buttons: flat rounded rects (radius 4), toggled = accent fill at ~30% + accent text. Combo/popup/tooltip
  plain. Keep `drawPanel` (plain: panel fill, 1px outline, title in `heading(13)` sentence case, no rivets/rope),
  `drawRope` may stay but should be unused; `drawCutlasses` stays (small logo mark only).

## Window layout (Source/PluginEditor.*), design size 1280 x 800, scaled as now
```
+--------------------------------------------------------------------------------------------------+
| [x] Swooshbuckler   [ SWASH ]  One-shot|Auto|Energy  (Rate)   < Preset name >  [dice Roll]       |  transport, 52 px
|                                                   [● Rec 00:00] [Save last 60s] [Sea] [⚙]        |
+--------------------------------------------------------------------------------------------------+
| TimelineView (fills)                                                             | (Sea, toggle) |
|   toolbar / ruler / built-in lanes / sample lanes / scrollbar                    |  floating 3D  |
+-------------------------------------------+------------------------------------------------------+
| FlightPad ("Throw it")  ~420 x 220         | Size  Bite  Swirl  Space  Pitch  Chaos   [More ▾]   |  bottom, ~230 px
|  draw a path / dice / comets / envelope    | Ceiling small + GR meter                             |
+-------------------------------------------+------------------------------------------------------+
| status bar: hints left, messages right (click a "Saved ..." message to reveal the file)            |
+--------------------------------------------------------------------------------------------------+
```
- "More" opens a drawer (overlay sliding up over the timeline's lower part) with all raw parameters as small
  knobs in 3 labelled groups: Flight (Length, Shape, Speed, Distance, Direction combo), Movement (Sweep,
  Swirl, Weight, Grit, Punch, Space), Output (Inflate, Push, Ceiling), plus Cargo master. Close with the same
  button or Esc.
- Rate knob only visible when mode is Auto or Energy.
- "Sea" toggle shows the existing `SeaVisualiser` as a floating panel (about 420 x 240) at the top-right of the
  timeline area; off by default.
- Keep: presets glide (Morph), Roll the Bones dice (for parameters; the FlightPad has its own path dice),
  settings panel (record folder, bit depth, auto-record), Space fires, MIDI fires, toast/status messages.
- Remove `CargoHoldView` usage (file deleted).

## Engine & model (Cargo.*, SampleClassifier.*, WhooshEngine.*, PluginProcessor.*, Presets.h)
- 6 sample tracks (`CargoHold::c_numTracks = 6`), lane index == engine layer index (0..5 built-ins, 6..11 samples).
- Mute/solo per lane in `CargoHold` (`laneGain`), applied by the processor/engine to every layer level.
- Role per track (`CargoRole`), detected by `SampleClassifier::analyse` in `CargoHold::loadFile`, override via
  `setRoleOverride`; focus points via `SampleClassifier::findFocusPoints` (inside `CargoData::analyse`).
- `CargoPlayer::render(data, role, skip, rate, chaos, rng, fadeSeconds, cue, out, frames)` implements the
  per-role behaviour described in `Cargo.h`. Loops keep the v1 graceful behaviour (long equal-power fades that
  follow the whoosh length, natural tails). One-shot roles start so that the region's focus lands on the
  flight peak (`cue.secondsToPeak`), then tail out (fade over fadeSeconds at the region end) to silence.
  Rumble follows pitch at half strength (rate = sqrt(rate)).
- `WhooshEngine` builds a `CargoCue` in `spawnFlight` (secondsToPeak = attack - closest - baseDelay, flightSeconds
  = attack + release) and passes it to every player on the next chunk.
- Processor: macros (Size, Bite) per `Params` comment in Parameters.h; levels multiplied by `laneGain`;
  6 cargo track params; state saves mute/solo/role override/user names (CargoHold::toValueTree).
- Presets.h: `isSessionParameter` covers all 6 track levels; macros are not session params (presets may set them).

## Tests
- `Tests/RenderTest.cpp` must keep passing (update for 6 tracks / new render signature).
- New `Tests/ClassifierTest.cpp` (target SwooshbucklerClassify): synthesise one clip per role (sustained noise
  bed, pitched chirps with silences = Tonal, short noise burst with fast attack = Impact, single noise swell =
  Air, sparse clicks = Crackle, 40 Hz rumble = Rumble), check detected roles (>= 5 of 6), and file-name labels
  ("PIG_Squeal_03_v2" -> "Pig Squeal").

## Working rules for implementers
- Only edit the files you own (listed in your task). Interfaces in the headers listed as contracts are fixed;
  if you truly need a change, add (don't rename/remove) and say so in your report.
- Don't run cmake builds (other agents are editing in parallel). Syntax-check your .cpp files with
  `python3 tools/syntax-check.py Source/YourFile.cpp` (uses the real compile flags). Errors that come only from
  other agents' unfinished files can be ignored, but say so.
- No allocation, locks or file I/O on the audio thread. UI work on the message thread only.
