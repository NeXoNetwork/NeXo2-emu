#pragma once
// Si NeXo revienta (acceso a memoria invalido, abort, excepcion de C++...), escribe al final
// de nexo2.log que paso y donde: codigo de la excepcion, la pila de llamadas (con nombres de
// funcion y lineas si esta el .pdb junto al .exe) y lo que diga 'context' (juego, PC de cada
// nucleo emulado...). Asi un cierre de golpe se puede investigar despues.
#include <functional>
#include <string>

namespace NeXo2::Frontend {

void InstallCrashReport(const std::string& log_path, std::function<std::string()> context);

} // namespace NeXo2::Frontend
