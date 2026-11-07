# Experiment 02: auto-classify and auto-name Cargo Hold samples

Status: research and plan only. No source changed yet.
Date: 2026-11-06

## Goal

When a sample is dropped on a Cargo lane, Swooshbuckler should:

1. Work out what **role** the sound plays in a whoosh: energy bed, tonal sweetener, impact, and so on.
2. Use that role to pick **engine behaviour** for the lane, such as looping, aligning to the peak or firing as a one-shot.
3. Give the lane a **readable name**, e.g. `Pig Squeal · Tonal`.

Two decisions shape everything below:

- **Role is acoustic, name is semantic.** How a lane should behave depends on what the audio *does*: sustained or not, pitched or not, transient or not, low or not. It does not depend on what the audio *is*. A noisy pig squeal behaves like a swish. So hand-written DSP features always decide the role. ML only names the sound, and at most nudges the role.
- **Ship in tiers.** Tier 0 is DSP plus filename hints, with no dependencies, and it does the useful part (role and behaviour). Tier 1 adds a small MIT-licensed tagger through ONNX Runtime for better nouns. Tier 2, CLAP zero-shot against a curated sound-design vocabulary, is an optional extra.

---

## 1. Role taxonomy and engine handling

Seven roles, plus `Generic`, which is today's behaviour and the fallback when confidence is low. This list is deliberately small: each role must map to a different engine behaviour, or it doesn't earn a place.

| Role | Typical sources | Acoustic signature | Engine handling (proposed) |
|---|---|---|---|
| **Bed** (energy bed / sustained texture) | wind, room tone, rain, air con, drones, steam | long (> 3 s active), high sustain ratio, flat-ish envelope, broadband or mid flatness, few onsets | Continuous gapless loop (current CargoPlayer with skip-silence on). Feeds the `bed` path more than the flights. CHAOS shuffles regions freely. Little doppler pitch (rate weight about 0.3). |
| **Tonal** (sweetener: animal, vocal, creature, whistle, squeal, scream) | pig squeal, monkey, horse, scream, whistle, bowed metal, synth sweep | high pitch clarity (YIN), harmonic, voiced ratio > 0.4, often an f0 glide | **One-shot per flight, aligned to the peak.** Choose the region with the best pitch clarity, compute its *focus point* (max loudness × clarity), and start it so the focus point lands on the flight peak. Full doppler pitch (rate weight 1.0). Don't randomise the start mid-region; CHAOS only picks which tonal region plays. |
| **Drone/Engine** (sub-type of tonal) | jet, motor, turbine, servo, hum | pitched with harmonics, long, steady f0, periodic AM (2–40 Hz) | Loop like a bed, but with full doppler and the pitch following. This is the "jet-by" layer. |
| **Impact** | hit, slam, punch, boom, gunshot tail, metal clang | short active audio (< 0.8 s per event), crest > 15 dB, attack < 10 ms, 1–2 onsets | One-shot fired at `peakFired` (the same moment as `fireHit`), pre-rolled so the onset lands on the peak. No looping. Excluded from the bed. |
| **Swish/Air** | cloth flap, sword swipe, stick whoosh, flag, bird wing | noisy (flatness > 0.25), mid–high centroid, smooth single-bump envelopes of 0.15–1.5 s | Per-flight one-shot, time-scaled to the flight's attack + release: the region's envelope peak sits on the flight peak and the rate is stretched to fit. Strong doppler candidate. |
| **Crackle/Granular** | fire, gravel, electricity, sparks, debris, sizzle, paper | high onset density (> 8/s), high flatness, high crest per grain | Granular: short grains from random regions, density following the flight envelope, sprinkled around the peak. CHAOS sets the grain scatter. |
| **Rumble/Low** | thunder, earthquake, sub drop, distant traffic, LFE | > 60 % of energy below 150 Hz, centroid < 250 Hz | Routed toward WEIGHT/Sub: low-pass, half the doppler pitch weight (low pitch shift sounds fake), level boost at the peak. |
| **Generic** | anything with confidence < 0.45, and all old sessions | – | Exactly today's CargoPlayer behaviour. |

