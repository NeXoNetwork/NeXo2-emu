# 🌀 NeXo 2 | Emulation Research Laboratory

**NeXo 2** es un proyecto de investigación de bajo nivel dedicado al estudio y análisis de la arquitectura de hardware de la plataforma **Nintendo Switch 2 (T239)**. El objetivo principal es la documentación de componentes del sistema y la experimentación con capas de compatibilidad en **C++20**.

**NeXo 2** is a low-level research project dedicated to the study and analysis of the **Nintendo Switch 2 (T239)** hardware architecture. The main goal is the documentation of system components and experimentation with compatibility layers using **C++20**.

---

## 🛠️ Objetivos del Proyecto / Project Goals

* **[ES] Documentación:** Análisis del SoC Nvidia T239 (Drake) y sus extensiones de instrucción.
* **[EN] Documentation:** Analysis of the Nvidia T239 (Drake) SoC and its instruction extensions.
* **[ES] Investigación de Memoria:** Gestión de memoria LPDDR5X y direccionamiento del sistema.
* **[EN] Memory Research:** LPDDR5X memory management and system addressing.
* **[ES] Traducción Binaria:** Experimentación con recompilación dinámica (JIT) de ARM64 a x86_64.
* **[EN] Binary Translation:** Experimenting with dynamic recompilation (JIT) from ARM64 to x86_64.
* **[ES] API Gráfica:** Mapeo experimental de funciones NVN2 hacia Vulkan 1.3.
* **[EN] Graphics API:** Experimental mapping of NVN2 functions to Vulkan 1.3.

---

## 📋 Especificaciones Técnicas / Technical Target

| Componente / Component | Especificación / Specification |
| :--- | :--- |
| **SoC** | Nvidia T239 (Custom Ampere) |
| **CPU** | 8x ARM Cortex-A78C |
| **GPU** | GA10B (1536 CUDA Cores) |
| **API Gráfica** | Vulkan 1.3 / NVN2 |
| **Arch** | ARMv8.2-A / ARMv9 (Research) |

---

## 📂 Estructura del Proyecto / Project Structure

```text
NeXo2/
├── .github/              # CI/CD & GitHub Templates
├── docs/                 # Documentación técnica y especificaciones
├── externals/            # Librerías externas (Dynarmic, SDL2, glad, etc.)
├── src/                  # Código fuente principal (Source code)
│   ├── common/           # Utilidades, Logger y tipos de datos globales
│   ├── core/             # El "corazón" del emulador
│   │   ├── arm64/        # Intérprete y lógica de CPU
│   │   ├── memory/       # Gestión de Memoria Virtual (VMM)
│   │   └── hle/          # Emulación de Alto Nivel (Kernel & Services)
│   ├── video_core/       # Implementación de Vulkan y Renderer
│   ├── input_common/     # Gestión de controles y periféricos
│   └── nexo_ui/          # Interfaz de usuario (Frontend)
├── tests/                # Pruebas unitarias para la CPU y Memoria
├── CMakeLists.txt        # Configuración principal de compilación
└── README.md             # Este archivo

```

## 🗺️ Hoja de Ruta / Roadmap (2026)

### 🚀 Fase 1: Infraestructura Base (Core) - [0%]
- [ ] **[ES/EN] Logging System:** Implementar un logger thread-safe en C++20 con soporte de colores. / Implement a thread-safe C++20 logger with color support.
- [ ] **[ES/EN] Memory Manager:** Clase `Memory` con soporte para paginación básica (4KB/64KB). / `Memory` class with basic paging support (4KB/64KB).
- [ ] **[ES/EN] VFS (Virtual File System):** Cargador de archivos para parsear cabeceras NCA/HFS2. / File loader for parsing NCA/HFS2 headers.
- [ ] **[ES/EN] Arg Parser:** Sistema de flags por consola (`--debug`, `--cpu-trace`). / CLI flags system (`--debug`, `--cpu-trace`).

### 🧠 Fase 2: Investigación de CPU (ARM64) - [0%]
- [ ] **[ES/EN] Registers State:** Estructura de datos para registros `X0-X30`, `SP`, `PC` y `NZCV`. / Data structure for `X0-X30`, `SP`, `PC`, and `NZCV` registers.
- [ ] **[ES/EN] Instruction Decoder:** Desensamblador básico para instrucciones ARMv8.2-A. / Basic instruction decoder for ARMv8.2-A.
- [ ] **[ES/EN] Interpreter Loop:** Ciclo Fetch-Decode-Execute para pruebas de concepto (PoC). / Basic Fetch-Decode-Execute cycle for Proof of Concept (PoC).
- [ ] **[ES/EN] JIT Integration:** Esqueleto de integración para `Dynarmic` o motor JIT nativo. / Integration skeleton for `Dynarmic` or a native JIT engine.

### 🖼️ Fase 3: Subsistema Gráfico (Vulkan) - [0%]
- [ ] **[ES/EN] Vulkan 1.3 Instance:** Inicialización de capas de validación y selección de GPU física. / Validation layers initialization and physical GPU selection.
- [ ] **[ES/EN] Windowing System:** Integración con SDL2/GLFW para la ventana de renderizado. / SDL2/GLFW integration for the render window.
- [ ] **[ES/EN] Memory Mapping:** Mapeo de memoria del emulador a objetos de Vulkan (VMA). / Emulator memory mapping to Vulkan objects (VMA).
- [ ] **[ES/EN] Shader Research:** Análisis del microcódigo Ampere para traducción a SPIR-V. / Ampere microcode analysis for SPIR-V translation.

### 📂 Fase 4: Kernel & Servicios (HLE) - [0%]
- [ ] **[ES/EN] Syscall Dispatcher:** Tabla de interrupciones para capturar llamadas al sistema. / Interrupt table for system call interception.
- [ ] **[ES/EN] Service Manager:** Arquitectura para registrar servicios `nn::*` (FS, Time, HID). / Architecture to register `nn::*` services (FS, Time, HID).
- [ ] **[ES/EN] Thread Manager:** Planificación de hilos para simular entorno multinúcleo. / Basic scheduling for multi-core simulation.

---

## ⚠️ Estado del Desarrollo / Development Status

**[ES]** El proyecto se encuentra en **Fase 0 (Exploración)**. No existe actualmente un binario ejecutable ni soporte para carga de software comercial. Este es un proyecto de **aprendizaje personal**.
**[EN]** This project is in **Phase 0 (Exploration)**. There is currently no executable binary or support for commercial software. This is a **personal learning project**.

## 🤝 Contribución / Contribution

**[ES]** Se aceptan contribuciones de desarrolladores interesados en C++20, ARM64 Assembly y Vulkan. Para colaborar, realice un Fork y envíe un Pull Request.
**[EN]** Contributions are welcome from developers interested in C++20, ARM64 Assembly, and Vulkan. To collaborate, please Fork the repo and submit a Pull Request.

## ⚖️ Licencia / License
Este proyecto se distribuye bajo fines estrictamente educativos y de preservación. / This project is distributed strictly for educational and preservation purposes.