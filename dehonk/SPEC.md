# De-Honk: specification

A dynamic resonance suppressor for the low mids, built to sit after a psychoacoustic bass enhancer (RBass, MaxxBass and similar) and remove the honk it adds without removing the bass it was put there to create. By Samuel Justice.

Status: draft for review. Nothing is built. Numbers marked *(to measure)* are starting points to be settled by the prototype, not decisions.

## 1. The problem

Psychoacoustic bass enhancers work by generating harmonics of a low fundamental. The ear infers the fundamental from the harmonic series, so the bass reads on speakers that cannot reproduce it. For fundamentals of 30 to 80 Hz, harmonics 2 to 8 land between roughly 60 and 640 Hz.

The lowest two or three harmonics do the useful work. The higher ones, and the way they pile up when the source is dense or the enhancer is driven hard, produce a nasal, boxy build-up somewhere between 150 and 500 Hz. In a cinematic or trailer mix this is the range already crowded by impacts, low brass, drums and dialogue body, so the result is mud or an obvious honk.

A static EQ cut fixes the loud moments and hollows out the quiet ones. A multiband compressor reacts to band level, not to the shape of the spectrum, so it ducks wanted body along with the honk. What is needed is what soothe does for harshness: cut only the parts of the spectrum that stick out, only while they stick out, at any input level.

### What De-Honk must not do

- Touch anything below the focus band. The fundamental and sub stay bit-identical in level terms (within 0.5 dB).
- Remove the harmonics that carry the bass illusion. The cut is a reduction of excess, with a ceiling, never a notch to silence.
- Change the sound when there is no honk. With nothing sticking out, gain reduction is zero.

## 2. Where it sits

Two places, mono and stereo:

- **On the master, before the limiter.** This is the main use: one instance cleaning the low mids of the whole mix before it reaches the limiter, where the enhancer was applied on tracks or stems upstream. It sees dialogue, music and effects together, so it has to leave a mix with no honk alone, and it has to be cheap and steady in CPU because it runs for the whole session at the session's sample rate.
- **On a track, bus or stem**, directly after the enhancer, or on a printed stem where the enhancer cannot be undone.

Formats: VST3, AU, AAX and Standalone, macOS and Windows, same build and release setup as the other plug-ins in this repository. Folder `dehonk/`, release tag `dehonk-vX.Y.Z`.

## 3. How it works

### 3.1 Overview

```
in ──┬──────────────────────────────► correction filters ──► mix/trim ──► out
     │                                        ▲
     └─► analysis bank ─► band levels ─► honk detector ─► per-band gains
```

Analysis runs beside the audio path. Correction is a set of minimum-phase bell filters in the audio path, so the plug-in adds no latency unless lookahead is switched on.

### 3.2 Analysis bank

A constant-Q band-pass bank at 1/6-octave spacing from 40 Hz to 2.5 kHz, about 36 bands *(to measure: 1/6 against 1/12 octave)*. Each band feeds an RMS envelope follower whose time constant scales with the band's period, so the bank is as fast as each frequency allows: around 20 ms at 150 Hz, 5 ms at 600 Hz.

The bank covers more than the focus band on purpose. The detector needs to see the bass below and the mids above to judge what the low mids should look like.

A constant-Q bank is preferred over an STFT because an STFT with enough resolution to separate harmonics at 150 Hz needs a window near 85 ms, which is too slow for the front of a trailer hit, where the honk is worst.

### 3.3 Honk detector

Per analysis frame:

1. Convert band levels to dB.
2. Build a reference envelope by smoothing the levels across frequency. The smoothing width is the **Sharpness** control: about two octaves at the broad end, a third of an octave at the narrow end. Smoothing in dB over log frequency leaves an ordinary spectral slope unchanged, so a bass sound that simply falls away with frequency produces no detection.
3. Excess = band level minus reference, in dB. Positive excess is something sticking out.
4. Per-band cut = Depth × soft-knee(excess), limited to a maximum of 18 dB *(to measure)*, weighted by the focus band shape (section 3.5).
5. Smooth the cuts in time with attack and release set by **Speed**.

At broad settings this catches the whole low-mid bump an enhancer produces. At narrow settings it catches individual ringing harmonics and leaves the overall balance alone. Because the detector works on the difference between a band and its neighbours, it does not depend on input level, and there is no threshold to set.

Stereo: the detector takes the louder channel per band by default so the image does not move. **Link** blends towards fully independent channels.

### 3.4 Correction filters

One peaking filter per analysis band inside the focus range, in series, using a state-variable form that stays stable while its gain is modulated. Neighbouring bells overlap, so the requested cuts are passed through a precomputed interaction matrix before being applied, the method used for accurate cascade graphic equalisers (Välimäki and Liski, 2017). Without this step, a broad cut comes out several dB deeper than asked.