Each lane gets a role drop-down: `Auto (Tonal 82 %)`, then each role, then `Generic`. A user choice sticks and is never overwritten by re-analysis.

### Where this plugs into the current engine

- `WhooshEngine::renderCargo` already renders every lane continuously into `m_layerLines`, and flights read those lines with doppler delays (`renderFlights`). Bed, Drone and Generic keep that path unchanged.
- Tonal, Swish and Impact need **cue scheduling**. In `spawnFlight`, for each lane with a one-shot role, the engine pushes a cue `{lane, startClock, regionIndex, focusOffset, rateScale}` so the focus point arrives at `spawn + attack` (plus `m_baseDelay` compensation). `CargoPlayer` gains a `triggerOneShot()` path driven by these cues; heads are already a 2-voice crossfade pair, so allow 2–4 one-shot voices per lane.
- Impact can reuse the `peakFired` hook in `renderFlights`, which already calls `fireHit(f)`.
- The audio thread reads the role from a per-lane `std::atomic<uint8_t>`. Per-region analysis (`focus`, `clarity`, `role`) goes inside the immutable `CargoData`, so it is published with the existing atomic pointer and graveyard scheme.

---

## 2. Options compared

### (a) Hand-crafted DSP features plus rules or a tiny classifier (Tier 0, recommended base)

Compute on a 16 kHz mono copy, frame 1024 / hop 256, over the frames inside the detected `regions` only (the existing gated-RMS detection does the silence removal for free):

| Feature | Use |
|---|---|
| Spectral flatness (median, IQR) | noisy vs tonal |
| Spectral centroid median, 85 % rolloff | bright/dark, swish vs rumble |
| Low-energy ratio (< 150 Hz / total) | rumble |
| YIN clarity (1 − min CMNDF), voiced ratio (clarity > 0.8), f0 median and range in semitones | tonal, glide/sweep, drone (steady f0) |
| Harmonicity / HNR (normalised autocorrelation peak) | tonal vs noisy-pitched |
| Onset density (spectral-flux peaks per active second) | crackle vs bed |
| Crest factor (dB), attack time 10 → 90 %, decay T20 of the loudest event | impact |
| Active duration, region count, mean region length | impact/swish vs bed |
| Sustain ratio (frames within 6 dB of the 90th-percentile level / active frames) | bed |
| Envelope modulation spectrum peak in 2–40 Hz | drone/engine, flutter |

**Reuse:** `../subbertone/Source/PitchDetector.{h,cpp}` already exists in this monorepo. Copy or adapt its YIN core rather than writing a new one. `juce::dsp::FFT` covers the STFT.

**Classifier:** start with soft-scored rules (below), then fit a depth-3 or depth-4 decision tree or a logistic regression in Python on the labelled set and export it as plain C++ `if`/`else` code or about 100 weights. There is no runtime dependency, it adds about 0 KB, and it takes about 20–60 ms of CPU for 10 s of audio and about 1–2 s for the 600 s maximum (so cap it, see §4).

Starting rules. Each role gets a score in [0, 1] as a product of sigmoids around these thresholds, and the argmax wins. Confidence is the top score minus the second.

```
Impact   : activeDur/region < 0.8 s  AND crest > 15 dB AND attack < 10 ms AND onsets <= 2/region
Rumble   : lowRatio > 0.6 AND centroid < 250 Hz
Tonal    : voicedRatio > 0.4 AND clarityMedian > 0.75 AND hnr > 6 dB
  Drone  :   + activeDur > 3 s AND f0 range < 2 st AND sustain > 0.6
  (sweep tag if f0 range > 5 st, used in naming only)
Crackle  : onsetDensity > 8 /s AND flatness > 0.3
Bed      : activeDur > 3 s AND sustain > 0.6 AND onsetDensity < 4 /s
Swish    : flatness > 0.25 AND 0.15 s < meanRegionLen < 1.5 s AND single-bump envelope
else     : Generic
```

