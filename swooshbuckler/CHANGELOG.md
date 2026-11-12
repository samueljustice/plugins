# Swooshbuckler changelog

## Unreleased

- **New: Render Variations.** A Variations button in the header opens a panel that renders a batch of whooshes straight to files, in the background and much faster than real time (100 whooshes of 3 seconds in about 20 seconds). Choose the **Count** (1 to 1000, 100 unless you change it), the **Length** of each whoosh, and **What Varies**: **Randomise Everything** (a fresh RANDOMISE roll per file), **Small Changes** (the settings as they are, nudged by up to **Amount**, with each track starting a little further in) or **Seed Only** (the same settings, different passes). Each file is one thrown whoosh rendered from silence until its tail has rung out, through the same output stage as the tape and in the tape's format, named `<Name>_001` onwards in a dated folder of its own (or a folder you choose, where numbers carry on and nothing is written over). A progress bar shows the percentage, "37 of 100", the file in hand and the time left; **Cancel** keeps the files already rendered and leaves nothing half-written. The rig keeps playing while a batch renders, and the header button shows how far it is. The same **Batch Seed** renders the same batch again, and every file's description lists the settings it was made with. The rendered files join Takes.

- **New: One File Per Whoosh** (in Settings, off by default). The armed tape closes its file each time a whoosh has ended and opens the next for the next one, so a session of riding leaves a folder of separate, numbered whooshes. A whoosh has ended when the level has sat about 25 dB under its peaks for three quarters of a second, or when a thrown whoosh has rung out with the rig stopped. Sounds with less than a fifth of a second of level in them are not kept.
- **New: Takes.** A Takes button in the header, with a count, opens the last 20 recordings of this run: tape takes, whoosh files and LAST 60s saves, each with a small waveform, its name and its length. Click one to hear it (it plays over the rig and is not recorded), drag it out to the Finder, Explorer or a DAW track, show it in its folder, star it to keep it in the list, or bin it to the system trash. Clicking a "Saved ..." message now opens Takes.
- **Markers in WAV takes.** Takes carry cue points with labels at every doppler pass peak (Pass A, Pass B), every thrown whoosh's peak (Throw) and every aimed, synced or gated hit (Hit), so a long take arrives already spotted in tools that read WAV cues.
- **File names you can use.** Recordings are named after the preset, or a **Take Name** you set in Settings, with a four-digit counter that carries on while Swooshbuckler is open: `Jet Passes_0007.wav`. **File Names** in Settings switches to the date and time form.
- **Every file says how it was made.** WAV, AIFF and FLAC recordings all carry a description (version, preset and the main settings) and a **Creator**, which you can set in Settings.
- **LAST 60s can keep 3 or 5 minutes.** **Keep the Last** in Settings sets it. The sound is held in memory: about 23 MB a minute at 48 kHz, 92 MB a minute at 192 kHz.
- **A take with no sound leaves no file.** Arming the tape and stopping it before anything played used to leave an empty file behind.
- **New: Throw.** A Throw button in the Ride panel (and the F key) fires exactly one whoosh: a single pass on both buses with a long approach, a peak and a shorter tail, and with AIM on the hits land on its peak. While the rig plays it goes on top of the ride. With the rig stopped you get just that one whoosh and then silence, so single whooshes can be made one at a time. The peak is heard half a Throw Length after you throw (within a few thousandths of a second), and the top of the Ride panel counts down to it ("Peak in 1.2 s"). The new **Throw Length** knob sets how long it lasts, from 0.3 to 6 seconds, or follows Rate.
- **New: Keys.** A Keys menu in the Ride panel sets what MIDI notes do. **Swell**: any held note holds Swell, as before. **Pitch**: the held notes set the pitch, each as its distance from middle C added to the Pitch knob; a chord is dealt out over the tracks in order, lowest note to the first track, so each layer gets its own pitch, and letting go glides back. **Throw**: every note throws one whoosh at that note's pitch, with how hard you play setting how close and loud it passes. The notes you hold are shown at the top of the Ride panel.
- **The app listens to every MIDI keyboard.** MIDI inputs are opened when the app starts and when a keyboard is plugged in.
- **New in the plug-in: Follow Host.** On by default: the rig plays while the DAW's transport plays and stops, with its tail, when it stops. Every transport start begins the ride from the same place, so the same project bounces the same way each time. Switch it off in the header to start and stop only with PLAY.
- **Play can be automated**, and the play state is saved with a DAW project. The app still always opens stopped.
- **Bounces keep the tail.** The plug-in now tells the host how long its tail can ring (6.5 seconds).
- **Wander carries on with the window closed.** The morph dot's wander now runs inside the rig, so it keeps moving when the Swooshbuckler window is closed and is part of an offline bounce. It no longer moves the Morph X and Morph Y controls, so it writes no automation.
- **New wander shape: Butterfly**, the butterfly curve, at the end of the shape menu.
- **Fast doppler passes stay clean.** A pass that comes in fast plays the sound up to twice as fast, and the top of a bright sound used to fold back down as whistles sweeping the wrong way. What would go above the top of the audible range is now filtered out: a 20 kHz test tone through the fastest pass used to leave fold-back only 4 dB under the sound, and now leaves it 70 dB under. Bright sounds also keep a little more top end through every pass. The rig uses about 1% more of one processor core.
- **PLAY straight after STOP doesn't click.** Pressing PLAY while the sounds were still letting go (the first 60 ms after STOP) used to cut them mid-wave. They now fade out over 5 ms first, then start again.
- **Fly-By is as loud as Bed.** A bus on Fly-By used to be about 1 to 2 LU quieter than the same bus on Bed. Its passes are now 1.8 dB louder, which puts the two modes within half a LU of each other on steady and gappy sounds alike. The presets that use Fly-By (Jet Passes, Monster Swells, Fast and Furious) are that much louder.
- **VARI on and off are closer in level when pitched up.** With a track's Semitones up, VARI on now carries a small make-up (1 dB per octave). On a hissy sound the two settings used to differ by 2 to 2.4 LU at +7 and +12 semitones; they are now within 1.4 LU, and within 1.1 LU on tonal sounds.
- **The safety copy is only written when you change something.** With Wander on, the moving morph dot used to count as a change, so the session was saved every minute for as long as the app ran. Only your own edits count now. Dragging how deep a wire reaches counts as an edit too (it didn't before, so that change could be missing from the copy).
- **The tape stops rolling when the rig rests.** With Skip Silence on, the tape used to show as capturing from STOP until the next PLAY. It now stops once the tail has died away.
- **Undo uses about half the memory.** Each undo step in Tracks held the sound plus the extra copies the rig keeps for clean pitched-up playback (nearly as much again). A step now keeps only the sound and gets its copies back when you undo to it. The whole undo history is also limited to about 500 MB of audio: past that, the oldest edits are forgotten first, and the latest one can always be undone.
- **Replaced sounds are freed when the audio stops.** A sound you had replaced or removed could stay in memory for as long as the audio device was stopped. It is now let go at once.
- **Switching the morph on and off is clean.** The morph runs about 43 ms behind the buses, and its fade used to mix the live sound with that late copy, which was heard as a doubled sound for a third of a second. The fade now runs against a copy of the buses lined up with the morph, joined on with a 10 ms splice. Switching the morph on again no longer plays back a scrap of sound from the last time it was on.
- **The morph keeps the dynamics of both buses.** With the dot between A and B, the level now follows the mix of the two buses' loudness, so a swell or a hit on one bus comes through at nearly its full size (about 3 dB down in the middle of the pad). Before, a swell on the louder bus was pulled halfway down towards the quieter one (a swell 26 dB over the other bus came out 13 dB down in the middle). With the two buses at different levels, the middle of the pad is now louder than it was, and the level moves evenly from one side to the other in every mode.
- **Glide keeps attacks sharper.** When a hit arrives, the gliding partials line up on it and no longer smear ahead of it. With the dot near one side there is now nothing ahead of the hit; towards the middle of the pad there is about 5 dB less than before.
- **VARI off holds its level.** Noisy sounds used to come out 1 to 3 dB quieter through the pitch shifter, more the further the shift; a steady tone could move by up to 0.8 dB with the setting. Both now come out at the level they went in. Shifting up still loses whatever goes above the top of the audible range, as it must.
- **Hits with VARI off have no pre-echo.** A shifted hit used to be preceded by about 20 ms of its own sound, some 25 dB under it. Nothing of a hit is now heard before it starts.
- **STOP lets the tail ring out.** Pressing STOP lets go of the sounds and lets the Fill echoes, the passes in flight and Space die away, in your speakers and on the tape, for up to 6 seconds, then rests. A take used to end in a cut at full level 60 ms after STOP; now it ends with its tail. PLAY during the tail starts again at once.
- **New setting: Skip Silence** (in Settings, on by default). The armed tape now judges quiet against the recent peaks: it stops rolling once the output has sat about 25 dB under them for three quarters of a second, and starts again, a quarter second early, when the sound comes back, with a short fade at each join. Before, it only stopped under a fixed, very low level, which a playing rig never reaches. Deep, slow swells leave gaps for it to skip; a busy rig with a steady bed is still recorded almost whole. Switch it off and the tape rolls all the time, for one continuous take.
- **LAST 60s is written the way the tape is set**: the same File Type, Bit Depth, Sample Rate and Channels (it was always a stereo WAV at the rig's rate).
- **16-Bit recordings are dithered**, so quiet fades end in a faint steady hiss and not in grit.
- **More room for a slow disk at high sample rates.** The tape buffers about 3 seconds of audio at any rate before anything is lost (at 192 kHz it was under 0.7 s). If audio is lost, the message that names the saved file says how much, and the figure is now right at high sample rates.
- **The app keeps a safety copy of your session.** About once a minute when something has changed, and a few seconds after a track is loaded, edited or removed, the session is saved in the background without holding up the interface. If the app doesn't quit properly, the next launch opens that copy.
- **Tape settings changed in the app are kept.** A change to the recording folder, format or Auto-Arm made while the app was open could be undone when the app quit and saved the session. The app and the rig now share one settings file handler.
- **Recordings name their author.** The broadcast-wave details in WAV files give Samuel Justice as the originator reference.
- **The wander shape that traces the logo is called Cherries.** Sessions and presets that use it are unaffected.
- **New: Pass Mode.** Each bus has a PASS menu in its title bar. **Bed** is the two overlapped doppler passes as before: a steady level, best for beds and air. **Fly-By** plays one pass at a time, with a long approach, the peak at the closest point and a shorter tail, so a tone or a sound with a sharp start is heard once and at one pitch (in Bed the incoming and outgoing pass sound together about a third of the time; in Fly-By under a tenth, and far quieter). The passes peak at the same moments, so AIM, LINK and the sea work the same; a linked bus B uses bus A's mode. Both buses start on Bed. The presets Jet Passes, Monster Swells and Fast and Furious now use Fly-By on bus A.
- **The passes move the colours when the faders are still.** With Auto Ride off, a low Energy, or Swell held, Low End, Swirl and Filter now follow how close each doppler pass is, not just the level (which hardly moves then). On a steady sound with Auto Ride off, the Filter and Swirl now sweep with every pass about as much as they do with the ride on, and the Low End lift comes down between passes. With the ride at its default depth or deeper, the sound is unchanged.
- **Auto Ride off and Swell no longer run hot.** With every fader up all the time the mix was about 4 dB louder into the limiter (which then squashed the peaks by 5 to 6 dB). The mix is now eased down to match the ridden level, by up to about 5 dB, so switching Auto Ride compares movement and not loudness, and Push and Inflate behave the same either way. A Swell is still a push: the level comes up at once and eases back over a couple of seconds if you keep holding. Settings with Energy at 35% or more sound as before; sessions with Auto Ride off or a lower Energy are quieter and less squashed.
- **A touch of head shadow.** When a pass is off to one side, the ear turned away from it hears a little less top end, so a pass feels like it goes around your head. It is subtle, and a mono fold-down is as solid as before.
- **Swirl no longer clicks.** The phaser and flanger sweeps are now smooth from sample to sample. Before, they moved in small steps about 1,500 times a second, which put a faint grit on clean, tonal sounds at high Swirl.
- **Doppler pitch like a real fly-by.** A pass now rises in pitch on the way in by more than it drops on the way out (at Doppler 100% and Bend 100%: about 9.4 semitones up, 6 down), the way a passing car or plane sounds. The total bend is the same size. High Bend settings sound more like a scream coming in and less like a dive.
- **Bend changes the pitch only.** How a pass swells and brightens is set by Doppler alone; turning Bend down no longer flattens the swell or makes the bus louder.
- **The two passes of a bus stay half a pass apart at any Chaos**, so a bus keeps a steady level (with Auto Ride off, a bed no longer dips and bumps at random as Chaos goes up). Chaos still varies each pass's length, speed, closeness and direction.
- **Low End's sub octave is steadier.** It follows the lowest part of the sound (about 40 to 120 Hz) and no longer flips at random on the noise between sounds, which was heard as rumble after a sound stopped.
- **Fill echoes are different on every track.** Each track has its own echo times (around 0.5, 1 and 2 s, in uneven ratios), so layered tracks no longer echo together on a half-second grid.
- **A new reverb in Space.** The reverb is now a smooth, dense room with no metallic ring: eight delay lines that feed each other, breathing slightly so no single note rings on. It starts about 15 ms after the sound and leaves the low end (below about 150 Hz) dry, so whooshes and hits stay clear in front of it, and it is wide while staying solid in mono. The tail grows from about 1 second to about 5 as Space goes up.
- **Space no longer leans on the limiter.** The dry sound is eased down as the room comes up (by up to about 5 dB at 100%), so the top of the knob is a wash and not just more level: at Space 100% the limiter now works about a quarter as hard on the default sounds, and the output is a little quieter there (under 1 dB). At the default 30% the level is about the same as before. The swells stay a little more distinct at the top than they were.
- **The Space echoes are less rigid.** The two ping-pong echo times are no longer in a simple ratio and each drifts by a few milliseconds, so the repeats don't settle into a fixed pattern.
- **Space at 0% is really empty.** Turning Space back up no longer plays back old echoes and reverb from before it was turned off.
- **Filter, Swirl and Low End recover sooner after a big swell.** The recent peak they measure against now falls back over about one pass (it follows Rate), not over a fixed few seconds.
- **The same feel at every sample rate and buffer size.** The glides behind the knobs, the ducking and the level matching are now timed in milliseconds, not in processing steps.
- The doppler delay is worked out at full precision, which removes a trace of noise on long, slow passes at high sample rates.
- **Cleaner pitched-up samples.** A track playing faster than it was recorded (Pitch or Semitones up, or a 96 kHz file in a 48 kHz session) no longer folds its high end back down as harsh, off-pitch tones: what would go above the top of the range is filtered out. Bright, hissy sounds pitched far up are quieter than they were, because that fold-back was most of their level. A sound playing at its own speed is sample-for-sample the file, and slightly brighter than before when it plays a little off speed.
- **Start flags are saved.** A track's start flag now comes back with the session and with the plug-in's state in a project. Loading a new file onto a track, or removing the track, puts the flag back at the top, and Cmd+Z (Ctrl+Z on Windows) brings it back along with the sound.
- **Auto gain respects peaks.** The automatic loudness trim no longer lifts peaky sounds (hits, clicks) over full scale: it stops when the loudest peak reaches 0 dBFS. Such tracks come in quieter than before; steadier sounds are levelled as they were.
- **Every added file is accounted for.** The status line says how many files went in, how many didn't fit ("2 didn't fit (ten tracks at most)") and which couldn't be read, and tells you when only the first 10 minutes of a long file were loaded ("Loaded the first 10:00 of 23:41"). Status messages use plain words throughout ("Added", "Removed").
- **Pasting between sample rates is clean.** Copying from a file at one sample rate and pasting into a track at another converts the audio with the same filtered resampler the tracks play through.
- **Safer sample swaps.** Sample data that has been replaced is kept until playback has really let go of it, even if the audio engine is paused or bypassed at that moment.
- **True-peak limiting.** The limiter now reads the waveform between the samples as well as at them, so a take stays under the Ceiling when it is converted to another sample rate or format, or played through a converter. On noisy hits the level between the samples used to reach about 3.5 dB over full scale with the Ceiling at −0.3 dB; it now stays at the Ceiling. The limiter's delay is 2 ms (it was 1.5 ms), still constant and reported to the host.
- **The limiter lets go to suit the sound.** A short over, such as a hit, is let go within a few hundredths of a second, so it no longer pulls the whoosh down behind it for a tenth of a second or more. Reduction that lasts is held steadily and let go over about a second, so a loud swell keeps more of its shape at high Push and heavy bass stays cleaner.
- **Inflate comes before Push.** Inflate now works on the mix at its own level and Push drives the result into the limiter, so Inflate does the same thing wherever Push is. The default sound is as loud as before (within 0.2 LU, as are the factory presets).
- **Meters you can read.** GR and OUT have dB marks, fall back at the same speed whatever the buffer size, and keep a line at the deepest recent reduction and the highest recent peak. The GR number shows the deepest recent reduction, so a short hit is not missed. OUT shows the level going to tape, before TRIM, so turning your monitoring down no longer makes a take look quiet.
- **Cleaner monitoring at a high Sample Rate.** When the tape's Sample Rate is above the device's, what you hear (and what your DAW gets) is now filtered before it is brought down to the device rate, so the extra high end no longer folds back into the audible range, and it is held to the Ceiling again afterwards. The reported delay stays exact. The recorded file was always clean and is unchanged.

## 1.4.2

- **New-version notice.** Swooshbuckler tells you when a new version is out: when you open it and a newer release has been published, a small window offers Download (the release page), Later or Skip this version. It checks in the background at most once a day, and says nothing when you're offline. The sound, the parameters and saved sessions are unchanged from 1.4.1.

## 1.4.1

- **The passes in the sea show their bus.** Each doppler pass glows in its bus's colour (coral for A, turquoise for B) with a small label above it naming the bus, and the track when it is the only one on that bus.

## 1.4.0

- **Every knob reworked so all of its travel is usable.** Each one was measured across its range and remapped; the details and numbers are in [docs/knob-audit.md](docs/knob-audit.md). The main changes:
  - Energy dips evenly in dB, all the way to silence. Fill's echoes come after the fader, so they fill the ride's dips. Chaos varies the size of each swell. Rate shows the swell time.
  - Track faders are 30 dB faders in dB, and Pitch is finer near the middle.
  - Width really pans, at any Doppler. Filter sweeps instead of just turning the level down. Drive and Swirl add character without jumping in level. Ducked by A reaches -20 dB, and Filtered by A works whatever the Filter is set to.
  - Hit Boost is -3 to +12 dB, Duck reaches -12 dB, Floor is curved, and Sensitivity is spread out in every gate mode (Level now follows the recent peak).
  - Push is 0 to +15 dB, Ceiling -6 to 0 dB, Master Trim stops at 0 dB, and Space no longer pushes wide beds out of phase. Inflate's last 30% gets harder.
  - Tremolo Depth and Threshold read in dB and are even.
  - Modulators run from 20 s cycles to 8 Hz and reach every knob without being smoothed away. Random starts moving straight away, and the Follower runs from 2 s to 26 ms. Morph X holds its level, and Glide no longer collapses.
- **Removed:** the Input parameter (the plug-in has no audio input).
- Saved sessions and automation use the new curves, so they can sound different.

## 1.3.0

- **AIM: hits on the peaks.** On by default for new sessions: Swooshbuckler looks ahead to each doppler pass's peak on buses A and B and fires every hit track early by its own lead-in, so the transients land right on the peak. Sensitivity sets which peaks earn a hit. Sessions saved before keep the hit gate they were made with.
- **Peak Tremolo.** A tremolo that shakes only the loudest moments, deepening as the level nears its peak: On/Off, Sine, Square or Flutter, and whether it shakes everything, the buses or the hits, with Depth, Rate and Threshold. It sits at the left of Output.
- **Master Trim.** A fader by the OUT meter for how loud you hear it; the tape still records at full level.
- **Modulation depth from the control.** Cmd-drag (Ctrl on Windows) a modulated knob to set its newest wire's depth, including negative; Cmd-double-click sets +50%. Negative wires draw dashed.
- **Modulate each track's pitch.** Drag a MOD jack onto a track's Semitones box for smooth pitch modulation (a 100% wire swings two octaves).

## 1.2.0

- **LINK the buses.** A LINK button in Bus B's title bar makes bus B fly the same doppler passes as bus A (same moment, closeness and direction) with A's Doppler and Width, so the two buses move as one source for a consistent whoosh. B keeps its own Bend. Turn it off (the default) to keep energy beds on B moving on their own; B's own Doppler and Width come back as you left them. Switching it fades bus B briefly instead of clicking.

## 1.1.0

- **Per-track pitch.** Every track has its own Semitones (±24) and Cents (±100), on top of the Ride's Pitch knob, and a **VARI** switch:
  - VARI on (the default): varispeed, like tape. The pitch and the speed change together.
  - VARI off: the pitch changes and the speed stays the same, through a new pitch shifter built for Swooshbuckler. It keeps attacks sharp and adds about 43 ms of delay to that track; synced hits allow for it.
  - Pitch glides to a new setting, and switching VARI fades the track rather than clicking. Each control is a host parameter, saved with the session; presets leave them alone.

## 1.0.2

- **Listed under Sammy J.** Hosts show it in a Sammy J folder with the other plug-ins, and the Windows installer names Sammy J as the publisher. The sound, parameters and saved sessions are unchanged.

## 1.0.1

- **Tracks show their bus.** Each track's name row has a BUS A, BUS B or HITS tag in that section's colour, and it follows the sound type.
- **Clearer names.** Sound types, presets, wander shapes, menu items, tooltip headings and the parameter names your DAW sees are in Title Case ("Tonal Content", "Steady Energy"). Tracks named after their type are no longer numbered. Old sessions still find their preset.
- **Undo across tracks.** Cmd+Z (Ctrl+Z on Windows) undoes the latest load or removal on any track, and selects it. Remove three tracks and three presses bring them all back. Shift+Cmd+Z redoes.
- **Boost shows the real gain.** The hits' Boost knob now reads −6 to +12 dB (default +6 dB), the gain the hits actually get. The sound is unchanged.
- **Follower source.** A Follower modulator can follow Bus A (the default), Bus B, the Hits or the Output.
- **Lorenz wander.** The morph pad's default wander shape is the Lorenz attractor: two wings, never the same path twice.
- **Morph pad tidy-up.** The shape menu has room for its names, and the moon no longer shows through the MORPH ON button.
- **Tape reels** run the right way.
- **Type tooltips** say what each type is for.
- **About** says what Swooshbuckler does, and the copyright year is the build year.
- **Windows** no longer offers CAF and M4A files, which only load on macOS.
- **README** is a full tour of the window, with a screenshot of each part.

## 1.0.0

First release: the app (macOS and Windows), VST3 and AU.
