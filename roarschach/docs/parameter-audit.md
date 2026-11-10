# Roarschach parameter audit

This audit checks every knob and switch in Roarschach against real voice material. For each control it records which part of the travel does something useful, which part does nothing ("dead"), and which part does something you would not want ("unusable"). It then sets out the remaps that make each knob's full travel useful, with an even feel from one end to the other.

The audit was run on the 1.0.0 build. The "Changes in 1.1.0" section lists what changes as a result. Parameter IDs, their order and their kinds (float, choice, toggle) do not change.

A review of the first set of remaps found knobs that still had dead or weak travel: De-ess above 70 %, Tame on the reference take, Gate on takes whose gaps are short, DETAIL's floor guard when the gate does not close, Overtone in Add over its whole travel, Ensemble below 40 %, Swirl at 50–70 %, Roar Tone at moderate Drive, PITCH Drive at 10–70 % and Overtone Frequency in Add. Their fixes are part of 1.1.0, and "Verification of the 1.1.0 laws" below gives the before and after measurements.

## Method

### Test material

- **Voice excerpts A and B:** two excerpts of a real voice recording at 96 kHz. A is a longer talking passage with short pauses (about 114 Hz average f0). B has longer pauses and more room tone.
- **Reference take:** a reference dry voice take at 48 kHz, 4.7 s, close-miked.
- **Level variants:** excerpt A scaled to −6, −18 and −30 dBFS peak, for the level-dependent controls (IN, PITCH Drive, ROAR).
- **Sines:** 55 Hz, 110 Hz (−12 dBFS) and 997 Hz (−20 dBFS), for distortion (THD), harmonic line levels, the limiter and the clipper.

### Renders

Every render used the test runner's offline render mode (`RoarschachTests --render`). It renders a file at its own sample rate, can load a preset, set any parameter, isolate modules (`--only XX`) or drop them (`--without XX`), and sweep one parameter over values or knob positions (`--sweep … --norm`). About 2,000 renders were made in total:

- every continuous parameter over its full range in 11 steps (more for the frequency knobs), and every option of every choice;
- each sweep in three contexts:
  - the **Init** preset (full chain);
  - the module **on its own**, with IN Auto Level off, so nothing else masks it;
  - **two or three factory presets** where the parameter matters most;
- 2-D grids for the interactions listed below.

### Metrics

Each render was measured per step. These are the measurements:

| Metric | What it measures |
|---|---|
| Loudness, LRA, pumping | Integrated loudness (LUFS), loudness range, and short-term loudness spread on voiced frames (LU) |
| dSpec | Mean absolute 1/3-octave change from the previous step, 100 Hz–10 kHz (dB) |
| dMod | Largest change of amplitude-modulation depth in the 1–150 Hz modulation bands |
| Centroid, band shares | Spectral centroid, and the energy share of 40–80, 160–500 Hz and similar bands |
| HNR | Harmonics-to-noise ratio of voiced frames (roughness) |
| Above 8 kHz | Voiced energy above 8 kHz (fizz and harshness) |
| Intelligibility | Correlation of the 1–4 kHz envelope with the input (1.0 = identical word shapes) |
| Crest, true peak, clamps, DC | Density and safety |
| Pitch wobble | Pitch deviation from the input in cents, P95 (modulation effects) |
| Spectral motion | How much the spectral shape moves over time compared with the input (modulation effects) |
| Mono loss, L/R correlation | Fold-down safety (stereo effects) |
| Room-tone lift | Level of the quietest 15 % of frames relative to speech, compared with the input |
| THD, line levels | On sine inputs |

### Thresholds

A step counts as **DEAD** when every delta is under its threshold:

- 0.3 LU loudness
- 0.4 dB dSpec
- 0.02 modulation depth
- 0.5 dB width
- 0.5 % THD

A step counts as **RUNAWAY** when one delta is more than 6 times that sweep's median step.

A point counts as **BROKEN** if any of these hold:

- it clamps or produces non-finite samples;
- its loudness falls more than 6 LU under the sweep's median;
- its intelligibility falls under 0.4;
- it turns **HARSH**: voiced energy above 8 kHz rises more than 6 dB in one step.

### When the automatic metrics were overridden

The automatic flags are a first pass. These cases were judged on the measurement that fits the effect instead:

- **Narrow-band processors (TAME, DE-ESS, SUB):** dSpec averages over 100 Hz–10 kHz, so it reads these as DEAD even when their own band moves several dB. They were judged on their own bands.
- **Drive:** low-order harmonic distortion on a voice barely moves the 1/3-octave metrics. Drive was judged by the distortion residual left after a least-squares FIR fit per 1 s segment.
- **Time-varying effects (SHIMMER, SWIRL, ENSEMBLE):** these were judged by spectral motion and pitch wobble.
- **HARSH after big pitch shifts:** when PITCH relaxes a large downward shift, energy above 8 kHz climbs back from about −105 dB. That trips the HARSH test without ever going above the dry input's own level. Such flags were discarded.

## Summary

Roughly a third of all knob travel was dead or unusable in 1.0.0. The causes fall into five groups:

1. **Hertz, ratio and millisecond knobs were not log tapers.** They used a power-law skew around a centre value, so the bottom 10–25 % of the travel covered almost nothing:
   - Growl Rate: 1.0 → 1.4 Hz over the first 20 %
   - Ring Freq: 20 → 24 Hz over the first 25 %
   - Swirl Rate: 0.05 → 0.14 Hz over the first 30 %
   - Release: 1.0 → 1.08 ms over the first 10 %
2. **Laws that saturated early.** De-ess stops changing at 60 %. Clean Gate stops changing at about 40 %. Shimmer turns back on itself above 60 %. Ensemble flattens above 80 %. ROAR Drive has a kink at 50 %.
3. **Laws that started late.** Gravel does nothing for its first 15–20 %. Growl Depth does nothing for its first 20–30 %. Ring Amount in AM mode does nothing for its first 30 %. Swirl is convex.
4. **Laws that ran too far.** DETAIL Low/High at 85–100 % lifts room tone by 15–20 dB. Ensemble Width above 105 % goes out of phase.
5. **Controls that did nothing at all:**
   - Sub Drive (its products were filtered out);
   - the Rattle growl shape (its threshold sat above any real level);
   - Overtone Amount in Add mode below 40 %;
   - Clip Shape at low Clip settings;
   - Attack, Release and Auto Release at every factory preset's Push setting.

Nothing produced clamps, NaNs or broken output, with one exception: a DC offset after the asymmetric clipper at extreme Push.

## Findings per module

In the tables below:

- **Usable** is the travel where every step is audible and nothing is unwanted.
- **Dead** is the travel where steps cannot be heard.
- **Unusable** is the travel where steps are audible but harmful.

### IN

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Input (inTrim) | −24…+24 dB, 0 | Auto off: all. Auto on: only beyond ±12 dB | Auto on: −8…+12 dB. Auto Level cancels the trim inside its gain window, so the centroid moves only ±10 Hz over −12…+12 | Auto off, +16…+24 dB on a hot take: peaks of +10…+18 dBFS into the chain, intelligibility 0.92 → 0.79. Auto off, below −6 dB: the output level match cannot make the loss up, so the trim also turns the output down (about 0.8 LU per dB) | Linear dB, right | **With Auto on, Input now offsets Auto Level's target** by half its value (±12 dB). The knob then means "how hard the voice hits the chain" in both modes. Range and display stay the same |
| Auto Level (inAuto) | On/Off, On | On | – | Very quiet takes are under-corrected. The +20 dB gain cap leaves a −30 dBFS-peak take about 10 dB short, so ROAR and PITCH Drive barely act on it | – | **Gain cap raised from +20 to +30 dB.** Learning is already gated to voiced frames, so room tone cannot drive the gain up |

### PITCH

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Pitch | −24…+12 st, −2 | Follow: −15…+6 st. Keep: −18…+12 st | Follow, +7…+12 st: the throat stops following (the formant read rate is capped at +6 st), so the change per semitone halves. The pitch itself stays exact | Follow below about −18 st: on 96 kHz material the words blur (intelligibility 0.72 at −18, 0.53 at −24). Keep below −15 st: a strong pulse buzz, though the words stay clear | Linear semitones, even (about 0.9–1.0 dB of spectral change per semitone) | **No change.** Tooltip gives the zones (see the guide below). In Mono mode the knob is an absolute note (0 st = C3), and the tooltip says so |
| Size | 0–100 %, 0 | 0–100 % | None on the knob. With Follow at −18 st or lower, the throat is already at its floor, so Size adds little | – | Linear, even (centroid about −3 % per 10 %) | **No change.** Tooltip notes the floor below −18 st |
| Drive | 0–100 %, 1 | 0–100 %: up to 25 % warmth (mostly the 2nd harmonic), 35–70 % grit, 100 % heavy (40 % / 50–80 % under the previous law) | The spectral metrics read 10–70 % as dead in Init, but the distortion residual rises (−25.9 → −6.9 dB over 10–100 %). The THD steps were back-loaded: 0.9, 0.5, 0.7, 1.1, 1.6 % per 10 % up to 50 %, then 2.6–5.3 % | Without Auto Level, a −18 dBFS take shows nothing below 50 % | Back-loaded (the drive gain was 24·u^1.5 dB) | **The drive gain is now linear in dB, 24u dB** (d = u^(2/3) feeds the drive stage). THD on a 110 Hz sine: 1.5 / 2.5 / 3.9 / 5.8 / 8.3 / 11.5 / 15.1 / 18.8 / 22.4 / 26 % over 10–100 %, so every step adds 1–3.7 points. The Auto Level cap fix makes it level-independent |
| Pitch Mode | Shift / Snap / Mono | All | Snap looks like Shift to the spectral metrics, as expected: it changes only intonation. On the f0 track it quantises (mean distance from a semitone 0.13 st, against 0.27 for Shift) | – | – | No change. Mono flattens the contour (27 % of frames follow it, against 98 % for Shift) |
| Formant | Follow / Keep | Both | – | Keep at −15 st or lower: pulse buzz | – | No change. Tooltip: beyond about −12 st, Keep holds the words and Follow is bigger but blurrier |

### CLEAN

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Contain | 0–100 %, 35 | 20–100 % | 0–20 % (pumping +0.2 LU, crest −0.1 dB) | None. Intelligibility ≥ 0.94 at 100 % on its own | Back-loaded: 0 → 30 % does +0.35 LU of control, 30 → 100 % does +1.7 LU | **Internal curve u^0.7** (30 % now acts like 43 % did, 100 % unchanged) |
| Gate | 0–100 %, 35 | 0–40 % | 40–100 %. Room tone sits about 10 dB under the threshold, and the fixed 2.5:1 expander can only cut that by about 15 dB, which the depth reaches at 37 %. On takes whose gaps between words are short, the gate rarely closes at all, so even the deeper law left the room tone flat above 20–40 % | None | All of the action is in 0–40 % | **Expander ratio now rises with the knob (1.5 + 3.5u) and depth is −30u dB.** **The threshold also follows the knob: T = floor + 6 + 12u dB** (10 dB at 35 %), so higher settings close on the shorter gaps too. Room tone (median of the quietest 15 % of frames, re speech) in Init on excerpt A: +11.7 / +8.1 / +3.0 / −1.5 / −6.7 dB at 0 / 30 / 50 / 70 / 100 % (it stopped near +7.3 dB from 40 % before) |
| Clean Tone | Flat / Voice / Heavy / Radio | All four distinct (centroid 368 / 498 / 782 / 1027 Hz in Init) | – | Radio is an extreme effect by design | – | No change. Tooltip: Heavy trims the lows so a heavy chain stays clear. It sounds thinner, not heavier |

### DE-ESS

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| De-ess | 0–100 %, 30 | 0–55 % in a preset, 0–70 % on its own | **60–100 % gives identical output** in every context. The detector's slope limits gain reduction to about 13–15 dB, so the −24 dB range (up to −36 dB with the ROAR link) is never reached. A range alone (−15u dB) still stopped at about 70 % | None (intelligibility ≥ 0.99) | Front-loaded: 0 → 30 % gives 65–75 % of the whole effect | **Range is −15u dB up to 40 %, and the ROAR link is removed. Above 40 % the detector threshold follows the knob** (down by 12x dB, x = ((u − 0.4) / 0.6)^1.5) **and the range grows by 9x dB** (−24 dB at 100 %). In Init the 5–12.6 kHz bands drop by 0.6–2.1 dB per 10 % over the whole travel on all three takes |
| De-ess Mode | Split / Wide | Both | – | Wide ducks the whole voice. At the same amount it removes 3–5 dB less top end than Split, and the dip is audible as a level drop (intelligibility 0.90 → 0.82 in Init) | – | No change. Tooltip explains Wide |

