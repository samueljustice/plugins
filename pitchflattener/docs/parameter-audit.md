# Pitch Flattener parameter audit

Every parameter and feature was checked two ways: measured, by sweeping it across its range on real recordings, and judged against what the plugin is for. The aim was a simpler plugin: knobs with no dead or harmful travel, and nothing on the panel that the job doesn't need.

## What the plugin is for

Pitch Flattener takes the pitch movement out of a sound and keeps its character: a Doppler sweep, a siren's wail, vibrato, a warbling tone. It is used mostly offline, in the Soundminer DSP rack or a DAW bounce, often on batches of files, so latency doesn't matter and nobody is turning knobs during a render.

## How it was measured

58 recordings, each played through its own Source Type with one parameter changed at a time:

| Source Type | Recordings |
|---|---|
| Siren | 22 (British and US police sirens, an air-horn siren, a siren Doppler pass, 4 synthetic) |
| Engine / Doppler | 8 (bus, police car and Charger pass-bys, prop-plane fly-bys, a processed spaceship pass, 2 synthetic) |
| Vocal / Speech | 7 (4 horses, 2 pigs, 1 synthetic voice) |
| Whistle / Tone | 7 (2 theremins, 4 feedback recordings, 1 synthetic whistle) |
| Instrument | 6 (2 solo violins, 3 processed synth and braam designs, 1 synthetic) |
| Drone / Hum | 5 pads and drones |
| Noisy Field | 3 (2 city ambiences, 1 synthetic) |

**Flatness** (0 to 1) is what the wobble warning measures: the share of the audible sound that gets a pitch, times the share of moments where the output's pitch can be measured near the target, times how close it lands (exp(-cents off / 40)).

Limits of the measurement:
- The output meter can't read polyphonic or heavily processed sounds (the drone pads, the processed traffic and braam designs, the city ambiences). 46 of the 58 recordings are readable; the tables use those.
- Flatness measures pitch only. It can't hear timbre, zipper noise or artefacts, so Formant, Mix, Amount, Quality's sound and Auto Window rest on the design, not the numbers.
- Auto mode can't be compared with Latch this way: its target follows the sound's own contour, so it always looks close to "its" target.
- Drone / Hum (2 readable recordings) and Noisy Field (1) are thin; treat their numbers as indications.

## Results and decisions

Median flatness of the readable recordings, averaged over Source Types so the many sirens don't dominate. The Source Type presets score **0.52** on that average.

