# Swooshbuckler v3: making it fun and piratey (without kitsch, posh or unreadable)

Research brief, 2026-11-06. This is research only. No source code was changed.
Read it with `docs/experiments/01-simplicity-ux.md` (simplicity) and `02-auto-classify-and-name.md`
(material detection), which are still valid.

What the user said, in short:
- v1 (navy, Azeret Mono all caps, brass wheels) was "too posh".
- v2 (grey DAW) was "boring, way too complex".
- The HTML mockup (parchment, chips, Lilita One) was "horrible". Its tracks "make no sense". The user wants tracks
  to **appear as you drop sounds**, **named after the material the app hears**, and a **cool OpenGL pirate ship or
  something fun**.

The lesson from all three: the fun has to come from **one living, animated hero object** plus **juicy
feedback**. Pirate fonts, parchment textures and themed nouns do not deliver it. The controls around the hero stay
few, big and plainly labelled.

---

## 1. What the references teach

### Games with pirate or nautical art direction

| Reference | What makes it work | Take for Swooshbuckler |
|---|---|---|
| **Sea of Thieves** (Rare) | The art director described the goal as a "timeless" pirate playground [1]. The GDC 2018 talk says "a few simple rules" are applied to every visual in the game [2]. Analyses of the look list: forms simplified, with complexity built by combining simple objects; things a bit "wonky" but believable; heavy wear, patches and repairs; light used as the main mood lever [3]. The water behaves realistically even though the art is stylised [4]. | Write a short rule set and apply it to everything. Proposed rules: chunky, a little wonky, worn but cheerful, lit by a warm lantern. The ship can be cartoon. The *motion* of the sea and ship should be physically convincing (spring dynamics, buoyancy), because that is what makes it feel premium. |
| **Return to Monkey Island** (Rex Crowle) | Picture-book look. Crowle drew on MI1's palettes, MI2's painterly approach and Curse's stylised shapes, and said the style should serve the game's themes [5][6]. Ron Gilbert defended it as the style he wanted [7]. | Bold, flat-ish shapes with angular silhouettes read well at plugin size. Humour comes from shapes and animation, not from text. |
| **Monkey Island 1-3** | Curse (MI3) used hand-painted backgrounds plus hand-drawn, fully anti-aliased animation from a digital ink-and-paint pipeline [8][9]. The pirates are goofy, never grim. | Tone: silly-swashbuckling, never "authentic historical". Skulls only as a wink. |
| **The Wind Waker** | Cel/toon shading with real-time lighting [10]. The water has no reflections: a flat-shaded surface with white foam lines around obstacles [11]. The boat (King of Red Lions) is a *character* that talks [12]. | **The technical model for the OpenGL sea:** flat toon water, 2-3 colour bands, white foam lines, no PBR. The ship should be a character with a face and reactions. Toon water is cheap to render and easy to read. |
| **Townscaper** (Oskar Stalberg) | No building menu. Click to add, right-click to remove; an algorithm makes everything fit [13]. Stalberg calls it a toy and "radically casual" [13][14]. | **This is the interaction model for the tracks.** Drop a sound, and the app decides the lane, name, colour, gain and trim. The user never configures a track, just like Townscaper never asks you to model a house. |
| **Dredge** (Black Salt) | Low-poly with hard edges; the style started as fast prototype art and stayed stylised [15][16]. One small boat on a big moody sea carries the whole game. | A **single small boat** as hero, low-poly with hard edges, is enough for a striking look and quick to build in raw GL. |
| **Overboard!** (inkle) | Restricted, balanced palette taken from 1930s travel posters; hand-drawn linework; dark comedy [17][18]. | A poster-style palette of 5-7 colours and no gradients beyond simple bands looks designed, not cheap. |
| **Sid Meier's Pirates!** | Deliberately cartoony and exaggerated [19]. Some players found it too cartoonish [19]. | The line between charming and childish: exaggerate shapes and motion, keep the palette and type grown-up. |
| **AC IV Black Flag** | The ship (Jackdaw) is a central character you customise [20]. The 2026 remake's minimal HUD keeps only essentials such as the cannon aiming arcs [21]. | Show the "aim" (flight path) and almost nothing else. Customising the ship (sail colour, flag) is a cheap, fun reward. |
| **One Piece** | The Going Merry / Thousand Sunny are ships with cute animal figureheads; the Jolly Roger is a crew's personal mark. (General knowledge, no source fetched.) | Give the ship a figurehead based on the Sweet Justice **cherry and grenade**. The flag is the SJ logo. |
| **Sea of Stars** | Rich palette and time-of-day lighting change the mood [22]. | Optional: the sky and lighting shift slowly with the session, or with the "energy" of the current whoosh. |
| **Cult of the Lamb** | 2D art inside a 3D scene (2.5D); cute plus dark [23][24]. | A good fallback if full 3D is too much: flat billboards (sails, flag, splashes, gulls) inside a simple 3D sea. |
| **Pirates Outlaws** | Card roguelike: chunky outlines, flat colour, readable icons. (No source fetched.) | Thick dark outlines (2-3 px) on every UI element help readability on a busy animated background. |

