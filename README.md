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

## 🗺️ Hoja de Ruta / Roadmap (2026)

### 🚀 Fase 1: Infraestructura Base (Core) - [0%]
- [ ] **[ES/EN] Logging System:** Logger thread-safe C++20 (colors/fmt).
- [ ] **[ES/EN] Memory Manager:** Virtual memory & paging (4KB/64KB).
- [ ] **[ES/EN] VFS (File System):** NCA/HFS2 binary header parsing.
- [ ] **[ES/EN] Arg Parser:** CLI flags (`--debug`, `--cpu-trace`).

### 🧠 Fase 2: CPU Research (ARM64) - [0%]
- [ ] **[ES/EN] Registers:** Data structures for `X0-X30`, `SP`, `PC`, `NZCV`.
- [ ] **[ES/EN] Instruction Decoder:** ARMv8.2-A base instruction decoding.
- [ ] **[ES/EN] Interpreter:** Basic Fetch-Decode-Execute loop (PoC).
- [ ] **[ES/EN] JIT Integration:** Dynarmic/Native JIT engine skeleton.

### 🖼️ Fase 3: Graphics (Vulkan) - [0%]
- [ ] **[ES/EN] Vulkan 1.3 Instance:** Validation layers & Physical Device selection.
- [ ] **[ES/EN] Windowing:** SDL2/GLFW integration for render window.
- [ ] **[ES/EN] Memory Mapping:** Emulator RAM to Vulkan Memory (VMA).
- [ ] **[ES/EN] Shader Research:** Ampere microcode analysis (SPIR-V).

### 📂 Fase 4: Kernel & Services (HLE) - [0%]
- [ ] **[ES/EN] Syscall Dispatcher:** Interrupt table for system call interception.
- [ ] **[ES/EN] Service Manager:** `nn::*` service registration (FS, Time, HID).
- [ ] **[ES/EN] Thread Manager:** Basic scheduling for multi-core simulation.

---

## ⚠️ Estado del Desarrollo / Development Status

**[ES]** El proyecto se encuentra en **Fase 0 (Exploración)**. No existe actualmente un binario ejecutable ni soporte para carga de software comercial. Este es un proyecto de **aprendizaje personal**.
**[EN]** This project is in **Phase 0 (Exploration)**. There is currently no executable binary or support for commercial software. This is a **personal learning project**.

## 🤝 Contribución / Contribution

**[ES]** Se aceptan contribuciones de desarrolladores interesados en C++20, ARM64 Assembly y Vulkan. Para colaborar, realice un Fork y envíe un Pull Request.
**[EN]** Contributions are welcome from developers interested in C++20, ARM64 Assembly, and Vulkan. To collaborate, please Fork the repo and submit a Pull Request.

## ⚖️ Licencia / License
Este proyecto se distribuye bajo fines estrictamente educativos y de preservación. / This project is distributed strictly for educational and preservation purposes.