Gains are smoothed per sample. At zero cut every filter is exactly flat, so an idle De-Honk is transparent.

Alternative to compare in the prototype: STFT analysis with the gain curve converted to a minimum-phase FIR each hop. Finer frequency detail, more CPU, slower response. The filter bank is the plan unless listening says otherwise.

### 3.5 Protecting the bass

- Nothing below the low edge of the focus band is cut. The low edge has a gentle slope, so cut depth rises gradually above it and the second and third harmonics of a low fundamental are treated more lightly than the fifth and sixth.
- The maximum cut is capped. A harmonic can be pulled back into line with its neighbours, not removed.
- Acceptance test: at default settings, on the test corpus, the two lowest harmonics inside the focus band lose no more than 3 dB on average *(to measure)*, and the processed signal still reads as bass through a 120 Hz high-pass.

### 3.6 Efficiency

The design is shaped by the master-bus use.

- **Analysis at a low sample rate.** Nothing above 2.5 kHz is analysed, so the analysis bank runs on a decimated copy of the signal (about 6 kHz, whatever the session rate). The analysis cost is then the same at 48, 96 and 192 kHz and a small fraction of what it would be at full rate. The decimation filter delays detection by a millisecond or two, not the audio.
- **Detector at control rate.** The envelope smoothing, excess and gain calculation run once every 32 samples of the session rate *(to measure)*. Filter coefficients are updated at that rate and interpolated between updates.
- **Only the correction filters run at full rate.** About 20 bell filters per channel inside the focus band. Left and right are processed together in one SIMD register, so stereo costs little more than mono.
- **Idle bands cost nothing.** A filter whose cut has been zero long enough for its state to settle is skipped. On a clean mix most of the cascade is not running.
- **Flat CPU.** The work per block is the same every block. There is no large periodic job, which is a second reason to prefer the filter bank to an STFT: an FFT every hop shows up as spikes at small buffer sizes.
- **The editor costs nothing when closed.** The audio thread stops writing display frames when no editor is open. With the editor open, the scene is capped at 30 frames a second, drops to a few frames a second when nothing is moving, and stops when the window is hidden or covered. All drawing is on the GPU; the message thread does no painting for the scene.

### 3.7 Latency and lookahead

Zero latency by default. **Lookahead** (off, 5, 10, 20 ms) delays the audio path so the cut is in place when a transient arrives, and reports the delay to the host.

## 4. Controls

| Control | Range | Default | What it does |
|---|---|---|---|
| Depth | 0 to 100% | 50% | How much of the detected excess is removed |
| Sharpness | Broad to Narrow | Middle | Whether it treats the whole low-mid bump or only individual resonances |
| Speed | Slow to Fast | Middle | How quickly cuts engage and let go; attack and release move together |
| Focus Low | 60 to 400 Hz | 120 Hz | Bottom of the band it may cut |
| Focus High | 200 Hz to 1.5 kHz | 600 Hz | Top of the band it may cut |
| Link | 0 to 100% | 100% | Shared or per-channel detection |
| Lookahead | Off, 5, 10, 20 ms | Off | Catches transients at the cost of latency |
| Mix | 0 to 100% | 100% | Dry/wet |
| Trim | ±12 dB | 0 dB | Output level |
| Solo Honk | On/off | Off | Plays only what is being removed |
| Diff | Hold or latch | Off | Plays the untreated signal for comparison, and overlays before and after on the display |
| Bypass | On/off | Off | Click-free, latency-matched |

Depth, Sharpness and Speed are the three main knobs. Focus Low and High are dragged directly on the display. Everything else sits in a small secondary row.

All parameters are automatable and smoothed. Presets: a short factory set ("After RBass", "Printed stem", "Narrow resonances only", "Heavy") plus user save/load, using the same preset manager pattern as Subbertone.

### 4.1 Solo Honk and Diff

Both are listening tools, not part of the sound, and both are large buttons beside the three main knobs.

**Solo Honk** outputs the input minus the treated signal: exactly the honk De-Honk is taking out and nothing else. If what you hear is only ugly, the settings are right. If you hear wanted body or bass in it, back off Depth or raise Focus Low. It follows Mix and Trim, so what you hear is what is leaving the track.

**Diff** switches the output to the untreated signal while held (click to latch). It is matched in latency, so with lookahead on there is no jump in time, and it crossfades over a few milliseconds so the switch itself does not click. While it is on, the display shows the untreated spectrum with the treated one outlined on top and the total difference in dB. It differs from Bypass in that the host's bypass state is not touched and analysis keeps running, so the comparison is instant in both directions.

