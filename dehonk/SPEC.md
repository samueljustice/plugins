# De-Honk: specification

A dynamic resonance suppressor for the low mids. It sits on the master bus before the limiter and removes the honk that bass enhancers, saturators and other harmonic generators leave in a mix, without removing the bass or the body. By Samuel Justice.

Status: draft for review. Nothing is built. Numbers marked *(to measure)* are starting points to be settled by the prototype, not decisions. Section 11 records what has been measured so far.

## 1. The problem

By the time a mix reaches the master, its music and effects have been through many harmonic generators: psychoacoustic bass enhancers (RBass, MaxxBass), sub synthesisers, saturation, tape and tube models, exciters, clipping. Each adds harmonics above a low fundamental. The psychoacoustic enhancers do it on purpose: the ear infers the fundamental from the harmonic series, so the bass reads on speakers that cannot reproduce it.

Summed across a mix, those added harmonics pile up between roughly 120 and 500 Hz as a nasal, boxy build-up. This is the range already crowded by impacts, low brass, drums and dialogue body, so the result is mud or an obvious honk. RBass is the generator we use most and the one measured first (section 11), but De-Honk cannot assume which generator made the honk, what the fundamentals were, or that there was only one. It has no access to the signal from before the generator. It can only judge the spectrum in front of it.

A static EQ cut fixes the loud moments and hollows out the quiet ones. A multiband compressor reacts to band level, not to the shape of the spectrum, so it ducks wanted body along with the honk. What is needed is what soothe does for harshness: cut only the parts of the spectrum that stick out, only while they stick out, at any input level.

### What De-Honk must not do

- Touch anything below the focus band. The fundamental and sub stay bit-identical in level terms (within 0.5 dB).
- Remove the harmonics that carry the bass illusion. The cut is a reduction of excess, with a ceiling, never a notch to silence.
- Thin out the wanted body of music and effects. Low-mid content that is in balance with what surrounds it is left alone, whatever made it.
- Audibly change dialogue sitting in a mix. De-Honk is for music and effects composites; it is not a dialogue tool and is not tuned on dialogue alone. Trailer mixes have dialogue in them, though, so it is judged by listening on those mixes.
- Change the sound when there is no honk. With nothing sticking out, gain reduction is zero.

## 2. Where it sits

Two places, mono and stereo:

- **On the master, before the limiter.** This is the main use: one instance cleaning the low mids of the whole mix before it reaches the limiter, where enhancers and other harmonic generators were applied on tracks and stems upstream. It sees dialogue, music and effects together, so it has to leave a mix with no honk alone, and it has to be cheap and steady in CPU because it runs for the whole session at the session's sample rate.
- **On a track, bus or stem**, directly after a generator, or on a printed stem where it cannot be undone.

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

A constant-Q band-pass bank at 1/6-octave spacing from 40 Hz to 2.56 kHz: 37 bands. The RBass measurement gave no reason to go to 1/12 octave (section 11); the mix corpus may. Each band is two cascaded second-order sections (12 dB per octave skirts, so a loud fundamental does not leak into the bands of its harmonics) followed by a mean-square follower whose time constant is two periods of the band's centre, limited to between 4 and 50 ms.

The bank covers more than the focus band on purpose. The detector needs to see the bass below and the mids above to judge what the low mids should look like.

A constant-Q bank is preferred over an STFT because an STFT with enough resolution to separate harmonics at 150 Hz needs a window near 85 ms, which is too slow for the front of a trailer hit, where the honk is worst.

### 3.3 Honk detector

Every four analysis samples (about 2000 times a second):

1. **Reference.** For each band, the reference is the average power of its surroundings: a ring around the band that leaves out the band's own neighbourhood. The width of the part left out is the **Sharpness** control, from two octaves at the broad end to a third of an octave at the narrow end *(to measure)*; the ring reaches 2.5 times as far. A feature up to that width stands clear of its reference. A plain local average does not work for this: a honk an octave wide raises its own average and all but disappears.
2. **The bass is left out of the reference.** Bands below Focus Low do not count. The bass is always louder than the low mids, and if it were included it would hide every harmonic above it, which is the case De-Honk exists for. The measured RBass harmonics sit 15 to 20 dB under their fundamental.
3. **Excess** = band level minus reference, in dB. Positive excess is something sticking out.
4. **Parent rule.** A band is only eligible if something at least comparable sits at a subharmonic below it (an octave, a twelfth, two octaves, or the fifth-harmonic interval down). Added harmonics always have such a parent; the fundamental of an instrument playing inside the focus band does not, and is left alone. Eligibility fades in as the parent goes from 6 dB below the band to 6 dB above it *(to measure)*. This is a heuristic added during the build and needs the mix corpus to confirm it.
5. **Cut** = Depth × 3.2 × (excess beyond a 3 dB dead zone, with a 3 dB soft knee) × eligibility × focus weight (section 3.5), limited to 18 dB *(all to measure)*. At 50% Depth 1.6 times the excess beyond the dead zone is removed.
6. **Threshold.** Honk belongs to the busy moments: full explosions and music together (section 11.3). The cut is scaled by how far the programme level is above the **Threshold** control, from nothing at the threshold to full effect 6 dB above it. The level is the RMS of the signal below 2.8 kHz on the louder channel, followed with a 30 ms attack and 400 ms release. This is the one part of the detector that depends on absolute level, which is why it is a control.
7. **Smoothing in time** with attack and release set by **Speed**: attack from 80 ms (slow) to 5 ms (fast), release from 600 ms to 60 ms *(to measure)*.

At broad settings this catches the whole low-mid bump that stacked generators produce. At narrow settings it catches individual ringing harmonics and leaves the overall balance alone. Because the detector works on the difference between a band and its surroundings, it does not depend on input level, and its sensitivity does not need setting. Nothing is detected when the loudest band is below -90 dBFS.

An optional step that removes the overall tilt of the spectrum before comparing exists in the code and is off. A least-squares fit absorbs about half of a bump at the low end of the range, so it needs a more robust fit before it can be used.

What the detector does today on synthetic signals (not on mixes): pink noise gets 0.2 dB of average cut; a 10 dB, octave-wide bump at 250 Hz on a bass-heavy noise bed gets about 1 dB at default settings and 3 dB with Sharpness fully broad; RBass-like harmonics of a 50 or 80 Hz tone get 5 to 6 dB; a lone 160 Hz note keeps its fundamental untouched with the parent rule and loses 6 dB of it without. A bass-heavy noise bed with no bump still gets about 1 dB between 160 and 200 Hz, because its spectrum falls steeply there. Whether that last behaviour is wanted is a question for the mix corpus.

Stereo: the detector takes the louder channel per band by default so the image does not move. **Link** blends towards fully independent channels.

### 3.4 Correction filters

One peaking filter per band from 57 Hz to 2.3 kHz (33 of them), in series, in a state-variable form that stays stable while its gain is modulated. Only the ones inside the focus band ever do anything.

Neighbouring bells overlap, so asking each for the wanted cut would give too deep a result. The wanted cuts are passed through a precomputed matrix that undoes the overlap, the method used for accurate cascade graphic equalisers (Välimäki and Liski, 2017). Each bell's width follows its gain so that its response at the next band's centre is always the same fraction of its gain, which is what makes one fixed matrix valid. For cuts deeper than 4 dB one correction step against the true response is added.

Measured: a single-band 6 dB cut, a 6 dB plateau and a 12 dB cosine dip are realised within 0.3 dB. A step of 15 dB between adjacent bands cannot be realised by filters this wide and comes out about 2 dB off at the edge.

Coefficients are updated at the detector rate and ramped per sample in between. At zero cut every filter is exactly flat, so an idle De-Honk is transparent.

Alternative to compare later: STFT analysis with the gain curve converted to a minimum-phase FIR each hop. Finer frequency detail, more CPU, slower response. The filter bank is the plan unless listening says otherwise.

### 3.5 Protecting the bass

The RBass measurement changed this section. The first draft assumed the second and third harmonics were the useful ones and the higher harmonics were the honk. RBass does not work like that: at normal settings nearly all of what it adds is the second and third harmonic, and everything from the fourth up is 20 dB or more below them (section 11). The honk is the second and third harmonics of the upper bass (fundamentals of 60 to 100 Hz, landing at 120 to 300 Hz). The same two harmonics of a 30 to 50 Hz fundamental land at 60 to 150 Hz and are the ones worth keeping. So protection is by frequency, not by harmonic number.

- Nothing below the low edge of the focus band is cut. With the default of 120 Hz, the second harmonic of anything up to 60 Hz and the third harmonic of anything up to 40 Hz are untouched.
- The low edge has a gentle slope, about an octave wide *(to measure)*, so cut depth rises gradually above it.
- The maximum cut is capped. A harmonic can be pulled back into line with its neighbours, not removed.
- Acceptance test: at default settings, level below Focus Low changes by less than 0.5 dB, and the processed signal still reads as bass through a 120 Hz high-pass.

### 3.6 Efficiency

The design is shaped by the master-bus use.

- **Analysis at a low sample rate.** Nothing above 2.5 kHz is analysed, so the analysis bank runs on a decimated copy of the signal (about 8 kHz, whatever the session rate), behind an eighth-order low-pass. The analysis cost is then the same at 48, 96 and 192 kHz and a small fraction of what it would be at full rate. The decimation filter delays detection by a millisecond or two, not the audio.
- **Detector at control rate.** The excess and gain calculation run once every four analysis samples (every 24 samples at 48 kHz). Filter coefficients are updated at that rate and ramped between updates. When the channels are linked the gains are worked out once for both.
- **Only the delay, the low-pass and the correction filters run at full rate.** Processing left and right together in one SIMD register is planned and not done yet.
- **Idle bands cost nothing.** A filter whose cut has been zero long enough for its state to settle is skipped. On a clean mix most of the cascade is not running.
- **Flat CPU.** The work per block is the same every block. There is no large periodic job, which is a second reason to prefer the filter bank to an STFT: an FFT every hop shows up as spikes at small buffer sizes.
- **The editor costs nothing when closed.** The audio thread stops writing display frames when no editor is open. With the editor open, the scene is capped at 30 frames a second, drops to a few frames a second when nothing is moving, and stops when the window is hidden or covered. All drawing is on the GPU; the message thread does no painting for the scene.

### 3.7 Latency and lookahead

Zero latency by default. **Lookahead** (off, 5, 10, 20 ms) delays the audio path so the cut is in place when a transient arrives, and reports the delay to the host.

## 4. Controls

| Control | Range | Default | What it does |
|---|---|---|---|
| Threshold | -60 to 0 dBFS | -16 dBFS | De-Honk acts only while the mix is louder than this. Fully off below it, fully on 6 dB above. At -60 it always acts |
| Depth | 0 to 100% | 50% | How much of the detected excess is removed |
| Sharpness | Broad to Narrow | 20% (towards broad) | Whether it treats the whole low-mid bump or only individual resonances |
| Speed | Slow to Fast | Middle | How quickly a cut goes in (40 ms at Slow to 5 ms at Fast). Until 17 November 2026 it set the release as well; see 13.16 |
| Release | 50 ms to 1.5 s | 400 ms | How slowly a cut lets go once its build-up has gone (13.16) |
| Max Cut | 3 to 12 dB | 6 dB | The deepest any one cut may go, whatever Depth is (13.16) |
| Impact Guard | Off to 100% | 100% | How much of the cut is lifted while a broadband impact sounds (13.16) |
| Focus Low | 60 to 400 Hz | 120 Hz | Bottom of the band it may cut |
| Focus High | 200 Hz to 1.5 kHz | 300 Hz | Top of the band it may cut |
| Link | 0 to 100% | 100% | Shared or per-channel detection |
| Lookahead | Off, 5, 10, 20 ms | Off | Catches transients at the cost of latency |
| Mix | 0 to 100% | 100% | Dry/wet |
| Trim | ±12 dB | 0 dB | Output level |
| Solo Honk | On/off | Off | Plays only what is being removed |
| Diff | Hold or latch | Off | Plays the untreated signal for comparison, and overlays before and after on the display |
| Bypass | On/off | Off | Click-free, latency-matched |

Threshold, Depth, Sharpness and Speed are the main knobs. The display shows the mix level against the threshold so it can be set by eye. Focus Low and High are dragged directly on the display. Everything else sits in a small secondary row.

All parameters are automatable and smoothed. Presets: a short factory set ("Master bus", "After RBass", "Printed stem", "Narrow resonances only", "Heavy") plus user save/load, using the same preset manager pattern as Subbertone.

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

The audio thread writes band levels and band cuts into a lock-free ring 60 times a second, and only while an editor is open. Each slot is a sequence lock (a stamp before and after, the frame kept as atomic words), so any number of windows read it, each from its own place, and each sees every frame; a reader that falls a ring's length behind skips the oldest. The UI thread reads the latest frames and eases towards them. No locks, no allocation on the audio thread.

If an OpenGL context cannot be created, the scene falls back to a software-painted spectrum and gain-reduction display with a still goose.

The goose is drawn entirely in shaders as flat shapes, with no textures. It scales to any window size and needs no art pipeline.

## 6. Performance targets

- Under 1% of one core for a stereo instance at 48 kHz on an M1, and under 2% at 96 kHz *(to measure)*, measured with the editor closed. First measurement of the core alone on the development Mac: 0.75% at 48 kHz and 1.3% at 96 kHz.
- No block takes more than twice the average block time, at buffer sizes from 32 to 2048 samples.
- With the editor open, the scene uses under 5% of one core and no measurable GPU load on an integrated GPU *(to measure)*.
- Denormals are flushed; silence costs no more than signal.
- 44.1 to 192 kHz. Band centres are fixed in Hz, coefficients recomputed on sample-rate change.
- No allocation, locks or system calls in `processBlock`.
- Automation of any control produces no clicks or zipper noise.

## 7. Testing

Offline render harness and unit tests under `dehonk/Tests`, as Swooshbuckler has.

