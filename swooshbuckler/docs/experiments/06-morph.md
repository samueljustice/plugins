# Experiment 06: Morph (a MORPH-style spectral morph + XY pad)

Status: prototype. New files only. Nothing is wired into the app yet.
- DSP: `Source/SpectralMorph.h/.cpp` (class `SpectralMorph`)
- UI: `Source/MorphPad.h/.cpp` (class `MorphPad`)
- Test: `Tests/morph/` (standalone CMake project, target `MorphTest`)

Design rule (user): keep it simple. The pad is the control. There are three modes, a butterfly
"wander" toggle with one Speed slider, and two rope handles to pick the sources. Amount stays a
parameter for automation only. Level matching and silence handling run automatically.

---

## 1. What Zynaptiq MORPH does (what we are recreating)

Zynaptiq does not publish its algorithms. Everything below comes from product pages and reviews.
We recreate the *behaviour* with published techniques, not their code.

- Two inputs: the main input and a sidechain. An XY pad blends them and a set of algorithms does
  the morphing. MORPH 2 had 9 algorithms. MORPH 3 adds Interweave V3, Imprint Smooth, Imprint
  Crystal and Enharmonic, and MORPH 3 Pro adds Fusion and Sonance, for 11 in total. Several have
  low-latency "(LL)" variants.
  - https://www.zynaptiq.com/morph/
  - https://www.soundonsound.com/reviews/zynaptiq-morph-pro-3
  - https://www.bonedo.de/artikel/zynaptiq-morph-3-pro-test
- XY pad: in MORPH 3 Pro, the horizontal axis is a plain level crossfade between source and
  sidechain, and the vertical axis is the amount of morph. Reviews of MORPH 2 say moving up
  "creates steps between the signals" while left and right decide which signal dominates.
- The algorithms fall into three groups, going by their names and the reviews:
  - Envelope / spectral imprinting (Imprint, the vocoder-like ones): one signal's spectral shape
    is put on the other.
  - Harmonic / partial morphing (Interweave, Enharmonic): partials move between the sources.
  - Hybrid "fusion": a blend whose character changes along the morph axis.
- "Adaptive" behaviour: the output stays at a usable level whatever the inputs do (no level dip
  mid-morph, no hole when one input stops). The product is described as "structural morphing",
  with the analysis adapting to the material.
- Latency: reported MORPH 2 figures are about 2048 samples (46 ms) for Interweave, 512 samples
  (11.6 ms) for "Tight", and about 3 ms for the LL algorithms. The reviews are not consistent, so
  treat these as approximate. Zynaptiq recommended 512 to 1024 sample host buffers.

For Swooshbuckler we don't need 11 algorithms. Three distinct behaviours (envelope morph,
pitch-gliding morph, cross-synthesis) cover what a sound designer reaches for. Two of our three
modes keep a crossfade-free x axis, and y is a meaningful second dimension in each mode.

## 2. Published techniques that can recreate it

**Envelope and fine-structure separation**
- Cepstral smoothing (lifter the real cepstrum). This is cheap, but it averages through harmonic
  peaks, so the envelope sits too low on harmonic sounds.
- LPC envelopes. These model formants well for voice but are biased on non-speech material.
- True envelope: iterated cepstral smoothing of max(spectrum, previous envelope), which rides over
  the harmonic peaks. Röbel & Rodet, DAFx 2005, "Efficient spectral envelope estimation and its
  application to pitch shifting and envelope preservation".
  - https://dafx.de/paper-archive/details/EDSTSaO17Gd_0LV03iwDfw
  - Real-time version: https://hal.archives-ouvertes.fr/hal-01161347
- **Used here**: the true envelope with 3 iterations, a Hann lifter and a 1.2 ms quefrency cutoff
  (formants, not harmonics, at any rate). The fine structure is log|X| minus the envelope.

**Interpolating magnitudes**
- Linear magnitude interpolation is the same as a crossfade, so it gives two-tone mixtures.
- Log-magnitude (geometric) interpolation "fuses" the sounds but loses energy where the spectra
  don't overlap, which causes the classic mid-morph dip.
