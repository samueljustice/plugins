## De-Honk 0.0.2 (test build)

De-Honk is a plug-in by Samuel Justice for the master bus of a trailer mix, before the limiter. When many of the sounds and much of the music have each been through a bass enhancer or a saturator, their added harmonics pile up in the low mids as a nasal, boxy build-up. De-Honk listens for that build-up as the mix plays and takes it down with narrow cuts, leaving the bass and the body alone. It works in mono, stereo and surround up to 7.1.4.

This is the second build for testing. Controls, presets and the sound may change before the first full release, and sessions saved with it may not open the same way in a later version. What each control does is in the [De-Honk README](https://github.com/samueljustice/plugins/blob/main/dehonk/README.md).

### New in 0.0.2

- **Dialogue sidechain.** Route the dialogue stem to the plug-in's sidechain (the key input in Pro Tools) and switch Dialogue on. Bands the dialogue accounts for are left alone, and with Snap on so are its clicks; build-up well above it is still cut. The stem can arrive at any level, and should be in time with the mix. The lamp turns red if the stem cannot be found in the mix or is most of it.
- **Detection, Fast or Steady.** Fast cuts a build-up as soon as it shows. Steady waits until a build-up has held for a while and leaves a broad lift alone: steadier, and kinder to dense, noisy textures, but slower to catch a new build-up, and in a dense mix it may leave a mild one alone. A new factory preset, "Master bus, steady", is the defaults at Steady.
- **Each range's own attack and release.** Click a range's name to select it, open its lock, and set them. Locked, a range follows Speed and Release.
- **Threshold and Max Cut can be dragged in the band view**, like the range edges. A double-click puts any of them back.
- **Ranges are tabs under the band view**, each in its own colour. The lamp switches a range on and off; the name selects it, and then only that range's creatures move in the pond.
- **Link is Linked, Grouped or Unlinked.** Grouped is for surround: the front pair, the centre, the surrounds and the heights each share their cuts.
- **Diff no longer clicks** when it is switched, and its level follows the treated output all the time, so where nothing is cut the two match.
- **Where the dialogue stem goes.** The sidechain input is called Dialogue. In Pro Tools it is the key input (mono); in Logic, the Side Chain menu; in VST3 hosts, the plug-in's sidechain, mono or stereo. The README has the routing.
- **Impact Guard's lamp is its switch.**
- **The pond runs at 60 pictures a second and is calmer.** Geese arrive one at a time and stay a while; the recordist works on one of them and turns before he walks to it. The trees mark the range edges, above their handles in the band view. The creatures react when clicked: a turtle topples into the water, the frog jumps, a goose tumbles away.
- **The window is styled throughout:** tooltips that wait and open beside the control, menus and dialogs in the window's own style, an About page (click the name) with a check for a newer version.
- **It is listed as SammyJs De-Honk.** The installers remove the 0.0.1 files. To the host it is the same plug-in, so sessions saved with 0.0.1 should still open with it in place; their Link percentage comes back as Linked (50% or more) or Unlinked. If a host does not find it in an old session, say which host.

The full list is in the [change log](https://github.com/samueljustice/plugins/blob/main/dehonk/CHANGELOG.md).

### Not yet tried

- The key input has not been tried in Pro Tools or Logic. If the sidechain does not appear or the Dialogue lamp does not light, say which host. In Pro Tools, choose the key input with the transport stopped: the output may drop out for a moment as it connects.
- Typing a preset's name inside the plug-in's window has not been tried in a host. If the keys do not arrive, or Space starts the transport while you type, say which host.
- A test build does not look for newer versions by itself. Open the About page and press Check for updates.
- Automation of Link written with 0.0.1 no longer does anything (Link is now a choice of three). Every other control keeps its place in a host's list, so other automation written with 0.0.1 still drives what it drove.

The AAX for Pro Tools is signed by hand after this release is made. Once it is, the two installers below are replaced with ones that include it. If the installer offers no AAX, download it again later.

### Downloads

| File | Contents |
|---|---|
| `DeHonk-0.0.2-macOS-Installer.pkg` | macOS installer: VST3, AU, the app, and the AAX once signed |
| `DeHonk-0.0.2-Windows-Installer.exe` | Windows installer: VST3, the app, and the AAX once signed |
| `DeHonk-0.0.2-macOS-VST3.zip`, `DeHonk-0.0.2-macOS-AU.zip`, `DeHonk-0.0.2-macOS-App.zip`, `DeHonk-0.0.2-Windows-VST3.zip`, `DeHonk-0.0.2-Windows-App.zip` | The plug-ins and the app on their own |
| `DeHonk-0.0.2-macOS-AAX.zip`, `DeHonk-0.0.2-Windows-AAX.zip` | The signed AAX on its own, once signed |

The macOS downloads are signed and notarised. Universal (Apple silicon and Intel), macOS 11 or later. Windows: 64-bit Windows 10 or later; the installer is not code-signed, so Windows may warn before running it.