### DETAIL

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Low | 0–100 %, 30 | 20–80 % (sweet spot 40–60 %) | 0–20 % (≤ 3 dB of depth, crest unchanged) | 85–100 %: room tone lifts +14…+20 dB on its own and up to +35 dB in a busy preset; gain flutter appears; intelligibility drops to 0.84; crest stops improving above 80 % | Accelerating (depth 48·u^1.68 dB) | **Depth is now 34·u^1.2 dB.** The new 100 % equals the previous 80 %, and the new 10 % gives 2.2 dB (was 1.0). **The floor guard now reads CLEAN's gate state** and stops boosting while the gate is closing. **It also has its own quiet tracker** (the quietest 10 ms level of the last 2 s, + 5 dB), so it holds when the gate does not close, and **near-floor material may rise at most 1 dB per dB above the guard's no-boost zone** |
| High | 0–100 %, 25 | 20–80 % (sweet spot 30–60 %) | 0–20 % | 85–100 %: room tone +15…+20 dB; 10–16 kHz hiss +3.9…+5.4 dB above the input | Accelerating | Same new law as Low |
| Detail Speed | Natural / Snappy | Both | Near-identical while Low is under about 50 % | – | – | No change. Tooltip: matters at higher Low settings |

### VOCODER

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Gravel | 0–100 %, 50 | 15–100 % | 0–15/20 %: the wet signal sits 14–20 dB under the dry. Above 70 % the roughness metric is at its floor, but brightness keeps rising (+0.7–1 dB above 8 kHz per 10 %), so that range is live | None (intelligibility ≥ 0.87; 100 % alone is 5.8 dB brighter than the input above 8 kHz) | S-shaped | **Internal start offset: g = 0.15 + 0.85u** (with a 1 % fade from 0). This removes the dead first fifth and keeps the top |
| Vocoder Octave | −1 Oct / −2 Oct | Both, but the difference is subtle | Close to dead (dSpec 0.2–0.5 dB). The filter bank starts at 80 Hz, so the carrier's sub-octave fundamental (52 Hz, or 26 Hz for −2) is filtered out, and −2 Oct adds only about 2 dB of subharmonic | – | – | **DSP follow-up, not changed in 1.1.0** (see Deferred). Tooltip no longer promises a literal octave |

### RING

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Ring Amount (Ring mode) | 0–100 %, 40 | 0–100 % | In Init, 30 → 60 % reads dead on two inputs, but the low share and the centroid still rise steadily | None (intelligibility ≥ 0.88). 100 % is a distinct pure-ring sound | Mixed. The roughness change is front-loaded and the spectral shares are linear | No change |
| Ring Amount (AM mode) | Same knob, AM index = a/2 | 30–100 % | 0–30 % (roughness change ≤ 0.3 dB) | None | Back-loaded (the effect grows as a²) | **AM index is now √u / 2**, so it is roughly linear in travel |
| Ring Freq | 20–4000 Hz, 150 | All of it, in zones (see Interactions) | The bottom 25 % of the knob covered only 20 → 24 Hz | None broken. At 1.3 kHz and up it is squeaky and thin, with intelligibility 0.69–0.79 at amounts ≥ 70 % | Power skew. In the middle one octave passes per 8 % of travel | **True log taper over the same range.** 150 Hz sits at 38 % and each 10 % of travel is 0.76 octave |
| Ring Ratio | x0.25–x4, x1 | x0.35–x4 | Bottom 17 % covered x0.25–0.31 | x0.25–0.35 at amounts of 30–60 %: a 16–32 Hz chop (intelligibility 0.66–0.79) | Power skew | **True log taper.** x0.5 sits at 25 %, x1 at 50 %, x2 at 75 % |
| Ring Carrier | Sine / Triangle / Square | Sine, Square | Triangle ≈ Sine (its 3rd harmonic is at −19 dB) | Square adds 6–7 dB above 8 kHz, and +15–20 dB with carriers at 640 Hz and up | – | **Square's harmonics are now band-limited at 10 kHz**, which keeps the buzz and drops the fizz. Triangle is documented as a softened sine |
| Ring Track, Ring Mode, Ring Body | Choices | All | None | None | – | No change |

### SHIMMER

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Shimmer | 0–100 %, 12 | 10–60 % | 0–10 % on its own; 0–20 % in the full chain | **65–100 % runs backwards.** The wobble falls (spectral motion 8.0 → 5.0 dB) while the sound keeps getting rougher | Close to linear below the peak | **Internal scale m = 0.65·u**, so 100 % is the previous peak |
| Shimmer Rate | 0.5–40 Hz, 11 | 0.5–13 Hz | The bottom 20 % covered 0.5–0.75 Hz | 20–40 Hz with amount ≥ 40 %: ring-mod roughness that collides with GROWL and RING (roughness down to −1.8 dB) | Power skew | **True log taper, range 0.5–25 Hz.** No preset goes above 12 Hz |
| Shimmer Mode | Up / Down / Up+Down / Stereo | All | Up ≈ Down in depth (Down is darker) | Stereo at ≥ 70 % loses 2.4–3 dB in mono. Up+Down peaks early, which the amount remap fixes | – | No change. Tooltip warns about Stereo in mono |

### GROWL

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Growl Depth | 0–100 %, 48 | 25–100 % on its own, 40–100 % in the chain | 0–20 % on its own; 0–30 % in Init | None (intelligibility ≥ 0.87) | Convex. Peak-to-trough is 1.6 dB at 20 % and 15.6 dB at 100 % | **The trough depth is now linear in dB, D = 1 − 10^(−18u/20).** Each 10 % adds about 0.1 of modulation index |
| Growl Rate | 1–150 Hz, 27 | 2–150 Hz | The bottom 26 % of the knob covered only 1 → 2 Hz, where the modulation disappears into syllable rhythm | Below 2 Hz at depth ≥ 75 %: seasick level pumping (3.7–4.6 LU). 150 Hz is edgy ring-mod, but usable | Power skew | **True log taper, range 2–150 Hz.** Zones: stutter 2–17 Hz takes 0–32 % of the travel, growl 18–50 Hz takes 32–72 %, buzz takes 72–100 % |
| Growl Shape | Sine / Opto / Square / Throat / Rattle | Sine, Opto, Square, Throat | **Rattle = Sine** in every preset. Its envelope threshold sat about 12 dB above any levelled voice | – | – | **Rattle threshold moved to R − 12 dB** (about −32 dBFS peak). Throat ignores Rate by design: the Rate knob is dimmed when Throat is selected |
| Growl Character | Clean / Warm / Grind | Clean, Grind | **Clean → Warm is near dead** (1.2 % THD on a sine) | None | – | **Warm drive raised from 3 to 9 dB, Grind from 8 to 12 dB** |

### SWIRL

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Swirl | 0–100 %, 30 | 10–90 % (Deep 6: 10–75 %) | 0–10 %. After the mix remap, 50–70 % read dead on the reference take: the resonance stopped growing at mix 0.61 | Deep 6 at 80–100 %: pumping 2.3–3.0 LU and pitch wobble 37–44 c. Talky 4 at 100 %: pumping 3.3 LU | Convex. The first 30 % gives 0.7 dB of motion, the last 30 % gives 2.0 dB | **Internal mix m = 1 − (1 − u)^1.5**, which makes Liquid 4 linear to within ±0.03. **The resonance now keeps rising with the mix** (k = k0·max(0.94, 0.6 + 0.4m / 0.35), was capped at 1.3·k0; 0.95 absolute cap kept) |
| Swirl Rate | 0.05–10 Hz, 0.35 | 0.07–4 Hz | The bottom 30 % covered only 0.05–0.14 Hz | ≥ 4 Hz at amount ≥ 60 %: seasick warble (P95 22–50 c) | Power skew | **True log taper, same range** |
| Swirl Circuit | 6 options | All distinct on their own | Masked inside the busiest presets | Deep 6 at the top (see above) | – | No change |
| Swirl Motion | LFO / LFO+Level / Random | LFO, Random | **LFO+Level ≈ LFO** on a levelled voice. Random reaches only 30–60 % of the LFO's depth | Random at ≤ 0.1 Hz is static | – | Deferred (see Deferred) |

### ROAR

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Roar Drive | 0–100 %, 45 | 0–50 % (mostly EQ) and 70–100 % (mild saturation) | **50–70 % on its own; 60–90 % in Init.** The scoop saturates at 50 % while the drive grows slowly. Saturation at 100 % is weak (110 Hz sine THD 8.8 %) | None at reference level | Kink at 50 % | **Scoop curve made smooth** (identical up to 40 %, reaching −7 dB at 60 % without a kink). **Drive extended above 50 %:** drive dB = 18a + 48·max(0, a − 0.5)², about +12 dB at 100 % (sine THD ≈ 27 %). Makeup recalibrated above 50 % |
| Roar Tone | −100…+100, +10 | ±40…±100 at Drive ≥ 50 | **The whole range at Drive ≤ 20** (±0.9 dB end to end). At Drive 45 every 20 % step is dead. With the strength at 1.5·max(Drive, 50 %), its ends still read dead at Drive 30–45 | None | Linear and even | **Tone strength decoupled from Drive:** the tilt strength is 1.5 at every Drive (the previous strength at 100 %). At Drive 30–45 each 20 % of Tone now tilts the voice (3–13 kHz against 50–250 Hz) by 0.8–2.0 dB (was 0.3–1.3) |
| Roar Voicing | Line / Mic | Both | – | – | – | No change. Tooltip: Mic mostly thins the lows |

### ENSEMBLE

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Ensemble | 0–100 %, 18 | 10–70 % | 0–10 % on its own; 0–20 % in the chain (0–40 % on excerpts A and B after a linear remap); 90–100 % | Wet ≥ 70 % with Slow or Medium: pitch wobble P95 80–111 c, seasick. **Vibrato: 10–70 % is bit-identical** (the amount is forced to at least 70 %) | Linear up to 80 %, then flat | **Internal scale m = 0.7·u^0.6** (the chorus is masked below m ≈ 0.2, so the bottom of the travel moves there quickly). **Vibrato's amount curve is now √m**, so it is progressive |
| Width | 0–150 %, 12 | 15–105 % | 0–15 % (by design); 120–150 % | ≥ 105 % with amount ≥ 60 %: L/R correlation ≤ 0 and mono loss −3.4…−4.8 dB | Front-loaded | **Remapped so 150 % equals the previous 105 %**, with even correlation steps: side gain = g_old(1.05)·(W/1.5)^1.3 |
| Ensemble Voices | 3 / 6 | Both | Subtle | – | – | No change |
| Ensemble Motion | Slow / Medium / Fast / Vibrato | All | – | **Slow wobbled most.** A fixed 6.2 Hz component scaled with each motion's depth | – | **The 6.2 Hz component now has a fixed depth (0.18 ms)**, so Slow is the gentlest motion |

### TAME

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Tame | 0–100 %, 30 | 0–100 % | 0–30 % on the reference take. Elsewhere 0.35–0.5 dB per step in its band, at the edge of audibility | None | Linear | **Under-powered:** the whole knob gave only 4–6 dB in its band. **The curve position is now t = 2.2·u^0.7** (2.5–6.5 kHz: threshold −24 − 12t dBFS, depth 12t dB; above 6.5 kHz: threshold −30 − 12t, depth 16t), so the previous curve's top (t = 1) sits at 32 % and the bottom steps carry on the reference take. Its own bands (3–13 kHz) now drop 0.5–1.3 dB per 10 % in Init on all three takes (the reference take gave 0.2–0.3 dB per step below 30 % before) |

### SUB

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Sub Level | 0–100 %, 45 | 10–85 % | None. 0 → 10 % is the fade-in | 85–100 % on heavy presets: +0.6 dB of sub per step, and it costs 0.6–0.9 dB of 1–4 kHz level through level matching | Good (3.0 → 0.8 dB per 10 %) | **Top capped:** L = −14 + 24·√u dB (+10 dB at 100 %, was +12) |
| Sub Drive | 0–100 %, 60 | None | **The whole range.** A 4th-order low-pass at 1.5× the sub frequency after the shaper removes every drive product (all stay ≥ 42 dB under the sub) | – | – | **The low-pass moves to 4× the sub frequency**, so the 2nd and 3rd harmonics (100–200 Hz, the part small speakers can play) come through. Makeup is recalibrated. The mapping (−6…+18 dB) is kept |
| Sub Register | Follow / Tight / Deep | Follow; Deep at −4…+5 st | **Tight = Follow** unless the f0 is 84–102 Hz | Deep with PITCH at −5 st or lower puts the sub at 25–37 Hz: inaudible rumble that still uses limiter headroom | – | **Tight now means "sub never below 55 Hz":** it picks the lowest of f0/2, f0 and 2·f0 that is ≥ 55 Hz, with 3 % hysteresis. **Deep gets a floor:** it divides 2·f0 when f0/2 < 32 Hz |
| Sub Character | Clean / Resonant / Thick | All, but barely distinct (within 1 dB from 40 Hz up) | – | – | – | Deferred to listening (see Deferred) |

