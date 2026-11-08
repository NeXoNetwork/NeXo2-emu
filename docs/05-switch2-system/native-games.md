# Running native Switch 2 games

What a game built for Switch 2 (program id `0x0400...`) needs from an emulator, grouped by area,
and where NeXo stands. Switch 1 games running on Switch 2 are covered in
[compatibility-mode.md](compatibility-mode.md).

Status: **Done** = in NeXo, **Partial** = enough for the first steps, **To do** = not started.

## CPU

Switch 2 builds use instructions newer than ARMv8.2. A single game has tens of thousands of each.

| What | Notes | Status |
| :--- | :--- | :--- |
| LSE atomics (LDADD, SWP, CAS, CASP...) | ARMv8.1 | Done |
| LDAPR | ARMv8.3 (RCpc) | Done |
| Pointer authentication | RETAA/RETAB, BRAA/BRAB/BLRAA/BLRAB (and the "Z" forms), LDRAA/LDRAB, PAC*/AUT*/XPAC*, PACGA. NeXo signs nothing: pointers stay as they are and authenticated branches jump to the plain address. PACGA returns a fixed hash of its inputs in the upper 32 bits. | Done |
| MTE | Only a few STG-family stores appear. Without tags, they only write back the base register (STZG also clears memory). | Done |
| Rare SIMD forms (FP16, ARMv8.2) | Seen in `sdk` | Check per game |

The JIT (dynarmic) does not know pointer authentication: it hands those instructions to NeXo's interpreter.

## Process and kernel

| What | Notes | Status |
| :--- | :--- | :--- |
| NPDM | 39-bit address space, cores 0-5, priorities 28-59. The ACID signature area is 0x300 bytes (Switch 1: 0x200). NeXo only reads META and ACI0. | Done |
| Six cores | Games create threads on cores 4 and 5. | Done |
| Memory | The console has 12 GB and a game is told about 9.5 GB (GetInfo 6/21). Some games put most of it in the heap: Mario Kart World asks `svcSetHeapSize` for about 7.9 GB at once. The rest goes through nn::os virtual memory in the alias region. | Done (9.5 GB; 16 GB heap region at `0x4'0000'0000`, 8 GB alias region; 64 GB address space) |
| `svcGetInfo` 29/30 | Region right after the alias region (address, size 0) | Done |
| `svcGetInfo` 31/32 | Remote memory used by graphics services (0) | Done |
| `svcGetInfo` 33 | Page granule of nn::os virtual memory (0x1000) | Done |
| `svc 0x80` | SetGpuMemoryAttribute | Partial (accepted) |

## Game files

Dumps of Switch 2 games are often extracted: an `exefs` folder (`main`, `main.npdm`, `rtld`, `sdk`,
`subsdk*`) next to a `romfs` folder with the game data as loose files (Mario Kart World: about
200,000 files). NeXo builds the RomFS tables in memory from that folder and reads each file only
when the game asks for it. Open `exefs/main.npdm` (or `main`) to start the game.

## Services

| What | Notes | Status |
| :--- | :--- | :--- |
| `appletOE` 10 | New OpenApplicationProxy | Done |
| `IApplicationProxy` 40, 60 | 60 is the graphics scale controller: its command 10 returns 10, 20 or 30 (1x, 2x, 3x) | Done (60 returns 10; 40 accepts everything) |
| `IApplicationFunctions` 320, 321 | Called during nn::oe start-up | Done (the interface answers unknown commands with success) |
| `aud:u` | New audio entry point; command 2 gives the audio renderer | Done |
| `hwopus` second entry | Hardware audio codec | Partial (silence) |
| `pl:u` 2000/2010 | Shared fonts | Check |

## Graphics system (the biggest part)

On Switch 2 the game does not talk to `nvdrv` and `vi` like on Switch 1. It gets a session from
`ISelfController` 2000 and speaks TIPC to a graphics system service with several sub-sessions:

- **gpu**: creates GPU-accessible memory and maps it. Mapped memory is used at the same address on the
  GPU as on the CPU. It also creates sparse regions for GPU-only memory (hundreds of MB, more for
  large games), which must be backed by real memory.
- **syncpt**: syncpoints through a shared page of values and wait slots, plus events.
- **vi2**: layers and presentation, with frame pacing at 60 Hz. The game waits for the vblank
  events; without them it never draws a frame.
- **nvsrv**: the old `nvdrv` protocol, used to open the GPU (`nvhost-ctrl-gpu`, channels, TSG).

The GPU is Ampere (T239, chip 0x17E). It differs from the Switch 1 Maxwell GPU in these ways:

- **Classes**: 3D `0xC797`, compute `0xC7C0`, copy `0xC7B5`.
- **Macros**: Turing-style 64-bit macros (MME64), with DMA reads into the macro FIFO.
- **Compute**: dispatch with QMD 3.0.
- **Synchronisation**: command buffers gated by semaphore acquires that the CPU releases later.
- **Shaders**: SM 8.6 SASS, a different instruction set from Maxwell's. This includes tensor
  instructions (HMMA) for upscaling, and task and mesh shaders.
- **Textures**: ASTC textures with the Switch 2 GOB swizzle.

| What | Status |
| :--- | :--- |
| Ampere characteristics in `nvhost-ctrl-gpu` | Done |
| Graphics system session and sub-sessions | To do |
| Ampere classes, MME64, QMD 3.0 | To do |
| SM 8.6 shader translation | To do |

## Order of work

1. CPU and kernel (done) → the game reaches the graphics system.
2. Graphics system sessions, with stubs that log every command → the game starts its first GPU work.
3. MME64 and Ampere classes → draws reach the rasterizer.
4. SM 8.6 shaders → pixels.
