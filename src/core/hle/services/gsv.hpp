#pragma once
// Sistema grafico de Switch 2 ("gsv").
//
// En Switch 2 el juego no habla con nvdrv y vi como en Switch 1: pide una sesion a
// ISelfController 2000 y le habla en TIPC. Esa sesion reparte otras:
//
//   202  syncpt  syncpoints: una pagina compartida con sus valores y eventos por "slot"
//   305  vi2     pantalla: capas, presentar imagenes, eventos de vsync (60 Hz)
//   497  gpu     memoria accesible por la GPU, espacios de direcciones, "trabajos"
//   1402 nvsrv   el protocolo de nvdrv de siempre (abrir la GPU, canales...)
//
// Los comandos que todavia no conocemos contestan "OK" con datos a cero y se apuntan una
// vez en el registro (asi se ve que pide el juego). Ver docs/05-switch2-system/native-games.md.
#include <memory>
#include "hle/service.hpp"

namespace NeXo2::HLE {

// La sesion que entrega ISelfController 2000
std::shared_ptr<ServiceObject> CreateGraphicsSystemSession();

} // namespace NeXo2::HLE
