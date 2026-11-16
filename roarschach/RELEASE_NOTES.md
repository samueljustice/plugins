## Roarschach 1.3.1

Roarschach 1.3.1 changes how the levels are set, reworks the sound of several modules, re-voices the factory presets for them and adds quicker ways to work with the controls. The creature looks as it did.

Sessions from earlier versions load, and every parameter ID and its place in the host's list is as it was; one parameter is added at the end (OUTPUT's Level Match). Sessions keep every setting, but they will not sound exactly as they did: most of the modules below sound different, and the levels are set in a different way. Latency is unchanged: about 46 ms (2192 samples at 48 kHz).

### Levels you can bounce

IN's Auto Level learns one gain for the take from the performance itself, not from room tone, hum or noise before it, and holds it. A whisper stays a whisper and a roar stays a roar, instead of every phrase being pulled to the same level. It turns down when a louder passage arrives and does not turn up again by itself, so it holds one working level per instance: on a track with several takes at different levels, the loudest sets it. Turn Auto off and on to learn again. A take heard only as breath, hiss or unpitched rasp is raised by at most 12 dB until a pitched voice comes in.

OUTPUT's level match learns what the modules do to the level and holds that one gain. It no longer rides the level or ducks after a loud sound, so CLEAN's leveller and the other dynamics are heard as they are. After a settings change it moves only once the voice has shown a clear difference. A **MATCH** switch at the top of the OUTPUT tile (Level Match in the host, on by default) turns it off.

What IN, ROAR and the match have learnt is kept when playback stops and saved with the session, so a region plays the same from wherever playback starts and a bounce matches what you heard. The levels are learnt on the first play of a take: play it through once, loudest part included, before bouncing. Each factory preset starts from a match measured for it on dialogue, so the first play is already close.

Mix holds the level at every setting. A processed voice close to the dry (light settings, CHAMBER alone, ENSEMBLE alone) was up to 3 dB louder at mid Mix; it now stays level. On heavy drive or the vocoder, an ess straight after a loud vowel can sit up to about 3 dB under at mid Mix.

Push's default is +6.5 dB.

### What the modules do

- **ROAR** changes the tone with Drive, not the level: it learns one makeup gain per setting, holds it and saves it with the session. The first play at a new Drive, Tone or Voicing is a learning pass: the level settles over the first seconds and again at the first louder passage. From the second play it holds. Changing anything before ROAR in the chain starts a new learn.
- **DE-ESS** dips a shelf only while an ess sounds and follows how bright the voice normally is, so a breathy or rasping performance keeps its top end while esses in speech are caught. Rasp comes through much brighter at the same setting. Sessions and user presets saved in 1.1 to 1.3.0 load with their De-ess setting moved so that esses are reduced as much as they were.
- **TAME** compares the top end with the voice's usual share of it, so a voice that is bright all the way through keeps its top at low settings. Below about 7% a screech keeps its edge; use 15 to 25% to hold a scream back.
- **VOCODER**: esses and breath come out at the level they went in, not hotter than the vowels. After a host reset it starts exactly as a new instance does.
- **DETAIL** is clean on low notes and after PITCH down or SUB.
- **PITCH** places word starts and plosives once, with no faint early copy, and no longer repeats the last cycle after a dead stop. In Keep, growls, croaks and breathy sound move with the pitch while esses stay; at large shifts a rough voice can flutter in tone between its pitched and unpitched parts. The VOCODER, RING Follow, SUB, OVERTONE Tracked and GROWL Throat take each phrase's pitch about 30 ms sooner, so there is no step at the start of a phrase. A bounce at an odd buffer size matches playback.
- **SUB**: Deep divides a high voice down until the layer is a real sub (46 to 131 Hz), so there is weight under a howl or a screech. The layer follows the whole voice and does not come out above it at the default Level and Drive. The register no longer flips octave inside a phrase when a low voice sits on its edge; each phrase after a pause picks its own.
- **OVERTONE**: Chest, Throat, Beast, Speaker and Broad move up an octave or two with a high voice, so they work on howls and higher voices. Each phrase picks its own octave, so bounce from a pause. The layer never comes out above the voice.
- **SWIRL**: Deep 6 keeps its level over the whole Amount travel. At 44.1 and 48 kHz the top octave matches higher sample rates, and very hot high notes no longer fold.
- **GROWL**: Sine, Opto, Square and Rattle vary a little from cycle to cycle above about 10 Hz, like a rough voice, instead of a fixed chop; Rate sets the average. Grind sits at Warm's level on a voice at working level (it was 2 to 5 dB quieter).
- **ENSEMBLE** keeps the low end out of the chorus and mono below about 60 Hz, and wide settings no longer turn the image out of phase.
- **SHIMMER** in Up+Down sounds the same at 44.1 kHz and at 88.2 kHz and above as at 48 kHz.

### Factory presets

The presets are re-voiced for these modules and sound different from the same presets in 1.3.0. Every one except Loud & Contained, Crunch Lift and Smashed plays within about 0.6 LU of Init on dialogue once learnt. Rasp comes through 5 to 9 dB brighter above 5 kHz on most presets, since DE-ESS no longer dulls it; five presets take a little more TAME for it. OVERTONE layers are either raised to where they are heard or switched off, GROWL rates are spread out instead of shared, the Grind presets chop a little deeper and the choruses sit a little lower. Sessions that used a factory preset keep their own saved settings; load the preset again to hear its new voicing.

### Easier to use

- **Knobs**: Shift-drag for fine control. The mouse wheel moves a knob in round steps (PITCH a semitone at a time, with Shift a cent). Right-click a knob to reset it or type a value, with the host's own options below where the host offers them.
- **Mode chips** open one list of all the module's choices under headings, without submenus. Scroll over a chip, or Tab to it and press Up or Down, to step its first choice without opening the list.
- **Solo**: Alt-click a module's On switch to hear it alone, and Alt-click it again to turn back on the modules that were on. Which ones were on is forgotten when another preset or the session is loaded.
- **Hold in Randomise All**: right-click a module's die to keep that module as it is when Randomise All rolls the rest. A held die shows a small ink pin, and holds are saved with the session. Shift-click Randomise All to roll only the modules that are on.
- **Names in the host** start with the module: PITCH Size, ROAR Drive, CHAMBER Resonance. Automation lanes in existing sessions show the new names; they are the same parameters.

### Downloads

| File | Contents |
|---|---|
| `Roarschach-1.3.1-macOS-Installer.pkg` | macOS installer (AU + VST3, universal) |
| `Roarschach-1.3.1-Windows-Installer.exe` | Windows installer (VST3) |
| `Roarschach-1.3.1-macOS-AU.zip`, `Roarschach-1.3.1-macOS-VST3.zip`, `Roarschach-1.3.1-Windows-VST3.zip` | The plug-ins on their own |

The macOS downloads are signed and notarised. Universal (Apple silicon and Intel), macOS 11 or later. Windows: 64-bit Windows 10 or later.

The manual, with a tour of every control: [roarschach/README.md](https://github.com/samueljustice/plugins/blob/main/roarschach/README.md).
