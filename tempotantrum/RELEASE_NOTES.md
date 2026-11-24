## Tempo Tantrum 0.1.0 (test build)

Tempo Tantrum is a macOS app by Samuel Justice that listens to a piece of trailer music and puts a bars and beats grid on its drum hits, ready to become a Pro Tools tempo map. It splits the music into sections wherever the beat stops or jumps, follows the beat within each section, and places pins where the grid is fixed to the audio. Anything it had to guess at is listed as a place to check.

This is the first build for testing. How it works, the window and the saved map file may change before the first full release. What each control does is in the [Tempo Tantrum README](https://github.com/samueljustice/plugins/blob/main/tempotantrum/README.md).

### In this build

- Open WAV, AIFF or anything else macOS can decode, with Open… or by dropping the file on the window.
- Set which bar and beat the first hit is. The tempo is found by itself, or typed in or tapped as a hint.
- An optional drum or percussion stem to find the hits on, placed on the attacks in the mix.
- Play with a click, louder on beat 1. N jumps to the next place to check and loops it.
- Drag, add and delete pins, start a new section where the beat restarts, and say which bar a hit is. Every change can be undone.
- Save the map beside the music as `<music file name>.tempotantrum.json`. It opens again with the music.
- Write to Pro Tools: puts a bar|beat marker on every pin by running Pro Tools' Event > Identify Beat from outside, at about a second a pin. Set the Main Counter to Samples and select the music clip first. It needs the macOS Accessibility permission and types into Pro Tools, so leave the keyboard and mouse alone while it runs. It can check every pin afterwards and send the undos. Use a copy of the session until you trust it.
- Export the click as a mono 24-bit WAV, as long as the music, under More in the footer.
- Test in Pro Tools…, under More: the same steps one stage at a time, with a log of everything it sees.

### Not in this build

- It does not look after the tempo maps of other trailers further down the session. Check them after a write.
- A tempo change inside a section is not followed: between two pins the tempo is constant.
- Single hits placed to picture, away from any beat, are not followed.
- Mac only.

### Download

| File | Contents |
|---|---|
| `TempoTantrum-0.1.0-macOS.dmg` | The app. Open the disk image and drag Tempo Tantrum to Applications. |

Signed and notarised. Universal (Apple silicon and Intel), macOS 11 or later.