- Generalised power means sit in between. **Used here**: envelopes are interpolated in log, and
  the fine structures are combined with a power mean whose exponent is the y axis (p = 2: power
  mix, p -> 0: geometric fusion). A per-frame energy normaliser fixes the level.

**Sinusoidal models and partial matching**
- SMS (Serra): sinusoids plus a residual. Morphing matches partials across sources and
  interpolates their frequency and amplitude. Tellman, Haken & Holloway (JAES 1995) handle unequal
  numbers of partials. Caetano & Rodet (IEEE TASLP 2013) cover perceptually guided morphing.
  Slaney, Covell & Lassiter (ICASSP 1996) is the classic "automatic audio morphing" paper.
- These are good on clean harmonic sounds. They are fragile on noise and whoosh material, need
  partial tracking (state, births and deaths), and are costly to run robustly in real time.

**Optimal transport / displacement interpolation**
- Henderson & Solomon, "Audio Transport: A Generalized Portamento via Optimal Transport", DAFx
  2019. Each STFT frame is cut into "spectral masses" around reassigned peaks. The monotone 1-D
  optimal-transport plan between the two normalised spectra is computed by walking the two CDFs,
  and each matched slice is moved rigidly to the interpolated frequency. Phase vocoder
  resynthesis keeps each moved mass phase-locked to its peak. Pitches slide instead of
  crossfading, and it works on noise as well as tones.
  - https://arxiv.org/abs/1906.06763
  - https://dafx.de/paper-archive/details/ynpqc2bCKpKJiK_jFLBjkw
  - https://people.csail.mit.edu/theia/projects/audio_transport
- **Used here** for Glide: a simplified, allocation-free version of the same idea. Masses are
  split at the minima between peaks, the frequency comes from the phase-vocoder instantaneous
  frequency at the peak, the plan is a two-pointer CDF walk, and slices are bin-shifted. Phase
  uses identity phase locking (Laroche & Dolson, IEEE TSAP 1999): the peak phase advances at the
  moved frequency, and neighbour bins keep their offset from the peak.

**Phase handling options**
- Classic phase vocoder: horizontal coherence only, so it sounds phasey.
- Phase locking (Laroche & Dolson): vertical coherence around peaks. Used in Glide.
- Phase taken from the dominant source, or from the weighted complex sum. Used in Blend and Cross:
  the phase is arg((1-w)A + wB), where w is the fine-structure weight, so at the endpoints the
  phase is exactly the source's.
- PGHI / RTPGHI (Průša et al.): phase from the magnitude's phase gradient, with no peak picking,
  so it is good for heavily modified magnitudes. A possible upgrade for Glide and Blend.
  - https://arxiv.org/abs/1609.00291
  - "Phase Vocoder Done Right": https://arxiv.org/abs/2202.07382

**Cross-synthesis / vocoding**
- Classic channel or LPC vocoder: whiten the carrier and multiply by the modulator's envelope.
  With a true envelope this is the same split as above: fine structure from one source, envelope
  from the other. **Used here** for Cross.

**Recent work (2023-2026)**
- SoundMorpher (Niu, Zhang, Martin, 2024): diffusion-based, perceptually uniform morphing. It
  maps the morph factor to constant perceptual steps (log-mel distance) by binary search.
  https://arxiv.org/abs/2410.02144
- MorphFader (2024/25): interpolates cross-attention in a text-to-audio diffusion model.
  https://arxiv.org/abs/2408.07260
- Temporal-envelope morphing (2025). It notes that latent-interpolation morphers fail to produce
  intermediate temporal envelopes. https://arxiv.org/html/2506.01588
- DDSP timbre transfer (Engel et al., 2020) is the neural "cross-synthesis" route.
  https://arxiv.org/abs/2001.04643
- Neural morphers are offline and GPU-heavy, so they are not usable on the audio thread. One idea
  worth borrowing is SoundMorpher's perceptually uniform morph factor: our level matching is a
  first step, and a later step could warp x by a spectral-distance measure.

