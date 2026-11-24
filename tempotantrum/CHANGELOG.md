# Tempo Tantrum changes

## 0.1.0

The first test build. It is published as a pre-release and is not in the Sammy J's Plugins installer.

It opens a piece of music, finds the hits, maps the beat section by section and puts a bars and beats grid on the hits, with the places it was unsure of listed to check. The map can be corrected by moving, adding and deleting pins, starting a section where the beat restarts and saying which bar a hit is. It plays the music with a click, saves the map beside the music and exports the click as a WAV file.

Write to Pro Tools puts the map into the open session by running Pro Tools' Event > Identify Beat once for each pin, with key presses and through the macOS Accessibility permission, at about a second a pin. It reads back the cursor position and the bar and beat before each OK, stops at the first thing that is not as expected, can check every pin afterwards without changing anything, and can send one Cmd+Z for each pin it wrote. The log of each run can be copied or saved.

Test in Pro Tools…, under More in the footer, runs the same steps in three stages (look only, one pin, all pins) and writes down everything it sees.
