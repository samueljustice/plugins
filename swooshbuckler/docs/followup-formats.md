# Follow-up: formats, sample rates, channel layouts

User requests (after the v2 redesign brief):
- Choose the sample rate to record out to (e.g. 96 kHz).
- Import samples of any bit depth / sample rate / file type and keep the data, like Reaper does with mixed
  rates; when recording out, keep the information. "The DSP needs to support it as well."
- The same for file types and channel layouts: mono, stereo, 5.1, 7.1, with graceful downmixing to stereo.
- Recording file names: `Swooshbuckler_YYYY-MM-DD_HH-MM-SS_NNNNN.wav` (5 random digits). DONE in TapeRecorder.

## Already done (Source/TapeRecorder.*)
`TapeRecorder::Options { bitDepth 16/24/32f, fileRate (0 = session, else windowed-sinc conversion on the writer
thread), type Wav/Aiff/Flac, mono }`, `start(folder, Options, error)`, `saveRetro(folder, Options, written, error)`,
BWF metadata on WAV, random 5-digit file tag. Not yet exposed in the processor/UI and not yet tested.

## To do
1. **Recorder settings** (processor + settings panel): file type (WAV / AIFF / FLAC), bit depth (16 / 24 /
   32-bit float, float only for WAV; grey it out otherwise), sample rate (Session, 44.1, 48, 88.2, 96, 176.4,
   192 kHz), channels (Stereo / Mono). Persist in the app PropertiesFile like the folder. Status text in the
   settings panel when the chosen rate is above the session rate: "Upsampled from 48 kHz. For full-band 96 kHz,
   set the audio device to 96 kHz" (standalone: Options > Audio settings).
2. **Import, keep everything** (Cargo.*, SampleClassifier unaffected):
   - `CargoData` gains `original` (AudioBuffer<float>, all channels, source rate, untouched), `sourceRate`,
     `sourceBits`, `sourceIsFloat`, `formatName`, `layout` (juce::AudioChannelSet from the reader's
     `getChannelLayout()`), and a `stereo` downmix (2 ch, source rate) used for display/bed playback if wanted.
     `audio` stays the MONO carrier used by the engine and region detection, but now **at the engine rate**
     (see 4). `sampleRate` = rate of `audio`.
   - Read any format the AudioFormatManager knows (registerBasicFormats: WAV, AIFF, FLAC, OGG, MP3, and on
     macOS CoreAudio: CAF, M4A/AAC/ALAC). Mono, stereo, LCR, quad, 5.0/5.1, 6.1, 7.0/7.1, 7.1.4 (bed channels
     only), or N discrete channels.
   - **Downmix to stereo, ITU-R BS.775 style** using `AudioChannelSet::getTypeOfChannel`: L/R 1.0;
     centre 0.7071 to both; every left surround/side/rear 0.7071 to L, right ones to R; centre surround 0.5 to
     both; LFE dropped (0); top/height channels 0.5 to their side. Unknown layout with > 2 channels and no mask:
     assume the WAVE_FORMAT_EXTENSIBLE default order (L R C LFE Ls Rs Lb Rb Lss Rss ...) for 3..8 channels, else
     alternate odd/even channels to L/R. Gain-safe: if the downmix peak exceeds the loudest source channel's
     peak, scale the downmix down to it (no clipping, no level jump).
   - **Mono carrier** from the stereo downmix: 0.5 (L + R); if that loses more than 6 dB versus the mean channel
     RMS (out-of-phase material), use the louder channel instead. Mono files pass straight through.
   - **Edits** (cut/copy/paste/duplicate/trim/delete/reverse/normalise/fade) apply to `original` (map sample
     positions by the rate ratio) and the derived buffers are rebuilt, so nothing is lost. The clipboard keeps
     channels and rate; pasting converts to the target clip's rate/channels if they differ.
   - **Session save is lossless**: FLAC (multi-channel) when the source is integer <= 24-bit and FLAC supports
     the rate; otherwise raw 32-bit float per channel. Save `sourceRate`, `sourceBits`, `sourceIsFloat`,
     `formatName`, layout (channel types), plus everything from v2 (role, names, mute/solo). Old v1/v2 sessions
     (mono "flac"/"raw" at one rate) still load.
