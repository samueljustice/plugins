## Roarschach 1.0.1

A tuning release. Every knob was measured on real voice recordings, step by step across its whole travel. Where part of a knob's travel did nothing, or the useful part was squeezed into a corner, the knob has been remapped so its whole travel does something. A few controls also had defects that are now fixed. Parameter IDs, their order and their types are unchanged, so sessions, presets and hosts keep finding every control.

The measurements, and the reason for each change, are in [docs/parameter-audit.md](https://github.com/samueljustice/plugins/blob/main/roarschach/docs/parameter-audit.md).

### Your sessions and presets

- **Sessions and user presets saved with 1.0.0 load with the same sound.** When they load, every remapped value is converted to the value that sounds the same under the new mapping. This is exact for Contain, Detail, Gravel, Shimmer, Swirl, Ensemble, Width, Growl Depth, Tame, Sub Level, Overtone, Clip Shape and Push. For Gate, De-ess and Roar it is close, within the steps you can hear. Saving the session or the preset again stores the new values.
- **A 1.0.0 session may still sound slightly different** where a fix makes a setting do what it says: Sub Drive, Sub Tight and Deep, Growl Rattle and Character, Ensemble motion, the Square ring carrier, and the output's DC removal. The details are under "Fixes" below.
- **Host automation cannot be converted.** Hosts store automation as knob positions, not values, so an automation lane on any knob in the table below now plays back a different value. Check automated sessions, and redraw lanes on remapped knobs where needed.
- Input with Auto Level on: values inside ±12 dB had no effect in 1.0.0, so they load as 0 dB. Values beyond that load as twice the excess.
- Ring Amount in AM mode loads as the square of its old value (the same sound).
- Sub Drive loads as 20 %, the clean octave that 1.0.0 produced at any setting.
- Ensemble with the Vibrato motion loads as 70 %, the amount that matches what 1.0.0 played (every amount from 10 % up sounded the same there).
- Growl Rate below 2 Hz loads as 2 Hz, Shimmer Rate above 25 Hz loads as 25 Hz, and Attack below 0.2 ms loads as 0.2 ms.

### Knob changes

"u" is the knob's position from 0 to 1. Knobs still show their own units (%, Hz, dB, ms).

| Control | 1.0.0 | 1.0.1 | Why |
|---|---|---|---|
| Input (Auto Level on) | Ignored inside Auto Level's −12…+20 dB window | Moves the levelled voice by half its value (±12 dB) | Most of the travel was dead with Auto on |
| Auto Level | Gain −12…+20 dB | Gain −12…+30 dB | Quiet takes arrived 10 dB short |
| Contain | u | u^0.7 | 0–20 % was dead |
| Gate | Ratio 2.5, depth −40u dB | Ratio 1.5 + 3.5u, depth −30u dB | The top 60 % was unreachable |
| De-ess | Range −24u dB, widened by ROAR Drive | Range −15u dB, no link to ROAR | 60–100 % sounded the same |
| Detail Low / High | Depth 48·u^1.68 dB | Depth 34·u^1.2 dB, and no lift of gated room tone | Dead bottom; the top lifted room tone by up to 35 dB |
| Gravel | u | 0.15 + 0.85u (faded in over the first 1 %) | 0–15 % was dead |
| Ring Amount (AM) | Index u/2 | Index √u/2 | 0–30 % was dead |
| Ring Freq, Ring Ratio | Power-law taper | Log taper (150 Hz at 38 %, x1 at 50 %) | The bottom quarter was dead |
| Shimmer | u | 0.65u | 65–100 % ran backwards |
| Shimmer Rate | 0.5–40 Hz | 0.5–25 Hz, log taper | Dead bottom, unusable top |
| Growl Depth | 0.85u | 1 − 10^(−18u/20) | Dead bottom |
| Growl Rate | 1–150 Hz | 2–150 Hz, log taper | The bottom quarter covered 1–2 Hz |
| Swirl | u | 1 − (1 − u)^1.5 | Dead bottom |
| Swirl Rate | Power-law taper | Log taper over 0.05–10 Hz | The bottom 30 % covered 0.05–0.14 Hz |
| Roar Drive | Scoop −min(7, 14u) dB; drive 18u dB | Smooth scoop; drive 18u + 48·max(0, u − 0.5)² dB | A kink at 50 %, a weak top |
| Roar Tone | Strength = Drive | Strength = 1.5·max(Drive, 50 %) | Did nothing at low Drive |
| Ensemble | u (Vibrato: at least 70 % wet) | 0.7u (Vibrato: its square root) | Seasick top; Vibrato ignored the knob |
| Width | Up to an out-of-phase top | 150 % is the old 105 % | Out-of-phase top |
| Tame | Its curve at u | Its curve at 5/3·u | Under-powered |
| Sub Level | −14 + 26·u^0.55 dB | −14 + 24·√u dB | The top 15 % cost mids for little sub |
| Overtone Amount | −20 + 26·u^0.6 dB | −10 + 20·u^0.6 dB | 0–40 % was dead in Add |
| Push | Linear | 10 dB at mid travel; relative to Ceiling | Dead top; Ceiling also acted as drive |
| Clip | Linear | 5 dB at mid travel | Two thirds of the travel was dead at typical Push |
| Clip Shape | Knee 0.5·(1 − s), capped by Clip | Knee min(0.5, cap)·(1 − s) | Dead below a Clip-dependent point |
| Attack | 0.05–10 ms | 0.2–10 ms, log taper | Dead bottom |
| Release | Power-law taper | Log taper; with Auto on, the fast stage follows every setting | Dead bottom, and dead with Auto on |
| Mix | Raw dry, linear blend | Level-matched dry, equal-power blend | A loudness dip in the low range |

### Fixes

- **Sub Drive** is now heard. Up to about 20 % the octave stays clean; above that it thickens into a growl. Kaiju, Hellhound and Sub Furnace keep their Drive and now growl underneath; the other presets use 20 %.
- **Sub Register**: Tight keeps the octave at 55 Hz or above. Deep goes a full octave down to about 32 Hz, then moves up an octave.
- **Growl**: the Rattle shape now rattles at normal speaking levels (it sounded the same as Sine). Warm and Grind are stronger, and Grind keeps the tremolo as deep as Warm. Growl Rate is dimmed when Shape is Throat, because Throat follows the voice.
- **Ensemble**: the 6.2 Hz pitch shimmer has the same small depth in every motion, so Slow is no longer the wobbliest motion.
- **Ring**: the Square carrier stops at 10 kHz, which removes its fizz above 8 kHz.
- **Overtone**: in Lean and Replace, the low cut grows with Amount, so a small Amount no longer acts as a plain high-pass. The crossover stays at 300 Hz or below.
- **Output**: the limiter and the clipper no longer leave a DC offset on heavy, asymmetric sounds. Ceiling now sets only the output level; Push sets the drive.
- **Detail** no longer lifts the room tone that CLEAN's gate is taking out.

### Factory presets

Each preset keeps its sound, converted to the new mappings. These presets change on purpose:

- **Overtone**: presets that had Overtone at 40 % or less now use 10 %, a light touch you can just measure. Their old values all sat in the dead zone.
- **Respirator Rasp**: Detail Low goes to 100 %, which matches the old 80 %. The old 85 % lifted the room tone.
- **Kraken**: its swirl was inaudible. It now uses the Deep 6 circuit, the LFO+Level motion and 0.25 Hz.
- **Crunch Lift**: Clip is 1 dB, so the fast release does the crunching. Mix is 90 % and Push 7 dB, which keep its old loudness under the new Mix law.
- **Golem, Symbiote, Throat Singer** (Tight register): the sub now stays at 55 Hz or above.
- **Cave Troll** (Deep register): on low voices the sub moves up an octave instead of going below 32 Hz.

### Downloads

| File | Contents |
|---|---|
| `Roarschach-1.0.1-macOS-Installer.pkg` | macOS installer (AU + VST3, universal) |
| `Roarschach-1.0.1-Windows-Installer.exe` | Windows installer (VST3) |
| `Roarschach-1.0.1-macOS-AU.zip`, `Roarschach-1.0.1-macOS-VST3.zip`, `Roarschach-1.0.1-Windows-VST3.zip` | The plug-ins on their own |

The macOS downloads are signed and notarised. Universal (Apple silicon and Intel), macOS 11 or later. Windows: 64-bit Windows 10 or later.

The manual, with a tour of every control: [roarschach/README.md](https://github.com/samueljustice/plugins/blob/main/roarschach/README.md).
