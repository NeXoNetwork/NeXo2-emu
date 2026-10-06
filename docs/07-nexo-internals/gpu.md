# GPU (phase 1: command processor, no shaders yet)

All existing homebrew draws with the **Maxwell GM20B** GPU of the Switch 1 (through deko3d or
nouveau), which the Switch 2 runs in compatibility mode. Phase 1 emulates everything a program
needs to *talk* to that GPU and the engines that do not need shaders: memory, channels,
command buffers, clears, copies, blits, macros, syncpoints and semaphores. Results are written
straight into the program's memory, so whatever the GPU clears or copies into a swapchain image
appears in the "Pantalla" window through the existing `vi` path ([display.md](display.md)).

## Files

| File | Role |
| :--- | :--- |
| `src/video_core/gpu.hpp/.cpp` | `Gpu`, `GpuMemoryManager` (GPU virtual memory), `Syncpoints`, `Channel` (GPFIFO + pushbuffers) |
| `src/video_core/engines.hpp/.cpp` | The engines: 3D, DMA copy, 2D, compute, inline-to-memory, and the macro (MME) interpreter |
| `src/video_core/surface.hpp/.cpp` | Block linear addressing, colour/depth formats, clear value encoding |
| `src/core/hle/services/nvdrv.*` | The `nvdrv` service and its devices (`/dev/nvhost-*`) |

## From a program to the GPU

```
deko3d / libnx                      NeXo
--------------                      ----
nvMapCreate (memory)        ->  /dev/nvmap          block of program memory
nvAddressSpaceMap           ->  /dev/nvhost-as-gpu  GpuMemoryManager::Map(gpu va -> cpu addr)
nvGpuChannelCreate          ->  /dev/nvhost-gpu     Gpu::CreateChannel (own syncpoint)
dkQueueSubmitCommands
  nvGpuChannelKickoff       ->  KICKOFF_PB (Ioctl2) Channel::SubmitGpfifo
                                  each GPFIFO entry -> pushbuffer -> methods -> engines
dkQueueWaitIdle / fences    ->  /dev/nvhost-ctrl    syncpoint already reached (see below)
dkSwapchainAcquire/Present  ->  vi binder           Display::Present reads the image
```

### Pushbuffers

A pushbuffer is a list of 32-bit words. Each command starts with a header:

| Bits | Field |
| :--- | :--- |
| 0..12 | method (register number) |
| 13..15 | subchannel (which engine; bound with method 0 = class id) |
| 16..28 | count, or the value itself for "inline" commands |
| 29..31 | mode: 1 increasing, 3 non-increasing, 4 inline, 5 increase once |

Methods below 0x40 belong to the channel itself (bind engine, semaphores, syncpoint
increment/wait). The rest go to the engine bound to the subchannel.

### Synchronous model

The GPU runs the work **inside the ioctl that submits it**. When the program gets its fence
back, the work is done and the syncpoint has its new value, so `nvFenceWait` returns at once.
Waits inside a pushbuffer (semaphore acquire, syncpoint wait) that are not met only log a
warning. `EVENT_WAIT_ASYNC` for a value not reached yet arms the event; it is signalled when
the syncpoint gets there (`Syncpoints::AddWaiter`).

## Devices and ioctls

| Device | Ioctls |
| :--- | :--- |
| `/dev/nvmap` | CREATE, FROM_ID, ALLOC, FREE, PARAM, GET_ID |
| `/dev/nvhost-ctrl` | SYNCPT_READ/READ_MAX/INCR/WAIT/WAIT_EX, EVENT_WAIT, EVENT_WAIT_ASYNC, EVENT_SIGNAL/REGISTER/UNREGISTER |
| `/dev/nvhost-ctrl-gpu` | GET_CHARACTERISTICS (GM20B: classes B197/B0B5/902D/B1C0/A140/B06F), ZCULL_GET_CTX_SIZE/INFO, TPC masks, ZBC, GPU time |
| `/dev/nvhost-as-gpu` | INITIALIZE_EX, ALLOC_SPACE (fixed or not), FREE_SPACE, MAP_BUFFER_EX (incl. MODIFY), UNMAP, GET_VA_REGIONS, BIND_CHANNEL |
| `/dev/nvhost-gpu` | SET_NVMAP_FD, ALLOC_GPFIFO_EX2, ALLOC_OBJ_CTX, SUBMIT_GPFIFO, KICKOFF_PB (Ioctl2), ZCULL_BIND, error notifier, priority/timeouts |