- **Corpus:** in three parts. (1) Full mixes captured at the master before the limiter, from real sessions: at least ten that have honk, with the honky passages marked by time, and at least five clean ones that should come through untouched. This is the main corpus and it is what the detector is tuned on. Historic mixes are only available after the limiter, and those are accepted: the spectral balance that makes a mix honky survives limiting, and the detector does not depend on level, so they are good for tuning what it detects and how deep it cuts. They are less good for tuning Speed, because the limiter has already flattened the dynamics, so a few captures from before the limiter, taken from current sessions as they come up, are still wanted for that. (2) Music and effects stems on their own, where available. (3) Single sources with and without a generator, for RBass and at least two others in regular use (a saturator and one more enhancer). All of it is session material and stays out of the repository.
- **Transparency:** Depth 0 nulls against the input. The clean full mixes show less than 1 dB of gain reduction at defaults. Pink noise and a clean sine sweep produce less than 0.5 dB of gain reduction at defaults.
- **Bass preservation:** level below Focus Low changes by less than 0.5 dB on every corpus file. Listening check from section 3.5.
- **Effect:** in the marked honky passages of the mix corpus, gain reduction is at least 3 dB somewhere in the focus band, and outside those passages it stays under 1 dB *(to measure)*. On the generator pairs, excess energy in the focus band (measured against the untreated file's spectral shape) drops by at least half. The final judge is listening, with Diff and Solo Honk, by the people who mix with it.
- **Accuracy:** a requested static cut curve is realised within 1 dB by the filter cascade.
- **CPU:** a benchmark in the test harness reports average and worst block time at 48 and 96 kHz and buffer sizes of 32, 128 and 1024, and fails if the targets in section 6 are missed.
- **Stability:** fast random automation of every parameter, silence, DC, full-scale square wave, denormals, sample-rate and block-size changes, 1-sample blocks.
- **Hosts:** Pro Tools (AAX, mono and stereo), Logic, Reaper, Nuendo. Latency reporting checked with lookahead on and off.

## 8. Build order

1. Measure RBass on test tones (done, section 11). Collect the mix corpus and mark the honky passages. Measure the other generators.
2. Offline prototype of detector and correction against the corpus. Settle band spacing, smoothing widths, maximum cut, time constants. Compare the filter bank with the STFT alternative by ear.
3. Plug-in with full DSP and a plain interface (spectrum, gain-reduction curve, knobs). Usable for real work at this point.
4. The pond and the goose.
5. Presets, README, changelog, release notes, landing-page section, installers, AAX signing.

### 8.1 Who builds what

The work is split between two models to keep the cost down. Opus 5.5 does the bulk of the code. Fable 5.2 does the parts where a wrong answer is expensive to find later: the signal-processing maths, the tuning decisions and the review of everything Opus writes.

| Stage | Opus 5.5 | Fable 5.2 |
|---|---|---|
| 1. Corpus and measurement | Measurement script for generators (written, pull request 100). Analysis script for the mix corpus: band levels over time, with the marked passages against the rest | Reads the results, sets the default focus band and the first values for the *(to measure)* numbers |
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

- **Surround:** up to 7.1.4, with linked detection across channels and the LFE passed through untouched. Confirmed as needed for the studio's mixes, after stereo works. The core is to be kept general in its channel count so this is not a rewrite. **Done in the core and the plug-in, not yet tried in a host:** every bed from LCR to 7.1.4 (input and output the same). The LFE is delayed with the rest and otherwise untouched: never analysed or filtered, silent in Solo Honk, as it is in Diff. Link works across every other channel as it does across two, so at 100% they all get the same notches. The Threshold reads the louder side of a stereo fold-down (L and R at 1, the centre and everything to one side at -3 dB, the LFE left out), so -16 dBFS means the same on a surround master as on its stereo version. Mono and stereo are bit-identical to before. Snap listens to the channels folded to one. 7.1.4 costs about 1% of one core at 48 kHz with the defaults.
- **Reference input:** feed the signal from before the generator into the sidechain so De-Honk cuts only what was added. Only possible on a single track, not on a master.
- **Pitch following:** track the fundamental and target its harmonics directly. Only meaningful on a single pitched source, not on a master.
- Mid/side detection and processing.
- Linear-phase correction mode for parallel routing.

## 10. Decisions

- Mono and stereo in the first release. Surround comes later.
- The target is honk on a full mix at the master, whatever generator made it. RBass was measured first because it is the most used, and is one calibration case among several.
- Default focus band is 120 to 300 Hz. It was 120 to 600 Hz; on the mixes that reached above the honk and took detail out, and the measured build-up stops at about 250 to 285 Hz.
- There is a Threshold control, with its default taken from the loudness of the reference mixes.
- The goose is drawn in flat shader shapes.
- It is a light-touch tool. The mixes it is used on are close to signed-off quality, with a little honk, so it should act only where there is honk. Where it does act it has to be audible: a change under about 3 dB across a full mix cannot be judged by ear, so the working range is 3 to 6 dB of cut on the offending bands, and the defaults are to be set to land there.

## 11. Measurements

### 11.1 RBass on test tones (16 November 2026)

Waves RBass 17.1.42 (WaveShell1-VST3 17.1), mono, 48 kHz, hosted in pedalboard 0.9.26 on macOS 27.0.1. Sines at 30, 40, 50, 60, 80 and 100 Hz and a 20 to 200 Hz sweep, each at -18 and -6 dBFS, over six Frequency settings (32 to 256 Hz), five Intensity settings (-24 to +24 dB) and Original Bass in and out: 854 renders. Script and step 1 output are in pull request 100.

What it showed:

- **It adds mostly the second and third harmonic.** With Intensity at 0 dB and the fundamental at or below the Frequency setting, the second harmonic sits 15 to 20 dB below the input and the third 18 to 26 dB below. The fourth and fifth are near -40 dB, the rest below -50 dB.
- **Odd harmonics outlast even ones.** From the fourth upward, the fifth, seventh and ninth are each 10 dB or more stronger than the even harmonic above them.
- **Intensity raises the upper harmonics faster than the lower.** At +24 dB the second and third reach roughly the level of the input, and the fifth to ninth come up to -11 to -30 dB.
- **The Frequency control decides which fundamentals are treated.** A tone well above it is nearly untouched: 100 Hz with Frequency at 32 Hz gets a second harmonic at -58 dB.
- **It raises the fundamental too.** With Original Bass in, the fundamental rises by up to 10 dB at 0 dB Intensity and 17 dB at +24 dB.
- **Where the energy lands.** With Frequency at 111 Hz or lower, energy added between 250 and 500 Hz is at least 25 dB below the bass, and between 500 Hz and 1 kHz at least 32 dB below, even at full Intensity. What RBass adds is concentrated between 100 and 300 Hz.
- **Hot input changes the character.** RBass does not exceed 0 dBFS. At -6 dBFS input with high Intensity the output sits at that ceiling and the upper harmonics flatten out (fourth to tenth all within about 8 dB of each other in the worst case), which is the signature of clipping. No render was over 0 dBFS.
- **No latency**, reported or measured.
- **Its least-effect setting is not transparent.** At -24 dB Intensity on pink noise it still adds 0.7 dB between 100 and 150 Hz.

What it did not cover: no real material was rendered (the corpus folder was empty), the stereo component was not rendered, only 48 kHz was used, and no session settings were supplied. Tones show which harmonics RBass makes. They say nothing about how much cut sounds right, how fast it should move, or how honk looks inside a full mix, so the numbers that depend on those are still *(to measure)*.

What it settled:

- Focus Low default of 120 Hz is supported.
- Protection is by frequency, not harmonic number (section 3.5).
- The reference envelope cannot be an average of dB (section 3.3).
- 1/6-octave bands separate the harmonics of a 40 Hz fundamental up to about 300 Hz and merge them above that. That is acceptable: above 300 Hz the detector sees a bump, which the broad end of Sharpness is for. 1/12 octave is not needed on this evidence.

### 11.2 First pass over real mixes (16 November 2026)

Three finished trailer and cinematic mixes, post-limiter, 48 kHz stereo, two to three minutes each, described as having a little honk. Run through the core at default settings, before any tuning.

- The cut is thin and diffuse: 0.3 to 0.7 dB on average in every band from 140 to 640 Hz, with the largest cut above 1 dB for about 80% of the time and above 3 dB for 15 to 20%. No band or passage is singled out. This is close to what the detector does on dense noise, which suggests it is reacting to ordinary moment-to-moment variation as much as to honk.
- The long-term spectra of all three are smooth: falling from 70 to 160 Hz, then nearly level from 200 Hz to 1 kHz. There is no standing low-mid bump to find. Whatever honk there is comes and goes.
- With Sharpness fully broad and Depth at 80% the largest cut averages about 5 dB and is above 3 dB for two thirds of the time.

Nothing here says whether what is removed is the honk. That needs ears: treated and removed-only renders at both settings were made for listening.

### 11.3 Busy moments against the rest (16 November 2026)

Four mixes now (a fourth was added). Honk belongs to the busy moments: full explosions and music together. For each mix the loudest 15% of the running time was compared with the middle 30%, each measured against its own 600 Hz to 2 kHz level.

- In the busy moments the bass (57 to 113 Hz) is 4 to 7 dB higher against the mids in three of the four mixes.
- In two of them the rise carries on into the low mids: 2.5 to 4 dB higher from 113 to 254 Hz in one, 1 to 2.5 dB higher from 113 to 285 Hz in another. A third shows it only up to about 143 Hz. The fourth shows no difference at all between busy and typical passages.
- So where it shows, the honk is not a bump. It is the bass shelf spreading upward to about 250 Hz when the mix gets loud, which is what second and third harmonics of 60 to 100 Hz content would do.
- The detector as built does not tell busy from quiet. With Sharpness broad and Depth at 80%, its cut in the busy moments (2 to 3.5 dB from 160 to 226 Hz) is about the same as in the rest, and in one mix it is smaller.

To do: make the detector respond to how busy the mix is. Two candidates, both independent of absolute level: weight the cut by short-term energy against the mix's own running average, and compare each low-mid band with its own slow average against the mids. Target: 3 to 6 dB between about 140 and 260 Hz in the busy moments, and close to nothing elsewhere.

### 11.4 Threshold and narrower focus (16 November 2026)

After listening to the first renders: the direction was right, but the cut reached too high and took detail out, and the detector should act mainly in the busy moments.

Changes: Focus High default 600 to 300 Hz, with a steeper top edge; a Threshold on programme level; Sharpness default 20%; cut slope 2 to 3.2.

The four mixes sit at a median programme level of -8 to -15 dBFS and reach -5 to -9.5 dBFS in their loudest 15%. A default threshold of -16 dBFS (full effect from -10) puts the busy moments fully in and the quieter half mostly out. These are post-limiter levels; before the limiter the same mixes will sit differently, so the threshold is expected to be adjusted per mix.

At the new defaults, on the four mixes:

| | Cut, 160 to 226 Hz, busy | Largest cut, busy (mean / 90th percentile) | Largest cut, quieter half (mean) |
|---|---|---|---|
| Mix 1 | 2.2 to 2.5 dB | 3.8 / 7.0 dB | 0.1 dB |
| Mix 2 | 1.8 to 2.5 dB | 3.2 / 5.4 dB | 1.4 dB |
| Mix 3 | 2.0 to 2.5 dB | 4.5 / 8.2 dB | 0.2 dB |
| Mix 4 | 1.5 to 1.9 dB | 3.2 / 5.5 dB | 1.4 dB |

Nothing is cut above 360 Hz. Mixes 2 and 4 are loud nearly throughout, so their quieter half is still partly above the threshold. Core CPU at these settings measured 1.1% at 48 kHz and 1.7% at 96 kHz while other builds were running on the machine; that is over the 1% target at 48 kHz and needs measuring again on a quiet machine.

### 11.5 Feedback on the second renders, and a reference plug-in (16 November 2026)

Feedback: the frequency range could be wider but must stay adjustable, with 120 to 300 Hz as the default. What is removed still sounds more like noise than honk. A second listener, on the BFV mix: the original almost distorts in places and the treatment takes that harshness away nicely.

RBass was also applied to the four mixes themselves (Frequency 80 Hz, Intensity +6 dB, mixes lowered 12 dB first) to make known-honk versions. In the busy moments it added 15 dB at 80 to 100 Hz, 7.5 dB at 160 Hz, 4.5 dB at 200 Hz and 2 dB at 254 Hz, which is the same upward-spreading shelf seen in section 11.3. That setting drove RBass to its 0 dBFS ceiling, so it is too hot to use as a tuning pair; a gentler one is needed.

W.A. Production Detroubler 's behaviour was measured as a reference, as a black box with test signals, in the same way RBass was. It is a product the studio owns. Nothing of it is copied; these are observations of what it does.

- It is six fixed, narrow, dynamic notches, at about 80 Hz, 151 Hz, 247 Hz, 1.48 kHz, 4.06 kHz and 7.0 kHz. Each can be switched on or off. One knob sets the depth of all of them.
- The notches are narrow: 0.13 to 0.25 octave wide at half depth (Q about 5 to 12).
- Depth follows level. Nothing happens on pink noise below about -30 dBFS, and it reaches 6 to 8 dB at the top of the knob on loud material. The knob does nothing below half way.
- Each notch is driven by the level in a region about an octave wide around it, not only by its own narrow band, and not by energy far away. It does not look for resonances: a resonance between two of its notches is not treated.
- It responds within about 50 ms and lets go within about 100 ms. No latency.
- It adds a little broadband gain to make up (0.4 to 0.9 dB at full).
- On the four mixes with only its three low areas on at 80%: about 6 dB at 80 Hz, 2.5 to 3 dB at 150 Hz and 3.5 to 4.5 dB at 250 Hz in the busy moments, less in the quiet ones.

What this says about De-Honk as built: it removes a slab one to one and a half octaves wide. A slab of a mix is a bit of everything, and that is why what it removes sounds like noise. Detroubler removes slices about an eighth of an octave wide, which leave the detail either side and sound like a ring when soloed. The width of the cut is the main difference, not the detection.

## 12. Change of direction: modules (proposed, not built)

Requested on 16 November 2026: several problem areas, each a module that can be switched on or off, and the goose eats things from the pond to represent them.

### 12.1 Modules

Six areas, each its own module with an on/off switch and its own activity meter. The names are ordinary mixing terms.

| Module | Range | What it is |
|---|---|---|
| Boom | 60 to 110 Hz | Bass that overpowers |
| Box | 110 to 200 Hz | Hollow, small-room low end |
| Mud | 200 to 400 Hz | Low mids running into each other |
| Honk | 400 Hz to 1.5 kHz | Nasal midrange |
| Click | around 2.5 kHz | Sharp pops and clicks. Replaces the Harsh and Sizzle that were first proposed here; see section 12.8 |

Box and Mud together are what this plug-in has been built around so far, and they stay on by default with the others off. The range of each module is adjustable, which replaces the single Focus Low and Focus High.

### 12.2 What each module does

Where De-Honk differs from a set of fixed notches: within its range, each module finds where the energy is actually piling up at that moment and puts its cut there. The detector already built (sections 3.2 and 3.3) does the finding. What changes is the cut:

- **Narrow cuts.** Up to two or three notches per module, each about an eighth to a quarter of an octave wide *(to measure)*, placed on the strongest peaks the detector finds in that range. Not a cut across the whole range.
- **Depth capped** at about 6 to 8 dB per notch.
- **Fast:** tens of milliseconds, not hundreds.
- **Threshold** on programme level stays, as now.
- **Finer analysis** where the notches need it: 1/12 octave in the low modules, so a notch can be centred on a peak. The earlier finding that 1/6 octave was enough was about detection, not about where to place a narrow cut.

The top two modules need analysis up to 12 kHz, which the present 8 kHz analysis rate cannot give. They need a second, coarser band bank at the full rate, and their cost has to be measured against the CPU target.

### 12.3 The goose

The pond holds something for each module, and the goose eats it when that module is cutting. Each kind of food lives in its own stretch of the frequency axis. One possible set, to be drawn in flat shapes like the goose:

| Module | In the pond |
|---|---|
| Boom | Frogs in the deep end |
| Box | Snails on the bank |
| Mud | Weed in the shallows |
| Honk | Other, smaller geese, which it chases off |
| Click | Water boatmen skittering on the surface, which it snaps up |

How much it eats follows the cut in that module. A module that is switched off has nothing in its stretch of pond. Solo Honk shows what the goose has eaten.

### 12.4 Order of work

1. Narrow, peak-following cuts in Box and Mud, with 1/12-octave analysis there. Render the four mixes again and listen: the removed signal should now ring, not hiss.
2. Module structure and controls for those two, then Boom and Honk, which the present band bank already covers.
3. Harsh and Sizzle with the second band bank. Measure CPU.
4. Display and goose.

The plug-in shell and tests built so far assume one focus band and have to be reworked for modules at step 2.

### 12.5 Step 1 as built (16 November 2026)

The core now cuts with narrow notches. Sections 3.2 to 3.4 describe the earlier wide cut and are out of date where they differ from this.

- Analysis is 1/12 octave, 73 bands from 40 Hz to 2.56 kHz.
- The detector of section 3.3 still decides how far each band should come down. That is now only the depth.
- A notch is placed only on a peak: a band that has stood at least 1.5 dB above the bands a sixth to a third of an octave either side of it, averaged over about 250 ms. The four most prominent are kept, at least a quarter of an octave apart, and a notch that is already open is preferred so it does not hop.
- Each notch is one bell with a Q of 9 (about 0.16 octave at half depth), at most 8 dB deep. The overlap matrix of section 3.4 is gone, since separate notches do not overlap.
- Default range 110 to 400 Hz (Box and Mud together). Attack 5 to 40 ms, release 50 to 300 ms.

Measured first: within any 0.7 s of a busy passage, all four mixes have a narrow peak 5 to 8 dB above its surroundings somewhere between 120 and 350 Hz. Averaged over a whole mix those peaks disappear (1 to 4 dB), so they move with the music. That is the case for notches that follow, and against fixed ones.

On the four mixes at defaults, in the busy moments: about two notches open at a time, the deepest averaging 3 to 4.3 dB and reaching 6 to 7.7 dB a tenth of the time, mostly between 127 and 260 Hz. In the quieter half, close to nothing on two mixes and about one notch of 1.2 to 1.5 dB on the two loud ones. A notch stays open for about 150 ms at the median, which may still be too restless.

Not yet done: the test harness and plug-in shell still expect the earlier core (37 bands, one focus band, the overlap matrix) and have not been rebuilt against this one. CPU has not been measured again.

### 12.6 Step 2, modules in the core (16 November 2026)

After a listen, the narrow notches of step 1 were judged closer to removing honk. The core now has the first four modules.

- **Boom, Box, Mud and Honk**, each switchable. Box and Mud are on by default. Harsh and Sizzle wait for the second band bank (step 3).
- **Shared edges.** Neighbouring modules share an edge, so five frequencies define all four: 60, 110, 200, 400 and 1500 Hz by default, each adjustable within its own limits (45 to 80, 90 to 150, 160 to 280, 300 to 600, 800 to 2200 Hz). This replaces Focus Low and Focus High.
- **Up to two notches per module** at a time.
- **Each module is judged the same whichever others are on.** Boom is compared with everything; the others never count the bass below 110 Hz. An earlier version let the modules that were on form one range, and switching Boom on then hid Box and Mud behind the bass.
- **Where modules are on next to each other** there is no gap between them. The cut eases in above the bottom of the lowest one and falls away above the top of the highest.

On the four mixes with all four modules on, in the busy moments: three and a half to four and a half notches open, the deepest averaging 3.8 to 5.5 dB. Box and Mud cut exactly as they do with the other two off. Boom does little on these mixes below 100 Hz (under 0.5 dB on average); it has not been tuned or listened to. Honk cuts 0.3 to 1.9 dB on average between 430 Hz and 1.2 kHz and has not been listened to either.

In progress: the plug-in shell and the test harness are being reworked for modules, and the core is under independent review for correctness, robustness and speed.

### 12.7 Shell, tests and review (16 November 2026)

**Plug-in shell.** Rebuilt for modules by Opus, then reviewed and fixed. Threshold, Depth, Sharpness and Speed knobs; four module buttons each with a meter of its own cut; five draggable edges on the display; a programme-level meter with the threshold marked; Solo Honk and Diff; presets. It builds as VST3 and Standalone with no warnings. It has not been run in a host, and pluginval is not installed on the build machine. The display is still the plain stand-in; the pond and goose are not started.

**Independent review of the core.** Three reviews (the mathematics, robustness, speed) found real faults, all now fixed:

- The analysis bands narrowed towards the top of the range, under-reading a tone between two bands by up to 4.4 dB in the Honk range. The band edges are now pre-warped as well as the centres.
- Half of Boom could never be cut: a leftover constant from the 1/6-octave version made the parent rule reject every band from 57 to 76 Hz.
- A change of Lookahead clicked. The two delays now cross-fade over 10 ms.
- One non-finite sample, or one non-finite parameter value, switched the detector off or the output to silence until a reset. Both are now kept out.
- A module's cut spilled above its top edge into a neighbour that was off, and Box cut differently once Boom was switched on. Modules now work strictly inside their own edges.
- Notches hopped: on a steady pair of peaks a notch stayed put for a median of 13 ms. Where a notch sits is now decided on slow averages and only its depth follows the fast detector. The same test now reads 680 ms, and on the four mixes 240 to 300 ms.
- A notch that had lost its place kept ringing down beside its replacement, so the two cuts added to more than the ceiling. It is now let go quickly.
- The gate took 100 to 150 ms to open after a quiet stretch, because the level follower had sunk far below the threshold. It is now held within 12 dB of it.
- Speed: the reference sum reorganised, the detector run every millisecond in place of every half, no detector work while the gate is closed, notches closed sooner, coefficients computed once.

**CPU**, core alone, stereo, on the development Mac (an M3 Ultra, faster than the M1 the targets are written for):

| | Before review | After |
|---|---|---|
| 48 kHz, Box and Mud | 0.63% | 0.35% |
| 48 kHz, all four modules | 0.98% | 0.49% |
| 96 kHz, Box and Mud | 0.87% | 0.47% |
| 96 kHz, all four modules | 1.29% | 0.71% |

The target that no block takes more than twice the average is not met at small buffers (the detector's work falls in one block in every few) and is to be restated as a limit on the worst block as a share of its own duration: the worst block measured is a few microseconds.

**Tests.** 21 tests, 20 pass. They cover: exact pass-through at zero Depth, notch shape (depth within 0.01 dB, 0.16 octave wide), transparency on noise and on a sweep, nothing changed below the lowest module, peaks cut where they are and not beside them, the parent rule, notch count and spacing, modules independent of each other, the threshold, Solo Honk and Diff identities, latency, block-size invariance, stability on hostile input, clicks under automation, sample-rate consistency, CPU, and no allocation on the audio thread.

The one failure is a design question, left failing on purpose: with Honk on, a lone narrow peak at 900 Hz is cut by only 0.9 dB, because nothing sits at any subharmonic of it and the parent rule takes it for an instrument's fundamental. In Box and Mud that is the wanted behaviour. Whether Honk should use the rule at all needs listening.

The earlier unexplained cut of up to 6.7 dB on a lone sine sweep is gone (0.02 dB); it belonged to the wide-cut detector.

**On the four mixes at defaults after these changes**, busy moments: 1.2 to 1.7 notches open, the deepest averaging 3.0 to 4.7 dB and reaching 6 to 7.8 dB a tenth of the time. A fourth set of excerpts was rendered for listening.

Still out of date in this document: sections 3.2 to 3.5, the controls table in section 4 (Focus Low and High are now the module edges), and the test list in section 7. They are to be rewritten once the module design is confirmed by ear.

### 12.8 Click, the Honk question, and the look (16 November 2026)

**Click replaces Harsh and Sizzle.** The request: something of our own for aggressive transient pops in the 2.5 kHz region, and not a dynamic EQ.

A dynamic EQ turns a band down when it is loud. That cannot tell a pop from a loud sustained sound, and it dulls every drum hit. Click asks a different question: is the 1.8 to 4 kHz band spiking, and are the bands either side of it not?

- It measures how sharply the band is spiking: its instant peak against its own level over the last 40 ms. A steady sound, however loud, does not spike.
- It measures the same in a band below (400 Hz to 1 kHz) and a band above (6.4 to 12 kHz). A drum hit or an explosion spikes in all three at once. A click spikes in the middle one only.
- What is left over (the middle band's spike beyond a 10 dB margin, and beyond the neighbours' spike plus 3 dB) is taken out of that band alone, by up to 12 dB, for as long as the spike lasts, and let go within 6 ms.
- It is not tied to the Threshold: a click is a click at any level. Depth sets how much is taken.
- Cost: a handful of filters at the full rate, only while the module is on. It adds no latency.

Measured on made-up signals, at defaults:

| Signal | Peak in the 1.8 to 4 kHz band |
|---|---|
| 2.5 kHz clicks, 3 ms long, over noise | 6.4 dB lower |
| 2.8 kHz pops, 1.2 ms long | 4.4 dB lower |
| 3.5 kHz clicks, 3 ms long | 3.9 dB lower |
| Broadband hits, 6 ms | 0.4 dB lower |
| Snare-like bursts, 120 ms | 0.2 dB lower |
| Sustained 2.5 kHz tone | unchanged |
| Noise alone | unchanged (0.01 dB) |

On the four mixes it acts in about half a percent to one percent of the running time, up to 12 dB. Nobody has listened to it yet, and whether what it catches are the pops meant is unknown: Click-only renders of the whole of each mix were made, treated and removed-only. The shortest pops are reduced least because the cut takes about a tenth of a millisecond to arrive; if that matters, a short look-ahead inside the module is the fix. It is not in the plug-in's controls or tests yet.

**Honk and a lone peak.** The open question from 12.7, decided for full mixes: Honk keeps the parent rule. A narrow peak between 400 Hz and 1.5 kHz with nothing at any subharmonic below it is the fundamental of something: a lead line, a sung note, a siren, a designed tone. On a composite of music, dialogue and effects that is the thing the listener is meant to hear, and cutting it would hurt. Honky midrange is different: it is emphasis on the overtones of lower-pitched sources, a voice or brass pushed nasal, and those always have something below them, so Honk still reaches them. The test now checks both: the 500 Hz peak with content below it is cut (4.8 dB), the lone 900 Hz peak is left alone. All 21 tests pass.

**The look.** Late-1990s low-polygon 3-D rendering, the same family as Pitch Flattener's window: a low-resolution picture scaled up in chunky pixels, low-polygon flat-shaded shapes, few colours with dither, vertices that snap to the pixel grid so things shimmer slightly as they move. This replaces "flat shader shapes" in section 5: the goose, the pond and what it eats are low-poly models. The whole window takes the style, controls included, as Pitch Flattener's does. No game is to be named in code, interface or documents.

Rendering choice: draw the scene with OpenGL into a small off-screen picture (about 320 pixels wide) and scale it up without smoothing. That keeps the drawing on the graphics card, which matters for a plug-in that sits on a master all session, and a picture that small costs almost nothing. Pitch Flattener draws in software; De-Honk's scene is animated the whole time it is open, so the card is the better place for it. A software still picture remains the fallback.

### 12.9 The look corrected, and who is in the picture (17 November 2026)

**The look is the generation after: smooth, not pixelated.** Section 12.8 said chunky pixels and dither; that was wrong and is withdrawn. The reference is a games console of about 2001 to 2005: low-polygon shapes with smooth shading, soft fog, clean edges, glassy panels and capsule buttons with a soft glow. No scanlines, no dither, no blocky scaling. No console or game is to be named in code, interface or documents.

**How the picture is drawn.** In software, at 480 by 270, and scaled up smoothly by the editor. The backdrop (sky, water, bank) is filled a row at a time, which costs almost nothing because the camera never rolls; only the characters and reeds are drawn as triangles. Measured: 1.2 ms a frame on the development Mac, so about 4% of one core while things are moving at 30 frames a second and well under 1% when the picture is still. That is on the message thread, never the audio thread, and only while the window is open. Software was chosen over the graphics card because it can be rendered and checked without a window (every picture in this section was), it cannot upset a host the way a plug-in's OpenGL view can, and at this size the cost is small. If the cost matters later, the row fills are the first thing to speed up.

**What is in it** (`dehonk/Source/scene`, prototype, `docs/scene-prototype.png`):

- A pond at dusk seen from the bank. The frequency axis runs along the water's edge, 40 Hz on the left to 5 kHz on the right.
- The reeds at the edge are the spectrum. A reed under a notch bends over and turns red.
- A post stands at each module edge, and each module that is on lights a strip of the bank.
- **A goose lands wherever a notch opens, and honks.** The deeper the notch, the bigger the goose and the faster it honks. When the notch closes, it flies off over the water.
- **A keeper with a boom pole runs along the bank after the worst of them** and shoos it with the fluffy end.
- A fish jumps when Click catches a click.
- Solo Honk: the light goes down and only the geese are lit. Bypass: the keeper sits down and the geese carry on. Diff: the keeper stops where he is.
- One goose lives out on the pond and minds its own business.

This is the idea that the goose is the honk, and De-Honk is whoever gets rid of it.

**Other ways to cast it**, any of which the same scene code can do by swapping the characters:

1. *The location recordist* (the prototype). A sound recordist in a raincoat and headphones, boom pole in hand, trying to get a clean take while geese keep landing in it. On-brand for a sound house, and the pole gives a reason to chase.
2. *The sheepdog.* A collie herds the geese off the bank, crouching and darting. Cuter, and a dog is easier to animate well than a person.
3. *The park keeper.* Flat cap and a broom. The plainest reading, the least specific to sound.
4. *The doorman.* The bank is a velvet rope outside the mix; a doorman in a long coat turns geese away one at a time. Funniest, slowest: it suits a few deep notches, not many quick ones.
5. *Pests by module.* Whoever chases, each module sends its own nuisance: a bullfrog for Boom, something hiding in a cardboard box for Box, a pig that has found the mud for Mud, the goose for Honk, a cricket or the fish for Click. More to draw, and the picture then tells you which module is working without reading a meter.

Recommended: the recordist, with the pests by module added once the single-goose version feels right. To be chosen.

The characters are rough: the keeper is a capsule with a cap, his arms and second leg barely read, and the headphone covers his face in profile. They need a modelling pass once the casting is settled.

### 12.10 Casting and style chosen (17 November 2026)

- **The recordist it is:** headphones, boom pole with a big fluffy end, chasing geese out of the take.
- **The style is a bright toy box**, not dusk: a sunny sky with fat clouds, round green hills, lollipop trees in sweet-shop colours, water in two blues, a lawn mown in stripes. Characters are a few chunky shapes in flat, cheerful colours with soft shading: the recordist has a big square head with dot eyes, a short round body and stubby legs. This supersedes "a pond at dusk" in 12.8 and 12.9, including for the window's palette, which is to follow the scene: sky blue, lawn green, white, with a warm yellow for anything live and the red of the recordist's jacket for cuts.
- The characters and world are De-Honk's own. No game is named, and nothing is copied from one.
- The backdrop is now redrawn every fourth frame and copied back in between, since only the clouds and water move in it. Measured: 1.3 ms a frame with the fuller backdrop.

`docs/scene-prototype.png` shows the current state.

Still to do in the scene: the pests by module, a walk for the goose when its notch drifts, the recordist's idle (he should fidget, check his headphones, look at the sky), "HONK" lettering, and a pass on proportions once it is seen moving in the window.

### 12.11 Weather (17 November 2026)

The weather follows how honky the mix has been over the last while, so the picture gives a slow read of the whole mix as well as the moment.

- **What drives it:** the deepest notch and how many notches are open, averaged over about twenty seconds on the way up and forty on the way down.
- **What it does:** clear and sunny when there is little to do. Then the sun dims, more clouds slide in and darken, the sky and water go grey, the water breaks into quicker, narrower bands and the reeds lean in the wind. Past about half way it rains.
- **How fast:** with a mix that keeps three deep notches open the whole time, it is half turned after 12 seconds and raining after about 30. When the mix calms it is half cleared after about 45 seconds. Nothing in it changes from one moment to the next.
- It keeps following the mix while the picture is otherwise held still (Calm), since it is a reading and not a chase.

`docs/scene-weather.png` shows clear, clouding, rain, and cleared again. Frame cost with weather: 1.4 ms.

Not done: anything for the recordist to do about the rain (a hood, an umbrella over the fluffy end), puddles, or a rainbow when it clears.

### 12.12 Click fixes, the window, and the band view (17 November 2026)

**Click.** The first tests of the Click module in the harness found two faults, both fixed:

- It fired on plain noise about 38 times a second, taking 0.2 dB out of its band for nothing. Ordinary peaks of noise stood more than 10 dB above the band's running level. The margin is now 14 dB: 5 events in 20 seconds of pink noise, and no change in level.
- Switching it off in the middle of a cut jumped. It now finishes letting go first. Switching it on used to fire at once, because its running levels start from nothing; it now waits about 120 ms before trusting them.

At the new margin a 3 ms click at 2.5 kHz comes down 5.0 dB (6.5 dB at full Depth), a 1.2 ms pop 2.9 dB, a 3.5 kHz click 3.4 dB. Drum-like hits change by 0.07 dB. With Click on, the core costs 0.15 points more of one core at 48 kHz.

**Tests:** 27, all passing, including six for Click.

**The scene in the window.** It is now drawn at the shape and size of its panel, not at a fixed 16:9 that was then cropped (the first window showed only the bottom half of the picture, with the recordist's head cut off). In a very wide window the characters are made smaller so they still fit.

**The first window was not liked:** dark glassy panels with amber glow were judged ugly against the bright scene. The window is being redone as a toy box to match: flat bright colour, a thick navy outline on everything, a flat shadow straight below each shape, fat pills and discs, a mown-lawn background that carries on down from the scene, one colour per module. The palette is in `Source/scene/ToyPalette.h`.

**Layout.** The pond is bigger and nothing is drawn over it any more. Under it, full width so the frequency axes line up, is a new second view.

**The band view** (`Source/scene/BandView`) is the exact reading, where the pond is the fun one:

- a bar for each of the 73 bands: pale blue for what comes in, with a red cap for what is taken out;
- above the bars, the same cuts on a larger scale (0 to 12 dB), hanging down in red, so a 3 dB notch is easy to see;
- each module's stretch in its colour, greyed when it is off, and the Click band at the right with one bar that jumps on each click;
- the edges between modules as handles to drag (this is where the edges are now moved);
- the programme level against the threshold at the left, yellow below it and red while De-Honk is acting;
- the deepest cut, or what is being heard (removed only, untreated, bypassed).

### 12.13 Our own names, and the window restyled (17 November 2026)

**The modules are renamed.** The first names (Boom, Box, Mud, Honk, and Click) were the ordinary mixing words and too close to another product's list. These are ours, in the words used for trailer and mix problems:

| Module | Range | What it is for |
|---|---|---|
| **Woof** | 60 to 110 Hz | The low end barking on every hit |
| **Tub** | 110 to 200 Hz | Tubby weight just above the bass, where added bass harmonics pile up first |
| **Fog** | 200 to 400 Hz | Low mids clouding over when music and effects stack up |
| **Bark** | 400 Hz to 1.5 kHz | Hard, shouty midrange |
| **Snap** | around 2.5 kHz | Sharp clicks and pops that the rest of the sound does not share |

Tub and Fog are on by default: between them they are the honk this plug-in is named for. The parameter IDs, presets, tests and render tool use the new names. Earlier sections of this document keep the old names where they record what was done at the time. "Solo Honk" keeps its name.

**The window was restyled by hand** as a toy box (`docs/window-prototype.png`, `docs/window-all-modules.png`): flat bright colour, a thick navy outline on every shape and a flat shadow straight below it, on a lawn that carries on down from the pond.

- Top bar: the name on a yellow pill, the preset in a wide white pill with round previous and next buttons, Save, Delete, Bypass.
- The pond, as a card, with nothing drawn over it. About 936 by 346 at the default window size (it was about 880 by 250 and cropped).
- The band view under it, the same width, so the two frequency axes line up.
- Five module pills, each in its module's colour while on, with a light that is yellow while it is cutting and a meter of how deep.
- Four knobs as flat coloured discs with a fat white pointer and a ring that fills to the value: Threshold red, Depth yellow, Sharpness green, Speed pink. Names above and values below on white pills.
- Solo Honk and Diff as two big rounded squares that stay pressed down and change colour while on.
- Link, Lookahead, Mix and Trim as pills along the bottom.

Default size 960 by 800, minimum 820 by 700.

Not done or not checked: the window has only been seen as rendered pictures, not live in a host; keyboard focus is no longer shown on controls and needs a proper focus ring; redraw cost measured through the snapshot path is high (10 ms at normal resolution, 23 ms at double) and has to be measured in a real window before deciding what to speed up; menus and the save dialog have the new colours but have not been looked at; the typeface is still the monospace used by the other plug-ins, where a rounded one would suit this look better.

### 12.14 Not a toy: the look changed again (17 November 2026)

The toy-box window of 12.13 (thick navy outlines, flat shadows, primary colours, capitals) was judged too childish. The direction is now Japanese graphic design of the early 2000s: a little strange, flat, restrained, not cute. Sections 12.10 and 12.13 are superseded where they describe colours and shapes; the layout and the contents of each view stand.

- **Colour:** off-beat pastels on cream (mint, peach, mustard, lavender, olive, aqua) with a tomato red for anything being cut. Lines and text are a warm dark brown, never black. The palette is in `Source/scene/ToyPalette.h` and is shared by the window, the band view and the pond.
- **Line and shape:** thin lines (about 1.25 px), no shadows, no gradients, small corner radii. A control that is on or held changes colour; nothing moves.
- **Type:** small, lowercase, widely spaced, in the monospace the plug-ins already carry, which suits this better than it suited the toy box.
- **Background:** cream, with pale rays fanning out from below the right-hand corner, and thin five-colour rainbow rules under the name and between the pictures and the controls.
- **Knobs:** a flat disc like a record, a pale hub, one thin hand, and a ring of fifteen dots that fill in up to the value. Threshold peach, Depth mustard, Sharpness mint, Speed lavender.
- **Modules:** flat tabs in the module's colour, with a small square lamp that turns red while the module is cutting and a thin line for its depth.
- **Solo Honk and Diff:** two flat discs. On, a disc turns dark with pale lettering and a dashed ring turns slowly round it.
- **The pond:** a peach sky with slowly turning sun rays and a rainbow over the far side, mint water, a pale lawn, pastel trees and hills. The recordist's jacket is the same tomato red as a cut. Honk rings are red.

`docs/window-prototype.png` and `docs/window-all-modules.png` show it. All 27 tests pass (the change is to the window only).

Open, as before: never yet seen live in a host; redraw cost to be measured in a real window; no visible keyboard focus; menus and dialogs not looked at; the sun and rainbow sit above the top of the pond's panel at the default window shape and need bringing down into view; the recordist often shows the back of his head.

## 13. Additions from mix review notes

Added 17 November 2026 from a written addendum. Where it said "the Honk module" and "Click" it now says Bark and Snap, the modules' names since 12.13. Numbers marked *(to measure)* are starting points. Each part says what state it is in.

### 13.1 Where this comes from

About 1,900 written review notes from past trailer and cinematic sessions were read for what reviewers actually ask for. They are edit-stage and mix-stage notes, not notes on a master, so they say what a good mix is judged on, not how De-Honk should cut. Counted by the words used:

| What the note asks for | Notes |
|---|---|
| More size, bigger, or "too small" | about 115 |
| Clarity, cleaner, clearer | about 80 |
| Less wash or noise | about 75 |
| More tail or sustain | about 65 |
| Tonal content, or in tune with the music | about 55 |
| Musical timing | about 45 |
| Dialogue | about 35 |
| Sounds like a click | about 20 |
| Boomy, boxy, muddy, honky, by name | under 20 in total |

Three things follow for De-Honk.

- Reviewers almost never name a low-mid fault. They ask for size and clarity. De-Honk will be judged on whether the busy moments get clearer without getting smaller, not on whether "honk" went away.
- Size is described as body and tail in the low mids, and the standing complaint about a weak hit is that it "sounds like a click". The low mids are where De-Honk cuts. Taking body or tail off a big hit is the most likely way for it to make a mix worse.
- "Wash" is always fixed by taking layers out, never by EQ. This agrees with section 11.5: when what De-Honk removes sounds like noise, it is cutting the wrong thing.

### 13.2 Body and tail of impacts (tests first, a guard only if they fail)

A ringing mode in the tail of an explosion is a narrow peak with a parent below it, so today it is eligible for a notch. That may be right (a ring) or wrong (the body that makes the hit big). Measure before changing anything.

Add to the corpus: at least ten big impacts over music, taken from the mix corpus, with onset times marked. Add tests:

- **Body.** Energy from 110 to 400 Hz, summed over the 1.5 s after each marked onset, changes by less than 1 dB at defaults *(to measure)*.
- **Tail.** The time for that band to fall 20 dB after the onset changes by less than 10% *(to measure)*.
- **Front.** The peak level of that band in the first 30 ms after the onset changes by less than 1 dB at defaults, at every Speed setting *(to measure)*.

If these fail on listening as well as on numbers, add a sustain guard: a peak that appeared with a broadband onset and has only fallen since is the tail of a hit, and its notch depth is capped at about 3 dB *(to measure)* until the next onset. Snap already has a broadband onset measure (section 12.8) that can be reused.

### 13.3 Do not chase noise

Section 11.5 found the removed signal sounded like noise. Section 12.5 improved this with narrow notches on peaks. Add a direct check so it cannot regress.

- For each candidate peak, measure how steady it is: its level over the last 100 ms against its own variation in that time, and how far it stands above bands a quarter of an octave either side *(to measure)*. A resonance is steady and narrow. A fluctuation in dense noise is neither.
- Scale eligibility by that measure.
- Test: dense band-limited noise at programme level above Threshold gets less than 0.5 dB of cut in any band. A steady resonance 6 dB proud of the same noise gets the full cut.

Optional, display only: a small Wash reading in the band view showing how much of the low-mid energy is noise-like while the gate is open. It changes nothing in the audio. It tells the mixer that the fix is upstream, by muting layers, and not in De-Honk.

### 13.4 Notes that belong to the music

The most repeated musical instruction in the notes is that anything tonal must be in tune with the music, and that a clashing tone can change the mood of a section. The reverse also holds: a sustained tone in the low mids that is in key is wanted (bass notes, low brass, tuned hits, drones).

The parent rule protects a lone fundamental. It does not protect a note in a full arrangement, because the bass an octave below gives every note a parent.

Step 1, measurement only, no change to the sound:

- Move the centres of the 1/12-octave bank onto equal-tempered semitones (41.2 Hz upward in place of 40 Hz). The bands are already a semitone apart, so this costs nothing. **Done:** band 0 is E1 (41.2 Hz), band 72 is E7 (2637 Hz).
- Sum band levels across octaves into twelve pitch classes, averaged over 5 to 10 s. This is a rough key profile. **Done:** the core reports it in each display frame, averaged over 8 s.
- On the four mixes, log where each notch opens against that profile: on one of the strongest pitch classes, on a weak one, or between semitones.

Step 2, only if step 1 shows notches landing on in-key notes and listening confirms it hurts:

- A Musical control, 0 to 100%. A peak on one of the strongest pitch classes that has been stable for longer than about 300 ms has its depth scaled down by this amount. A peak more than about 30 cents from any semitone, or on a weak pitch class, is cut in full *(all to measure)*.

Known limits: the reference pitch may not be 440 Hz, effects are mostly unpitched, and a busy trailer passage may have no stable key. The default must leave De-Honk no worse on those passages than it is now. One more, from the build: with bands a semitone wide, "more than 30 cents from a semitone" cannot be read from one band; it needs the two neighbouring bands compared.

### 13.5 Sidechain: make room

The commonest balance instruction in the notes is to keep effects, music and dialogue out of each other's range, and to make a moment bigger without making it louder. On a track or bus (section 2, second use) De-Honk can do that if it can hear the other element.

- An optional external sidechain input.
- Version 1: when the sidechain is connected and switched in, Threshold reads the sidechain's level in place of the track's own. Nothing else changes. On a music bus keyed from the effects bus, the music's low-mid resonances come down only while a hit is playing. On a music and effects bus keyed from dialogue, with Bark on, the midrange clears only under a line.
- Later: place notches where the sidechain has energy and the track has peaks, so only the overlap is cut.

Decide the bus layout before the first release. Adding a sidechain bus afterwards changes the plug-in's layouts in every format, and the host notes already warn about changes after release.

### 13.6 Listening tools

All three follow the rules of Solo Honk and Diff in section 4.1: not stored, not automatable, clearly indicated while on.

- **Level-matched Diff.** The untreated signal is louder, and the louder one tends to win a comparison. While Diff is on, match the two within 0.1 dB on a running loudness measure and show the offset applied. **Done in the core:** Diff plays the untreated signal turned down by the difference in level between the two over the last three seconds, and reports the offset. The measure is plain mean-square level, not a loudness model; that is to be judged by ear. The window does not show the offset yet. Bypass is unchanged: exactly the input.
- **Small speaker.** A monitor-only band limit, about 150 Hz to 6 kHz, with an optional mono sum *(to measure)*. Mixes are checked quietly on small speakers, and that is where low-mid build-up is judged.
- **Headroom.** A readout of how much the peak and short-term level fell while the gate was open. Returned headroom before the limiter is the practical benefit, and it shows "bigger, not louder" as a number. In the harness, run a plain reference limiter after De-Honk and report the change in its gain reduction on the corpus.

### 13.7 Corpus labels and the listening sheet

Extend section 7. As well as honky passages, mark in each mix: big impacts (onset times), tonal hits and logo hits, dialogue lines over music and effects, and music-only passages.

Listeners score each marked passage, untreated against treated and level matched, with a fixed list of words: boomy, boxy, muddy, honky, clicky, washy, small, thin, unclear. Acceptance: no passage gains "small", "thin" or "unclear" when treated, and the marked honky passages lose at least one of the first four.

Dialogue: on the marked dialogue passages, report the cut between 110 and 400 Hz while the line is playing. Voice body sits there. Set a limit after the first listen *(to measure)*.

### 13.8 Surround, when it comes

Add to section 9: detect on the stereo fold-down as well as per channel. A mix that is clear in surround often builds up in the low mids only when folded down, and stereo is what most listeners hear.

So far only the Threshold reads the fold-down. Detection takes the louder channel per band, as in stereo; detecting on the fold-down as well is still to do.

### 13.9 Order and cost

1. Corpus labels and the tests in 13.2, 13.3 and 13.7. No DSP change. Opus writes the tests, Fable reads the results.
2. Level-matched Diff.
3. The noise check in 13.3, if its test fails.
4. Sidechain bus layout decided, then version 1.
5. The key measurement in 13.4, step 1.
6. Small speaker and Headroom.
7. Sustain guard and Musical control, only if their measurements call for them.

Not proposed: release times tied to tempo, any automatic level change, or any broadband cut. None of them is supported by what was measured.

### 13.10 Questions for Samuel

1. Is a sidechain wanted in the first release, or only the bus layout reserved for it?
2. Should the Wash reading exist at all, given the display is already busy?
3. Who marks the impacts and dialogue lines in the corpus?

### 13.11 Drop the borrowed modules (decided 17 November 2026, not yet built)

The five switchable areas came from looking at another product (section 11.5), and the review notes in 13.1 say they are not what our mixes are judged on. They go. De-Honk is one thing done well:

- **One range**, 110 to 400 Hz by default, with two edges to drag. It is the low-mid build-up this plug-in was started for. No Woof, Tub, Fog or Bark, no five switches, no per-module meters.
- **What matters for our mixes, in order:** the busy moments get clearer without getting smaller (13.2: body, tail and front of impacts), it does not chase noise (13.3), it leaves notes that belong to the music alone if measurement says it should (13.4), and it can be judged fairly (13.6: level-matched Diff, done; Headroom; Small speaker).
- **Snap stays**, as one switch. It was asked for separately, it is our own design, and it does one job the range cannot. It is a second tool, not a module in a row.
- The freed space in the window goes to the pond and the band view. The band view shows one coloured stretch with two handles.

To do, once the section 13 tests now being written are in: take the modules out of the core (one range, the bass still left out of its reference, up to four notches), the parameters and presets, the window and the tests.

### 13.12 Visual fixes (17 November 2026)

- The camera now tilts by the shape of the picture so that the horizon sits about three tenths of the way down at any window size. The sun's rays and the clouds are placed by where they should appear on screen, so they are in view. The rainbow is still mostly hidden behind the far hills and needs raising.
- The recordist turns further towards the camera, more so when standing still, so his face shows.
- A dashed focus ring appears just inside the control that has the keyboard focus, only while the keyboard is being used to move about; a mouse click hides it again.
- The save dialog was rendered and looks right: cream, thin outline, rainbow rule, lowercase pill buttons. The pop-up menus have still not been seen.

### 13.13 The three questions answered (17 November 2026)

1. **Sidechain: in the first release.** Built in the core: a block can be processed with a key, and with the key switched in the Threshold compares the key's level in place of the track's own. The key's level is taken over its whole spectrum on its loudest channel (the track's own is taken below 2.8 kHz). Nothing else changes. Not yet done: the sidechain bus in the plug-in's layouts (to be settled before any release), the switch in the window, the key's level in the band view, and tests.
2. **Wash reading: yes, and the window can grow to fit it.** Built in the core as a figure from 0 to 1: of the energy in the range, the share that is not in peaks, averaged over two seconds. Not yet in the window.
3. **Dialogue: stems will be supplied.** They are for three things: checking that De-Honk does not thin a voice (13.7), finding where lines fall in a mix when a stem belongs to one of the corpus mixes, and keying the sidechain from dialogue (13.5). Stems that belong to the four corpus mixes are the most useful; any trailer dialogue will do for the first and third.

**What the Wash reading says about the four mixes.** While De-Honk is acting:

| Signal | Wash |
|---|---|
| Three steady tones on their own | 0.08 |
| Pink noise with three steady tones in it | 0.32 |
| Pink noise alone | 0.80 |
| Mix 1 | 0.84 |
| Mix 2 | 0.88 |
| Mix 3 | 0.74 |
| Mix 4 | 0.85 |

The reading does not reach 1 on plain noise, because noise throws up short-lived peaks by chance; 0.8 is its ceiling in practice and it should be rescaled before it is shown. Read against that ceiling, the low mids of all four mixes are almost entirely wash by this measure, with mix 3 the only one showing a clear share of peaks. That fits what was heard in section 11.5 and what the review notes say in 13.1: on these mixes most of the low-mid build-up is not something a notch should take out, and De-Honk should be doing little on them. It is a strong reason to get the noise test of 13.3 passing before tuning anything else, and to expect modest cuts on this material however it is tuned.

### 13.14 The studio library: 47 finished mixes and their stems (17 November 2026)

The studio's delivery archive was opened up for this. It holds about 41,000 audio files in 536 output folders. Of those, 1,062 are stereo mixes with a dialogue stem beside them, and 1,011 of those have music and effects stems as well, across 43 projects. These mixes were described as not honky. Nothing from the archive is in the repository; the list of what was used and the measurements are kept in the git-ignored corpus folder.

A spread of 47 mixes from 33 projects (106 minutes) was run through De-Honk at defaults, each with its dialogue stem.

**Levels.** The middle of a mix sits at -14.7 dBFS (the measure the Threshold uses), and its loudest 15% at -9.8 dBFS or above; the quietest tenth of the mixes run about 10 dB lower. With the default Threshold of -16 dBFS, De-Honk is acting for 38% of a typical mix. The four reference mixes sit inside this range, so -16 stays the default.

**De-Honk does as much on these as on the mixes called honky.**

| | 47 library mixes (median, 10th to 90th percentile) | The four reference mixes |
|---|---|---|
| A notch over 1 dB open | 37% of the time (0 to 63%) | 37 to 68% |
| A notch over 3 dB open | 19% (0 to 39%) | 19 to 39% |
| Deepest notch, averaged | 1.45 dB (0 to 2.6) | 1.4 to 2.5 dB |
| Wash while acting | 0.78 (0.69 to 0.84) | 0.74 to 0.88 |
| 110 to 400 Hz against 600 Hz to 2 kHz | +2.8 dB (0.8 to 5.2) | +2.1 to +4.4 dB |

On every one of these the reference mixes fall inside the spread of the ordinary ones. De-Honk as built cannot tell the two apart, and neither can the plain measures of balance. Either the four have too little honk to register, which is how they were described when they were supplied, or the detector is not looking at what makes a mix honky. On mixes that are not honky it should be doing close to nothing, and it is not.

**Dialogue.** A line is playing for about a quarter of a typical mix. While it plays, the deepest cut between 110 and 400 Hz averages 0.6 dB, against 1.65 dB when no one is speaking, because spoken passages are mostly below the Threshold. A cut over 3 dB lands on 6% of dialogue time. Fed a dialogue stem on its own with the Threshold off, De-Honk has a notch over 3 dB open for 18% of the speech and takes 0.36 dB out of the voice's low mids: it does notch voices when nothing stops it, and the Threshold is what keeps it off them in a mix.

**Making known honk from the stems.** For ten of the mixes the music and effects stems were each put through RBass (Frequency 80 Hz, Original Bass in) and summed again with the dialogue, giving a clean version and a honky version of the same mix with the difference known exactly.

| | Clean | RBass at 0 dB | RBass at +6 dB |
|---|---|---|---|
| Most added in any band from 110 to 400 Hz | 0 | +6 to +10.5 dB | +10 to +15 dB |
| Deepest notch, averaged | 0.6 to 2.3 dB | 1.3 to 4.0 dB | 2.6 to 4.7 dB |
| A notch over 3 dB open | 8 to 32% | 17 to 61% | 36 to 68% |

So it does respond to real added honk: on every one of the ten it cut more on the honky version, usually about twice as much. Part of that is only that RBass makes the mix louder and so keeps it above the Threshold for longer, which has to be separated out. And these settings add far more than a mixer would leave in.

**What this changes.**

- There is now a way to tune against a known answer: clean and honky versions of the same mixes, at settings taken from real sessions, with the aim of cutting what RBass added on the honky one and close to nothing on the clean one. That replaces tuning on four mixes by ear.
- The first target is the clean side: De-Honk at defaults should be nearly idle on the 47.
- The settings RBass is really used at are needed from someone who uses it, so that the honky versions are honest.

### 13.15 The section 13 tests, and a score with a known answer (17 November 2026)

**Tests.** The harness now has 31 tests (26 pass). New: `noise_is_left_alone`, `impacts_keep_body_tail_front`, `headroom` and `key_profile`, with tools that find impacts in a mix and report where notches land against the note profile. Five fail:

| Test | What it found |
|---|---|
| `noise_is_left_alone` | Dense noise from 80 to 600 Hz gets notches: the deepest is under 0.5 dB only 69% of the time (the limit is 99.5%). A steady resonance 6 dB proud of that noise is cut by 0.1 to 1.2 dB (it should be at least 3). It cuts the noise and misses the resonance. |
| `impacts_keep_body_tail_front` | On ten synthetic impacts over music at defaults: Body down 1.2 dB at worst (limit 1), Tail 13.5% shorter (limit 10%), Front unchanged. With the Threshold off: Body down 3.4 dB, Front down 3.7 dB. |
| `notch_count` | For one to three milliseconds at a time, a notch that is letting go overlaps its replacement a semitone or two away. 15 such frames in 11,000 against 11 allowed. |
| `solo_honk_identity` | Level-matched Diff is 0.13 dB off the treated signal over three seconds on a signal whose level is moving (limit 0.1). No bias over twelve seconds. |
| `automation_clicks` | Switching Diff steps the level by its offset over 5 ms. That is a fade, not a click, but a 6 dB change that fast needs slowing. |

**Pairs.** From the library's stems, twelve mixes were made in a clean version and two honky versions (RBass on the effects stem at -6 dB; RBass on music and effects at 0 dB), with what RBass added known for every semitone band and every 50 ms. Eight more are held back for checking. A scorer runs any version of the core over them.

**Where the core stands on the pairs,** with the Threshold off:

- On the clean versions the deepest notch averages 3.1 dB and is over 1 dB for 71% of the time.
- On the honky versions it removes 15% of the energy RBass added.
- In the places where RBass added more than 3 dB (6 to 7 dB on average), it cuts 0.3 to 0.45 dB. In those same places on the clean version it cuts 0.27 to 0.32 dB. So the cut there has almost nothing to do with the honk being present.
- Where nothing was added it cuts 1.3 to 2.4 dB.

The detector built so far does not detect honk. It notches whatever tonal peaks are in the low mids when the mix is loud. That is the finding the four reference mixes could not give, and everything in section 12 about how much it cut on them should be read in that light.

**In progress:** four independent redesigns of the detector, each from a different idea (harmonics that move with their fundamental; steadiness and narrowness; looking for the harmonic series of the bass; tracing and repairing the present one), each scored on the twelve pairs and then checked on the eight held back, against the tests, and for anything fitted to these mixes.


### 13.16 Max Cut, Release and Impact Guard as controls (17 November 2026, revised after review the same day)

Three things that were fixed, or tied to another control, are now controls of their own, each automatable and saved with the session and with presets. The first version was reviewed against the four reference mixes and ten library mixes the same day and found wanting in several places; this section describes what is built now and says where the first version went wrong.

**What has not changed.** With Max Cut at 8 dB, Release at what Speed used to give, Impact Guard off and the old way of moving a notch, the core's output is bit for bit what it was, in mono, stereo, 5.1 and 7.1.4, on arm64 and x86_64 (`golden_mono_stereo`). The old way of moving a notch is `Tuning::handOver = false`. No control reaches it: the old sound cannot be dialled in from the window. It is kept in the core for this proof only. The hashes of the new defaults were recorded on purpose (`golden_defaults`), with three extra cases that reach Max Cut and bring the guard up.

**Max Cut, 3 to 12 dB, default 6.** No notch goes deeper, whatever Depth is set to. Depth keeps its meaning: how hard it works under that ceiling. It replaces the fixed 8 dB. Two notches closer together than the spacing never add up to more than the ceiling either. Turned down while a notch is open, the notch follows over about 15 ms, so automating it does not click. The band view shows it as a dashed line across the cut lane, and a full module meter is Max Cut as it is set.

**Release, 50 ms to 1.5 s, default 400 ms.** Speed used to set the attack and the release together, and at its middle the release was 122 ms. Speed now sets the attack only (40 ms at Slow to 5 ms at Fast, as before) and Release sets how slowly a notch lets go. It applies in full to a notch that has settled, and there are three cases where a notch goes faster, each put in because the first version measured badly without it:

1. *A notch whose peak has moved to the next band.* The notch taking over grows only as the old one lets go, so the two never add up to more than one notch was asked for and nothing jumps. The old one lets go in 130 ms (or at Release if that is shorter), not at Release. In the first version it took the whole Release: on a tone that steps two semitones the band it had left was still cut by over 1 dB 0.8 s later, and in loud passages of the reference mixes the cut on bands that were not peaks went up by 52 to 76%. "Moved" means the band now chosen is within 3 dB of what the old band was while it held the notch. A band that is merely the strongest thing left after a peak has faded (the noise after a note stops) does not hurry the old notch out.
2. *A notch that never settled.* Settled means: on a chosen peak for a quarter of a second at the depth a peak 4.9 dB above its surroundings gets (3 dB at the default Depth, never more than three quarters of Max Cut). One that has not lets go in 45 ms. On plain noise the bands take turns to stand out and get shallow, short notches; holding those for the whole Release made `noise_is_left_alone` worse than it was before the controls existed.
3. *Under the Threshold.* The Threshold is now a gain of its own on the cut, not part of what the release smooths. When the mix drops under it every notch closes over 40 ms, whatever Release is (measured: shut 294 to 375 ms after a tone stops, at Release 50, 400 and 1500 ms; most of that is the level meter falling). In the first version a notch stayed open into the quiet at the Release: with the mix under the Threshold a notch over 0.5 dB was still open 6 to 39% of the time, against 2 to 10% before. Now it is 0.0%. The detector keeps what it had, so if the mix comes straight back the notches are where they were.

A chosen notch's depth now follows the fast detector alone. It used to be held up by a slow average as well, which kept every notch open a quarter of a second after its peak had gone, whatever the release.

What the number means: once a notch's peak has gone, its depth falls with this time constant: half after 0.7 of it, a tenth after 2.3 of it. Before that starts, the tone has to leave its analysis band, which is narrow and rings: 100 to 200 ms for a tone 20 dB or more above its surroundings at 150 to 250 Hz, less higher up. Measured on a gated harmonic tone, a 6 dB notch is at half depth 150 to 240 ms after the tone stops at Release 50 ms, 400 to 510 ms at 400 ms, and 1.2 to 1.3 s at 1500 ms. At long settings the last decibel takes long to go if what is left behind asks for small cuts of its own. The first version of this section said a 6 dB notch at 400 ms is under half a dB a second after its peak has gone. That was not true then and is not promised now.

Measured on the four reference mixes at the default Threshold. A notch is a band cut by more than 1 dB until it is under 0.5 dB for 100 ms; "deep" ones reach 3 dB. Loud passages are those with the mix at least half way into the Threshold's knee.

| | Before (release 122 ms, Max Cut 8) | First version (400 ms, slow hand-over) | Now (defaults) |
|---|---|---|---|
| Deep notches: median time open | 478 to 629 ms | 1.1 to 1.2 s | 581 to 882 ms |
| Deep notches: open under 250 ms | 13 to 17% | 1 to 2% | 8 to 17% |
| Deep notches: how fast the depth moves (median) | 21 to 26 dB/s | 9 to 10 dB/s | 13 to 16 dB/s |
| All notches: median time open | 352 to 456 ms | 875 to 984 ms | 410 to 534 ms |
| Sum of all cuts, loud passages | 4.8 to 6.5 dB | 6.2 to 7.7 dB | 5.2 to 6.9 dB |
| ... on bands 3 dB or more proud of their neighbours | 2.4 to 4.4 dB | 2.4 to 4.4 dB | 2.1 to 4.2 dB |
| ... on bands under 1 dB proud (not a peak at that moment) | 1.3 to 2.0 dB | 2.2 to 3.0 dB | 1.6 to 2.3 dB |
| A notch over 0.5 dB open while the mix is under the Threshold | 2 to 10% of that time | 6 to 39% | 0.0% |
| Most bands cut at once (0.1 dB or more), two modules | 6 to 7 | 8 to 9 | 6 to 8 |

So the deep notches, the ones that are heard, stay 20 to 40% longer and move at a little over half the speed they did, and the shallow ones come and go as before. The first version was steadier still, but it bought that by leaving cuts behind on bands the sound had left. The price that remains: 15 to 31% more cut on bands that are not peaks at that moment than before (it was 52 to 76% more), which is what a slower release is.

On synthetic tones over pink noise (a mechanical check, not evidence about honk): stepping two semitones, the new band reaches 3 dB after 232 to 290 ms (226 to 251 before, 349 to 446 in the first version) and the band left behind is over 1 dB for 293 to 417 ms (96 to 243 before, 774 to 871 in the first version). Gliding an octave in 6 s, the cut on the 2nd and 3rd harmonics is 3.8 and 3.1 dB (4.8 and 4.0 with the old movement at Max Cut 6; 2.7 and 2.0 in the first version), with 3.2 dB left on bands the tone has passed (1.2; 5.6). A glide is still followed less closely than before: the cross-over takes 130 ms where the old code switched in 14.

Other release times now, on the four mixes (deep notches: median time open; how fast the depth moves; sum of cuts on bands that are not peaks): 200 ms gives 472 to 692 ms, 18 to 22 dB/s, 1.1 to 1.6 dB; 300 ms gives 533 to 767 ms, 15 to 18 dB/s, 1.4 to 1.9 dB; 700 ms gives 0.7 to 1.1 s, 10 to 13 dB/s, 2.1 to 3.0 dB; 1.5 s gives 0.8 to 1.2 s, 9 to 12 dB/s, 2.8 to 4.0 dB. At 200 ms the depth moves as fast as it did before; from 700 ms the cut on what is not a peak is where the first version was. 400 ms stays the default.

On dense noise (`noise_is_left_alone`, which failed before the controls and still fails its own limits): at the defaults the deepest notch is under half a dB 72.5% of the time and averages 0.371 dB (69.25% and 0.390 dB before the controls; 54.8% and 0.595 dB in the first version); with all four modules 42.2% and 1.204 dB (33.2% and 1.209 before; 15.5% and 1.582 in the first version). The test now also fails if these get worse than they were before the controls. The 99th percentile is a little higher than before (2.59 dB against 2.36). Bringing the test inside its limits needs the detector work of 13.15.

**Impact Guard, Off to 100%, default 100%.** When a broadband impact lands (an explosion, a hit, a slam), the cuts are kept out of it while it sounds. At Off impacts are treated like anything else.

How it tells one, every millisecond, from the analysis bands that are there anyway (no lookahead):

1. *It rises everywhere at once.* A band counts as risen when it stands 6 dB or more above its own short average, taken from about 50 to 250 ms back (and never less than a floor 25 dB under the whole range, so that an empty band does not count for going from nothing to next to nothing). When 45% or more of the bands from 150 Hz up have risen together, something has landed, and the guard comes up (half up 9 ms after the test burst lands, fully in 17 ms). A note, a chord or a resonance raises a few bands and never gets there. The first version compared each band with a peak it remembered for half a second. A hit inside a loud passage does not stand 6 dB over the last second's peaks, so that version went up only on a loud onset after about a second of relative quiet, when there is usually no notch to protect: on 7 of 59 hits in the reference mixes and 9 of 154 impacts in the library stems.
2. *It dies away, or it does not.* The level of those bands is watched, averaged over 25 ms. Once it is 6 dB under the highest it reached, it was an impact, and the guard stays up until the level is back within 2 dB of what was there before it landed, at most 600 ms. If the level has not fallen by 280 ms (a hard cut to a new scene, a held chord, a wall of sound starting), it was an entry and the guard lets go then. It lets go in a straight line over 120 ms, so the stated times are the real ones.
3. *It cannot switch the plug-in off through a busy passage.* The average a band is compared with catches up with a new level in a couple of hundred milliseconds, so a passage that is loud all the time raises nothing. A hit inside one is over when the level is back to the passage's own. Nothing new can land for 150 ms after a landing. And the guard is never up for more than about 35% of any few seconds.
4. *There is something to keep out.* Nothing counts while the mix is under the Threshold. One absurd sample cannot blind it: no band is taken as louder than +12 dBFS (a single sample of 1e6 blinded the first version for about 12 s; now it is blind for about a second, and a burst 1.5 s later is caught).

What it does while it is up, at 100% (less in proportion below):

- *No new depth.* The detector's own state is held where it was. No notch grows into the impact, and the notches that were there are still there after it. A tone that starts with a burst gets no notch while the guard is up (0.21 dB in `impact_guard`, against 6 dB with the guard off).
- *Open notches are eased off as far as the impact drowns out what they were put there for.* Each open notch is lifted as far as the impact stands over what was there before it landed, in the notch's own band or across the whole range, whichever is more: not at all up to 2 dB over, fully from 7 dB. A notch under an explosion that has taken its band over would be cutting the explosion, and the resonance it was put there for cannot be heard under it. A notch on a band where the old sound still leads, under a hit no louder than the passage, stays as it is and goes on doing its job. As the impact dies away the notches come back with it, over 100 ms, so nothing is switched. The first version lifted every notch to nothing for about 0.8 s on anything it took for an impact, and then some cut was held back for up to 1.75 s. Holding every open notch at its depth instead was also measured: it is too little when an impact has a mode on the same note as an open notch (3.7 dB off its front in the test signal).

Measured on the four reference mixes at the defaults. Hits were found on the input by a separate rule (three quarters of the third-octave bands from 100 Hz to 12.8 kHz up 6 dB and 10 dB at the median over the previous 140 ms, within 8 dB of the mix's loudest, falling 3 dB within 0.4 s, the plug-in at work): 59 of them.

- 32 of the 59 raise the low mids (110 to 400 Hz, where the default modules work) by 6 dB or more. The guard goes up on 25 of those (78%). Their loss in that band: front (first 60 ms) -0.37 dB at the median with the guard off and -0.16 dB with it on (worst -1.52 and -1.06); body (400 ms) -0.45 and -0.10 dB (worst -2.28 and -1.04).
- The other 27 are bright hits over a low-mid bed that hardly moves (up under 6 dB there). The guard goes up on 15. What the low mids lose under those is the plug-in cutting the music's own peaks, which is its job; the guard changes it little (front -0.53 to -0.25 dB at the median).
- The guard goes up 4 to 14 times a minute and is up for 1.9 to 7.1% of the time (3.5 to 8.6% of the loud passages). The sum of all cuts over the loud passages falls by 6.1%, 6.6%, 2.6% and 6.2% against the guard switched off. With the share of bands at 50% in place of 45% it goes up on 22 of the 32 and gives up about 2 to 6%; at 40% on 30 of the 32, and gives up more. 45% is the default.

On ten library mixes with stems (sets.txt lines 2, 3, 6, 10, 13, 14, 16, 17, 18, 19), impacts found the same way in the FX stem alone, where the FX stem is at least half the mix and the plug-in is at work: 154, of which the guard goes up on 104 (68%; the first version: 6%). Of the 116 where the plug-in has at least 2 dB of cut open 100 ms in: 75 (65%). These mixes are full of impacts: the guard is up for 8 to 23% of their loud passages (15% at the median) and gives up 4 to 27% of the cut there (11% at the median). That is the guard doing what it is for on material that is mostly impacts. Where it is unwanted the control is there to turn down.

What an entry costs: when a loud passage starts out of quiet, everything comes up at once and the guard takes it for an impact until it has seen that nothing dies away. The first notch then comes about a third of a second later than with the guard off (half depth after 416 ms against 67 ms from silence in `release_time`). The first version cost the same at the start and then kept the plug-in out for up to a second.

Measured on the ten impacts of `impacts_keep_body_tail_front`, worst case of the ten:

| | Body | Tail | Front |
|---|---|---|---|
| Limit | 1 dB | 10% | 1 dB |
| At defaults, before the controls | -1.22 dB | -13.5% | 0.00 dB |
| At defaults, now | -0.08 dB | -4.4% | 0.00 dB |
| Always acting (Threshold -60), Impact Guard off: the impact alone / on the mix | -3.55 dB / -2.95 dB | -17.6% | -2.73 dB |
| Always acting, now: the impact alone / on the mix | -0.24 dB / -1.12 dB | -18.0% | -0.77 dB |

At the defaults the test passes with room. Always acting, the test now checks what the impact itself loses: its Body (the impact alone through the filters the mix got) and its Front are inside 1 dB, and are far outside with the guard off, so the test would notice a guard that only showed on the display. Two figures are not inside the limits of the default case, and are not meant to be. Body on the mix over 1.5 s (-1.12 dB) is mostly the bed's chord tones being cut again once the impact has died away, which is the plug-in's job. Tail (-18%, checked against 25%) follows the impact alone 20 dB down, well under the bed, and counts what the notches on the bed's own tones take from the far end of its decay once it is under what was there before; under the bed that cannot be heard, and the guard does not change it (-17.6% with the guard off). Both came inside when the notches were kept lifted for 300 ms longer (the first version kept everything lifted for 0.85 s), and that gave up 3.6 to 10% of the cutting on the reference mixes in place of 2 to 6% at the same trigger setting, which is too much to pay for a figure on a synthetic signal.

What it will not tell apart: a bright, full chord or a stab starting hard out of quiet is broadband by any cheap measure. New notches wait about a third of a second for it, or up to 0.7 s if it also falls away at once.

**In the window.** Max Cut and Release are knobs beside Depth and Speed, six in the row. Impact Guard is a bar at the left of the bottom row, in whole percent (what reads Off is 0), with a small square lamp, like the ones on the module switches, lit in mustard while the guard is up. The lamp and the module meters fall back when the host stops calling, like the rest of the display. The factory presets keep 6 dB, 400 ms and 100%, except "Narrow resonances only" (Release 700 ms: a resonance stays where it is) and "Heavy" (Max Cut 9 dB).

**Cost.** Percent of one core at 48 kHz, 512-sample blocks, on a loud minute of one reference mix, the lowest of twenty passes each, old and new built and run side by side: stereo 0.41% before the controls and 0.37% now at the defaults (0.36% at the old settings, 0.37% with the guard off); 7.1.4 0.87% and 0.88% (0.86%, 0.87%). The guard's detector and what it does add about 1%. Heaviest (all four modules, unlinked, always acting): stereo 0.85% and 0.87%, 7.1.4 4.03% and 4.23%. The first version was 0.45%, 0.96% and 5.16%: its slow hand-over kept more filters running.

**Tests.** `max_cut_is_a_ceiling`, `release_time` and `impact_guard` are new, each in mono, stereo, 5.1 and 7.1.4; `golden_defaults` is new. After the review: `impact_guard` and `impacts_keep_body_tail_front` check the audio and not only the display, and fail with the guard taken out of the filters; `release_time` checks the whole time from the tone stopping, and that every notch is shut within half a second under the Threshold at any Release; `notch_count` bounds the number of filters running; `noise_is_left_alone` holds the figures from before the controls as a ceiling; the click limits for Max Cut and Impact Guard are -50 dB, where taking their smoothing out is noticed; `golden_defaults` checks that its cases cut, reach Max Cut and bring the guard up, and hashes the guard's reading. 46 tests, 43 pass. The three that fail: `noise_is_left_alone` (its own limits, as before), `automation_clicks` (switching Diff, as before), and `solo_honk_identity` (Diff's level against the treated signal over 3 s: 0.109 dB against a limit of 0.1; it failed before the controls, passed the first version by 0.002 dB, and nothing in Diff has changed).

### 13.17 Dialogue sidechain, each range's own timing, link modes, and Diff's switch (18 November 2026)

Three additions asked for after the first test build, and two faults in Diff put right.

**What has not changed.** With Dialogue off, every range's timing locked, Link at Linked or Unlinked and Diff not engaged, the core gives the samples and the display frames it gave before, bit for bit. The core as it was the day before was kept (`Tests/build/ref-dsp3`) and run beside the new one on the golden cases: 100 renders at its own defaults, 100 at the settings that stand for the older cores, and 124 at the plug-in's defaults with the cases that reach Max Cut and bring the guard up: mono, stereo, 5.1 and 7.1.4, with Link at 0, 30, 50 and 100% and swept. All 324 are the same, on arm64 and on x86_64. The proofs against the two older cores still hold (`golden_mono_stereo`).

Two things were left out of those comparisons because they changed on purpose: anything with Diff engaged, and the offset Diff reports in each frame. So the stored hashes were recorded again, deliberately:

- `golden_ref_*` and `golden_surround_*` (the two older cores): from those same cores, with the "diff" case dropped, "automation" no longer switching Diff, and the offset out of the hash. Before that, each old core was built with the cases as they were and gave the old file exactly (72 of 72, both architectures), so they are the cores the files came from.
- `golden_defaults_*` (the current core): recorded again, now with two Diff cases and the offset in the hash. Every one of its hashes differs from the day before's because the offset is in every frame; the renders without Diff are the 124 above.

#### Dialogue sidechain

The plug-in has a second input, and a switch, Dialogue. Feed the dialogue stem to the sidechain and switch it on: De-Honk then knows what in the mix is the voice and leaves it alone. Latency stays at zero. The key is taken in time with the mix, as the host delivers it.

How it works. The key goes through the same low-pass, decimator and 1/12-octave band bank as a channel of the mix, with the same smoothing, so the two are in step. Every millisecond, per band, the key's power is set against the power the detector is working from (the louder channel per band when linked, a group's when grouped, a channel's own when unlinked):

- Dialogue 9 dB or more under the band: the band is treated as ever. Build-up that stands well over the voice is still cut while the voice is in it.
- Dialogue within 3 dB of the band (half its power or more): the band is the dialogue's. Nothing is asked for there, so it is not chosen for a notch and does not use up one of its range's two notches, and a notch already open there is eased off.
- In between, in proportion (a smooth ramp on the power ratio: about a quarter at 6 dB under, two thirds at 4.5 dB).
- A notch is about two bands wide, so a band next to one that is the dialogue's is left alone by half as much.
- A band is handed to the dialogue over 15 ms and taken back over 250 ms, so the gaps between words do not pump the notch in and out. When the voice has gone the cut on what is left returns at the usual attack.
- Dialogue Amount (0 to 100%, default 100%) is how far a band that is all dialogue is left alone: at 50% half the cut stays.

Nothing else changes: what a band is compared with, the Threshold and the impact guard see the mix as they always do. So with the key in, the cut elsewhere is what it would have been, less what the rules above take off next to the voice and just after it.

(Superseded the same day: see 13.18, where the level is measured.) The comparison needs the key at the level the dialogue has in the mix. A key that arrives louder is turned down to fit, by up to 12 dB: the dialogue cannot be more than the mix it is part of, so over the bands the dialogue is strongest in (within 15 dB of its loudest), the lowest the mix stands against the key (the lowest tenth of those bands, on 250 ms averages) is how far the key is over. It goes down over half a second and comes back over ten. This matters in practice: a stereo stem sent to a mono key input arrives 6 dB up for a voice in the middle. It only ever turns the key down. A key that arrives quieter than the dialogue is in the mix is taken as it is, and protects less (see the figures); turning a quiet key up by the same reasoning would hand bands to a voice that is hardly there in a dense passage.

The constants (`dehonk::Tuning`): `dialogueStartDb` -9, `dialogueFullDb` -3, `dialogueSpread` 0.5, `dialogueAttackMs` 15, `dialogueReleaseMs` 250, `dialogueTrimMaxDb` 12, `dialogueTrimFallMs` 500, `dialogueTrimRiseSeconds` 10. The first two channels of the key are listened to, and a band counts as the dialogue's if it is in either.

Measured on the studio library (13.14): all 47 mixes with their DX stems, 1 h 46 min, the stereo mix through the plug-in at its defaults (Threshold -16 dBFS, Tub and Fog) with the stereo DX stem as the key (`dehonk_dialogue`, `tools/dialogue_library.sh`). Every band of every frame in the two ranges was sorted by how the stem stands against the mix there, from renders without the key: the dialogue's (stem within 3 dB of the mix), not dialogue (12 dB or more under), mixed. At the median 11% of them are the dialogue's (1 to 42% by mix). 34 of the 47 mixes have a cut open (half a dB or more) in a band that is the dialogue's at some point; the figures are over those, mean of the mixes.

| | No key | Keyed |
|---|---|---|
| Bands that are the dialogue's, where a cut was open | 1.61 dB | 0.047 dB (2% left at the median, 12% on the worst mix) |
| The deepest cut in any band that is the dialogue's, per frame | 0.36 dB | 0.013 dB |
| Mixed bands | 0.031 dB | 0.007 dB |
| Bands that are not dialogue, where a cut was open | 2.33 dB | 2.20 dB (96% kept at the median, 80% on the worst mix) |
| The key's trim | | -0.3 dB at the median (39 of 43 mixes with speech within 1 dB of 0; one at -4.3) |

The 4% given up outside the dialogue is the half share next to a dialogue band and the quarter of a second after the voice.

The same with other things changed:

- The stem's two channels added into a mono key (6 dB hot): 1.61 to 0.040 dB on the dialogue's bands, 2.33 to 2.19 dB on the others. The trim reads -5.6 dB at the median, within 1.5 dB of -6 on 34 of 43 mixes (-8.2 to -2.4 over all of them), so a hot key does as well as a true one.
- Always acting, all four ranges on (45 mixes with a cut in a dialogue band): 2.24 to 0.063 dB on the dialogue's bands, and the deepest cut in one, per frame, 1.69 to 0.066 dB; 2.78 to 2.55 dB on the others (93% kept at the median).
- Dialogue Amount 50%: 1.61 to 0.75 dB (47% left at the median).
- The key 6 dB quieter than the dialogue is in the mix: 1.61 to 0.79 dB. Half of the protection is lost. The stem should be sent at the level it has in the mix, or hotter.

What it cannot do. It compares powers, not waveforms, so it does not know which channel the voice is in. Unlinked or grouped in surround, a channel with no dialogue in it has its notch eased too while the voice (in the centre) is strong in that band; the cut comes back when the voice leaves. And a mix that has passed through something that changes the dialogue's level by band before De-Honk (heavy bus processing) is compared less exactly; the 3 dB of tolerance is for that.

Buses and hosts. The processor has two input buses: the main one, and "Dialogue", mono by default, which may also be stereo or off. The main input and output still have to be the same layout, mono to 7.1.4.

- Pro Tools (AAX): JUCE's wrapper offers a key input only for a mono second input bus, in the layout it settles on from the defaults, and lets the host switch it off if the plug-in also takes the bus disabled. Both hold (checked by `DeHonkHostCheck` with the wrapper's own rules, for every stem format), so the plug-in shows the key input selector, mono, on every track width. The AAX build was not made or run for this change.
- VST3: the bus is an auxiliary input; hosts show it as a sidechain, mono by default, stereo if the host asks.
- AU: a second input element; Logic shows it in the Side Chain menu. Not built here (VST3 and the standalone app were).
- The standalone app has no sidechain and ignores the bus.

With Dialogue on and nothing connected, or silence on the key, the output is bit-identical to no key. The key's channels are only read. `DisplayFrame::keyed` is true while the dialogue is being listened for, `keyDb` is the key's level, `dialogue[band]` (0 to 1) is how far each band is being left to the dialogue, and `dialogueTrimDb` is the trim. The processor's `isSidechainConnected()` says whether the host has the bus on.

The switch that was already in the core (`Params::keyOn`: the Threshold listens to the key's level instead of the mix's) is still there and still not reached by any control.

#### Each range's own timing

Woof, Tub, Fog and Bark each have a lock, an attack and a release. Locked, which is the default, a range follows Speed and Release exactly as before. Unlocked, its notches go in over its own attack (2 to 100 ms; Speed covers 40 ms at Slow down to 5 ms at Fast) and let go over its own release (50 ms to 1.5 s, as Release). The three cases of 13.16 where a notch goes faster than its release apply to an unlocked range as to any other, against its own release. The Threshold opening the cut still follows Speed, and closing it still takes 40 ms. Snap keeps its own fixed timing.

Measured (`range_timing`, mono, stereo, 5.1 and 7.1.4): with Release at 150 ms and Fog unlocked at 1000 ms, Fog's notch falls with a time constant of 999.9 ms and Tub's with 150.0 ms. Fog's notch is at half depth 30 ms or more later with its attack at 100 ms, and sooner at 2 ms, than locked, while Tub's does not move. Locked, with other values set, the output is bit-identical to the defaults.

#### Link modes

The Link percentage is gone. Link is a choice:

- **Linked** (default): every channel shares one detection and gets the same notches, so the image does not move. What Link 100% was.
- **Grouped**: the front left and right, the centre, the surrounds and the heights each share within their group. The wides and inner fronts count with the front pair, rear and side surrounds and a rear centre together as the surrounds, every overhead speaker as a height. With one or two channels it is the same as Linked.
- **Unlinked**: each channel on its own. What Link 0% was.

The Threshold reads the whole mix's level in every mode, and Snap's detector is shared, as before. Checked (`link_modes`): Unlinked is bit-identical to what Link 0% was; in 5.1, 7.1.4, 7.0.2 and LCR each group comes out bit for bit as its channels do on their own in a linked instance; the LFE is untouched; switching modes while it plays does not click.

A session or a user preset from the first test build still has "link" as a percentage. It is read as Linked from 50% up and Unlinked below, and the parameters that build did not have take their defaults (`DeHonkHostCheck` restores such a state). The in-between percentages, which shared part of the detection, are no longer reachable from the window; the core keeps the value (`Params::link`) for the proof above.

#### Diff

Two faults from 13.16's list.

- *Switching it clicked.* Diff plays the untreated signal turned down to the treated one's loudness, and it used to move there with the 5 ms smoothing every output mode uses: a change of several dB in 5 ms. It is now a cross-fade of its own, 80 ms long, along a curve that starts and ends at rest and has no corner in it even when Diff is switched again half-way (a cubic from where the fade is, at the speed it has). The two signals are faded with gains that add up to one: they are the same signal but for the notches, and gains that kept the power constant, as for unrelated signals, would lift the level 3 dB half-way. `automation_clicks`: -52 dB against a limit of -40 (it was -24), -48 dB switched every 60 ms.
- *The level match was 0.108 dB out over the last 3 s of the test, against a limit of 0.1.* The match compares the loudness of treated and untreated over about 3 s. It did so with a running average whose time constant was 3 s, which looks back further than a 3 s window does. The time constant is now half of that (the average age of what it weighs is then that of a 3 s window), and the gain it applies moves over 50 ms. The test reads 0.086 dB. That is inside the limit without much room: on that signal the treated level moves by 0.3 dB from second to second, any running measure lags a plain mean of the last 3 s, and the reading does not fall steadily as the time is shortened (0.101 dB at 2 s, 0.049 at 1.5 s, 0.082 at 1 s). Over the whole settled stretch the two agree within 0.02 dB.

Bypass still wins over Diff, and Diff over Solo Honk. Neither test was changed.

#### Parameters

Added at the end of the host's list, except Link, whose mode took the place of the percentage (same position, new ID). IDs are for sessions, presets and automation.

| ID | Name | Values | Default |
|---|---|---|---|
| `linkMode` | Link | Linked, Grouped, Unlinked (0, 1, 2) | Linked |
| `dialogue` | Dialogue | Off, On | Off |
| `dialogueAmount` | Dialogue Amount | 0 to 100%, whole percent | 100% |
| `woofTimingLocked`, `tubTimingLocked`, `fogTimingLocked`, `barkTimingLocked` | (range) Timing Locked | Off, On | On |
| `woofAttack`, `tubAttack`, `fogAttack`, `barkAttack` | (range) Attack | 2 to 100 ms, logarithmic; tenths up to 10 ms, then whole | 14 ms |
| `woofRelease`, `tubRelease`, `fogRelease`, `barkRelease` | (range) Release | 50 to 1500 ms, as Release | 400 ms |

`link` is no longer a parameter. The state version is 2. The factory presets leave every range locked and Link at Linked; one is new, "Master bus, dialogue keyed" (the defaults with Dialogue on).

In the window so far only the Link bar has changed (it steps through the three modes). Dialogue, its Amount and the ranges' timing have no controls yet; that belongs to the window's restyle.

#### Cost

Percent of one core at 48 kHz, the better of 128 and 1024-sample blocks, 20 s (`surround_cpu`), old and new run on the same machine one after the other. Defaults: stereo 0.36% before and 0.36% now, 7.1.4 0.85% and 0.85%. All four ranges and Snap, always acting, linked: stereo 0.64% and 0.64%, 7.1.4 1.18% and 1.22%; unlinked: 0.88% and 0.85%, 3.77% and 3.84%. These are within what one run differs from the next. New settings: Dialogue on with a loud key adds about 0.05 points in stereo (0.42%) and 0.10 in 7.1.4 (0.95%), which is the key's own analysis; every range unlocked costs nothing measurable (0.37%, 0.87%); Grouped with everything on in 7.1.4 is 2.25%, between linked (1.22%) and unlinked (3.84%), since it runs four detectors and the notches channel by channel.

#### Tests

Three new tests, each at 1, 2, 6 and 12 channels: `link_modes`, `range_timing` and `dialogue_sidechain` (the mechanics on made-up signals: the dialogue's band keeps under 0.1 dB of cut and is let go within 100 ms, a tone 15 dB over the dialogue keeps its cut, the mix's own tone is cut again once the dialogue has left, a key 6 dB hot is trimmed, a hostile key does no harm, nothing clicks). `automation_clicks` also switches Diff inside its own cross-fade and with Mix moving, steps the link mode and sweeps the ranges' timing. `no_allocation_in_process` changes the new parameters and runs with and without a key. `DeHonkHostCheck` covers the sidechain bus and the old state. 49 tests, 48 pass. `automation_clicks` and `solo_honk_identity` pass now. The one that fails is `noise_is_left_alone`, on its own limits, as before (13.15, 13.16); nothing here touches it.

Not done: no AAX or AU build, so the key input has not been seen in Pro Tools or Logic; no listening. The figures say the cut leaves the dialogue's bands and stays elsewhere, not how that sounds.

### 13.18 After review of 13.17 and of the window (18 November 2026, later the same day)

A review of the second test build before it went out found the things below. Each was put right the same day. Where this section and 13.17 disagree, this one stands; in particular the "level trim" of 13.17 (the key turned down, never up, from the lowest tenth of mix over key) is gone.

**What has not changed.** With Dialogue off, every range's timing locked and Diff not engaged, the core still gives what the core of 17 November gave: the same three comparisons as in 13.17 (`dehonk_golden_ref3` against `dehonk_golden`: 100 renders at the core's defaults, 100 at the compatibility settings, 124 at the plug-in's defaults), 324 of 324 bit-identical on arm64 and on x86_64. `golden_defaults_*` was recorded again, because the offset Diff reports is in every frame and is worked out differently now (7 of its 140 hashes, the ones with no cut, did not move). `golden_ref_*` and `golden_surround_*` were not touched.

#### The dialogue's level is measured, not assumed

The review found that a key arriving quieter than the dialogue is in the mix lost its protection with nothing to show for it (about half at 6 dB under, nearly all at 12), that a key more than 12 dB hot, or the wrong stem, switched De-Honk off elsewhere, that a key out of time was read as a hot one, and that in surround with Link at Grouped or Unlinked a channel with no dialogue lost up to a third of its cut while the centre spoke.

All four come from comparing powers. The key is now looked for in each channel of the mix. Every band of the bank gives its output (the channel's and the key's) and, for the key, the same output a quarter of a cycle behind (the band-pass's own low-pass output). The products of a channel's output with the key's two are averaged over half a second (`dialogueGainMs` 500) of the time the key was playing; they wait unchanged through its pauses. What the two share in a band, R squared plus I squared, does not depend on what else is in the band, nor on a delay of a few milliseconds between them, nor on polarity. Summed over the bands, less what two unrelated signals share by chance, and divided by the key's own power squared, it is the gain (as power) the dialogue has in that channel:

- The chance level per band is K x M / (1 + 4 x T x 0.0578 f), K and M the key's and the channel's power there, T the averaging time. The 4 was measured with unrelated pink noise on mix and key (3.3 to 5.8 across the bands; with a steady tone in the mix the band it is in reads about twice the model).
- Eight delays from 0 to 45 ms are tried, each with what it would leave of the shared part in every band ((1 + x) e^-x in amplitude, x = pi x one filter section's width x the delay), and the one that leaves the least unexplained gives the gain. So a key up to about 20 ms out of time is still read at close to its level.
- The gain is held between -40 and +18 dB (`dialogueTrimMinDb`, `dialogueTrimMaxDb`) and followed in dB.
- The key counts as found in a channel when the shared part is `dialogueMatchRatio` (4) times the chance level over it. While it is found the gain follows the measurement. While it is not, the gain follows the measurement down only if the key would show at the gain it is held at (so a channel the dialogue is not in goes to nothing), and otherwise stays (the dialogue is covered by a loud passage: nothing can be told).
- Until the key has been found in some channel (the first half second of dialogue after `prepare()`, or a key that is not in the mix or is far out of time) it is taken at the level it arrives at, as before. Once found, the gains are kept through `reset()` and through the key being switched: they are how the session is routed, not audio that has gone by.

The dialogue's power in a channel is then that gain times the key's band power, and the rest of 13.17 (start and full at -9 and -3 dB, the spread, the 15 and 250 ms) is as it was. Linked, the most of any channel is set against the loudest channel; Grouped and Unlinked, each group or channel has its own, so a surround with no dialogue in it keeps its cut.

`DisplayFrame` has `dialogueTrimDb` (the gain the key is taken at, now either way), `keyMatch` (0 to 1: how surely the key has been found), `keyOver` (the key is nearly everything in most of the bands it is in and has been for `dialogueOverSeconds`, or its gain is at an end of its range) and `snapDialogue`. The window's Dialogue lamp turns red while `keyOver` is set, or while the key is playing and has not been found for two and a half seconds.

Measured (`tools/dialogue_library.sh`, all 47 sets, plug-in defaults; the 36 mixes with a cut open in a dialogue band):

| | No key | Key at mix level | 12 dB quiet | Stereo stem summed to mono |
|---|---|---|---|---|
| Dialogue bands where a cut was open | 1.59 dB | 0.055 dB (2% left at the median, 21% on the worst mix) | 0.119 dB (4%, 53%) | 0.065 dB (2%, 28%) |
| Other bands where a cut was open | 2.34 dB | 2.20 dB (96% kept at the median, 81% on the worst) | 2.21 dB | 2.21 dB |
| The gain the key was taken at (median of the mixes' medians) | | +0.1 dB | +12.1 dB | -4.8 dB |

And on six of the mixes with the key as it should not be (`dehonk_realmix key`, through `tools/realmix_library.sh`): 1 ms late changes nothing; 5 ms late leaves 0.11 dB where the aligned key leaves 0.07 (one mix); 20 ms late leaves 0.13 to 0.60 dB of 1.95 to 2.44; 100 ms and more is not found and falls back to the level it arrives at (0.2 to 1.1 dB left). 12 and 24 dB hot are read as -12.4 and -24.4 dB and leave the other bands their cut (2.30 of 2.32 dB at +24, where the trim of 13.17 left 1.96 of 2.72). Another mix's stem on the key is never found (0.00) and is taken at its own level. The mix itself on the key takes every cut off, as it must, and is flagged 97 to 100% of the time; the mix without its dialogue is flagged only about a fifth of the time (while the dialogue speaks it is not most of the mix), so that mistake is caught late.

Cost: dialogue keyed is now 0.51% of a core in stereo and 1.48% in 7.1.4 at 48 kHz (0.42 and 0.95 before).

#### Snap and the dialogue

Snap did not hear the key at all, and cut consonants: with Snap on, 8 to 24% of the 20 ms windows from 1.8 to 3.5 kHz that were mostly dialogue had a cut of 1 dB or more, against 4 to 10% of the windows with none. The key (its first two channels folded to one, through the same lookahead delay as the channels) now goes through Snap's own band filter. Its power there and the mix's are followed with 1 ms up and 50 ms down (`snapDialogueAttackMs`, `snapDialogueReleaseMs`), the key's is scaled by the gain the dialogue has in what Snap listens to, and the same ramp as for a band (start -9 dB, full -3 dB) takes Dialogue Amount times that share off Snap's cut. No latency. With the key: 0.1 to 1.5% of the dialogue windows, and the windows without dialogue as before (`dehonk_realmix snap`).

#### Diff's level match

The review measured Diff against the treated output on real mixes: 0.2 to 0.6 dB out over 3 s windows, with the gain moving at up to 3.5 dB a second after the averaging time had been halved to pass the test. The match is now the ratio of the two levels (each over 0.3 s) in dB, averaged with `diffMatchSeconds` (4 s), every moment counting alike, held while the programme is under the Threshold, and kept with its own weight so that it is right from the first moment. On six mixes, all ranges on, Depth 80% (`dehonk_realmix diff`): over 3 s windows Diff is 0.12 to 0.23 dB from the treated output on average and 0.27 to 0.81 dB at worst; the gain moves at 0.25 to 0.5 dB a second at most. The cut on a real mix moves faster than a steady gain can follow, so no setting gives the 0.1 dB the generated signal gives; a shorter time (1.5 s) matches no better on these mixes and moves twice as fast. `solo_honk_identity` passes with its limit untouched (0.04 dB against 0.1), but its reading still jumps about as the time constant is changed, so it says little on its own.

#### The plug-in's parameters and the hosts

JUCE's AU wrapper orders parameters by version hint and then by a hash of the ID. Everything added since the first test build now carries hint 2 (`ParamIDs::versionHintSecond`), and `link` is a parameter again, in its old place, doing nothing: not automatable, left out of presets, set back to its default on load. So the 22 parameters of the first test build keep the indices Logic and GarageBand address them by. `DeHonkHostCheck` checks the hints, and restores seven states that the first test build itself saved (`Tests/host/FirstBuildStates.h`: one on each of its factory presets, "After RBass" among them, one on its "User preset" entry and one with settings moved).

Display frames are written a sixtieth of a second of audio apart whatever the host's buffer (the block is cut where one falls due and the remainder is kept), and the window takes them one at a time, leaving up to a buffer's worth waiting, so that a large buffer does not show as steps.

Still to be seen in a host: the key input in Pro Tools and Logic. JUCE's AAX wrapper silences the output until the message thread has re-prepared the plug-in when a key connects or disconnects on a plug-in whose sidechain bus may be switched off; the README says to choose the key with the transport stopped. If that is a nuisance in practice, refusing a disabled sidechain bus under AAX avoids it.

#### The pond and the window

- Geese: on real mixes they came and went 59 to 98 times a minute at the defaults. A notch must now have been 1.5 dB deep for half a second (a sixth of a second from 3 dB) before a goose comes for it; a goose keeps its notch down to 0.6 dB, waits 2 to 3 s for one that has closed, and stays 3 s at least; arrivals are 2 s apart; at most five on the bank and two to a range, a much deeper notch sending the shallowest goose in its way off. Measured on four mixes with the real processor's frames (`.wip/pondcheck/beh`): 8 to 13 arrivals a minute at the defaults and 22 to 27 with everything on; the recordist changes goose 2 to 6 and 3 to 10 times a minute.
- With a range in focus, nothing comes for a notch outside it and the geese outside it are not looked for, do not wait, walk or leave. Measured: no arrivals out of focus, none of them moving.
- The recordist turns before he sets off (he does not move until his body is within sixty degrees of the way he is going) and only a shuffle of half a pace is taken facing the goose. At work he stands further round to the side.
- The weather needs cuts near the ceiling in several places to turn, clears quickly in silence, and greys the characters by no more than 30%. Those out of focus fade 38% towards the paper.
- The scene is drawn at the display's resolution (twice over and averaged down on a display of one pixel to the point), within 1.6 million pixels, and one step more coarsely if a picture takes over 7 ms on average on the machine. In the 960 by 800 window on a 2x display: 2.0 ms a picture on average, 5.6 at worst, 60 pictures a second.
- The pond's frequency axis is laid over the band view's plot, so each tree and post stands over its edge handle.
- In the band view a handle moves from where it was (it used to jump to the pointer, by up to 10 dB for the Threshold), Shift moves it a tenth as far, the Threshold shows red from the moment the mix is over it, and the ranges' names are drawn over the cuts.
- A bar's value is where the pointer is (the value's label was inset, which put every value about 11 points to the left).


### 13.19 Detection: Fast and Steady, and Diff's level match again (18 November 2026, later still)

The detection trials (baseline fitting, morphological filtering, pole pulling) agreed on one thing that helps: a narrow peak should be averaged over half a second to a second before it is believed. Two of them added a second: a lift that is wide along the spectrum is not a peak. The owner chose to add these as a switch, Detection, with Fast (as before) and Steady. The trials also showed that no detector tells the honky mixes from the library ones, so Steady is offered as a choice of behaviour, not as better detection.

#### What Steady does

Only the detector's input changes. The notches, their attack and release, the parent rule, the Threshold, the impact guard and the dialogue sidechain work as they do at Fast.

- Each band's power (per detector: the shared one when linked, a group's or a channel's otherwise) is smoothed over `steadySmoothMs` (50 ms). Its level in dB then follows that with `steadyRiseMs` (1000 ms) going up and `steadyFallMs` (400 ms) going down: the steady level. A band is never taken as more than 70 dB under the loudest.
- The excess over the surroundings (the centre-surround ring of section 3, Sharpness as before) is taken on the steady levels, not on the levels now.
- A floor is drawn under the steady spectrum: each band is brought down to the mean of the two bands m away, if that is lower, for m = 1, 2, 3 in turn (`steadyLumpBands` 3, a quarter of an octave; SNIP clipping). The excess used is the smaller of the ring's excess and the band's height over that floor plus `steadyLumpMarginDb` (1 dB). With the 3 dB dead zone a band must stand 2 dB over the floor to be cut at all. A lift half an octave wide or more is part of the floor.
- The parent rule and the silence test still read the levels now. The impact guard and the notch placement (`keepPeaks`) are unchanged.
- The steady level runs only while Steady is chosen. When it is chosen, or the link mode changes which detector a channel uses, or after `reset()`, it starts from the band's 250 ms average, which the notch placement keeps, so the first second is not a jump. That average is only up to date for a slot whose detector ran at the last tick; a channel that was not its own detector while Linked has one as old as the last time it was, so after a switch from Linked such a slot starts from its band powers now instead (a tick stamp per slot says which).
- Cost per detector per 1 ms tick: 73 one-pole steps, 73 logarithms, 73 asymmetric steps and three passes of 73 min-of-mean. No allocation; state is 146 floats and a tick stamp per detector slot.

Constants. The five are the trial's variant G, rounded. They were scanned on the tuning half of the trial's material (honky 1 and 2, library L01, L03, L05, L07, L09, L11: the loudest 60 s of each, at the plug-in's defaults): rise and fall 500/200 and 2000/800 ms, the floor's reach 0 (no floor test) and 6, the margin 0 and 2 dB, smoothing 1 and 50 ms, fall 200 ms. The faster rise cut more and spared dialogue less per dB removed (0.47 dB lost per dB removed against 0.32); the slower one cut half as often on the honky pair and took longer still to catch a build-up. A reach of 6 bands (half an octave) cut a little more with about the same harm per dB; 3 was kept, the width the trials' floor test was made for, and with it a lift an octave wide gets no notch (below). A reach of 0 (no floor test) cut more on the honky pair (51% of loud time over 1 dB against 26%) with the same harm per dB on the library; the owner asked for the floor test. The margin moves the amount up or down without much change in harm per dB. Smoothing of 1 ms and a fall of 200 ms both spared dialogue a little less. Nothing beat the trial's constants, so they stand.

#### Fast against Steady on real mixes

Plug-in defaults (Threshold -16, Tub and Fog, Depth 50%, Max Cut 6 dB, Release 400 ms). The loudest 60 s of the four honky mixes and of twelve library mixes with DX, MX and FX stems (`.wip/trials/baseline/excerpts.tsv`). The measures are the baseline trial's (`.wip/detect/metrics.py`): loud time is where Fast's own activity is over 0.5; dialogue cells are third-octave by 21 ms cells, 100 to 500 Hz, where the DX stem holds more than half the mix's power; hits are FX-stem onsets (9 dB in 30 ms); dense noise is where the FX or MX stem is flat (over 0.5) and steady (within 2.5 dB over 400 ms) in 100 to 500 Hz and holds more than half the mix there.

| | Tuning half: honky (2) | Tuning half: library (6) | Held out: honky (2) | Held out: library (6) |
|---|---|---|---|---|
| Loud time with a cut over 1 dB | 83% / 26% | 81% / 41% | 89% / 55% | 86% / 62% |
| Deepest cut, mean over loud time | 2.97 / 0.71 dB | 2.94 / 1.26 dB | 3.37 / 1.93 dB | 3.40 / 1.99 dB |
| Level change 100 to 500 Hz, loud time | -0.58 / -0.09 dB | -0.63 / -0.28 dB | -0.75 / -0.34 dB | -0.72 / -0.33 dB |
| Cuts over 1 dB at once / time with 3 or more | 1.9 / 1.1; 20% / 0% | 2.0 / 1.3; 24% / 1% | 2.1 / 1.4; 31% / 4% | 2.1 / 1.4; 31% / 3% |
| Dialogue cells: loss | | -0.30 / -0.09 dB | | -0.47 / -0.20 dB |
| Dialogue cells losing over 1 dB | | 7.1% / 1.7% | | 15.5% / 4.5% |
| Real hits, first 60 ms: mean (worst tenth) | | -0.29 (-0.64) / -0.11 (-0.28) dB | | -0.48 (-0.91) / -0.22 (-0.38) dB |
| Real hits, first 400 ms | | -0.41 / -0.17 dB | | -0.46 / -0.25 dB |
| Dense noisy low mids: loss | | -0.53 / -0.17 dB | | -0.57 / -0.21 dB |
| ... share of that time with a cut over 1 dB | | 82% / 40% | | 85% / 53% |

(Fast / Steady in each cell.) Per mix, held out, share of loud time over 1 dB and mean deepest cut, Fast then Steady: honky 3 92% 3.7 dB, 68% 2.6 dB; honky 4 87% 3.1, 42% 1.2; L02 69% 2.7, 29% 0.9; L04 92% 4.0, 75% 2.6; L06 88% 3.7, 69% 2.3; L08 83% 3.2, 58% 1.7; L10 84% 3.1, 48% 1.6; L12 98% 3.9, 90% 2.9.

Steady does about half as much, and most of what it no longer does was harm by these measures. Per dB taken out of the low mids, held-out library: dialogue 0.65 (Fast) against 0.61 (Steady), hits in the first 60 ms 0.67 against 0.67, hits over 400 ms 0.64 against 0.76, dense noise 0.79 against 0.64. So Steady is kinder to dense, noisy layers for what it removes, the same on dialogue and on the fronts of hits, and a little worse on the bodies of hits; the gain on dialogue in absolute terms comes from doing less. On the tuning half the per-dB figures favour Steady on all four (0.32, 0.39, 0.61, 0.61 against 0.48, 0.46, 0.65, 0.84), which is why a held-out half matters. Neither tells the honky mixes from the library ones.

#### The price: time to catch a new build-up

Into each of the sixteen excerpts a narrow lift (a Q 9 peaking bell, +6 or +9 dB, at 130, 160, 200, 250 or 315 Hz) is switched in at 20 s and held (`.wip/detect/probe.py`): a new steady build-up on real material. Time from then until the cut at that band or next to it reaches 1, 2 and 3 dB, where neither mode had more than 1 dB there in the 2 s before (63 cases each):

| | to 1 dB | to 2 dB | to 3 dB | Steady's extra time to 3 dB (median, quartiles) |
|---|---|---|---|---|
| +9 dB lift, Fast / Steady (median) | 0.48 / 1.31 s | 0.56 / 1.52 s | 0.68 / 2.00 s | 0.75 s (0.10 to 2.10) |
| +6 dB lift | 0.62 / 1.97 s | 0.70 / 2.68 s | 1.21 / 3.68 s | 2.67 s (0.60 to 6.17) |

Once settled (from 5 s after the lift) the two cut a strong lift alike: 2.45 and 2.52 dB for the +9 dB lift. A moderate one they do not: 1.87 and 1.43 dB for the +6 dB lift, and per frequency Steady's settled cut is 25 to 35% under Fast's (130 Hz 1.52 against 2.01 dB, 200 Hz 1.26 against 1.80, 250 Hz 1.29 against 1.85, 315 Hz 1.10 against 1.60). Steady's median time to 3 dB on the +6 dB lift is 4.55 s at 200 Hz, 10.05 s at 250 Hz and 6.45 s at 315 Hz.

A review switched the same Q 9 bell into four library mixes not used here (Raven_DigicTrail, FIFA_JOURNEY, SPARTAMP_PicV6, 2K_BattBorn; 160 and 280 Hz, +6 and +9 dB, plug-in defaults; FIFA had no 8 s stretch loud enough). Median time to a 3 dB cut: +6 dB Fast 1.67 s, Steady 3.44 s; +9 dB Fast 0.50 s, Steady 1.97 s. But at 280 Hz in the two densest mixes (SPARTAMP, BattBorn) Steady took 10 to 59 s for the +6 dB lift and 4.5 to 11 s for the +9 dB one, and from 6 to 8 s after the switch its cut there was 0.00 dB in five of the eight cases where Fast's was 0.25 to 3.26 dB. Mean cut from 2 to 12 s: SPARTAMP 280 Hz +9 dB, Fast 2.26 and Steady 0.54 dB; BattBorn 280 Hz +6 dB, Fast 0.98 and Steady 0.12 dB. Neither the floor test (`steadyLumpBands` 0: 0.54 / 0.12 dB) nor the parent rule (off: 0.59 / 0.15 dB) is the cause, nor the rise and fall (700/700 or 400/1000: 0.45 to 0.55 / 0.11 to 0.12 dB). The cause is that the steady level averages an excess that sits near the 3 dB dead zone, so a moderate build-up in a dense mix is only sometimes over it; a dead zone of 2 dB for the whole detector gives 0.85 dB. If Steady should match Fast once settled, a Steady-only offset on the dead zone, scanned on the tuning half with the probe at 200 to 315 Hz, is the thing to try; it has not been done.

So Steady is slower to catch a new build-up, typically by one to three seconds, and in a dense mix it may take much longer or leave a mild build-up mostly alone. The window's tooltip says this and promises no figure.

#### Generated signals (mechanics only)

`noise_is_left_alone` at Fast is judged as before and fails as before (Tub and Fog 72.51% of frames under 0.5 dB, mean 0.371 dB; all four 42.24%, 1.204 dB; the resonances 0.15 and 1.58 dB). The same test at Steady (`steady_noise_is_left_alone`, figures from 4 s) also fails, on less: the noise passes with Tub and Fog (100%, 0.000 dB) and nearly with all four (95.67%, mean 0.033 dB, never over 0.63 dB), but the resonances 6 dB proud are cut 0.43 to 0.55 dB at 160 Hz and 2.22 to 2.39 dB at 250 Hz, against the 3 dB asked. At 160 Hz the parent rule is what stops it, at both settings: with it off, Fast cuts the 160 and 250 Hz sines 4.19 and 5.92 dB and Steady 3.55 and 5.92 dB. A +6 dB lift an octave wide at 200 Hz gets no notch at Steady (Fast: 0.37 dB on average, 2.39 dB at the 99th percentile); half an octave wide, 0.63 dB on average against 0.98.

#### Peaks

Narrow cuts shift phase near their centre, and on these mixes that raises sample peaks a little. Per 100 ms window whose input peak is within 6 dB of the mix's highest, the output's peak against the input's (the four honky mixes whole, the twelve library excerpts, 11,041 windows):

| | median | 90% | 99% | highest | windows over +0.5 dB / +1 dB |
|---|---|---|---|---|---|
| Fast | 0.00 dB | +0.23 dB | +0.66 dB | +1.55 dB (honky 2) | 2.6% / 0.1% |
| Steady | 0.00 dB | +0.08 dB | +0.38 dB | +1.26 dB (honky 2) | 0.5% / 0.0% |

The highest sample of each mix rises by +0.44 to +1.55 dB at Fast and +0.27 to +1.26 dB at Steady. With a limiter after it (the reference limiter of 13.6: -1 dBFS ceiling, 5 ms look-ahead, 100 ms release, each mix raised so its untreated peak is at 0 dBFS), the gain reduction averages 0.325 dB untreated, 0.319 at Fast and 0.321 at Steady (De-Honk takes a little level out of the low mids, which pays for the peaks); the deepest moment of limiting goes from 1.00 to 1.80 dB at Fast and 1.58 at Steady, and the share of time limiting from 60.0% to 58.4% and 59.4%. Nothing was changed. What could be done:

- Nothing, as now: the limiter that follows on a master takes the extra in its stride; it costs up to 0.8 dB more limiting at the few highest moments and nothing on average. This is the recommendation.
- A ceiling inside De-Honk that holds the output's peaks to the input's: without look-ahead it can only clip or duck the whole signal for a few milliseconds, which is distortion or a broadband dip on 1 to 3% of the loudest windows; with look-ahead it is a limiter in the plug-in, with the latency De-Honk avoids at Lookahead Off and a second limiter's character before the real one.
- Notches without the phase shift (linear-phase): the peaks would not rise, but at 110 Hz and Q 9 that is tens of milliseconds of latency and pre-ringing.

#### Diff's level match

13.18 left Diff 0.1 to 0.2 dB from the treated output on average over 3 s windows of real mixes and up to 0.8 dB at worst, against the 0.1 dB the generated signal shows. Measured again, Diff against the treated output over 3 s and 0.4 s windows (both channels, input over -40 dBFS, from 5 s), on the four honky mixes whole and two library mixes whole (JFO Reveal, LSEVTRL), at the defaults and with all four ranges at Depth 80% (`.wip/detect/diffm.py`, and `diffsim.py`, which works the gain out offline from the same treated and untreated signals and reproduces the core's to the figure):

- Most of it is lag. The cut follows the programme, so the treated signal's loudness against the untreated one moves by several tenths of a dB from one few seconds to the next; a gain that must not pump follows with a delay, and the worst windows are where the cut changes most. Every causal match has this.
- Under the Threshold the match was held where the last treated passage left it, so Diff stayed turned down where nothing is cut: in the windows under the Threshold Diff was 0.38 dB out on average with all four ranges (0.07 at the defaults), twice as far as in the loud ones.
- Averaging decibels, every moment alike, weighs quiet moments as much as loud ones: worth a few hundredths.

The match is now the ratio of two powers: the treated and the untreated signal's mean square, each averaged with a time constant of `diffMatchSeconds` (now 1.5 s), all the time. Where nothing is cut the two come together by themselves; the gain still moves over 50 ms (`diffGainMs`).

| Over 3 s windows (six mixes) | Defaults: worst, mean | All four, Depth 80%: worst, mean | Gain moves (all four): 99th percentile |
|---|---|---|---|
| Before (dB average over 4 s, held under the Threshold) | 0.37, 0.05 dB | 0.99, 0.22 dB | 0.34 dB/s |
| Power ratio, 4 s | 0.33, 0.05 dB | 0.91, 0.18 dB | 0.76 dB/s |
| Power ratio, 2 s | 0.26, 0.04 dB | 0.85, 0.15 dB | 1.21 dB/s |
| Power ratio, 1.5 s (chosen) | 0.25, 0.03 dB | 0.83, 0.13 dB | 1.44 dB/s |
| Power ratio, 1 s | 0.22, 0.03 dB | 0.76, 0.11 dB | 1.72 dB/s |

Over 0.4 s windows: 1.09 and 0.07 dB at the defaults (was 1.22 and 0.09), 1.99 and 0.27 dB with all four (was 2.56 and 0.34). Under the Threshold the average error with all four is now 0.19 dB (was 0.38). The worst windows improve least; they are lag, and only a shorter average (which moves the gain faster still) or a look-ahead could do better. 1.5 s is the shortest that keeps `solo_honk_identity` inside its limit; that test reads 0.085 dB against 0.1 (0.126 at 1 s, 0.129 at 2 s, 0.110 at 3 s, 0.036 at 4 s: its single 3 s window makes it jump about, as 13.17 said). `automation_clicks` is unchanged (-43.9 dB). The gain Diff applies moves at up to about 1.4 dB a second with everything on (0.35 at the defaults), against 0.34 and 0.11 before; it follows a change of the same size in the treated signal, so the comparison stays fair.

#### What has not changed

At Detection Fast, with Diff not engaged, the core gives what the core before this section gave: `dehonk_golden_ref4` (that core, copied to `Tests/build-detect/ref-dsp4/`) against `dehonk_golden` with `--surround` (100 renders), `--compat --surround` (100) and `--defaults-no-diff` (124): 324 of 324 bit-identical on arm64 and on x86_64. `golden_defaults_*` was recorded again because the offset Diff reports is in every frame (133 of 140 hashes moved; the 7 with no cut did not). `golden_ref_*` and `golden_surround_*` were not touched.

#### Parameter, preset and window

- `detection`, a choice (Fast, Steady; default Fast), last in the host's order with version hint 2 like everything else added since the first test build. Older sessions and presets without it load at Fast. `DeHonkHostCheck` passes (the 22 first-build parameters at hint 1, the 16 added since higher).
- A seventh factory preset, "Master bus, steady": the defaults at Steady. It is last, so the host's program numbers of the other six do not move.
- In the window a two-part pill, fast and steady, as Link's, sits under Sharpness in a row of its own below the knobs, with its name to its left. The pond is 24 points shorter to make room.

#### Cost

Percent of one core at 48 kHz (`surround_cpu`, the better of blocks of 128 and 1024, run on its own), Fast / Steady: defaults stereo 0.36 / 0.38, 7.1.4 0.83 / 0.85; all four and Snap always, linked, stereo 0.64 / 0.63, 7.1.4 1.21 / 1.23; the same unlinked (every channel its own detector, the heaviest), stereo 0.86 / 0.86, 7.1.4 3.82 / 3.75. At 96 kHz the heaviest 7.1.4 is 5.00 / 4.74. The difference is inside the run-to-run spread: the steady level is a few hundred simple operations a millisecond per detector.

#### Tests

New: `steady_noise_is_left_alone` (fails, as above) and `steady_detection` (passes: both peaks found at 1, 2, 6 and 12 channels, Unlinked exact against mono, Grouped front pair exact against stereo, block sizes exact, switching every 0.7 s and every 60 ms under -54 dB above 6 kHz against the -40 limit). Extended: `automation_clicks` and `no_allocation_in_process` and the stability tests switch Detection; `surround_cpu` has three Steady rows; `noise_is_left_alone` prints the parent-rule-off and broad-lift figures. `dehonk_render --detection fast|steady`.

### 13.20 After review of 13.19 and of the window's drawing (18 November 2026, last of the day)

- **CI.** `steady_noise_is_left_alone` fails on purpose (13.19) and is now on both non-blocking lists of `build-release_dehonk.yml`.
- **Detection's tooltip** promises no figure: Steady "waits until a build-up has held for a while … slower to catch a new build-up, and it may leave a mild one alone in a dense mix". 13.19's price paragraph is corrected (a moderate lift is cut 25 to 35% less once settled, and in dense mixes can take tens of seconds or stay mostly uncut). The Steady-only dead-zone offset it names has not been tried.
- **Steady's re-prime.** A slot's slow average is only fresh if its detector ran at the last tick. A tick stamp per slot (`slowStamp`) now says so, and a slot whose average is stale (a channel that was not its own detector while Linked) starts its steady level from the band powers now. Fast is untouched: `dehonk_golden_ref4` against `dehonk_golden` (`--surround`, `--compat --surround`, `--defaults-no-diff`), 324 of 324 bit-identical on arm64.
- **Host check.** Both restore loops set Detection to Steady first and require Fast after an old state; an early-0.0.2 state (stateVersion 2, no `detection`) and a user preset file from before Detection (loaded through the new `PresetManager::loadPresetFile`, from a temporary folder) both come back at Fast.
- **Band view.** It is opaque, and draws itself with JUCE's own renderer into one image at the screen's scale: the window's background behind it (the editor's own paint) and everything that stays put are in a backdrop made only when something in it changes; the bars, cuts, lines and readings are drawn over a copy of it at each picture. Measured with `sample` on the main thread over 5 s, busy, every range on, a 960x800 window at 2x: the CoreAnimation commit took 50.7% of the main thread before and 19.5% after (7.4% with the band view hidden); at 1800x1400, 76% (the review's figure) to 42.6% (23.2% hidden: what is left is mostly CoreGraphics converting the pond's picture). Process CPU at the default size 461 to 391 ms/s. Followers stay at every picture.
- **Resizing.** For the refresh after a size change the old picture is drawn as it is, at its own scale, over paper, instead of being resampled to fit. Resizing up to every refresh for 4 s: 1384 ms/s of CPU, gaps median 31 ms and 99th percentile 70 ms (the review measured 5153 to 5304 ms/s and 475 to 482 ms before).
- **Refreshes are counted.** At the full rate a picture goes with every refresh (every other one only above about 150 Hz) as long as a third of a refresh has passed, instead of a fixed 6.5 ms gap. With the clock driven at 120 Hz: 119.9 pictures a second (was 104.5), 111.6 under message-thread load from 1118 ticks.
- **Several windows.** The display frames are a ring that any number of windows read, each from its own place (section 4). Three windows on one instance: 355, 356 and 361 frames each in 8 s, all three at 60 pictures a second (was 86, 68 and 406 frames, 47.6 to 60 pictures a second).
- **Trim's value** no longer sits on a paper patch: each letter has a thin paper edge, so the whole fill shows at small trims.
- **Geese.** A goose taking off holds its place in the spacing until it is half way up, so the others do not walk into it; a goose hopping in is placed with the rest and lands where it will stand; the two rows behind the log, being close together, are spaced nearly as one row; a goose with far to go walks a little faster. On the recorded busy frames (1800 pictures), with a wider count that includes landing and leaving geese: at 1868x588 (the default picture) 92 to 88 pictures with two on top of each other (57 to 58 counting only geese on the ground), at 934x331 70 to 51 (35 to 14), at 333x187 65 to 35 (30 to 0). At the default size what is left is crowding at the log end of the bank, where there is not room for every goose within reach of its notch, and geese swapping rows when one joins a run.
