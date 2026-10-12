#pragma once
// Servicios que piden los juegos comerciales al arrancar y que los homebrew casi nunca usan:
//
//   lm        registro de mensajes del juego (van a nexo2.log)
//   acc:u0/1  cuentas de usuario: hay un usuario, "NeXo"
//   pctl*     control parental: sin restricciones
//   friends   amigos: ninguno, sin conexion
//   prepo     informes de juego para Nintendo: se aceptan y se tiran
//   aoc:u     contenido descargable (DLC): ninguno
//   fatal:u   error fatal del juego: para la emulacion con el codigo de error
//
// Varias de estas interfaces son "permisivas" (LenientService): un comando que no conocemos
// contesta OK sin datos (y se apunta una vez en el registro) en vez de parar el juego.
// Ver docs/07-nexo-internals/games.md.
#include "hle/service.hpp"

namespace NeXo2::HLE {

class LmService final : public ServiceObject { public: LmService(); };
class AccountService final : public ServiceObject { public: explicit AccountService(std::string name); };
class PctlService final : public LenientService { public: explicit PctlService(std::string name); };
class FriendsService final : public LenientService { public: explicit FriendsService(std::string name); };
class PrepoService final : public LenientService { public: explicit PrepoService(std::string name); };
class AocService final : public LenientService { public: AocService(); };
class FatalService final : public ServiceObject { public: FatalService(); };
// ectx:aw: contexto de errores (17.0.0+). CreateContextRegistrar -> objeto que acepta todo
class ErrorContextWriter final : public LenientService { public: ErrorContextWriter(); };

// Usuario unico (UID que ven los juegos)
constexpr u64 USER_ID_LO = 0x4E65586F55736572ULL;   // "NeXoUser"
constexpr u64 USER_ID_HI = 0x0000000000000001ULL;

} // namespace NeXo2::HLE