3. **Timeline**: each clip shows a small info tag, e.g. `96 kHz · 24-bit · 5.1 → stereo` or `44.1 kHz · 16-bit · mono`
   (Reaper-style), plus the same in the clip tooltip with the file format name.
4. **DSP at any rate**:
   - The engine already uses `sampleRate` everywhere; verify it at 44.1, 48, 88.2, 96, 176.4 and 192 kHz
     (filters clamp below Nyquist, delay lines sized from the rate, oversampling, reverb, limiter).
   - **Band-limited sample playback**: `CargoHold::setEngineRate(double)` (processor calls it from
     prepareToPlay via `MessageManager::callAsync`; the hold rebuilds each track's `audio` from `original` with a
     high-quality windowed-sinc resampler on the message thread, re-runs analysis, republishes). The player then
     reads at a ratio of exactly 1 x pitch. Until a rebuild lands, the player keeps working with the ratio
     `data.sampleRate / engineRate` (current behaviour). This removes the aliasing you get today when e.g. a
     96 kHz file is read at 48 kHz with cubic interpolation.
5. **Tests**:
   - `Tests/RecorderTest.cpp` (new target SwooshbucklerRecorder): record 2 s of a 1 kHz sine from a 48 kHz
     session to 96 kHz WAV 24-bit, 44.1 kHz FLAC, 48 kHz AIFF, 32-bit float WAV, and mono; re-read each file and
     check rate, bits, channels, length (+-2 samples of the exact converted length), pitch (zero-crossing count
     within 0.5%), level (within 0.2 dB), no start/end click, BWF description present, and the file name pattern
     `^Swooshbuckler_\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d_\d{5}\.(wav|aif|flac)$`.
   - Import tests (RenderTest or a new target): synthesise 5.1 and 7.1 WAVs (with channel masks) where each
     channel has its own tone, load them, check every channel retained bit-exactly in `original`, check the
     stereo downmix coefficients (centre at -3 dB in both, LFE absent, surrounds on the right side) and that the
     downmix never exceeds the loudest source channel's peak; load a 96 kHz/24-bit file with a 30 kHz tone into a
     48 kHz engine and check the tone is gone from the mono carrier (> 60 dB down); save/restore keeps 24-bit
     data bit-exact and float data exact.
   - Engine render at every supported rate: finite, under the ceiling, faster than real time.

## Also in this pass: Peak Trem (user request: "a peak tremolator dynamic function, simple, on/off, rubber banded")
- Parameters: `tremOn` (bool, default off), `tremDepth` (0..1, default 0.5), `tremRate` (1..20 Hz, default 6, skewed).
- WhooshEngine, after Swirl: per-sample gain g = 1 - d * shape(phase), where
  - rate = tremRate * (1 + 3 * peak^1.5) (peak = m_peakEnv: the flight envelope heard now), so it speeds up
    towards the pass and relaxes on the tail; phase integrates rate (no jumps when rate changes);
  - d = depth * (0.35 + 0.65 * peak) * onFade; onFade rubber-bands 0<->1 over ~150 ms when toggled;
  - shape morphs from a raised sine (approach) to a softened pulse (0.5+0.5 tanh(k sin)) at the peak;
  - right channel phase offset 0.18 cycles for movement; depth/rate chased by sj::Spring (slight overshoot).
- UI: "Peak trem" toggle + Depth + Rate in the macro area (small), also in the More drawer under Movement.
- Test: in RenderTest, with tremOn the 30-60 Hz amplitude-modulation energy near the flight peak is higher
  than in the approach, no clicks when toggled mid-whoosh, off == bit-identical to before.

## Also in this pass: Morph integration
The morph agent is building Source/SpectralMorph.*, Source/MorphPad.* and docs/experiments/06-morph.md (own test
in Tests/morph). Integrate per its report: params, an extra mono "Morph" layer computed from lanes A and B,
MorphPad in the editor (ropes dropped onto timeline lanes via laneAtScreenPosition), butterfly-curve wander.
