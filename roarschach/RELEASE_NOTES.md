## Roarschach 1.1.0

Roarschach 1.1.0 adds a fifteenth module, **CHAMBER**: the voice played through a small speaker into a tight, resonant room and blended back under it, with eight bundled rooms and a Width control. The creature has a new renderer, every knob was measured and remapped so its whole travel does something, the factory presets have been redesigned, with ten new ones, and three kinds of die roll new sounds.

Parameter IDs, their order and their types are unchanged, so sessions, presets and hosts keep finding every control. CHAMBER's nine parameters are new, and are added after the existing ones.

### New: CHAMBER

CHAMBER plays the voice through a small speaker into a tight, resonant room, then blends it back under the untreated voice. A little of it gives a voice an edge and a sense of size. More of it puts the voice inside a box, a booth or a can. It is not a hall reverb: the rooms are small and they ring.

| Control | Range | Default | What it does |
|---|---|---|---|
| **Chamber** | 0 to 100% | 50% | How much of the voice played into the room is blended under the dry voice. 15–35% adds a subtle edge, 45–65% is a clear chamber; around 80% the two are equal; at 100% the room leads. |
| **Resonance** | 0 to 100% | 40% | How much the room rings after each sound. Lower is tight and boxy; higher lets its resonances sustain longer. The level stays the same. |
| **Edge** | 0 to 100% | 30% | Drives the small speaker into breakup: grit at 15–35%, a cranked speaker at 50–70%, torn and buzzing towards 100%. Heard once Chamber is about 60% or more. |
| **Width** | 0 to 100% | 35% | How far the room spreads to the sides. 0% keeps it centred; 100% is the room's full stereo. Summed to mono it sounds the same at any Width. |
| **Room** | 8 rooms | Concrete Room | Steel Box, Resonant Chamber, Concrete Room, Tin Booth, Padded Cell, Wood Chamber, Timber Room, Soft Bloom. |
| **Speaker** | Tiny / Small / Horn | Small | Tiny is thin and nasal, Small a compact full-range box, Horn a narrow, honking paging horn. |
| **Boom** | Off / Light / Medium / Heavy | Light | Low-end weight added after the whole chain: a shelf at 110 Hz of +3, +6 or +9 dB. |

