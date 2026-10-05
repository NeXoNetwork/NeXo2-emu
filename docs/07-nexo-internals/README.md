# 07 — NeXo internals

How NeXo 2 itself is built (our code, not the console).

| File | Content |
| :--- | :--- |
| [cpu-interpreter.md](cpu-interpreter.md) | ARM64 interpreter: structure, implemented instructions, tests, how to add an instruction, performance |
| [nro-loader-and-hle.md](nro-loader-and-hle.md) | NRO loader, process memory layout, Homebrew ABI, HLE kernel SVCs |
| [ipc-and-services.md](ipc-and-services.md) | IPC (HIPC/CMIF/TIPC, domains), all services (sm:, applet, hid, time, fs/SD card...), how to add a service |
| [input.md](input.md) | Controllers: hid shared memory layout, LIFOs, PC keyboard/gamepad mapping |
| [display.md](display.md) | vi + nvdrv: binder buffer queue, GraphicBuffer layout, block-linear deswizzle, the "Pantalla" window |

## Test suite

`nexo2_tests` (49 tests) covers every page above: CPU programs and SIMD differential tests,
the NRO loader, IPC, the libnx start-up services, display (deswizzle, GraphicBuffer parsing,
pixel formats), SD card paths and controller LIFOs. Run it after any change:
`build\Release\nexo2_tests.exe`.