| Parameter | What the sweep showed | Decision |
|---|---|---|
| **Source Type** | Each Source Type's own detector and settings are best or close to best for its sounds. The biggest gains available were preset retunes (below). | **Keep**, as the main control. Whistle / Tone and Drone / Hum retuned. |
| **Detector** | Each Source Type's own choice wins or ties for 5 of 7 types; a wrong manual pick costs a lot (pYIN: 0.00 on drones and noisy field). | **Advanced** (Custom only), no longer automatable. It stays as a rescue for sounds no Source Type handles. |
| **Min / Max Freq** | Has a real effect both ways: a floor at 0.25x the preset drops the average to 0.46; raising Noisy Field's floor 2x collapses it from 0.91 to 0. The right value depends on the file. | **Keep** on the panel (Range section). Range unchanged. |
| **Sensitivity** | Almost flat on average (0.51 to 0.53 across 0 to 100 %), but large and opposite per type: vocal improves as it rises (0.00 at 0 % to 0.10 at 100 %), whistle is best low (0.27 at 0–35 % against 0.13 at 50 %). | **Advanced.** Whistle / Tone now 30 %. |
| **Speed** | 0.5–1 oct/s is harmful (sirens 0.43, whistles 0.00); 2 to 40 oct/s is flat (0.51 to 0.52). Nothing to gain from adjusting it. | **Hidden**, set by the Source Type. Drone / Hum raised from 1 to 6 oct/s. |
| **Smoothing** | 0–8 ms is best everywhere; above 15 ms it only gets worse (sirens 0.68 → 0.41 at 50 ms). | **Hidden**, set by the Source Type. See open checks. |
| **Gap Fill** | No measurable effect from 0 to 400 ms (every type within 0.02). It also sets how far ahead a sound's correction starts, so it isn't removed from the processing. | **Hidden**, set by the Source Type. |
| **Gate** | -60 to -50 dB are best; whistles and pure tones lose out at -70 and -80 dB (0.13 → 0.05); -40 starts cutting sound (instrument 0.83 → 0.71), -30 and -20 destroy it (average 0.41, 0.11). | **Narrowed to -80…-40 dB**, kept on the panel: the right value depends on each file's noise floor. |
| **High-pass / Low-pass** (detection) | Flat for most types; the High-pass is the single biggest win for whistles and feedback (0.13 → 0.35 at 1.5 kHz) and the Low-pass helps animal calls (0.03 → 0.15 at 1 kHz), but each hurts other types. | **Advanced.** Per-type changes left for a listening check. |
| **Max Shift** | 1–4 semitones stops the flattening (sirens 0.00–0.22); 8–24 is fine; 18–24 helps whistles (0.13 → 0.31) and drones (0.30 → 0.47). | **Narrowed to 6–24 semitones, Advanced.** Whistle / Tone and Drone / Hum now 24 (Siren already was). |
| **Quality** | Tight beats Smooth on sirens (0.68 against 0.51), the same elsewhere. Smooth's cleaner sound on slow material can't be measured. | **Advanced**, both options kept. |
| **Target Mode** | Not comparable by measurement (see limits). | **Keep all three.** See "Is Latch needed?" below. |
| **Re-latch button** | — | **Removed.** It can't be pressed during an offline render and can't be automated, and Latch already re-latches after a second without pitch. |
| **Target Hz** | — | **Keep** (Fixed only). |
| **Auto Window** | Not comparable by measurement. | **Keep**, with more of the knob's travel given to 200–600 ms, the vibrato and warble zone. |
| **Amount**, **Mix**, **Formant** | Not measurable (they shape the result's sound, not its pitch accuracy). | **Keep** on the panel: partial flattening, a parallel blend that can't be rebuilt in the Soundminer rack, and natural voices are deliberate choices. |
| **Manual Override + Override Hz** | The code made Override do exactly what Fixed with Target Hz does. | **Removed** from the panel. Old sessions and presets with it on load as Fixed at the Override pitch. |
| **Wobble warning** | Uses the same measurement as this audit. | **Keep.** Its "or lower Speed" advice is replaced: lowering Speed never helped. |

### Is Latch needed?

Yes, and it stays the default. It is the only mode that flattens each file in a batch to that file's own pitch with nothing to set per file, which is what offline Soundminer work needs:
- **Fixed** needs a pitch typed in for every file.
- **Auto** deliberately keeps slow movement, so a Doppler drop or a slow wail partly survives it.

What went was the clutter around Latch: the Re-latch button and Manual Override.

### Source Type retunes

| Source Type | Change | Readable recordings, flatness before → after |
|---|---|---|
| Whistle / Tone | Detector Spectral Peak → Spectral + MPM, Sensitivity 50 → 30 %, Max Shift 12 → 24 | Theremins 0.04 → 0.15 and 0.05 → 0.39; feedback 0.00–0.41 → 0.58–0.83; synthetic whistle 0.99 → 0.99 |
| Drone / Hum | Speed 1 → 6 oct/s, Max Shift 12 → 24 | Drones 0.49 → 0.59 and 0.12 → 0.47 |

## The panel now

- **Flatten:** Target mode (Latch / Fixed / Auto), Target Hz (Fixed), Auto Win (Auto), Amount, Mix, Formant.
- **Range:** Min Freq, Max Freq, Gate, input level meter.
- **Advanced** (set by the Source Type, adjustable on Custom): Sensitivity, Max Shift, Quality, detection High-pass and Low-pass, and the Detector menu in the source strip.
- **Hidden, set by the Source Type:** Speed, Smoothing, Gap Fill.

## Old sessions and presets

No parameter was deleted, because hosts save and automate parameters by ID or position. Hidden and retired parameters keep their IDs and stored values.
- A session or preset with Manual Override on loads as Fixed at the Override pitch, with Override off.
- A stored Gate above -40 dB loads as -40 dB, and a stored Max Shift below 6 semitones loads as 6. Both old settings were already harmful.
- Detector, Speed, Smoothing, Gap Fill and the Override pair are no longer automatable. Automation already recorded on them, and on Gate, Max Shift or Auto Window (whose knob mappings changed), should be recorded again.

## Open checks (need ears, not the meter)

1. **Smoothing at about 5 ms** for every Source Type, which measured best: listen for zipper noise on engines, noisy field recordings and drones before changing the presets (currently 8–50 ms).
2. **Vocal / Speech on MPM with a 1–1.5 kHz Low-pass**, which measured better on the animal calls: check on human speech and singing first.
3. **Whistle / Tone High-pass at 600–800 Hz**: check it doesn't upset low theremin notes.
4. **Max Shift 24 for Engine / Doppler and Noisy Field**: listen for octave-error jumps.
5. **Latching to the most confident stretch** instead of the first ~300 ms, for quiet Doppler approaches. Fixed covers that case for now.
