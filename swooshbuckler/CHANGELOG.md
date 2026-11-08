# Swooshbuckler changelog

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
