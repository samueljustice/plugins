# Swooshbuckler

A whoosh-making rig. Drop in up to ten sounds, press play, and Swooshbuckler rides the faders across them so the layers swell, pass by and fall away. You get a steady stream of doppler passes, swells, hits and tails. Arm the tape and it records every one into a single file, skipping the silence. All that's left is to pick out the good ones.

Swooshbuckler is a standalone app for macOS and Windows, and also a VST3 plug-in (both platforms) and an Audio Unit (macOS).

![Swooshbuckler](docs/screenshot.png)

- [Download](#download)
- [Quick start](#quick-start)
- [How it works](#how-it-works)
- [A tour of the window](#a-tour-of-the-window)
  - [Header bar](#header-bar)
  - [Tracks](#tracks)
  - [Ride](#ride)
  - [Bus A and Bus B](#bus-a-and-bus-b)
  - [Hits](#hits)
  - [Output](#output)
  - [The sea](#the-sea)
  - [Morph pad](#morph-pad)
  - [Modulation](#modulation)
  - [Settings and recording](#settings-and-recording)
  - [About](#about)
- [Presets and RANDOMISE](#presets-and-randomise)
- [Keyboard, mouse and MIDI](#keyboard-mouse-and-midi)
- [Tips](#tips)
- [Building](#building)
- [Releasing](#releasing)
- [Credits](#credits)

## Download

[![Download Swooshbuckler v1.4.0](https://img.shields.io/badge/download-v1.4.0-d00a35?style=for-the-badge)](https://github.com/samueljustice/plugins/releases/tag/swooshbuckler-v1.4.0)

Or get every Sammy J plug-in at once, Swooshbuckler included, with one installer that always has the newest version of each (its file names carry the date and time it was built):

[![Everything for macOS](https://img.shields.io/badge/Everything_for_macOS-000000?style=flat-square&logo=apple&logoColor=white)](https://github.com/samueljustice/plugins/releases/latest)
[![Everything for Windows](https://img.shields.io/badge/Everything_for_Windows-0078D6?style=flat-square&logo=windows&logoColor=white)](https://github.com/samueljustice/plugins/releases/latest)

Swooshbuckler's own release (above, or every version on the [releases page](https://github.com/samueljustice/plugins/releases?q=swooshbuckler)) has these files:

| File | What's in it |
|---|---|
| `Swooshbuckler-x.y.z-macOS-Installer.pkg` | The app, the VST3 and the AU. Click **Customize** to choose which. |
| `Swooshbuckler-x.y.z-macOS-App.dmg` | Just the app. Drag it to Applications. |
| `Swooshbuckler-x.y.z-Windows-Installer.exe` | The app and the VST3. |
| `...-VST3.zip`, `...-AU.zip`, `...-Windows-App.zip` | Each part on its own, for installing by hand. |

macOS builds are universal (Apple silicon and Intel), signed and notarised, and need macOS 11 or later. Windows builds need 64-bit Windows 10 or later.

What's changed in each version is in the [changelog](CHANGELOG.md).

## Quick start

1. **Drop sounds into Tracks**, or click the drop zone to browse. Anything works: cloth, air, engines, animals, metal, voices. Each file gets its own looping track. Swooshbuckler finds the separate sounds in it, guesses what type of sound it is, and sends it to bus A, bus B or the hits.
2. **Press PLAY.** With **AUTO RIDE** on (it is by default), every track's fader moves by itself at its own speed. When the layers swell together you get a whoosh.
3. **Shape it** with Energy, Rate and Chaos, or try a preset or **RANDOMISE**.
4. **Arm the tape** (ARM). It records whenever there's sound and skips the silence. Click again to stop and close the file.
5. Missed a good one? **LAST 60s** saves the last minute, even if the tape wasn't armed.

Hold **SWELL**, the space bar or any MIDI note to push every fader up at once.

Every control has a tooltip: hover over it to see what it does and how to use it.

## How it works

```
 Tracks ─┬─> Bus A (tonal, crackle, rumble, mixed) ─┐
         ├─> Bus B (beds, air)  <── ducked by A ────┼─> Output ─> tape
         └─> Hits (impacts) <── gate opens on A + B ┘
```

1. **Tracks** loop your sounds. The ride moves each track's fader, so the layers rise and fall on their own.
2. Each track's **sound type** decides where it plays:
   - **Bus A** gets Tonal Content, Crackle, Rumble and Mixed Content.
   - **Bus B** gets Energy Bed and Air / Swish.
   - **Hits** gets Impact.
3. **Bus A and Bus B** each turn their tracks into moving sources: doppler passes that fly across the stereo field, with low end, swirl, an envelope filter and drive. Bus A is the key for bus B: when A gets loud it ducks and darkens B, so the two don't pile up.
4. **Hits** only play when the hit gate opens. By default the gate opens at the loudest moments of the two buses, so the impacts land on the biggest swells and push the buses out of the way.
5. **Output** adds echoes and reverb, makes it louder and denser, and limits it. The tape and LAST 60s record exactly what you hear.

The small brass arrows between the sections show this flow, and each section's title bar has a small waveform of the last 3 seconds of what it sends on.

## A tour of the window

### Header bar

![Header bar](docs/images/header.png)

From left to right:

| Control | What it does |
|---|---|
| **Swooshbuckler** title | Click it, or the version number beside the website link under it, to open [About](#about). |
| **PLAY / STOP** | Starts and stops the whole rig. It turns green and reads STOP while running. PLAY starts every track from its start flag and the ride from the top. Starting and stopping fade over about 60 ms, so there are no clicks. Swooshbuckler always opens stopped. |
| **NEW** | A new session. It stops the rig and the tape, empties all ten tracks, turns off every mute and solo, puts every control back to its default, and removes every modulation wire. |
| **<** and **>** | Step through the factory presets, wrapping round at either end. |
| Preset name | Shows the current preset (or "Randomised"). Click it for the list, with **Back to Defaults** at the bottom. See [Presets and RANDOMISE](#presets-and-randomise). |
| **RANDOMISE** | Rolls a random but musical setting and glides to it. |
| Tape (**ARM**) | The tape machine. See [Recording](#recording). It reads **ARM**, **ARMED** (with a blinking red dot, waiting for sound) or **REC mm:ss** (recording, reels spinning). |
| **LAST 60s** | Saves the last minute of output to a file, even if the tape wasn't armed. |
| **SETTINGS** | Opens [Settings](#settings-and-recording): where recordings go and how they're written, and the audio device in the app. |

A status line runs along the bottom of the window. The left side always shows a hint. The right side shows the latest message, such as "Saved ...", which fades after about 8 seconds. Click a "Saved ..." message to show the file in Finder or Explorer.

### Tracks

![Tracks](docs/images/tracks.png)

This is where your sounds live: up to ten files, each on its own looping lane.

**Loading sounds**

- Drag audio files onto the drop zone, or anywhere in the window, or click the drop zone to browse. You can pick several files at once.
- Each file takes the next empty track. Drop a file onto a lane that already has a sound to replace it.
- Swooshbuckler reads WAV, AIFF, FLAC, OGG and MP3, and on macOS also CAF and M4A.
- Files are folded to mono and play at their own sample rate. Only the first 10 minutes of a file are loaded.
- Each track gets an automatic loudness trim, so quiet and loud files sit at similar levels before the fader.
- When you load a file, Swooshbuckler finds the separate sounds in it, keeping their natural tails, and guesses what type of sound it is. The guess uses the audio and words in the file name: "hit", "boom" and "slam" suggest an Impact, "wind", "drone" and "rain" an Energy Bed, "whoosh", "cloth" and "sword" Air / Swish, and so on. Naming your files well gets better guesses.

**A track lane**

![A track lane](docs/images/track-lane.png)

| Part | What it does |
|---|---|
| **Name** | The track is named after its sound type ("Energy Bed", "Tonal Content" and so on) until you rename it. Two tracks of the same type share a name; the lane colour tells them apart. **Double-click** the name to rename it. Return keeps it and Esc cancels. Clear it to go back to the automatic name. |
| **Bus tag** | Where the track plays: **BUS A** (coral), **BUS B** (turquoise) or **HITS** (red), in the colour of that section's knobs. It follows the sound type. |
| **Type badge** | The sound type. Click it to change it, or choose **Auto** to go back to the guess. A ` *` means you set it yourself. The type decides the bus: see the table below. |
| **SKIP SILENCE** | On (the default): only the sounds play and the gaps between them are skipped, so a recording full of pauses becomes a steady source. The sounds are drawn bright and the gaps faint. Off: the whole file loops, silence and all, and the lane reads "silence kept". |
| **Fader** | The track's level, in dB: 30 dB of travel (each 10% is 3 dB), with the last 5% fading to silence; default −2.1 dB. With AUTO RIDE on, it sets how high the ride can take the track. Double-click to reset. Each one is also a host parameter, "Track 1" to "Track 10". |
| **Semitones / Cents** | The track's own pitch: up to 24 semitones either way, plus up to 100 cents for fine tuning. It adds to the Ride's Pitch knob and glides to a new setting. **Drag up or down** on a box to change it, and **double-click** to reset. Each one is also a host parameter ("Track 1 Semitones", "Track 1 Cents" and so on). Drag a MOD jack onto the Semitones box to modulate the track's pitch; its plugs sit in a row under the box. |
| **VARI** | On (the default): varispeed, like tape. The pitch and the speed change together, so up an octave also plays twice as fast. Off: the pitch changes and the speed stays the same, through a pitch shifter, which delays that track by about 43 ms. Synced hits allow for both. |
| **M / S / x** | Mute, solo and remove. A muted track keeps its place in the ride, so it comes straight back in when you unmute it. |
| **Waveform** | The yellow **flag** marks where the track starts playing. It loops from there, and the part before the flag is dimmed. **Click or drag** on the waveform to move it, and **double-click** to put it back at the top. |
| **Ride line** | The line across the waveform traces the track's actual level over the last 8 seconds, like an automation lane scrolling past, with a dot at the right for right now. The thin strip left of the waveform shows the level now. Both include the fader, the ride, and mute and solo. |
| **Playheads** | Thin white lines show where in the file the track is playing (two while it crossfades from one sound to the next). |
| Top left of the waveform | The file length and how many sounds were found in it. |

**Sound types**

| Type | What it's for | Plays on |
|---|---|---|
| Tonal Content | Sounds with a pitch or a voice: growls, squeals, creatures, tones. | Bus A |
| Crackle | Busy, broken-up texture: fire, debris, sparks. | Bus A |
| Rumble | Low end: sub, thunder, rumble. | Bus A |
| Mixed Content | A bit of everything, or no single clear type. | Bus A |
| Energy Bed | Steady sound with body: wind, drones, engines, rain. | Bus B |
| Air / Swish | Air movement: swishes, cloth, flaps, sword swings. | Bus B |
| Impact | Hits and transients. | Hits |

Changing a track's type is how you move it to the other bus.

**Undo**

With the Tracks panel focused (click it first), Cmd+Z (Ctrl+Z on Windows) undoes the last sound you loaded or the last track you removed, on whichever track it was, and selects that track. Shift+Cmd+Z (Ctrl+Shift+Z) redoes it. Remove three tracks and three presses bring them all back. Each track keeps up to 40 steps.

**Hit sync view**

When **SYNC HITS** is on in the Hits section, every track on HITS zooms and slides so the transient of the hit it fired last sits on a shared red dashed line, which flashes on every beat. The lane reads "SYNCED hit N of M". If a track reads "no hits found", turn its SKIP SILENCE off: the whole file then fires as one hit, lined up on its first transient.

### Ride

![Ride](docs/images/ride.png)

This is where the whooshes come from. With AUTO RIDE on, each track's fader rises and falls at its own speed (the ten tracks run at different speeds, so they never move together), and the layers swell, overlap and fall away.

| Control | Range (default) | What it does |
|---|---|---|
| **Swell** | button | Hold it to push every track's fader up to its full level, over the ride. Let go and the ride takes over again. It glides rather than jumps. The space bar and any MIDI note do the same. |
| **AUTO RIDE** | on / off (on) | The faders move by themselves. Off: the movement glides to a stop and each fader stays where you set it. |
| **Energy** | 0–100% (35%) | How deep the ride dips the faders between swells, evenly in dB: 4 dB more for every 10%, to −36 dB at 90% and silence at 100%. Dimmed with AUTO RIDE off, when it does nothing. |
| **Rate** | 25 s to 1.9 s (6.4 s) | How fast the faders move: the readout is the time of one rise and fall. It also sets how long each doppler pass lasts, from about 6 s down to about 1.6 s. |
| **Fill** | 0–100% (15%) | Long, softened echoes at 0.5, 1 and 2 s, after each track's fader, so they hang on into the dips of the ride and the gaps of gappy sounds. |
| **Chaos** | 0–100% (40%) | How unpredictable it all is. At 0% the rides and passes repeat in a steady pattern and every track plays its sounds in order from its start. Higher: each swell gets its own size and speed, every pass its own length, speed, closeness and direction, and tracks more often jump to a random sound, sometimes starting partway in. |
| **Pitch** | ±24 semitones (0) | Finer near the middle. Plays every track higher or lower, like changing tape speed: lower is also slower and heavier. It glides to a new setting and isn't locked to whole semitones. |

Pitch moves every track at once. To pitch one track on its own, with or without changing its speed, use that track's Semitones, Cents and VARI controls in [Tracks](#tracks).

Swell only lifts the faders up to the ride's ceiling, so with AUTO RIDE off (or Energy at 0%) the faders are already there and Swell does nothing you'll hear.

How far each knob goes, and why, is measured in the [knob audit](docs/knob-audit.md).

### Bus A and Bus B

![Bus A](docs/images/bus-a.png)

![Bus B](docs/images/bus-b.png)

Each bus turns its tracks into a moving source: it always runs two overlapping doppler passes, and each one flies across the stereo field. In order, the chain is: doppler passes (with Width and Bend), then Low End, Swirl, Filter and Drive, then the ducking.

Bus A ("growls, tones, crackle, rumble - and the key for B") is the aggressive bus. Bus B ("air and beds, ducked and filtered by A") sits under it.

| Control | Default (A / B) | What it does |
|---|---|---|
| **Doppler** | 55% / 40% | How dramatic each pass is: how close it comes, how much it swells and brightens as it goes by, and (with Bend) how far its pitch dives. |
| **Width** | 70% / 70% | How far each pass travels across the stereo field: at 0% it stays in the middle, at 100% it flies fully from side to side. |
| **Bend** | 70% / 60% | How much the pitch dives as a pass goes by, up to a race-car drop. |
| **Low End** | 35% / 30% | Weight: a sub octave and low overtones, plus a low lift that grows at the bus's loudest moments. |
| **Swirl** | 30% / 30% | Movement inside the sound: a phaser, then (from about a third of the way) a flanger that sweeps as the passes come and go. Its level is matched, so it changes the colour, not the loudness. |
| **Filter** | 30% / 40% | A low-pass filter that opens as each pass swells and closes in its tail, so the tails are dark and the peaks bright, like air rushing past. More blends more of it in and closes it further at the peak; the top 40% adds resonance. |
| **Drive** | 12% / 0% | Saturation, from gentle thickening to a hard, flattened growl. The drive rises evenly in dB and the level stays roughly steady, so it adds character rather than just loudness. |
| **Ducked** (B only) | 35% | How far bus A pushes bus B down at A's peaks, up to −20 dB, judged against A's own recent peak so it works the same on quiet and loud material. |
| **Filtered** (B only) | 30% | How far bus A closes a low-pass filter on bus B at A's peaks, down to about 280 Hz at 100%, so B goes dark under A rather than just quieter. |
| **LINK** (in B's title bar) | off | Bus B flies the same doppler passes as bus A: at the same moment, as close, in the same direction, with A's Doppler and Width. The two buses move as one source, for one consistent whoosh. B keeps its own Bend, so its pitch can dive more or less. While linked, B's own Doppler and Width are dimmed, and they come back as you set them when you unlink. Leave it off to keep energy beds on B moving on their own. |
| **TAKE TURNS** (in B's title bar) | off | Only the louder bus plays at full level and the other ducks under it by the Ducked amount (at 0% Ducked it does nothing), so they trade places. The other bus has to be clearly louder (about 2 dB) for 30 ms before the turn passes. Off: A always ducks B. |

All the knobs run from 0 to 100%. How bus B gets out of the way (turned down, darkened or just its mids) follows the **DUCK** box in the Hits section, and how fast it comes back follows the Hits **Return** knob.

The waveform in each title bar shows the last 3 seconds of what that bus sends on. Bus B's is drawn after A has ducked and filtered it, so a thin line under a busy bus A means the ducking is working.

### Hits

![Hits](docs/images/hits.png)

Every Impact track plays here. The hits jump up and push both buses down out of the way, so each impact punches through the whoosh. There are two ways of timing them:

- **AIM** (on by default): the hits are **aimed at the peaks of the doppler passes** on buses A and B. Each pass is loudest at its middle, when it's closest to you, and that moment is known as soon as the pass starts. So Swooshbuckler looks ahead: when a peak is coming, it decides whether it earns a hit (closer passes on a louder bus score higher, against **Sensitivity**), picks every hit track's next sound, and starts each one early by that sound's own lead-in, so all the transients land right on the peak. It allows for each track's pitch and VARI, and peaks that land together (with LINK, say) fire once. The gate isn't needed, so GATE, Release, Hold, Floor and SYNC HITS rest, and the lamp reads AIMED HITS and flashes on each hit.
- **The hit gate** (AIM off): the hits play only when the gate opens. The gate listens to buses A and B together.

Sessions saved before AIM existed open with it off, so they keep the gate they were made with.

| Control | Range (default) | What it does |
|---|---|---|
| **GATE** box | Peaks / Level / Jumps / Open (Peaks) | What opens the gate. **Peaks**: the loudest moments of the two buses. **Level**: any time they're louder than a level set by Sensitivity. **Jumps**: sudden rises, however loud or quiet. **Open**: no gate, the hits always play. |
| **DUCK** box | Volume / Darken / Mids (Volume) | How a ducked bus gets out of the way: turned down, low-passed, or only its crowded middle (250 Hz to 4 kHz) turned down. This also applies to bus A ducking bus B. |
| **Sensitivity** | 0–100% (50%) | How easily the hits fire. With AIM: which pass peaks get a hit. With the gate: how easily it opens (Level: within 2 dB of the recent peak at 0%, within 30 dB at 100%). Low: only the biggest moments. High: often. With GATE on Open it is dimmed, as are Release, Hold and Floor. |
| **Boost** | −3 to +12 dB (+6 dB) | How loud the hits come in when they fire, against their own level. |
| **Release** | 60 ms to 1.2 s (459 ms) | How long the hits tail out after the gate shuts. |
| **Hold** | 0–500 ms (60 ms) | How long the gate stays open after the last trigger. Longer stops it chattering, so each hit reads as one event. |
| **Floor** | Closed, or −22 to −3 dB, finer near the top (Closed) | How far the gate closes between hits. |
| **Duck** | Off, or down to −12 dB (−4.8 dB) | How far the hits push buses A and B down at their peaks. |
| **Return** | Fast, or 20 ms to 1 s (Fast) | How fast ducked buses come back up. This also applies to bus A ducking bus B. Slower is smoother, with less pumping. |
| **HIT GATE / AIMED HITS** | light | Glows as the gate opens and fades as it closes; with AIM, flashes on each aimed hit. |
| **AIM** | on / off (on) | Aims the hits at the pass peaks (see above). |
| **SYNC HITS** | on / off (off), with AIM off | Every Impact track fires its next hit on one shared beat, so their transients line up into one big hit. Switching it restarts the hit tracks. See the [hit sync view](#tracks). |

The waveform in the title bar shows the last 3 seconds of what got through the gate: each spike is a hit, and the gaps are the gate closed.

### Output

![Output](docs/images/output.png)

The last stage before your speakers and the tape. At its left is the **Peak Tremolo**, a tremolo that shakes only the loudest moments: as the level climbs towards its recent peak, past **Threshold**, the tremolo deepens (up to **Depth**), and it lets go as the level falls, so the dynamics flutter where the whoosh is biggest. It comes before Space, so the echoes and reverb carry it.

| Control | Range (default) | What it does |
|---|---|---|
| Tremolo **On / Off** | menu (Off) | Switches the peak tremolo. It fades in and out. |
| Tremolo wave | Sine / Square / Flutter (Sine) | **Sine**: a smooth throb. **Square**: hard chops, like a helicopter. **Flutter**: a random wobble, like wings or a loose flap. |
| Tremolo shakes | Everything / Buses A + B / Hits (Everything) | What it shakes, and whose level drives it. **Everything**: the whole mix. **Buses A + B**: each bus by its own level, dipping together, while the hits stay solid. **Hits**: only the hits. |
| **Depth** | 0 dB to a full chop (−8.3 dB) | How far the level dips at the very peak (the readout is the deepest dip); 100% chops right down to silence. |
| **Rate** | 1.5–30 Hz (6.7 Hz) | From a slow throb to a buzzing flutter. |
| **Threshold** | All, or −17 to −0.2 dB under the peak (−4.7 dB) | How near its recent peak the level has to be before it starts. Low: it shakes most of the time. High: only right at the top. A steady bed sits at its own peak, so it shakes all the time. |
| **Space** | 0–100% (30%) | Echoes and reverb around the whole mix: a ping-pong echo and a reverb whose room grows as you turn it up. At 0% it's off; at the top it's a big wash that smooths out the swells. |
| **Inflate** | 0–100% (30%) | Louder and denser without new peaks, so quieter detail comes forward. Up to 70% it blends in; the last 30% makes it harder and grittier. At 0% the sound passes through untouched. |
| **Push** | 0 to +15 dB (+3 dB) | How hard the mix drives into the limiter. The ceiling stays put, so it can't clip. |
| **Ceiling** | −6 to 0 dB (−0.3 dB) | The limiter's ceiling, finer near the top. The limiter is always on. |
| **GR** | meter | How much the limiter is turning the sound down right now (a full bar is 12 dB). The number above the meters is the same thing in dB, and turns coral while the limiter is working. |
| **OUT** | meter | The output peak level, after the limiter and the trim. |
| **TRIM** | silence to 0 dB (0 dB) | How loud you hear it (and what your DAW gets from the plug-in), after the limiter. The tape and LAST 60s record before it, at full level, so turning it down never makes a quieter take. Double-click to reset. |

If the gain reduction number is coral all the time, back off Push or raise Ceiling.

### The sea

![The sea](docs/images/sea.png)

The OUTPUT view at the top right pictures what the rig is sending out:

- **The waves** are the output's spectrum, lows on the left and highs on the right. The newest wave is at the front, and older ones roll away towards the horizon.
- **Each doppler pass** is a glowing ball that arcs across the sea. It comes closer the closer the pass, glows brighter the louder it is, and leaves a ripple as it crosses the middle.
- After a moment of silence, the Sweet Justice logo slowly draws itself in the sky.
- While the tape records, a blinking red dot and a "Rec" timer show in the corner.
- **Click the sea** for a short swell.
- **MORPH** (top right) shows or hides the [morph pad](#morph-pad). It reads **MORPH ON** in green while the morph is switched on.

### Morph pad

![Morph pad](docs/images/morph-pad.png)

The morph blends bus A into bus B spectrally, so the two sounds trade tone and pitch rather than just crossfading. Click **MORPH** over the sea to show the pad, then switch the morph on with **ON**. The MORPH button only shows and hides the pad; the morph keeps running when the pad is hidden.

- **ON / OFF** switches the morph itself, with a fade rather than a hard switch. While it's off the pad is veiled and dimmed, but everything still works, so you can set it up first.
- **Blend / Glide / Cross** pick the mode. Changing mode crossfades, so it doesn't click.
- **The dot**: left is bus A and right is bus B in every mode. Up and down depends on the mode:

| Mode | What it does | Bottom | Top |
|---|---|---|---|
| **Blend** | Morphs the tone of A into B's. | **Mix**: both sounds' pitches stay audible. | **Fuse**: one fused hybrid texture. |
| **Glide** | The pitches inside the sound slide from A's to B's. | **Move**: the character moves with the pitch. | **Shape**: the character (formants) stays put while the pitch moves. |
| **Cross** | A vocoder: one bus played through the other's tone. | **B thru A** | **A thru B** |

- **Click or drag** on the pad to move the dot. **Double-click** to put it back in the middle.
- **Wander** (the play button at the top right) makes the dot move along a shape by itself, so the morph keeps moving. The whole shape is drawn faintly, with a bright trail behind the dot. Click it again, or click the pad, to stop.
  - The **speed slider**, along the bottom between the Bus A and Bus B labels, sets how fast it goes, from a slow drift to busy.
  - The **shape menu**, beside it, picks the path:
    - **Lorenz** (the default): a chaotic loop that swaps between two wings and never repeats.
    - **Sweet Justice**: the outline of the Sweet Justice Sound logo.
    - **Figure Eight**, **Rose** and **Circle**: these turn slowly, one degree per lap, so they never repeat exactly.
- The **Bus A** and **Bus B** labels under the pad show the two sources. The morph always runs between the two buses.

The wandering dot is sent to your host as Morph X / Morph Y changes, so the dot only wanders while the Swooshbuckler window is open, and a host recording automation will record it.

### Modulation

![Modulation](docs/images/modulation.png)

Four modulators run along the bottom: **MOD 1** (yellow), **MOD 2** (pink), **MOD 3** (green) and **MOD 4** (violet). Each can be one of four types, picked from the menu at its top right:

| Type | What it does | Knob |
|---|---|---|
| **LFO** | A repeating wave that swings each wired knob up and down. The shape menu offers Sine, Triangle, Ramp (rises then drops), Square and Steps (a new random value every cycle). | **Rate**: one cycle every 20 s (0.05 Hz) to 8 Hz |
| **Random** | A smooth random wander up and down, never the same twice. | **Rate**: as above |
| **Follower** | Follows the loudness of **Bus A**, **Bus B**, the **Hits** or the whole **Output** (pick which in the menu under the scope), and turns its knobs one way only. | **Speed**: slow and smooth (2 s) to fast and jumpy (15 ms) |
| **Macro** | One knob that turns every knob it's wired to, each by its own depth. It's a host parameter, so you can automate it. | **Amount** |

The defaults are MOD 1: LFO (Sine), MOD 2: LFO (Triangle), MOD 3: Random, MOD 4: Follower (Bus A). The scope on each card shows what the modulator has done over the last few seconds.

**Wiring**

![Cables](docs/images/cables.png)

- **Drag from a modulator's jack** (the round socket at its top left) onto a knob. A cable follows the mouse, and the knob under it lights up; drop it there to wire it. Drop it anywhere else to cancel. Drag from the same jack again to wire more knobs.
- You can wire every knob in Ride, Bus A, Bus B, Hits and Output (the Peak Tremolo's included), and each track's **Semitones** box, which modulates that track's pitch smoothly (a 100% wire swings two octaves). You can't wire the morph pad, the track faders, or the modulators' own knobs.
- Each wire leaves a **plug**, a small coloured dot on the knob's rim, and draws an arc showing how far it moves the knob:
  - **Cmd-drag the control itself** (Ctrl-drag on Windows) to set the depth of its newest wire: up for more, down through zero for negative (the wire then moves the control the other way). A bubble shows the amount. Hold Shift for fine control. **Cmd-double-click** (Ctrl on Windows) sets it back to +50%.
  - Or **drag the plug up or down**, from −100% to +100%. New wires start at +50%. Hold Shift for fine control.
  - A negative depth draws its arc **dashed**, so you can see at a glance which way a wire pushes.
  - **Double-click** a plug to set it to +50%.
  - **Right-click** a plug for a menu: Invert, Depth 25%, 50% or 100%, or Pull Out the Wire.
  - **Alt-click** a plug to pull the wire out straight away.
- **CLEAR** on a card pulls out all of that modulator's wires.
- **CABLES** (top right) shows or hides the cables. The plugs stay on the knobs either way.
- Several wires on one knob add together. You can have up to 48 wires in all.
- Wires only move the value that reaches the sound. The knob itself never moves, so your DAW automation stays clean.
- Wires and modulator settings are saved with the session. Presets and RANDOMISE don't touch them, and NEW pulls them all out.

Some ideas: follow **Bus A** with a wire on bus B's Filter to make B react to A, follow **Hits** with a wire on Space to make every impact open up the room, or put a slow Random on Pitch.

### Settings and recording

![Settings](docs/images/settings.png)

Click **SETTINGS** to open. Close it with CLOSE, Esc, or a click outside the panel. These settings belong to Swooshbuckler itself, not to a session, and are shared by the app and every copy of the plug-in.

| Setting | Options (default) | What it does |
|---|---|---|
| **Recordings Are Saved To** | (your Desktop) | **CHOOSE...** picks a folder, **DESKTOP** goes back to the default, and **SHOW** opens the folder. |
| **File Type** | WAV / AIFF / FLAC (WAV) | WAV files also carry broadcast-wave details. FLAC is lossless and about half the size. |
| **Bit Depth** | 16-Bit / 24-Bit / 32-Bit Float (24-Bit) | 32-Bit Float can't clip in the file, and is WAV only. |
| **Sample Rate** | Session / 44.1 / 48 / 88.2 / 96 / 192 kHz (Session) | Session records at the audio device's or host's rate. Pick a higher rate and the whole rig runs at that rate, so the recording keeps the extra high end, which is good for pitching down later; you still hear it at the device rate. A note in the panel says which is happening. It can't be changed while the tape is armed. |
| **Channels** | Stereo / Mono (Stereo) | Mono folds the output to one channel, for sounds you'll pan yourself. |
| **Arm the tape as soon as Swooshbuckler opens** | (off) | So you never miss the first good take. |
| **AUDIO DEVICE...** | | The app only: the audio output, sample rate and buffer size. |

#### Recording

- **The tape (ARM).** Click to arm. While armed, the tape writes whenever the output is above about −56 dBFS. It keeps a quarter second before each sound and 2 seconds after it, so attacks and tails survive, and leaves the silence out. Everything goes into one file until you click the tape again, press NEW or close Swooshbuckler. The REC timer counts only what's been written.
- **LAST 60s.** Swooshbuckler always keeps the last minute of output while it plays. Click LAST 60s to save it, even if the tape wasn't armed and even while a take is going. Silence at the start is trimmed. It's always written as a stereo WAV at the rig's rate, in the bit depth you've chosen.
- **What's recorded** is the final output, after Space and the limiter: exactly what you hear.
- **File names** look like `Swooshbuckler_2026-11-07_16-32-10_48213.wav`. The number on the end keeps two takes in the same second apart. LAST 60s files end in `_retro`.
- File Type, Bit Depth, Sample Rate and Channels are fixed when you arm, so changes apply to the next take.

### About

![About](docs/images/about.png)

Click the **Swooshbuckler** title or the version number to open it. It shows the version, which format you're running (app, VST3 or AU), the build date and commit, and the credits. **COPY VERSION** copies the version, build date and commit, plus your operating system, for a bug report. **WEBSITE** opens sweetjusticesound.com.

## Presets and RANDOMISE

The factory presets are **Steady Energy**, **Jet Passes**, **Monster Swells**, **Gentle Breeze**, **Fast and Furious**, **Magic Dust** and **Heavy Hits**. Pick one from the preset name, or step through them with **<** and **>**. Every control glides to its new setting while it plays, so you can switch mid-whoosh. Grab a knob during the glide and your move wins.

**RANDOMISE** rolls Energy, Rate, Fill, Chaos and Pitch (whole semitones from −7 to +7), the bus knobs (Doppler, Low End, Swirl, Filter and Drive, plus Ducked and Filtered on B), the hit knobs (Sensitivity, Boost, Release and Duck) and Space.

Presets and RANDOMISE set the sound, not your session. They leave these alone:

- your tracks, their levels and each track's pitch and VARI
- Inflate, Push, Ceiling, TRIM and the Peak Tremolo
- AUTO RIDE, AIM, SYNC HITS, the GATE and DUCK modes, TAKE TURNS and LINK
- the morph's ON switch, wander, shape and speed
- the modulators and their wires

Anything else a preset doesn't set goes back to its default. **Back to Defaults** at the bottom of the preset list puts every one of those controls back to its default, and leaves the same things alone. It's much gentler than NEW, which also empties the tracks, resets everything else and pulls out the wires.

## Keyboard, mouse and MIDI

| Input | What it does |
|---|---|
| Space bar (hold) | Swell. With a track lane selected, it gives a short swell instead: click outside Tracks for a held one. |
| Any MIDI note (hold) | Swell, for as long as any note is held. Notes only work while the rig is playing. |
| Click the sea | A short swell. |
| Cmd+Z / Shift+Cmd+Z (Ctrl+Z / Ctrl+Shift+Z on Windows) | Undo and redo the last load or removal in Tracks, newest first. Click the Tracks panel first. |
| Esc | Closes Settings or About. |
| Double-click a knob | Resets it. |
| Double-click a track name | Renames it. |
| Double-click a waveform | Puts the start flag back at the top. |

## Tips

- **Start with beds and a tonal layer.** A couple of Energy Beds on bus B and one or two growls or tones on bus A make a whoosh straight away. Add impacts for punctuation.
- **Gappy recordings** (single swishes, takes with pauses) work best with SKIP SILENCE on and a little Fill.
- **For tight synced hits**, put two or three impacts on HITS, turn on SYNC HITS and raise Boost.
- **For monster passes**, lower Pitch, raise Low End and Bend, and slow the Rate.
- **Two takes of one sound**: load the same file on two tracks and detune one by 10 to 20 cents with VARI off, for a thicker, chorused layer.
- **Keep a hit's timing but change its size**: pitch an impact down with VARI off and it lands on the same beat, just heavier.
- **For one clean pass instead of two layers flying about**, turn on LINK so bus B follows bus A's passes. Turn it off when B carries a bed that should keep moving on its own.
- **If the buses fight**, turn on TAKE TURNS, or raise Ducked and Filtered on bus B.
- **Record at 96 or 192 kHz** if you plan to pitch the whooshes down later.
- **Leave the tape armed** while you play with the controls. It only writes when there's sound, so you just need to listen back for the good ones.

## Building

### Prerequisites
- CMake 3.21 or later
- Xcode command line tools (macOS) or Visual Studio 2022 (Windows)
- JUCE 8.0.8 at `../SDK/JUCE`, relative to this folder

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

This builds the app, the VST3 and the AU (macOS) into `build/Swooshbuckler_artefacts/Release/`, along with three offline test programs: `SwooshbucklerRig`, `SwooshbucklerCargo` and `SwooshbucklerClassify`. Pass `-DSWOOSHBUCKLER_TESTS=OFF` to skip the tests, or `-DSWOOSHBUCKLER_FORMATS="Standalone;VST3"` to choose the formats.

### Version

The version lives in one place, `SWOOSHBUCKLER_VERSION` in [CMakeLists.txt](CMakeLists.txt). Release builds override it from the tag. The copyright year in the About panel is the year it was built.

### Icon

The icon is drawn in code, in [tools/IconRender.cpp](tools/IconRender.cpp). After changing it, regenerate `Resources/icon.png` and `Resources/icon.ico` and commit them:

```bash
cmake -B build-icon -DSWOOSHBUCKLER_TOOLS=ON -DSWOOSHBUCKLER_TESTS=OFF -DSWOOSHBUCKLER_FORMATS=Standalone
cmake --build build-icon --target SwooshbucklerIcon
./build-icon/SwooshbucklerIcon_artefacts/SwooshbucklerIcon Resources/icon.png
```

## Releasing

Releases are built by [build-release_swooshbuckler.yml](../.github/workflows/build-release_swooshbuckler.yml). Push a tag and it builds, tests, signs and publishes everything:

```bash
git tag swooshbuckler-v1.0.0
git push origin swooshbuckler-v1.0.0
```

The number in the tag becomes the version everywhere: the About panel, the bundles and both installers.

When the release finishes, the combined Sammy J installer ([build-release_bundle.yml](../.github/workflows/build-release_bundle.yml)) rebuilds itself with the new version, so the "Everything" downloads pick it up. Bump the download button at the top of this README and the root README to the new version.

- **macOS**: uses the same signing secrets as the other plug-ins. The p12 must hold both the Developer ID Application and the Developer ID Installer certificates. `APPLE_ID`, `APPLE_APP_PASSWORD` and `APPLE_TEAM_ID` are for notarisation.
- **Windows**: add `WINDOWS_CERTIFICATE_PFX_BASE64` and `WINDOWS_CERTIFICATE_PASSWORD` (a code-signing certificate as base64 .pfx) to sign the app, the VST3 and the installer. Without them the Windows build is unsigned.

You can also run the workflow by hand (Actions → Build and Release Swooshbuckler → Run workflow) to get the files without publishing a release.

## Credits

- Made by Samuel Justice at [Sweet Justice Sound](https://www.sweetjusticesound.com).
- Fonts: [Pirata One](https://fonts.google.com/specimen/Pirata+One) by Rodrigo Fuenzalida and Nicolas Massi, and [IBM Plex Sans Condensed](https://github.com/IBM/plex) by IBM, both under the SIL Open Font License 1.1.
- Built with [JUCE](https://juce.com).
- VST is a registered trademark of Steinberg Media Technologies GmbH.

## License

MIT License. See [LICENSE](LICENSE).
