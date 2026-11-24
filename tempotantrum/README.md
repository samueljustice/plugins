# Tempo Tantrum

A macOS app by **Samuel Justice** that listens to a piece of trailer music and puts a bars and beats grid on its drum hits, ready to become a Pro Tools tempo map.

It finds the hits, maps them, lets you check and correct the map, plays it back with a click and saves the map beside the music. **Write to Pro Tools** then puts the map into the open session by running Pro Tools' own Event > Identify Beat once for each pin. The click can also be exported as a WAV file.

## What it does

Trailer music is cut to picture. The beat runs for a while, stops for a riser, a dropout or an edit, and comes back at a moment that has nothing to do with the old grid. Tempo Tantrum does not try to count beats through those gaps. It splits the music into sections wherever the beat stops or jumps, maps each section from its own first hit, and asks you which bar each restart lands on.

Within a section it follows the beat from hit to hit and places pins: points where the grid is fixed to the audio. Between two pins the tempo is constant, so the pins are where a tempo map needs its events. Anything it had to guess at is listed as a place to check.

## How to use it

Open the music with **Open…** or by dropping the file on the window. WAV and AIFF files open, and so does anything else macOS can decode. Tempo Tantrum listens to it straight away and draws the grid and the pins over the waveform.

Tell it the one thing it cannot hear: which bar the first hit is. Set **First hit is bar** and **Beat** (for a trailer cue that starts on bar 40, set bar 40, beat 1). If the first hit it picked is the wrong one, right-click the hit that should be first and choose **This is the first hit**. It estimates the tempo by itself; if it settles on the wrong one (half or double speed, say), type a rough tempo into **Tempo** or tap along with **Tap** or Shift-T. It refines whatever you give it from the audio, and shows the tempo it found beside the box. Clear the box to let it estimate again.

If you have a drum or percussion stem, add it with **Add drum stem…** (or drop it on the window and choose **Drum stem**). **Listen to** then switches between finding the hits on the full mix and on the stem. Hits found on the stem are still placed on the start of the attack in the mix. The stem must be at the same sample rate as the music and the same length, within half a second.

Play it with **Play with click** (or the space bar) and listen. The click is higher and louder on beat 1 of each bar. The click and music levels are beside the button.

**Pin every** sets how often a pin is kept on beat 1 where a hit is there: every 8, 4, 2 or 1 bars, or **Only where needed** for pins only where the tempo changes.

When the music has been mapped the window shows the first 16 bars. The keys that move about are the ones Pro Tools uses with Commands Keyboard Focus: R and T zoom out and in, Option-A shows the whole file, Option-F fills the window with the stretch between the pins either side, E zooms to two bars and back, and Tab moves the playhead to the next hit. The strip under the waveform is the whole file: drag the box on it, or click anywhere in it, to move the view. Drag in the ruler to move the playhead. Zoomed in, the waveform is drawn as an outline and every hit the app heard is a tick along the bottom of the lane, taller for a stronger hit. The small numbers in the ruler are the tempo of each stretch between two pins.

The badge in the footer says how many places need checking. N jumps to the next one, zooms the view to it with two bars either side, and loops that stretch with the click. The line at the bottom of the window says what the app was unsure of there. Escape, or the space bar, stops the loop.

To correct the map:

- Drag a pin to move it. **Snap**, under the waveform, says what it lands on: **Hits** is the nearest hit within 12 pixels, **Grid** is the nearest beat of the grid as it stood when the drag began (eighth notes too, when zoomed in far enough to tell them apart), and **Off** is anywhere. Hold Cmd while dragging to place it freely whatever Snap says. While it moves, a readout beside it gives its time and how far it has come, and a diamond marks the hit or line it is sitting on. A moved pin keeps its beat.
- Select a pin and press comma or full stop to move it 1 ms earlier or later. With Shift it moves 10 ms, with Option one sample.
- Double-click to add a pin. It snaps the same way and takes the nearest whole beat of the grid.
- Select a pin and press Backspace or Delete to remove it. A pin you delete stays deleted when the map is worked out again.
- Right-click where the beat comes back after a riser and choose **The beat restarts here** to start a new section there.
- Right-click a pin and choose **This hit is bar…** to say which bar and beat it is. This is how you confirm or correct the bar a section restarts on.

After every change the map is worked out again around your pins, which it always keeps exactly where you put them. Every change can be undone.

**Save map** writes the map beside the music. Quitting, or opening other music, with changes that are not saved asks whether to save them. **More** in the footer holds **Export click…**, which writes the click as a WAV file to lay against the music in Pro Tools, and **Test in Pro Tools…**.

## Writing to Pro Tools

**Write to Pro Tools** puts a bar|beat marker on every pin, the way it is done by hand: it moves the Pro Tools cursor to the pin, opens Event > Identify Beat, types the pin's bar, beat and ticks, and presses OK. It does this from outside, with key presses and through the macOS Accessibility permission, so while it runs the keyboard and mouse must be left alone. A pin takes about a second.