Expected accuracy: roles like these are acoustically separable, so a good rule set should reach about 75–85 % macro-F1 on the in-house set (estimate, to be measured; see §5). It can't name the source ("pig" vs "monkey"). Filename hints cover a lot of that in practice, because SFX libraries are named carefully.

### (b) Pretrained AudioSet taggers (Tier 1, for naming)

| Model | Params / file | AudioSet mAP | Code licence | Weights licence | C++ path | Verdict |
|---|---|---|---|---|---|---|
| **EfficientAT mn04_as / mn05_as** (MobileNetV3, distilled from PaSST) | 0.98 M / 1.43 M, about 4–6 MB fp32, about 1.5 MB int8 | 43.2 / 44.3 | MIT | released with the MIT repo (no separate licence stated; confirm with the author) | export ONNX yourself (`torch.onnx.export`, standard conv ops) → ONNX Runtime | **Recommended.** Best accuracy per MB, MIT. mn10_as (4.88 M, 47.1) if size allows. |
| **YAMNet** (MobileNetV1) | 3.7 M, about 15 MB | about 0.31 (weaker) | Apache-2.0 | Apache-2.0 | official TFLite; ONNX conversions on HF | Safe fallback with the simplest licence. Its fixed 0.975 s frame suits per-region tagging. |
| **CED-Tiny / Mini** | 5.5 M / 9.6 M | 48.1 / 49.0 | **GPL-3.0** (GitHub repo) | Apache-2.0 (HF `mispeech/ced-tiny`) | ONNX export script in the repo | Very accurate for its size, but the code is GPL-3.0. Only use it if you can export from the Apache HF checkpoint without GPL code in the shipped binary. **Needs legal review; not the default.** |
| PANNs MobileNetV1 / CNN14 | 4.8 M (23.6 MB) / 80 M (327 MB) | 38.9 / 43.1 | MIT | CC BY 4.0 (attribution needed, commercial OK) | ONNX export | Outclassed by EfficientAT at the same size; CNN14 is too big. |
| AST, BEATs, EAT, Dasheng (large SSL/transformer) | 85–600 M | 45–50 | BSD-3 / MIT / various | various | heavy | Overkill for a plugin; skip. |
| Apple SoundAnalysis built-in (`SNClassifierIdentifier.version1`) | 0 (ships with the OS) | n/a, 303 labels | Apple SDK | – | Obj-C++ in a `.mm` file | macOS only, so Windows behaves differently. Useful as a no-download Mac bonus or a dev-time labelling helper, not as the main path. |
| Essentia TF models | small | – | AGPL | CC BY-NC-SA (verify) | – | **Avoid** (non-commercial). |

**Runtime:** ONNX Runtime (MIT). Use a **minimal build with only the needed operators, loading a `.ort`-format model** (built with ort-builder). That keeps it to a few MB per architecture and drops protobuf, which avoids the protobuf symbol clashes reported in JUCE plugins. The universal binary doubles the size. Alternatives:

- TFLite C API: fine for YAMNet.
- RTNeural: no good 2-D conv/MobileNet support; it's built for small real-time RNN/MLP models, so it doesn't fit here.
- libtorch: over 100 MB, no.
- anira: built for *real-time* inference. This job is offline on a worker thread, so the plain ORT C++ API is simpler.

Set intra-op threads to 1 to be polite in a DAW. Create the session lazily and share it across plugin instances with `juce::SharedResourcePointer`.

**Front-end trap:** EfficientAT expects 32 kHz log-mel (128 bins). Put the mel front-end *inside* the exported graph (export the wrapper module, opset ≥ 17, which supports STFT), so the C++ side only resamples to 32 kHz and feeds raw audio. Then check logits against Python on 20 files (max abs diff < 1e-3).

