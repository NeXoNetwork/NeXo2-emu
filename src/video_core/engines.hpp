#pragma once
// Los motores ("engines" o "clases") de la GPU Maxwell. Cada uno tiene un banco de
// registros; el pushbuffer escribe en ellos ("metodos") y algunas escrituras disparan
// una accion (borrar, copiar, liberar un semaforo...).
//
//   clase 0xB197  Maxwell3D        dibujo 3D: render targets, borrados, macros, semaforos
//   clase 0xB0B5  MaxwellDma       copias de memoria (lineal <-> block linear) y rellenos
//   clase 0x902D  Fermi2D          copias de imagenes con escalado (blits)
//   clase 0xB1C0  KeplerCompute    compute (fase 1: solo guarda registros y sube datos)
//   clase 0xA140  InlineToMemory   subir datos que van dentro del propio pushbuffer
//
// Los numeros de los metodos son los de los registros (direccion / 4). Fuente de los
// nombres y campos: los ficheros .def de deko3d (codigo abierto) y la documentacion
// publica de NVIDIA (open-gpu-doc).
#include <array>
#include <vector>
#include "gpu.hpp"
#include "rasterizer.hpp"
#include "surface.hpp"

namespace NeXo2::GPU {

// ----------------------------------------------------------------------------
// Subir datos del pushbuffer a memoria (metodos 0x60..0x6D, iguales en 3D, compute e inline)
// ----------------------------------------------------------------------------
class InlineToMemoryState {
public:
    explicit InlineToMemoryState(Gpu& gpu) : m_gpu(gpu) {}
    // Devuelve true si el metodo era suyo
    bool CallMethod(const u32* regs, u32 method, u32 arg);
private:
    void Finish(const u32* regs);
    Gpu& m_gpu;
    std::vector<u8> m_data;
    u32 m_expected = 0;   // bytes que faltan
};

class InlineToMemory final : public Engine {
public:
    explicit InlineToMemory(Gpu& gpu) : m_i2m(gpu) {}
    void CallMethod(u32 method, u32 arg, bool last) override;
    const char* Name() const override { return "Inline"; }
private:
    std::array<u32, 0x80> m_regs{};
    InlineToMemoryState m_i2m;
};

// ----------------------------------------------------------------------------
// Macros (MME): pequenos programas que se suben a la GPU y que escriben metodos.
// deko3d las usa, por ejemplo, para borrar todas las capas de un render target.
// ----------------------------------------------------------------------------
class MacroInterpreter {
public:
    using MethodFn = std::function<void(u32 method, u32 value)>;
    using ReadFn = std::function<u32(u32 method)>;
    // Ejecuta el codigo desde 'start' con los parametros dados
    void Execute(const std::vector<u32>& code, u32 start, const std::vector<u32>& params,
                 const MethodFn& send, const ReadFn& read);
    std::string last_error;   // vacio = bien
};

// ----------------------------------------------------------------------------
// 3D
// ----------------------------------------------------------------------------
class Maxwell3D final : public Engine {
public:
    static constexpr u32 NUM_REGS = 0xE00;
    explicit Maxwell3D(Gpu& gpu, u32 channel_syncpoint);
    void CallMethod(u32 method, u32 arg, bool last) override;
    const char* Name() const override { return "3D"; }

    u32 Reg(u32 method) const { return method < NUM_REGS ? m_regs[method] : 0; }

private:
    void WriteReg(u32 method, u32 arg);
    void ClearBuffers(u32 arg);
    void ReportSemaphore();
    void LoadConstbuf(u32 arg);
    void RunMacro();
    Surface RenderTarget(u32 index) const;
    void BindConstbuf(u32 stage, u32 arg);
    void DoDraw(bool indexed, u32 first, u32 count, u32 topology);

    Gpu& m_gpu;
    std::array<u32, NUM_REGS> m_regs{};
    // "Shadow RAM": copia de los registros que lee el MME (macros). MmeShadowRamControl
    // (0x049) dice si una escritura la actualiza (0/1), no (2, "passthrough") o si en vez del
    // valor enviado se usa el guardado (3, "replay": asi deko3d restaura el estado).
    std::array<u32, NUM_REGS> m_shadow{};
    u32 m_shadowMode = 0;
    InlineToMemoryState m_i2m;
    // Macros
    std::vector<u32> m_macroCode = std::vector<u32>(0x2000, 0);   // memoria de instrucciones
    std::array<u32, 0x80> m_macroStart{};                         // inicio de cada macro
    u32 m_macroCodePtr = 0, m_macroStartPtr = 0;
    s32 m_macroPending = -1;                                      // macro que espera parametros
    std::vector<u32> m_macroParams;
    MacroInterpreter m_mme;
    // Dibujo
    ConstbufTable m_constbufs{};   // constbufs enlazados a cada etapa (BindGroup)
    SoftwareRasterizer m_raster;
    u32 m_instance = 0;            // gl_InstanceID del dibujo actual
};

// ----------------------------------------------------------------------------
// Copias (DMA)
// ----------------------------------------------------------------------------
class MaxwellDma final : public Engine {
public:
    explicit MaxwellDma(Gpu& gpu) : m_gpu(gpu) {}
    void CallMethod(u32 method, u32 arg, bool last) override;
    const char* Name() const override { return "DMA"; }
private:
    void Launch(u32 arg);
    Gpu& m_gpu;
    std::array<u32, 0x200> m_regs{};
};

// ----------------------------------------------------------------------------
// 2D (blits)
// ----------------------------------------------------------------------------
class Fermi2D final : public Engine {
public:
    explicit Fermi2D(Gpu& gpu) : m_gpu(gpu) {}
    void CallMethod(u32 method, u32 arg, bool last) override;
    const char* Name() const override { return "2D"; }
private:
    void Blit();
    Gpu& m_gpu;
    std::array<u32, 0x300> m_regs{};
};

// ----------------------------------------------------------------------------
// Compute (fase 1: guarda registros, sube datos; los lanzamientos se ignoran con aviso)
// ----------------------------------------------------------------------------
class KeplerCompute final : public Engine {
public:
    explicit KeplerCompute(Gpu& gpu) : m_gpu(gpu), m_i2m(gpu) {}
    void CallMethod(u32 method, u32 arg, bool last) override;
    const char* Name() const override { return "Compute"; }
private:
    Gpu& m_gpu;
    std::array<u32, 0x1000> m_regs{};
    InlineToMemoryState m_i2m;
};

} // namespace NeXo2::GPU
