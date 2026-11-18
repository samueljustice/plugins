# De-Honk changes

## 0.0.2

The second test build. It is published as a pre-release and is not in the Sammy J's Plugins installer.

**The name.** Hosts now list it as SammyJs De-Honk, and its files are called `SammyJs De-Honk`. The installers remove the 0.0.1 files. To the host it is the same plug-in, so sessions saved with 0.0.1 should still open.

**Sound and controls**

- Dialogue sidechain. Route the dialogue stem to the plug-in's sidechain (the key input in Pro Tools) and switch Dialogue on: a band the dialogue accounts for is left alone, and with Snap on so is a click that is the dialogue's. The bar beside it sets how far, and a lamp shows the stem arriving. The stem can arrive at any level: De-Honk finds it in the mix and measures how loud the dialogue is there, in each channel, so a quiet send or a stereo stem summed into a mono key works as well as one at mix level, and in surround with Link at Grouped or Unlinked only the channels the dialogue is in are left alone. The lamp turns red if the stem cannot be found in the mix or is most of it. Measured on a library of 47 finished mixes with their dialogue stems: in the 36 mixes where a cut was open in a band that is the dialogue's, that cut fell from 1.6 dB to under 0.06 dB on average (0.12 dB with the stem sent 12 dB quiet), and on a typical mix 96% of the cut elsewhere was kept. On six of them with Snap on, Snap cut into 8 to 24% of the moments that were mostly dialogue without the stem and 0.1 to 1.5% with it. A new factory preset, "Master bus, dialogue keyed", has it switched on.
- Detection, Fast or Steady, a switch under the knobs. Fast cuts a build-up as soon as it shows, as before. Steady waits until a build-up has held for a while, and leaves a lift alone when it is broad rather than narrow: it does about half as much on real mixes, is steadier, and is kinder to dense, noisy textures, but it is slower to catch a new build-up, and in a dense mix it may leave a mild one alone. A new factory preset, "Master bus, steady", is the defaults at Steady.
- Each of Woof, Tub, Fog and Bark can have its own attack and release. A range is locked to Speed and Release until its lock is opened.
- Link is now three choices, Linked, Grouped and Unlinked, where it was a percentage. Grouped links the front pair, the centre, the surrounds and the heights each within their group, and is offered in surround only. A 0.0.1 session comes back as Linked from 50% up and Unlinked below.
- Diff fades to the untreated signal and back over 80 ms, so switching it no longer clicks. Its level match follows the treated and untreated levels over about a second and a half, all the time, so where nothing is cut the two match.
- Impact Guard's lamp is now its switch: it turns the guard off and on again at the amount it had, and fills with yellow while the guard is up.
- Snap is called Snap in the host's parameter list (it was listed as Click).
- The factory preset for use straight after a bass enhancer is now called "After a bass enhancer". A session saved on it under its old name still comes back on it.

**The window**

- The Threshold and Max Cut can be dragged in the band view, like the range edges: the Threshold by its grip on the level bar or its line across the bands, Max Cut by its dashed line. The pointer and a highlight show what can be dragged, a label shows the value, Shift moves it finely, and a double-click puts it back. The two knobs are still there.
- The band view's bars, its level bar and the Threshold now share one scale, so the Threshold's line is at the same level on all of them.
- The ranges are tabs on a shelf under the band view, each in its range's colour, without meters. A tab's lamp switches the range; a click on its name selects it. With a range selected, only its creatures move in the pond, the band view fades the others, and its attack, release and lock appear under the shelf.
- Mix and Trim have moved under the listening buttons.
- Tooltips wait a little over a second, open beside the control they describe and not over it, and close on a click or when the pointer moves away. They are short.
- Menus, the save and delete dialogs, text boxes and scrollbars are drawn in the window's own style, inside the window.
- An About page, opened by clicking the name: the version, links to Samuel's site and email, a check for a newer version (test builds included) and the licence.
- The update check the other Sammy J plug-ins have, for full releases: a message when a newer full release is out. A test build does not look by itself; the About page's check does.
- Saving a preset says so when the name cannot be used or the preset could not be written.

**The pond**

- It runs at 60 pictures a second from the screen's own refresh (it was 30) and is drawn at the display's full resolution, with smoothed edges on a display of ordinary density.
- It is calmer. A goose comes only for a cut that has been open a moment and stays a few seconds at least. They arrive one at a time, at most two to a range, and keep out of each other's way when they land and leave. The recordist picks one goose and stays with it, turns before he sets off, walks or runs to it with a proper stride, and swings the boom only now and then. Only the goose he is working on honks at him. The small jokes come one at a time.
- With a range selected, the creatures of the other ranges stay exactly where they are, faded back: none arrives, leaves or moves.
- The weather turns only when De-Honk is kept hard at work, and the creatures keep their colours when it does.
- A tree stands at each range edge, straight above that edge's handle in the band view, in the colour of the range to its right, and moves when the edge is dragged.
- The reeds are scenery now, in clumps, and no longer show the spectrum; the band view does that.
- The creatures can be clicked, and the pointer becomes a hand over them: a turtle topples off the log into the water, the beaver waves or slaps its tail, the frog jumps, a goose tumbles away, the recordist tips his hat or waves, the dragonflies scatter, the goose on the water tips up. None of it changes the sound.
- Dragonflies come out while the dialogue sidechain is being listened to.

## 0.0.1

The first test build, for a few people to try on real mixes. It is published as a pre-release and is not in the Sammy J's Plugins installer.

- VST3, AU and AAX plug-ins and a standalone app, for macOS and Windows. Mono, stereo and surround up to 7.1.4.
- Four modules (Woof, Tub, Fog, Bark) that find build-ups in the low end and low mids and take them down with narrow cuts, and Snap for clicks around 2.5 kHz.
- Threshold, Depth, Max Cut, Sharpness, Speed and Release; Impact Guard, Link, Mix, Trim, Lookahead and Bypass; module edges dragged in the band view; factory and user presets.
- Solo Honk and Diff, to hear what is removed and what the mix sounded like before.
