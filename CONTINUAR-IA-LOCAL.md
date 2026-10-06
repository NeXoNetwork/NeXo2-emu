# NeXo 2 — Guía para continuar el desarrollo con otra IA

> Pásale este archivo entero a la IA (local o la que sea) al empezar cada sesión.
> Contiene todo lo necesario para seguir trabajando sin el historial anterior.
> Última actualización: 2026-10-06.

---

## 1. Instrucciones para la IA (léelas primero)

Eres el asistente de programación de **Jose**, que está **aprendiendo a programar**.

- **Responde en español**, de forma breve y con comandos listos para copiar y pegar (CMD de Windows).
- Explica el *porqué* de los cambios en pocas frases: Jose quiere aprender, no solo copiar.
- **Jose hace los commits él mismo.** Al terminar un cambio, dale el `git add` / `git commit -m "..."` sugerido, pero no hagas commits tú.
- Antes de dar algo por terminado: **debe compilar en Release y `nexo2_tests` debe salir con 0 fallos.**
- No inventes APIs de la consola. Si no estás seguro de un valor (un comando IPC, un método de la GPU), dilo y busca en Switchbrew, libnx o deko3d.
- **Clean-room:** escribimos el código nosotros a partir de documentación pública (Switchbrew, libnx, deko3d, envytools). No copies código de otros emuladores. En la documentación de NeXo no nombres otros proyectos de emuladores de referencia.
- Si tu ventana de contexto es pequeña, **no leas todo el repo**: lee solo los archivos de la tarea (sección 4) y la guía de `docs/07-nexo-internals/` que toque.

---

## 2. Qué es NeXo 2

Es un emulador experimental y de código abierto (GPLv3) de **Nintendo Switch 2**, escrito desde cero en **C++20**.

- Ejecuta homebrew `.nro` hecho con libnx (por ejemplo NX-FixCheat y UMSDPong, que se juega con un mando de Xbox).
- No incluye firmware, claves ni juegos.

| Cosa | Dónde |
| :--- | :--- |
| Repo local | `C:\Users\Jous\Documents\NeXo\NeXo2-emu` |
| Repo remoto | `https://git.joustech.space/NeXo/NeXo2-emu` |
| Web (GitHub Pages) | `C:\Users\Jous\Documents\NeXo\www` (HTML/CSS/JS sin dependencias, ES/EN) |
| Herramientas | Windows, Visual Studio 2022 (MSVC v143), CMake ≥ 3.25, Git, Python 3 (opcional), LLVM (opcional, solo para recompilar los NRO de test) |

**Dependencias:** son submódulos en `externals/`.

- SDL3, ImGui y glad.
- **dynarmic**, el JIT (de `github.com/azahar-emu/dynarmic`, licencia 0BSD). Tiene submódulos propios.
- **ext-boost**, solo cabeceras.

---

## 3. Compilar y probar

```cmd
cd C:\Users\Jous\Documents\NeXo\NeXo2-emu
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 17 2022" -T v143 -A x64
cmake --build build --config Release
build\Release\nexo2_tests.exe
build\Release\NeXo2.exe tests\generated\hello.nro
```

### Opciones de CMake

| Opción | Por defecto | Qué hace |
| :--- | :--- | :--- |
| `NEXO2_BUILD_APP` | ON | Compila la app; necesita SDL3 e ImGui |
| `NEXO2_BUILD_TESTS` | ON | Compila `nexo2_tests` |
| `NEXO2_ENABLE_JIT` | ON | Activa dynarmic; solo funciona en x86-64 |

Para compilar sin JIT: `-DNEXO2_ENABLE_JIT=OFF`.

### Tests

- `nexo2_tests` ejecuta **cada test dos veces**: una con el intérprete y otra con el JIT. Hoy son **68 × 2 = 136 tests**.
- Para usar un solo modo: `--interp` o `--jit`.
- Usa siempre **Release**: en Debug el intérprete es muchísimo más lento.

### Herramientas opcionales (Python, en `tools/`)

