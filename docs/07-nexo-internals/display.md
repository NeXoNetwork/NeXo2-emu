# Display: vi + nvdrv (how a frame reaches the "Pantalla" window)

NeXo 2 shows what a homebrew draws with libnx's software framebuffer (`framebufferCreate`,
`consoleInit`...). There is no GPU emulation yet: the program writes pixels into memory itself,
and NeXo only has to find that memory, undo the GPU tiling, and copy it to a texture.

## Code map

| File | Role |
| :--- | :--- |
| `src/core/hle/display.hpp/.cpp` | `Display` (last presented frame), `NvMapTable` (GPU memory blocks), `DeswizzleBlockLinear`, pixel format conversion |
| `src/core/hle/services/nvdrv.*` | `nvdrv`, `nvdrv:a`, `nvdrv:s` - Open/Ioctl/Close/Initialize/QueryEvent/SetAruid; devices `/dev/nvmap` and `/dev/nvhost-ctrl` |
| `src/core/hle/services/vi.*` | `vi:m`, `vi:s`, `vi:u` - display service, layers, and the binder (Android buffer queue) |
| `src/core/hle/services/applet.cpp` | ISelfController 40 `CreateManagedDisplayLayer` (returns layer id 1) |
| `src/main.cpp` | "Pantalla" window: SDL streaming texture updated when `Display::Frame().count` changes |

## The path of a frame

```
libnx                                   NeXo
-----                                   ----
viInitialize        vi:m cmd 2  ------> ApplicationDisplayService (+ 100/101/102/103 sub-interfaces)
viOpenDefaultDisplay       1010 ------> display id 0
appletCreateManagedDisplayLayer  -----> layer id 1
viCreateLayer → OpenLayer  2020 ------> writes the "native window" parcel (binder id = layer id)
nwindowCreate → binder CONNECT -------> BufferQueue for that binder id
nvInitialize (transfer memory) -------> svcCreateTransferMemory + nvdrv Initialize
nvMapInit / nvFenceInit --------------> Open("/dev/nvmap"), Open("/dev/nvhost-ctrl")
framebufferCreate:
  nvmap CREATE + ALLOC ---------------> NvMapTable: id -> address in guest memory
  SET_PREALLOCATED_BUFFER x2 ---------> BufferQueue slot: width, height, format, layout, pitch...
every frame:
  DEQUEUE_BUFFER ---------------------> next slot (round robin), empty fence
  (program draws into the buffer)
  QUEUE_BUFFER -----------------------> Display::Present(): read, deswizzle, convert to RGBA
```

## Binder parcels

The binder speaks Android's `IGraphicBufferProducer` protocol. A parcel is a 16-byte header
(data size, data offset, objects size, objects offset) followed by the data. Requests start
with an interface token (i32 strict-mode policy + UTF-16 name). `ParcelReader` / `ParcelWriter`
in `vi.cpp` handle this. Supported transaction codes: REQUEST_BUFFER, SET_BUFFER_COUNT,
DEQUEUE_BUFFER, DETACH_BUFFER, QUEUE_BUFFER, CANCEL_BUFFER, QUERY, CONNECT, DISCONNECT,
SET_PREALLOCATED_BUFFER. Anything else stops the CPU with a clear message.

## GraphicBuffer layout (as libnx serializes it)

`ParseGraphicBuffer` reads the flattened buffer as 32-bit words:

| Words | Content |
| :--- | :--- |
| 0..9 | Android header: magic `GBFR` (0x47424652), width, height, stride, format, usage, pid, refcount, numFds, numInts |
| 10..22 | NV header: -1, **nvmap id** (11), 0, magic 0xDAFFCAFF, pid, type, usage, **format** (17), ext format, stride, total size, plane count, 0 |
| 23.. | Plane 0 (`NvSurface`): +0 width, +1 height, +2/+3 color format (u64), **+4 layout** (1 pitch, 3 block linear), **+5 pitch**, +6 unused, **+7 offset**, +8 kind, **+9 block_height_log2**, +10 scan, +11 second field offset, +12/+13 flags, **+14 size** |

The plane starts on a 4-byte boundary (the structure is packed), so the u64 color format
is not 8-byte aligned. This layout was confirmed from a real buffer sent by libnx.

## Block linear (GPU tiling)

NVIDIA GPUs store images in "GOBs" (groups of bytes) of 64 bytes x 8 rows = 512 bytes.
GOBs are stacked vertically into blocks of `1 << block_height_log2` GOBs (libnx uses 16,
i.e. 128 rows). Inside a GOB, 16-byte chunks are placed like this:

```
chunk i (0..31) -> y = ((i >> 1) & 6) | (i & 1)
                   x = ((i << 3) & 0x10) | ((i << 1) & 0x20)
```

`DeswizzleBlockLinear` walks the blocks in memory order and copies each chunk to its linear
position. `tests/display_tests.cpp` checks it against the classic byte-address formula
written independently.

## Pixel formats

| Android format | Value | Converted as |
| :--- | :--- | :--- |
| RGBA_8888 | 1 | as is |
| RGBX_8888 | 2 | alpha forced to 255 |
| RGB_565 | 4 | 5/6/5 bits expanded to 8 (libnx console default) |
| BGRA_8888 | 5 | R and B swapped |
| RGBA_4444 | 7 | 4 bits expanded to 8 |

The UI ignores alpha (`SDL_BLENDMODE_NONE`), like the real screen.

## Limitations (next steps)

- No vsync: DEQUEUE_BUFFER never waits, so a program draws as fast as the interpreter runs.
- GPU work (`/dev/nvhost-gpu`, `nvhost-as-gpu`...) goes to the emulated GPU: clears, copies and
  blits into swapchain images show up here. Drawing with shaders is the next phase: see [gpu.md](gpu.md).
- One display (1280x720, handheld). Docked 1920x1080 is not reported yet.
- Fences: the emulated GPU finishes work when it is submitted, so they are already reached.
