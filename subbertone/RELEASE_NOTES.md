# Subbertone 1.4.0

**LEARN, for engines and anything else the live tracker struggles with.** Loop a passage in your host, press LEARN and play the loop. Subbertone works out the pitch of the whole pass with hindsight, and from then on the sub follows what it learned in that passage: no lag, gear shifts as clean steps, short gaps bridged. Double-click LEARN for the curve view, where you can pick which line the sub follows, the method and the gap fill. What it learned is saved with your session. If the audio changes, that stretch goes back to live tracking.

**Deeper subs.** Two new bars left of H1 add clean sines two and three octaves below the input (S2 and S3), locked to the sub. Nothing is made below 20 Hz.

**Knobs.** Cutoff, Hi Cut, Lo Cut, Glide and both release knobs give more of their turn to the range that gets used, and Shift-drag is a true fine adjustment on every knob. Automation plays back exactly as before.

**New defaults** for new instances and the Init preset: Threshold -50 dB, Drive 30 %, Limiter Release 150 ms, Ceiling -1.0 dB. Existing sessions and every other preset sound as they did.

**Also**
- PATTERN's "Sub off" clears the deeper subs too.
- Tooltips rewritten, with four wrong claims corrected (Max Pitch, Inflate, Ceiling and Lo Cut).
- The AAX version is offered in mono and stereo, not multi-mono, so a stereo track gets one instance that tracks both sides together.

**Good to know**
- LEARN needs a host timeline; it is not available in the standalone app. In hosts that don't report their loop points it finishes on the second pass of the loop.
- LEARN is meant for close recordings (exhaust, engine bay, a voice, a bass). Pass-bys and distant takes stay on live tracking.
