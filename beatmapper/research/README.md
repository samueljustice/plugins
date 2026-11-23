# Beat mapper research scripts

Throwaway Python from the design research for a Pro Tools beat mapping tool. Nothing here is the app. The scripts were written quickly, expect paths to be passed in or edited at the top, and have no tests.

## ptx

Reads the tempo map out of a Pro Tools session file.

- `unxor.py` removes the session file's XOR obfuscation (type 5).
- `tempo.py` finds the tempo block and lists its events: sample position, tick position (960000 per quarter note) and BPM. The BPM on an event is the tempo from that event to the next. The last record is an end marker.
- `batch.py`, `analyze.py`, `stats.py` run that over many session versions, group events into separate maps and measure them (events per bar, which beats they sit on, tempo spread).
- `onsets.py`, `match.py` detect onsets in a music file and find where a map lines up with it.
- `ctx.py`, `hexat.py`, `findpat.py`, `findpat2.py` are small helpers used while working out the file layout.

## engine

A first pass at the automatic map: multi-band onset detection, a hinted beat tracker (first hit plus a whole-number tempo), a dynamic-programming snap of beats to onsets, and greedy pin selection.

- `engine.py` is the version that was scored. `engine_reset_variant.py` tried letting the tracker jump phase and did worse.
- `score.py` compares the result with a hand-made map. It imports `pairs.py`, which names the music file and timeline position for each map. That file names real projects, so it is kept outside the repository.
- `trackers.py` runs Beat This and librosa for the fully automatic mode. `diag.py` prints where the result and the hand-made map disagree.

## stats

- `a.py` measures how consistent the hand-made maps are with themselves, for example how far the same hit moved between session versions.

## Not in the repository

The session data, the written reports, the pairing files and the design sketch are in `~/Documents/BeatMapper-work`, because they name client projects and this repository is public.
