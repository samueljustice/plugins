# Swooshbuckler knob audit

Every knob was swept across its travel through the real plug-in, offline, and measured at each step. Then the knobs were remapped so each part of their travel does something you can hear, and measured again. This page sets out what was found and what changed.

## How it was measured

[`tools/KnobAudit.cpp`](../tools/KnobAudit.cpp) builds as `SwooshbucklerAudit` (configure with `-DSWOOSHBUCKLER_TOOLS=ON`). It runs the plug-in at 48 kHz on synthetic sources:

- growls and a steady 330 Hz tone on Bus A;
- a wind bed and a hiss bed on Bus B;
- impacts on the hits;
- a wideband rumble and a steady low drone, which Stowaway takes by itself in Sub; any source can be sent to Stowaway (`--src wind@s`).

It sets a knob to each point of its travel, renders 20 s three times (from different stretches of the rig's random passes) and measures:

- loudness, peak and movement (the spread of momentary loudness: how big the swells are);
- spectral centroid and band levels, side/mid and pan movement, and fast level wobble;
- pitch range and median (on the tone), and energy outside the fundamental;
- hit-gate openings, transients per minute and limiter gain reduction.

A step counts as audible when its change is bigger than a just-noticeable difference and bigger than the spread between the three renders.

```
SwooshbucklerAudit --param aFilter --steps 11 --src growl --set aDrive=0 --work /tmp/run1
```

Each knob was swept in its default setting and in variations: other sources, other knobs that interact with it, Auto Ride off, Swell held, each gate mode, each tremolo shape and target, and each morph mode.

**What counts as usable:** each 10% of travel makes an audible, roughly even change, with no dead stretches and no cliffs. Both ends should be sounds you'd use. Characterful extremes are kept, even when they are extreme. The default sits somewhere sensible.

## Ride

| Knob | Before | Now |
|---|---|---|
| **Energy** | The fader dipped linearly in level, so 55–74% did nearly everything (movement 6 → 25 dB) and 74–100% did nothing at all: the faders were already reaching silence. At high Chaos, Chaos also lifted the dips, so Energy couldn't reach silence. | The dip falls 4 dB per 10% of travel, to −36 dB at 90%, then to silence. Chaos varies swell size without lifting the dips. The default is 35% (the same sound as before). Dimmed with Auto Ride off, when it does nothing. |
| **Rate** | Already close to even in speed (about 1.2× per 10%). The pass length was a straight line, so it changed slowly at the bottom and quickly at the top. | The ride law is unchanged. The pass length now follows Rate the same exponential way. The readout shows the swell time (25 s down to 1.9 s). |
| **Fill** | Inaudible below 20% and weak above that (at most +0.5–0.9 dB of echo). The echoes came before the fader, so they could never fill the ride's dips. | The echoes now come after the fader, with a stronger send (2·k^1.2). The default is 15%. Measured on growls at high Energy: the gaps between swells fall from 32% of the time to none, evenly across the travel, for about +2 dB of level at the top. |
| **Chaos** | The size of each swell barely changed. The random drift was added on top of the fader, so it lifted the dips. | Each swell cycle now gets a random depth (r·(1 − 0.6c·u)), and the speed wobble is stronger. The swell size spread grows from 0.9 dB to about 3 dB. 0% is still a repeating pattern. |
| **Track faders** | Linear in level: −inf to −20 dB in the bottom 10%, and only about 6 dB over the top half. | A 30 dB fader in dB: each 10% is 3 dB, and the last 5% fades to silence. The readout is in dB, and the default is −2.1 dB (the same as before). |
| **Pitch** | 4.8 semitones per 10%, so the ±7 semitone range presets use took up only the middle 30% of the travel. | A symmetric skew, finer near the middle: steps run from 3 semitones at the centre to 6 at the ends. |
| **Input** | Dead: the plug-in has no audio input, but the parameter still showed in the host. | Removed. |

## Bus A and Bus B (the controls they share)

| Knob | Before | Now |
|---|---|---|
| **Doppler** | Even. At 0% the bus collapsed to mono, because the time difference between the ears came from the pass speed. | The curve is unchanged. The time difference now comes from Width, so Doppler 0 and Bend 0 keep the stereo. |
| **Width** | Nearly inaudible below 30%, and dead at Doppler 0 (it was scaled by √Doppler). It only ever panned, and stopped short of the sides. | Width now sets both the pan and the time difference between the ears. The curve is width^0.8, with no Doppler factor, and it reaches the sides. The default is 70%. |
| **Bend** | Even all the way. | Unchanged. |
| **Low End** | Even all the way. | Unchanged. Bus B's default is now 30%. |
| **Swirl** | 0–20% was inaudible, and the knob turned the bus up by up to 5 dB (more limiting) rather than just colouring it. | A k^0.7 curve, and the flanger's level boost is removed. The level is matched over 300 ms, within ±6 dB, so loudness stays flat while the colour moves. The default is 30%. |
| **Filter** | Followed the bus's absolute level, so on Bus A it stayed fully open and the knob only turned the level down (up to −8.5 dB). On Bus B it had a cliff at 40–62%. Above 62% only the resonance changed. | It now follows each pass against its own peak. The cutoff at the peak falls with √Filter (21 kHz at 0% to about 280 Hz at 100%), and the wet mix comes in exponentially. A light resonance comes in over the top 40%, with its level trimmed. Measured: Bus A's top end goes −34 → −82 dB and Bus B's hiss centroid 10.7 kHz → 216 Hz, both evenly and in one direction. |
| **Drive** | Dead below 12%. 15–45% was mostly a 9–12 dB jump in loudness, and 75–100% was redundant. | The drive gain rises evenly in dB (0 to 26 dB, shaped k^0.7). A slow, partial level match keeps the loudness roughly steady (within about 1.5 dB on beds), while the swell gets denser and flatter, evenly across the travel. Bus A's default is 12%. |
| **Ducked by A** (B) | Keyed off A's absolute level, so it never got deeper than about −2.6 dB. Below 60% it was fluff. | Up to −20 dB at A's peaks, keyed by A against its own recent peak. Nothing ducks below −40 dBFS, so a near-silent A doesn't push B down. Mids mode now uses a proper 1 kHz band and reaches the same depth as Volume. The default is 35%. |
| **Filtered by A** (B) | Only worked when Filter was 0 or high. At Bus B's default Filter it did nothing. | Its own low-pass after the filter: 18 kHz down to about 280 Hz at A's peaks, independent of Filter. |
| **LINK / Take Turns** | Correct. | Unchanged. Take Turns uses the new duck depth. |

## Hits

| Knob | Before | Now |
|---|---|---|
| **Sensitivity** | AIM: silent at 0–5%, and identical from 60% up. Level: 55–100% was the same as Open. Jumps: 0–30% never opened. Open: dead. | AIM: an eased bar (0.80 − 0.65·s^1.55), with closeness weighted more than level. Jumps: 5.6 → 1.4 dB. Level: now relative to the recent peak (within 2 to 30 dB of it), so it works however the ride is set. Dimmed in GATE Open. |
| **Boost** | 0–17% (−6 to −3 dB) left the hits under the bed. | −3 to +12 dB, default +6 dB, and the readout is the gain played. |
| **Release / Hold** | Even. Dead in AIM and Open. | Unchanged. Now dimmed in Open as well as AIM. |
| **Floor** | −40 to −27 dB was inaudible under the bed. | Closed, or −28 to −3 dB, curved (−22 dB at 10%, −11.5 dB at 50%). |
| **Duck** | Back-loaded, and at most −8 dB. | dB-even, down to −12 dB at the hits' peaks. The readout is in dB, and the default is −4.8 dB. |
| **Return** | Even. | Unchanged. |

## Output and Peak Tremolo

| Knob | Before | Now |
|---|---|---|
| **Space** | A 1.5× mid/side widen ran across the whole range, pushing wide beds out of phase (correlation −0.38 on hiss). | The widen stage is removed. The echo and reverb carry the width. The default is 30%. |
| **Inflate** | Even but subtle (0.3–0.4 LU per 10%). | Up to 70% the effect blends in. The last 30% moves its curve from neutral to aggressive. |
| **Push** | 12–18 dB only squashed: no more loudness, swells crushed, pumping. | 0 to +15 dB (keeps a squash end for sparse beds). |
| **Ceiling** | The bottom half (−12 to −6 dB) duplicated Push and Trim, and the useful −2 to 0 dB had 17% of the travel. | −6 to 0 dB, finer near the top. |
| **Master Trim** | 0 to +6 dB pushed the output past the Ceiling (overs). | Silence to 0 dB. |
| **Tremolo Depth** | Linear in gain: 0–30% was under 3 dB of dip, and 90–100% was a cliff. | Dip = 1 − (1 − k)^1.6. The readout is the dip at the peak, in dB (Full at 100%). The default is 45% (−8.3 dB). |
| **Tremolo Rate** | Even. The readout showed odd decimals. | The readout is fixed. |
| **Tremolo Threshold** | The bottom half barely changed, and the last 2% was dead. | Spread over the whole travel: 0.98·(1 − (1 − k)^1.5). The readout is in dB under the recent peak. The default is 45% (the same sound). |

## Stowaway

Stowaway's five knobs were swept in each Mode, eleven steps each, 20 s three times. There is no Before column: this is how they were built. **Level** is Stowaway's own level over its loud moments, **movement** is how far that level rises and falls (its 95th percentile against its 10th), and LUFS is the whole output.

```
SwooshbucklerAudit --param stowFollow --steps 11 --src growl,drone --work /tmp/run1
SwooshbucklerAudit --param stowFilter --steps 11 --src wind@s --set stowMode=1 --bands --work /tmp/run2
```

### Sub

| Knob | Measured |
|---|---|
| **Follow** | On a steady drone under growls on Bus A, the sub's movement goes 1.0 → 12.6 dB, 1.1 to 1.3 dB for every 10% from end to end, with no dead stretch. Its level stays within 0.5 dB (−11.0 → −11.5 dB): the make-up holds. With nothing on Bus A or Bus B (its own tracks ridden instead) the movement is the same shape, 1.0 → 13.4 dB. |
| **Low End** | The sub's level rises 5.4 to 6.1 dB over the travel, about 0.6 dB per 10% and even (rumble −15.1 → −9.7 dB, drone −11.4 → −5.3 dB), and the output 1.8 to 3.6 LU. The centroid falls from 56 to 48 Hz as the octave below comes in. Nothing appears above 2 kHz at any setting (under −114 dB against the whole). |
| **Filter** | The crossover, 40 to 250 Hz, reading in Hz. On a rumble the output rises 12.7 LU over the travel, more per step at the bottom (4.2 LU from 40 to 53 Hz) than at the top (0.3 LU from 219 to 250 Hz), because that is where a rumble's energy is; the centroid climbs evenly, 31 → 79 Hz, about 5 Hz per 10%. The default, 120 Hz, is at 50%. |
| **Drive** | The sub's movement falls from 9.5 to about 8 dB on a rumble while its level rises 1.9 dB (1.5 dB on a drone). The output rises 4.2 LU, 0.2 to 0.5 LU per 10%, with the larger steps in the top half: the overtones Drive adds sit where the ear is more sensitive, so it sounds louder than its level says. Nothing appears above 2 kHz. |
| **Width** | Not used: a sub is mono. The knob rests and reads Mono. |

### Wake

| Knob | Measured |
|---|---|
| **Follow** | Under growls on Bus A, with hiss on Stowaway: the trail's movement goes 4.5 → 21.8 dB, 1.2 to 1.9 dB per 10%, even from 10% up. Its level stays within 0.8 dB (−11.2 → −10.8 dB). |
| **Low End** | On wind: the band under 200 Hz rises 4.0 dB against the whole (−7.0 → −3.0 dB) and the centroid falls 1278 → 857 Hz, evenly; the trail's level rises 2.2 dB. On hiss, which has nothing down there to work on, it does almost nothing (0.1 LU). |
| **Filter** | The tilt. On wind, below the middle the low-pass takes the centroid 1278 → 110 Hz and the output down 7.9 LU, most of it in the bottom 20% (as the cutoff falls under the wind's own body); above the middle the high-pass takes the band under 200 Hz from −7 to −50 dB and the centroid up to 5.7 kHz, for 3.7 LU. 40% to 60% is nearly open (0.2 LU): the middle is a safe place to leave it. On hiss the low-pass side is a 36 LU fall and the high-pass side is inaudible, as it should be. |
| **Drive** | On wind: the trail's movement falls 19.7 → 10.4 dB, about 1 dB per 10%, while its level stays within 0.5 dB. The output rises 4.1 LU as the trail gets denser. |
| **Width** | Side against middle rises −12.9 → −10.0 dB and the pan movement 0.15 → 0.34, evenly, 0.02 per 10% (the growls on Bus A are in the measure too). The trail's level does not change. |

### Fly

| Knob | Measured |
|---|---|
| **Follow** | Moves when the passes come, not how loud they are: level and movement stay within the spread between renders across the whole travel (movement 21 to 22 dB). The timing is checked by `SwooshbucklerStowaway`: at 100% every pass is at its closest with one of Bus A's, at 0% they keep their own time. |
| **Low End** | On wind: the band under 200 Hz rises 4.1 dB (−7.0 → −2.9 dB), the centroid falls 1073 → 706 Hz, the level rises 2.5 dB, all evenly. |
| **Filter** | The same tilt as in Wake, with the same shape: on wind 9.0 LU down over the low-pass half and 5.9 LU over the high-pass half, 40% to 60% open. |
| **Drive** | The level rises 1.9 dB and the movement falls 23.9 → 21.3 dB, evenly (0.1 to 0.3 dB per 10%); the output rises 1.5 LU. |
| **Width** | On wind, pan movement goes 0.03 → 0.38 and side against middle −13.5 → −5.6 dB, both evenly. On hiss the first 10% does more than the rest (correlation 0.96 → 0.08), because the time difference between the ears arrives with the first of the width; Bus A's Width does the same on hiss (0.76 → 0.34). |

### Dry

| Knob | Measured |
|---|---|
| **Follow** | Movement 4.5 → 15.9 dB, 0.7 to 1.3 dB per 10% and even; the level stays within 0.8 dB (−11.3 → −10.5 dB). |
| **Filter** | The same tilt: on wind 8.7 LU down over the low-pass half, 4.6 LU over the high-pass half, 40% to 60% open. |
| **Width** | Not measurable here: the tool's sources are mono, and on a mono sound Width does nothing in Dry (measured: no change at any setting). The stereo case is checked by `SwooshbucklerStowaway`. |
| **Low End**, **Drive** | Not used. The knobs rest. |

The factory presets were rendered too, on growls, wind, hits and a rumble with Stowaway switched on as a sub: the seven older presets come out within 1.1 LU of where they are without it (−9.5 to −11.2 LUFS), with the sub at −15 to −17 dB under them. The five Stowaway presets come out between −7.5 and −11.3 LUFS, depending on what is sent to Stowaway.

## Modulators and Morph

| Control | Before | Now |
|---|---|---|
| **LFO / Random rate** | 0.02–12 Hz. The top 20% was inaudible on most knobs (their glide smoothed it away), and the bottom 10% gave under half a cycle in a whoosh. | 0.05–8 Hz (20 s cycles to 8 Hz). Offsets on the knobs that glide are now added after the glide, smoothed per 32 samples, so fast rates come through without clicks. The readout shows the cycle time or the rate. |
| **Random / Steps** | Sat still for their whole first cycle (up to 50 s). | Start moving straight away. |
| **Follower speed** | 1 s to 10 ms. The fast end was finer than it updates, and the input window left a constant push. | 2 s to 26 ms, and the input window is −40 to −6 dBFS. The readout is in ms or s. |
| **Morph X** | A 5.4 dB dip in level in the middle. | An equal-power split. While morphing, B isn't ducked or filtered by A (they carry the same sound). |
| **Morph Y** | Blend: the bottom 40% was inert. Glide: the top half collapsed the level by up to 17 dB. Cross at Y 50% was the same as Blend. | Blend: a square law. Glide: the level holds (the correction is clamped, with a wider trim), and X is eased. Cross: a clean geometric swap. |

## Known limits

These were noted and left for later:

- Playing bright material fast (Pitch up, or VARI on with Semitones up) aliases, because the varispeed read has no anti-alias filter. It darkens beds above about +6 semitones.
- Doppler and Bend set each pass's speed when the pass starts, so a modulator on them changes the bend from the next pass on.
- The morph pad's wander runs in the editor, so it stops while the window is closed.
