# Tempo Tantrum changes

## 0.1.0

The first test build. It is published as a pre-release and is not in the Sammy J's Plugins installer.

It opens a piece of music, finds the hits, maps the beat section by section and puts a bars and beats grid on the hits, with the places it was unsure of listed to check. The map can be corrected by moving, adding and deleting pins, starting a section where the beat restarts and saying which bar a hit is. It plays the music with a click, saves the map beside the music and exports the click as a WAV file.

Write to Pro Tools puts the map into the open session by running Pro Tools' Event > Identify Beat once for each pin, at about a second a pin. Through Pro Tools' scripting link it reads the session and the selected music clip (its file, where it starts, and where in its file when the clip list says), warns when the clip's file or the session's sample rate is not the music's, stops the transport, sets the Main Counter to Samples, moves the cursor to each pin, checks every pin afterwards without opening a dialog, and reads the tempo of the rest of the session before and after to say whether it changed. The Main Counter and the edit settings are put back at the end of the run, also after Stop; an Identify Beat dialog left open by a stop is cancelled first. Identify Beat itself is driven with key presses through the macOS Accessibility permission. Without the scripting link every step is done with keys. It reads back the cursor position and the bar and beat before each OK, stops at the first thing that is not as expected, and can send one Cmd+Z for each pin it wrote. The log of each run can be copied or saved.

Get from Pro Tools (Cmd+Shift+G) takes the edit selection in Pro Tools as the music: Pro Tools renders it offline through the scripting link (Pro Tools 2025.06 and later), as it sounds on the selected track or tracks, with nothing imported and nothing in the session changed. The music then starts where the selection starts, so Write to Pro Tools writes from there with no clip to match; music opened from a file is matched to its clip as before. When the offline render cannot be used, Tempo Tantrum has Pro Tools render the selection with Tempo Tantrum Capture in AudioSuite through the Accessibility permission, after asking once, and undoes the render afterwards. Every run is logged.

Tempo Tantrum Capture is an AAX AudioSuite plug-in that sends the audio it renders to Tempo Tantrum without changing it. It can be used by hand from the AudioSuite menu; Cmd+Z in Pro Tools takes the rendered clip out.

Test in Pro Tools…, under More in the footer, runs the same steps in three stages (look only, one pin, all pins) and writes down everything it sees.
