## SammyJs Reversinator 2.0.1

Reversinator's interface now has the same look as Subbertone's. Every menu, field, link, tooltip and focus state is drawn in Reversinator's own colours, and nothing is left in JUCE's default style. The sound, the parameters and saved sessions are unchanged from 2.0.0.

![Reversinator](https://raw.githubusercontent.com/samueljustice/plugins/reversinator-v2.0.1/reversinator/docs/screenshot.png)

### What's new

- **Menus.** The right-click menus of the value boxes and the licence text, and any submenus, are rounded, framed panels, with section headers, a rounded highlight, and drawn ticks and arrows.
- **Typing a value.** While you type into a value box, it becomes a rounded field in that control's colour, and the text stays centred.
- **Keyboard focus.** After you press Tab, a ring shows which control has the keyboard focus. This includes the mode lamps and the REVERSER button. A click hides the rings again.
- **About panel.**
  - The website and e-mail links are plain text that turns blue and underlines under the mouse.
  - CHECK FOR UPDATES is greyed out while a check runs.
  - The result is green when you are up to date and blue when an update is available.
- **Tooltips, scrollbar and resize grip** are drawn in the same style as Subbertone's.

### Fixes

- The licence text no longer shows "fi" as "fı" (a font ligature).
- The arrows beside REVERSING and FORWARDS in the display are now the same size.
- Tooltips no longer have unpainted corners at the default window size.
- While the About panel is open, Tab and Return no longer reach the controls behind it.
- On Windows, menus no longer have JUCE's bright grey frame.

### Downloads

| File | Contents |
|---|---|
| `SammyJs_Reversinator_Installer.pkg` | macOS installer (AU + VST3, universal) |
| `SammyJs_Reversinator_Installer.exe` | Windows installer (VST3) |
| `Reversinator-macOS-AU.zip`, `Reversinator-macOS-VST3.zip`, `Reversinator-Windows-VST3.zip` | The plugins on their own |

*Backwards Machine is a trademark of The Sound Guy, Inc. Reversinator is an independent recreation and is not affiliated with or endorsed by them.*
