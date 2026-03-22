# Especificaciones Técnicas del Objetivo (Target Hardware)

Este documento detalla las especificaciones técnicas estimadas para la plataforma Nintendo Switch 2 (SoC Nvidia T239), que sirven como base para el desarrollo del laboratorio NeXo 2.

## 1. Unidad de Procesamiento Central (CPU)
* **Arquitectura:** 8x núcleos ARM Cortex-A78C (Personalizados).
* **ISA:** ARMv8.2-A / ARMv9 (64-bit exclusivo). No hay soporte nativo para AArch32.
* **Frecuencia (Clock):** * Modo Docked: ~1.7 GHz (Pico).
    * Modo Portátil: ~1.1 GHz.
* **Caché:** 4MB L3 Compartido / 256KB L2 por núcleo.

## 2. Unidad de Procesamiento Gráfico (GPU)
* **Arquitectura:** Nvidia Ampere (Serie RTX 30 Custom).
* **Configuración:** 12 Streaming Multiprocessors (SM).
* **Núcleos CUDA:** 1,536 Cores.
* **Núcleos Tensor:** 48 (Soporte nativo DLSS 3.1/3.5).
* **Núcleos RT:** 12 (Soporte Ray Tracing).
* **Ancho de Banda Teórico:** 0.6 - 4.0 TFLOPS (Dependiendo del perfil energético).

## 3. Memoria (RAM)
* **Tipo:** 12 GB LPDDR5X.
* **Bus de Memoria:** 128-bit.
* **Velocidad de Transferencia:** 7500 MT/s.
* **Ancho de Banda Total:** 102 GB/s.
* **Distribución Sugerida:** * 9 GB disponibles para aplicaciones.
    * 3 GB reservados para Horizon OS (Next).

## 4. Almacenamiento y Entrada/Salida
* **Interno:** 256 GB UFS 3.1 (Velocidad de lectura ~2100 MB/s).
* **Externo:** Ranura MicroSD Express (Soporte SD 7.1).
* **Salida de Video:** HDMI 2.1 con soporte para 4K HDR y VRR.

## 5. Notas para el Desarrollo (Emulación)
* **Prioridad JIT:** Implementar decodificadores específicos para el set de instrucciones Cortex-A78C.
* **Prioridad Vulkan:** Mapear extensiones de Ampere (como el Conservative Rasterization o el Variable Rate Shading) directamente a través de Vulkan 1.3.
* **Audio:** DSP basado en ARM para procesamiento de audio espacial.