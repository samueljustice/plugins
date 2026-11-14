## Roarschach 1.2.0

Roarschach 1.2.0 has a new visualiser: an abstract ink creature that forms out of lit cloud, swirls with the voice and tears back into smoke, a different blot in every session. It also fixes a set of sound problems in PITCH, VOCODER, RING, GROWL, ROAR and OUTPUT, and makes the window easier to live with: it opens larger and remembers its size, a preset load can be undone, knobs the settings do not use are dimmed, and the About window has a quick start.

Parameter IDs, their order and their types are unchanged, so sessions, presets and hosts keep finding every control. Latency is unchanged: about 46 ms (2192 samples at 48 kHz), the same for every setting and every module order.

### New: the ink creature

The view above the chain now draws an abstract creature in ink. It is not a face: it is a mirrored blot that the voice gathers out of the sky.

- **It forms out of lit cloud by flow.** Asleep, the view is a slow sky of lit cloud in three layers, with banks, gaps and shaded undersides. When a voice comes in, the cloud near the middle starts to turn, and the faint ink in it streams inwards along mirrored eddies, leaving the cloud it came from paler. It gathers first as shreds, then joins into one folded mass, the cloud parting round it.
- **It swirls with the voice.** The mass never holds still: the flow keeps carrying its edge off as smoke and the voice keeps pulling more in, its inside marbled by where the ink came from, with banks of cloud drifting in front of it.
- **A roar tears it open.** On loud, heated sound a ragged rift opens down its middle with the weight of the phrase, and the blot swells larger and darker. In some blots the rift is lit from inside by a warm, drifting glow; in others it stays a dark tear onto the cloud behind. The light comes and goes.
- **A hit ripples it.** A stomp, a thump, a plosive or the start of a roar sends rings out through the cloud and the ink, like a struck water surface, so the picture heaves and settles. Steady talking does not. A sustained low end makes the surface tremble finely instead.
- **It tears back to smoke.** When the voice stops, the ink lets go: its limbs tear into ribbons, then smoke, then the grey of the cloud. After 1.5 seconds of silence it is asleep.
- **A different blot every time.** Each instance grows its own blot from its own seed, and each preset gives it a different one. Right-click the view and choose Reset creature to grow a new one.
- **It evolves slowly.** Over minutes of sustained, intense sound the blot grows from a dense mass into a larger, torn, branching one, and the modules doing the work lean its character: SHIMMER, SWIRL and ENSEMBLE make it longer and more fluid, ROAR, GROWL and the clipper heavier and more torn, the VOCODER, RING and OVERTONE quicker to branch, SUB and a low PITCH lower and slower. Quiet lets it settle back. Evolution: Frozen holds its form and colour.
- **Every module leaves its own trace.** Each module changes the blot in its own way (the shape, the flow or the ink itself, never only a tint), and their traces share one limit, so with every module on they still bend one blot. For example: GROWL runs a ruffle along its edge at the growl's rate, SWIRL winds its limbs into spiral arms, SUB heaves the whole mass and hangs a dark pool under it, the VOCODER lays layers through it from its 32 bands, SHIMMER sends pale beads rising through it, CHAMBER turns the cloud into the room's walls, and OVERTONE draws thin wisps up out of it, one for each overtone. The [manual](https://github.com/samueljustice/plugins/blob/main/roarschach/README.md#visualiser) lists them all.
- **OVERTONE draws nothing at low settings.** Its wisps appear from about 10% of Amount and are full from about a third, so the few per cent a new instance has leave the blot alone.
- **Its colour still follows your settings.** The clouds drift towards ember red, electric blue, violet or lichen green-grey to suit the sound, while the ink itself stays near black, with only its thin edges taking the colour.
- **On slower graphics cards it lowers its own detail** to stay smooth, and without OpenGL the view shows a still cloud.

Saved sessions and presets keep every setting, but they grow a different creature from the one they showed when they were saved.

### New: easier to use

- **A bigger window that remembers its size.** A new window opens as large as the display comfortably allows (80% to 100%). The size you last chose is remembered on this computer and used for new instances, and each session keeps its own. The window can now be made as small as 65% (up to 125%, never larger than the screen).
- **Undo for preset loads.** A preset loaded from the preset bar (the list, the arrows or the die) is one step in the same history as the dice. Choose Undo Load in the preset list, or press Cmd+Z (Ctrl+Z on Windows) straight after loading, to get back the sound you had, edits and all; Redo loads the preset again, so you can compare the two.
- **A hint line.** Until a new instance first hears sound, the view shows one line of help after a moment of quiet: play a voice through it to wake the creature, and pick a preset or roll a die to change it.
- **Dimmed knobs.** A knob the current settings do not use is dimmed (it still turns): for example SHIMMER's Rate at Amount 0, SUB's Drive at Level 0, CHAMBER's Reson and Edge with no room blended in, and GROWL's Rate with the Throat shape. A knob that does only a little, such as CHAMBER's Edge at a low Amount, is half-dimmed.
- **Easier to read.** The knob captions are larger and the small text is brighter.
- **Quick start and latency in About.** The About window (the **?** at the top right) has a five-step quick start and shows the latency the plug-in reports to the host, in milliseconds and samples. On a short display it is never taller than the screen.

### Sound fixes

- **PITCH** holds a steady shifted vowel as smoothly as it went in, no longer sags in pitch at the end of a phrase, and no longer combs a steady tone. Size, and Formant on Keep, keep the voice at the same level as Follow.
- **VOCODER** follows the low end of a word without lagging behind it, so the bass of each syllable lands with the word.
- **The carrier pitch of VOCODER and RING Follow** is taken only from clear, voiced sound. Room noise between phrases and the quiet tail of a word no longer leave the carrier an octave off; in a pause it waits at your usual pitch, and the first pitch of a new phrase is taken at once, even when it is much quieter than the last.
- **GROWL**'s Throat shape no longer drops out in the middle of rough or breathy words. S sounds stay unchopped.
- **ROAR** keeps its top octave at 44.1 and 48 kHz: at 15 to 18 kHz it is within about 1 dB of a 96 kHz session.
- **OUTPUT** has less low-end distortion when Push is high: the DC removal after the limiter and the clipper sits lower (2.5 Hz), so it no longer disturbs bass notes.

These change the sound a little, in the direction of what each module was meant to do. Sessions and presets keep every setting.

### Downloads

| File | Contents |
|---|---|
| `Roarschach-1.2.0-macOS-Installer.pkg` | macOS installer (AU + VST3, universal) |
| `Roarschach-1.2.0-Windows-Installer.exe` | Windows installer (VST3) |
| `Roarschach-1.2.0-macOS-AU.zip`, `Roarschach-1.2.0-macOS-VST3.zip`, `Roarschach-1.2.0-Windows-VST3.zip` | The plug-ins on their own |

The macOS downloads are signed and notarised. Universal (Apple silicon and Intel), macOS 11 or later. Windows: 64-bit Windows 10 or later.

The manual, with a tour of every control: [roarschach/README.md](https://github.com/samueljustice/plugins/blob/main/roarschach/README.md).
