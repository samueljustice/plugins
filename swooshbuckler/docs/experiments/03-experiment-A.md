# Experiment A: "Throw It" (performance and physical gesture)

Theme: how the whoosh is *played*, not just how it is configured.

Status: plan only. Nothing in `Source/` has been changed.

---

## 1. Survey: what already exists

| Product / work | What it does with gesture and motion | What it does not do |
|---|---|---|
| UVI Whoosh FX ([rekkerd](https://rekkerd.org/uvi-releases-whoosh-fx-whoosh-and-motion-sound-design-tool/), [UVI tips](https://support.uvi.net/hc/en-us/articles/360001692797-Whoosh-Designer-Tips)) | You draw *envelopes* with the mouse. It has Pass-By and a Speed Curve for doppler and panning. | The motion is a 1D parameter curve, not a path in space. There is no propagation model. |
| Tonsturm Whoosh, Reaktor ([KVR](https://www.kvraudio.com/product/whoosh-by-tonsturm), [Designing Sound](https://designingsound.org/2014/02/25/whoosh-review/)) | Doppler engine plus a "Manual" real-time performance mode. | The doppler engine is a straight-line pass-by driven by parameters. |
| Krotos Studio / Reformer ([Krotos blog](https://krotos.studio/blog/whoosh-sound-effects-krotos-studio)) | 4-zone XY pad. Whoosh presets have a pin "path" you trace with the mouse, and AI maps quiet/mid/loud material onto the pad. | The XY pad picks timbre. The pad position is not a position in space. |
| Krotos Concept "XY Capture", Krotos Igniter ([synthanatomy](https://synthanatomy.com/2020/02/krotos-concept-fuses-classic-synthesis-with-new-modulation-options.html), [gearnews](https://www.gearnews.com/igniter-vehicle-sound-design-spaceships-sports-cars-trucks-and-texture/)) | Records XY moves as modulation. Igniter has a doppler "master controller" for vehicles. | Same as above: the moves are modulation, not physics. |
| Waves Doppler, 2025 ([Thomann](https://www.thomann.de/be/waves_doppler.htm)) | Doppler processor for existing audio, with a straight pass-by curve. | It does not generate sound and has no drawn path. |
| Sketch2Sound, Adobe/Northwestern, ICASSP 2025 ([arXiv 2412.08550](https://arxiv.org/abs/2412.08550), [project](https://interactiveaudiolab.github.io/project/sketch2sound.html)) | A vocal imitation (loudness, centroid and pitch contours) controls a diffusion text-to-audio model. Shipped as the Firefly "Generate Sound Effects" beta in September 2025. | It is offline and generative. It has no spatial or physical motion model. |
| Metabow, NIME 2025 ([nime.org](https://nime.org/proc/nime2025_62/)) | IMU bow gestures mapped to spatial audio, using hybrid direct and ML mapping. | It is a music instrument, not a pass-by synth. |
| Vehicle and aircraft auralisation: Dreier & Vorlaender RWTH ([IOA](https://author.ioa.org.uk/conference-proceeding/internoise-proceedings-v3/171-dreir)), York traffic auralisation ([thesis](https://etheses.whiterose.ac.uk/28802/6/Fu_203026484_ThesisDeposit.pdf)), NASA flyover ([NTRS](https://ntrs.nasa.gov/api/citations/20200002559/downloads/20200002559.pdf)), KU Leuven ([lirias](https://lirias.kuleuven.be/4073823)) | Retarded-time propagation along arbitrary trajectories, plus a ground-reflection image source and air absorption. | These are research or engineering tools for traffic and aircraft noise. None is a playable sound-design instrument. |
| Retarded-time Doppler tracking ([Riabko et al. 2025](https://acnsci.org/journal/index.php/jec/article/download/1201/941/4716)) | Shows that ignoring retardation biases moving-source estimates. | It is analysis only. |

**The gap.** Two things exist separately:
- Commercial whoosh tools let you draw *parameter curves*.
- Acoustics research renders *arbitrary trajectories* with retarded time and ground reflection.

Nobody has made the trajectory itself the instrument: drawn or thrown live, heard causally while you move, and with the gesture's own speed driving the aeroacoustic loudness and timbre.

Swooshbuckler is unusually well placed to do this. Its flights already read continuous sources through per-ear delays measured in "seconds of sound travel" (c = 1). So a 2D path is a small step from the straight-line case.

---

## 2. Primary experiment: "Throw It": gesture trajectories with retarded-time propagation

### Hypothesis

Sound designers get a more varied and more convincing set of pass-bys, faster, if:
- they draw or flick the source's path through space on the visualiser, and
- the engine solves the physics along that path: retarded time per ear, a ground reflection, and loudness and brightness that follow the gesture's speed.

Varied here means swerves, near-misses, orbits, decelerating stops and boomerangs. The comparison is against tweaking length, shape, speed and distance knobs.

A second, testable claim is that the physics turns sloppy hand gestures into plausible sound. Sloppy here means jittery, uneven speed. Plausible means doppler, ITD, comb sweep and level all stay mutually consistent, because they come from one geometry.

### Why it is new

1. **The path is the instrument.** The path is in 2D space, top-down: lateral x and depth y. It is not a parameter curve. Orbits give a constant distance and no pitch shift, and swerves give a pitch "wobble". None of that is possible in the current model, in UVI, or in Krotos.
2. **It is causal, so live play works with zero lookahead.** The sound heard at time t was emitted at an earlier time te, where `t - te = |p(te) - ear|`. That equation needs only *past* positions. So the source can follow your finger while you draw: the propagation delay is the "latency", and it is physically correct.
3. **The gesture velocity drives the source physics directly** (Lighthill/Curle aeroacoustic scaling):
   - Emitted power goes as |v|^n, with n of about 5 to 6 for dipole or trailing-edge noise. An "Aero" knob blends n from 0 to 6.
   - Spectral centre follows Strouhal shedding, f ∝ St·|v|/d. That drives the AIR band centres and the FLUTTER chop rate.
   - So a fast flick is loud and bright on its own, and a slowing drag goes dark. This is the "gesture's velocity profile used directly".
4. **Throw physics after release.** On mouse-up the path continues ballistically from the release velocity, with drag and an optional "spin" curvature in the style of the Magnus effect. A flick becomes a thrown object that sails past and away. No whoosh tool lets you *throw* the source.
5. **Ground reflection as a physical comb.**
   - A mirrored image source at listener height (h of about 1.6 m) gives the moving comb filter of a real jet or car fly-by.
   - Its notches, f_k = (2k+1)/(2·Δτ), where Δτ is the extra path time, sweep because the geometry moves.
   - This is unlike the existing SWIRL jet flanger, which is LFO or envelope driven. The two can coexist.

### DSP design

Units are unchanged: positions are in seconds of sound travel (1 m ≈ 1/343 s), and c = 1.

**Path representation**
- `struct GesturePath { int count; float dt; std::array<juce::Point<float>, 512> p; float duration; }`.
- Points are resampled on the UI side to a uniform `dt` (for example 1/200 s), so there are no timestamps on the audio thread.
- This gives about 2.5 s at 200 Hz. Throw extrapolation is appended to the same buffer, up to the 512-point limit. Use dt = 1/100 for longer throws.
- `posAt(te)` uses Catmull-Rom between samples. `velAt(te)` is the analytic derivative of the same spline, so speed is smooth and not differentiated noise.
- The UI pre-smooths the path with a One-Euro filter (Casiez 2012), which is the standard jitter-versus-lag trade-off for pointer input.

**Per-ear retarded time (per sample, per flight)**
- For each ear e (offset ±`m_earOffset` on x), solve `g(te) = te + |p(te) - ear_e| - t = 0`.
- Newton's method: `te -= g / (1 + dot(unit(p - ear), v(te)))`.
  - Seed it from the previous sample's te_e plus dt.
  - One iteration per sample is enough when |v| < 0.9 (warm start). Clamp the source speed to Mach 0.9 on the UI side.
- Read delay: `delay_e = (m_baseDelay + (t - te_e)) * fs + back`. This replaces the `distL`/`distR` lines in `renderFlights`.
- The existing straight line is the special case p(te) = (dir·mach·(te - attack), closest). Code it as a path generator to keep one code path, and use it as a regression check.

**Ground image**
- A second pair of reads at the mirrored source, `p' = (x, y)` with height offset: `dist' = sqrt(dx² + dy² + (2h)²)`, using its own retarded-time solve.
  - In 2D top-down, treat the ground as a vertical offset 2h, with source height = listener height h.
- The image is mixed at gain `r = groundAmount * 0.8`, through a one-pole "grass" lowpass, at about 3 to 6 kHz.
- Cost: 2 more reads per layer per ear per flight. This is acceptable because there are at most 4 flights and 10 layers.

**Envelope**
- New flight envelope: `envPath(te) = lerp(1, (|v(te)| / vRef)^n, aero) * fadeIn/Out(te)`, normalised so the path's maximum is 1.
  - Evaluate it at *emission* time te, not at t. Distance attenuation then happens naturally.
- Add explicit `1/max(dist, d0)` spreading, with `d0` taken from the current `closest` mapping so levels match the existing presets. The current model folds spreading into `envelopeAt`.

**Pan, ITD and air**
- ITD comes for free from the per-ear te.
- Azimuth is `atan2(x, y)`. This replaces `x / dist`, so sources behind the listener (y < 0) are handled.
- Add a mild front/back cue: a slight high-shelf cut when y < 0. This is a cheap stand-in for HRTF.
- Air-absorption cutoff is computed from `dist(te)`, exactly as now.

**Strouhal brightness**
- Per flight, publish `speedNorm` into the same "energy one delay ahead" computation in `renderSources`, around line 300.
- Then `m_sourceEnergy` also tracks |v| at emission. AIR, FLUTTER and ROAR already brighten with `m_sourceEnergy`, so this is a near-zero-code mapping.

**Punch hit**
- `fireHit` fires at the *arrival* of the closest approach. That is the minimum of `te + dist(te)`, found as the minimum of dist, precomputed on the UI side as `tClosest`.
- It does not fire at `attack`.

### UX design

The SeaVisualiser becomes a top-down "flight deck". The listener's head is at centre-bottom, about 0.35 of the height, and the grid spans ±40 m.

- **Draw (hold and move):** you hear the source live as you drag. The visualiser shows the path trail and the source dot, plus a faint "heard position" ghost at p(te), so the retardation is visible. This is a nice teaching visual.
- **Flick:** release while moving and the throw extrapolates, with drag about 0.6 /s and spin from path curvature at release. Release while still and the path ends with a fade.
- **Re-fire:** the last path is stored as the "Drawn" path.
  - New `direction` choice: `Drawn`, alongside L→R / R→L / Random.
  - MIDI notes and the trigger button replay it, with velocity scaling playback rate from 0.7x to 1.3x and level.
  - Chaos adds a small spline jitter and a random mirror of x.
- **Modifiers:**
  - Shift-drag: draw without sound (sketch, then fire).
  - Alt: mirror on fire.
  - Double-click: clear back to straight lines.
- **Persistence:** save the path in plugin state as a base64 float blob in the APVTS ValueTree, so sessions recall it.
- **Knobs:** two new knobs, `Aero` (0..1, velocity-to-level/brightness exponent blend) and `Ground` (0..1). Default both to 0 so existing presets are untouched.

### How it plugs into WhooshEngine

| Location | Change |
|---|---|
| `WhooshEngine.h`, `Params` | Add `float aero = 0, ground = 0;`. Add `Drawn` to the `Direction` enum. |
| `WhooshEngine.h`, new `struct GesturePath` | As above. Pool `std::array<GesturePath, 2> m_paths` with `std::atomic<int> m_livePath`, `m_newPathReady`, double-buffered. The UI writes the inactive slot, then flips the atomic. No locks and no allocation on the audio thread. |
| New public API | `void submitPath(const GesturePath&)` on the message thread, which copies into the inactive slot. `void beginLiveGesture()`, `void pushLivePoint(Point)`, `void endLiveGesture()`, using an SPSC ring of points (`juce::AbstractFifo`, 1024). The live flight's path grows as points arrive. |
| `Flight` struct | Add `const GesturePath* path` (nullptr = legacy straight), `double teL, teR, teGL, teGR` (warm-start state), `float vRef, tClosest`. |
| `spawnFlight(float)` | Add an overload `spawnFlight(float velocity, bool drawn)`. When drawn: copy a pointer to the stable path slot, set `attack = tClosest`, set `release = duration - tClosest`, and seed te. The legacy code is unchanged. |
| `Flight::envelopeAt` | Unchanged for legacy. Add `pathEnvelopeAt(te)`. |
| `renderFlights` | In the per-sample loop, add a branch `if (f.path)`: retarded-time Newton per ear, plus the ground image reads, atan2 pan, and env at te. Move the chunk-rate air-absorption block to use `dist(teMid)`. |
| `renderSources` (energy-ahead block, ~l.295-303) | For path flights, use `env(te + delay)` and include `aero·speedNorm` in `energy`. |
| `fireHit` | Unchanged signature. Triggered from the `tClosest` arrival. |
| `publishViews` / `FlightView` | Add `float y` (depth) and `heardX, heardY`, for the deck view. `m_viewData` grows from 4 to 7 floats per flight. |
| `SeaVisualiser` | Add `mouseDrag` / `mouseUp` / `mouseDoubleClick`, a One-Euro filter, resampling, throw extrapolation, and the deck drawing mode. |
| `PluginProcessor` | Add 2 parameters and the `Drawn` direction choice. Save and restore the path blob in `get/setStateInformation`. |

### One-day prototype scope (one engineer)

Morning:
1. Add `GesturePath` and `submitPath`.
2. Add the path branch in `renderFlights`, with the retarded-time Newton solve and atan2 pan.
3. Write a unit or offline test that renders:
   - (a) the straight line as a path, compared against the legacy flight (the outputs should be close: residual below -40 dB, tone pitch within 1 cent);
   - (b) a circle at constant radius (pitch should stay constant within 1 cent);
   - (c) a U-turn (the pitch should go up, down and up).

Afternoon:
4. SeaVisualiser drag capture: One-Euro filter, 200 Hz resample, throw extrapolation, and `submitPath` on mouse-up. The trigger button and MIDI replay it.
5. The `Aero` envelope and the ground image, with the ground hard-coded on first and then a knob if time allows.

Out of scope for day 1:
- live-while-drawing (the SPSC streaming);
- state persistence;
- supersonic / Mach cone (multiple te roots);
- external controllers.

Each is a follow-up of half a day to a day.

### Success criteria: does it "stick"?

**Physics checks (automatic)**
- Straight-path parity with the legacy model.
- Constant-radius orbit gives zero doppler.
- Ground notch frequencies within 3% of `(2k+1)/(2Δτ)` at the closest point.
- No zipper or clicks at path joins: spectral flux spike below -60 dB at segment boundaries.
- CPU: below 1.5x the current `renderFlights` cost with 4 drawn flights and the ground on, at 48 kHz on an M1.

**Speed test (5 sound designers)**
- Brief: "make a near-miss that swerves away", "an object circling your head", "something thrown past that slows and drops".
- Knobs-only versus Throw It, timed. Target: Throw It at least 2x faster on 2 of 3 briefs.

**Blind preference (MUSHRA-lite, 10 listeners)**
- Drawn flights versus best-effort knob flights, rated on "realism of motion".
- Target: a significant preference (p < 0.05) on at least 2 of 3 briefs.

**Stickiness**
- Over a week of beta use, at least 30% of bounced whooshes use `Drawn` direction, which we can log locally on an opt-in basis.
- At least 1 of 5 testers spontaneously uses flick or throw in a real cue.
- Kill it if testers draw once and go back to knobs.

---

## 3. Backup experiment: "Say It": voice-to-flight inverse fitting

**Hypothesis.** Designers already *say* whooshes ("shhh-WOOOM-ff"). Fitting the Swooshbuckler flight model to a 1 to 3 s vocal imitation recovers a playable flight faster than dialling knobs. The fitted parameters are peak time, closest distance, speed, shape curves and layer balance. Sketch2Sound does this generatively and offline with a diffusion model. This version would be a real-time, interpretable, *physical* inversion, and no shipped tool does that.

**DSP**
1. Record the INPUT layer while trigger is held, into a 3 s buffer on the audio thread (preallocated).
2. On release, a background thread extracts at a 100 Hz frame rate:
   - loudness (RMS, dB);
   - spectral centroid;
   - pitch (YIN), with confidence.
3. Fit the flight parameters to these contours with a coarse grid followed by Nelder-Mead (around 2,000 evaluations of a closed-form contour model, in under 50 ms):
   - **Peak and shape.** `attack` and `release` come from the time of maximum loudness. `curveA` and `curveR` come from the log-loudness slopes on either side.
   - **Speed (mach).** Speed comes from the pitch-glide ratio across the peak, f_before / f_after = (1+M)/(1-M). If pitch confidence is low, use the centroid ratio instead.
   - **Closeness.** Closeness comes from the sharpness of the glide: its width is about closest / mach.
4. Map the voice's noisiness (spectral flatness) and its low-band ratio to the AIR / ROAR / SUB levels.
5. Push the result through the normal `Params` and `spawnFlight`, so nothing new runs in the DSP path. Optionally store the vocal loudness contour as a custom envelope table that replaces `envelopeAt`.

**Integration**
- New `VoiceFitter` class, on the message or background thread.
- `WhooshEngine::Flight` gains an optional `const float* envTable`.
- One "Say it" button in the editor.

**1-day scope.** Do contour extraction and the closed-form fit offline in a test harness on 20 recorded imitations. Then wire up the button.

**Success criteria**
- Fitted flights rated "matches what I meant" at 3.5/5 or higher by the imitator.
- Faster than knobs on 3 briefs.

---

## Stretch ideas, noted and not pursued

- Supersonic flicks: track multiple retarded-time roots and render the Mach-cone "boom" plus the time-reversed approach.
- MPE or trackpad pressure as source height or size.
- iPhone gyro over OSC as a "wand".
- First-order Ambisonic or binaural output (SADIE II HRTFs) from the same per-ear geometry.