Solo hacen falta si cambias lo que generan, porque los resultados ya están en el repo.

| Herramienta | Qué hace |
| :--- | :--- |
| `make_nro.py` | Compila `tests/programs/nro_*/main.c` → `tests/generated/*.nro`. Necesita clang, ld.lld y llvm-objcopy (`winget install LLVM.LLVM`) |
| `cpu_fuzz.py` | Genera `tests/generated/cpu_fuzz.bin` comparando con QEMU (necesita Linux/WSL con qemu-aarch64) |
| `gen_simd_tests.py` | Genera los tests SIMD comparando con QEMU |
| `asm2cpp.py` | Ensamblador ARM64 → array C++ para los tests |

`tests/homebrew/` está en `.gitignore`. Ahí van NRO de terceros para probar a mano.

---

## 4. Mapa del código

```
src/
  main.cpp                  App: ventana SDL3 + ImGui, EmuThread (CPU en su propio hilo),
                            ventanas Programa / Pantalla / Diagnostics (casilla JIT, estadísticas GPU)
  core/
    memory/memory.hpp       Memoria virtual: tabla de páginas de 2 niveles + tabla plana para el JIT
                            (EnableFlatPageTable, FLAT_BITS = 34), MarkCode (código automodificable)
    arm64/
      interpreter.hpp/.cpp  CPUState, Run/Step; SetJitEnabled / JitAvailable
      interpreter_*.cpp     Intérprete por grupos: dp_imm, dp_reg, ldst, fp, simd, simd_ldst, crypto
      interpreter_fast.cpp  Caché de instrucciones decodificadas (Decode() + funciones rápidas)
      simd_common.hpp, fp_ops  Coma flotante con las reglas exactas de ARM
      jit_dynarmic.*        JitBackend: callbacks de memoria, SVC → kernel, instrucciones
                            desconocidas → intérprete (RunPlain), monitor exclusivo
    loader/nro.*            Cargador NRO + Homebrew ABI
    system.*                Une memoria + CPU + kernel
    hle/
      kernel.* / kernel_threads.cpp / kernel_objects.hpp
                            SVCs, handles, planificador con 6 núcleos emulados (turnos en 1 hilo del PC),
                            mutex / condvar, detección de bloqueos. Contiene GPU::Gpu (GetGpu(), NvEventSlot)
      ipc.*, service.*      HIPC / CMIF / TIPC, dominios, ServiceObject
      display.*, input.*    Cola de buffers del binder; LIFOs de hid (teclado y mando SDL)
      services/             sm, set, apm, applet, hid, time, fs (SD = carpeta sdmc), vi, nvdrv
  video_core/               GPU (fase 1, síncrona)
    gpu.*                   GpuMemoryManager, Syncpoints, Channel (GPFIFO + pushbuffers), Gpu
    engines.*               Maxwell3D (0xB197), DMA (0xB0B5), Fermi2D (0x902D),
                            KeplerCompute (0xB1C0), InlineToMemory (0xA140), MacroInterpreter (MME)
    surface.*               Block linear, formatos de color y profundidad
tests/                      *_tests.cpp + test_framework.hpp + test_main.cpp
  programs/nro_*/           Fuentes C de los homebrew de prueba (hello, ipc, libnx_init, threads, gpu)
  generated/                .nro, cpu_fuzz.bin, deko3d_macros.hpp, etc. (ya generados)
docs/                       01-hardware … 06-resources, 07-nexo-internals (en inglés)
```

### Guías internas (léelas antes de tocar cada parte)

Están en `docs/07-nexo-internals/`:

- `cpu-interpreter.md`
- `jit.md`
- `cpu-fuzzing.md`
- `nro-loader-and-hle.md`
- `ipc-and-services.md`
- `threads.md`
- `input.md`
- `display.md`
- `gpu.md`

### Convenciones del código