### OVERTONE

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Overtone Amount | 0–100 %, 10 | Add: only 60–100 %, and mild. Lean/Replace: 10–100 % | **Add: 0–40 %** (under 0.5 dB in every band). The default 10 % does nothing | – | Progressive shape, but 8–10 dB too quiet | **Lean / Replace: L = −10 + 20·u^0.6 dB** (was −20 + 26·u^0.6). **Add has its own law, g = 1.83·√(10^u − 1)** (−0.6 / +8.6 / +14.8 dB at 10 / 50 / 100 %): with the shared law, Add moved the 200–315 Hz band by only 0.1–0.4 dB per 10 %. Now that band rises 0.3–0.7 dB per 10 % (2.1–3.4 dB by 50 %, 4.6–6.4 dB at 100 %), and the 160–500 Hz share at 100 % stays at or under the dry voice's (−3.4 / −5.1 / −3.5 dB against −2.1 / −4.3 / −3.5) |
| Overtone Frequency | −1…+1 oct, 0 | Add: about ±0.5 oct. Lean/Replace: −1…+0.4 | Add: nearly dead (≤ 2 dB of local movement; 0.1–0.3 dB of mean change per 0.2 oct in 100 Hz–1.3 kHz at Amount 50) | Replace at +0.6…+1 oct: the crossover reaches 333–440 Hz and removes the voice's body (a thin phone sound) | Linear in octaves, right | **The crossover in Lean/Replace is clamped at 300 Hz** (was 1 kHz), so the top of the knob moves the overtones instead of acting as a high-pass. In Add, the louder Add law makes the moving layer heard: 0.2–0.7 dB of mean change per 0.2 oct in 100 Hz–1.3 kHz at Amount 50 |
| Overtone Pattern | 6 options | All | In Add they are within about 1 dB of each other (Speaker apart) | – | – | No change. They separate with the Amount fix |
| Overtone Mode | Add / Lean / Replace | Add and Lean always; Replace from about 40 % | – | **Replace or Lean below about 40 % Amount is just a high-pass:** the lows go and nothing replaces them | – | **The low cut now follows Amount:** the cut is applied by min(1, u / 0.25), so 0 % is neutral in every mode |

### OUTPUT

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Push | 0–24 dB, 8 | About 4–16 dB with Clip off; 4–24 dB with Clip ≥ 6 | 0–3 dB is plain gain. With Clip off, 16–24 dB adds only +0.2…0.9 LU | With Clip off, above about 18 dB: intelligibility and pumping worsen for almost no loudness | Linear | **Skew centre at 10 dB** (half the knob covers 0–10 dB). Range and display stay the same |
| Ceiling | −12…0 dB, −1 | All, as an output level | Above the program's own peak | None, but **it also squashed:** Push was absolute, so each dB of lower ceiling added 0.3–0.5 dB of gain reduction | Linear | **Push is now relative to the ceiling:** pre-limiter gain = Push + (Ceiling + 1 dB). Ceiling becomes a pure output trim |
| Clip | 0–18 dB (0 = Off), 0 | 0 up to about Push − 5 dB | Anything above about Push − 5 dB | None. At high Push, Clip is more intelligible than the limiter (0.808 → 0.870 at Push 20) | Linear wastes about 2/3 of the knob at preset Push values | **Skew centre at 5 dB** |
| Clip Shape | 0–100 %, 0 | Clip ≥ 6 dB: all. Clip 3 dB: 45–100 %. Clip 1–2 dB: 60–100 % | **Below 1 − 2·cap nothing happens**, because the knee is limited by the clip amount (Clip 1 dB: 0–78 %; 3 dB: 0–42 %) | None | Most of the change happens in the last 20 % | **Knee width w = min(0.5, cap)·(1 − s)**, which spans the allowed knee at every Clip setting (a squared taper was tried and left the last 40 % dead) |
| Attack | 0.05–10 ms, 2 | 0.3 to about 6 ms, and only with Auto off, a short Release and the limiter working | 0.05–0.3 ms (the bottom 20 %); above 6–8 ms; everything with Auto on | None | Power skew | **Range 0.2–10 ms, true log taper**, default 2 ms |
| Release | 1–1000 ms, 60 | Manual: 2–1000 ms once the limiter works. Auto: about 60–1000 ms | The bottom 10 % of the knob covered 1.0–1.08 ms. With Auto on, everything below about 12 ms was identical (the fast stage clamps at 3 ms) | Manual 1–3 ms at Push ≥ 18: deliberate splatter (THD 8.7–16.5 % at 55 Hz) | Power skew | **True log taper.** With Auto on, the fast stage maps piecewise-log over the whole knob: 1–60 ms → 3–15 ms, and 60–1000 ms → 15–250 ms as before |
| Auto Release | On/Off, On | Matters only when the limiter is working | ≤ 0.4 LU at every preset | None | – | No change. It is a strong low-end clean-up when driven (THD /10 to /200) |
| Mix | 0–100 %, 100 | 10–70 % is the blend | 70–100 % is mostly a level ramp | 0–20 %: loudness dips below both ends. 30–40 %: the dry and wet voices flam. The dry was raw and about 8 dB under the wet | Linear amplitude on two uncorrelated signals | **The dry is level-matched to the wet** (dry × the Push gain, the same reference auto gain uses), and the crossfade is equal-power. 50 % then means equal loudness |

## Interactions

**PITCH × Size.**
- In Follow, the formant shift is pitch − 6·Size, with a floor at −24 st. So Size's effective range shrinks to nothing between −18 and −24 st.
- In Keep, Size works the same at every pitch.

**PITCH × Formant on real voice.**
- Follow is "bigger and blurrier": intelligibility ≥ 0.84 down to −12 st, and 0.53 at −24.
- Keep is "lower but human": ≥ 0.94 down to −18 st, with a pulse buzz below −15.
- Mono with Keep is the cleanest at large shifts (0.91 at −24).

**PITCH × SUB.** SUB follows the rewritten f0 correctly. Below about −15 st, though, every register put the sub under 40 Hz, where most playback cannot reproduce it. The new Tight rule keeps it at 55 Hz or above (about 57 Hz at −24 st).

**PITCH Drive and ROAR × input level.**
- With Auto Level on, −6 and −18 dBFS-peak takes behave identically.
- A −30 dBFS-peak take arrived about 10 dB short. The +30 dB cap fixes this.
- With Auto Level off, ROAR only bites about 12 dB above the reference level, and at −12 dB trim the top half of Drive does nothing.

**DETAIL × CLEAN Gate.** The gate could not hold DETAIL back. DETAIL (position 4) undid about 11 dB of the gate (position 2). On the reference take, Gate 100 with Low 100 still ended 3.2 dB above the input's room tone. With the new DETAIL law and the gate-aware floor guard, room tone at Low 100 now stays at or below the input's level whenever the gate is closing. On takes where the gate rarely closes (excerpts A and B), DETAIL's own quiet tracker and the boost cap hold it instead: with Gate 35, Low 0 → 100 adds +0.8 / −1.8 dB of room tone (median of the quietest 15 % of frames, in Init on A / B; it was +9.2 / +6.1 dB). Breaths in those gaps still come up, by design: they are 10–20 dB above the room.

**DE-ESS × ROAR.**
- ROAR 0 → 100 raises energy above 8 kHz by 3.5–5.3 dB.
- DE-ESS sits before ROAR, so it can only pre-soften sibilants (2–4 dB at best). It cannot reach fizz that ROAR creates.
- The ROAR link had no effect, so it is removed. Fizz after ROAR is TAME's job, and the stronger TAME law covers it.

**Ring frequency zones on real voice.** The same zones appear at every amount:

| Ring Freq | Character | Knob position (new taper) |
|---|---|---|
| 20–56 Hz | Tremolo / chop | 0–19 % |
| 56–110 Hz | Rough, buzzy | 19–32 % |
| 110–450 Hz | **Metallic robot voice** (intelligibility ≥ 0.95) | 32–59 % |
| 450–1300 Hz | Clangy bell | 59–79 % |
| 1.3–4 kHz | Squeaky, thin | 79–100 % |

**Ring Ratio with Follow.** Integer ratios (x0.5, x1, x2, x3, x4) and x1.5 stay in tune and give the highest HNR. In-between ratios clang. Below x0.35 the ring chops unless Amount is 100 %.

**RING × ROAR.**
- Never harsh (intelligibility ≥ 0.88).
- RING before ROAR: the saturation flattens the ring.
- RING after ROAR: it rings the distortion harmonics, which is brighter but still fine.

**Gravel × PITCH.** At −12 st and below, Gravel mostly restores high end lost to the shift: the centroid goes 169 → 392 Hz at −24 st. The dead first fifth was present at every pitch, which supports the offset remap.

**Shimmer rate zones.**

| Rate | Character |
|---|---|
| 0.5–2 Hz | Liquid swell |
| 2.5–9 Hz | Warble |
| 13 Hz and up | Rough, ring-like |

With amount ≥ 40 % and rate ≥ 20 Hz the voice goes inharmonic, which is why the range now tops out at 25 Hz.

**Seasick zones.** In each of these combinations the pitch wobble (P95) climbs to 22–111 c:
- Swirl at amount ≥ 60 % with rate ≥ 4 Hz;
- Ensemble at ≥ 70 % with Slow or Medium.

The amount remaps move these zones to the very top of the travel.

**Ensemble Width × mono.** Folding to mono stays tonally clean (comb colouring ≤ 0.66 dB). The risk is level: the chorus drops by up to 4.8 dB once correlation goes below 0. The Width remap keeps 150 % at the previous 105 %.

**Growl rate × depth.**

| Rate | Character |
|---|---|
| 4–17 Hz | Stutter. The worst intelligibility (0.78) is at depth 100 and 6–12 Hz |
| 18–50 Hz | Growl |
| 60–120 Hz | Buzz. Heard as pitch |

No combination becomes unintelligible.

**SUB × GROWL.** SUB sits after GROWL, and its sub layer carries none of the growl modulation. At Depth 100 % the 27 Hz modulation peak is 0.34 in the chain and 0.60 without SUB. See Deferred.

**SUB before ROAR (the Sub Furnace order).** Drive turns into a sub-thickness and compression control: the whole Drive sweep reads dead for brightness. This is by design and is now documented on the preset.

**Sub Level × Push.** No runaway at any setting. The top 15 % of Sub costs 1.2–1.6 dB of 1–3 kHz level at every Push, mostly through output level matching. This supports the gentler top of the new Sub law.

