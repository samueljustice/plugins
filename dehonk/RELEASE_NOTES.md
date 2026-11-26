## De-Honk 0.0.4 (test build)

De-Honk is a plug-in by Samuel Justice for the master bus of a trailer mix, before the limiter. When many of the sounds and much of the music have each been through a bass enhancer or a saturator, their added harmonics pile up in the low mids as a nasal, boxy build-up. De-Honk listens for that build-up as the mix plays and takes it down with narrow cuts, leaving the bass and the body alone. It works in mono, stereo and surround up to 7.1.4.

This is the fourth build for testing. Controls, presets and the sound may change before the first full release, and sessions saved with it may not open the same way in a later version. What each control does is in the [De-Honk README](https://github.com/samueljustice/plugins/blob/main/dehonk/README.md).

### New in 0.0.4

- Pro Tools now lists De-Honk under **EQ** in its plug-in menus. It used to appear under Other. Nothing else changes from 0.0.3.

### New in 0.0.3

- **Solo Honk is now Delta; Diff is now Compare.** Delta plays only what is being removed; Compare plays the untreated mix, latency-matched and level-matched, held or clicked on. They sit under a small "listen" caption. What they do has not changed.
- **Speed is now Attack**, shown as the attack time it gives, 5 to 40 ms (14 ms by default). Turned up in the window, the attack is slower. In a host's automation lane and on a control surface it runs the other way round: higher is a shorter attack. The time shown is right either way.
- **Sharpness is now Focus**, and **Dialogue is now Dialogue Key** (its bar is Dialogue Key Amount in the host; the sidechain input is still called Dialogue).
- **Each range tab shows its frequencies** under its name, and follows the edges as they are dragged. Snap's reads "clicks, 2.5 kHz". The level bar in the band view is captioned "level".
- **Host names.** The range switches read Woof On, Tub On, Fog On, Bark On and Snap On; the edges begin with Edge; Link is Channel Link.
- **Two factory presets are renamed:** Heavy is now "Master bus, heavy", and "Narrow resonances only" is now "Narrow focus only". Sessions saved on them still come back on them.
- **The sound recordist and the animals are livelier.**

Only names have changed in the controls: every parameter keeps its place and its values, so sessions, presets and automation from 0.0.2 come back as they were.

The full list is in the [change log](https://github.com/samueljustice/plugins/blob/main/dehonk/CHANGELOG.md).

### Not yet tried

- Attack in a host's automation lane or on a control surface runs the other way round from the knob in the window: higher is a shorter attack. The time shown is right either way. If that gets in the way, say so.
- How the new names look in a host's automation lanes and on a control surface. Several start with the same word (Woof ..., Dialogue Key ...), so a surface that shortens names may show two the same. If one does, say which.
- The key input has not been tried in Pro Tools or Logic. If the sidechain does not appear or the Dialogue Key lamp does not light, say which host. In Pro Tools, choose the key input with the transport stopped: the output may drop out for a moment as it connects.
- Typing a preset's name inside the plug-in's window has not been tried in a host. If the keys do not arrive, or Space starts the transport while you type, say which host.
- A test build does not look for newer versions by itself. Open the About page and press Check for updates.

The AAX for Pro Tools is signed by hand after this release is made. Once it is, the two installers below are replaced with ones that include it. If the installer offers no AAX, download it again later.

### Downloads

| File | Contents |
|---|---|
| `DeHonk-0.0.3-macOS-Installer.pkg` | macOS installer: VST3, AU, the app, and the AAX once signed |
| `DeHonk-0.0.3-Windows-Installer.exe` | Windows installer: VST3, the app, and the AAX once signed |
| `DeHonk-0.0.3-macOS-VST3.zip`, `DeHonk-0.0.3-macOS-AU.zip`, `DeHonk-0.0.3-macOS-App.zip`, `DeHonk-0.0.3-Windows-VST3.zip`, `DeHonk-0.0.3-Windows-App.zip` | The plug-ins and the app on their own |
| `DeHonk-0.0.3-macOS-AAX.zip`, `DeHonk-0.0.3-Windows-AAX.zip` | The signed AAX on its own, once signed |

The macOS downloads are signed and notarised. Universal (Apple silicon and Intel), macOS 11 or later. Windows: 64-bit Windows 10 or later; the installer is not code-signed, so Windows may warn before running it.
