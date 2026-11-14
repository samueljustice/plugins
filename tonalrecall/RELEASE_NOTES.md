## Tonal Recall 0.2.0

The first release of Tonal Recall, an offline pitch matcher for Pro Tools AudioSuite, made for trailer sound design. Play it the music, play it a sound effect, and it works out the shift that puts the effect on one of the music's notes, shows it, and stamps it onto the clip when you render. The clip keeps its length and the timing of its hits.

- **Learn by playing**: arm LEARN, select the clip and press Preview in the AudioSuite window. The music's key, root, strong notes and tuning are found as it plays.
- **A sting, not a tune**: by default the effect takes one fixed shift onto the nearest strong note of the music and keeps its own movement. The SUBTLE and MOVING curves let it lean with the music.
- **Editable**: pull the line, draw it freehand, snap it to the music's notes or scale, undo and redo.
- **Hear it before you commit**: Preview plays the stamped effect, and Render writes it into the clip.

The VST3 and AU versions are the same offline tool for other hosts, not a live effect: Preview means playing the clip through the plug-in, and Render means bouncing or freezing the track.

![Tonal Recall](https://raw.githubusercontent.com/samueljustice/plugins/tonalrecall-v0.2.0/tonalrecall/docs/screenshot.png)

The AAX for Pro Tools is in the [Sammy J's Plugins installer](https://github.com/samueljustice/plugins/releases/latest), which also installs the newest version of every other Sammy J plug-in.

### Downloads

| File | Contents |
|---|---|
| `TonalRecall-0.2.0-macOS-Installer.pkg` | macOS installer (AU + VST3, universal) |
| `TonalRecall-0.2.0-Windows-Installer.exe` | Windows installer (VST3) |
| `TonalRecall-0.2.0-macOS-AU.zip`, `TonalRecall-0.2.0-macOS-VST3.zip`, `TonalRecall-0.2.0-Windows-VST3.zip` | The plug-ins on their own |

The macOS downloads are signed and notarised. Universal (Apple silicon and Intel), macOS 11 or later. Windows: 64-bit Windows 10 or later.

The manual, with a tour of every control: [tonalrecall/README.md](https://github.com/samueljustice/plugins/blob/main/tonalrecall/README.md).
