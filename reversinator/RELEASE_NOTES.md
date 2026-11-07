## SammyJs Reversinator 2.0.0

Reversinator has been rebuilt to match **Backwards Machine** (The Sound Guy, 2007), the plugin it was always inspired by. The new version also has a new interface.

![Reversinator](https://raw.githubusercontent.com/samueljustice/plugins/main/reversinator/docs/screenshot.png)

### What's new

- **The original's three effects, faithfully recreated.** Reverse Playback, Forward Backwards and Reverse Repeat now use Backwards Machine's own signal flow: swept delay lines, sine windows, the Reverse Repeat vibrato, and the feedback routing and gain staging. Reverse Playback null-tests against the original's own demo render at 0.98 correlation, which is the limit of the MP3 the demo was published as.
- **The original's controls.** REVERSER, TIME (0.5–4 s, same taper), FEEDBACK DEPTH (0–75%), WET MIX and DRY MIX (squared law), with the same defaults.
- **Click-free everywhere.**
  - REVERSER switches with a 10 ms crossfade.
  - TIME moves crossfade between the old and new read positions.
  - Switching effects crossfades straight into the new one.
  - Wet, dry and feedback changes are smoothed.
  - None of this depends on the host buffer size.
- **New interface** in the spirit of the original: mode lamps, the big REVERSER button, an hourglass TIME control and a feedback loop. A live snippet display shows the read heads sweeping backwards through the audio. The window is resizable from 75% to 200%.

### Fixes

- macOS builds now run on **macOS 11 or later**. Previous builds required the macOS version of the machine that built them.
- The engine never allocates memory or does large copies on the audio thread, so there are no CPU spikes when TIME changes.
- Feedback is now real feedback of the reversed signal. It was a one-sample filter on the output.
- Turning REVERSER back on no longer replays stale audio. The effect keeps running, as on the original.
- Fixed a possible crash when closing the About window during an update check. Update checks now recognise newer versions correctly.
- The macOS installer no longer redirects the install to a stray copy elsewhere on disk.
- The Windows installer now also runs on Windows on Arm.

### Upgrading from 1.x

Sessions saved with 1.x still open. **Envelope** and **Crossfade** have been removed, because the original's windows are fixed. TIME and FEEDBACK values outside the new ranges are clamped, and REVERSER now defaults to on.

### Downloads

| File | Contents |
|---|---|
| `SammyJs_Reversinator_Installer.pkg` | macOS installer (AU + VST3, universal) |
| `SammyJs_Reversinator_Installer.exe` | Windows installer (VST3) |
| `Reversinator-macOS-AU.zip`, `Reversinator-macOS-VST3.zip`, `Reversinator-Windows-VST3.zip` | The plugins on their own |

*Backwards Machine is a trademark of The Sound Guy, Inc. Reversinator is an independent recreation and is not affiliated with or endorsed by them.*
