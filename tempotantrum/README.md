# Tempo Tantrum

A macOS app by **Samuel Justice** that listens to a piece of trailer music and puts a bars and beats grid on its drum hits, ready to become a Pro Tools tempo map.

This version does not write to Pro Tools. It finds the hits, maps them, lets you check and correct the map, plays it back with a click, saves the map beside the music and exports the click as a WAV file. Writing the map into a Pro Tools session comes in a later version.

## What it does

Trailer music is cut to picture. The beat runs for a while, stops for a riser, a dropout or an edit, and comes back at a moment that has nothing to do with the old grid. Tempo Tantrum does not try to count beats through those gaps. It splits the music into sections wherever the beat stops or jumps, maps each section from its own first hit, and asks you which bar each restart lands on.

Within a section it follows the beat from hit to hit and places pins: points where the grid is fixed to the audio. Between two pins the tempo is constant, so the pins are where a tempo map needs its events. Anything it had to guess at is listed as a place to check.

## How to use it

Open the music with **Open…** or by dropping the file on the window. WAV and AIFF files open, and so does anything else macOS can decode. Tempo Tantrum listens to it straight away and draws the grid and the pins over the waveform.

Tell it the one thing it cannot hear: which bar the first hit is. Set **First hit is bar** and **Beat** (for a trailer cue that starts on bar 40, set bar 40, beat 1). If the first hit it picked is the wrong one, right-click the hit that should be first and choose **This is the first hit**. It estimates the tempo by itself; if it settles on the wrong one (half or double speed, say), type a rough tempo into **Tempo** or tap along with **Tap** or the T key. It refines whatever you give it from the audio, and shows the tempo it found beside the box. Clear the box to let it estimate again.

If you have a drum or percussion stem, add it with **Add drum stem…** (or drop it on the window and choose **Drum stem**). **Listen to** then switches between finding the hits on the full mix and on the stem. Hits found on the stem are still placed on the start of the attack in the mix. The stem must be at the same sample rate as the music and the same length, within half a second.

Play it with **Play with click** (or the space bar) and listen. The click is higher and louder on beat 1 of each bar. The click and music levels are beside the button.

The badge in the footer says how many places need checking. Tab jumps to the next one, zooms the view to it with two bars either side, and loops that stretch with the click. The line at the bottom of the window says what the app was unsure of there. Escape, or the space bar, stops the loop.

To correct the map:

- Drag a pin to move it. It snaps to the nearest hit; hold Alt to place it freely. A moved pin keeps its beat.
- Double-click near a hit to add a pin there. It takes the nearest whole beat of the grid.
- Select a pin and press Backspace or Delete to remove it. A pin you delete stays deleted when the map is worked out again.
- Right-click where the beat comes back after a riser and choose **The beat restarts here** to start a new section there.
- Right-click a pin and choose **This hit is bar…** to say which bar and beat it is. This is how you confirm or correct the bar a section restarts on.

After every change the map is worked out again around your pins, which it always keeps exactly where you put them. Every change can be undone.

**Save map** writes the map beside the music. Quitting, or opening other music, with changes that are not saved asks whether to save them. **Export click…** writes the click as a WAV file to line up in Pro Tools by hand until the app can write the tempo map itself.

## Keys

| Key | What it does |
| --- | --- |
| Space | Play with the click, or stop. Stops a loop. |
| Tab | Jump to the next place to check and loop it. Shift-Tab goes back. While a field is being typed in or has the focus, Tab moves to the next field and Escape returns to the window's keys. |
| Escape | Stop looping and keep playing. |
| T | Tap the tempo. Taps more than two seconds apart start a new count. |
| Left, Right | Move to the previous or next pin. |
| Backspace, Delete | Delete the selected pin. |
| Cmd-O | Open music. |
| Cmd-S | Save the map. |
| Cmd-Z, Shift-Cmd-Z | Undo, redo. |

## The saved map

The map is saved as `<music file name>.tempotantrum.json` in the same folder as the music, so `Cue 3.wav` gets `Cue 3.wav.tempotantrum.json`. It is a plain JSON file holding:

- the music file's name, its length in samples, its sample rate and a short fingerprint of the audio,
- what you told the app: the first hit, its bar and beat, the tempo, which source it listens to and the drum stem's file name,
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

It maps the file, lays the window out at 1280 by 760 (or `--width` and `--height`), saves it as a PNG at twice that size and quits. `--drums <file>` adds a drum stem.

`--snapshot-empty window.png` does the same with no music open. `--selftest music.wav` copies the file to a temporary folder, maps it, edits the map, changes the first bar, adds the copy as its own drum stem, saves the map, opens it again and compares, exports the click and reads it back, then prints `PASS` or `FAIL`.

## Credits

Created by Samuel Justice.

- Application framework: [JUCE](https://juce.com).
- UI font: [Azeret Mono](https://github.com/displaay/Azeret), SIL Open Font License 1.1.

## License

MIT License. See [LICENSE](LICENSE) for details. Third-party licences are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