**Labels:** the AudioSet *dataset/labels* are CC BY 4.0, but the *ontology* is **CC BY-SA 4.0**. Don't ship ontology text. Ship our own `cargo_vocab.csv` (`classIndex, displayName, rolePrior, weight`): curated display names for about 150 of the 527 classes that matter to whoosh design. Mark it as derived from AudioSet with attribution in the About box and the README. If the mapping counts as a derivative, license that one data file CC BY-SA. That's a small cost; check with a lawyer if it matters.

Mapping examples: `Pig/Oink → "Pig" (Tonal)`, `Roar/Growling → "Growl" (Tonal)`, `Whoosh, swoosh, swish → "Swish" (Swish)`, `Flap → "Flap" (Swish)`, `Fire/Crackle → "Fire" (Crackle)`, `Thunder/Rumble → "Thunder" (Rumble)`, `Jet engine → "Jet" (Drone)`, `Wind → "Wind" (Bed)`, `Bang/Slam/Thump → "Hit" (Impact)`.

### (c) CLAP zero-shot naming (Tier 2, optional)

- **The trick:** only the *audio* tower ships. Text embeddings for a curated vocabulary of about 300–600 prompts ("a pig squealing", "a jet engine roaring past", "cloth flapping in wind", "a sword swishing through air", "a monster growl", "fire crackling", ...) are computed offline in Python and baked in as a `float[N][512]` binary resource (about 1 MB). There's no tokenizer and no text encoder in the plugin. Inference is one audio-encoder pass plus a dot product and a softmax with temperature.
- **LAION-CLAP** (HTSAT audio encoder): GitHub code is CC0; the HF checkpoints (`laion/clap-htsat-unfused`, `larger_clap_general`) are tagged **Apache-2.0**. The training set (LAION-Audio-630K) mixes Freesound licences, so there's some data-provenance risk; the model licence itself is permissive. The HTSAT audio tower is about 30 M params (about 120 MB fp32, about 30 MB int8), so it is an **optional download**, not bundled.
- **MS-CLAP:** code MIT, but the HF weights are **MS-PL**, a weak-copyleft licence that's awkward in an MIT project. Avoid unless reviewed.
- **tinyCLAP** (Paissan & Farella, Interspeech 2024): distils the MS-CLAP audio encoder to about 6 % of the parameters with less than 5 % zero-shot loss. It is small enough to bundle, but it inherits the MS-CLAP weights question. Worth tracking.
- **Whisper-CLAP (LAION, 2025):** Whisper-small audio encoder trained on descriptive captions. Larger, but promising for richer descriptions.
- Advantages over (b): free vocabulary, so it can say things like "creature growl" and "cloth flap" that AudioSet labels badly. Downsides: about 30× the size of EfficientAT, and zero-shot top-1 is noisy, so show the top 3 as suggestions.

**Recommendation:** Tier 0 now. Tier 1 with EfficientAT mn05 (or YAMNet if the licence must be bulletproof) behind a CMake option once Tier 0 is evaluated. Tier 2 only if users ask for richer names; ship it as a downloadable "naming pack".

---

## 3. Auto-naming scheme

Display: `<Noun> · <Role>`, truncated to about 22 characters for the lane header. Examples: `Pig Squeal · Tonal`, `Wind Bed`, `Gravel · Crackle`, `Tonal 2` (no noun found).

Priority for the noun:

1. **User rename** (sticky).
2. **UCS filename.** If the name matches `^[A-Z]{3,}[a-z]*_` and the CatID is in the UCS table (Universal Category System v8.2, public domain), use `FXName` (the second field) as the noun and the UCS category as a strong role prior. For example, `ANMLFarm_Pig Squeal Close_SJ_Lib.wav` → "Pig Squeal", prior Tonal.
3. **ML label** (Tier 1/2) if its probability is > 0.4 and it agrees with the role, or if the filename has no noun.
4. **Cleaned filename:**
   - split on `_ - . space` and camelCase;
   - drop numbers, take numbers like `01`/`v2`, `mono`/`st`/`48k`/`24b`, and creator IDs (all-caps tokens of 2–4 characters after the first);
   - title-case the first 1–3 remaining words.