## 3. What was built

`SpectralMorph`. STFT with Hann/Hann windows and hop = N/4. N = 2048 at 44.1/48 kHz, 4096 at
88.2/96 kHz and 8192 at 176.4/192 kHz, so the frame is always about 43 ms. **Latency = N samples**
(2048 at 48 kHz = 42.7 ms; 46.4 ms at 44.1 kHz). The class allocates nothing after `prepare()`
and has no locks.

| Mode | How it works | y axis (bottom -> top) |
|---|---|---|
| **Blend** (default) | The true envelopes interpolate in log (formants slide). The whitened fine structures combine with a power mean. | **Mix** (p = 2: both pitch structures audible) -> **Fuse** (geometric: one hybrid texture) |
| **Glide** | Audio-transport displacement: peak masses matched by 1-D OT and moved to (1-x)fA + x fB, with phase-locked resynthesis. Pitches glide. | **Move** (formants travel with the partials) -> **Shape** (the in-between envelope is re-imposed, so formants morph in place) |
| **Cross** | Vocoder: x travels A -> hybrid -> B, and the hybrid in the middle depends on y. | **B thru A** (B's fine structure through A's envelope) -> **A thru B**. At y = 0.5 it is the plain diagonal morph. |

Automatic behaviour (no UI needed):
- x, y and amount are smoothed per frame (about 40 ms). A mode change crossfades over 6 frames
  (64 ms), with the outgoing mode level-matched to the incoming one.
- Level matching has two parts. Each frame's spectrum is scaled to the log-interpolated input
  energy. A slow loudness trim (about 300 ms, -3..+4 dB) then compares the emitted output with the
  latency-delayed target and removes the residual overlap-add phase loss (mainly in Glide).
- Silence: each input has a presence gate (-75 to -60 dBFS, 15 ms attack, 120 ms release). The
  effective x is xe = x gB / (x gB + (1-x) gA), so a silent A gives B and vice versa, and when both
  are silent the output is silent. `a` or `b` may be null.
- Amount (automation only) blends the morph with an equal-power crossfade at the same xe.

## 4. Test results (`Tests/morph`, 48 kHz unless noted)

Inputs are at -20 dBFS RMS. Pairs: 220 Hz saw vs band-passed noise, 300 Hz vs 600 Hz sines, and a
formant pulse train (140 Hz, F1-F3) vs a C-E-G saw chord. Every check passes (0 failures). WAVs
are in `/private/tmp/claude-501/-Users-samueljustice--claude-spares-spare-ZpAbzR/dfdcec56-f40c-4196-ada3-668f67b9775a/scratchpad/morph`.

- x = 0 vs A and x = 1 vs B: linear long-term spectral correlation of **0.998-1.000** in every mode
  and on every pair.
- Level at x = 0 / .25 / .5 / .75 / 1: all modes within **0.1 dB** of -20 dBFS on every pair.
  Along a slow 0->1 sweep (200 ms windows), Blend and Cross stay within -20.2..-19.5 dB, and Glide
  within -21.9..-18.8 dB (the trim tracks with about 300 ms lag).
- Glide on 300 -> 600 Hz: at x = 0.5, **100 %** of the 250-650 Hz energy lies between 350 and 550 Hz
  (a single partial near 450 Hz). The sweep's peak track is 328 363 387 410 434 457 492 516 539 562
  586 Hz, a true glide. Blend and Cross give 0 % (305 Hz then 598 Hz), which is the expected
  two-tone behaviour.
- Clicks: x jumps 0 <-> 1 every 80 ms and y jumps every 130 ms. The peak of the second difference
  stays at 0.2-1.2x the static renders' peak (the limit is 2x). Mode switching every 150 ms gives
  1.0x.
- Silence: with A null, the output is B (correlation 1.000, level 0.0 dB), and with B null it is A.
  When A stops mid-render, the output is B afterwards (correlation 0.994-0.996, within 0.3 dB).