**Sub × Overtone.** There was no mud risk at the previous gain (the 160–500 Hz share peaks at −6.1 dB, under the dry voice's −2.1 dB). With the Add law, Overtone 100 in Init measures −3.4 dB on excerpt A, still under the dry voice.

**Overtone Replace × SUB.** OVERTONE runs after SUB, so Replace also removes SUB's octave. In Replace, Sub Level acts as "weight through the overtones" (live from about 40 %).

**Push × Clip.** Clip's live range ends at about Push − 5 dB. The loud zone that stays clean is Push 14–18 with Clip 6–9. Clip at high Push is more intelligible than the limiter alone.

**Attack × Release (Auto off).** Crunch needs both to be short:

| Setting | Result |
|---|---|
| Release 3–10 ms, Attack 0.3–3 ms | Useful crunch |
| Release ≤ 3 ms, Attack < 1 ms | Splatter |
| Release ≥ 100 ms | Attack does almost nothing |

When Clip already removes the overload, the limiter idles and Attack, Release and Auto do nothing.

## Defects found

| # | Defect | Fix in 1.1.0 |
|---|---|---|
| 1 | Hertz, ratio and millisecond knobs were documented as log tapers but used a power-law skew. Their bottom 10–26 % was dead | True log taper for every such knob |
| 2 | De-ess range was unreachable, so 60–100 % and the ROAR link did nothing | New range law, link removed |
| 3 | Gate depth beyond about 40 % was unreachable | Ratio rises with the knob |
| 4 | DETAIL's floor guard did not stop room-tone lift at high settings (+20 dB on its own, +35 dB in a preset) | New law, plus a gate-aware guard |
| 5 | Growl's Rattle shape was identical to Sine at every real level | Threshold lowered |
| 6 | Growl Rate does nothing with Shape = Throat | By design. The Rate knob is dimmed when Throat is selected |
| 7 | ROAR Drive had a kink at 50 % | Smooth scoop and drive extension |
| 8 | Ensemble Vibrato forced 70 % wet at every amount from 10 % | Progressive curve |
| 9 | Ensemble's 6.2 Hz component made Slow the wobbliest motion | Fixed depth |
| 10 | Sub Drive was inaudible | Low-pass moved after the shaper's useful harmonics |
| 11 | Sub Tight differed from Follow only for f0 of 84–102 Hz; Deep had no floor | New register rules |
| 12 | Overtone Amount was 8–10 dB too quiet in Add | New law |
| 13 | Overtone Lean/Replace at low Amount acted as a plain high-pass | The cut follows Amount |
| 14 | Clip Shape did nothing below 1 − 2·cap | New knee mapping |
| 15 | With Auto on, Release below about 12 ms was identical | Piecewise-log fast stage |
| 16 | DC after the asymmetric clipper: −39.6 dBFS mean at Push 24 on a heavy preset | 10 Hz DC blockers on the limiter's gain reduction and on the clipper's residual |
| 17 | Ceiling also acted as a drive control | Push relative to the ceiling |
| 18 | The dry signal in Mix was not level-matched, and 0–20 % dipped in loudness | Matched, equal-power mix |
| 19 | Square ring carrier: +6–7 dB of fizz above 8 kHz | Band-limited at 10 kHz |
| 20 | `PitchSettings::drive` defaulted to 0.25 against the parameter's 5 % (harmless; overwritten every block) | Defaults aligned |
| 21 | Test runner: `--only` silently accepted unknown module codes | Unknown codes are now an error |
| 22 | De-ess still stopped at about 70 % with the −15u range: the detector's slope set the ceiling | The threshold follows the top of the travel, and the range grows with it |
| 23 | Gate barely acted above 20–40 % on takes whose gaps are short: it closed only in long pauses | The threshold's distance from the room floor follows the knob |
| 24 | DETAIL's floor guard needed a stationary 0.4 s pause or a closing gate: on a take with short gaps it lifted the room tone by about 9 dB at Low 100 | Its own quiet tracker (2 s minimum), and a boost cap near the floor |
| 25 | Overtone in Add moved its band by 0.1–0.4 dB per 10 % with the raised law | A separate Add law |
| 26 | Roar Tone's ends were dead at Drive 30–45 | Tone strength 1.5 at every Drive |
| 27 | PITCH Drive's gain law (24·u^1.5 dB) put most of the change in the last 30 % | Gain linear in dB |
| 28 | Ensemble's first 40 % was masked in a full chain, Swirl's resonance stopped growing at mix 0.61, Tame's bottom steps were weak on a quieter top band | Front-loaded Ensemble law; the Swirl resonance keeps rising; Tame curve position 2.2·u^0.7 |

## Changes in 1.1.0

Parameter IDs, order and kinds do not change. "u" is the knob value as 0–1.

| Parameter | Old mapping | New mapping | Why |
|---|---|---|---|
| Input (with Auto Level on) | Ignored inside Auto Level's −12…+20 dB window | Offsets Auto Level's target by 0.5 × Input (±12 dB) | −8…+12 dB was dead with Auto on |
| Auto Level | Gain −12…+20 dB | Gain −12…+30 dB | Quiet takes arrived 10 dB short |
| Contain | u | u^0.7 | 0–20 % was dead and the curve back-loaded |
| Gate | Ratio 2.5, depth −40u dB, threshold floor + 10 dB | Ratio 1.5 + 3.5u, depth −30u dB, threshold floor + 6 + 12u dB | Travel above about 40 % was unreachable, and the gate rarely closed on short gaps |
| De-ess | Range −24u·(1 + 0.5·ROAR Drive) dB, detector threshold fixed | Range −15u − 9x dB, threshold 12x dB lower, x = ((u − 0.4) / 0.6)^1.5 above 40 %; no link | 60–100 % was identical |
| Detail Low / High | Depth 48·u^1.68 dB | Depth 34·u^1.2 dB, plus a gate-aware floor guard, a quiet tracker (2 s minimum + 5 dB) and a boost cap of 1 dB per dB above the floor | 0–20 % was dead and 85–100 % lifted room tone |
| PITCH Drive | Drive gain 24·u^1.5 dB | 24u dB (d = u^(2/3)) | 10–70 % was dead |
| Gravel | g = u | g = 0.15 + 0.85u (1 % fade-in from 0) | 0–15 % was dead |
| Ring Amount (AM mode) | Index a/2 | Index √u / 2 | 0–30 % was dead |
| Ring Freq | Power skew, centre 150 Hz | Log taper over 20–4000 Hz (150 Hz at 38 %) | The bottom 25 % covered 20–24 Hz |
| Ring Ratio | Power skew, centre x1 | Log taper over x0.25–x4 (x1 at 50 %) | The bottom 17 % was dead |
| Ring Carrier (Square) | Harmonics to 20 kHz | Harmonics to 10 kHz | Fizz above 8 kHz |
| Shimmer | m = u | m = 0.65u | 65–100 % ran backwards |
| Shimmer Rate | 0.5–40 Hz, power skew | 0.5–25 Hz, log taper | Dead bottom and an unusable top |
| Growl Depth | D = 0.85u | D = 1 − 10^(−18u/20) | Dead bottom and a convex curve |
| Growl Rate | 1–150 Hz, power skew | 2–150 Hz, log taper | The bottom 26 % covered 1–2 Hz |
| Growl Shape (Rattle) | Envelope threshold R + 6 dB | R − 12 dB | Rattle was identical to Sine |
| Growl Character | Warm 3 dB, Grind 8 dB | Warm 9 dB, Grind 12 dB | Clean → Warm was near dead |
| Swirl | m = u; resonance k0·(0.6 + 0.4m / 0.35), at most 1.3·k0 | m = 1 − (1 − u)^1.5; resonance k0·max(0.94, 0.6 + 0.4m / 0.35) | Convex curve, dead bottom; dead 50–70 % once the resonance stopped |
| Swirl Rate | Power skew, centre 0.71 Hz | Log taper over 0.05–10 Hz | The bottom 30 % covered 0.05–0.14 Hz |
| Roar Drive | Scoop −min(7, 14a); drive 18a dB | Smooth scoop (same to 40 %, −7 dB from 60 %); drive 18a + 48·max(0, a − 0.5)² dB | Kink at 50 %, weak top |
| Roar Tone | Strength = Drive | Strength = 1.5 at every Drive | Dead at low and moderate Drive |
| Ensemble | m = u (Vibrato: at least 0.7) | m = 0.7·u^0.6 (Vibrato: √m) | Seasick top, masked bottom; Vibrato bug |
| Ensemble Motion | 6.2 Hz component scaled with motion depth | Fixed 0.18 ms depth | Slow wobbled most |
| Width | g = 1 − e^(−(W/0.75)²) | g = g_old(1.05)·(W/1.5)^1.3 | Out-of-phase top |
| Tame | Depth 12u / 16u dB, threshold −24 − 12u / −30 − 12u | Same curve at t = 2.2·u^0.7 | Under-powered, weak bottom |
| Sub Level | −14 + 26·u^0.55 dB | −14 + 24·√u dB | The top 15 % cost mids for little sub |
| Sub Drive | Low-pass at 1.5× the sub frequency after the shaper | Low-pass at 4× the sub frequency | Inaudible |
| Sub Register | Tight switch at 102 Hz; Deep has no floor | Tight: sub ≥ 55 Hz; Deep: floor at 32 Hz | Tight ≈ Follow; Deep rumble |
| Overtone Amount | −20 + 26·u^0.6 dB | Add: 1.83·√(10^u − 1) (linear gain). Lean / Replace: −10 + 20·u^0.6 dB | 0–40 % was dead in Add, and weak over the whole travel |
| Overtone Frequency | Crossover clamp 1 kHz | 300 Hz in Lean/Replace | Acted as a high-pass |
| Overtone Mode | Full low cut at any Amount | Cut × min(1, u / 0.25) | A plain high-pass at low Amount |
| Push | Linear, absolute | Skew centre 10 dB; gain relative to the ceiling | Dead top; Ceiling squashed |
| Ceiling | Output ceiling and drive | Output ceiling only | – |
| Clip | Linear | Skew centre 5 dB | 2/3 of the travel was dead at typical Push |
| Clip Shape | w = min(0.5(1 − s), cap) | w = min(0.5, cap)·(1 − s) | Dead below 1 − 2·cap |
| Attack | 0.05–10 ms, power skew | 0.2–10 ms, log taper | Dead bottom |
| Release | Power skew; Auto fast stage = R/4, at least 3 ms | Log taper; Auto fast stage piecewise-log, 3–250 ms | Dead bottom; dead with Auto on |
| Mix | Raw dry, linear crossfade | Level-matched dry, equal-power crossfade | Loudness dip, flamming |
| Output | No DC removal after the clipper | 10 Hz DC blockers on the gain reductions and the clipper residual | DC offset at extreme Push |

### Factory presets

Each preset's stored values are converted so that it sounds the same under the new laws. The rules are:

| Parameter | Conversion (old stored value → new stored value) |
|---|---|
| Contain | new = 100·(old/100)^(1/0.7) |
| Gate | new = min(40·old/100, 15) / 30 × 100, which matches the previous room-tone cut |
| De-ess | r = old·24·link / 15 (link = 1 + 0.5·ROAR Drive when ROAR is on). Up to r = 40: new = r. Above: a map measured on the three takes (the 5–12.6 kHz bands), 45 → 43, 60 → 50, 80 → 54, 100 or more → 56 |
| Detail Low / High | new = 100·(48·(old/100)^1.68 / 34)^(1/1.2), capped at 100 |
| Gravel | new = (old − 15) / 0.85 |
| Shimmer | new = old / 0.65 |
| Swirl | new = 100·(1 − (1 − old/100)^(2/3)) |
| Ensemble | new = 100·(min(old / 100, 0.7) / 0.7)^(1/0.6) |
| Width | new = 150·(g_old(old) / g_old(1.05))^(1/1.3), capped at 150 |
| Growl Depth | new = −20·log10(1 − 0.85·old/100) / 18 × 100 |
| Roar Drive | Unchanged up to 50. Above that, solve the new drive law for the previous drive dB (old 100 → 78) |
| Roar Tone | new = old·(old Drive) / 150 |
| Tame | new = 100·((old / 100) / 2.2)^(1/0.7) |
| PITCH Drive | new = 100·(old / 100)^1.5 |
| Sub Level | new = 100·((26/24)·(old/100)^0.55)², capped at 100 |
| Overtone Amount | Solve the new law of the preset's mode for the previous level. Values that sat in the dead zone (old ≤ 40) take the level of 10 % on the Lean / Replace law, −5 dB, which is 4 % on Add's law (see deliberate changes) |
| Push | new = old − (Ceiling + 1) |
| Clip Shape | Match the knee width w |
| Hertz, ratio and ms values | Unchanged: presets store real units. Attack values below 0.2 ms become 0.2 ms |

Values are rounded to whole percent. Rounding error is under 0.5 % of travel, below every audible threshold above.

| Preset | Pitch Drive | Contain | Gate | De-ess | Tame | Low | High | Gravel | Shimmer | Swirl | Ensemble | Width | Growl | Roar | Tone | Sub | Sub Drive | Overtone | OUTPUT |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Init | 5→1 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 18→10 | 12→10 | 48→25 | 45 | 10→3 | 45→49 | 60→20 | 10→4 | – |
| Symbiote | 20→9 | 40→27 | 45→50 | 40→54 | 35→7 | 30→25 | 25→19 | 50→41 | 25→38 | 32→23 | 30→24 | 15→14 | 55→30 | 50 | 0 | 55→61 | 40→20 | 10→4 | – |
| Liquid Beast | 25→12 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 50→41 | 40→62 | 50→37 | 60→77 | 70→111 | 40→20 | 40 | 0 | 55→61 | 50→20 | 10→4 | – |
| Gravel Throat | 45→30 | 35→22 | 35→47 | 30→50 | 30→6 | 30→25 | 25→19 | 45→35 | 12→18 | 30→21 | 18→10 | 12→10 | 55→30 | 50 | 0 | 50→55 | 50→20 | 35→4 | – |
| Cave Troll | 25→12 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 50→41 | 15→23 | 30→21 | 30→24 | 10→8 | 50→27 | 45 | −30→−9 | 70→79 | 50→20 | 40→4 | – |
| Kaiju | 50→35 | 35→22 | 35→47 | 30→51 | 70→19 | 30→25 | 25→19 | 40→29 | 12→18 | 30→21 | 18→10 | 12→10 | 60→34 | 65→61 | −20→−9 | 80→92 | 65 | 40→4 | – |
| Gargoyle | 25→12 | 35→22 | 35→47 | 30→50 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 50→37 | 18→10 | 12→10 | 35→17 | 60→58 | 40→16 | 50→55 | 50→20 | 10→4 | – |
| Hellhound | 60→46 | 35→22 | 35→47 | 30→51 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 18→10 | 12→10 | 70→44 | 70→64 | 20→9 | 45→49 | 70 | 35→4 | Push 8→9, Clip Shape 60→32 |
| Golem | 25→12 | 35→22 | 35→47 | 30→49 | 30→6 | 60→65 | 35→31 | 50→41 | 12→18 | 30→21 | 18→10 | 12→10 | 30→14 | 40 | 0 | 60→67 | 50→20 | 10→4 | – |
| Kraken | 25→12 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 50→41 | 60→92 | 55→41 | 70→100 | 90→137 | 20→9 | 45 | 10→3 | 50→55 | 50→20 | 10→4 | – |
| Throat Singer | 25→12 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 18→10 | 12→10 | 40→20 | 45 | 10→3 | 60→67 | 50→20 | 55→8 | – |
| Big Bad Narrator | 30→16 | 35→22 | 35→47 | 30→48 | 30→6 | 70→81 | 40→37 | 50→41 | 12→18 | 20→14 | 18→10 | 12→10 | 15→7 | 30 | 0 | 40→43 | 50→20 | 10→4 | – |
| Demon Whisper | 25→12 | 50→37 | 15→20 | 70→56 | 30→6 | 60→65 | 40→37 | 50→41 | 35→54 | 30→21 | 35→31 | 60→95 | 48→25 | 45 | 40→12 | 25→26 | 50→20 | 10→4 | – |
| Giant | 25→12 | 35→22 | 35→47 | 30→48 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 35→25 | 25→18 | 0 | 45→23 | 30 | −30→−6 | 65→73 | 50→20 | 10→4 | – |
| Possessed | 25→12 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 35→24 | 30→46 | 30→21 | 18→10 | 12→10 | 40→20 | 40 | 0 | 50→55 | 50→20 | 10→4 | – |
| Ogre Radio | 25→12 | 50→37 | 45→50 | 30→51 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 35→31 | 15→14 | 48→25 | 70→64 | 50→23 | 20 | 50→20 | 10→4 | – |
| Cyber Commander | 40→25 | 55→43 | 40→50 | 60→56 | 55→14 | 65→73 | 35→31 | 50→41 | 25→38 | 35→25 | 35→31 | 15→14 | 20→9 | 45 | 30→9 | 50→55 | 50→20 | 30→4 | Push 9→10.5, Clip Shape 40→0 |
| Respirator Rasp | 30→16 | 50→37 | 20→27 | 35→51 | 30→6 | 85→100 | 40→37 | 50→41 | 15→23 | 35→25 | 35→31 | 15→14 | 35→17 | 35 | 10→2 | 50→55 | 50→20 | 30→4 | – |
| Rasp Machine | 55→41 | 60→48 | 45→50 | 55→56 | 60→16 | 55→58 | 45→44 | 50→41 | 25→38 | 35→25 | 35→31 | 15→14 | 25→12 | 65→61 | 40→17 | 50→55 | 50→20 | 30→4 | Push 9→11, Clip Shape 70→70 |
| Robot Ogre | 25→12 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 80→76 | 12→18 | 30→21 | 18→10 | 12→10 | 40→20 | 45 | 10→3 | 50→55 | 50→20 | 10→4 | – |
| Scorched Swirl | 5→1 | 35→22 | 35→47 | 30→50 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 55→41 | 18→10 | 12→10 | 48→25 | 50 | 0 | 50→55 | 50→20 | 10→4 | – |
| Molten Chorus | 5→1 | 35→22 | 35→47 | 30→50 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 55→67 | 80→125 | 48→25 | 55→54 | 0 | 50→55 | 50→20 | 10→4 | – |
| Sub Furnace | 5→1 | 35→22 | 35→47 | 30→50 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 18→10 | 12→10 | 48→25 | 60→58 | −30→−12 | 65→73 | 75 | 10→4 | – |
| Ribcage | 5→1 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 18→10 | 12→10 | 48→25 | 45 | −10→−3 | 60→67 | 50→20 | 60→9 | – |
| Tar Pit | 25→12 | 35→22 | 35→47 | 30→50 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 18→10 | 12→10 | 50→27 | 50 | 0 | 50→55 | 50→20 | 10→4 | – |
| Breathing Walls | 5→1 | 35→22 | 35→47 | 30→49 | 30→6 | 80→98 | 30→25 | 50→41 | 12→18 | 30→21 | 18→10 | 12→10 | 48→25 | 45 | 10→3 | 50→55 | 50→20 | 10→4 | – |
| Chopped Space | 5→1 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 50→41 | 35→54 | 30→21 | 18→10 | 12→10 | 50→27 | 45 | 10→3 | 50→55 | 50→20 | 10→4 | – |
| Vocoded Fire | 5→1 | 35→22 | 35→47 | 30→50 | 30→6 | 30→25 | 25→19 | 55→47 | 12→18 | 30→21 | 18→10 | 12→10 | 48→25 | 55→54 | 0 | 50→55 | 50→20 | 10→4 | – |
| Clanking Choir | 5→1 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 60→77 | 90→137 | 48→25 | 45 | 10→3 | 50→55 | 50→20 | 10→4 | – |
| Pulse Choir | 5→1 | 35→22 | 35→47 | 30→47 | 30→6 | 30→25 | 25→19 | 70→65 | 12→18 | 30→21 | 60→77 | 100→146 | 45→23 | 20 | 0 | 30→31 | 50→20 | 10→4 | – |
| Two Octaves Down | 5→1 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 75→71 | 12→18 | 30→21 | 18→10 | 12→10 | 48→25 | 45 | 10→3 | 60→67 | 50→20 | 10→4 | – |
| Gravel Pit | 5→1 | 35→22 | 35→47 | 30→50 | 30→6 | 30→25 | 25→19 | 90→88 | 12→18 | 30→21 | 18→10 | 12→10 | 50→27 | 55→54 | 0 | 50→55 | 50→20 | 10→4 | – |
| Iron Lung | 5→1 | 35→22 | 35→47 | 30→49 | 30→6 | 80→98 | 35→31 | 60→53 | 50→77 | 50→37 | 18→10 | 12→10 | 48→25 | 45 | 10→3 | 50→55 | 50→20 | 10→4 | – |
| Detail Only | 25→12 | 50→37 | 45→50 | 50→54 | 50→12 | 70→81 | 35→31 | 50→41 | 25→38 | 35→25 | 35→31 | 15→14 | 45→23 | 35 | 0 | 50→55 | 50→20 | 30→4 | – |
| Subtle Size | 15→6 | 35→22 | 35→47 | 30→47 | 30→6 | 30→25 | 25→19 | 50→41 | 15→23 | 20→14 | 20→12 | 15→14 | 25→12 | 20 | 0 | 35→37 | 50→20 | 20→4 | – |
| Clean Lift | 25→12 | 35→22 | 35→47 | 30→45 | 30→6 | 55→58 | 30→25 | 50→41 | 25→38 | 35→25 | 35→31 | 15→14 | 45→23 | 35 | 0 | 50→55 | 50→20 | 30→4 | – |
| Loud & Contained | 5→1 | 70→60 | 45→50 | 30→49 | 70→19 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 18→10 | 12→10 | 48→25 | 45 | 10→3 | 50→55 | 50→20 | 10→4 | – |
| Crunch Lift | 5→1 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 18→10 | 12→10 | 48→25 | 45 | 10→3 | 50→55 | 50→20 | 10→4 | Re-tuned (see below) |
| Smashed | 5→1 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 18→10 | 12→10 | 48→25 | 45 | 10→3 | 50→55 | 50→20 | 10→4 | Attack 0.05→0.2 ms |
| Phone Monster | 5→1 | 35→22 | 35→47 | 30→49 | 30→6 | 30→25 | 25→19 | 50→41 | 12→18 | 30→21 | 18→10 | 12→10 | 48→25 | 45 | 10→3 | 55→61 | 50→20 | 60→27 | – |

The parameter defaults move with Init, so a fresh instance sounds as it did:

- PITCH Drive 1, Contain 22, Gate 47, De-ess 49, Low 25, High 19
- Gravel 41, Shimmer 18, Swirl 21, Ensemble 10, Width 10
- Growl 25, Roar Tone 3, Tame 6, Sub 49, Sub Drive 20, Overtone 4

Several of these sit low on their knobs (Tame 6, Ensemble 10, Overtone 4, PITCH Drive 1): the previous settings were subtle, and the new laws spend more of the travel on what used to be the bottom.

Some values for modules that are switched off in a preset are converted anyway, so that switching the module on gives the sound the preset author left there.

### Deliberate changes to presets

These presets change on purpose, because they sat in a dead or unusable zone, or because a defect fix makes a setting finally do what it says:

| Preset(s) | Change | Reason |
|---|---|---|
| Every preset with Overtone at 40 or less (Init and most others) | Overtone becomes 4 on Add's law (−5 dB, the previous law's 10 %): a light touch that can just be measured | The previous values were all in the Add dead zone. A literal conversion would set many of them to 0 |
| Respirator Rasp | Detail Low 85 → 100 (= the previous 80) | 85 sat in the unusable zone: room tone +35 dB |
| All presets with Sub on, except Kaiju, Hellhound and Sub Furnace | Sub Drive becomes 20, the highest value whose products stay inaudible (confirm by re-sweep after the fix) | Sub Drive was inaudible, so 20 keeps the sound |
| Kaiju, Hellhound, Sub Furnace | Sub Drive stays at 65 / 70 / 75 and is now heard | These presets chose a growlier sub |
| Kaiju, Hellhound, Rasp Machine | The Rattle shape now rattles | Rattle was identical to Sine |
| Every preset using Warm or Grind | Character is slightly stronger | Warm was near dead |
| Every preset using ENSEMBLE, most audibly Slow (Liquid Beast, Kraken) | Less 6.2 Hz pitch wobble | Slow wobbled most |
| Rasp Machine | Square carrier loses its fizz above 10 kHz | Band-limited square |
| Symbiote, Golem, Throat Singer (Tight) | The sub stays at 55 Hz or above for voices whose f0 sits above about 110 Hz after pitch | Tight now does what it says |
| Kraken | Swirl circuit Deep 12 → Deep 6, motion Random → LFO+Level, rate 0.15 → 0.25 Hz | Its swirl was inaudible (motion 10.12 → 10.17 dB over the whole amount sweep) |
| Crunch Lift | Clip 3 → 1 dB, Clip Shape 60 → 32 (converted), Mix 70 → 90 % and Push 10 → 7 dB (set by render: the same loudness as the previous render within 0.4 LU; the 40–80 Hz share is 1.1–1.6 dB lower, the closest any Mix gets) | Its fast release never acted: Clip took all the overload. Under the level-matched mix the dry is pushed too, so the previous blend corresponds to about 90 % |
| Presets with Roar Drive 45–50 | Scoop up to 0.35 dB lighter | Smooth scoop curve |