Neither is stored as on in a preset or recalled as on with a session, and neither is automatable, so a mix cannot be bounced with one left engaged by accident. While either is on, its button pulses and a label on the display says what is being heard.

## 5. Interface

### 5.1 Layout

One OpenGL scene fills most of the window, with the control row beneath it. Resizable, drawn at native resolution.

The scene is a frequency axis drawn as a path along the edge of a pond, log scale, 40 Hz at the left to 2.5 kHz at the right. The input spectrum rises from the path as reeds or grass: tall where the signal is loud. The focus band is a marked stretch of the path with two draggable posts for Focus Low and Focus High.

### 5.2 The goose

A goose patrols the focus band.

- **Idle:** with no honk detected, it wanders, pecks at the ground, preens, occasionally looks at the user.
- **Alert:** when excess appears, its head comes up and it turns towards the offending frequency.
- **Honk:** it walks or runs to that frequency (its position is the centroid of the current gain reduction), stretches its neck and honks. The honk is drawn as rings from the beak, and the reeds under it are flattened by exactly the amount of gain reduction in each band. The flattened reeds are the gain-reduction meter, so the joke is also the readout.
- **Scale:** neck stretch, wing spread and ring size follow total gain reduction. A little honk gets a mutter. A lot gets wings out and a charge.
- **Depth as temperament:** low Depth is a placid goose, high Depth is a furious one.
- **Solo Honk:** the goose faces the user and the removed signal comes out of its beak.
- **Diff:** the goose stops and steps aside, and the reeds stand back up to their untreated height with the treated outline drawn over them.
- **Bypass:** the goose sits down and goes to sleep.

The goose makes no sound. Nothing it does is added to the audio.

### 5.3 Keeping it usable

- The spectrum, the gain reduction per band and the focus band edges must be readable at a glance with the goose ignored. A numeric total gain-reduction readout sits in the corner.
- A **Calm** switch stops the walking and honking and leaves the goose standing still at the side, for long sessions and for anyone who finds it distracting. The meters carry on.
- Animation is capped at 30 frames a second, slows when nothing is moving, and stops when the editor is closed or hidden (section 3.6).

### 5.4 Rendering

`juce::OpenGLContext` on the scene component, the same approach as Subbertone's waveform visualiser. The goose is built from separate parts (body, neck segments, head, beak, wings, legs, feet) positioned procedurally each frame, so the walk cycle, neck stretch and wing spread are driven by continuous values from the audio, not picked from fixed animation frames. Reeds and honk rings are instanced quads with a small fragment shader each.

The audio thread writes band levels and band cuts into a lock-free FIFO about 30 times a second, and only while an editor is open. The UI thread reads the latest frame and eases towards it. No locks, no allocation on the audio thread.

If an OpenGL context cannot be created, the scene falls back to a software-painted spectrum and gain-reduction display with a still goose.

The goose is drawn entirely in shaders as flat shapes, with no textures. It scales to any window size and needs no art pipeline.

## 6. Performance targets

- Under 1% of one core for a stereo instance at 48 kHz on an M1, and under 2% at 96 kHz *(to measure)*, measured with the editor closed.
- No block takes more than twice the average block time, at buffer sizes from 32 to 2048 samples.
- With the editor open, the scene uses under 5% of one core and no measurable GPU load on an integrated GPU *(to measure)*.
- Denormals are flushed; silence costs no more than signal.
- 44.1 to 192 kHz. Band centres are fixed in Hz, coefficients recomputed on sample-rate change.
- No allocation, locks or system calls in `processBlock`.
- Automation of any control produces no clicks or zipper noise.

## 7. Testing

Offline render harness and unit tests under `dehonk/Tests`, as Swooshbuckler has.

