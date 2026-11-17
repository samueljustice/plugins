# Sound Advice

A page of advice from people who work on film, TV and game sound: sound designers, mixers, editors and audio directors, plus a Disney Imagineer on leading creative work. Pick what you're working on (mixing, trailers, creatures, dialogue, silence, running a creative team...) and it shows you something one of them said or did about it. Roll the die for another.

It behaves like a book. It opens on a closed cover, which swings open onto an entry, and the entry is written in letter by letter. Rolling the die or choosing a category shuts the book and opens it at the new page; going back turns the page.

It's a VST3 / AU plug-in (audio passes straight through, untouched) and a standalone app (no audio device at all), so it can sit on a mix bus or on a second screen. It has a light theme and a dark one.

![Sound Advice](docs/screenshot.png)

It comes in [Sammy J's Plugins](https://github.com/samueljustice/plugins/releases/latest), one installer for all of these plug-ins: on macOS the app, VST3, AU and AAX (signed and notarised, universal, macOS 11+), on Windows the app, VST3 and AAX.

## What's in it

910 entries from 147 people and one studio, kept to concrete, technically creative advice: sources and processing, recording, design methods and specific mix decisions, each tied to the production it came from. The sound designers and mixers all have film, TV or game credits on IMDb, and every entry links to the interview, article or SoundWorks Collection video it came from. Nothing predates 2005, either in the source or in the work it describes, apart from the Star Wars prequels. The Creativity & Teams category draws on a Disney Imagineer's book about creative work: leading teams, finding a theme, critique, risk and knowing when to stop. The Sweet Justice Tips category is 48 working tips from Sweet Justice Sound, mostly on trailers, music editing and mixing; they aren't tied to a production and link to the studio's site. Two kinds:

- **Quotes**, in quotation marks and italics: their own words, kept short. When a quote is about a particular film, show or game, it's named above it ("On Arrival").
- **Field notes**: a technique, decision or story from that source, retold in a sentence or two. "Field note" is printed above them.

Click a person's name to open their IMDb (or other profile) page; click the source line at the foot of the page to read the original article. Right-click to copy.

### Categories

| Category | Entries |
|---|---|
| Mixing | 293 |
| Action & Impact | 114 |
| Trailers | 54 |
| Creatures & Voices | 87 |
| Ambience & Worlds | 121 |
| Dialogue | 182 |
| Field Recording | 200 |
| Weapons & Vehicles | 62 |
| Sci-Fi & Magic | 153 |
| Horror & Tension | 54 |
| Silence & Restraint | 75 |
| Music & Sound | 90 |
| Games | 31 |
| Story & Emotion | 127 |
| Craft & Technique | 237 |
| Collaboration & Career | 82 |
| Creativity & Teams | 75 |
| Sweet Justice Tips | 48 |

**Anything** deals from all of them. Each category is a shuffled deck: you see every entry in it once before any comes round again.

## Using it

| | |
|---|---|
| Die, Return or → | Another from this category |
| ‹ (bottom left), ← or Backspace | The one before |
| ‹ › at the top, ↑ ↓ | Previous / next category |
| Category name | Choose from the list |
| Moon / sun | Night or paper |
| ⌘C / Ctrl+C, or right-click | Copy |

Drag the bottom-right corner to resize. The category and theme are saved with your session; every time you open it (or reopen the window) it deals a fresh entry.

## The database

The entries live in `data/*.json`, one file per research group, each entry with its person, kind, categories and source (title, publication, URL, year). `tools/build_quotes.py` checks them (known person and category, a source URL, quotes 15 words or fewer, no duplicates), gives each a stable id and writes `Resources/quotes.json`, which is embedded in the plug-in:

```bash
python3 tools/build_quotes.py
```

To add an entry, add it to a data file (or a new one) with its source, run the script and rebuild. `data/credits.json` sets the credits line shown under each name. To leave someone out, put their name in `data/omit.txt`; to drop a single entry (too generic, a fragment, a near-duplicate), put its id in `data/omit-entries.txt`. Either way the research stays in the data files and the build just skips it. `data/contexts.json` names the production a quote is about, shown above it. `data/rewrites.json` changes an entry's wording (for example to name who did the work) without changing its id. The `SoundAdviceTests` program checks the built database: a sanity floor of 300 entries, every category stocked, and the no-repeat dealing.

## Building

Needs JUCE 8.0.8 at `../SDK/JUCE` (see the [root README](../README.md)).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/SoundAdviceTests_artefacts/Release/SoundAdviceTests
```

`-DSOUNDADVICE_TOOLS=ON` also builds `SoundAdvicePreview`, which renders the editor to PNGs without a host:

```bash
./build/SoundAdvicePreview_artefacts/Release/SoundAdvicePreview previews 6
./build/SoundAdvicePreview_artefacts/Release/SoundAdvicePreview --hero docs/screenshot.png <paper-entry-id> <night-entry-id>
```

`--ink`, `--turn` and `--cover` (each `<out.png> <entry-id>`, with `night` after it for the dark theme) render frames of the writing, the page turn and the cover opening, for checking the animations without a window. `--icon Resources/icon.png` redraws the app icon (and `icon.ico` beside it). The `--hero` form makes the image at the top of this page; entry ids are in `Resources/quotes.json`.

## Changes

See [CHANGELOG.md](CHANGELOG.md).

## Releases

Push a tag `soundadvice-vX.Y.Z` from `main` to build, sign, notarise and publish a release (`.github/workflows/build-release_soundadvice.yml`): a macOS installer (`.pkg`, with the app, VST3 and AU), the app on its own (`.dmg`), a Windows installer (VST3 and app) and zips of each. The tag's number becomes the version everywhere. Pull requests that touch `soundadvice/` build, test and package without releasing, and the workflow can be run by hand for a dry run.

## Credits

Type: [Cormorant Garamond](https://github.com/CatharsisFonts/Cormorant) by Christian Thalmann, under the SIL Open Font License (`Resources/Fonts/OFL.txt`).

The words and work belong to the people quoted. Each entry links to where it came from; go and read the originals, they're better than any summary.