- **It moves like any other module.** By default it sits right after PITCH, so the pitched voice is in the room. After ROAR it becomes an amp into a cabinet; at the end of the chain it puts the whole creature in the room.
- **It is Off in Init and in every 1.0.0 session**, so it changes nothing in a 1.0.0 session until you turn it on.
- **Latency is unchanged.** CHAMBER adds none, at any setting or position, and it runs at the host's sample rate. With Width above 0% the chain is stereo from CHAMBER on (the ST badge marks it).
- **The rooms are bundled.** They are licensed for use only within Roarschach; see [LICENSE-IRS](https://github.com/samueljustice/plugins/blob/main/roarschach/LICENSE-IRS). The code stays MIT.
- **The creature hears the room.** While CHAMBER rings, its wings and tendrils trail each movement for longer, and Boom adds a slow swell to its breathing.

### New: the dice

- **Random preset**: a die beside the preset name loads a random factory preset, never the one showing. Every factory preset comes up once before any comes up again.
- **Randomise all**: a die after the last module rolls a new creature. It turns three to seven modules on (at least two that change the voice) and gives every module's knobs and modes new settings.
- **One module**: a small die beside each module's mode chip rolls just that module's knobs and modes, and leaves its On switch and its place in the chain alone.
- **Always in the useful range.** Each knob rolls only inside the part of its travel the knob audit found useful, so a roll never lands in a dead or harsh corner. The dice never move a module or touch IN or OUTPUT. On five real voice takes, 200 rolls each, 998 of the 1,000 rolls played within 3 LU of Init (the other two 3.0 and 3.8 LU quieter) and none went over the ceiling.
- **Undo.** Every roll is one step: right-click any die (or press Shift+F10 on it) for Undo and Redo, or press Cmd+Z (Ctrl+Z) straight after clicking it. A knob you turn after a roll keeps your setting through undo and redo. The host records each control a roll changes, and a roll marks the preset as edited.

### New: the creature

The creature is drawn by a new renderer. Its clouds swirl as a moving fluid, and the creature is lit ink smoke instead of outlines, with its eyes and throat glowing through it. A top layer of pale, cool blue-grey mist veils everything: it thins a little on loud roars and around the eyes, and thickens when the creature sleeps. Its jaw now follows each phrase, with weight, instead of snapping open on every syllable. Its colour follows your settings: the clouds drift slowly towards ember red as the drive and growl get heavier, a colder electric blue with the vocoder, ring and shimmer, violet with swirl, ensemble and overtone, and a lichen green-grey in CHAMBER's room, and stay pale blue-grey at Init. On slower graphics cards it lowers its own detail to stay smooth.

### Your sessions and presets

- **Sessions and user presets saved with 1.0.0 load, with every remapped knob converted to the nearest setting under the new mapping.** Most sound the same or very close. Some may sound slightly different, because one knob now does more than it did, or the module itself improved:
  - **Gate** keeps its depth, but its threshold and ratio now move with the knob too, so it closes on slightly more or fewer of the gaps (25% converts exactly).
  - **Swirl** keeps its mix, but its resonance has changed at low and high settings.
  - **Detail Low** lifts less room tone between words than it did, at every setting.
  - **Ensemble** above 70% loads as 100%, the new top.
  - **De-ess**, **Roar Drive** and **Overtone** convert closely, within the steps you can hear.
  - The fixes below also change these settings where they were broken: Sub Drive, Sub Tight and Deep, Growl Rattle and Character, Ensemble motion, the Square ring carrier, and the output's DC removal.
- Saving the session or the preset again stores the new values.
- **Host automation cannot be converted.** Hosts store automation as knob positions, not values, so an automation lane on any knob in the table below now plays back a different value. Check automated sessions, and redraw lanes on remapped knobs where needed.
- Input with Auto Level on: values inside ±12 dB had no effect in 1.0.0, so they load as 0 dB. Values beyond that load as twice the excess.
- Ring Amount in AM mode loads as the square of its old value (the same sound).
- Sub Drive loads as 20%, the clean octave that 1.0.0 produced at any setting.
- Ensemble with the Vibrato motion loads as the amount that matches what 1.0.0 played (every amount from 10% up sounded the same there).
- Growl Rate below 2 Hz loads as 2 Hz, Shimmer Rate above 25 Hz loads as 25 Hz, and Attack below 0.2 ms loads as 0.2 ms.

### Knob changes

Every knob was measured on real voice recordings, step by step across its whole travel. Where part of a knob's travel did nothing, or the useful part was squeezed into a corner, the knob has been remapped. "u" is the knob's position from 0 to 1. Knobs still show their own units (%, Hz, dB, ms). The measurements, and the reason for each change, are in [docs/parameter-audit.md](https://github.com/samueljustice/plugins/blob/main/roarschach/docs/parameter-audit.md).

| Control | 1.0.0 | 1.1.0 | Why |
|---|---|---|---|
| Input (Auto Level on) | Ignored inside Auto Level's −12…+20 dB window | Moves the levelled voice by half its value (±12 dB) | Most of the travel was dead with Auto on |
| Auto Level | Gain −12…+20 dB | Gain −12…+30 dB | Quiet takes arrived 10 dB short |
| PITCH Drive | Drive gain 24·u^1.5 dB | Drive gain 24u dB | 10–70% did little; the grit came only at the top |
| Contain | u | u^0.7 | 0–20% was dead |
| Gate | Ratio 2.5, depth −40u dB, threshold 10 dB above the room floor | Ratio 1.5 + 3.5u, depth −30u dB, threshold 6 + 12u dB above the room floor | The top 60% was unreachable, and the gate rarely closed on short gaps |
| De-ess | Range −24u dB, widened by ROAR Drive | Range −15u dB, plus up to 9 dB more and a detector threshold up to 12 dB lower above 40%; no link to ROAR | 60–100% sounded the same |
| Detail Low / High | Depth 48·u^1.68 dB | Depth 34·u^1.2 dB, and no lift of the room tone between words | Dead bottom; the top lifted room tone by up to 35 dB |
| Gravel | u | 0.15 + 0.85u (faded in over the first 1%) | 0–15% was dead |
| Ring Amount (AM) | Index u/2 | Index √u/2 | 0–30% was dead |
| Ring Freq, Ring Ratio | Power-law taper | Log taper (150 Hz at 38%, ×1 at 50%) | The bottom quarter was dead |
| Shimmer | u | 0.65u | 65–100% ran backwards |
| Shimmer Rate | 0.5–40 Hz | 0.5–25 Hz, log taper | Dead bottom, unusable top |
| Growl Depth | 0.85u | 1 − 10^(−18u/20) | Dead bottom |
| Growl Rate | 1–150 Hz | 2–150 Hz, log taper | The bottom quarter covered 1–2 Hz |
| Swirl | u; the resonance stopped growing from about 60% | 1 − (1 − u)^1.5; the resonance keeps growing | Dead bottom, and 50–70% did nothing |
| Swirl Rate | Power-law taper | Log taper over 0.05–10 Hz | The bottom 30% covered 0.05–0.14 Hz |
| Roar Drive | Scoop −min(7, 14u) dB; drive 18u dB | Smooth scoop; drive 18u + 48·max(0, u − 0.5)² dB | A kink at 50%, a weak top |
| Roar Tone | Strength = Drive | The same strength (1.5) at every Drive | Did little below 50% Drive |
| Ensemble | u (Vibrato: at least 70% wet) | 0.7·u^0.6 (Vibrato: its square root) | Seasick top, a bottom lost in the mix; Vibrato ignored the knob |
| Width | Up to an out-of-phase top | 150% is where 105% was | Out-of-phase top |
| Tame | Its curve at u | Its curve at 2.2·u^0.7 | Under-powered, weak first steps |
| Sub Level | −14 + 26·u^0.55 dB | −14 + 24·√u dB | The top 15% cost mids for little sub |
| Overtone Amount | −20 + 26·u^0.6 dB | Add: a gain of 1.83·√(10^u − 1), up to +14.8 dB. Lean / Replace: −10 + 20·u^0.6 dB | 0–40% was dead in Add, and weak over the whole travel |
| Push | Linear | 10 dB at mid travel; relative to Ceiling | Dead top; Ceiling also acted as drive |
| Clip | Linear | 5 dB at mid travel | Two thirds of the travel was dead at typical Push |
| Clip Shape | Knee 0.5·(1 − s), capped by Clip | Knee min(0.5, cap)·(1 − s) | Dead below a Clip-dependent point |
| Attack | 0.05–10 ms | 0.2–10 ms, log taper | Dead bottom |
| Release | Power-law taper | Log taper; with Auto on, the fast stage follows every setting | Dead bottom, and dead with Auto on |
| Mix | Raw dry, linear blend | Level-matched dry, equal-power blend | A loudness dip in the low range |

### Fixes

- **Sub Drive** is now heard. Up to about 20% the octave stays clean; above that it thickens into a growl. Kaiju, Hellhound and Sub Furnace keep their Drive and now growl underneath; the other presets use 20%.
- **Sub Register**: Tight keeps the octave at 55 Hz or above. Deep goes a full octave down to about 32 Hz, then moves up an octave.
- **Growl**: the Rattle shape now rattles at normal speaking levels (it sounded the same as Sine). Warm and Grind are stronger, and Grind keeps the tremolo as deep as Warm. Growl Rate is dimmed when Shape is Throat, because Throat follows the voice.
- **Ensemble**: the 6.2 Hz pitch shimmer has the same small depth in every motion, so Slow is no longer the wobbliest motion.
- **Ring**: the Square carrier stops at 10 kHz, which removes its fizz above 8 kHz.
- **Overtone**: in Lean and Replace, the low cut grows with Amount, so a small Amount no longer acts as a plain high-pass. The crossover stays at 300 Hz or below.
- **Output**: the limiter and the clipper no longer leave a DC offset on heavy, asymmetric sounds. Ceiling now sets only the output level; Push sets the drive.
- **Detail** no longer lifts the room tone that CLEAN's gate is taking out, and it follows the quiet between words, so it lifts breaths, not the noise floor.
- **The window** can no longer be made larger than the screen.

### Factory presets

The factory presets have been redesigned on the new mappings. Each keeps its name, its group and its idea, but its settings now sit in the useful part of every knob, so each one is more clearly itself. Values that sat where a knob did nothing (a chorus too low to hear, a growl under its threshold, a shimmer masked by the chain) now either do something or are switched off. Values that sat where a knob misbehaves (a chorus width that thins in mono, a sub level that costs the voice its presence, an Ensemble that turned seasick) moved back into range. Init plays the defaults, with CHAMBER Off. Your own presets and sessions are not affected.

Every preset except Loud & Contained, Crunch Lift and Smashed plays within about 1.5 LU of Init. Those three are louder on purpose (about +3, +1.5 and +5 LU).

Ten new presets:

- **Simple Size** (Voices): a subtly bigger, wider voice with a touch of chamber and low-end weight, and no creature effects.
- **Iron Tyrant** (Machines): a low, driven voice through a horn speaker into a steel box, with metal modes ringing and a heavy low end.
- **Tin Can Drone** (Machines): talking down a pipe, through a tiny speaker in a ringing tin booth.
- **Symbiote** (Creatures): a liquid alien voice built from many subtle layers (a wobble, a fine chop, a phaser, warm tube drive, a near-mono chorus and a tight octave underneath) that still keeps every word.
- **Upside Down** (Voices): a deep, creaking villain voice: your own voice kept at pitch and in the centre, levelled in gentle stages, with low-end girth and a clean octave underneath, in a wide, ringing room.
- **Use Your Words** (Voices): a commanding voice from inside the listener: an octave down through a rattling speaker into a small room with a heavy thump and a crowd of voices, blended with your natural voice.
- **Spore Throat** (Creatures): a dry, human-sounding horror throat, narrowed to its mid-range screech with the wet mouth clicks lifted and a rattle that speeds up as you get louder. Perform the clicks and screeches yourself.
- **Hoarse Hoarder** (Creatures): an enormous beast that still speaks clearly: eight semitones down with a lower throat, over a gravelly vocoder bed and a light rumble, in a wide, ringing chamber.
- **Angry Pepper Pot** (Machines): a furious voice in a metal shell, chopped by a low ring modulator into a staccato electronic stutter. Shout, and pitch your delivery up.
- **Ball Bearing** (Machines): a small, chirpy machine: an octave up, stepped to semitones, with a faint synthetic buzz, a quick warble and a tiny speaker. Coo and chirp short syllables.

**Cyber Commander** (Machines) is the 1.0.0 cyborg preset under a new name. It keeps its chain (tone shaping, then a ring modulator in a small resonant box, then a slight pitch drop), and the gravel on its peaks is now audible.

Some of the other changes you will hear:

- **Big Bad Narrator** sits in a warm wooden room. **Cave Troll**, **Gargoyle** and **Demon Whisper** put the whole creature in a room, with CHAMBER last.
- **Hellhound** snarls harder, **Gravel Throat** rasps drier, **Golem** stomps without any wobble, and **Throat Singer**'s second, lower pitch is louder.
- **Liquid Beast** swells slowly instead of warbling. **Kraken** and the wide chorus presets (Molten Chorus, Clanking Choir, Pulse Choir) keep their width but thin less when summed to mono.
- **Two Octaves Down**, **Vocoded Fire** and **Gravel Pit** are now clearly apart: dark and heavy, bright and burning, and chopped.
- **Robot Ogre** gains a metallic ring edge and a square stutter. **Ribcage** builds its roar on loud low overtones.

### Downloads

| File | Contents |
|---|---|
| `Roarschach-1.1.0-macOS-Installer.pkg` | macOS installer (AU + VST3, universal) |
| `Roarschach-1.1.0-Windows-Installer.exe` | Windows installer (VST3) |
| `Roarschach-1.1.0-macOS-AU.zip`, `Roarschach-1.1.0-macOS-VST3.zip`, `Roarschach-1.1.0-Windows-VST3.zip` | The plug-ins on their own |

The macOS downloads are signed and notarised. Universal (Apple silicon and Intel), macOS 11 or later. Windows: 64-bit Windows 10 or later.

The manual, with a tour of every control: [roarschach/README.md](https://github.com/samueljustice/plugins/blob/main/roarschach/README.md).