- **Corpus:** 15 to 20 pairs of the same material with and without RBass, at the settings used in real sessions, including at least five full mixes captured before the limiter with RBass on and off upstream, and a few clean mixes that should come through untouched: sub drops, booms, impacts, low brass, synth bass, a full effects stem. These come from real sessions and are needed before any DSP is tuned. The first job is to measure what RBass adds in each pair, which settles the default focus band and the numbers marked *(to measure)*.
- **Transparency:** Depth 0 nulls against the input. The clean full mixes show less than 1 dB of gain reduction at defaults. Pink noise and a clean sine sweep produce less than 0.5 dB of gain reduction at defaults.
- **Bass preservation:** level below Focus Low changes by less than 0.5 dB on every corpus file. Harmonic test from section 3.5.
- **Effect:** on the enhanced files, excess energy in the focus band (measured against the unenhanced file's spectral shape) drops by at least half at default settings.
- **Accuracy:** a requested static cut curve is realised within 1 dB by the filter cascade.
- **CPU:** a benchmark in the test harness reports average and worst block time at 48 and 96 kHz and buffer sizes of 32, 128 and 1024, and fails if the targets in section 6 are missed.
- **Stability:** fast random automation of every parameter, silence, DC, full-scale square wave, denormals, sample-rate and block-size changes, 1-sample blocks.
- **Hosts:** Pro Tools (AAX, mono and stereo), Logic, Reaper, Nuendo. Latency reporting checked with lookahead on and off.

## 8. Build order

1. Collect the RBass corpus and measure what it adds.
2. Offline prototype of detector and correction against the corpus. Settle band spacing, smoothing widths, maximum cut, time constants. Compare the filter bank with the STFT alternative by ear.
3. Plug-in with full DSP and a plain interface (spectrum, gain-reduction curve, knobs). Usable for real work at this point.
4. The pond and the goose.
5. Presets, README, changelog, release notes, landing-page section, installers, AAX signing.

### 8.1 Who builds what

The work is split between two models to keep the cost down. Opus 5.5 does the bulk of the code. Fable 5.2 does the parts where a wrong answer is expensive to find later: the signal-processing maths, the tuning decisions and the review of everything Opus writes.

| Stage | Opus 5.5 | Fable 5.2 |
|---|---|---|
| 1. Corpus and measurement | Analysis script: load each RBass pair, align, write spectra and band differences to CSV and plots | Reads the results, sets the default focus band and the first values for the *(to measure)* numbers |
| 2. Offline prototype | Render harness: file in, file out, metrics from section 7, CPU benchmark, batch over the corpus | Writes the analysis bank, honk detector, interaction matrix and filter cascade. Tunes them against the corpus. Decides filter bank or STFT |
| 3. Plug-in shell | Folder, CMake, parameters, state save and load, preset manager, bypass, latency reporting, installer, entitlements, CI, following Subbertone's layout | Reviews the diff |
| 3. DSP in the plug-in | Wires the prototype classes into `processBlock`, smoothing, Mix, Trim, Link, lookahead delay, Solo Honk and Diff routing, the FIFO to the editor | Ports the DSP core from the prototype. Reviews the wiring for real-time safety. Runs the transparency, accuracy and stability tests |
| 3. Controls | Knobs, buttons, layout, look and feel, tooltips | Reviews screenshots from the README screenshot harness |
| 3. Display | Nothing | Writes the OpenGL display: spectrum, gain-reduction curve, draggable focus posts, Diff overlay, frame pacing, software fallback |
| 4. Pond and goose | Nothing | Writes all of it on top of the stage 3 display: reed and ring shaders, goose parts, walk cycle, behaviour, Calm switch. Measures frame time and CPU |
| 5. Release | Factory presets file, README, changelog, release notes, landing-page section | Sets preset values by measurement. Final read of the docs. Dry-run of the release workflow |

Rules that keep it cheap:

- Every Opus job gets a written brief naming the files it may touch, the existing file to copy the pattern from, and the check that says it is done (a test passing, a build succeeding, a screenshot). A vague brief costs a second run.
- Fable reviews diffs and test output, not whole files, and sends problems back to the same Opus agent with its context intact instead of fixing bulk code itself.
- One Opus agent at a time per set of files. The shell and the controls can run side by side because they touch different files, and Fable's display work is in its own files, so it can go on at the same time.
- The DSP core is written once, in the prototype, as plain C++ classes with no JUCE dependency, and the plug-in uses the same files. Nothing is written twice.
- Tests are written by Opus from section 7 before the code they check, so Fable's review is mostly reading results.
- Documentation gets a single read by Fable, with no separate review pass.

The visualiser is Fable's because it is the part Opus does worst, and because its frame pacing is part of the efficiency target. By volume of code this puts roughly two thirds of the writing on Opus. Fable's share is the DSP core, the display and goose, the tuning, and review.

## 9. Possible later additions

- **Surround:** up to 7.1.4, with linked detection across channels and the LFE passed through untouched.
- **Other enhancers:** widen the corpus beyond RBass (MaxxBass and similar) and check the defaults against it.
- **Reference input:** feed the signal from before the enhancer into the sidechain so De-Honk cuts only what the enhancer added. More exact than guessing from the spectrum, but needs the two paths time-aligned.
- **Pitch following:** track the fundamental and target its upper harmonics directly.
- Mid/side detection and processing.
- Linear-phase correction mode for parallel routing.

## 10. Decisions

- Mono and stereo in the first release. Surround comes later.
- Defaults and factory presets are tuned to RBass, the common case. Other enhancers are checked later.
- The goose is drawn in flat shader shapes.
