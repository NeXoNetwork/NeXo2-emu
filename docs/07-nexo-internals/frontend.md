# Launcher (frontend)

`src/main.cpp` and `src/frontend/`. The application uses SDL3 and Dear ImGui, laid out like a modern emulator.

## Screens

- **Menu bar:**
  - Archivo: open a `.nro`, add a game folder, reload, open the SD/NeXo folders.
  - Emulación: pause, stop, restart.
  - Opciones: settings, controls.
  - Ver: list or grid, icon size, developer mode.
  - Ayuda: links, about.
- **Library:** fills the window when no game is running.
  - It scans `sdmc/switch` plus the folders added in Settings, up to 3 levels deep.
  - It can be shown as a sortable table (icon, name, author, version, size, file) or as a grid of icons.
  - There is search, a right-click menu, and double click or Enter to play.
- **Game:** the console image fills the central area at 16:9.
  - When the program stops (it exits or fails), a box offers "Volver a la biblioteca" or "Reiniciar".
- **Status bar:** state (playing / paused / stopped / ready), FPS, MIPS, CPU mode, GPU, controller and version.
- **Settings:** a modal with tabs.
  - General: game folders and developer mode.
  - CPU: JIT, multicore, decode cache.
  - Gráficos: GPU thread, draw threads, filtering.
  - Audio: on/off and volume.
  - Controles: the key map.
  - Saved in `nexo2.ini` next to the executable (`frontend/settings.*`).
- **Developer mode:** the old debug windows ("NeXo 2 Diagnostics" and "Programa") float over everything.

## Game metadata

`frontend/game_list.*` reads only the NRO header and its `ASET` section, without loading the program:

- From the NACP: name and author (Spanish first, then English) and the display version at `0x3060`.
- The JPEG icon, decoded with `stb_image` (`externals/stb`, public domain).

Icons become SDL textures the first time they are drawn.

## Shortcuts and environment

- F5 pauses or resumes, Ctrl+O opens a file, Ctrl+, opens settings. Dropping a `.nro` on the window plays it.
- `NEXO2_UI_OPEN=ajustes|acerca|controles` opens that window at start, for screenshots.
- The `NEXO2_MULTICORE`, `NEXO2_GPU_ASYNC`, `NEXO2_AUDIO` and `NEXO2_GPU_THREADS` environment variables still override the settings.
