# Experiment C: intent and picture (describe the whoosh you want instead of turning knobs)

Status: plan only. No source files changed.
Owner theme: describe the target (words, a reference sound, a vocal sketch, a picture), and let the tool find the knob settings.

---

## 0. What the codebase gives us

- `Source/Parameters.h`: 30 automatable parameters. About 17 of them are "sound design" continuous parameters, the same set that `Presets::roll()` touches: length, shape, speed, distance, pitch, chaos, air, roar, flutter, sub, tone, sweep, swirl, weight, grit, punch, space. The rest are mode/direction choices, session parameters (input, cargo1-4) and master parameters (inflate, push, ceiling), which `Presets::isSessionParameter()` already excludes.
- `Source/Presets.h`: a `Presets::Preset` is a `{name, map<id, plain value>}`. `PluginEditor::applyPreset(const Presets::Preset&, bool includeMode)` (declared in `Source/PluginEditor.h:105`) glides the parameters to it on springs. **Anything this experiment produces is just a `Preset`**, so the output needs no new audio-thread code.
- `Source/WhooshEngine.h`: `WhooshEngine::Params` is a plain struct, and `trigger()`, `process()` and `prepare()` work without a UI. The RNG is reseeded to `0xC0FFEE` in `prepare()` (`WhooshEngine.cpp:95`), so renders are deterministic. Repeated `trigger()` calls in one render give different chaos draws, which covers seed variation without an engine change.
- `Tests/RenderTest.cpp`: the `render()` helper already drives the engine plus `MasterProcessor` offline at block size 256 and writes WAVs. The new harness copies this pattern.
- `TapeRecorder` (Captain's Log) records the output and the retro buffer. It does **not** store which parameters produced a sound. That gap matters for the backup experiment and for preference learning (section 5).

---

## 1. Survey (latest relevant work, with licences where they matter)

**Text-guided synth programming (closest prior art)**
- CTAG: "Creative Text-to-Audio Generation via Synthesizer Programming" (Cherep, Singh, Shaw, ICML 2024). https://arxiv.org/abs/2406.00294. A 78-parameter virtual modular synth, optimised against LAION-CLAP (audioset-best checkpoint) with the LES evolution strategy (evosax). Prompts need 50 to 300+ iterations, taking 5 to 97 s on a V100. Its sounds score higher on CLAP than real ESC-50 recordings, a sign that the score can be reward-hacked.
- SynthScribe (2023): LAION-CLAP text and audio search over synth sounds. https://arxiv.org/abs/2312.04690
- Neural Proxies for Sound Synthesizers (Combes et al., JAES 2025): maps presets into a pretrained audio-embedding space, a learned surrogate for "render then embed". https://arxiv.org/abs/2509.07635
- FM-SynAPSE (DAFx 2026): joint audio/parameter embeddings (SLAP objective) for DX7 preset retrieval. https://arxiv.org/abs/2608.18226
- INSTRUMENTAL (2026): CMA-ES beats gradient descent at recovering 28 synth parameters from audio, on a non-convex landscape. https://arxiv.org/abs/2603.15905
- Huang et al., "Active learning of intuitive control knobs for synthesizers" (2014): GP plus active learning for "scary"-style controls. https://kgajos.seas.harvard.edu/papers/huang14knobs.pdf

**Text-audio embedding models (the scorer)**
- LAION-CLAP. Code: https://github.com/LAION-AI/CLAP (CC0-1.0). HF `laion/larger_clap_general` is tagged Apache-2.0 (https://huggingface.co/laion/larger_clap_general). The training data is mixed-licence (AudioSet, Freesound and others) and the full set was not released.
- MS-CLAP 2023: https://github.com/microsoft/CLAP. HF weights are tagged MS-PL (https://huggingface.co/microsoft/msclap), which permits commercial use. Verify the LICENSE file before shipping anything derived from it.
- Licence stance: we use CLAP **only at development time or as an optional offline step**. What ships is parameter presets and, optionally, a small table of embeddings. Shipping the model weights is not required for the primary experiment. Even so, review the Apache-2.0 tag and the training-data provenance before any weights go inside the installer.

**Time-varying control from imitation or video**
- Sketch2Sound (Adobe/Northwestern, 2024/25): controls a DiT text-to-audio model with loudness, spectral centroid and pitch (CREPE-tiny) curves taken from a vocal imitation, median-filtered to tolerate sloppy imitations. https://arxiv.org/abs/2412.08550
- Video-Foley (2024, v3 2025): video to frame RMS to sound. Frame-level RMS is the temporal condition, with no annotation needed. https://arxiv.org/abs/2408.11915
- MMAudio (CVPR 2025): joint video/text-to-audio with a Synchformer sync module. HF weights tagged MIT. https://arxiv.org/abs/2412.15322, https://huggingface.co/hkchengrex/MMAudio
- HunyuanVideo-Foley (2025): Synchformer-based timestep alignment (secondary source). https://www.emergentmind.com/topics/hunyuanvideo-foley
- FoleyCrafter (2024), https://arxiv.org/abs/2407.01494, and V-AURA (2024), https://arxiv.org/abs/2409.13689: onset/timestamp-conditioned and autoregressive visually aligned audio.
- Observation: every video-to-audio system above *generates waveforms* with a large model. None of them *drives an interpretable, physical sound engine*. Swooshbuckler's flight model (one point source on a straight line past the listener) means a tracked motion path *is* the parameter set.

**Preference learning / interactive evolution**
- Preferential Bayesian optimisation: Consecutive PBO (2025), https://arxiv.org/abs/2511.05163; constrained PBO for designers (IJCAI 2025), https://arxiv.org/abs/2505.10954; Preferential multi-objective BO, https://arxiv.org/abs/2406.14699; GimmBO (2026), PBO for image model merging, https://arxiv.org/abs/2601.18585
- I found no PBO work for synth or whoosh design. The gap is real, but the tape log does not yet record parameters (see section 5).

---

## 2. Primary experiment: "Hail the Parrot". Text to whoosh by CLAP-guided, seed-robust CMA-ES, distilled into a no-ML-at-runtime atlas

### Hypothesis
H1. A short text prompt ("a heavy iron cannonball passing close, left to right", "a ghostly airy swish far away") can be turned into a Swooshbuckler preset by black-box optimisation of the 17 design parameters. The optimisation scores *the expected* CLAP text-audio similarity over several chaos draws, with a domain anchor and a guard against cross-model reward hacking. In blind tests, listeners should rate the result as matching the prompt better than (a) the best factory preset for that prompt and (b) the best of 16 Roll-the-Bones rolls.

H2. The expensive search can be *distilled*. A precomputed atlas of about 20k rendered settings plus their audio embeddings answers text queries in under 50 ms, with no ML model in the plugin: it uses precomputed text embeddings for a fixed vocabulary of descriptors and their combinations. It should reach at least 85% of the full-search CLAP score.

### Why this is new for whoosh design
- CTAG did text-to-synth on a generic modular synth. Nobody has done it on a **physically parameterised flight engine**, where speed, distance and shape are Doppler physics rather than abstract synth knobs. Prompts with spatial or motion language ("close", "far", "fast", "sucking in") then have a direct physical target.
- CTAG reports CLAP scores *above real recordings*, a sign of reward hacking. Here we optimise against LAION-CLAP and **validate with MS-CLAP** (a different model with different training data) and with humans. That gives a measured answer to whether CLAP-guided synth search generalises.
- **Seed-robust objective.** Swooshbuckler's `chaos` makes every flight different. We optimise E[score] over K flights (minus a variance penalty), so the preset reliably produces the described whoosh rather than one lucky render. CTAG optimises a single deterministic render.
- **Distillation into a shippable lookup**: commercial-friendly, laptop-instant, no model weights in the installer.

### Algorithm (exact)

**Search space.** x ∈ [0,1]^17, mapped to plain values:
- length: log-mapped over 0.1..8 s
- shape: -1..1
- pitch: -24..24 st, rounded to 0.5 st
- the 14 percentage parameters: identity

The mode is fixed to One-shot during search. Direction is set from the prompt by keyword (port/starboard/left/right), otherwise Either. Session and master parameters are excluded (`isSessionParameter`), master at defaults.

**Render of a candidate** (C++ harness, section 2.4).
- Run engine and `MasterProcessor` exactly as in `RenderTest::render()`.
- Trigger K = 3 flights at t = 0, T, 2T, where T = length + 1.5 s, with space tails included.
- Cut 3 clips of length min(T, 10 s). Resample to 48 kHz mono for CLAP (LAION-CLAP expects 48 kHz).
- The repeated triggers give different chaos draws without an engine change.

**Objective** (to maximise):
```
s_i   = cos(E_a(clip_i), E_t(prompt))                      # LAION-CLAP 630k-audioset-best
a_i   = cos(E_a(clip_i), E_t("a whoosh sound effect"))     # domain anchor
n_i   = max_j cos(E_a(clip_i), E_t(neg_j))                 # optional negative prompts, e.g. "music", "speech", "distorted noise"
J(x)  = mean_i(s_i) - 0.5*std_i(s_i) + 0.2*min(a_i, 0.25) - 0.3*max(n_i - 0.2, 0)
        - 1.0*[peak/rms sanity fail]                       # silence or DC: hard penalty
```
- Prompt augmentation: average E_t over 4 templates ("{p}", "the sound of {p}", "a whoosh of {p}", "{p} sound effect") to reduce prompt-wording overfitting.

**Optimiser.** CMA-ES (`cma` package, BSD-3) with:
- λ = 16, σ0 = 0.25, at most 40 generations (640 candidates × 3 clips)
- bounds handled by `BoundTransform`
- start x0 = the top-1 atlas hit for the prompt (warm start, section 2.2.H2), else the parameter defaults
- restart once with IPOP if it stalls for 8 generations
- keep the top 5 distinct results (L2 > 0.15 in x)

**Laptop cost estimate** (to confirm in the prototype).
- Rendering: about 6 s of audio per candidate at an assumed 100x realtime is about 60 ms, so a generation of 16 takes about 1 s.
- CLAP HTSAT on CPU is roughly 30 to 80 ms per clip, about 2 to 4 s per generation batched.
- About 40 generations comes to roughly 2 to 3 minutes per prompt. That is acceptable as an "offline, go make tea" mode, and it is why H2 exists.

**Atlas (H2).**
1. Sample N = 20,000 x by Sobol, 50%, and by `Presets::roll()`-distributed draws, 50%. The second half keeps the atlas inside the musically safe region.
2. Render each x as one K = 3 clip set.
3. Store the mean CLAP audio embedding (512-d) and reduce it to 64-d by PCA, fit on the atlas: 20k × 64 × float16 is about 2.5 MB.
4. Precompute CLAP text embeddings, projected with the same PCA, for a vocabulary of about 1,500 whoosh descriptors: materials (iron, cloth, feather, glass), sizes, speeds, distances, moods ("ghostly", "aggressive"), motion words ("swipe", "pass-by", "suck", "rise"), and source nouns (sword, arrow, cannonball, jet, bird, wind, ghost). Mined from the factory preset names, sound-effects library naming conventions and the CTAG prompts.
5. At runtime the prompt is tokenised and matched to vocabulary entries (exact word, then stem, then fuzzy match). The query embedding is the normalised mean of the matched entries' embeddings, with negation words ("not", "without") subtracting. The result is the top-k cosine against the atlas.
6. Return the best x, plus a "Nearby" list of 4 alternatives for exploration.

Everything in steps 5 and 6 is a dot product over 20k × 64: well under 1 ms.

### Where it plugs in
- **New test target (dev only):** `Tests/ProbeRender.cpp` becomes a console app `SwooshbucklerProbe`, added to `CMakeLists.txt` next to `SwooshbucklerRender` with the same `${SWOOSHBUCKLER_DSP_SOURCES}`. It reads a JSONL of candidates (`{"id":..., "x":{paramId: plainValue}}`) and writes `<id>_<k>.wav`, or raw float32 to stdout to skip disk I/O, plus a peak/RMS line per candidate. Batch mode lets one process render a whole CMA-ES generation, about 16 renders per call.
- **Python orchestration (dev only):** `tools/parrot/` contains `search.py` (CMA-ES plus CLAP scoring), `build_atlas.py`, `eval_crossmodel.py` and `export_preset.py`. The export writes C++ `Presets::Preset` initialisers or a JSON preset file.
- **Plugin (phase 2, after success):**
  - Add `Source/ParrotAtlas.h/.cpp`: it loads a BinaryData blob (atlas x-table, PCA'd audio embeddings, vocabulary text embeddings) and implements `Presets::Preset query(const juce::String& prompt, int rank)`.
  - The UI is a text box ("Hail the Parrot") in the editor. On Enter it calls `applyPreset(atlas.query(text, 0), false)`, so the result glides in on the existing springs.
  - Message thread only. The audio thread is untouched.

### One-day prototype (one engineer)
1. **(2 h)** Write `Tests/ProbeRender.cpp` by copying `render()` from `RenderTest.cpp`. Add `--batch candidates.jsonl --out dir --k 3`, map JSON keys onto `WhooshEngine::Params` (levels array indices: Air, Roar, Flutter, Sub, Tone) and add the CMake target. Measure realtime factor on the laptop.
2. **(1 h)** Write `tools/parrot/clap_score.py` using `laion_clap` (CC0 code) with checkpoint `630k-audioset-best.pt` and batched CPU inference. Sanity-check that the factory preset "Kraken Rise" scores higher for "a huge monster rising from the deep" than "Parrot Flap" does.
3. **(2 h)** Write `search.py`: cma.CMAEvolutionStrategy with the objective above, subprocess calls to `SwooshbucklerProbe --batch`, and logging of every generation to CSV.
4. **(2 h)** Run 12 fixed prompts:
   - 4 that map onto factory presets: swipe, cannonball, ghost, jet
   - 4 novel: "a paper airplane gliding past", "a swarm of bees zipping by", "an angry metal blade spinning close", "a soft distant gust in a cave"
   - 4 spatial: "very close and fast left to right", "far and slow", "sucking in towards me", "a long rising drone"

   For each, also score all 11 factory presets and 16 rolls.
5. **(1 h)** Write `eval_crossmodel.py`: re-score every winner and baseline with MS-CLAP 2023. Write a WAV contact sheet and an HTML page for blind listening. Skip the atlas today, or build a 2k mini-atlas overnight.

### Success criteria
- **S1 (search works):** on at least 10 of 12 prompts, LAION-CLAP J(search) exceeds max J(factory) and max J(16 rolls).
- **S2 (not reward-hacked):** on at least 8 of 12 prompts, MS-CLAP also ranks the search result first among search result, best factory preset and best roll. The Spearman correlation between LAION and MS-CLAP scores over all evaluated candidates must be at least 0.4. If it is lower, CLAP guidance is not trustworthy for this engine and we report that.
- **S3 (humans agree):** blind 3-way forced choice (search vs best factory vs best roll, "which matches the text?"), 5 listeners × 12 prompts. Search wins at least 50% of trials, against a chance level of 33%, at p < 0.05 by binomial test.
- **S4 (physics is legible):** for the spatial prompts, the parameters move the right way. "close" gives distance < 0.3, "far" gives distance > 0.6, "fast" gives speed > 0.7, and "sucking in" gives shape > 0.33. Expect at least 3 of 4 correct.
- **S5 (cost):** median search at most 3 minutes per prompt on the laptop CPU.
- **S6 (H2, phase 2):** atlas top-1 reaches at least 85% of the search J on the 12 prompts, with query latency under 5 ms in C++.

**Kill criteria:** S2 fails, or the human win rate is no better than chance. In that case, fall back to the backup, which needs no ML.

---

## 3. Backup experiment: "Shout the Heading". Vocal sketch or reference whoosh to preset, by trajectory matching (no ML, fully commercial)

### Hypothesis
A designer can say "shhhh-WOOOM" into a microphone, or drop in a reference whoosh. Matching three time-varying descriptors (A-weighted loudness, spectral centroid and pitch, as in Sketch2Sound) plus a stereo-pan trajectory recovers length, shape, speed, distance, pitch, sweep and the source balance well enough that the rendered flight follows the sketch's contour. Target correlation is at least 0.8 on loudness and at least 0.6 on centroid. This uses deterministic DSP features only, with no licence questions.

### Why new
Sketch2Sound feeds these curves into a diffusion model. Here the same curves are **inverted into an interpretable physical flight**: the loudness peak time gives the attack/release split (shape), the curvature gives the envelope exponents (length), the centroid swing gives speed and Doppler, the absolute centroid gives distance (air absorption), and the pan slope gives direction. The result is editable knobs, not a frozen waveform.

### Algorithm
1. **Features of the target**, at 100 frames/s: L(t), the A-weighted RMS in dB; C(t), the log spectral centroid; P(t), the YIN pitch, confidence-gated (pYIN in librosa, ISC licence); and for stereo references, the pan trajectory D(t) = (L−R)/(L+R) energy difference.
   - Median-filter each curve with 5- and 15-frame windows.
   - Trim to the region within 40 dB of the peak, and time-normalise around the loudness peak.
2. **Analytic initialisation:**
   - length = trimmed duration
   - shape is taken from the peak position: peak at 20% of the duration means a swipe (negative shape), at 80% a suck (positive)
   - pitch = median P relative to the engine's default centre
   - direction = sign of the D(t) slope
3. **Refinement:** CMA-ES over 9 parameters (length, shape, speed, distance, pitch, sweep, air, roar, flutter), with
   ```
   loss = 1 - corr(L) + 0.5*(1 - corr(C)) + 0.3*|meanC - meanC*| + 0.3*(1 - corr(D))
   ```
   using DTW-free alignment, because the curves are already peak-aligned. λ = 12 for 25 generations, with K = 1 render at chaos fixed at 0 during the fit, then restored.

   Cheap: about 300 renders of about 3 s each takes about 10 s. This can run **in the plugin** on a background thread using the engine directly, with no Python.
4. **Plug-in point:** a "Shout" button records 4 s from `input`, the existing input path in `PluginProcessor`, or accepts a dropped WAV the way `CargoHold::loadFile` does. A `juce::Thread` runs the fit on a private `WhooshEngine` instance, then calls `applyPreset()`.

**One-day prototype:** Python features plus `cma`, using the same `SwooshbucklerProbe --batch`. Test set:
- 10 reference whooshes rendered *by the engine itself* from known random parameters. Parameter recovery is the ground truth: aim for normalised parameter error under 0.15 on length, shape and distance.
- 10 recorded vocal sketches.

**Success criteria:**
- loudness correlation at least 0.8 and centroid correlation at least 0.6 on 8 of 10 vocal sketches
- self-recovery error under 0.15 on the core parameters
- fit time under 15 s on CPU

---

## 4. Follow-ons, not tested in this experiment

- **Picture to flight** (video): dense optical flow (OpenCV Farneback, Apache-2.0) on a clip, then the dominant moving blob's track. Closest-approach frame gives the trigger time, path length/speed gives length and speed, apparent size at closest approach gives distance, and screen x-trajectory gives direction. This yields a timed list of one-shot triggers plus per-flight parameters to render or print to tape. It uses no generative model and is frame-accurate by construction. Compare its sync against MMAudio's Synchformer-based DeSync metric.
- **Taste learning:** to learn from keep/discard, the tape log must first record the parameters at each retro save. That means adding a sidecar JSON next to the WAV written by `TapeRecorder::saveRetro` and the rolling record (proposed, not done). Then fit a GP/Bradley-Terry preference model ("saved" beats "heard but not saved" within a session) and bias `Presets::roll()` by sampling from the preference-weighted posterior (PBO with Thompson sampling).
- **Latent map UI:** a 2-D UMAP of the atlas embeddings becomes a clickable "sea chart" whose islands are labelled by their nearest vocabulary word.

## 5. Risks
- **CLAP is weak on very short sounds (< 0.5 s) and on spatial words.** S4 and S2 test this directly. Mitigation: the backup does not depend on it.
- **Mono CLAP ignores stereo,** so direction comes from keywords, not from scoring.
- **Licences:** dev-time use of LAION-CLAP and MS-CLAP is low-risk. Before shipping any embeddings derived from them, have the Apache-2.0 / MS-PL terms and the training-data provenance reviewed. The backup and the optical-flow follow-on carry no model licence risk.