5. Fallback: `<Role> <n>`.

**Filename prior on the role:** a dictionary of about 300 keywords maps each word to a role weight. Examples:

- `squeal, growl, roar, scream, vocal, whistle, bird, monkey, creature, whale` → Tonal
- `jet, engine, motor, turbine, hum, servo` → Drone
- `rumble, sub, thunder, quake, lfe, boom` → Rumble
- `hit, impact, punch, slam, clang, smash` → Impact
- `fire, crackle, gravel, debris, spark, electric, sizzle` → Crackle
- `wind, air, amb, roomtone, rain, bed, drone, steam` → Bed
- `cloth, flap, swish, swipe, whoosh, sword, wing` → Swish

Fusion: `score[role] = dsp[role] × (1 + 2·filename[role]) × (1 + ml[role])`, then renormalise. The DSP result stays dominant, because the role drives behaviour.

Duplicate names across lanes get a ` 2`/` 3` suffix.

---

## 4. Implementation plan

### Step 1: analysis core (pure, testable, no JUCE GUI)

- New `Source/CargoAnalysis.h/.cpp`:
  - `struct CargoFeatures { ... }` (the fields in §2a, plus a version number).
  - `enum class CargoRole : uint8_t { Generic, Bed, Tonal, Drone, Impact, Swish, Crackle, Rumble }`.
  - `struct CargoRegionInfo { int focus; float clarity; CargoRole role; }`, one per region.
  - `struct CargoClassification { CargoRole role; float confidence; juce::String noun, displayName, source /* "ucs" | "file" | "ml" | "role" */; std::array<float,8> scores; }`.
  - Functions:
    - `computeFeatures(const float*, int, double sr, const std::vector<CargoRegion>&, std::atomic<bool>& cancel)`
    - `classifyRules(const CargoFeatures&)`
    - `FilenameHints parseFilename(const juce::String&)`
    - `fuse(...)`
    - `makeDisplayName(...)`
  - Analyse at most 30 s of *active* audio, sampled evenly from the regions, so a 600 s file costs about as much as a 30 s one. Resample to 16 kHz with `juce::LagrangeInterpolator` after a simple anti-alias low-pass (good enough for features).
- Add it to `SWOOSHBUCKLER_DSP_SOURCES` in `CMakeLists.txt`.
- Add the keyword dictionary and UCS CatID table as `BinaryData` resources (`Resources/cargo_keywords.csv`, `Resources/ucs_catids.csv`).

### Step 2: threading

- Today, `CargoHold::publish()` calls `data->analyse()` synchronously on the message thread. Keep region detection there: it's cheap, and the audio thread needs regions immediately.
- `CargoHold` gets a `juce::ThreadPool m_analysisPool { 1 }`. After `publish`, it queues a `CargoAnalysisJob` holding a `shared_ptr<const CargoData>` (so the data stays alive) and the track's generation counter.
- The job computes features, classification and per-region info. It checks `shouldExit()` and the generation regularly to cancel stale jobs when a new edit lands.
- Results return via `juce::MessageManager::callAsync` using a `WeakReference`/alive-flag guard. On the message thread, if the generation still matches:
  1. Store `Track::classification`.
  2. Rebuild the `CargoData` with `regionInfo` filled. Publish it *without* an undo entry: copy the audio pointer by sharing, so make `CargoData::audio` a `shared_ptr<const AudioBuffer>` first to avoid duplicating up to 600 s of audio.
  3. Set the `role` atomic and call `sendChangeMessage()`.