### Saved sessions, user presets and automation

- **Sessions and user presets saved by 1.0.0 load close to their old sound, not identical to it.** On load, any state without the new law marker gets the same conversions as the factory presets, applied to every remapped value (without the deliberate changes).
  - The knob values convert exactly for Contain, Detail, Gravel, Shimmer, Swirl, Width, Growl, Tame, PITCH Drive, Roar Tone, Sub, Clip Shape and Push, and for Ensemble up to 70 % (higher values load as 100 %, the new top).
  - They are close (within the audible thresholds above) for De-ess, Roar Drive and Overtone. De-ess uses the measured map above 40 %.
  - **Gate** is matched on its depth only. One knob now also sets the ratio (1.5 + 3.5u, was 2.5) and the threshold (6 + 12u dB over the floor, was 10 dB), so a 1.0 Gate closes on slightly more or fewer gaps: 25 % converts exactly, 10 % sits 2.4 dB lower, 35 % 1.6 dB higher and 37.5 % or more 2 dB higher.
  - **Swirl** keeps its mix, but its resonance changed with the module (no drop at a small mix, no 1.3·k0 stop at a large one), so Swirl settings under about 30 % or over about 60 % (as mix) ring differently.
  - **Detail Low**'s quiet tracker and boost cap act at every setting, so a 1.0 session lifts less room tone than it did (on excerpt A at Gate 35, room tone at Low 100 % went from +9.2 to +0.8 dB).
  - These module changes have no version switch: a 1.0 state always plays with the 1.1 modules.
