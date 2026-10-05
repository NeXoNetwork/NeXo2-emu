# Input: controllers through hid shared memory

Programs do not ask for button state over IPC. The `hid` service hands out a 0x40000-byte
shared memory block (IAppletResource cmd 0) and keeps writing controller states into it;
libnx's `padUpdate` reads it directly. NeXo does the same.

## Code map

| File | Role |
| :--- | :--- |
| `src/core/hle/input.hpp/.cpp` | `PadInput` (buttons + sticks), `NpadButton` bits, `HidLayout` offsets, `InputState` (writes the LIFOs) |
| `src/core/hle/kernel.*` | `GetHidSharedMemoryHandle()` (one hid block per process), `SetPadInput()` |
| `src/core/hle/services/hid.cpp` | IAppletResource `GetSharedMemoryHandle` |
| `src/main.cpp` | `ReadPadInput()`: PC keyboard / gamepad → `PadInput`, sent once per UI frame while running |

## Shared memory layout (only what NeXo writes)

Offsets were checked by compiling libnx's `hid.h` for aarch64.

| Offset | Content |
| :--- | :--- |
| 0x9A00 | `npad`: 10 entries of 0x5000 bytes (No1..No8, Handheld = 8, Other = 9) |

Inside one npad entry (`HidNpadInternalState`):

| Offset | Content |
| :--- | :--- |
| 0x000 | style set (1 = FullKey / Pro Controller, 2 = Handheld) |
| 0x004 | joy assignment mode (0 = dual) |
| 0x008 | full key colors (attribute, body, buttons) |
| 0x014 | Joy-Con colors (attribute, left body/buttons, right body/buttons) |
| 0x028 | full key LIFO |
| 0x378 | handheld LIFO |
| 0x4188 | device type (bit 0 FullKey, bits 2-3 handheld left/right) |

A LIFO (`HidNpadCommonLifo`, 0x350 bytes):

| Offset | Content |
| :--- | :--- |
| 0x00 | unused (u64) |
| 0x08 | buffer count = 17 |
| 0x10 | tail: index of the **most recent** entry |
| 0x18 | count of valid entries (max 17) |
| 0x20 | 17 entries of 0x30 bytes: u64 sampling number, then `HidNpadCommonState` |

`HidNpadCommonState` (0x28 bytes): u64 sampling number, u64 buttons, stick L (s32 x, y),
stick R (s32 x, y), u32 attributes (bit 0 = connected), u32 reserved.

libnx reads entry `(tail + 18 - n + i) % 17` and retries if two neighbouring entries do not
have consecutive sampling numbers, so every write uses the next sampling number.

## What NeXo connects

- **No1** as a Pro Controller (FullKey), and
- **Handheld** (Joy-Con attached to the console), both with the same buttons.

`padInitializeDefault` reads No1 + Handheld and ORs the buttons, so either one works.
Both are connected from the start (a first empty state is written when the memory is created).

Stick pseudo-buttons (StickLLeft, StickLUp...) are set when a stick passes half its range,
as the real hid service does. Stick range is ±0x7FFF, up is positive Y.

## Live updates

Shared memory is copied into the program's memory when it is mapped. `KSharedMemory`
remembers `mapped_address`, and `InputState::Update` writes the two LIFOs both to the
service copy and to the program's mapped copy.

## Default PC mapping

| Switch | Keyboard (by key position) | Gamepad (SDL, by position) |
| :--- | :--- | :--- |
| D-pad | arrow keys | D-pad |
| A / B / X / Y | X / Z / S / A | right / bottom / top / left face button |
| L / R | Q / W | shoulders |
| ZL / ZR | 1 / 2 | triggers |
| + / − | Enter / Backspace | Start / Back |
| Left stick | T F G H | left stick |
| Right stick | I J K L | right stick |

The keyboard is ignored while an ImGui text field has focus.

## Limitations

- One player only; touch screen, mouse, keyboard device and motion sensors are not filled.
- Input is sampled once per UI frame, not on a hid timer.