- Edits (cut, trim, reverse, ...) re-queue the analysis, debounced by 400 ms with the existing timer. A user override of the role or name is never touched.
- Undo/redo: a classification is cached per `CargoData` pointer (`std::map<const CargoData*, CargoClassification>` holding a weak ref), so undo restores the old label instantly.
- Headless/offline render (`Tests/RenderTest.cpp`): add `CargoHold::waitForAnalysis(int ms)` so renders are deterministic.

### Step 3: persistence

`toValueTree`/`fromValueTree` per track:

- `role` (auto result), `roleOverride` (−1 means none), `autoName`, `userName`, `analysisVersion`.
- On load, if `analysisVersion` is missing (an old session), set `roleOverride = Generic`, so **old sessions sound identical**, and still compute the name. Show a small "Auto-assign roles?" chip on the lane.

### Step 4: UI (`CargoHoldView`)

- Lane header shows the display name (double-click to rename), a role pill coloured from `Theme.h`, a confidence tick or `?`, and a spinner while the job runs.
- The role pill opens a popup: Auto (shows the current guess and %), the 7 roles, Generic, and Re-analyse.
- Optional overlay: tonal focus points as small markers on the waveform regions, to help debugging and user trust.

### Step 5: engine behaviours (separate PR, behind a "Smart roles" toggle until tuned)