- **Input with Auto Level on:** old values inside ±12 dB had no effect, so they load as 0 dB. Values beyond that load as 2 × (old ∓ 12) dB.
- **Ring Amount in AM mode** loads as old².
- **Sub Drive** loads as 20, its inaudible equivalent.
- **Ensemble in Vibrato** cannot be converted exactly, because every old amount from 10 % up sounded the same. It loads as the amount whose new curve gives the previous 70 %.
- **Host automation lanes are stored as normalised knob positions and cannot be converted.** After the update, an automation lane on any knob listed in "Changes in 1.1.0" plays back a different value.
- Growl Rate values below 2 Hz load as 2 Hz. Shimmer Rate values above 25 Hz load as 25 Hz. Attack values below 0.2 ms load as 0.2 ms.

### How the 1.1.0 build implements this

Where the build differs from the plan above, or fills in a detail, it does so for these reasons:

- **One home for the laws.** `Source/dsp/KnobLaws.h` holds every law, the 1.0.0 laws (`v1`), the converters and `migrateFromV1`. The modules, the creature view, the state migration and the tests all use it. States are written with `stateVersion` 3, and user presets with format `version` 3. Anything older is converted on load.
- **Sub Drive.** Moving the low-pass to 4·fsub on its own would also have passed the divider's own third harmonic, which is only 4.4 dB under the sub. That would have changed every preset. Instead, the shaper now runs on the band-passed sub (a near sine), followed by a 2nd-order low-pass at 4·fsub. The shaper sees the Drive law minus 6 dB, so 0–20 % stays within 1 dB of the clean sub's own 3rd harmonic. On a 110 Hz sine, the 165 Hz line goes −29.5 / −28.8 / −21.2 / −13.6 dB under the sub at 0 / 20 / 50 / 100 %. Sub Level means the same octave level at every Drive.
- **Rattle.** The R − 12 dB threshold drives only Rattle's rate push. The +30 % envelope depth push keeps R + 6, so no other shape changes.
- **Grind.** At +12 dB, Grind's curve flattened the open part of the cycle, and its tremolo read 27 % shallower than Warm's. Grind's depth is now scaled by 1.2. The 27 Hz modulation peak then sits at 0.90 / 1.04 / 1.09 × Warm's at Depth 25 / 50 / 80 %.
- **Detail's gate guard.** When CLEAN runs before DETAIL, DETAIL reads CLEAN's gate gain sample by sample. Between −1 and −5 dB of gate reduction, it removes the net boost. Since it works per sample, it does not depend on the host's block size.
- **DC.** The DC came mostly from the limiter's gain following an asymmetric wave, not only from the clipper. So the limiter's reduction, (g − 1)·x, and the clipper's residual each go through a 10 Hz one-pole high-pass. Material the output stage leaves alone still passes bit-exactly, so the null and latency tests are unchanged. The safety stage stays unfiltered: filtering it let peaks through to the final clamp. Smashed at Push 24 measures a mean of −0.00016 on the real recording and −0.00003 on the reference take. In 1.0.0 these were −0.0088 and −0.0039. On the test suite's synthetic voice it is 0.0005, where the plan asked for 0.0001. The test allows 0.001.
- **Mix.** The dry is multiplied by the same Push gain as the wet, and the blend is cos/sin. At Mix 100 % the wet passes exactly as before.
- **ROAR makeup.** The table was recalibrated so that every (Drive, Tone) knob position keeps the loudness the 1.0.0 table gave it. It was measured with the previous and the new laws on the reference take and on voice-shaped pink noise. Above 50 % Drive the new table adds up to 6.4 dB, which absorbs the steeper drive.
- **Ring Freq centre.** The log taper's mid point is √(20 × 4000) = 283 Hz. 150 Hz sits at 38 % of the travel.
- **Clip Shape.** The plan's squared taper, w = min(0.5, cap)·(1 − s)², moved the change to the soft end instead: at Clip 3 and 6 dB (Push 14), the steps from 60 % to Hard nulled 52–80 dB under the signal, so the last 40 % did nothing. The build uses the linear taper over the allowed knee, w = min(0.5, cap)·(1 − s). Each 20 % step then nulls within about 12 dB of the others at Clip 1, 3 and 6 dB, with nothing dead below 1 − 2·cap. Hellhound, Rasp Machine and Crunch Lift keep their knee width (Clip Shape 32, 70 and 32).
- **Preset rounding.** Values round to whole percent. The largest rounding effects are PITCH Drive 5 → 1 (exact 1.1: 0.24 against 0.27 dB of drive gain) and Ensemble 26 → 10 (exact 10.4: m = 0.176 against 0.182).
- **De-ess.** The detector's threshold and the range move together over the top 60 % of the travel, smoothed like the range (20 ms). Below 40 % the module is unchanged.
- **DETAIL's quiet tracker.** It takes the quietest 10 ms level of each 0.25 s segment, keeps the last 8 (2 s), and uses the lowest + 5 dB as a floor, at most −45 dBFS. Control blocks where CLEAN's gate is reducing by more than 1 dB are left out, so a closing gate cannot pull the floor down. The guard uses the higher of this floor and the stationary one. The boost cap (1 dB of boost per dB above the floor + 2 dB) applies to the net boost of both sections. It works per 32-sample control block, so it does not depend on the host's block size. Breaths, 10–20 dB above the room, still come up.
- **Overtone Add law.** Lean and Replace keep their law and makeup. A mode change glides the gain (20 ms), so switching Add ↔ Lean at the same Amount changes the layer's level by the difference of the two laws.
- **Swirl.** Only the resonance changes, and only at mixes below 0.3 (it holds 0.94·k0) and above 0.61 (it keeps rising instead of stopping at 1.3·k0). The factory presets sit at mixes of 0.20–0.55: Big Bad Narrator and Subtle Size (Swirl 14, mix 0.20) get 13 % more resonance, the others are unchanged.
- **Parameter defaults.** They follow Init, so PITCH Drive, De-ess, Ensemble, Tame, Roar Tone and Overtone have new defaults (listed above).

## Verification of the 1.1.0 laws

The knobs the review left with dead or weak travel were swept again in Init on excerpts A and B and the reference take (11 steps), with the build before these fixes and with 1.1.0. Each knob is judged on the measure that fits it, as in the audit: a narrow-band processor on its own bands, the gate and DETAIL's guard on the room tone, a tilt on the tilt, a drive on its distortion, and the time-varying effects on their spectral motion. A step under 0.4 dB of its own measure counts as dead.

| Knob | Measure | Before | 1.1.0 |
|---|---|---|---|
| De-ess | Mean change of the 5–12.6 kHz bands per 10 % | 70–100 %: −0.7 / −0.4 / −0.2 (A), −0.3 / −0.2 / −0.1 / −0.1 / −0.1 from 50 % (reference) | −0.6 to −2.1 dB on every step of all three takes; 70–100 %: −1.8 / −1.2 / −0.6 (A), −2.1 / −2.0 / −1.5 (reference) |
| Tame | Mean change of the 3–13 kHz bands per 10 % | Reference take: −0.2 / −0.2 / −0.3 below 30 %; A and B −0.3 to −0.4 below 30 % | −1.0 to −1.3 (A, B), −0.5 to −1.2 (reference) |
| Gate | Room tone: median of the quietest 15 % of frames, against speech, 0 → 100 % | A +13.9 → +7.3 dB, flat from 40 %; B +10.6 → +1.8 dB | A +11.7 → −6.7 dB, 1.4–3.0 dB per 10 % from 30 %; B +9.8 → −10.8 dB; the reference take unchanged (+5.1 → −29.8 dB) |
| DETAIL Low 0 → 100 at Gate 35 | Room tone DETAIL adds (same measure) | A +9.2 dB, B +6.1 dB, reference −5.5 dB | A +0.8 dB, B −1.8 dB, reference −5.3 dB. Breaths in the gaps still come up (on A the power mean of those frames still rises 6.7 dB) |
| Overtone (Add) | 200–315 Hz per 10 %; 160–500 Hz share at 100 % | +0.1 to +0.6 dB (2.6 / 3.4 / 4.1 dB in all, 0.7–1.4 dB by 50 %); share −5.0 / −7.0 / −5.2 dB | +0.3 to +0.7 dB (4.6 / 5.9 / 6.4 dB in all, 2.1–3.4 dB by 50 %); share −3.4 / −5.1 / −3.5 dB, at or under the dry voice's −2.1 / −4.3 / −3.5 dB |
| Overtone Frequency (Add, Amount 50) | Mean change of the 100 Hz–1.3 kHz bands per 0.2 oct | 0.1–0.4 dB | 0.2–0.7 dB; two steps of 0.2–0.3 dB at the bottom of the range, where the bands sit at 35–50 Hz |
| Ensemble | Spectral motion per 10 % | A: +0.0 dB over 0–30 % | A: +0.03 to +0.14 dB on every step, rising from 10 %; the static spectrum stops changing above 80 % (the comb is deepest near an equal mix), the motion keeps rising |
| Swirl | The audit's DEAD flags | Reference take: 50–70 % dead | Reference take: none. A: single dead steps. B: 0–20 % (its motion is flat there in both builds) |
| Roar Tone, Drive 30 and 45 | Tilt (3–13 kHz against 50–250 Hz) per 20 % | 0.3–1.3 dB | 0.8–2.0 dB; the loudness over the whole Tone range moves 0.9–1.1 LU (was 0.4–0.5) |
| PITCH Drive | THD of a 110 Hz sine (PITCH alone), points per 10 % | 0.9 / 0.5 / 0.7 / 1.1 / 1.6 / 2.6 / 3.6 / 4.6 / 5.1 / 5.3 | 1.5 / 1.0 / 1.4 / 1.9 / 2.5 / 3.2 / 3.6 / 3.7 / 3.6 / 3.6. On a voice in Init the spectral flags stay under their thresholds through 10–60 % in both builds: ROAR and GROWL sit after it and mask it |

No sweep shows RUNAWAY or BROKEN. All 40 factory presets, rendered with both builds on the three takes, stay within 0.26 LU and 0.42 dB of mean 1/3-octave change; the largest single band is 2.0 dB (5–8 kHz on Demon Whisper and Cyber Commander, whose old De-ess settings use the measured map).

## CHAMBER (new in 1.1.0)

CHAMBER was audited the same way as the other modules, with the build that carries the bundled rooms and with the build that falls back to the modelled rooms. It is new in 1.1.0, so its laws and defaults are chosen here and no saved state needs converting.

### How CHAMBER was measured

- **Material:** voice excerpts A and B (96 kHz), the reference take (48 kHz), excerpt A scaled so its voiced frames sit at IN's working level (−20 dBFS RMS), an impulse at 48 and 96 kHz, and sines at 110 Hz (−12 dBFS), 997 Hz (−20 and −6 dBFS) and 2.5 kHz (−20 dBFS, at 48 and 96 kHz).
- **Contexts:**
  - CHAMBER on its own, with IN Auto Level off and OUTPUT's filters, auto gain and Ride off (the render harness's new `--raw-out`), Push 0 dB, Ceiling 0 dB;
  - Init with CHAMBER on;
  - Kaiju with CHAMBER on, as the dense preset.
- **Sweeps:** Chamber, Resonance, Edge and Width over 11 steps; every Room, Speaker and Boom option; the position over all 15 slots. About 1,100 renders in all.
- **The module's own response:** with Edge at 0 % CHAMBER is linear, so its exact response comes from an impulse render divided by the rest of the chain's (an all-pass). The wet part W follows from the known dry gain. Decay, mode prominence, tone and per-room loudness are measured on W. Real voices are then convolved with W for the loudness figures.
- **Measures that fit each control:**

