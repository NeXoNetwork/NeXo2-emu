# Games (NSO + NPDM)

Homebrew comes as a single `.nro` file ([nro-loader-and-hle.md](nro-loader-and-hle.md)).
Games for Switch 1 and Switch 2 are built differently. The executable part, the **ExeFS**,
is a set of NSO modules plus a `main.npdm` file, and the data lives in the **RomFS**.

NeXo loads games that are already **extracted**, i.e. a folder made from your own console's dump.
NeXo never decrypts anything. Reading NSP/XCI/NCA files directly with your own keys is the next phase.

## Folder layout

```
games/
  My Game/
    exefs/          rtld, main, subsdk0..9, sdk, main.npdm
    romfs.bin       the RomFS as one file (optional)
    control.nacp    name, author, version (optional)
    icon_AmericanEnglish.dat   icon, a JPEG (optional)
```

- The NSO files can also sit directly in the game folder, without the `exefs/` subfolder.
- The library scans `games/` next to the executable, plus `sdmc/switch` and the folders added in Settings.
  A game folder shows up as one entry.
- You can also open it with Archivo → Abrir: choose its `main.npdm`.
- From the command line, pass the folder: `NeXo2.exe "games\My Game"`.

## Loading (`System::LoadGame`)

1. **`main.npdm`** (`loader/npdm.cpp`). The `META` header gives:
   - main thread priority (0x0E), core (0x0F) and stack size (0x1C);
   - 64-bit flag (0x0C bit 0). 32-bit programs are rejected.
   - The `ACI0` block gives the program id (title id).
   - Service and SVC permissions are ignored.
2. **NSO modules** (`loader/nso.cpp`), in this order: `rtld`, `main`, `subsdk0`…`subsdk9`, `sdk`.
   - Each one goes right after the previous one, starting at 0x8000000.
   - The 0x100-byte header gives three segments (.text, .rodata, .data). Each has a file offset, a memory offset and a size.
   - Flag bits 0–2 mark which segments are LZ4-compressed. They use raw LZ4 blocks with no frame header (`Lz4DecompressBlock`).
   - `.bss` follows `.data` and is zero-filled.
   - Regions are mapped R-X / R-- / RW- with the module name, so the memory map shows "main .text" and so on.
3. **Process** (`Kernel::SetupApplicationProcess`). This is how Horizon starts a game, unlike homebrew:
   - X0 = 0 (no loader config list), X1 = main thread handle, PC = first module (`rtld`).
     `rtld` relocates and links the other modules itself.
   - Stack and priority come from `main.npdm`.
   - `svcGetInfo(ProgramId)` returns the real program id.
4. **RomFS:** `romfs.bin` next to the ExeFS. `fsp-srv` command 200 `OpenDataStorageByCurrentProcess` returns an
   `IStorage` that reads that file (`FileStorage`).
5. **Save data:** `fsp-srv` command 51 `OpenSaveDataFileSystem` opens `save/<program id>/` next to the `sdmc` folder.
   It is created the first time.

## What is still missing

- Many services that commercial games use and homebrew does not:
  - `acc:u0` (user accounts), `pctl`, `friends`, `ns`, `aoc` (DLC), `prepo`, `mm:u`, `audren:u` (real audio renderer)…
  - each is added as games ask for it. The log names any service that does not exist yet.
- Reading NSP / XCI / NCA directly (needs the user's own keys).
- Updates (patch RomFS) and DLC.

## Tests

- `Nso_Lz4Block`: LZ4 blocks, including overlapping copies, lengths over 15 and broken data.
- `Game_LoadsExtractedExefs`:
  - Builds a game folder from `hello.nro`: its `.text` becomes an LZ4-compressed NSO, plus a `main.npdm` and a `romfs.bin`.
  - Checks the npdm values, the entry registers (X1 = 0x10001), the memory map, that it runs to exit, and `Restart`.
