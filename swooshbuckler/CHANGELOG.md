# Swooshbuckler changelog

## Unreleased

- **Swirl no longer clicks.** The phaser and flanger sweeps are now smooth from sample to sample. Before, they moved in small steps about 1,500 times a second, which put a faint grit on clean, tonal sounds at high Swirl.
- **Doppler pitch like a real fly-by.** A pass now rises in pitch on the way in by more than it drops on the way out (at Doppler 100% and Bend 100%: about 9.4 semitones up, 6 down), the way a passing car or plane sounds. The total bend is the same size. High Bend settings sound more like a scream coming in and less like a dive.
- **Bend changes the pitch only.** How a pass swells and brightens is set by Doppler alone; turning Bend down no longer flattens the swell or makes the bus louder.
- **The two passes of a bus stay half a pass apart at any Chaos**, so a bus keeps a steady level (with Auto Ride off, a bed no longer dips and bumps at random as Chaos goes up). Chaos still varies each pass's length, speed, closeness and direction.
- **Low End's sub octave is steadier.** It follows the lowest part of the sound (about 40 to 120 Hz) and no longer flips at random on the noise between sounds, which was heard as rumble after a sound stopped.
- **Fill echoes are different on every track.** Each track has its own echo times (around 0.5, 1 and 2 s, in uneven ratios), so layered tracks no longer echo together on a half-second grid.
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