- Los comentarios del código van en **español**, sin tildes; la documentación de `docs/` va en **inglés**.
- Namespaces: `NeXo2::Core`, `NeXo2::HLE`, `NeXo2::GPU`. Los tipos son `u8`…`u64` y `s32`…
- Para registrar mensajes se usa `Common::Logger::Log(Logger::Level::Warning, "...")`. Los avisos de la GPU que se repiten pasan por `Gpu::Warn(clave, mensaje)`, que solo avisa una vez.
- Cada cosa nueva lleva **tests** en `tests/` (añádelos al `CMakeLists.txt`) y su página en `docs/07-nexo-internals/`, que también se añade al `README.md` de esa carpeta.
- Los tests deben pasar **en los dos modos** (intérprete y JIT).

---

## 5. Estado actual

| Parte | Estado |
| :--- | :--- |
| CPU ARM64 (ARMv8.2 EL0: enteros, FP, NEON, AES/SHA, CRC32) | ✅ 100 % igual que QEMU en ~15 600 instrucciones al azar |
| Caché de decodificación | ✅ ~180 M instr/s |
| JIT dynarmic | ✅ ~1 100 M instr/s (×6). Commit `6e999e9` |
| Hilos (6 núcleos emulados que se turnan en 1 hilo del PC) | ✅ fase 1 |
| Kernel HLE, IPC, servicios del arranque de libnx | ✅ |
| Pantalla (vi + nvdrv + binder), mandos, tarjeta SD | ✅ |
| GPU fase 1 | ✅ hecha y probada, **⚠️ SIN COMMIT** (ver abajo) |
| Web en estilo Switch 2 | ✅ hecha, **⚠️ SIN COMMIT** |
| GPU fase 2 (shaders, dibujar, Vulkan) | ❌ |
| Audio | ❌ |
| Formatos de juego (NSO, NCA, RomFS) | ❌ |

### Lo primero que hay que hacer (pendiente)

**1. Compilar y probar la GPU fase 1** en Windows. Si sale todo bien, hacer su commit:

```cmd
cd C:\Users\Jous\Documents\NeXo\NeXo2-emu
cmake --build build --config Release
build\Release\nexo2_tests.exe
git add -A
git commit -m "GPU phase 1: nvhost devices, channels, pushbuffers, MME macros, clears, DMA/2D/inline, syncpoints"
git push
```

Lo esperado es **136 tests y 0 fallos**. Si MSVC da errores o avisos en `src/video_core/`, esos archivos solo se han compilado con GCC/Clang, así que puede faltar algún `#include` o haber algún cast.

**2. Subir la web:**

```cmd
cd C:\Users\Jous\Documents\NeXo\www
git add -A
git commit -m "Web: Switch 2 style, JIT, threads and GPU phase 1"
git push
```

---

## 6. Detalles técnicos que conviene saber

### JIT

- `JitBackend` va dentro de `Interpreter::Run`. En cada franja copia el `CPUState` a dynarmic y lo trae de vuelta.
- Las páginas de código tienen la entrada a `nullptr` en la tabla plana. Así las escrituras pasan por un callback que llama a `InvalidateCacheRange`.
- El pase **MiscIROpt está desactivado**: recorría GB de ceros. `MemoryReadCode` devuelve `nullopt` si la página no existe.
- Cuando se cae al intérprete (`RunPlain`), se restauran los ticks para no contar dos veces.
- Ticks = instrucciones.

### GPU (Maxwell GM20B, síncrona)

**GPFIFO:**

- dirección = `entry & 0xFFFFFFFFFC`
- palabras = `(entry >> 42) & 0x1FFFFF`

**Cabecera del pushbuffer:**

| Bits | Campo |
| :--- | :--- |
| 0–12 | método |
| 13–15 | subcanal |
| 16–28 | count |
| 29–31 | modo: 1 = incrementar, 3 = no incrementar, 4 = inmediato, 5 = incrementar una vez |

**Métodos del canal (< 0x40):**

- 0x00 = SetObject (asigna un motor a la subcanal)
- 0x04–0x07 = semáforo
- 0x1C/0x1D = syncpoint

