# De-Honk

A plug-in by **Samuel Justice** that takes the honk out of the low mids of a finished mix.

In a trailer mix, many of the sounds and much of the music have each been through a bass enhancer (RBass, MaxxBass and the like), a saturator or another harmonic generator. Each one adds harmonics above the bass so it reads on small speakers. Summed across the mix, those harmonics pile up between roughly 120 and 500 Hz as a nasal, boxy build-up. De-Honk listens for that build-up as the mix plays and takes it down with narrow cuts that come and go with it. The bass and the body of the mix are left alone.

It goes on the master bus, before the limiter. It works in mono, stereo and surround up to 7.1.4. It comes as VST3, AU and AAX plug-ins and as a standalone app, for macOS and Windows.

**This is an early test build (0.0.1).** It is for a few people to try on real mixes and report back. Controls, presets and the sound may change before the first full release, and sessions saved with it may not open the same way in a later version.

<!-- TODO: screenshot of the De-Honk window (docs/screenshot.png), once the interface is settled. -->

## Getting the test build

The test build is on its own release page, not in the Sammy J's Plugins installer. Samuel sends the link to the people testing it. There is one installer for each system:

- **macOS** (`DeHonk-0.0.1-macOS-Installer.pkg`): the VST3, AU and AAX plug-ins and the app. Choose Customize to pick. Signed and notarised; universal (Apple silicon and Intel), macOS 11 or later.
- **Windows** (`DeHonk-0.0.1-Windows-Installer.exe`): the VST3 and AAX plug-ins and the app. 64-bit Windows 10 or later. The installer is not code-signed, so Windows may warn before running it.

The AAX for Pro Tools is signed by hand after the release is made. Until it is, the installers on the release page have no AAX in them; download them again once it is there.

## How it works

De-Honk splits the range it watches into four modules, each with its own kind of build-up, and has a fifth, Snap, for clicks. It measures the mix in narrow bands, compares each band with its neighbours, and cuts a band that stands out. A cut goes in and lets go as the build-up comes and goes, so a passage without honk passes through untouched. It acts only while the mix is louder than the threshold.

Bass enhancers add their harmonics above a low fundamental. De-Honk protects a fundamental it can hear below a band, so a note's own pitch is kept while the harmonics piled on top of it are reduced.

## Controls

**Threshold.** De-Honk acts only while the mix is louder than this. It is fully off below it and fully on 6 dB above. At the bottom of the range, Always, it acts at any level. The bar at the left of the band view shows the mix level with the threshold marked on it.

**Depth.** How much of the detected excess is removed. No cut goes deeper than Max Cut. At 0 nothing is cut.

**Sharpness.** How wide a build-up has to be to count. Broad responds to a whole bump; Narrow responds only to single ringing resonances. The cuts themselves are narrow either way.

**Speed.** How quickly a cut goes in once a build-up has been found. Release sets how it lets go.

**Max Cut.** The deepest any one cut may go, from 3 to 12 dB, whatever Depth is set to. Two cuts side by side never add up to more. The dashed line across the top of the band view marks it.

**Release.** How slowly a cut lets go once its build-up has gone, from 50 ms to 1.5 s. Longer holds each cut steadier; shorter follows the mix more closely. It is for cuts that have settled on a clear build-up: a light cut that comes and goes, or one whose build-up has moved to the next note, lets go quickly whatever is set here. Under the Threshold every cut closes straight away.

### Modules

Each module covers part of the range. Click one to switch it on or off. Its light is on while it is cutting, and its meter shows how deep: full is Max Cut. Drag the handles in the band view to move the edges between modules.

- **Woof:** the low end barking on every hit, about 60 to 110 Hz. Off at first.
- **Tub:** tubby weight just above the bass, about 110 to 200 Hz. This is where added bass harmonics pile up first. On at first.
- **Fog:** low mids clouding over when music and effects stack up, about 200 to 400 Hz. On at first.
- **Bark:** hard, shouty midrange, about 400 Hz to 1.5 kHz. Off at first.
- **Snap:** sharp clicks and pops around 2.5 kHz. It takes the edge off a pop that the rest of the sound does not share, and leaves drum hits and steady sounds alone. It works at any level, whatever the threshold. Off at first.

### The second row

**Impact Guard.** Keeps the cuts out of big hits. When something lands across the whole spectrum at once (an explosion, a slam, a hit), no new cut is made while it sounds, and a cut that is already open is eased off as far as the hit drowns out what it was there for, by up to this much. The cuts come back as the hit dies away. At Off, hits are treated like anything else. The lamp beside it is lit while the guard is up.

**Link.** At 100% every channel shares one detection, so the image does not move. Lower values let each channel be cut on its own.

**Mix.** The balance between the untreated signal (0%) and the treated signal (100%).

**Trim.** Output level, after the mix.

**Lookahead.** Off, 5, 10 or 20 ms. Delays the audio so each cut is in place when a transient arrives. It adds the chosen latency, which is reported to the host.

**Bypass.** Passes the signal through untreated, with the same latency and without a click.

**Presets.** The arrows step through the presets, and the name opens the list. A * after the name means the settings have changed since the preset was loaded or saved. Save stores the current settings as a user preset; user presets can be deleted, factory presets cannot.

## Listening

Two buttons beside the knobs let you hear what De-Honk is doing. One works at a time, and the band view says which one you are hearing. Neither is saved with the session.

**Solo Honk** plays only what is being removed. If it sounds only ugly, the settings are right. If you hear wanted bass or body in it, lower Depth or switch off the lowest module.

**Diff** plays the untreated signal, latency-matched, so you can compare. Hold it to listen, or click it to keep it on and click again to return. The host's bypass is not touched.

## The pond

The picture at the top shows what De-Honk is doing, slowly enough to glance at while mixing. When the mix is quiet it settles down.

- **Geese** are the cuts. Each lands where a cut is, and the sound recordist with the boom pole chases them off.
- **The jumping fish** is Snap taking out a click.
- **The beaver's dam** grows while the low mids stay clogged and noisy, and shrinks as they clear.
- **The turtles** pull into their shells while Impact Guard is holding back for a hit. With Impact Guard off they sleep.
- **The frog** tenses as a cut nears Max Cut and leaps off its lily pad when one reaches it.
- **Dragonflies** will come out while a sidechain is driving the detection. The sidechain input is not in this test build, so you will not see them yet.
- In Bypass the recordist sits down with a flask; in Solo Honk he listens closely; in Diff he looks from side to side.

## Surround

De-Honk accepts the same layout in and out, from mono to 7.1.4. With Link at 100% every channel gets the same cuts, so nothing moves in the image. The LFE channel is passed through untouched.

## Reporting back

Tell Samuel what you used it on, what it did well and where it got in the way: a cut you could hear working, wanted bass or body going missing, a hit that lost its weight, a host or layout where it misbehaved. A short bounce with De-Honk on and off helps more than a description.

## Credits

De-Honk is by Samuel Justice. RBass is a Waves product, named here only to explain the kind of build-up De-Honk is for.
