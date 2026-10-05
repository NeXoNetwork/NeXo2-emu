#pragma once
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/types.hpp"

// Objetos del kernel a los que apuntan los "handles".
//
// Un handle es solo un numero (u32) que el programa usa para referirse a algo
// del kernel: una sesion con un servicio, un evento, un hilo... La tabla de
// handles traduce ese numero al objeto real.
namespace NeXo2::HLE {

class ServiceObject;

struct KObject {
    virtual ~KObject() = default;
    virtual const char* TypeName() const = 0;
};

// Estado compartido de una sesion IPC. Varias copias del handle (CloneCurrentObject)
// apuntan al mismo estado.
//
// Si la sesion es un "dominio", un mismo handle da acceso a varios objetos de
// servicio, cada uno con su id (1, 2, 3...). libnx convierte algunas sesiones en
// dominio para ahorrar handles.
struct SessionState {
    std::shared_ptr<ServiceObject> root;     // objeto principal de la sesion
    bool is_domain = false;
    std::map<u32, std::shared_ptr<ServiceObject>> domain_objects;
    u32 next_object_id = 1;

    u32 AddDomainObject(std::shared_ptr<ServiceObject> object) {
        const u32 id = next_object_id++;
        domain_objects[id] = std::move(object);
        return id;
    }
};

// Lado "cliente" de una sesion: lo que tiene el programa para enviar peticiones.
struct KClientSession final : KObject {
    std::shared_ptr<SessionState> state;
    explicit KClientSession(std::shared_ptr<SessionState> s) : state(std::move(s)) {}
    const char* TypeName() const override { return "KClientSession"; }
};

// Objeto "de relleno" para handles que el programa recibe pero que todavia no
// emulamos de verdad (el proceso actual, el hilo principal...).
struct KDummyObject final : KObject {
    std::string name;
    explicit KDummyObject(std::string n) : name(std::move(n)) {}
    const char* TypeName() const override { return "KDummyObject"; }
};

// Evento: un "aviso" que un servicio puede activar (signaled) y el programa esperar.
// libnx los usa por ejemplo para los mensajes del applet.
struct KEvent final : KObject {
    std::string name;
    bool signaled = false;
    explicit KEvent(std::string n, bool s = false) : name(std::move(n)), signaled(s) {}
    const char* TypeName() const override { return "KEvent"; }
};

// Memoria compartida: un bloque que un servicio entrega al programa (hid: estado
// de los mandos; time: el reloj). El programa la mapea con svcMapSharedMemory.
// De momento se copia al mapear (los cambios posteriores del servicio no se ven).
struct KSharedMemory final : KObject {
    std::string name;
    std::vector<u8> data;
    KSharedMemory(std::string n, size_t size) : name(std::move(n)), data(size, 0) {}
    const char* TypeName() const override { return "KSharedMemory"; }
};

class HandleTable {
public:
    // Devuelve el handle nuevo (nunca 0).
    u32 Create(std::shared_ptr<KObject> object) {
        const u32 handle = m_next++;
        m_objects[handle] = std::move(object);
        return handle;
    }

    // Registra un objeto con un handle concreto (por ejemplo el del hilo principal).
    void Insert(u32 handle, std::shared_ptr<KObject> object) { m_objects[handle] = std::move(object); }

    bool Close(u32 handle) { return m_objects.erase(handle) != 0; }

    std::shared_ptr<KObject> Get(u32 handle) const {
        auto it = m_objects.find(handle);
        return it == m_objects.end() ? nullptr : it->second;
    }

    template <typename T>
    std::shared_ptr<T> Get(u32 handle) const { return std::dynamic_pointer_cast<T>(Get(handle)); }

    size_t Count() const { return m_objects.size(); }

    void Clear() {
        m_objects.clear();
        m_next = FIRST_HANDLE;
    }

private:
    static constexpr u32 FIRST_HANDLE = 0x101;
    std::unordered_map<u32, std::shared_ptr<KObject>> m_objects;
    u32 m_next = FIRST_HANDLE;
};

} // namespace NeXo2::HLE