**Macros (MME):**

- Campos de la instrucción:

  | Bits | Campo |
  | :--- | :--- |
  | 0–2 | op |
  | 4–6 | result op |
  | 7 | exit |
  | 8–10 | dst |
  | 11–13 | ra |
  | 14–16 | rb |
  | 14–31 | inmediato |
  | 17–21 | ALU op |

- Op 3: `((ra >> rb) & mask) << bf_dst`
- Op 4: `((ra >> bf_src) & mask) << rb`
- Los saltos tienen delay slot salvo que lleven *annul* (bit 5).
- El bit de exit se ignora dentro de un delay slot y en los saltos tomados.
- Están probadas con las macros reales de deko3d (`tests/generated/deko3d_macros.hpp`).

**Fences:**

- Cada canal tiene su syncpoint.
- El método 3D 0xB2 (SyncptAction) incrementa el syncpoint.
- `Syncpoints::AddWaiter` señala el evento de nvhost-ctrl cuando se alcanza el valor.

**Block linear:**

- Offset dentro de un GOB: `(x/32)*256 + (y%8/2)*64 + (x%32/16)*32 + (y%2)*16 + x%16`.

**Los métodos de dibujo (draw)** solo se cuentan como "saltados". Implementarlos es la GPU fase 2.

### Lecciones aprendidas (errores que ya pasaron)

- **MSVC y coma flotante:** `ldexp` con 2^-1024 dejaba flags del PC en FPSR. Usa constantes literales (`0x1p-1024`).
- **Git:** usa `git --no-optional-locks status` si algo más está leyendo el repo, para que no queden archivos `.lock`.

---

## 7. Próximos pasos (en orden recomendado)

### 1. GPU fase 2: dibujar de verdad

1. Guardar el estado 3D completo: vertex buffers, índices, viewport, scissor, blend, depth y render targets (ya están en 0x200).
2. Traducir los shaders de Maxwell (SM 5.x) a SPIR-V. Es lo más grande. Empieza por un decodificador y un IR propios, con tests unitarios por instrucción.
3. Backend Vulkan (en el PC): caché de pipelines, texturas (block linear → lineal), sincronización con los syncpoints.
4. Test de extremo a extremo: un NRO con deko3d que dibuje un triángulo, comparado con una imagen esperada.

### 2. Audio

- Servicios `audout:u` (sencillo, buffers PCM) y luego `audren:u` (el renderer, mucho más complejo).
- Salida con SDL3 audio.

### 3. Formatos de juego

- NSO (segmentos comprimidos con LZ4), NPDM y luego NCA y RomFS.
- **Nunca incluir claves.** El usuario tendría que aportarlas.

### 4. Rendimiento

- Franjas del JIT más grandes cuando solo hay un hilo listo.
- Fastmem.
- Hilos fase 2: cada núcleo emulado en su propio hilo del PC.

### Recordatorio de cada paso

- tests nuevos que pasen en los dos modos;
- página en `docs/07-nexo-internals/`;
- actualizar la tabla de estado de este archivo y la web (`www/index.html`; las traducciones al inglés están en `main.js`, en el objeto `EN`).

---

## 8. Consejos para trabajar con una IA local

- **Modelos recomendados:** uno de código de 14B–32B (por ejemplo Qwen2.5-Coder o DeepSeek-Coder) con **contexto ≥ 32k**, en Ollama o LM Studio. Para escribir y entender el código se puede usar en VS Code con la extensión Continue.
- **Dale contexto pequeño y concreto:** este archivo, más los 2–4 archivos que hay que tocar, más la guía de `docs/07` que corresponda.
- **Pide cambios pequeños** (una función o un test cada vez), compila y ejecuta `nexo2_tests` después de cada uno.
- **Si un test falla,** pégale a la IA la salida exacta del test y el archivo afectado.
- **Desconfía de los valores "de memoria"** (números de métodos, offsets, comandos IPC): compruébalos en Switchbrew, libnx o deko3d.
