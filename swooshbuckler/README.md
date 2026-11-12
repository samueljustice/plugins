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
- [In a DAW](#in-a-daw)
- [Presets and RANDOMISE](#presets-and-randomise)
- [Keyboard, mouse and MIDI](#keyboard-mouse-and-midi)
- [Tips](#tips)
- [Building](#building)
- [Releasing](#releasing)
- [Credits](#credits)

## Download

[![Download Swooshbuckler v1.4.2](https://img.shields.io/badge/download-v1.4.2-d00a35?style=for-the-badge)](https://github.com/samueljustice/plugins/releases/tag/swooshbuckler-v1.4.2)

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

No sounds to hand? Click **Load Demo Sounds** in the empty Tracks panel. It fills four tracks with a growl, a wind bed, air swishes and hits, so you can press PLAY and hear the rig straight away.

1. **Drop sounds into Tracks**, or click the drop zone to browse. Anything works: cloth, air, engines, animals, metal, voices. Each file gets its own looping track. Swooshbuckler finds the separate sounds in it, guesses what type of sound it is, and sends it to bus A, bus B or the hits.
2. **Press PLAY.** With **AUTO RIDE** on (it is by default), every track's fader moves by itself at its own speed. When the layers swell together you get a whoosh.
3. **Shape it** with Energy, Rate and Chaos, or try a preset or **RANDOMISE**.
4. **Arm the tape** (ARM). It records whenever there's sound and skips the quiet stretches. Click again to stop and close the file.
5. Missed a good one? **LAST 60s** saves the last minute, even if the tape wasn't armed.

Hold **SWELL**, the space bar or any MIDI note to push every fader up at once. Press **Throw** (or the F key) for one whoosh on demand, with the rig playing or stopped. Return is PLAY / STOP, R is RANDOMISE, T arms the tape and L saves the last minute: see [Keyboard, mouse and MIDI](#keyboard-mouse-and-midi).

Every control has a tooltip: hover over it to see what it does and how to use it.

## How it works

```
 Tracks ─┬─> Bus A (tonal, crackle, rumble, mixed) ─┐
         ├─> Bus B (beds, air)  <── ducked by A ────┼─> Output ─> tape
         └─> Hits (impacts) <── gate opens on A + B ┘
```

1. **Tracks** loop your sounds. The ride moves each track's fader, so the layers rise and fall on their own.
2. Each track's **sound type** decides where it plays, unless you choose the bus yourself on the lane's bus tag:
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
| **PLAY / STOP** | Starts and stops the whole rig. It turns green and reads STOP while running. PLAY starts every track from its start flag and the ride from the top. PLAY fades in over about 60 ms. STOP lets go of the sounds and lets the Fill echoes, the passes in flight and Space ring out, in your speakers and on the tape, until they have died away (6 seconds at most, then a short fade), so a take ends with its tail and not a cut. Press PLAY during the tail to start again at once (even straight after STOP: sounds that are still letting go fade out in a few milliseconds first, so the restart doesn't click). The app always opens stopped. In a DAW, Play is a parameter you can automate, and it is saved with the project, so a project saved while playing plays when it is opened. |
| **Follow Host** (plug-in only) | On (the default): the rig plays while your DAW's transport plays and stops when it stops, letting the tail ring out. Every transport start begins the ride again from the same place, with the modulators and the morph's Wander starting over too, so the same settings bounce the same way every time. PLAY still starts and stops the rig by hand in between. Off: only PLAY, or its automation, starts and stops the rig. Saved with the project. See [In a DAW](#in-a-daw). |
| **NEW** | A new session. It stops the rig and the tape, empties all ten tracks, turns off every mute and solo, puts every control back to its default, removes every modulation wire and forgets the session's file. One **Undo** brings all of it back. |
| **Undo** and **Redo** (the two curved arrows) | Take back, and do again, the last thing you changed. See [Undo and redo](#undo-and-redo). |
| **<** and **>** | Step through the presets, the factory ones and then your own, wrapping round at either end. |
| Preset name | Shows the current preset (or "Randomised"). Click it for the list: the factory presets, your own, saving and renaming, A / B and the session. See [Presets and RANDOMISE](#presets-and-randomise). |
| **A** / **B** | Which of the two sets of settings you are on. Click it to go to the other. See [A / B](#a--b). |
| **RANDOMISE** | Rolls a random but musical setting and glides to it. |
| The two triangles | **Previous Roll** and **Next Roll**: step back and forward through the last 20 rolls. See [The roll history](#the-roll-history). |
| The star | **Keep This Roll**: saves the settings as they are as a preset of your own, called Roll, Roll 2 and so on. |
| Tape (**ARM**) | The tape machine. See [Recording](#recording). It reads **ARM**, **ARMED** (with a blinking red dot, waiting for sound) or **REC mm:ss** (recording, reels spinning). |
| **LAST 60s** | Saves the last minute of output to a file, even if the tape wasn't armed. With **Keep the Last** in Settings at 3 or 5 minutes it reads LAST 3 MIN or LAST 5 MIN and saves that much. |
| **Variations** | Opens [Render Variations](#render-variations): a batch of whooshes rendered straight to files. While a batch renders it lights up and reads **Render 37%**. |
| **Takes** | Opens [Takes](#takes): the recordings of this run, to hear, drag out, keep or delete. The number beside it is how many there are. |
| **SETTINGS** | Opens [Settings](#settings-and-recording): where recordings go and how they're written, and the audio device in the app. |

A status line runs along the bottom of the window. The left side always shows a hint. The right side shows the latest message, such as "Saved ...", which fades after about 8 seconds. Click a "Saved ..." message to open [Takes](#takes), where the file is at the top of the list.

### Tracks

![Tracks](docs/images/tracks.png)

This is where your sounds live: up to ten files, each on its own looping lane.

**Loading sounds**

- **Load Demo Sounds**, the button in the empty drop zone, fills four tracks with sounds Swooshbuckler makes on the spot (nothing is read from disk): **Demo Growl**, a creature's voice in five phrases, on bus A; **Demo Wind Bed**, eight seconds of gusting wind in stereo that loops without a seam, on bus B; **Demo Swish**, six air swishes of different lengths, on bus B; and **Demo Hit**, a thump, a crack, a clang and a boom, on the hits. They are the same every time, they behave like any sound you load, and one Undo (Cmd+Z) takes all four out again. With fewer than four tracks free it loads as many as fit.
- Drag audio files onto the drop zone, or anywhere in the window, or click the drop zone to browse. You can pick several files at once.
- Each file takes the next empty track. Drop a file onto a lane that already has a sound to replace it.
- Swooshbuckler reads WAV, AIFF, FLAC, OGG and MP3, and on macOS also CAF and M4A.
- Every sound plays from its mono sum, which is what a doppler pass needs: one point that moves. A stereo file whose two sides differ also keeps its stereo width, for beds on bus B (see [Stereo beds](#stereo-beds)); a stereo file with the same sound on both sides is stored as mono. Files with more channels (up to 7.1.4) are mixed down to stereo first. A stereo sound takes about twice the memory of a mono one: roughly 45 MB a minute at 48 kHz, against 22 MB.
- Files play at their own sample rate. Only the first 10 minutes of a file are loaded; when a longer file is cut, the status line says so ("Loaded the first 10:00 of 23:41").
- The status line accounts for every file you add: how many went in, how many didn't fit (ten tracks at most), and any that couldn't be read.
- Each track gets an automatic loudness trim, so quiet and loud files sit at similar levels before the fader. A peaky sound, such as a hit or a click, is only lifted until its peak reaches full scale, never past it.
- Sounds stay clean when they play faster than they were recorded: pitched up (by Pitch or a track's Semitones), or a 96 kHz file in a 48 kHz session. Anything pushed above the top of the audible range is filtered out instead of folding back down as harsh, off-pitch tones. A sound playing at its own speed is untouched.
- When you load a file, Swooshbuckler finds the separate sounds in it, keeping their natural tails, and guesses what type of sound it is. The guess uses the audio and words in the file name: "hit", "boom" and "slam" suggest an Impact, "wind", "drone" and "rain" an Energy Bed, "whoosh", "cloth" and "sword" Air / Swish, and so on. Naming your files well gets better guesses.

**A track lane**

![A track lane](docs/images/track-lane.png)

| Part | What it does |
|---|---|
| **Name** | The track is named after its sound type ("Energy Bed", "Tonal Content" and so on) until you rename it. Two tracks of the same type share a name; the lane colour tells them apart. **Double-click** the name to rename it. Return keeps it and Esc cancels. Clear it to go back to the automatic name. |
| **Bus tag** | Where the track plays: **BUS A** (coral), **BUS B** (turquoise) or **HITS** (red), in the colour of that section's knobs. **Click it** to choose: **Auto** (the bus the sound type picks, the default), **Bus A**, **Bus B** or **Hits**. A ` *` on the tag means you chose it yourself. See [Choosing the bus](#choosing-the-bus). |
| **Star** | Makes this track the **Master**: the one sound the others can follow. Only one track is the Master at a time; click the lit star again to have none. See [The Master track](#the-master-track). |
| **Type badge** | The sound type. Click it to change it, or choose **Auto** to go back to the guess. A ` *` means you set it yourself. On Auto the type decides the bus: see the table below. |
| **SKIP SILENCE** | On (the default): only the sounds play and the gaps between them are skipped, so a recording full of pauses becomes a steady source. The sounds are drawn bright and the gaps faint. Off: the whole file loops, silence and all, and the lane reads "silence kept". |
| **Fader** | The track's level, in dB: 30 dB of travel (each 10% is 3 dB), with the last 5% fading to silence; default −2.1 dB. With AUTO RIDE on, it sets how high the ride can take the track. Double-click to reset. Each one is also a host parameter, "Track 1" to "Track 10". |
| **Semitones / Cents** | The track's own pitch: up to 24 semitones either way, plus up to 100 cents for fine tuning. It adds to the Ride's Pitch knob and glides to a new setting. **Drag up or down** on a box to change it, and **double-click** to reset. Each one is also a host parameter ("Track 1 Semitones", "Track 1 Cents" and so on). Drag a MOD jack onto the Semitones box to modulate the track's pitch; its plugs sit in a row under the box. |
| **VARI** | On (the default): varispeed, like tape. The pitch and the speed change together, so up an octave also plays twice as fast. Off: the pitch changes and the speed stays the same, through a pitch shifter, which delays that track by about 43 ms. The shifter holds the level of the sound (shifting up only loses what goes above the top of the audible range) and keeps the front of a hit clean. A pitched track is about as loud with VARI on as with it off; pitched up, VARI on carries a small make-up (1 dB per octave) for the top end the faster playback has to filter away. Synced hits allow for both. |
| **M / S / x** | Mute, solo and remove. A muted track keeps its place in the ride, so it comes straight back in when you unmute it. |
| **Waveform** | The yellow **flag** marks where the track starts playing. It loops from there, and the part before the flag is dimmed. **Click or drag** on the waveform to move it, and **double-click** to put it back at the top. The flag is saved with the session; loading a new sound onto the track puts it back at the top. |
| **Ride line** | The line across the waveform traces the track's actual level over the last 8 seconds, like an automation lane scrolling past, with a dot at the right for right now. The thin strip left of the waveform shows the level now. Both include the fader, the ride, and mute and solo. |
| **Playheads** | Thin white lines show where in the file the track is playing (two while it crossfades from one sound to the next). |
| Top left of the waveform | The file length and how many sounds were found in it. |
| **Right-click** a lane | The track's menu: **Master Track**, **Match Pitch To Master** and **Plays On** (the bus). |

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

#### Choosing the bus

A track plays on the bus its sound type picks until you say otherwise. Click the **bus tag** on its lane (or right-click the lane and open **Plays On**) and choose:

- **Auto**: the bus the type picks, as in the table above. The menu says which that is. This is the default.
- **Bus A**, **Bus B** or **Hits**: the track plays there whatever its type. The tag is marked ` *`.

The type, and the name the track takes from it, stay as they are, so a growl can sit under the beds on bus B, or a swish can go through the hit gate, and still be called what it is. The choice is saved with the session and can be undone. Dropping a new sound on the lane puts it back on Auto.

#### The Master track

![The Master star and a bus chosen by hand](docs/images/track-master.png)

One track can be the **Master**: the sound you want everything else to move like. Click the **star** on its lane; it lights, and any other track that was the Master lets go. Click it again to have no Master.

When a track becomes the Master, Swooshbuckler works out how that sound moves over its length, every 10 ms: how loud it is, what pitch it is at, and how bright it is. Each is measured over the sound's own range (its quietest to its loudest moment, its lowest to its highest pitch, its dullest to its brightest). A long file takes a moment. They are worked out again whenever you edit the Master or drop a new sound on its lane. Three things use them:

- **Follower modulators.** A [Follower](#modulation) can follow **Master**, **Master Pitch** or **Master Brightness**: the Master's loudness, pitch or brightness at the moment it is playing. Wire it to any knob, and that knob moves with the Master sound: bus B's Filter opening as the Master gets brighter, Space growing as it gets louder, another track's Semitones rising with its pitch.
- **Match Pitch To Master.** Right-click a pitched track's lane and choose **Match Pitch To Master**: its Semitones and Cents are set so the pitch it sits at most of the time is the Master's, to the nearest cent. The Master's own Semitones and Cents count. If the two are more than two octaves apart, the nearest octave in reach is used. A sound with no clear pitch (wind, noise, a hit) is left alone, and the status line says so. It is one undo step.
- **Follow Master**, in the [Ride](#ride) panel: every other track's ride swells with the Master's loudness instead of its own slow wave.

The Master is saved with the session. Removing its track leaves no Master; undo brings it back.

**Undo**

Cmd+Z (Ctrl+Z on Windows), or the Undo arrow in the header, undoes the last sound you loaded or the last track you removed, on whichever track it was, and selects that track. Shift+Cmd+Z (Ctrl+Shift+Z) redoes it. Remove three tracks and three presses bring them all back. Track edits share one history with the knobs and presets, so everything is taken back in the order you did it: see [Undo and redo](#undo-and-redo). Each track keeps up to 40 steps, and all the steps together hold up to about 500 MB of audio: past that the oldest edits are forgotten first, and the latest one can always be undone.

**Hit sync view**

When **SYNC HITS** is on in the Hits section, every track on HITS zooms and slides so the transient of the hit it fired last sits on a shared red dashed line, which flashes on every beat. The lane reads "SYNCED hit N of M". If a track reads "no hits found", turn its SKIP SILENCE off: the whole file then fires as one hit, lined up on its first transient.

### Ride

![Ride](docs/images/ride.png)

This is where the whooshes come from. With AUTO RIDE on, each track's fader rises and falls at its own speed (the ten tracks run at different speeds, so they never move together), and the layers swell, overlap and fall away.

| Control | Range (default) | What it does |
|---|---|---|
| **Swell** | button | Hold it to push every track's fader up to its full level, over the ride. Let go and the ride takes over again. It glides rather than jumps. It is a push: held for longer than a couple of seconds, the mix eases back to its usual level with every fader still up (see the note under the table). The space bar does the same, and so does any MIDI note while Keys is set to Swell. |
| **Throw** | button | Throws one whoosh now. See [Throw](#throw) below. The F key does the same. |
| **AUTO RIDE** | on / off (on) | The faders move by themselves. Off: the movement glides to a stop and each fader stays where you set it. The doppler passes then do the moving: Low End, Swirl and Filter follow how close each pass is. |
| **Follow Master** | on / off (off) | On: every track's ride swells with the [Master track](#the-master-track)'s loudness instead of its own slow wave, so all the layers move like that one sound. See the note under the table. It waits, dimmed, until AUTO RIDE is on and a track is the Master. |
| **Energy** | 0–100% (35%) | How deep the ride dips the faders between swells, evenly in dB: 4 dB more for every 10%, to −36 dB at 90% and silence at 100%. Dimmed with AUTO RIDE off, when it does nothing. |
| **Rate** | 25 s to 1.9 s (6.4 s) | How fast the faders move: the readout is the time of one rise and fall. It also sets how long each doppler pass lasts, from about 6 s down to about 1.6 s. |
| **Fill** | 0–100% (15%) | Long, softened echoes at about 0.5, 1 and 2 s (each track has its own times, so layered tracks don't echo in step), after each track's fader, so they hang on into the dips of the ride and the gaps of gappy sounds. |
| **Chaos** | 0–100% (40%) | How unpredictable it all is. At 0% the rides and passes repeat in a steady pattern and every track plays its sounds in order from its start. Higher: each swell gets its own size and speed, every pass its own length, speed, closeness and direction, and tracks more often jump to a random sound, sometimes starting partway in. |
| **Pitch** | ±24 semitones (0) | Finer near the middle. Plays every track higher or lower, like changing tape speed: lower is also slower and heavier. It glides to a new setting and isn't locked to whole semitones. |
| **Throw Length** | Rate, or 0.3–6 s (Rate) | How long a thrown whoosh lasts. Its peak is halfway through. Fully left it reads **Rate**: a throw is as long as the ride's own passes, which Rate sets (6 s down to 1.6 s). |
| **Keys** | Swell / Pitch / Throw (Swell) | What MIDI notes do. See [Keys](#keys) below. |

Pitch moves every track at once. To pitch one track on its own, with or without changing its speed, use that track's Semitones, Cents and VARI controls in [Tracks](#tracks).

Swell only lifts the faders up to the ride's ceiling, so with AUTO RIDE off (or Energy at 0%) the faders are already there and Swell does nothing you'll hear.

**Follow Master** swaps each track's own rise and fall for the Master's: when the Master sound is loud the other faders are up, and when it falls away they dip. The Master itself plays as it is, unridden, since its own sound is the swell. Energy still sets how deep the dips go: at 100% the other layers follow the Master all the way down to silence, at low settings they only lean with it. The faders rise quickly with the Master (about 15 ms) and fall a little slower (about 90 ms). Rate still sets the length of the doppler passes. Follow Master is a host parameter and is saved with the session; presets and RANDOMISE leave it alone.

**The level stays put when the ride changes.** With every fader up all the time (AUTO RIDE off, Energy below its default, or Swell held) the tracks add up to more than they do while the ride dips them. Swooshbuckler eases the mix down to match, by up to about 5 dB with the faders fully still, so the rig reaches Inflate and the limiter about as loud as with the default ride, and switching AUTO RIDE on and off compares the movement, not the loudness. It follows Energy and AUTO RIDE as they glide, and a held Swell over about 2.5 seconds, so a short Swell is still a push. From the default Energy (35%) up, nothing is changed.

How far each knob goes, and why, is measured in the [knob audit](docs/knob-audit.md).

#### Throw

**Throw** fires exactly one whoosh: a single doppler pass, the same on both buses, with a long approach, the peak as it goes by and a shorter tail (the shape of a [Fly-By](#bus-a-and-bus-b) pass). It plays your tracks at their fader levels, through each bus's own Low End, Swirl, Filter and Drive, and with **AIM** on the hit tracks land on its peak.

- **While the rig plays**, the throw goes on top of the ride.
- **With the rig stopped**, you hear just that one whoosh, its tail rings out, and it is silent again. So you can make single whooshes one at a time without the endless ride: arm the tape, press Throw, and each whoosh is recorded with the quiet between them skipped.
- **The peak is heard half a Throw Length after you throw.** While a throw is on its way in, the top of the Ride panel counts down to it ("Peak in 1.2 s"), so you can place it against picture. In the sea a thrown whoosh is drawn as a comet in its own colour.
- Up to four throws can be in flight at once. More than that are dropped until one has finished.
- A throw's hit is as loud with the rig playing as from rest: the hit track plays once, at its fader level.
- A throw takes its own path into the buses: Fill's echoes and the Morph work on the ride, not on a throw.
- Throw from the button, the **F** key, or a MIDI note with Keys set to Throw.

#### Keys

The **Keys** menu at the top right of the Ride panel sets what MIDI notes do. The notes you are holding are shown at the top of the panel ("C3 E3 G3"). In the app, every connected MIDI keyboard is listened to; in a DAW, send notes to the plug-in.

| Keys | What notes do |
|---|---|
| **Swell** | Any held note holds Swell. |
| **Pitch** | The held notes set the pitch. Each note counts as its distance from middle C (MIDI note 60) in semitones, added to the Pitch knob and each track's own Semitones. One note moves every track. Hold several and they are dealt out over your loaded tracks in order: the first track takes the lowest note, the next track the next note, going round again when there are more tracks than notes. So a chord gives each layer its own pitch. Let go of every note and the pitch glides back to the knob. |
| **Throw** | Every note throws one whoosh at that note's pitch. How hard you play sets how close and how loud it passes. Notes played together (within 30 ms) make one whoosh, with their pitches dealt out over the tracks as in Pitch. The pitch stays where the last notes put it until you play again. |

Notes glide from one pitch to the next, so playing never clicks. Each track follows its own VARI switch (on: pitch and speed move together; off: the pitch moves and the speed stays), and the total never goes past two octaves either way. In Swell and Pitch, notes act while the rig is playing; in Throw, a note wakes a stopped rig for its one whoosh.

### Bus A and Bus B

![Bus A](docs/images/bus-a.png)

![Bus B](docs/images/bus-b.png)

Each bus turns its tracks into a moving source: it runs two doppler passes, half a pass apart, and each one flies across the stereo field. **PASS**, the menu in each bus's title bar, sets how the two follow each other: overlapped into a steady level (Bed) or one at a time (Fly-By). In order, the chain is: doppler passes (with Width and Bend), then Low End, Swirl, Filter and Drive, then the ducking. A pass that comes in fast plays the sound up to twice as fast, and it stays clean: whatever that pushes above the top of the audible range is filtered out instead of folding back down as whistles.

Low End, Swirl and Filter move with the sound: they follow the bus's level against its own recent peak, which with AUTO RIDE on is mostly the ride's swells. The stiller the faders are (AUTO RIDE off, a low Energy, or Swell held), the more they follow how close the nearer pass is instead, so the colours keep moving with the passes you see on the sea when the level doesn't.

#### Stereo beds

A wide ambience should stay wide. When a stereo sound plays on **Bus B** with **PASS** set to **Bed**, the bus keeps its stereo width: each pass carries the sound's left and right, not just its mono sum.

- **Doppler** decides how much of the width is heard. At 0% the bed is as wide as the file; as Doppler rises it folds in towards the moving point, and at 100% it is the same single point a mono file gives.
- **Width** scales it too: at 0% everything sits in the middle, mono. A pass that has travelled far to one side narrows on its way out, so the bed is never wider than its source.
- Low End, Swirl, Filter and Drive work on the mono sum, as they do for every sound; the width is added back after them at a matching level.
- Bus A, the hits, thrown whooshes and Fly-By passes always play the mono sum: a fly-by is one point going past. So do a track with **VARI** off and a sound that is being morphed.
- A mono file is not touched by any of this. A session stores the extra stereo information only for the sounds that have it.

Bus A ("growls, tones, crackle, rumble - and the key for B") is the aggressive bus. Bus B ("air and beds, ducked and filtered by A") sits under it.

| Control | Default (A / B) | What it does |
|---|---|---|
| **Doppler** | 55% / 40% | How dramatic each pass is: how close it comes, how much it swells and brightens as it goes by, and (with Bend) how far its pitch bends. |
| **Width** | 70% / 70% | How far each pass travels across the stereo field: at 0% it stays in the middle, at 100% it flies fully from side to side. When a pass is off to one side, the ear turned away from it hears it a little duller (only the top end, so a mono fold-down is as solid as without it). |
| **Bend** | 70% / 60% | How much the pitch bends as a pass goes by: it rises on the way in and drops as the pass goes by, like a real fly-by (the rise is the bigger of the two), up to a race-car scream and drop. It changes the pitch only: how the pass swells and brightens is set by Doppler. |
| **Low End** | 35% / 30% | Weight: a sub octave and low overtones, plus a low lift that grows at the bus's loudest moments. The sub octave follows the lowest part of the sound (about 40 to 120 Hz) and holds still on noise, so it stays a note and not a rumble. |
| **Swirl** | 30% / 30% | Movement inside the sound: a phaser, then (from about a third of the way) a flanger that sweeps as the passes come and go. Its level is matched, so it changes the colour, not the loudness. |
| **Filter** | 30% / 40% | A low-pass filter that opens as each pass swells and closes in its tail, so the tails are dark and the peaks bright, like air rushing past. More blends more of it in and closes it further at the peak; the top 40% adds resonance. |
| **Drive** | 12% / 0% | Saturation, from gentle thickening to a hard, flattened growl. The drive rises evenly in dB and the level stays roughly steady, so it adds character rather than just loudness. |
| **Ducked** (B only) | 35% | How far bus A pushes bus B down at A's peaks, up to −20 dB, judged against A's own recent peak so it works the same on quiet and loud material. |
| **Filtered** (B only) | 30% | How far bus A closes a low-pass filter on bus B at A's peaks, down to about 280 Hz at 100%, so B goes dark under A rather than just quieter. |
| **PASS** (in each title bar) | Bed / Bed | How the bus's two doppler passes follow each other. **Bed**: they overlap, one coming in as the other goes out, so the bus keeps a steady level. Best for beds, air and long energy files. The two are at different pitches while they overlap, which thickens noise and can sound like two notes on a tone. **Fly-By**: one pass at a time, with a long approach, the peak at its closest point and a shorter tail, so each sound is heard once and at one pitch. The level dips between passes (by about 10 dB) without going silent, and the passes are a little louder to make up for it, so a bus is about as loud in either mode. Best for a single clear whoosh, for tones, and for sounds with a sharp start. The passes peak at the same moments in both, so AIM, LINK and the sea work the same, and switching mid-pass glides. With LINK on, bus B uses bus A's PASS and its own menu is dimmed. |
| **LINK** (in B's title bar) | off | Bus B flies the same doppler passes as bus A: at the same moment, as close, in the same direction, with A's Doppler, Width and PASS. The two buses move as one source, for one consistent whoosh. B keeps its own Bend, so its pitch can bend more or less. While linked, B's own Doppler, Width and PASS are dimmed, and they come back as you set them when you unlink. Leave it off to keep energy beds on B moving on their own. |
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
| **Space** | 0–100% (30%) | Echoes and reverb around the whole mix: a ping-pong echo whose repeats drift a little, and a smooth, wide reverb whose tail grows from about 1 to about 5 seconds as you turn it up. The reverb starts a moment after the sound and leaves the low end dry, so the whoosh stays clear in front of it; the dry sound is eased down as the room comes up, so more Space means more room, not more level into the limiter. At 0% it's off (and its echoes and reverb are emptied, so nothing old comes back when you turn it up again); at the top it's a big wash that smooths out the swells. |
| **Inflate** | 0–100% (30%) | Louder and denser without new peaks, so quieter detail comes forward. Up to 70% it blends in; the last 30% makes it harder and grittier. At 0% the sound passes through untouched. It comes before Push, so it does the same wherever Push is. |
| **Push** | 0 to +15 dB (+3 dB) | How hard the mix drives into the limiter, after Inflate. The ceiling stays put, so it can't clip. |
| **Ceiling** | −6 to 0 dB (−0.3 dB) | The limiter's ceiling, finer near the top. It is a true-peak ceiling: the limiter reads the waveform between the samples as well as at them, so a take stays under the ceiling when it is converted to another sample rate or format, or played through a converter. The limiter is always on. |
| **GR** | meter | How much the limiter is turning the sound down (a full bar is 12 dB, with marks at 3, 6 and 12). The bar falls back steadily and a line stays at the deepest recent reduction for a moment, so a short hit is easy to see. The number above the meters is that deepest recent reduction in dB, and turns coral while the limiter is working. |
| **OUT** | meter | The peak level going to tape, after the limiter and before TRIM, with marks at 0, −6, −12, −24 and −48 dB. A line stays at the highest recent peak for a moment. Turning TRIM down doesn't move it, because it doesn't change the take. |
| **TRIM** | silence to 0 dB (0 dB) | How loud you hear it (and what your DAW gets from the plug-in), after the limiter. The tape and LAST 60s record before it, at full level, so turning it down never makes a quieter take. Double-click to reset. |

If the gain reduction number is coral all the time, back off Push or raise Ceiling.

**How the limiter lets go.** A short over, such as a hit, is let go within a few hundredths of a second, so it doesn't leave a dip in the sound behind it. Reduction that lasts, such as a long loud swell or a heavy low end, is held steadily and let go over about a second, so the swell keeps its shape and the bass stays clean.

### The sea

![The sea](docs/images/sea.png)

The OUTPUT view at the top right pictures what the rig is sending out:

- **The waves** are the output's spectrum, lows on the left and highs on the right. The newest wave is at the front, and older ones roll away towards the horizon.
- **Each doppler pass** is a glowing ball that arcs across the sea, in its bus's colour (coral for bus A, turquoise for bus B). It comes closer the closer the pass, glows brighter the louder it is, and leaves a ripple as it crosses the middle. A label above it names the bus, and the track too when only one track plays on that bus ("BUS B · Energy Bed").
- After a moment of silence, the Sweet Justice logo slowly draws itself in the sky.
- While the tape records, a blinking red dot and a "Rec" timer show in the corner.
- **Click the sea** for a short swell.
- **MORPH** (top right) shows or hides the [morph pad](#morph-pad). It reads **MORPH ON** in green while the morph is switched on.

### Morph pad

![Morph pad](docs/images/morph-pad.png)

The morph blends bus A into bus B spectrally, so the two sounds trade tone and pitch rather than just crossfading. Click **MORPH** over the sea to show the pad, then switch the morph on with **ON**. The MORPH button only shows and hides the pad; the morph keeps running when the pad is hidden.

- **ON / OFF** switches the morph itself, with a fade rather than a hard switch. The morph takes about 43 ms to do its work, so while it is on, what goes through it runs that much later; the fade is lined up with that, so you never hear the sound doubled while switching. While it's off the pad is veiled and dimmed, but everything still works, so you can set it up first.
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
    - **Cherries**: the outline of the logo in the sky, a grenade with a pair of cherries.
    - **Figure Eight**, **Rose** and **Circle**: these turn slowly, one degree per lap, so they never repeat exactly.
    - **Butterfly**: the butterfly curve, two pairs of wings drawn in one closed line. It turns slowly like the others.
- The **A** and **B** handles under the pad show the two sources. Each side is its whole bus (**Bus A**, **Bus B**) until you tie it to one track: see below.

**Ropes: morphing two single sounds**

![A rope from the morph to a track](docs/images/morph-rope.png)

Each side of the morph listens to its whole bus by default. To morph one particular sound instead, **drag the rope** from the **A** or **B** handle under the pad and drop it on a track's lane. The handle takes the track's name and colour, and the rope stays hanging between the two while the pad is showing.

- **A roped side is that one track.** Only that track goes into the morph; the other tracks on its bus play on as they are, unmorphed.
- **An unroped side is its bus**, as before. You can rope one side and leave the other on its bus, or rope both to morph two single sounds.
- **A rope reaches any track**, whichever bus it plays on, the hits included. The morphed result still comes out through bus A and bus B in proportion to the dot, so it flies with their doppler passes.
- **With the morph off, a roped track simply plays through its own bus**, exactly as it would without the rope.
- **Double-click a handle** to let its rope go: the side is its bus again. Dropping a rope anywhere that isn't a track leaves things as they were. One track can't be both sides: roping it to the second side lets go of the first.
- Changing a rope while the morph is on dips that side for a few hundredths of a second, so the swap doesn't click.
- Ropes stay with their track when other tracks are added or removed. Removing a roped track lets its rope go, and undo ties it back. Ropes are saved with the session and can be undone.

The wander runs inside the rig, not in the window: the dot keeps flying with the Swooshbuckler window closed, and it is part of an offline bounce. It leaves the Morph X and Morph Y controls alone (so it writes no automation); when you stop it, the dot stays where the wander left it.

### Modulation

![Modulation](docs/images/modulation.png)

Four modulators run along the bottom: **MOD 1** (yellow), **MOD 2** (pink), **MOD 3** (green) and **MOD 4** (violet). Each can be one of four types, picked from the menu at its top right:

| Type | What it does | Knob |
|---|---|---|
| **LFO** | A repeating wave that swings each wired knob up and down. The shape menu offers Sine, Triangle, Ramp (rises then drops), Square and Steps (a new random value every cycle). | **Rate**: one cycle every 20 s (0.05 Hz) to 8 Hz |
| **Random** | A smooth random wander up and down, never the same twice. | **Rate**: as above |
| **Follower** | Follows the loudness of **Bus A**, **Bus B**, the **Hits** or the whole **Output**, or the [Master track](#the-master-track): **Master** (its loudness), **Master Pitch** or **Master Brightness**, each read where the Master is playing and spread over that sound's own range. Pick which in the menu under the scope. It turns its knobs one way only. The Master inputs rest until a track is the Master. | **Speed**: slow and smooth (2 s) to fast and jumpy (15 ms) |
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
| **File Type** | WAV / AIFF / FLAC (WAV) | FLAC is lossless and about half the size. Every type carries a description of how the take was made; WAV files also get markers (see [Recording](#recording)). |
| **Bit Depth** | 16-Bit / 24-Bit / 32-Bit Float (24-Bit) | 32-Bit Float can't clip in the file, and is WAV only. 16-Bit files are dithered, so quiet fades end in a faint steady hiss and not in grit. |
| **Sample Rate** | Session / 44.1 / 48 / 88.2 / 96 / 192 kHz (Session) | Session records at the audio device's or host's rate. Pick a higher rate and the whole rig runs at that rate, so the recording keeps the extra high end, which is good for pitching down later; you still hear it at the device rate, filtered so the extra high end is taken out cleanly and held to the Ceiling again. A note in the panel says which is happening. It can't be changed while the tape is armed. |
| **Channels** | Stereo / Mono (Stereo) | Mono folds the output to one channel, for sounds you'll pan yourself. |
| **Arm the tape as soon as Swooshbuckler opens** | (off) | So you never miss the first good take. |
| **Skip Silence** | (on) | On: the armed tape leaves the quiet stretches out (see [Recording](#recording)). Off: the tape rolls all the time, for one continuous take. You can switch it during a take. |
| **One File Per Whoosh** | (off) | On: the armed tape closes its file each time a whoosh has ended and opens the next one for the next whoosh (see [Recording](#recording)). Off: one file for the whole take. Switching it while the tape is armed closes the open take and arms again. |
| **Take Name** | (empty) | The start of every recorded file's name. Empty: the preset's name (Swooshbuckler for the default preset). |
| **Creator** | (empty) | Who made the recording, written into every file. Empty: Samuel Justice. |
| **File Names** | Counter / Date and Time (Counter) | Counter: `Name_0001`, `Name_0002` and so on. Date and Time: `Name_2026-11-07_16-32-10`, good for single long takes. |
| **Keep the Last** | 1 Minute / 3 Minutes / 5 Minutes (1 Minute) | How much the LAST button can save after the fact. The sound is held in memory all the time: about 23 MB a minute at a 48 kHz recording rate, 46 MB at 96 kHz and 92 MB at 192 kHz, so 5 minutes at 192 kHz is about 460 MB. Changing it empties what is held. |
| **AUDIO DEVICE...** | | The app only: the audio output, sample rate and buffer size. |

#### Recording

- **The tape (ARM).** Click to arm. Everything goes into one file until you click the tape again, press NEW or close Swooshbuckler. The REC timer counts only what's been written. A take with no sound in it leaves no file behind.
- **One File Per Whoosh** (off by default, in Settings). The armed tape writes a separate file for each whoosh, so a session of riding leaves a folder of numbered whooshes and not one long take to cut up. A whoosh has ended when the output has sat about 25 dB under its recent peaks for three quarters of a second (the same test Skip Silence uses, and the quiet between whooshes is always left out), or when a thrown whoosh has rung out with the rig stopped. Each file keeps the quarter second before its sound and its tail. Sounds with less than a fifth of a second of level in them are not kept, so clicks and blips don't become files. Each file appears in [Takes](#takes) as its whoosh ends. Whooshes that overlap, with no quiet between them, share a file.
- **Markers.** WAV takes carry cue points with labels, so a long take arrives already spotted in Reaper, Soundminer and other tools that read them: **Pass A** and **Pass B** where a doppler pass on that bus goes by at its closest (with LINK on the two buses fly the same passes, so only Pass A is written), **Throw** at the peak of a thrown whoosh, and **Hit** where an aimed or synced hit lands or the hit gate opens. Markers that fall together are one cue, such as "Throw + Hit". They are placed in the file itself, so they stay on their sounds when Skip Silence has left stretches out. AIFF, FLAC and LAST 60s files have no markers.
- **What each file says about itself.** Every file carries a description: the Swooshbuckler version, the preset, and Energy, Rate, Fill, Chaos, Pitch, Space, the Pass Modes, Morph and AIM as they were when the file was started. It also names the Creator from Settings and the software. In a WAV these are the broadcast-wave description and originator and the INFO comment, artist and software; in an AIFF, comments; in a FLAC, the DESCRIPTION, ARTIST and ENCODER tags.
- **Skip Silence** (on by default, in Settings). The tape judges quiet against the recent peaks, not against a fixed level, because a rig with a bed in it never falls truly silent. It stops rolling once the output has sat about 25 dB under the recent peaks (or under about −56 dBFS) for three quarters of a second, and starts again when the sound comes back, keeping the quarter second before it so attacks survive. Each join is faded over 30 ms. The recent peak is remembered for about 10 seconds and then follows the level down, so a quieter passage is recorded once it is the new normal. After STOP the tape keeps the whole tail, and once the rig has come to rest the tape stops rolling until the next PLAY. How much is left out depends on the sound: deep, slow swells (a high Energy, few layers) leave gaps to skip, while a busy rig with a steady bed is recorded almost whole. Switch Skip Silence off for one continuous take.
- **If the disk can't keep up**, the tape buffers about 3 seconds of audio at any sample rate before anything is lost, and the message that names the saved file says how much is missing.
- **LAST 60s.** Swooshbuckler always keeps the last minute of output while it plays (or the last 3 or 5 minutes: **Keep the Last** in Settings, at the memory cost given there). Click LAST 60s to save it, even if the tape wasn't armed and even while a take is going. Silence at the start is trimmed. It's written the way the tape is set: the same File Type, Bit Depth, Sample Rate and Channels.
- **What's recorded** is the final output, after Space and the limiter: exactly what you hear.
- **File names** look like `Jet Passes_0007.wav`: the Take Name from Settings (or the preset's name), then a four-digit counter. The counter carries on for as long as Swooshbuckler is open and never takes a number that a file in the folder already has. Set **File Names** to Date and Time for names like `Jet Passes_2026-11-07_16-32-10.wav`. LAST 60s files end in `_retro`.
- File Type, Bit Depth, Sample Rate and Channels are fixed when you arm, so changes apply to the next take.

#### Takes

Click **Takes** in the header (or a "Saved ..." message) to open the list of this run's recordings, newest first: tape takes, whoosh files, LAST 60s saves and rendered variations, up to 20. Close it with Close, Esc, or a click outside the panel. The list starts empty each time Swooshbuckler opens; the files themselves stay in the recordings folder.

| Part of a row | What it does |
|---|---|
| Star | Keeps the take in the list. When the list is full the oldest take without a star leaves first. Leaving the list never deletes a file. |
| Waveform and name | Click to hear the take; click again, or another take, to stop. It plays over whatever the rig is doing, after the tape, so it is never recorded. A line shows where it is. Drag the row out to the Finder or Explorer, or straight onto a track in your DAW, to copy the file there. |
| Tag and length | What it is (Tape, Whoosh, Last 60s, Variation) and how long. A file that has been moved or deleted since reads Missing. |
| **Show** | Shows the file in the Finder or Explorer. |
| Bin | Moves the file to the system trash and takes it off the list. |

**Show Folder** opens the recordings folder.

#### Render Variations

Click **Variations** in the header to render a batch of whooshes straight to files, without playing them. Each file is one thrown whoosh (the same [Throw](#throw) as the Ride panel's), rendered from silence until its echoes and Space have rung out, with the silence in front taken off and a short fade at the very end. The files go through the same output stage as the tape (Inflate, Push, the limiter and its Ceiling, the tremolo, Space) and are not affected by Master Trim.

The batch renders in the background, much faster than real time (on the machine this was written on, 100 whooshes of 3 seconds take about 20 seconds), through a second copy of the rig made from your session at the moment you press Render. The rig you hear is never touched: it keeps playing, you can keep working, and nothing you change afterwards reaches the batch.

| Control | What it does |
|---|---|
| **Count** | How many whooshes to render, from 1 to 1000. It starts at 100 and remembers what you set. |
| **Length** | How long each thrown whoosh is, from 0.3 to 6 seconds; it peaks half-way through. It starts out as the Ride panel's Throw Length. Every file runs on after its whoosh until the tail has rung out (six seconds at the most), so the files are longer than this. |
| **Batch Seed** | The number every file's random draws come from. The same seed, with the same settings and samples, renders the same batch again, sample for sample (16-bit files differ only in their dither); another seed renders another batch. **New Seed** draws a new one, and a new one is drawn after every batch unless you typed your own. |
| **What Varies** | **Randomise Everything**: a fresh RANDOMISE roll for every file, so the ride, the buses, the hits and the space all change; your tracks, their levels, the output, every knob a roll doesn't name and any section you have locked stay as they are. **Small Changes**: every file starts from the settings as they are, nudges each of those controls by up to Amount, and starts each track a little further into its sample: a family of whooshes around one you like. **Seed Only**: the settings stay exactly as they are and only the rig's own dice change: how each pass flies, how close it comes and which sound of each track it picks. |
| **Amount** | How far Small Changes may move each control: at 100%, up to half of its travel either way, and up to four semitones of Pitch. |
| **Folder** | Where the files go. **Dated Folder** (the default) gives every batch a new folder of its own inside the recordings folder, named after the take name and the date and time. **Choose...** picks a folder that stays; numbers carry on after the files already there, so nothing is ever written over. |
| **Render** | Starts the batch. |
| **Cancel** | Stops it. The files already rendered are kept; the one in hand is dropped, so nothing half-written is left. |
| **Show In Finder** / **Show In Explorer** | Shows the batch's folder. |
| **Open Takes** | Opens [Takes](#takes), where the newest 20 of the rendered files can be heard, starred and dragged out. |

While a batch renders the panel shows a progress bar with the percentage, which file it is on ("37 of 100" and the file's name) and an estimate of the time left. You can close the panel; the header button keeps showing the percentage, and the status line says when the batch is done. When it has finished the panel reads "100 files in" and the folder's name, with the batch seed underneath.

Files are named `<Name>_001`, `<Name>_002` and so on (four digits for a batch that reaches 1000), where the name is the Take Name from Settings, or the preset's. The file type, bit depth, sample rate and mono or stereo are the tape's, from Settings. Every file carries a description: which variation of which batch it is, the batch seed, and the value of every control it was made with, so a favourite can be dialled in again by hand or rendered again from its seed.

If the rig has nothing to play (no samples loaded and every lane muted, say) the batch stops with a message and writes no files.

#### Your session in the app

The app reopens where you left off: the sounds, settings, wires, A / B sets, locks and the session's name are all as they were when you quit. While it runs it also keeps a safety copy: about once a minute when you have changed something (a knob, a wire or how deep it reaches, a track), and a few seconds after a track is loaded, edited or removed. Movement the rig makes by itself, such as the morph dot while Wander is on, doesn't count, so a session that is only playing isn't saved over and over. The copy is written in the background, so the interface never waits for it (with about 20 MB of audio in the session a save takes around a fifth of a second). If the app doesn't get to quit properly (a crash, a power cut), the next launch opens that copy, so you lose a minute at most. In a DAW, the project keeps the plug-in's state as usual.

#### Session files

A session can also be saved as a file of its own, to keep several pieces of work or to move one to another machine or into a DAW. The commands are in the **Session** part of the preset list (click the preset name):

| Command | What it does |
|---|---|
| **Save Session As...** | Writes the whole session to one `.swooshsession` file: every setting, the wires, the A / B sets, the locks and all the audio in the tracks (stored losslessly), so the file needs nothing else. Shift+Cmd+S in the app. |
| **Save Session** | Saves to the session's file again (the list shows its name). The first time, it asks where. Cmd+S in the app. |
| **Open Session...** | Opens a session file in place of what is loaded. Cmd+O in the app. Dropping a `.swooshsession` file on the window does the same. |
| **Recent Sessions** | The last ten session files you saved or opened that still exist, newest first, and **Clear Recent Sessions**. |

In the app the window's title shows the session's name. **NEW** starts an unnamed session again. Opening a session clears the undo history. The plug-in has the same commands, so a session made in the app opens in a DAW and the other way round; the DAW project still keeps the plug-in's state by itself.

Session files, and the state a DAW project keeps, hold the audio in a compact binary form. Sessions and projects saved by earlier versions still open. If a session's file is no longer where it was when the session is reopened (it was moved, or the project came from another machine), the session comes back unnamed and **Save Session** asks where to save.

### About

![About](docs/images/about.png)

Click the **Swooshbuckler** title or the version number to open it. It shows the version, which format you're running (app, VST3 or AU), the build date and commit, and the credits. **COPY VERSION** copies the version, build date and commit, plus your operating system, for a bug report. **WEBSITE** opens sweetjusticesound.com. **Licences** shows the notices and licences of the software and typefaces Swooshbuckler is built with (the same text as [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)); click it again to go back.

## In a DAW

- **Put it on an instrument track.** Swooshbuckler is an instrument (a generator): it makes sound from the tracks you load into it, not from the audio of the track it sits on. Use an instrument or software-instrument track (VST3 or AU), then open the window and drop your sounds in, or click Load Demo Sounds.
- **Follow Host** (in the header, on by default) starts and stops the rig with the transport. Every start begins the ride from the same place, so a bounce of the same project is the same every time, including the modulators and the morph's Wander.
- **The tail is bounced too.** Swooshbuckler tells the host its tail can last up to 6.5 seconds, so a bounce or a freeze that respects plug-in tails keeps the ring-out after the transport stops.
- **Play is a parameter.** Automate it to start and stop the rig at exact places, with Follow Host on or off.
- **MIDI notes**: play or draw notes on the instrument track and pick what they do with [Keys](#keys). **Swell**: any held note holds Swell. **Pitch**: the held notes set the pitch of the tracks, a chord dealt out over them. **Throw**: every note throws one whoosh at that note's pitch, its peak half a Throw Length after the note, so you can place whooshes on the timeline note by note.
- **MIDI controllers**: a track's fader, or Energy, can follow a fader or knob on a controller. Right-click it and choose **Learn MIDI CC** (see [MIDI controllers](#midi-controllers)). There is no mapping until you make one.
- **Automation**: every knob, fader and switch is a host parameter and can be automated, Play included. Typing a value into a knob, or moving it by hand, writes automation like any other move; a MIDI controller moves the control without starting a write of its own.
- **Where recordings go**: the tape, LAST 60s and Render Variations write files to the folder set in [Settings](#settings-and-recording) (the Desktop unless you change it), not into the DAW project. Drag a take from **Takes** straight onto a DAW track. The plug-in's output is also just audio on the track: bounce or record it there as you would any instrument.
- **Keys**: the single-key shortcuts work while the Swooshbuckler window has the keyboard (click in it first). Some hosts keep keys such as the space bar and Return for their own transport.
- Follow Host, the play state, Keys and Throw Length are saved with the project. The settings in Settings, and learned MIDI controllers, are shared by every project and the app.

## Presets and RANDOMISE

The factory presets are **Steady Energy**, **Jet Passes**, **Monster Swells**, **Gentle Breeze**, **Fast and Furious**, **Magic Dust** and **Heavy Hits**. Pick one from the preset name, or step through them with **<** and **>**. Every control glides to its new setting while it plays, so you can switch mid-whoosh. Grab a knob during the glide and your move wins.

A preset is a complete look: every control it owns that it doesn't set goes back to its default. The presets set PASS too: **Jet Passes**, **Monster Swells** and **Fast and Furious** put bus A on Fly-By, and the others use Bed on both buses.

**RANDOMISE** rolls Energy, Rate, Fill, Chaos and Pitch (whole semitones from −7 to +7), the bus knobs (Doppler, Width, Bend, Low End, Swirl, Filter and Drive, plus Ducked and Filtered on B), the hit knobs (Sensitivity, Boost, Release and Duck) and Space. Width stays between 35% and 100% and Bend between 30% and 90%, so a roll never leaves a pass stuck in the middle or screaming. Only those knobs move. Everything a roll doesn't name stays exactly where you set it: Hold, Floor, Return, each bus's PASS menu, Throw Length, the morph's dot and mode.

Presets and RANDOMISE set the sound, not your session. They leave these alone:

- your tracks, their levels and each track's pitch and VARI
- Inflate, Push, Ceiling, TRIM and the Peak Tremolo
- AUTO RIDE, Follow Master, AIM, SYNC HITS, the GATE and DUCK modes, TAKE TURNS and LINK
- the Master track, each track's bus and the morph's ropes
- the morph's ON switch, wander, shape and speed
- PLAY, the Keys menu and Throw Length
- the modulators and their wires (unless a preset of your own was saved with them)

**Back to Defaults** in the preset list puts every control a preset owns back to its default, and leaves the same things alone. It's much gentler than NEW, which also empties the tracks, resets everything else and pulls out the wires.

### Section dice and locks

Each of Ride, Bus A, Bus B, Hits and Output has a small die and a small padlock in its title bar.

- **The die** rolls just that section and leaves everything else where it is. The Output die rolls Space and, gently, Inflate (up to 50%) and Push (up to +6 dB); the header's RANDOMISE never touches those two.
- **The lock** keeps that section out of the header's RANDOMISE, and out of Randomise Everything in [Render Variations](#render-variations). Lock the bus you like and keep rolling the rest. A locked section's own die still rolls it. The locks are saved with the session.

### The roll history

The two triangles beside RANDOMISE step back and forward through the last 20 rolls, the header's and the section dice alike. Past the oldest roll, back ends at the settings you had before the first of them. Each stop comes back as you left it, so a tweak you made on a roll is still there when you return to it. Rolling again from the middle of the history drops the rolls that were ahead. The star beside them keeps the settings you have now as a preset of your own (Roll, Roll 2, ...), ready to rename.

### Your own presets

The preset list has a **Your Presets** section under the factory ones.

| Command | What it does |
|---|---|
| **Save Preset...** | Saves the settings as they are under a name (up to 24 characters). A preset of yours with the same name is replaced; a factory preset's name can't be used. Tick **Include Modulators and Wires** to store the four modulators' settings and every wire as well: loading that preset then replaces yours. Without it, a preset leaves your modulators and wires alone. |
| **Rename Preset...** and **Delete Preset** | Act on the preset of yours that is loaded. Delete moves its file to the system trash. |
| **Show Preset Folder** | Opens the folder the presets are kept in. |

A preset holds the same controls a factory preset does, so it leaves your tracks, their levels and the output stage alone. Each one is a small `.swooshpreset` file in a `Swooshbuckler Presets` folder beside Swooshbuckler's settings file. To share a preset, send the file. To load one you were sent, drop it on the window: it loads and joins your presets (or put it in the folder yourself). Renaming a file in the folder renames the preset.

### A / B

The **A** / **B** button in the header holds two sets of the controls a preset owns, for comparing two versions of a sound. You are always on one of them, and the other waits as you left it. Click the button (or **Switch to B** / **Switch to A** in the preset list) to go to the other; the change is immediate. The first time, B starts as a copy of A. **Copy to Other** in the preset list copies the settings you have now into the other set. Both sets are saved with the session. Your tracks, the output stage and the modulators are not part of a set, so they stay the same on both.

### Undo and redo

The two curved arrows in the header, and Cmd+Z / Shift+Cmd+Z (Ctrl+Z / Ctrl+Shift+Z on Windows; Ctrl+Y also redoes), take back and do again what you changed, in the order you did it:

- a knob, fader, menu or switch you moved (one step per move; quick moves in a row, such as turns of the mouse wheel, are one step)
- a preset, a RANDOMISE roll, a section's die, a step through the roll history, A / B: one step each, however many knobs glided
- NEW: one step brings back the tracks, every setting, the wires, the mutes and solos and the session's file
- loading, replacing, removing and editing a track

The keys work wherever you last clicked in the window (while you type in a text box, they undo the typing). The message at the bottom right says what was undone. PLAY and STOP are not steps, and neither is automation from a DAW. Dragging a wire is not a step either: pull the plug out to remove it. Opening a session starts the history afresh.

## Keyboard, mouse and MIDI

| Input | What it does |
|---|---|
| Return | PLAY / STOP. |
| Space bar (hold) | Swell. With a track lane selected, it gives a short swell instead: click outside Tracks for a held one. |
| F | Throw: one whoosh now, playing or stopped. See [Throw](#throw). |
| R | RANDOMISE: a new roll. |
| T | Arms the tape; again to stop it and close the file. |
| L | Saves the last minute (or the last 3 or 5, as Keep the Last is set). |
| MIDI notes | What the **Keys** menu in the Ride panel says: hold Swell (the default), set the pitch of the tracks, or throw one whoosh per note. See [Keys](#keys). |
| Click the sea | A short swell. |
| Cmd+Z / Shift+Cmd+Z (Ctrl+Z / Ctrl+Shift+Z on Windows) | Undo and redo, wherever you last clicked in the window: knobs, presets, rolls, A / B, NEW and track edits, newest first. See [Undo and redo](#undo-and-redo). |
| Cmd+S / Shift+Cmd+S / Cmd+O (Ctrl on Windows), in the app | Save Session, Save Session As and Open Session. See [Session files](#session-files). |
| Drop a `.swooshpreset` or `.swooshsession` file on the window | Loads the preset (and adds it to yours), or opens the session. |
| Esc | Closes Settings, Takes, Render Variations or About. A batch that is rendering carries on. |
| Double-click a knob | Resets it. |
| Right-click a knob, or Alt-double-click it | Opens a box to type a value. See [Typing a value](#typing-a-value). |
| Right-click a track's fader, or the Energy knob | A menu: Type a Value and Learn MIDI CC. See [MIDI controllers](#midi-controllers). |
| Double-click a track name | Renames it. |
| Double-click a waveform | Puts the start flag back at the top. |
| Click a lane's bus tag | Chooses the track's bus: Auto, Bus A, Bus B or Hits. See [Choosing the bus](#choosing-the-bus). |
| Right-click a track lane | The track's menu: Master Track, Match Pitch To Master and Plays On. |
| Drag from the A or B handle under the morph pad onto a track | Ropes that side of the morph to the track. Double-click the handle to let the rope go. See [Morph pad](#morph-pad). |

The single keys (Return, F, R, T, L) do nothing while you are typing in a text box, and nothing while Settings, Takes, Render Variations or About is open, so a name or a value you type never triggers them.

### Typing a value

Right-click a knob (or hold Alt and double-click it) and a small box opens on it with the current value. Type a new one in the knob's own units and press Return to keep it; Esc, or a click anywhere else, leaves the knob as it was. A plain double-click still resets the knob.

- Units are optional and are the ones the knob shows: `-12 dB`, `4.2 s`, `35%`, or just `-12`, `4.2`, `35`.
- Milliseconds and seconds are converted, so `0.3 s` on a knob that shows `300 ms` is understood, and a bare number is read in the unit showing at that moment.
- Words the knob shows work too: `Rate` on Throw Length, `Off` on Duck, `-inf` on a fader.
- A value out of the knob's range goes to the nearest end. Text that isn't a value changes nothing.
- A typed value is one Undo step.

It works on every knob, each track's fader (through its right-click menu), a track's Semitones and Cents boxes, and Master Trim.

### MIDI controllers

A track's fader, and the Energy knob, can follow a fader or knob on a MIDI controller. Nothing is mapped until you do this:

1. Right-click the track's fader (or the Energy knob) and choose **Learn MIDI CC**. The status line asks you to move a control.
2. Move the fader or knob on your controller. The first one that moves is learned (any controller from 0 to 119), on any MIDI channel, and the status line says which ("Track 1 Fader follows MIDI CC 7").

From then on that controller moves the control, whether the rig is playing or stopped. The same menu has **Forget MIDI CC** to let it go and **Cancel Learn MIDI CC** if you change your mind while it waits. One controller can be learned for several things, to move them together. The mappings are kept in the settings, so they are the same in every session, and in the app and the plug-in. Controller moves are not Undo steps.

## Tips

- **Start with beds and a tonal layer.** A couple of Energy Beds on bus B and one or two growls or tones on bus A make a whoosh straight away. Add impacts for punctuation.
- **Gappy recordings** (single swishes, takes with pauses) work best with SKIP SILENCE on and a little Fill.
- **For tight synced hits**, put two or three impacts on HITS, turn on SYNC HITS and raise Boost.
- **For monster passes**, lower Pitch, raise Low End and Bend, and slow the Rate.
- **Two takes of one sound**: load the same file on two tracks and detune one by 10 to 20 cents with VARI off, for a thicker, chorused layer.
- **Keep a hit's timing but change its size**: pitch an impact down with VARI off and it lands on the same beat, just heavier.
- **For one clean pass instead of two layers flying about**, turn on LINK so bus B follows bus A's passes. Turn it off when B carries a bed that should keep moving on its own.
- **For a single object going by** (a jet, a car, a creature's call), set that bus's PASS to Fly-By: each sound is heard once, at one pitch. Keep Bed for wind, air and long energy beds.
- **If the buses fight**, turn on TAKE TURNS, or raise Ducked and Filtered on bus B.
- **To make every layer move like one sound**, make that sound the Master (the star on its lane) and turn on Follow Master. Raise Energy for layers that vanish between its swells.
- **To tune the layers to one sound**, make it the Master, then right-click each pitched track and choose Match Pitch To Master.
- **To morph a growl into a wind**, rope side A to the growl and side B to the wind, switch the morph on and let the dot wander. Everything else keeps playing around them.
- **Record at 96 or 192 kHz** if you plan to pitch the whooshes down later.
- **Leave the tape armed** while you play with the controls. It only writes when there's sound, so you just need to listen back for the good ones.
- **Press STOP and wait for the tail** before you close a take: the echoes and Space ring out onto the tape.

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

This builds the app, the VST3 and the AU (macOS) into `build/Swooshbuckler_artefacts/Release/`, along with the offline test programs: `SwooshbucklerRig`, `SwooshbucklerCargo`, `SwooshbucklerClassify`, `SwooshbucklerPitch`, `SwooshbucklerMorph`, `SwooshbucklerMaster`, `SwooshbucklerSession`, `SwooshbucklerRender`, `SwooshbucklerKeep` and `SwooshbucklerRouting`. Pass `-DSWOOSHBUCKLER_TESTS=OFF` to skip the tests, or `-DSWOOSHBUCKLER_FORMATS="Standalone;VST3"` to choose the formats.

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
- The notices and licences of everything it is built with are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), and under **Licences** in About.

## License

MIT License. See [LICENSE](LICENSE).