- 44.1, 96 and 192 kHz: finite output with the sweep level at -20.1 dB. FFT and latency are
  2048/46.4 ms, 4096/42.7 ms and 8192/42.7 ms.
- CPU (Apple Silicon, arm64 Release, 256-sample blocks, one instance):

| | Blend | Glide | Cross |
|---|---|---|---|
| 48 kHz | 0.78 % | 1.35 % | 0.78 % |
| 192 kHz | 3.45 % | 5.7 % | 3.4 % |

Known limits / next steps:
- 43 ms latency. A low-latency variant (N = 512, envelope-only) could follow the MORPH "(LL)"
  idea.
- Glide on dense noise has a slightly phasey vocoder character. RTPGHI phase is the upgrade path.
- Transients smear a little in every mode, as in any 43 ms STFT. That suits whooshes, not drums.

## 5. Integration (for whoever wires it in)

Parameters (suggested IDs and ranges):
- `morphOn` (bool, default off), `morphX` (0..1, 0.5), `morphY` (0..1, 0.5)
- `morphMode` (choice Blend/Glide/Cross, Blend), `morphAmount` (0..1, 1, automation only, not on the UI)
- `morphWander` (bool, off), `morphSpeed` (0..1, 0.5)
- `morphA` / `morphB` (int lane 0..11, defaults e.g. 0 = Air and 6 = first sample track)
- `morphLevel` (gain of the morph layer, 0..1)

Engine: the morph output is one more mono layer line that flights read like any other source.
- In `WhooshEngine`, keep a `SpectralMorph` member. In `prepare()`, call
  `morph.prepare(fs, c_chunk)` and add a `DelayLine` for the morph layer (or extend the layer
  arrays to `c_numLayers + 1`).
- In `renderSources` / `renderCargo`, after layers A and B have been rendered into
  `m_layerScratch` for the chunk, and before they are pushed into `m_layerLines`, call
  `morph.process(scratch[A], scratch[B], morphScratch, frames)` and push `morphScratch` into the
  morph line. Cargo layers must already be rendered at that point, so run the morph after
  `renderCargo`.
- The flight mix reads the morph line with its own level (`morphLevel`), included in the flight's
  per-layer weights.
- Latency: the morph line is N samples late compared with the raw layers. Either (a) delay the
  other layers by N when the morph is on (simplest, and the engine already has delay lines), or
  (b) read the morph line N samples earlier in the flight doppler read: `delay - morphLatency`,
  clamped >= 1. Option (b) adds no plugin latency but needs `baseDelay >= N / fs` (43 ms). Since
  `m_baseDelay` already exists, prefer (b) and raise baseDelay when the morph is on.
- Optionally ducking A and B while the morph layer is in use (morphLevel) avoids hearing the
  sources twice. Leave that as a user choice.
- `setParams()` once per block, from the audio thread. Smoothing is internal.

UI (`MorphPad`, message thread):
- Construct it with `Theme::Fonts&` and put it in the bottom area, for example next to the
  FlightPad or in the More drawer (about 280 x 200 minimum).
- Call `pad.setRopeOverlayParent(&editor)` once.
- `pad.laneAtScreenPosition = [this](juce::Point<int> s) { return timeline.laneAt(timeline.getLocalPoint(nullptr, s)); }`
  (returns 0..11 or -1).
- `pad.onSourceAssigned = [](int slot, int lane) { /* set morphA / morphB (begin/end gesture) */ };`
- `pad.onXYChanged` sets morphX/morphY. Wrap pad drags in begin/endChangeGesture; during wander,
  either set the values without gestures or don't record automation.
- `pad.onModeChanged` sets morphMode, and `pad.onWanderChanged(on, speed)` sets
  morphWander/morphSpeed.
- From a parameter listener or timer, call `setXY`, `setMode`, `setWander`, and
  `setSources(laneA, laneB, nameA, nameB)` with the lane display names. `setXY` is ignored while
  the user drags or wanders.
- Wander runs in the UI (60 Hz timer), so it only moves while the editor is open. If it must run
  headless, port `stepWander` to the processor; the butterfly maths is self-contained.