| Control | Measure |
|---|---|
| Chamber | Wet against dry (dB); the ring heard in the pauses (frames more than 30 dB under the voice, against voiced frames, compared with the dry take); comb colour (ripple of the blend's first 30 ms, 200 Hz–4 kHz, against its 1/3-octave average); loudness |
| Resonance | EDT and T20 of the wet; the ring 150–450 ms after an impulse, against the whole; mode prominence (the strongest peaks of the late part over their 1/3-octave median); the wet's loudness |
| Edge | Measured before the room: the wet is divided by W, so the room's modes do not colour the numbers. THD and inharmonic (aliasing) energy on the sines; on the voice, the distortion residual left after a 32-tap least-squares fit per 1 s segment |
| Width | Side against mid, L/R correlation, and the loudness of the mono fold-down |
| Room, Speaker | The wet's loudness against the dry take on all three takes; EDT, T20, the strongest mode and the wet's spectral centroid |
| Boom | 1/3-octave level at 40–100 Hz, and what it costs at 1–4 kHz once OUTPUT's limiter reacts |

The automatic DEAD flags were overridden as for the other modules:

- Resonance changes the ring and holds the level by design, so loudness and 1/3-octave metrics read it as dead. It was judged on decay.
- Edge is low-order distortion of the speaker's lows. It was judged on the distortion residual.
- Width's first step reads RUNAWAY because the output turns from mono to stereo there.

### Defects found and fixed

| # | Defect | Effect before the fix | Fix |
|---|---|---|---|
| 1 | The speakers' filters, and the excursion filter of the Edge stage, never received their coefficients (they ran as pass-throughs) | Tiny, Small and Horn sounded the same: the wet's centroid was 475 Hz with every speaker in Steel Box, and only the makeup gain differed. Edge broke up the whole band instead of the speaker's lows | The coefficients are set whenever a speaker type is set, and copied with the state. The wet's centroid is now 990 / 540 / 1,300 Hz (Tiny / Small / Horn). A test checks that the Small box keeps 15–23 dB more energy under 220 Hz than the other two |
| 2 | Resonance lengthened the decay from 10.7 ms on, but four of the captured rooms swell in for 75–130 ms before they decay | Soft Bloom at 100 %: the swell rose by 20 dB, the decay itself barely changed (T20 0.20 → 0.22 s), and the wet came out 9 dB louder than at 40 % because the level hold hit its 12 dB limit | The decay change starts where the room's late energy peaks (37–133 ms in the captures, 16 ms in the modelled rooms). Soft Bloom now holds its level within 0.2 dB over the whole travel |
| 3 | At high Resonance the lengthened decay ran past the end of the IR, and a fade over its last 30 % cut it off | Concrete Room at 100 %: the ring fell from −29 to −89 dB in the last 150 ms, an audible gate | Each room gets a longest decay: the largest scale (at most ×1.6) that keeps the tail 40 dB under its peak where the IR's final fade starts (its last 15 %). Resonance's top end reaches that scale. In the captures it is ×1.14 (Resonant Chamber), ×1.2 (Concrete Room) and ×1.56–1.6 for the rest; the modelled rooms get ×1.38–1.6 |
| 4 | The rooms and speakers were level-matched on pink noise between 150 Hz and 5 kHz, which carries far less energy under 500 Hz than speech | The eight rooms' mean levels spread over 3.1 dB on real voices (captures) and 1.7 dB (modelled). With the speakers working (fix 1), Tiny and Horn read 0.9 and 1.6 dB quieter than Small on average (2–3 dB on excerpt A) | Both matches are weighted by the long-term average speech spectrum times the BS.1770 K weighting. The rooms' mean levels now spread over 0.7 dB (captures) and 1.5 dB (modelled). Horn carries a further −0.9 dB, measured on the three takes |
| 5 | The Chamber makeup assumed the wet and the dry were 20 % correlated; they are close to uncorrelated | The output lost 1.0–1.2 LU at 80–100 % compared with the dry (captures, measured on the module's own response) | The makeup is the plain power sum. The loss is now at most 0.5 LU over the whole travel |
| 6 | Width scaled the room's side linearly | At Chamber 50 % the L/R correlation moved only 0.002 / 0.005 / 0.008 over the first three 10 % steps | The side gain is Width^0.75 (see the table below) |

Nothing clamped, produced non-finite samples or turned BROKEN in any sweep. The lowest intelligibility is 0.52, on Kaiju at Chamber 100 % on excerpt A.

### Findings per control

| Parameter | Range / default | Usable | Dead | Unusable | Taper | Decision |
|---|---|---|---|---|---|---|
| Chamber | 0–100 %, 50 | 5–100 %. The wet goes from −23 dB to +5.5 dB against the dry, 1.5 dB per 5 %. The ring in the pauses grows 1.2–1.3 dB per 5 % up to 80 % (excerpt A: +2.0 dB at 5 %, +11.5 dB at 50 %, +18.4 dB at 80 %, +20.9 dB at 100 %) | None. Below 30 % the 1/3-octave metrics read dead in Init, but the pauses fill by 2–6 dB | None. Intelligibility in Init falls 0.91 → 0.78 → 0.62 → 0.57 at 0 / 50 / 80 / 100 %, as a room should. Comb colour stays small with the captures (ripple 0.3 dB at 50 %, 1.9 dB at 100 %); the modelled rooms' discrete early reflections comb more (1.9 / 5.5 dB) | Linear in dB (`laws::chamberBalanceDb`), even | **Law kept**; makeup fixed (defect 5). Loudness against the dry: −0.07 / −0.35 / −0.49 LU at 50 / 80 / 100 % (the first build: −0.77 / −1.83 / −2.33 LU) |
| Resonance | 0–100 %, 40 | 0–100 %. Steel Box T20 0.26 / 0.42 / 0.63 s at 0 / 40 / 100 %, EDT 0.26 / 0.57 / 0.80 s; the late part's level −9 / 0 / +9 dB | None on decay. The loudness and 1/3-octave metrics read it dead, as designed | Only the defects above | Late level linear in dB with a knee at 40 %; decay ×0.55 → ×1 over 0–40 %, then ×1 → the room's longest. T20 grows 0.04–0.05 s per 10 % | **Laws kept** and moved to `laws::chamberLateLevelDb` / `chamberDecayScale`. The decay start and the per-room limit are new (defects 2 and 3). The wet's level holds within ±1 dB of its 40 % value on real voices in every room (Resonant Chamber −0.9 / +1.2 dB at the ends, the widest; it was −3.5 / +9.1 dB in Soft Bloom) |
| Edge | 0–100 %, 30 | 10–100 % at IN's working level. The distortion residual on the voice before the room (Small) is −27 / −24 / −20 / −15 / −11 / −9 dB at 10 / 30 / 50 / 70 / 90 / 100 %, about 2 dB per 10 %. THD on 110 Hz at −12 dBFS: 3.5 % at 10 %, 10 % at 50 %, 16–18 % from 70 % (Small); Tiny and Horn reach 25–29 % | Heard only where the wet is: at Chamber 50 % the residual is 9 dB further down, under the rest of Init. At Chamber 80 % in Init, Edge 30 % already changes the 0.5–2 kHz band by −24 dB, more than a 10 % step of ROAR Drive (−29 dB) | None. Aliasing is −66 dB at worst (Horn, 997 Hz at −6 dBFS, 48 kHz, Edge 90–100 %) and under −75 dB everywhere else | Linear in dB of drive (`laws::chamberEdgeDriveDb`, 24 u dB). Even at the working level. Without Auto Level, on a quiet take, the bottom 40 % does little | **No change.** The audit's even feel holds where the plug-in runs: IN puts the voice at the working level |
| Width | 0–100 %, 35 | 0–100 %. At Chamber 100 % the L/R correlation is 1 / 0.89 / 0.71 / 0.53 / 0.35 / 0.20 at 0 / 20 / 40 / 60 / 80 / 100 % (it was 1 / 0.95 / 0.81 / 0.61 / 0.40 / 0.20) | Linear gain: the bottom 20 % was nearly mono (correlation 0.993 at 20 % with Chamber 50 %) | None. The mono fold-down is the same at every Width, bit for bit (0.00 LU with CHAMBER on its own). In Init it moves by at most 0.45 LU, through OUTPUT's limiter | Side gain w^0.75 (`laws::chamberSideGain`) | **Remapped.** At Chamber 50 % the correlation steps are now 0.005–0.022 per 10 %, spread evenly (they were 0.002–0.028 and back-loaded) |
| Room | 8 rooms, Concrete Room | All. See the room table | – | – | – | Level-matched (defect 4): on voice-band pink the eight rooms sit within 0.7 dB (captures) and 1.8 dB (modelled). A single sung pitch still lands on or between a room's modes (−3.2 to +2.9 dB at 110 Hz) |
| Speaker | Tiny / Small / Horn, Small | All (after defect 1) | – | – | – | Small is the fullest (energy under 220 Hz −10 dB of its wet). Tiny thins to a phone-sized speaker (−25 dB). Horn is a narrow paging band (−34 dB), the most "PA" |
| Boom | Off / Light / Medium / Heavy, Light | Off to Heavy in Init: 40–100 Hz +1.9 / +3.6 / +5.1 dB, loudness +0.27 / +0.52 / +0.78 LU, 1–4 kHz −0.4 / −1.0 / −1.7 dB | – | Medium and Heavy on a preset that already drives OUTPUT's limiter with lows. On Kaiju, 40–100 Hz gains only +1.0 / +1.7 / +2.3 dB while 1–4 kHz drops 1.6 / 3.5 / 5.5 dB and pumping rises 2.6 → 3.1 LU | The shelf is within 0.2 dB of +3 / +6 / +9 dB at 30 Hz | **No change.** Boom works after the whole chain, as designed, so it also lifts SUB and the pitched voice. The guide says to keep it at Off or Light on dense, low presets |
| Position | Insertion slot 0–14, 1 | All slots | – | – | – | Moving CHAMBER changes the loudness by at most 0.3 LU and keeps the latency (as the tests check). The slot decides what processes the room: see the interactions |

### The rooms

Measured at Resonance 40 % with the Small speaker, 96 kHz. Loudness is the wet against the dry take on excerpt A / excerpt B / the reference take, averaged over the three speakers. Each column's mean differs by take (−1.4 / +0.1 / +1.0 dB), because the takes carry different amounts of low end under the speakers' high-pass. Each room stays within 1.3 dB of its column's mean with the captures, and within 1.8 dB with the modelled rooms.

| Room | EDT / T20 (s), captured | EDT / T20 (s), modelled | Strongest mode, captured | Strongest mode, modelled | Wet centroid, captured / modelled | Loudness, captured (dB) | Loudness, modelled (dB) |
|---|---|---|---|---|---|---|---|
| Steel Box | 0.57 / 0.42 | 0.43 / 0.42 | 0.92 kHz (+11 dB) | 2.72 kHz (+13 dB) | 543 / 790 Hz | −1.1 / +0.0 / +0.4 | −0.2 / +0.8 / +2.5 |
| Resonant Chamber | 0.88 / 0.70 | 0.38 / 0.71 | 2.23 kHz (+9 dB) | 2.24 kHz (+8 dB) | 557 / 730 Hz | −1.6 / +0.1 / +2.3 | −1.1 / −0.1 / +0.7 |
| Concrete Room | 0.94 / 0.62 | 0.59 / 0.62 | 1.94 kHz (+11 dB) | 1.72 kHz (+9 dB) | 525 / 706 Hz | −1.7 / −0.4 / +1.2 | −0.0 / +0.4 / +0.3 |
| Tin Booth | 0.56 / 0.52 | 0.59 / 0.53 | 2.89 kHz (+8 dB) | 2.87 kHz (+12 dB) | 878 / 750 Hz | −0.9 / +0.8 / +1.3 | −1.7 / −0.1 / +0.3 |
| Padded Cell | 0.52 / 0.37 | 0.36 / 0.37 | 1.66 kHz (+7 dB) | 1.66 kHz (+10 dB) | 421 / 751 Hz | −1.5 / −0.3 / +1.0 | −1.2 / +1.3 / +0.5 |
| Wood Chamber | 0.95 / 0.54 | 0.59 / 0.59 | 2.56 kHz (+8 dB) | 2.51 kHz (+8 dB) | 559 / 599 Hz | −1.4 / +0.4 / +0.4 | −1.3 / +0.2 / +0.7 |
| Timber Room | 0.92 / 0.54 | 0.59 / 0.54 | 2.74 kHz (+8 dB) | 0.62 kHz (+9 dB) | 546 / 604 Hz | −1.3 / +0.5 / +0.6 | −1.5 / +0.7 / +0.1 |
| Soft Bloom | 1.54 / 0.17 | 0.37 / 0.27 | 1.99 kHz (+9 dB) | 2.90 kHz (+8 dB) | 508 / 623 Hz | −1.5 / −0.1 / +0.7 | −1.0 / −0.1 / +0.6 |

The modelled rooms match the captures' T20 and modes well. They differ in three ways:

- **No swell.** The captured Concrete Room, Wood Chamber, Timber Room and Soft Bloom swell in for 75–130 ms; the modelled ones peak at once. This is why their EDT reads shorter.
- **Brighter.** Their wet is brighter (centroid 600–790 Hz against 420–560 Hz).
- **Discrete early reflections.** These comb more against the dry.

A build without the captures still loads every preset with the same room names, at the same levels within about 1 dB.

### Interactions

- **Chamber × Edge:** Edge is heard in proportion to the wet. At the default Chamber 50 % it adds texture only from about 60 %. From Chamber 65 % up its whole travel is audible.
- **Chamber × Width:** Width acts on the wet only. At Chamber 50 % the widest setting gives a correlation of 0.84; at Chamber 100 %, 0.20.
- **Resonance × Room:** each room's longest decay sets how far the top half of Resonance can lengthen the ring. In Resonant Chamber and Concrete Room the top half mostly raises the late level (+9 dB at 100 %, T20 +0.05 / +0.12 s). In the other rooms T20 grows up to ×1.5.
- **Edge × Speaker:** the breakup sits on the speaker's lows. Tiny and Horn distort most: residual −6.7 / −7.9 dB at 100 %, against −9.0 dB for Small. Small's THD on a low sine levels off above 70 % because its box passes the fundamental.
- **Boom × OUTPUT:** Boom is post-chain. On presets that already push OUTPUT's limiter with lows (Kaiju), Medium and Heavy duck the voice's presence (above).
- **Position:**
  - **Before PITCH:** the room's modes move with the pitch.
  - **After PITCH (slot 1, the default):** the pitched voice in the room. CLEAN's gate then trims the tail, and DETAIL lifts the ring.
  - **After ROAR (slot 10):** an amp into a cabinet. The speaker band-limits ROAR's fizz.
  - **At the end:** the room over everything, SUB included.

  Loudness, centroid and intelligibility move little between slots: 0.3 LU, about 40 Hz and 0.05 at most. Kaiju's side signal drops about 4 dB once CHAMBER sits after VOCODER, which otherwise widens the room further.

### CPU

CHAMBER on its own, one core, steady state: 0.60 % (Width 0 %) and 0.83 % (Width 100 %) at 48 kHz; 4.1 % and 6.7 % at 192 kHz. A Room change (or Width turning on) costs more for a moment: the new room's current block of partitions is computed at once on the audio thread, then both rooms run through the 50 ms crossfade. At 192 kHz with 256-sample blocks and Width 70 %, the block with the change takes about 2.8 times a normal block (about 280 µs, 21 % of the block), and the crossfade blocks about 1.5–1.8 times. The tests check that the worst block stays under half of the block's time. Automating Room on many instances at once adds these peaks together.

### Sweet spots (for presets)

| Control | Guide |
|---|---|
| Chamber | 15–35 % a hint of a box behind the voice (wet −20 to −13 dB); 45–65 % a clear chamber (−10 to −4.5 dB); 75–90 % the voice inside the can (wet at or over the dry); 100 % the dry 5.5 dB under the room |
| Resonance | 0–25 % tight and dry (T20 ×0.55–0.85); 40 % the room as captured; 60–100 % a ringing room (the late part +3 to +9 dB, decay up to ×1.6 where the room allows) |
| Edge | Needs Chamber 60 % or more to be heard. 15–35 % speaker grit; 50–70 % a cranked loudspeaker; 85–100 % blown. Tiny and Horn break up sooner than Small |
| Width | 20–40 % a room just off mono; 50–70 % a clearly stereo room; 100 % the room's full stereo. Mono-safe at every setting |
| Room | Steel Box and Tin Booth for metal and machines; Resonant Chamber and Concrete Room for a hard, ringing chamber; Wood Chamber and Timber Room for warm creatures; Padded Cell for a tight, dead box; Soft Bloom for a soft swell with almost no ring |
| Speaker | Small for full creatures; Tiny for radio, intercom and phone voices; Horn for PA, megaphone and robot voices |
| Boom | Light by default; Medium only on presets that are not already low and dense; Heavy for a deliberate thump with a lighter SUB |
| Position | After PITCH for a pitched voice in a room; after ROAR for an amp into a cabinet (the "villain" order); at the end to put the whole creature in the room |

## Random ranges (the dice)

The dice (Randomise All and each module's die) roll every knob inside a random range taken from the tables above: the **Usable** column, narrowed to the sweet-spot guide where the extremes are harsh. The table lives in `Source/RandomRanges.h`, next to the parameter table, and the build checks that every module knob and mode has exactly one range inside its parameter's range. Hertz, ratio and millisecond knobs roll evenly over their knob travel; the others evenly over their values. Modes roll evenly over their options; only Boom Heavy is left out (a deliberate thump that ducks the voice on a low, dense chain). Randomise All turns 3–7 modules on, at least two of them not CLEAN, DE-ESS, DETAIL or TAME, and never touches the positions, IN or OUTPUT.

Where a mode changes what a knob does, the knob's roll is narrowed after the mode is rolled (`adjustModule`): SWIRL's Deep 6 circuit rolls 15–30 % (the other circuits 15–50 %); RING's Ratio rolls x1–x2 when Track is Follow (the carrier is then the ratio times the voice's own pitch, and under x1 a pitched-down voice put it at 20–25 Hz, a 0.99-deep chop in the 16–32 Hz band); PITCH in Mono, where the knob is a note (0 st = C3), rolls a whole note from A#1 to A#2, a fixed-note drone whatever the speaker's pitch. A deep PITCH can still take a Follow carrier at x1 near tremolo rates (45–56 Hz at −14 st).

**Loudness check.** Init, then Randomise All with seeds 1–200, rendered through `RoarschachTests --render ... --roll 1 200 --metrics` on five 14 s excerpts of the real voice recording at 96 kHz (from 1:35, 2:45, 5:00, 7:05 and 8:55 into it), 1,000 renders with the modelled rooms and 1,000 with the captured rooms. Loudness against Init, with the modelled rooms (the captured rooms read the same to within 0.15 LU at every percentile shown):

| Take | Init | min | 5th pct | median | 95th pct | max | Clamps, non-finite, over the ceiling |
|---|---|---|---|---|---|---|---|
| From 1:35 | −14.78 LUFS | −1.66 LU | −0.66 | +0.39 | +0.88 | +1.47 LU | none |
| From 2:45 | −14.32 LUFS | −2.15 LU | −1.10 | +0.20 | +0.75 | +1.34 LU | none |
| From 5:00 | −15.84 LUFS | −1.37 LU | −0.69 | +0.21 | +0.77 | +1.50 LU | none |
| From 7:05 | −13.26 LUFS | −3.76 LU | −1.80 | −0.25 | +0.52 | +1.06 LU | none |
| From 8:55 | −14.80 LUFS | −1.92 LU | −1.13 | +0.20 | +0.92 | +1.70 LU | none |

The first batch rolled Swirl up to 70 %, and three rolls lost 2.7–5.9 LU: SWIRL's Deep 6 circuit at 63–68 % with a deep GROWL or RING took more level than OUTPUT's auto gain gives back. A cap of 50 % for every circuit kept the two takes first measured within 3 LU, but on the other three, seeds 160 and 200 (Deep 6 at 46–49 %) still lost 3.3–5.8 LU. With Deep 6 capped at 30 %, 998 of the 1,000 rolls play within 3 LU of Init. Two rolls on the take from 7:05 do not: seed 200 (−3.76 LU: PITCH Mono, deep GROWL Square/Grind at 74 Hz, Deep 6 at 28 % and ROAR's Mic voicing; without ROAR it reads −0.7 LU, and Deep 6 at 15 % still leaves −2.8 LU) and seed 95 (−3.01 LU: CLEAN's Radio tone with a deep RING; without CLEAN it reads −0.6 LU). Neither is a harsh setting on its own: these are dense chains that each thin the voice a little.

**Pumping and intelligibility.** The level movement (pumping) of a roll reaches 3.8–4.1 LU on four takes (Init: 2.3–2.6 LU) and 5.4 LU on the take from 7:05 (Init 2.56), all from seed 161 (seven modules: CLEAN, DETAIL, SHIMMER Down, GROWL at 5.4 Hz, ROAR 79 %, ENSEMBLE and OVERTONE Add). It is the sum of the chain: taking any one module out leaves 4.0–4.9 LU, and SHIMMER or GROWL on their own with the same settings read 1.3–1.7 LU, so no single range was narrowed for it. The intelligibility proxy stays at or above 0.58 with the modelled rooms (seed 174 on the take from 8:55: PITCH −14 st, GROWL Rattle 77 %, CHAMBER 64 %) and 0.50 with the captured rooms.

## Deferred (not in 1.1.0)

| Item | Why it waits |
|---|---|
| VOCODER filter bank down to 35–40 Hz, so −1/−2 Oct carry a real sub-octave | Changes latency, CPU and the timbre of every vocoder preset; needs its own listening pass |
| Growl modulation applied to the SUB layer (SUB masks half of Growl Depth) | Makes every preset growl harder; the Depth remap and conversions are kept neutral until this is decided by ear |
| Sub Character made more distinct (Resonant Q 1.4 → 3, Thick band gain −6 → −2 dB) | Needs listening once Sub Drive is audible |
| Swirl LFO+Level envelope window (−30 dBFS over 18 dB), Random depth ×1.6, Deep 6 resonance cap 1.15·k0 | Each needs verifying by ear |
| Ensemble width depending on Motion | Needs a design decision |
| Triangle ring carrier made distinct from Sine | Changes saved sessions; the tooltip documents it as a softened sine |
| CHAMBER Resonance at a low Chamber setting: in Simple Size (Chamber 30 %, the wet 15 dB under the dry) Resonance 0 → 100 % moves the loudness 0.03 LU and the pauses 1.4 dB; what changes most is the width (side −26 → −20 dB). Resonance holds the wet's energy by design, so under a quiet wet it is a subtle control | A follow-up CHAMBER knob pass: judge Resonance on the tail at low Chamber, or let it change the blend more |
| Contain raising LRA on long clips (the leveller may hold gain reduction through pauses) | Needs listening to confirm before a change |

## Sweet-spot guide

Tooltips describe each control on its own terms, using these zones:

| Control | Guide |
|---|---|
| Pitch | −2…−4 st bigger human; −5…−9 creature; −10…−14 monster (add Size, or use Keep to hold the words); below −15 the words blur in Follow; above +6 the throat stops following |
| Size | Below −18 st with Follow, the throat is already at its lowest, so Size only adds body |
| Drive (PITCH) | Up to 25 % warmth, 35–70 % grit, 100 % heavy |
| Gate | 25–40 % for dialogue |
| Detail Low / High | 40–60 % / 30–60 % |
| Gravel | About 30 % gritty with the words fully intact; about 70 % dominant buzz |
| Ring Amount | 20 % metallic edge; 50 % equal metallic; 100 % pure ring |
| Ring Freq | 110–450 Hz metallic voice; below about 50 Hz tremolo; above about 1.3 kHz squeak |
| Ring Ratio | Integer ratios stay in tune; in-between ratios clang; below x0.35 it chops |
| Shimmer | 15–30 % subtle layer; 90–100 % full wobble |
| Shimmer Rate | 0.5–2 Hz liquid swell; 3–9 Hz warble; 13 Hz and up rough |
| Growl Depth | 15–35 % texture; 45–100 % obvious growl |
| Growl Rate | 5–7 Hz stutter; 20–35 Hz growl; 60–100 Hz buzz |
| Swirl Rate | 0.15–0.5 Hz slow liquid; 1–2.5 Hz animated; 4 Hz and up warble |
| Roar Drive | 25–45 % warmth and presence; 70–100 % grit |
| Width | Past about 100 % the chorus thins when summed to mono |
| Chamber | 15–35 % a hint of a box; 45–65 % a clear chamber; 75–90 % the voice inside the can |
| Resonance | 0–25 % tight and dry; 40 % the room as captured; 60–100 % ringing |
| Edge | Heard from Chamber 60 % up: 15–35 % grit, 50–70 % cranked, 85–100 % blown |
| Chamber Width | 20–40 % just off mono; 50–70 % clearly stereo; mono-safe throughout |
| Boom | Off or Light on presets that are already low and dense |
| Sub Level | 40–65 % solid octave. In Overtone Replace, Sub Level adds weight through the overtones |
| Push / Clip | Push 8–12 clean and dense; for loud, Push 14–18 with Clip 6–9 |
| Attack / Release | Only with Auto off and a short Release: Attack 0.3–3 ms crunches the low end. Release 2–10 ms crunch, 25–60 ms grit, 200 ms and up clean |