Use a copy of the session until you trust it.

It needs the Accessibility permission, which you give once. The sheet says when it is missing: press **Ask macOS for permission**, switch Tempo Tantrum on in System Settings > Privacy & Security > Accessibility, and reopen the app.

In Pro Tools first:

1. Stop the transport.
2. Set the Main Counter to Samples.
3. Select the music clip, so the cursor is at its start.

Then press **Write to Pro Tools**. The sheet says how many pins it will send and which bars they cover, and the sample the Main Counter is at, which it takes as the start of the music. It warns when that sample is 0, when it is where the last run wrote from, and when it is a pin of the last run. If places in the map are still listed to check, the button reads **Write (3 unchecked)** and still works. Press **Write**.

Pro Tools comes to the front and a small panel stays above it with the count, **Leave the keyboard and mouse alone** and a **Stop** button. For each pin the app checks that the Main Counter shows the pin's sample before it opens Identify Beat, and that the dialog shows the bar and beat it typed before it presses OK. If either is not so, it cancels the dialog and stops. It stops as well when Pro Tools is no longer the front application, so nothing is typed into another application, and when a window it did not expect is open. When it has finished it moves the cursor back to the start of the music and Tempo Tantrum comes forward with the result: **Written: 24 pins**, or **Stopped at pin 7 of 24** and why.

**More** in the sheet holds:

- **Music starts at sample**: type the sample where the music clip starts. Left empty, it is read from the Main Counter.
- **Session sample rate**: filled in from the music file. It must match the session.
- **Check each pin afterwards**: once every pin is written, the app goes to each one again, opens Identify Beat, reads the location Pro Tools suggests there and cancels. It presses no OK, so it changes nothing. A pin within 2 ticks of its bar and beat is good. The sheet says **All 24 pins check out**, or lists the pins that do not and how many ticks they are off.
- **Type slowly**: holds each key for longer. Try it if a run stops because a number did not arrive.

**Undo in Pro Tools (24 steps)** brings Pro Tools forward and presses Edit > Undo once for each pin that was written. It asks for a second click first. The app cannot see what Pro Tools did with them, so it says **Sent 24 undos**: look at the session. If you have done anything else in Pro Tools since the run, undo by hand, because Undo undoes whatever came last.

**Copy log** and **Save log…** give the log of every run since the app was opened. It holds the names of the Pro Tools windows, which include the session name, and the name of the music file.

Identify Beat changes the tempo map from each marker onward. The app does not look after the tempo maps of other trailers further down the session: check them after a write.

## Testing the Pro Tools link

**Test in Pro Tools…**, under **More** in the footer, runs the same steps one careful stage at a time and writes down everything it sees. Use it when **Write to Pro Tools** stops and the reason is not clear, or on a version of Pro Tools it has not been run with, and send the log to Sam.

Give the Accessibility permission as above. Before a run:

1. Use a scratch session or a copy, not a session in use.
2. Stop the transport.
3. Set the Main Counter to Samples.
4. Put the cursor at the start of the music clip, or type the sample where the music starts into **Music starts at sample**.
5. Check **Session sample rate**. It is filled in from the music file and must match the session.
6. Do not touch the keyboard or mouse during the run.

Then, in this order:

1. **Look only.** It brings Pro Tools to the front, finds the Identify Beat menu item, reads the Main Counter, opens the Identify Beat dialog, writes down what the dialog contains and cancels it. It presses no key and writes nothing to the session. Press **Copy log** or **Save log…** and send the log.
2. **Send one pin.** It moves the cursor to the first pin, opens Identify Beat, enters the pin's bar and beat and presses OK. Look in Pro Tools: is there a bar|beat marker on the first hit, with the right bar number? Send the log and say what you saw.
3. **Send all pins.** The button asks for a second click. It does the same for every pin, left to right, at about three seconds a pin, and stops at the first one that fails or whose bar and beat it cannot read back from the dialog.

The line at the top of the panel says how the run ended. **Stop**, or Escape in the Tempo Tantrum window, ends a run. Cmd+Z in Pro Tools undoes the Identify Beat steps one at a time.

**Bar|beat entry** chooses between setting the dialog's field and typing into it. Pro Tools does not let the field be set, so the test types either way.

## Keys

