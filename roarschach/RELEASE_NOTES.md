## Roarschach 1.0.0

The first release. Roarschach turns a spoken or shouted performance into a big, liquid, gravelly creature voice that still reads as words.

- **Fourteen modules in any order**: PITCH, CLEAN, DE-ESS, DETAIL, VOCODER, RING, SHIMMER, GROWL, SWIRL, ROAR, ENSEMBLE, TAME, SUB and OVERTONE, each with an On switch. Drag a module by its header, use its ••• menu, or press Alt+Left / Alt+Right to move it. Moves crossfade over 30 ms, so they are click-free while audio plays, and the order is saved, recalled by presets and automatable.
- **Pitch and size**: shift the voice from two octaves down to one octave up (Shift, Snap or Mono), and make the throat bigger separately.
- **Native sample rate**: the audio is never resampled, so at 96 or 192 kHz a two-octave drop brings the recording's ultrasonic detail down into the audible band.
- **Machines**: a 32-band vocoder on a pulse carrier one or two octaves below the voice, and a ring modulator with a fixed or pitch-tracking carrier, Ring or AM, and a small resonant body.
- **Weight**: a tracked octave below the voice, and overtones that carry the low end on small speakers.
- **A finished output**: level matching, Push into a true-peak-safe limiter, an oversampled clipper (Clip and Shape), and Attack, Release and Auto release to choose between clean limiting and low-end crunch. MIX blends in the phase-matched dry voice.
- **A creature that reacts**: an OpenGL ink-blot creature whose jaw, eyes, skin and throat follow the voice and every module, and which falls asleep in silence.
- **39 factory presets** in six groups (Creatures, Voices, Machines, Signal Flow, Vocoder, Utility), plus Init.
- Constant latency of about 46 ms, reported to the host. Resizable window from 50% to 125%.

### Downloads

| File | Contents |
|---|---|
| `Roarschach-1.0.0-macOS-Installer.pkg` | macOS installer (AU + VST3, universal) |
| `Roarschach-1.0.0-Windows-Installer.exe` | Windows installer (VST3) |
| `Roarschach-1.0.0-macOS-AU.zip`, `Roarschach-1.0.0-macOS-VST3.zip`, `Roarschach-1.0.0-Windows-VST3.zip` | The plug-ins on their own |

The macOS downloads are signed and notarised. Universal (Apple silicon and Intel), macOS 11 or later. Windows: 64-bit Windows 10 or later.

The manual, with a tour of every control: [roarschach/README.md](https://github.com/samueljustice/plugins/blob/main/roarschach/README.md).