- `WhooshEngine`: per-lane role atomic. `spawnFlight` pushes cues for Tonal, Swish and Impact lanes into a fixed-size lock-free cue array; nothing allocates on the audio thread.
- `CargoPlayer`:
  - `triggerOneShot(regionIdx, startOffset, rate)`, with a small voice pool;
  - a granular mode for Crackle (grain length 20–80 ms, Hann window, density following the flight envelope);
  - a time-scale factor for Swish (playback rate fitted so the region's envelope peak hits the flight peak, clamped to 0.5×–2×).
- Rumble: route that lane's line through the WEIGHT low-pass and halve its doppler pitch weight. Bed: increase that lane's contribution to the `bed` sum.
- Per-role weights for how strongly the rate follows the pitch: Bed 0.3, Tonal 1.0, Drone 1.0, Swish 0.8, Crackle 0.5, Rumble 0.5, Impact 0.0 (pitch set once at the trigger).
- Add a render-test case per role, using synthetic fixtures (sine glide = Tonal, pink noise = Bed, click = Impact, low sine = Rumble, Poisson clicks = Crackle), so the engine paths are covered in CI.

### Step 6 (optional): Tier 1 ML naming

- CMake `option(SWOOSHBUCKLER_ML_TAGGER "ONNX tagger for naming" OFF)`. When it's ON, link a minimal static ONNX Runtime (ort-builder, universal mac plus x64 Windows), embed `efficientat_mn05.ort` (about 1.5–6 MB) and `cargo_vocab.csv` with `juce_add_binary_data`, and define `SWOOSHBUCKLER_HAS_TAGGER=1`.
- New `Source/CargoTagger.h/.cpp`: `std::optional<std::array<float,527>> tag(const float* mono32k, int n)`. It runs over up to 5 windows of 10 s taken from the loudest regions, mean-pools the results, and is called from the same analysis job. Expected cost is tens of ms on Apple Silicon (verify).
- If the session fails to create, return `nullopt` and fall back to Tier 0. Show a log line, not an error.
- Python tooling in `Tests/tools/` (not shipped): `export_efficientat_onnx.py` (wraps mel + model), `build_vocab.py`, `fit_rules.py` (decision tree → C++), `confusion.py`.

---

## 5. Evaluation plan

**Test set:** about 280 clips, 40 per role × 7, from Samuel's library plus **CC0-only Freesound** clips. Commit only a CSV in `Tests/data/cargo_eval.csv` (`freesound_id or local path, role, acceptableRoles, noun`) and a download script. Don't commit audio. Include hard cases: noisy screams (Tonal/Swish), rain (Bed/Crackle), jet passes (Drone/Bed), thunder (Rumble/Impact), reversed impacts, and files containing silence gaps and several different sounds.

**Tool:** `Tests/ClassifyTest.cpp`, a console target `swoosh_classify <csv>`. It runs the exact plugin code and writes `features.csv` and `predictions.csv`. `Tests/tools/confusion.py` prints:

- an 8×8 confusion matrix;
- per-class precision and recall, and macro-F1;
- an "acceptable" accuracy, where any listed acceptable role counts as correct;
- naming accuracy: top-1 and top-3 nouns judged acceptable.

**Protocol:**

- Tune thresholds on 60 % of the set and report on the held-out 40 %. Split by *source library*, not by clip, to avoid near-duplicates leaking between the sets.
- Report three configurations: DSP only, DSP + filename, and DSP + filename + ML.
- Also test with filenames scrambled, so the filename prior can't hide weak DSP.

**Targets to ship Tier 0:**

- role macro-F1 ≥ 0.75 (DSP only) and ≥ 0.85 (with filename);
- confusion between Impact and Bed under 2 %, because that pair is the worst for behaviour;
- analysis under 150 ms for a 10 s file and under 2 s for a 600 s file on an M1, with zero allocation or locking on the audio thread (checked with the existing render test under TSan/ASan).

**Robustness:**

- The role should stay the same after normalise, ±6 dB gain, a 44.1/48/96 kHz source rate and mono-summing.
- Trimming the silence must not change it.
- Reversal is allowed to change Impact.

**Listening check:** render 10 whooshes per role with Smart roles on and off. Run a blind A/B with 2–3 sound designers before switching the default on.

---

## Sources

- EfficientAT (MIT; mn04/mn05/mn10 sizes and mAP): https://github.com/fschmid56/EfficientAT
- CED (repo GPL-3.0, ONNX export): https://github.com/RicherMans/CED · HF weights Apache-2.0: https://huggingface.co/mispeech/ced-tiny
- YAMNet TFLite: https://www.tensorflow.org/lite/examples/audio_classification/overview · ONNX conversion: https://huggingface.co/andrelgomes/yamnet-onnx
- PANNs weights (CC BY 4.0, sizes): https://zenodo.org/records/3987831
- AudioSet licences (dataset CC BY 4.0, ontology CC BY-SA 4.0): https://research.google.com/audioset/download.html · https://github.com/audioset/ontology
- LAION-CLAP: https://github.com/LAION-AI/CLAP · https://huggingface.co/laion/clap-htsat-unfused (Apache-2.0) · paper https://arxiv.org/abs/2211.06687
- MS-CLAP (code MIT, weights MS-PL): https://github.com/microsoft/CLAP · https://huggingface.co/microsoft/msclap
- tinyCLAP (2023, Interspeech 2024 version): https://arxiv.org/abs/2311.14517
- Whisper-CLAP (LAION): https://huggingface.co/laion/whisper-clap-version-0.1
- Apple SoundAnalysis built-in classifier: https://developer.apple.com/documentation/soundanalysis/snclassifieridentifier · https://www.swiftjectivec.com/sound-analysis-framework-built-in-model
- UCS (public domain SFX taxonomy, CatID filenames): https://universalcategorysystem.com · https://www.spencerbruce.com/blog1/2025/10/3/understanding-the-universal-category-system-ucs
- Sound Effects Dataset Unification With UCS (Beck & Lerch, DAFx 2026; EnvSound-UCS, 58k clips mapped from AudioSet/FSD50K/ESC-50 to UCS; a possible source of training and eval data): https://arxiv.org/abs/2606.05571
- ONNX Runtime in JUCE (static vs dynamic, protobuf clash): https://forum.juce.com/t/deploying-an-onnx-model/59753 · ort-builder: https://github.com/olilarkin/ort-builder · anira: https://marketplace.juce.com/products/anira

Not verified, check before relying on them: the EfficientAT weights licence (no separate statement found; assumed covered by the repo's MIT), the Essentia model licence, and ONNX Runtime minimal-build sizes per architecture.
