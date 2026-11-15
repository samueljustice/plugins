## Tonal Recall 0.2.2

Tonal Recall 0.2.2 lets you play back what it has learned and what it will stamp, with a playhead on each view, keeps a stamp from going over full scale, and lets you zoom in on the effect's pitch line for long selections.

Sessions keep their settings. The reported latency is about 290 ms at 48 kHz (2 ms more, for the limiter's look-ahead); Pro Tools compensates it in a render as before.

### Play buttons and playheads

- **PLAY MUSIC**, **PLAY INPUT** and **PLAY OUTPUT** play the music and the effect as learned, and the effect as it will be stamped. Click from one to the next to compare them. One plays at a time, and each stops at its end.
- **They play through Preview.** With Preview running in the AudioSuite window a button plays straight away. Pressed with Preview stopped, it waits, the strip says to press Preview, and it starts from the beginning when Preview starts.
- **PLAY OUTPUT follows what you change.** Pick another curve or target, move BEND or OCTAVE, or pull and draw the line while it plays, and the next moment you hear has the change.
- **A playhead moves across each view**, scaled to that clip's own length, with the time beside the buttons. With STAMP on it also runs across the effect's scan while Preview plays the clip.
- **The music is kept in stereo**, so it plays back as it was learned.

### Staying under full scale

Shifting pitch can raise a sound's peaks, so a loud effect could clip when it was stamped.

- **TRIM** (on by default): after each match the stamp is measured, and it is turned down by just enough to keep it under -0.1 dBFS. The button shows how much. Off, the level is left alone.
- **A limiter** at the end of the stamp holds any peak the trim did not cover, such as a different clip from the one that was learned. Below full scale it leaves the audio untouched.
- **The strip says** when the limiter had to act on the last render, when TRIM is off and the stamp would go over, and when the effect was already at full scale as learned.
- **COPY VALUES** adds the trim when one is applied.

### Zooming in on the effect

A long selection (20 seconds, say, with the effect sounding for a few of them) left the pitch line small and its points hard to place.

- **Zoom and scroll the effect's scan in time.** The wheel or a pinch zooms around the moment under the pointer; sideways, or with Shift, it scrolls.
- **An overview bar** at the top of the scan shows the whole selection in miniature with the stretch on view lit. Drag it to scroll, drag its edges to zoom, double-click to see everything.
- **FIT** shows the whole selection and **SOUND** frames just the part where the effect sounds. **-** and **+** step out and in.
- **A time ruler** runs along the bottom of the scan.
- **The view follows the playhead** while PLAY INPUT or PLAY OUTPUT plays.
- **Freehand drawing keeps its detail when zoomed in**, and a line drawn slowly no longer collapses to a single point.

### Fixed

- UNDO, REDO and RESET CURVE could sit on top of the effect's readout line.
- A capture that stopped at the 60 second limit also asked for the selection to be learned again, which could not help.

- An effect learned with a long silence before or after it (a selection much longer than the sound) could be read as having no clear pitch, or the wrong one. Only the part that sounds is analysed now, and the pitch line keeps its place in time.
- The music's tuning could read `+-0 C`.

![Tonal Recall](https://raw.githubusercontent.com/samueljustice/plugins/tonalrecall-v0.2.2/tonalrecall/docs/screenshot.png)

The AAX for Pro Tools is in the [Sammy J's Plugins installer](https://github.com/samueljustice/plugins/releases/latest), which also installs the newest version of every other Sammy J plug-in.

### Downloads

| File | Contents |
|---|---|
| `TonalRecall-0.2.2-macOS-Installer.pkg` | macOS installer (AU + VST3, universal) |
| `TonalRecall-0.2.2-Windows-Installer.exe` | Windows installer (VST3) |
| `TonalRecall-0.2.2-macOS-AU.zip`, `TonalRecall-0.2.2-macOS-VST3.zip`, `TonalRecall-0.2.2-Windows-VST3.zip` | The plug-ins on their own |

The macOS downloads are signed and notarised. Universal (Apple silicon and Intel), macOS 11 or later. Windows: 64-bit Windows 10 or later.

The manual, with a tour of every control: [tonalrecall/README.md](https://github.com/samueljustice/plugins/blob/main/tonalrecall/README.md).
