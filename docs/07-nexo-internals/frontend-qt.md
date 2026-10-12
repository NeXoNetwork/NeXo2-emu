# Qt interface (NeXo2-Qt.exe)

NeXo has two programs that share `nexo2.ini`, `sdmc/`, `games/` and `keys/`:

| Program | Interface | For |
| :--- | :--- | :--- |
| `NeXo2-Qt.exe` | Qt 6 Widgets | playing: library with covers, game details, settings, full screen |
| `NeXo2.exe` | SDL3 + Dear ImGui | development: CPU, threads, memory and GPU windows, logs |

`NeXo2-Qt.exe` is only built when CMake finds Qt 6. Without Qt, everything else builds as before.

## Qt (downloaded by build.bat)

`build.bat` looks for Qt 6 in this order:

1. `NEXO2_QT_DIR` (the folder that contains `bin\windeployqt.exe`);
2. `externals\qt\6.8.3\msvc2022_64`;
3. `C:\Qt\6.*\msvc2022_64`.

If none exists, it downloads Qt 6.8.3 into `externals\qt` with aqtinstall. This needs Python, takes a few minutes and happens only the first time.
`NEXO2_NO_QT=1` skips the download, and then only `NeXo2.exe` is built.

`externals/qt/` is in `.gitignore`. Qt's binaries take several hundred MB, more than a git repository should carry, and every machine can download them in one step.
After the build, `windeployqt` copies the Qt DLLs next to `NeXo2-Qt.exe`.

By hand:

```
py -m pip install aqtinstall
py -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O externals\qt
```

## Code map (`src/qt/`)

| File | Role |
| :--- | :--- |
| `emu_host.*` | The emulated console for the window. It owns the `System`, the emulation thread (same scheme as `NeXo2.exe`: batches of instructions, real-time pacing, multicore start/stop), SDL audio, the SDL gamepad and the settings. No Qt in it. |
| `main_window.*` | Sidebar (Library, Now playing, Settings, About) and the pages. A precise ~120 Hz timer reads the keyboard and gamepad, passes the buttons to the game and picks up new frames. Drag and drop opens a game. |
| `library_page.*` | Grid of cards (`QListView` in icon mode + `CardDelegate`), search (`QSortFilterProxyModel`), and the details panel with the cover, the data and the Play button. |
| `game_view.*` | The console image (kept in proportion), a top bar that hides after 2.5 s without mouse movement, the performance pill (F3), the pause overlay and the "starting" spinner. |
| `settings_page.*` | Settings cards. Each change is saved at once. Graphics and CPU changes apply to the next game; volume and sound apply immediately. |
| `widgets.*` | Animated toggle switch, sidebar button, round icon button, logo and toast notifications. |
| `theme.*` | Colours, the style sheet, the dark title bar on Windows (DWM), and the line icons drawn with `QPainter`. There are no image or icon-font files, so icons stay sharp at any scale. |

## Look

- Dark background (`#0B0E14`), surfaces in steps of blue-grey, and one brand gradient (violet `#7C5CFF` to cyan `#00C8FF`).
  The gradient is used for the primary buttons, the selected item, the switches and the logo.
- Cards lift and show a play button on hover. A selected card gets a gradient border.
  A game without an icon gets a cover made from its initials, in a colour taken from its name.
- The details panel shows the cover over a blurred copy of itself.

## Keys

| Key | Action |
| :--- | :--- |
| Ctrl+O | open a file |
| Ctrl+F | search the library |
| Ctrl+, | settings |
| F5 | pause / continue |
| F11, double click | full screen |
| Esc | leave full screen |
| F3 | performance pill |

The keyboard maps to controller 1 as in `NeXo2.exe` (see [input.md](input.md)).

## Screenshots without a screen

`NEXO2_QT_SHOT=file.png:page` grabs the window after ~2 s and quits.
Pages: `detalles` (first game selected), `ajustes`, `acerca`, `juego`, `pausa` (the last two use a test image).
With `QT_QPA_PLATFORM=offscreen` it works without a display.

## Not yet

- The debugging windows are still only in `NeXo2.exe`.
- An ImGui overlay inside the game view.
- Key remapping, per-game settings, recently played games.
