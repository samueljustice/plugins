## SammyJs Reversinator 2.0.0

Reversinator has been rebuilt to match **Backwards Machine** (The Sound Guy, 2007), the plugin it was always inspired by. The new version also has a new interface.

![Reversinator](https://raw.githubusercontent.com/samueljustice/plugins/reversinator-v2.0.0/reversinator/docs/screenshot.png)

### What's new

- **The original's three effects, faithfully recreated.** Reverse Playback, Forward Backwards and Reverse Repeat now use Backwards Machine's own signal flow: swept delay lines, sine windows, the Reverse Repeat vibrato, and the feedback routing and gain staging.
- **Checked against the real thing.** The vendor's dry demo clip, run through Reversinator, nulls against the vendor's own Backwards Machine renders. Correlation is 0.9994 for Reverse Playback, 0.9995 for Forward Backwards and 0.9990 for Reverse Repeat, with the residual 27–30 dB down. A single 192 kbps MP3 round trip, the format the demos were published in, leaves about 33 dB. The renders match how the vendor made the demos: processed at 44.1 kHz, with the snippet clock started 448 samples before the clip, and Reverse Repeat at its demo's TIME (2.0047 s).
- **In tune at any sample rate.** At 44.1 and 48 kHz the snippet clock is the original's float32 clock, sample for sample. Above that, where the original's clock would drift up to a semitone out of tune within each snippet, it runs in double precision and is exact.
- **The original's controls.** REVERSER, TIME (0.5–4 s, same taper), FEEDBACK DEPTH (0–75%), WET MIX and DRY MIX (squared law), with the same defaults.
- **Click-free everywhere.**
  - REVERSER switches with a 10 ms crossfade.
  - TIME moves crossfade between the old and new read positions.
  - Switching effects crossfades straight into the new one.
  - Host bypass crossfades too, and the effect keeps running underneath, so un-bypassing lands in a live effect.
  - Wet, dry and feedback changes are smoothed.
  - None of this depends on the host buffer size.
- **New interface** in the spirit of the original: mode lamps, the big REVERSER button, an hourglass TIME control and a feedback loop. A live snippet display shows the read heads sweeping backwards through the audio. The window is resizable from 75% to 200%.

### Fixes

- macOS builds now run on **macOS 11 or later**. Previous builds required the macOS version of the machine that built them.
- The engine never allocates memory or does large copies on the audio thread, so there are no CPU spikes when TIME changes.
- Feedback is now real feedback of the reversed signal. It was a one-sample filter on the output.
- Turning REVERSER back on no longer replays stale audio. The effect keeps running, as on the original.
- The About box is now a panel inside the plugin window, so it no longer floats above other apps.
- Closing the plugin during an update check no longer risks a crash or waits on the network.
- Update checks now recognise newer versions correctly.
- The macOS installer no longer redirects the install to a stray copy elsewhere on disk.
- The Windows plugin no longer needs a recent Visual C++ runtime on the machine: it is now built in.
- The Windows installer now also runs on Windows on Arm.

### Upgrading from 1.x

Sessions saved with 1.x still open, but some settings sound different:

- **Envelope** and **Crossfade** have been removed, because the original's windows are fixed.
- TIME and FEEDBACK values outside the new ranges are clamped.
- **WET MIX** and **DRY MIX** now use the original's squared law, so settings below 100% are quieter than in 1.x (50% was -6 dB and is now -12 dB).
- TIME and FEEDBACK DEPTH **automation** recorded in 1.x replays rescaled onto the new ranges, in hosts that identify parameters by ID. For example, a TIME point at 2.00 s now plays at about 1.89 s.
- **Logic Pro and GarageBand** identify Audio Unit parameters by position, and removing Envelope and Crossfade moved them. There only the Mode and TIME automation lanes carry over: delete the others and redraw them.
- REVERSER now defaults to on. Set to forwards, it now plays the input at the WET MIX level plus DRY MIX, as on the original, where 1.x passed it through unchanged. Forward passages change level unless WET is 100% and DRY 0%.

### Downloads

| File | Contents |
|---|---|
| `SammyJs_Reversinator_Installer.pkg` | macOS installer (AU + VST3, universal) |
| `SammyJs_Reversinator_Installer.exe` | Windows installer (VST3) |
| `Reversinator-macOS-AU.zip`, `Reversinator-macOS-VST3.zip`, `Reversinator-Windows-VST3.zip` | The plugins on their own |

*Backwards Machine is a trademark of The Sound Guy, Inc. Reversinator is an independent recreation and is not affiliated with or endorsed by them.*
