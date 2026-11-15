# Subbertone 1.4.0

Subbertone is made mostly for cars and other engines, and this version is built around that.

**LEARN, for engines and anything else the live tracker struggles with.** Loop a passage in your host, press LEARN and play the loop. Subbertone works out the pitch of the whole pass with hindsight, and from then on the sub follows what it learned in that passage: no lag, gear shifts as clean steps, short gaps bridged. Double-click LEARN for the curve view, where you can pick which line the sub follows, the method and the gap fill. What it learned is saved with your session. If the audio changes, that stretch goes back to live tracking.

**Deeper subs.** Two new bars left of H1 add clean sines two and three octaves below the input (S2 and S3), locked to the sub. Nothing is made below 20 Hz.

**Knobs.** Cutoff, Hi Cut, Lo Cut, Glide and both release knobs give more of their turn to the range that gets used, and Shift-drag is a true fine adjustment on every knob. Automation plays back exactly as before.

**New defaults** for new instances and the Init preset: Threshold -50 dB, Drive 30 %, Limiter Release 150 ms, Ceiling -1.0 dB. Existing sessions and every other preset sound as they did.

**Also**
- PATTERN's "Sub off" clears the deeper subs too.
- Tooltips rewritten, with four wrong claims corrected (Max Pitch, Inflate, Ceiling and Lo Cut).
- The AAX version is offered in mono and stereo, not multi-mono, so a stereo track gets one instance that tracks both sides together.

**Known limitations**
- LEARN is meant for close recordings: exhaust, engine bay, a voice, a bass. In tests on about 860 engine recordings the learned line was clean on roughly two in three. Pass-bys, distant and in-car recordings stay on live tracking and are hit and miss.
- Motorcycles with fast gear changes and animal calls are weaker: about half or fewer give a clean line.
- On some four-cylinder and two-stroke recordings the learned line sits an octave higher than you want. Double-click LEARN and pick a lower line in the view.
- A wrong line doesn't always flag itself, so look at the line in the view after learning something new.
- LEARN needs a host timeline and a passage of 1 to 60 seconds. It is not available in the standalone app, AudioSuite or clip launchers. In hosts that don't report their loop points it finishes on the second pass of the loop.
- With the sub and both deeper subs at 100%, peaks can be about 9 dB higher than the sub alone.
- The full list is in the README under Known limitations.