### Audio products with playful art

| Product | Lesson |
|---|---|
| **Teenage Engineering OP-1** | Small animations per engine that *show the sound* [25]. Graphics work as feedback [26]. Constraint: one screen and four encoders [27]. The device was called a toy, and that was meant as praise [28]. |
| **Baby Audio** | Effects designed to behave like instruments [29]; day/night skins (Comeback Kid) [30]; reviewers single out the sparse design [31]. Caspar Bock says buyers increasingly want everything fast, and many have never used a physical studio [32]. |
| **Native Instruments Play Series** | Two layers, six macros, always visible; everything else sits behind a view [33][34]. |
| **Minimal Audio Current 2** | A "Play View" with a dual XY pad and 4 macros; you click an engine name to go into detail [35][36]. Friendly, consistent house style [37]. |
| **Sonic Charge Synplant 2** | A seed with 12 branches you grab and replant: a toy metaphor for exploring sounds. Genopatch makes a patch *from audio you drop in* [38][39]. The closest precedent for "scan the sound and act on it". |
| **Sinevibes** | Colour-coded, animated transitions, live waveforms aligned with the controls, hardware-accelerated rendering [40][41]. |
| **Klevgrand** | Walls reverb was designed to be "playful and imaginative", surreal as much as real, and still intuitive from the start [42]. |
| **Ableton Note** | Live's Session View slimmed down for quick idea capture; reviewers call it easy to use [43][44]. |
| **Unfiltered Audio BYOME** | Called brilliantly intuitive by one reviewer [45], cramped by forum users once many modules are loaded [46]. Warning: modular freedom becomes clutter quickly. |
| **Output Arcade** | Onboarding redesign replaced a long unskippable video with a guided flow [47]. Teach inside the UI, not with a video. |
| **Arturia, u-he Zebra/Hive themes, Puremagnetik** | Not researched in depth for this brief. Known pattern: skinnable themes. A dark and a light skin (like Baby Audio's day/night) cost little and please users. |

---

## 2. Cheap vs delightful pirate theming

| Feels cheap or kitsch | Feels delightful |
|---|---|
| "Pirate" fonts (distressed, shivery, blackletter) on labels. Typographic cliché is the reflex choice for a theme [48], and "shivery" effects date fast [49]. | Plain, friendly type. The pirate flavour lives in the **art and motion**. |
| Parchment and wood *textures* behind controls (they lower contrast and look like clip art). | Flat colour fields in a sea palette; texture only on the ship art. |
| Skulls everywhere [50], "Arr!", "Ahoy matey" in every label. | One joke at a time, in toasts and empty states ("Hold's empty. Drop some cargo!"). Labels stay plain English. |
| Brass, ropes and rivets on every panel (this read as "posh" in v1). | One hero object rich in detail (the ship); the rest of the UI is simple shapes. |
| Static decoration. | Everything that moves *means* something: the ship rides the whoosh, sails fill with level, cannons fire on transients. |
| Themed nouns for core functions ("Crew", "Rigging", "Powder Keg"). They made v1 hard to learn. | Plain names for controls (Length, Bite, Space). Pirate names only for objects the user doesn't need to understand (preset names, the dice, the ship). |
| Historical grit (dark browns, sepia). | Holiday pirate: bright lagoon teal, cherry red, warm gold, night-sea navy. Restricted like a travel poster [17]. |

**Rule of thumb:** if you remove the pirate art, the app must still be obvious to use. If you remove the plain
labels, it must still look like a pirate toy. These two layers stay separate.

---

## 3. Game feel / juice

Main sources: Jonasson and Purho's "Juice It or Lose It" (GDC Europe 2012), which turns a dull breakout game
into a joyful one with tweening, colour, particles, sound and shake [51][52]. Jan Willem Nijman's "The Art of
Screenshake" (2013) applies about 30 small tweaks to a dull shooter, including shake, muzzle flash and camera kick [53][54].
A counterpoint argues heavy juice can cost immersion [52]. For a tool, juice must never hide information.

UI animation guidance: a press squashes about 5% (scaleY 0.95); feedback within 50-100 ms; transitions 100-200 ms;
toggles overshoot then settle [55]. Springs beat bezier curves for squash and stretch; one example uses stiffness 300 and damping 12 [56].
Respect reduced motion [56]. Too much squash and stretch looks juvenile [55][57].

The Swooshbuckler juice menu (each item is small; together they make the app feel alive):

| Event | Juice |
|---|---|
| Drop a file onto the window | The ship's hold glows; a crate drops on deck with a squash (y 0.8, then spring back to 1.0, stiffness ~300, damping ~14). A wooden *thunk* plays in the UI (plugin: optional, off by default in DAWs). |
| Sound scanned and classified | A small spyglass sweeps over the new lane for ~600 ms, then the name **types itself in** ("Roar", "Air", "Crackle") with a 1-frame colour flash in the role colour. The lane grows from 0 to full height with an overshoot. |
| Press SWASH | Button squashes to 0.92 for 60 ms, pops back past 1.0. The ship lurches along the flight path; the camera dollies with it; sails snap full; foam spray particles at the bow. The camera shakes on the peak, scaled by the whoosh's peak level and capped at a few pixels. |
| Peak / impact in the whoosh | Cannon puff (2D billboard smoke), 2-3 px camera kick, a brief bloom on the gold highlights. |
| Dice roll | The die tumbles with real physics (3 bounces), lands, and the result *pushes* the parameter knobs, which spring to their new values (the rubber-band parameters already in the brief). |
| Morph XY | The ship's wake changes colour between the two sources' role colours. |
| Tape recorder | A little lantern on the stern glows while recording; when a take is saved, a bottle bobs away with a "Saved" toast. |
| Idle | The ship bobs gently, gulls cross now and then, the flag flutters. Idle motion is low amplitude and slow, so it doesn't pull focus while you work. |

Rules: every animated value uses springs (the app already has spring-animated knobs). No animation is longer than
~400 ms for UI chrome. Shake and particles scale with **audio**, so the visuals tell the truth about the sound.
Offer one "Calm seas" toggle (reduced motion) in settings.

---

## 4. Typography

The HTML mockup used Lilita One and the user rejected the whole visual. Typography wasn't the main complaint, but
to break that association, v3 should use a different hero face. All the fonts below are free for embedding in a
commercial plugin (OFL 1.1 or Apache 2.0). Embed the files via BinaryData and ship the licence text with the installer.

### Recommended stack

| Role | Font | Licence | Why | Download |
|---|---|---|---|---|
| **UI and headings** (track names, buttons, knob captions) | **Fredoka** (variable, weights 300-700; use 500-600) | SIL OFL 1.1, (c) Milena Brandao [58][59] | Rounded, chunky, friendly, very readable at 11-14 px. Feels like a toy, not a parchment. Lower case reads less shouty than v1's all caps. | fonts.google.com/specimen/Fredoka · github.com/google/fonts/tree/main/ofl/fredoka |
| **Body, hints, numbers** | **Atkinson Hyperlegible Next** (7 weights, variable) | SIL OFL 1.1, (c) Braille Institute [60][61] | Designed for legibility; distinct glyphs (1/l/I, 0/O) help with dB and ms values. It has a matching **Mono** for tabular readouts. | fonts.google.com/specimen/Atkinson+Hyperlegible+Next · brailleinstitute.org/freefont |
| **Big moments only** (the wordmark, "SWASH!", toasts like "Roar spotted!") | **Luckiest Guy** | Apache 2.0, Astigmatic [62][63] | Comic, chunky, adventure-cartoon caps. Use only above ~24 px and never for controls. | fonts.google.com/specimen/Luckiest+Guy |

Alternatives, also on Google Fonts (licences are my own knowledge; check each `OFL.txt` before shipping):
- **Titan One** (OFL, Rodrigo Fuenzalida): a heavier, rounder alternative to Luckiest Guy for the wordmark.
- **Baloo 2** (OFL, Ek Type): an alternative to Fredoka with more weights and a slightly more grown-up look [64].
- **Nunito** (OFL) [65]: a softer rounded body face if Atkinson feels too plain.
- **Bangers** (OFL, Vernon Adams): comic-book caps, more "action" than "adventure".
- **Pirata One** (OFL 1.1, Fuenzalida/Massi, 2012) [66][67]: a blackletter that works on screen. **Wordmark only, if at all.** Blackletter
  reads gothic/posh, which is the v1 failure mode. Not recommended.

### Rules
- Sentence case everywhere except the wordmark and the SWASH button.
- No letter-spacing added (v1's tracking made it look posh).
- Minimum 11 px for any text, 13 px for anything you click.
- Text colour is sail cream (see palette), not pure white. Every label sits on a solid panel, never directly
  on the animated sea.

---

## 5. Colour palette (Sweet Justice at sea)

Built from the existing Sweet Justice tokens in `subbertone/Source/Theme.h` (navy bg `#0D0F14`, teal `#5EC8D8`,
rose `#EF6F8E`, amber `#FFB54D`) and the logo's cherry red (`#CF1D35`, from `Resources/sj-logo.svg`).
The navy is warmed slightly toward ocean blue so it reads "night sea", not "luxury app". Approach: a poster-like
restricted palette [17], with flat colour bands for toon shading.

### Core (UI chrome)
| Token | Hex | Use |
|---|---|---|
| `abyss` | `#0B1424` | Window background, deep water band 3 |
| `deep` | `#12223A` | Panels, lane background |
| `hull` | `#1C3354` | Raised panels, buttons |
| `rope` | `#2C4A70` | Outlines, grid, inactive knob track |
| `sail` | `#F4EBDD` | Primary text, sails, foam |
| `mist` | `#A9B8CC` | Secondary text |
| `fog` | `#6A7C96` | Hints, disabled |
| `outline` | `#081020` | 2-3 px "cartoon ink" outline around buttons, crates and the ship |

### Accents (meaning)
| Token | Hex | Meaning |
|---|---|---|
| `cherry` | `#CF1D35` (logo) / hover `#E8324C` | **SWASH**, record, the flag. The only big red thing on screen. |
| `doubloon` | `#FFB54D` (brand amber) / glint `#FFD98A` | Active state, focus, selected lane, lantern, gold highlights on the ship |
| `lagoon` | `#5EC8D8` (brand teal) / shallows `#9BE8E0` | Water band 1, flight path, links |
| `rose` | `#EF6F8E` (brand rose) | Morph, secondary actions, the cherry figurehead's highlight |

### Sea toon bands (OpenGL)
Water 3 bands: `#9BE8E0` crest, `#3FA9C0` mid, `#1A5F86` trough, fading to `abyss` at the horizon. Foam lines are `sail`.
Sky: dusk gradient `#12223A` to `#3B3F72` to a rose rim `#EF6F8E` at the horizon (it makes the brand rose feel natural).
Ship hull wood: `#7A4B2E` base, `#5A3420` shadow band, `#A8693F` lit band, with `doubloon` trim.

### Material (track) colours
One colour per role from `SampleClassifier` (Bed, Tonal, Impact, Air, Crackle, Rumble). The lane takes the colour
of what it *is*, so the colour itself tells you the material:
| Role | Name shown | Hex |
|---|---|---|
| Air | "Air" / "Swish" | `#9BE8E0` (shallows teal) |
| Tonal | "Roar" / "Squeal" / "Voice" (from the name prior) | `#EF6F8E` (rose) |
| Impact | "Hit" | `#FF7A45` (cannon orange) |
| Crackle | "Crackle" / "Fire" | `#FFB54D` (amber) |
| Rumble | "Rumble" / "Sub" | `#8C7CF0` (storm violet) |
| Bed | "Bed" / "Wind" | `#7FB7A0` (kelp green) |

Check contrast: `sail` on `deep` is about 13:1; `doubloon` on `abyss` is about 10:1. Run the numbers again if the hex values change.

---

## 6. Illustration and 3D style

**Pick: low-poly toon-shaded, chunky, with ink outlines.** It sits between Wind Waker (toon bands, flat
water, foam lines) and Dredge (hard-edged low poly). Billboards carry the 2D charm, as in Cult of the Lamb.

Why this style:
- It renders cheaply in raw OpenGL (the editor already attaches an `OpenGLContext`; see `SeaVisualiser.h`,
  `PluginEditor.h`). Toon shading is one `floor(dot(N,L) * bands)` step plus a rim. There are no textures to
  license or paint. Wind Waker-style water needs no reflections [11]. Public tutorials cover toon water with
  depth-based foam [68][69][70].
- Low poly avoids the "posh" brass realism of v1 and the flatness of the HTML mockup.
- Hard edges and outlines read well at plugin window sizes and on any monitor.

**The hero: "the Swooshbuckler"** (a small, stubby, slightly wonky sloop, about 300-600 tris)
- A cherry figurehead. The flag is the SJ grenade-and-cherry mark in `sail` on `cherry`.
- Lantern on the stern = tape recorder state. Sails = output level (they billow with RMS).
- The ship flies the *flight path*: the drawn "Throw it" curve becomes the ship's course across the sea, seen from
  a 3/4 camera. Doppler is visible as the ship passes the camera (listener = a little buoy, or the camera itself).
- Each track (cargo) is a crate on deck, coloured by its role. Drop a sound and a crate lands. Mute a lane and
  the crate is tied down in rope (grey).
- The two morph sources are two sails, or two flags, in their role colours.

**Shape language:** chunky silhouettes, rounded tops, nothing perfectly straight (the Sea of Thieves "wonky but
believable" idea [3]); the scale is toy-like (big head, small body).

**Fallback if 3D is too much work for now:** 2.5D. A toon-shaded GL sea plane with the ship, gulls, splashes and
crates as outlined 2D sprites (vector, drawn with `juce::Path` into textures). It keeps the hero idea without a modelling pipeline.

**Performance:** cap the frame rate at 60 fps; drop to 15 fps when idle or the window is hidden; one draw call per
material. GL on macOS is deprecated but still works through JUCE. The JUCE forum notes CoreGraphics can be faster for 2D,
so keep the 2D chrome in `juce::Graphics` and use GL only for the sea and ship [71].

---

## 7. Layout that keeps it simple

Layout principles from the research, as text (not a mockup):
1. **The sea is the window.** The 3D scene fills the top ~60%. You draw the flight path on the sea; the dice
   throws one. The ship sails it when you press SWASH. One canvas instead of v2's separate flight lane and flight pad.
2. **The hold (tracks) appears only when cargo exists.** The empty state is a big friendly drop target on the sea:
   "Drop sounds here, any sounds". Each drop adds a lane, Townscaper-style: no add-track button, no type menus,
   no "Input" lane unless audio is actually arriving at the plugin input (then a lane called "Live input"
   appears, as the OP-1 shows controls only when they matter [27]; see `01-simplicity-ux.md`).
3. **A lane is a role colour + an auto name + a waveform + trim handles + mute.** Nothing else on the lane; a right-click
   offers "Rename", "Change type", "Remove".
4. **One row of 4-6 big macro knobs** under the sea (like NI Play's six macros [33] or Current's four [35]),
   then a **More** drawer. Labels in plain English.
5. **SWASH is the only cherry-red thing** and the biggest button.

---

## 8. Style guide summary (v3)

1. **Hero:** one low-poly, toon-shaded pirate sloop on a Wind Waker-style toon sea, rendered in OpenGL. It flies the
   flight path and reacts to the audio (sails = level, cannon puff = peaks, lantern = recording).
2. **Rules on every visual:** chunky, a bit wonky, ink-outlined, cheerful, lit by warm gold.
3. **Palette:** night-sea navy (`#0B1424` / `#12223A` / `#1C3354`), sail cream text (`#F4EBDD`), cherry SWASH
   (`#CF1D35`), doubloon gold active state (`#FFB54D`), lagoon teal water and path (`#5EC8D8`), rose accent
   (`#EF6F8E`). Material colours per role (Air teal, Tonal rose, Impact orange, Crackle amber, Rumble violet, Bed kelp).
4. **Type:** Fredoka (UI), Atkinson Hyperlegible Next (+ Mono for numbers), Luckiest Guy (wordmark and big moments only).
   Sentence case, no tracking, 11 px minimum.
5. **Tracks:** created by dropping, named and coloured by what the app hears, shown as crates on deck and as lanes
   in the hold. Never pre-made, never empty.
6. **Juice:** springs everywhere, squash on press, audio-scaled shake and particles, UI sounds optional (off in a DAW by
   default), and a "Calm seas" reduced-motion toggle.
7. **Pirate in the art, plain in the words:** plain labels for controls; pirate jokes only in toasts, empty states,
   preset names and the dice.
8. **Forbidden:** parchment textures, distressed or blackletter fonts on controls, brass and rivets, skull spam,
   all-caps tracking, more than one red element.

---

## Sources

1. Sea of Thieves art goal "timeless" (Stevenson) - https://gamedeveloper.com/art/video-how-rare-crafted-the-look-and-feel-of-i-sea-of-thieves-i-
2. GDC18 "Visual Adventures on Sea of Thieves", 80.lv summary - https://80.lv/articles/gdc18-visual-adventures-on-sea-of-thieves/ ; GDC Vault - https://gdcvault.com/play/1025015/Visual-Adventures-on-Sea-of
3. Stylised graphics analysis, Fortnite vs Sea of Thieves - https://blog.habrador.com/2018/08/stylized-graphics-fortnite-sea-of-thieves.html
4. Sea of Thieves water dev diary - https://mcvuk.com/development-news/rares-sea-of-thieves-started-with-photorealistic-water/
5. Rex Crowle on RtMI influences (GamesBeat) - https://gamesbeat.com/return-to-monkey-islands-art-director-talks-inspirations-and-expectations
6. Rex Crowle interview - https://legendofmi.com/news/marius-interviews-rex-crowle-762
7. Ron Gilbert defends the art style - https://www.videogameschronicle.com/news/ron-gilbert-sad-about-negative-reaction-to-return-to-monkey-islands-art-style
8. Making of Curse of Monkey Island - https://timeextension.com/features/the-making-of-the-curse-of-monkey-island-scumms-underrated-swansong
9. Bill Tiller talk transcript - https://scummbar.com/resources/articles/BillTillerInterview/tillerconferance.html
10. Wind Waker making-of - https://gamesradar.com/the-making-of-the-legend-of-zelda-the-wind-waker
11. Wind Waker water: flat-shaded, foam lines (Godot shader notes) - https://godotshaders.com/shader/wind-waker-water-no-textures-needed/
12. King of Red Lions - https://zeldadungeon.net/wiki/King_of_Red_Lions
13. Townscaper - https://en.wikipedia.org/wiki/Townscaper ; https://reason.com/2021/03/19/townscraper/
14. "Radically casual" design (PC Gamer) - https://pcgamer.com/townscapers-developer-on-how-its-radically-casual-design-is-inspiring-a-new-wave-of-low-stress-builders-to-adapt-the-blueprint
15. Dredge art, low-poly hard edges (AusGamers) - https://www.ausgamers.com/features/read/3646015
16. Dredge art born out of prototyping - https://www.gamedeveloper.com/design/creating-the-creepy-salty-world-of-dredge
17. Overboard! art: travel posters, restricted palette - https://www.pocketgamer.biz/feature/76884/how-inkle-studios-created-a-hit-in-100-days/
18. Overboard! review - https://toucharcade.com/2021/06/04/toucharcade-game-of-the-week-overboard/
19. Sid Meier's Pirates! 2004 cartoony style - https://retroreview-blog.blogspot.com/2025/09/retro-review-sid-meiers-pirates-2004-pc.html
20. Black Flag Resynced naval deep dive - https://news.ubisoft.com/en-us/article/2ld4JIABg5aOvoiUlJlUWF/assassins-creed-black-flag-resynced-deep-dive-into-the-naval-gameplay
21. Black Flag Resynced HUD customisation - https://news.ubisoft.com/en-us/article/1QLCE5mmONckIxhB8xJw6d/assassins-creed-black-flag-resynced-deep-dive-into-hud-customization
22. Sea of Stars review - https://gamesradar.com/sea-of-stars-review
23. Cult of the Lamb art direction (Unity) - https://unity.com/en/blog/games/recipe-behind-smash-hit-cult-of-the-lamb
24. Cult of the Lamb art director interview - https://www.dreadxp.com/?p=27832
25. OP-1 playful animations - https://desirabilitylab.com/backfill/backfill-2022-104-teenage-engineering-op-1-field
26. OP-1 graphics as feedback - https://equipboard.com/posts/the-teenage-engineering-op-1-a-modern-classic
27. Teenage Engineering constraints essay - https://blakecrosley.com/guides/design/teenage-engineering
28. Inside design at Teenage Engineering - https://read.cv/jdsimcoe/inside-design-at-teenage-engineering
29. Baby Audio Q&A, effects like instruments - https://www.pressreader.com/australia/computer-music/20200617/282961042368235
30. Comeback Kid day/night - https://gear4music.ie/Recording-and-Computers/Baby-Audio-Comeback-Kid/46NA
31. Crystalline review (SOS) - https://www.soundonsound.com/node/4929079
32. Future of plugin design (MusicRadar) - https://www.musicradar.com/music-tech/plugins/the-biggest-driver-right-now-is-people-wanting-everything-fast-predicting-the-future-evolution-of-plugin-design
33. Analog Dreams manual, main overview - https://www.native-instruments.com/ni-tech-manuals/analog-dreams-manual/en/main-overview
34. Analog Dreams macro editor - https://www.native-instruments.com/ni-tech-manuals/analog-dreams-manual/en/macro-editor
35. Current 2.0 Play View (SOS) - https://www.soundonsound.com/news/minimal-audio-current-20-now-available
36. Current 2.0 (Plugin Boutique) - https://www.pluginboutique.com/products/11770-Current-2-0
37. Current review (CDM) - https://cdm.link/minimal-audio-current-synth/
38. Synplant 2 review (SOS) - https://www.soundonsound.com/reviews/sonic-charge-synplant-2?amp
39. Synplant 2 hands-on (CDM) - https://cdm.link/synplant-2-hands-on/
40. Sinevibes Flow II - https://rekkerd.org/sinevibes-releases-flow-ii-animated-flangerphaser-plugin-for-mac/
41. Sinevibes Emission - https://rekkerd.org/emission-artificial-reverb-plugin-for-mac-by-sinevibes/
42. Inside Klevgrand (ADAM Audio) - https://www.adam-audio.com/blog/inside-klevgrand/
43. Ableton Note (SOS) - https://www.soundonsound.com/reviews/ableton-note
44. Ableton Note first look (MusicRadar) - https://www.musicradar.com/news/ableton-note-app-iphone-ableton-live
45. BYOME review (MusicRadar) - https://musicradar.com/reviews/unfiltered-audio-byome
46. BYOME forum discussion (KVR) - https://www.kvraudio.com/forum/viewtopic.php?p=7201607
47. Output Arcade guided onboarding case study - https://p2026-cez.pages.dev/work/guided-onboarding
48. Uxcel: avoid typographic cliches - https://app.uxcel.com/lessons/selecting-typefaces-611/avoid-clichs-3961
49. Pirate fonts dating - https://advertising-creative-bra-d1v.pages.dev/evaluating-the-pirate-shivers-font-for-design-projects/ (low-authority)
50. Pirate cliches to avoid - https://sites.google.com/site/realpirates000/home/cliches-to-avoid
51. Juice It or Lose It (GDC Vault) - https://gdcvault.com/play/1016487/contactUs
52. Juice summary and critique - https://roblog.co.uk/2024/03/juicy-games/ ; https://gamedeveloper.com/design/video-indies-resist-the-urge-to-juice-it-or-lose-it-
53. Vlambeer on action game feel - https://gamedeveloper.com/design/vlambeer-co-founder-shares-advice-on-building-better-action-games
54. Kenney's must-see videos (incl. Art of Screenshake) - https://kenney.nl/learn/must-see-videos-for-indie-developers
55. Micro-interaction timing (0.95 press, 50-100 ms) - https://www.skills.lc/dylantarre/animation-principles/dylantarre-animation-principles-skills-01-by-domain-micro-interactions-skill-md
56. Josh W. Comeau, squash and stretch with springs - https://joshwcomeau.com/animation/squash-and-stretch
57. Disney principles in UI (Dribbble) - https://dribbble.com/stories/2020/07/27/disney-principles-of-animation-ui-interactions
58. Fredoka licence (Font Squirrel) - https://www.fontsquirrel.com/fonts/fredoka
59. Fredoka variable (Fontsource) - https://www.jsdelivr.com/package/npm/@fontsource-variable/fredoka
60. Atkinson Hyperlegible (googlefonts) - https://github.com/googlefonts/atkinson-hyperlegible
61. Atkinson Hyperlegible Next licence and editions - https://www.linuxlinks.com/atkinson-hyperlegible-next-sans-serif-typeface/
62. Luckiest Guy METADATA (APACHE2) - https://raw.githubusercontent.com/google/fonts/2796410152d4f9524b68ed46e69c1b60f8e0f7c3/apache/luckiestguy/METADATA.pb
63. Luckiest Guy (Font Squirrel) - https://www.fontsquirrel.com/fonts/luckiest-guy
64. Baloo 2 listing - https://pub.dev/documentation/google_fonts/3.0.1/google_fonts/GoogleFonts/asMap.html
65. Nunito OFL (Arch package) - https://www.archlinux.org/packages/extra/any/ttf-nunito
66. Pirata One (Typewolf) - https://www.typewolf.com/pirata-one
67. Pirata One OFL (Fontsource) - https://fontsource.org/fonts/pirata-one/cdn
68. Roystan Toon Water Shader - https://openupm.com/packages/net.roystan.toonwatershader/
69. Creating toon water for the web (Tuts+) - https://code.tutsplus.com/creating-toon-water-for-the-web-part-1--cms-30447t
70. Toon water, no textures (Godot) - https://godotshaders.com/shader/toon-style-3d-water-shader-no-textures-needed/
71. JUCE forum: OpenGL deprecated in Mojave - https://forum.juce.com/t/opengl-deprecated-in-mojave/27995 ; OpenGL roadmap - https://forum.juce.com/t/opengl-roadmap/31173