`nvdrv` also answers `Ioctl2`/`Ioctl3` (commands 11/12) and `QueryEvent`. An ioctl that is
not implemented stops the CPU with its number, so it is easy to see what a program needs next.

## Engines

| Class | Engine | Implemented |
| :--- | :--- | :--- |
| 0xB197 | 3D | all registers stored; **clears** of colour targets (all formats, write mask, scissor, screen scissor, layers) and depth/stencil; macro upload and execution; syncpoint increment (`SyncptAction`); report semaphores; constant buffer upload; inline-to-memory. Draws are counted and skipped |
| 0xB0B5 | DMA copy | pitch <-> block linear copies, multi-line, component remap (memset-style fills, swizzles), semaphore release |
| 0x902D | 2D | blits between surfaces with point-sampled scaling, RGBA8 <-> BGRA8 |
| 0xA140 | Inline to memory | uploads from the pushbuffer, pitch or block linear |
| 0xB1C0 | Compute | registers and inline uploads; launches are skipped |

### Macros (MME)

deko3d uploads small programs to the 3D engine and calls them through methods 0xE00 and up
(an even method starts a call with its first parameter, the odd one adds parameters; the
macro runs at the end of the command). The interpreter (`MacroInterpreter`) has the 8
registers (r0 = 0), the ALU (add/sub with carry, logic), bitfield extract/insert, register
reads, branches with a delay slot (unless "annulled") and the exit bit (also with a delay
slot; ignored inside a delay slot and on taken branches). Results can be sent as method
writes with an auto-incrementing method address.

It is tested with **deko3d's real macros** (`tests/generated/deko3d_macros.hpp`, assembled
with deko3d's own tool from its `.mme` sources): ClearColor over a 3-layer render target,
BindColorBlendEnableState, FillRegisters.

### Block linear

NVIDIA images are stored in GOBs (64 bytes x 8 rows = 512 bytes) stacked in blocks of
2^n GOBs. `BlockLinearOffset(x_bytes, y, width_bytes, block_height_log2)` gives the byte
offset of any pixel; it is the exact inverse of the display's deswizzle (tested).

## Tests

`tests/gpu_tests.cpp` (run with the interpreter and with the JIT):

- GPU memory: allocation in the 4 KB / 64 KB regions, reads/writes across mappings.
- Clears: block linear RGBA8 with scissor and partial write mask, linear BGRA8, depth/stencil.
- Macros: a hand-assembled FillRegisters, every ALU/bitfield/read operation, and deko3d's real macros.
- DMA (pitch <-> block linear round trip, fill, component swap), inline upload, scaled 2D blit.
- Syncpoints, waiters and semaphores.
- `gpu.nro` (`tests/programs/nro_gpu`): a homebrew that does what libnx + deko3d do through
  `nvdrv` (open the devices, map memory, create a channel, upload a macro, clear, copy,
  KICKOFF_PB, wait for the fence) and checks the pixels in its own memory.

Deliberately breaking the macro delay slot, the exit rule, an ALU operation or the DMA remap
makes these tests fail.

## Next phases

1. **Shaders**: decode Maxwell shader binaries and run them (first in software, later as
   SPIR-V on Vulkan), plus textures (TIC/TSC descriptors), vertex fetch and rasterisation.
   Draws are already counted (`draws_skipped`) in the Diagnostics window.
2. **Vulkan backend**: render targets and textures as Vulkan images instead of software.
3. The Switch 2's own GPU (Ampere, T239) for native games.
