# NeXo 2 | Emulation Research Laboratory

NeXo 2 es un proyecto de investigación de bajo nivel dedicado al estudio y análisis de la arquitectura de hardware de la plataforma Nintendo Switch 2 (T239). El objetivo principal es la documentación de componentes de sistema y la experimentación con capas de compatibilidad.

## Objetivos del Proyecto

* **Documentación de Arquitectura:** Análisis del SoC Nvidia T239 (Drake) y sus extensiones de instrucción.
* **Investigación de Memoria:** Estudio de la gestión de memoria LPDDR5X y el direccionamiento del sistema.
* **Capa de Traducción Binaria:** Experimentación con la recompilación dinámica (JIT) de ARM64 a x86_64.
* **Implementación de API:** Mapeo experimental de funciones NVN2 hacia Vulkan 1.3.

## Especificaciones Técnicas (Target)

| Componente | Especificación |
| :--- | :--- |
| SoC | Nvidia T239 (Custom Ampere) |
| CPU | 8x ARM Cortex-A78C |
| GPU | 1536 CUDA Cores |
| API Gráfica | Vulkan 1.3 / NVN2 |
| Arquitectura | ARMv8.2-A / ARMv9 (Research) |

## Estado del Desarrollo

El proyecto se encuentra en **Fase 0 (Exploración)**. No existe actualmente un binario ejecutable ni soporte para carga de software comercial. Las actividades actuales se centran en:

1.  Análisis de la estructura de archivos NCA y HFS2.
2.  Desarrollo de un intérprete ARM64 minimalista para pruebas de concepto.
3.  Implementación de un gestor de memoria virtual inicial.

## Requisitos de Contribución

Se aceptan contribuciones de desarrolladores con experiencia demostrable en:

* Lenguajes C++20 y Assembly ARM64.
* Desarrollo de drivers gráficos y API Vulkan.
* Ingeniería inversa de Kernels basados en microservicios.

Para colaborar, realice un Fork del repositorio y envíe un Pull Request detallando los cambios técnicos realizados.

## Licencia

Este proyecto se distribuye bajo fines estrictamente educativos y de preservación. Consulte el archivo `LICENSE` para más detalles.