| Key | What it does |
| --- | --- |
| Space | Play with the click, or stop. Stops a loop. While it plays, the view moves on a page when the playhead leaves it. |
| Return | Move the playhead to the start. |
| Tab, Option-Tab | Move the playhead to the next hit, or the one before. While a field is being typed in or has the focus, Tab moves to the next field and Escape returns to the window's keys. |
| R, T | Zoom out, zoom in, about the selected pin if it is in view, or else the playhead. Cmd-[ and Cmd-] do the same. |
| Option-A | Show the whole file. |
| Option-F | Fill the window with the stretch from the pin before the selected pin to the pin after it. With no pin selected, the pins either side of the playhead. |
| E | Zoom to two bars around the selected pin or the playhead. Again goes back. |
| Cmd-Option-], Cmd-Option-[ | Draw the waveform taller or shorter. |
| Left, Right | Select the previous or next pin and bring it into view. |
| Comma, Full stop | Move the selected pin 1 ms earlier or later. With Shift 10 ms, with Option one sample. Moves of one pin less than a second apart are undone together. |
| Backspace, Delete | Delete the selected pin. |
| F2, F3, F4 | Snap: Off, Hits, Grid. On most Mac keyboards, hold fn with them. |
| N, Shift-N | Jump to the next place to check, or the one before, and loop it. |
| Escape | Stop looping and keep playing. |
| Shift-T | Tap the tempo. Taps more than two seconds apart start a new count. |
| Cmd-O | Open music. |
| Cmd-S | Save the map. |
| Cmd-Z, Shift-Cmd-Z | Undo, redo. |

With the mouse: the scroll wheel or a two-finger swipe scrolls along the music, Option or Cmd with the wheel zooms about the pointer, and so does a pinch. Double-click in the ruler to show the whole file.

## The saved map

The map is saved as `<music file name>.tempotantrum.json` in the same folder as the music, so `Cue 3.wav` gets `Cue 3.wav.tempotantrum.json`. It is a plain JSON file holding:

- the music file's name, its length in samples, its sample rate and a short fingerprint of the audio,
- what you told the app: the first hit, its bar and beat, the tempo, how often pins are placed, which source it listens to and the drum stem's file name,
- the pins you placed or moved, and the pins you deleted,
- the finished map: every pin's beat, and its time in seconds and in samples.

When you open music that has a map beside it, the map is opened with it and the line at the bottom of the window says so. The fingerprint makes sure the map belongs to that audio: if the file has been changed since, the map is left alone.

The click is exported as a mono 24-bit WAV at the music's sample rate and exactly as long as the music, named `<music name> CLICK.wav` unless you choose another name.

## Building

### Prerequisites

- CMake 3.22 or higher
- Xcode Command Line Tools
- JUCE 8.0.8 at `../SDK/JUCE`, relative to this folder
- The UI font is read from `../pitchflattener/Resources/Fonts`

### Build instructions

From the repository root:

```bash
cmake -S tempotantrum -B tempotantrum/build -DCMAKE_BUILD_TYPE=Release -DTEMPOTANTRUM_TESTS=ON
cmake --build tempotantrum/build --config Release
tempotantrum/build/TempoTantrumCoreTests_artefacts/Release/TempoTantrumCoreTests
```

The app lands in `tempotantrum/build/TempoTantrum_artefacts/Release/Tempo Tantrum.app`. Leave out `-DTEMPOTANTRUM_TESTS=ON` to build only the app.

`TempoTantrumCoreTests` checks the listening engine on made-up music with known answers. `tt-cli` maps a music file from the command line, without the window.

The app also takes a picture of its own window without opening one, which is how the look is checked:

```bash
"Tempo Tantrum.app/Contents/MacOS/Tempo Tantrum" --snapshot music.wav window.png --bar 40 --bpm 165
```

It maps the file, lays the window out at 1280 by 760 (or `--width` and `--height`), saves it as a PNG at twice that size and quits. `--drums <file>` adds a drum stem. `--view-start` and `--view-end` (seconds) set the stretch in view, `--select-pin N` selects a pin, `--snap hits|grid|off` sets Snap, `--keys "right right t opt+f"` presses keys, `--drag-pin N --drag-to S` shows a pin part-way through a drag, and `--print-view` prints what is in view.

`--test-panel-snapshot` takes the same arguments and has the Pro Tools test panel open. `--write-sheet-snapshot` has the Write to Pro Tools sheet open, in the state given by `--state ready|permission|notrunning|running|checking|finished|stopped`, with `--more` to open More and `--float panel.png` to save the floating panel as well. `--write-test` runs the write against an application that is not running and prints its log, which ends at "Pro Tools is not running". `--ax-selfcheck` prints whether macOS lets the app control other applications and whether Pro Tools is running, and touches nothing.

`--snapshot-empty window.png` does the same with no music open. `--selftest music.wav` copies the file to a temporary folder, maps it, edits the map, changes the first bar, adds the copy as its own drum stem, saves the map, opens it again and compares, exports the click and reads it back, then prints `PASS` or `FAIL`.

## Credits

Created by Samuel Justice.

- Application framework: [JUCE](https://juce.com).
- UI font: [Azeret Mono](https://github.com/displaay/Azeret), SIL Open Font License 1.1.

## License

MIT License. See [LICENSE](LICENSE) for details. Third-party licences are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
