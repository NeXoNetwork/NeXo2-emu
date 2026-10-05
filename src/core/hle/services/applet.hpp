#pragma once
#include "hle/service.hpp"

// "appletOE": el gestor de applets para aplicaciones (AM = Applet Manager).
// Al arrancar, libnx pide un IApplicationProxy y de el saca varias interfaces
// (estado, ventana, audio...). Referencia: https://switchbrew.org/wiki/Applet_Manager_services
//
//   appletOE
//     └─ OpenApplicationProxy -> IApplicationProxy
//          ├─ 0    ICommonStateGetter    (mensajes, foco, modo portatil/TV)
//          ├─ 1    ISelfController       (ajustes del propio programa)
//          ├─ 2    IWindowController     (AppletResourceUserId, primer plano)
//          ├─ 3    IAudioController
//          ├─ 4    IDisplayController
//          ├─ 11   ILibraryAppletCreator (lanzar teclado, selector de usuario...)
//          ├─ 20   IApplicationFunctions (cosas propias de un juego)
//          └─ 1000 IDebugFunctions
namespace NeXo2::HLE {

// Identificador del "applet" que somos; hid y otros servicios lo piden.
constexpr u64 APPLET_RESOURCE_USER_ID = 0x0000000000000101ULL;

class AppletOE final : public ServiceObject { public: AppletOE(); };
class ApplicationProxy final : public ServiceObject { public: ApplicationProxy(); };
class CommonStateGetter final : public ServiceObject { public: CommonStateGetter(); };
class SelfController final : public ServiceObject { public: SelfController(); };
class WindowController final : public ServiceObject { public: WindowController(); };
class ApplicationFunctions final : public ServiceObject { public: ApplicationFunctions(); };

// Interfaces que de momento no tienen ningun comando: si el programa usa
// alguno, la CPU se para diciendo cual.
class EmptyInterface final : public ServiceObject {
public:
    explicit EmptyInterface(std::string name) : ServiceObject(std::move(name)) {}
};

} // namespace NeXo2::HLE
