# Games (NSP, XCI, NSO + NPDM)

Homebrew comes as a single `.nro` file ([nro-loader-and-hle.md](nro-loader-and-hle.md)).
Games for Switch 1 and Switch 2 are built differently. The executable part, the **ExeFS**,
is a set of NSO modules plus a `main.npdm` file, and the data lives in the **RomFS**.

NeXo loads games in two ways. Both need a dump of your own console:

- **NSP / XCI** files, decrypted with **your console's keys**. NeXo ships no keys.
- **extracted** folders (ExeFS + `romfs.bin`).

## Keys and NSP / XCI (`loader/package.*`, `crypto/aes.*`)

Put your `prod.keys`, and `title.keys` if you have one, in `keys/` next to the executable. NeXo creates that folder.
`NEXO2_KEYS` can point somewhere else. Without keys, opening an NSP gives a clear message saying where to put them.

- **NSP** = PFS0. Its `.tik` tickets give the title key (common tickets only; personalized ones are tied to a console).
- **XCI** = "HEAD" header at 0x100 (or 0x1100 if the dump starts with the key area) → root HFS0 → `secure` partition (HFS0).
- **NCA**:
  - The 0xC00-byte header is AES-128-XTS with `header_key`. Sectors are 0x200 bytes with a big-endian sector number as the tweak.
  - NCA3 decrypts 0x000–0xBFF as sectors 0–5. NCA2 decrypts each FS header as sector 0.
  - Key generation = max(0x206, 0x220) − 1.
  - If there is a rights id: title key (ticket or title.keys) decrypted with `titlekek_XX`.
  - If not: key area (0x300) decrypted with `key_area_key_{application,ocean,system}_XX`; key 2 is the CTR key.
  - Sections: start/end at 0x240 + 0x10·i, in 0x200 units. FS header at 0x400 + 0x200·i: type (0 RomFS, 1 PFS0) at +2, encryption at +4 (1 none, 3 CTR).
  - CTR counter: the 8 bytes at +0x140 reversed, then the absolute offset / 16 (big endian). Sections are decrypted on read (`CtrFile`).
  - PFS0 is at hash data +0x38 (FS header 0x40). The RomFS is IVFC level 6 (FS header 0x90).
- The program NCA (type 0) gives the ExeFS and the RomFS. The control NCA (type 2) gives `control.nacp` and the icon from its RomFS (the library shows them).
- AES uses mbedTLS when it is built (with the CPU's AES instructions), or a built-in implementation otherwise (checked against FIPS-197).
- Not supported yet: updates (BKTR), DLC, XTS-encrypted sections.

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
  - `acc:u0` (user accounts), `pctl`, `friends`, `ns`, `aoc` (DLC), `prepo`, `mm:u`, `hwopus`…
  - each is added as games ask for it. The log names any service that does not exist yet.
- Updates (patch RomFS) and DLC.

## Tests

- `Crypto_AesKnownAnswer`: FIPS-197 vector, XTS round trip, CTR from the middle = CTR of the whole.
- `Package_NspLoadsAndRuns`: builds a real-format NSP with made-up keys.
  - Program NCA (PFS0 ExeFS from `hello.nro` + RomFS) and control NCA, headers XTS-encrypted and sections CTR-encrypted.
  - Checks the missing-keys message, loading, the title from the NACP, a RomFS file, that it runs, and that the library reads the control.
- `Nso_Lz4Block`: LZ4 blocks, including overlapping copies, lengths over 15 and broken data.
- `Game_LoadsExtractedExefs`:
  - Builds a game folder from `hello.nro`: its `.text` becomes an LZ4-compressed NSO, plus a `main.npdm` and a `romfs.bin`.
  - Checks the npdm values, the entry registers (X1 = 0x10001), the memory map, that it runs to exit, and `Restart`.
