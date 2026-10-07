#include "engines.hpp"
#include "common/logger.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

namespace NeXo2::GPU {

namespace {
inline u64 Iova(u32 hi, u32 lo) { return (u64(hi & 0xFF) << 32) | lo; }

// Escribe 'size' bytes de una fila en una superficie block linear (o lineal), en trozos
// que no cruzan un bloque de 16 bytes de un GOB (dentro de el son seguidos).
void WriteRow(GpuMemoryManager& mm, const Surface& s, u32 x_bytes, u32 y, const u8* data, u32 size) {
    if (s.linear) {
        mm.WriteBlock(s.address + u64(y) * s.pitch + x_bytes, data, size);
        return;
    }
    const u32 width_bytes = s.width * s.bytes_per_pixel;
    while (size > 0) {
        const u32 chunk = std::min(size, 16 - (x_bytes % 16));
        mm.WriteBlock(s.address + BlockLinearOffset(x_bytes, y, width_bytes, s.block_height_log2), data, chunk);
        x_bytes += chunk; data += chunk; size -= chunk;
    }
}
void ReadRow(GpuMemoryManager& mm, const Surface& s, u32 x_bytes, u32 y, u8* data, u32 size) {
    if (s.linear) {
        mm.ReadBlock(s.address + u64(y) * s.pitch + x_bytes, data, size);
        return;
    }
    const u32 width_bytes = s.width * s.bytes_per_pixel;
    while (size > 0) {
        const u32 chunk = std::min(size, 16 - (x_bytes % 16));
        mm.ReadBlock(s.address + BlockLinearOffset(x_bytes, y, width_bytes, s.block_height_log2), data, chunk);
        x_bytes += chunk; data += chunk; size -= chunk;
    }
}
} // namespace

// ============================================================================
//  Subir datos del pushbuffer (inline to memory)
// ============================================================================

bool InlineToMemoryState::CallMethod(const u32* r, u32 method, u32 arg) {
    if (method == 0x6C) {            // LaunchDma: empieza una subida
        m_data.clear();
        m_expected = r[0x60] * std::max<u32>(r[0x61], 1);
        if (m_expected == 0) Finish(r);
        return true;
    }
    if (method == 0x6D) {            // LoadInlineData: 4 bytes mas
        const u8* b = reinterpret_cast<const u8*>(&arg);
        m_data.insert(m_data.end(), b, b + 4);
        if (m_data.size() >= m_expected) Finish(r);
        return true;
    }
    return method >= 0x60 && method < 0x6C;   // configuracion: solo se guarda el registro
}

void InlineToMemoryState::Finish(const u32* r) {
    auto& mm = m_gpu.MemoryManager();
    const u32 line = r[0x60], lines = std::max<u32>(r[0x61], 1);
    const u64 dst = Iova(r[0x62], r[0x63]);
    const bool pitch = r[0x6C] & 1;
    m_data.resize(size_t(line) * lines, 0);
    if (pitch) {
        for (u32 y = 0; y < lines; ++y) mm.WriteBlock(dst + u64(y) * r[0x64], m_data.data() + size_t(y) * line, line);
    } else {
        Surface s;
        s.address = dst;
        s.bytes_per_pixel = 1;
        s.width = r[0x66];
        s.block_height_log2 = (r[0x65] >> 4) & 0xF;
        for (u32 y = 0; y < lines; ++y)
            WriteRow(mm, s, r[0x6A], r[0x6B] + y, m_data.data() + size_t(y) * line, line);
    }
    if (((r[0x6C] >> 4) & 3) == 2) m_gpu.Warn("i2msem", "inline-to-memory con semaforo no implementado");
    ++m_gpu.GetStats().copies;
    m_data.clear();
    m_expected = 0;
}

void InlineToMemory::CallMethod(u32 method, u32 arg, bool) {
    if (method < m_regs.size()) m_regs[method] = arg;
    m_i2m.CallMethod(m_regs.data(), method, arg);
}

// ============================================================================
//  Interprete de macros (MME)
//
//  Instruccion de 32 bits:
//    0..2  operacion: 0 ALU, 1 sumar inmediato, 2 insertar campo de bits,
//          3 extraer y desplazar (inmediato), 4 extraer y desplazar (registro),
//          5 leer registro del motor, 7 salto
//    4..6  que hacer con el resultado (ver abajo); en saltos: bit 4 = saltar si != 0,
//          bit 5 = sin hueco de retardo
//    7     salir (tras ejecutar la instruccion siguiente)
//    8..10 registro destino, 11..13 fuente A, 14..16 fuente B
//    14..31 inmediato (con signo); 17..21 operacion ALU / bit origen;
//    22..26 tamano del campo; 27..31 bit destino
//  r0 vale siempre 0; r1 empieza con el primer parametro.
// ============================================================================

void MacroInterpreter::Execute(const std::vector<u32>& code, u32 start, const std::vector<u32>& params,
                               const MethodFn& send, const ReadFn& read) {
    last_error.clear();
    std::array<u32, 8> reg{};
    size_t next_param = 0;
    auto fetch = [&]() -> u32 { return next_param < params.size() ? params[next_param++] : 0; };
    reg[1] = fetch();
    u32 method_addr = 0, method_incr = 0;
    bool carry = false;
    auto set_method = [&](u32 v) { method_addr = v & 0xFFF; method_incr = (v >> 12) & 0x3F; };
    auto do_send = [&](u32 v) { send(method_addr, v); method_addr = (method_addr + method_incr) & 0xFFF; };
    auto set_reg = [&](u32 r, u32 v) { if (r) reg[r] = v; };

    // Ejecuta una instruccion. Devuelve false cuando la macro termina.
    //  - Salto tomado: si no anula el hueco, primero se ejecuta la instruccion siguiente.
    //    Un salto tomado no sale aunque tenga el bit de salida.
    //  - Bit de salida: se ejecuta una instruccion mas (hueco) y se acaba. Dentro de un
    //    hueco el bit de salida no cuenta.
    u32 pc = start;
    u32 steps = 0;
    std::function<bool(bool)> step = [&](bool in_delay_slot) -> bool {
        if (++steps > 1'000'000) { last_error = "macro sin fin (mas de 1 millon de instrucciones)"; return false; }
        if (pc >= code.size()) { last_error = "PC de macro fuera de rango"; return false; }
        const u32 in = code[pc];
        const u32 this_pc = pc++;
        const u32 op = in & 7;
        const u32 result_op = (in >> 4) & 7;
        const bool exit_bit = (in >> 7) & 1;
        const u32 dst = (in >> 8) & 7, ra = (in >> 11) & 7, rb = (in >> 14) & 7;
        const s32 imm = s32(in) >> 14;
        const u32 bf_src = (in >> 17) & 0x1F, bf_size = (in >> 22) & 0x1F, bf_dst = (in >> 27) & 0x1F;
        const u32 mask = bf_size >= 32 ? ~0u : ((1u << bf_size) - 1);

        if (op == 7) {   // salto
            const bool if_not_zero = (in >> 4) & 1, annul = (in >> 5) & 1;
            const bool taken = if_not_zero ? reg[ra] != 0 : reg[ra] == 0;
            if (taken) {
                const u32 target = u32(s32(this_pc) + imm);
                if (!annul && !step(true)) return false;   // hueco de retardo
                pc = target;
                return true;
            }
        } else {
            u32 result = 0;
            switch (op) {
                case 0: {   // ALU
                    const u32 a = reg[ra], b = reg[rb];
                    switch ((in >> 17) & 0x1F) {
                        case 0: { const u64 r = u64(a) + b; result = u32(r); carry = r >> 32; break; }
                        case 1: { const u64 r = u64(a) + b + (carry ? 1 : 0); result = u32(r); carry = r >> 32; break; }
                        case 2: { const u64 r = u64(a) - b; result = u32(r); carry = a >= b; break; }
                        case 3: { const u64 r = u64(a) - b - (carry ? 0 : 1); result = u32(r); carry = !(r >> 63); break; }
                        case 8:  result = a ^ b; break;
                        case 9:  result = a | b; break;
                        case 10: result = a & b; break;
                        case 11: result = a & ~b; break;
                        case 12: result = ~(a & b); break;
                        default: last_error = "operacion ALU de macro desconocida"; return false;
                    }
                    break;
                }
                case 1: result = u32(s32(reg[ra]) + imm); break;
                case 2: {   // insertar: base = A, campo sacado de B
                    const u32 field = (reg[rb] >> bf_src) & mask;
                    result = (reg[ra] & ~(mask << bf_dst)) | (field << bf_dst);
                    break;
                }
                case 3: result = ((reg[ra] >> (reg[rb] & 0x1F)) & mask) << bf_dst; break;   // desplazamiento en B
                case 4: result = ((reg[ra] >> bf_src) & mask) << (reg[rb] & 0x1F); break;   // desplazamiento final en B
                case 5: result = read(reg[ra] + u32(imm)); break;
                default: last_error = "operacion de macro desconocida"; return false;
            }
            switch (result_op) {
                case 0: set_reg(dst, fetch()); break;                                      // ignorar y leer parametro
                case 1: set_reg(dst, result); break;                                       // mover
                case 2: set_reg(dst, result); set_method(result); break;                   // mover y fijar metodo
                case 3: set_reg(dst, fetch()); do_send(result); break;                     // leer parametro y enviar
                case 4: set_reg(dst, result); do_send(result); break;                      // mover y enviar
                case 5: set_reg(dst, fetch()); set_method(result); break;                  // leer parametro y fijar metodo
                case 6: set_reg(dst, result); set_method(result); do_send(fetch()); break; // mover, metodo, enviar parametro
                case 7: set_reg(dst, result); set_method(result); do_send((result >> 12) & 0x3F); break;
            }
        }
        if (exit_bit && !in_delay_slot) {
            step(true);   // hueco de retardo de la salida
            return false;
        }
        return true;
    };
    while (step(false)) {}
}


// ============================================================================
//  3D
// ============================================================================

Maxwell3D::Maxwell3D(Gpu& gpu, u32 /*channel_syncpoint*/) : m_gpu(gpu), m_i2m(gpu), m_raster(gpu) {}

void Maxwell3D::CallMethod(u32 method, u32 arg, bool last) {
    if (method >= NUM_REGS) {   // llamada a macro: metodo par = empezar, impar = mas parametros
        const s32 index = s32((method - NUM_REGS) >> 1);
        if (!(method & 1) || m_macroPending != index) {
            m_macroPending = index;
            m_macroParams.clear();
        }
        m_macroParams.push_back(arg);
        if (last) RunMacro();
        return;
    }
    WriteReg(method, arg);
}

void Maxwell3D::RunMacro() {
    const u32 index = u32(m_macroPending) & 0x7F;
    m_macroPending = -1;
    ++m_gpu.GetStats().macros;
    m_mme.Execute(m_macroCode, m_macroStart[index], m_macroParams,
                  [this](u32 m, u32 v) { WriteReg(m, v); },
                  [this](u32 m) { return m < NUM_REGS ? m_shadow[m] : 0u; });   // el MME lee la shadow RAM
    if (!m_mme.last_error.empty()) m_gpu.Warn("mme:" + m_mme.last_error, "macro " + std::to_string(index) + ": " + m_mme.last_error);
    m_macroParams.clear();
}

void Maxwell3D::WriteReg(u32 method, u32 arg) {
    if (method >= NUM_REGS) { CallMethod(method, arg, true); return; }
    if (method == 0x049) m_shadowMode = arg & 3;                 // MmeShadowRamControl
    else if (m_shadowMode == 3) arg = m_shadow[method];          // replay: valor guardado
    else if (m_shadowMode != 2) m_shadow[method] = arg;          // track: guardar
    m_regs[method] = arg;
    if (method >= 0x60 && method <= 0x6D) { m_i2m.CallMethod(m_regs.data(), method, arg); return; }
    switch (method) {
        case 0x45: m_macroCodePtr = arg; break;
        case 0x46: m_macroCode[m_macroCodePtr++ % m_macroCode.size()] = arg; break;
        case 0x47: m_macroStartPtr = arg; break;
        case 0x48: m_macroStart[m_macroStartPtr++ % m_macroStart.size()] = arg; break;
        case 0x0B2:   // SyncptAction: incrementar un syncpoint (asi avisa deko3d de que termino)
            if ((arg >> 20) & 1) m_gpu.GetSyncpoints().Increment(arg & 0xFFF);
            break;
        case 0x674: ClearBuffers(arg); break;
        case 0x6C3: ReportSemaphore(); break;
        // --- Dibujos (rasterizer.cpp) ---
        case 0x586:   // VertexBeginGl: empieza una primitiva; bits 26/27 = instancia siguiente / misma
            if ((arg >> 26) & 1) ++m_instance;
            else if (!((arg >> 27) & 1)) m_instance = 0;
            break;
        case 0x35E:   // DrawArraysCount: dibujar 'arg' vertices desde DrawArraysFirst
            DoDraw(false, m_regs[0x35D], arg, m_regs[0x586] & 0xFFFF);
            break;
        case 0x5F8:   // DrawElementsCount: dibujar 'arg' indices desde DrawElementsFirst
            DoDraw(true, m_regs[0x5F7], arg, m_regs[0x586] & 0xFFFF);
            break;
        case 0x485: case 0x486:   // DRAW_VERTEX_ARRAY_BEGIN_END_INSTANCE_FIRST/SUBSEQUENT
            m_instance = method == 0x485 ? 0 : m_instance + 1;
            DoDraw(false, arg & 0xFFFF, (arg >> 16) & 0xFFF, arg >> 28);
            break;
        case 0x5F9: case 0x5FA: case 0x5FB: case 0x5FC: case 0x5FD: case 0x5FE: {
            // DRAW_INDEX_BUFFER32/16/8_BEGIN_END_INSTANCE_FIRST/SUBSEQUENT
            const u32 k = method - 0x5F9;
            m_regs[0x5F6] = 2 - (k % 3);   // tamano del indice: 32, 16, 8 bits
            m_instance = k < 3 ? 0 : m_instance + 1;
            DoDraw(true, arg & 0xFFFF, (arg >> 16) & 0xFFF, arg >> 28);
            break;
        }
        default:
            // FirmwareCall[0..31] (0x8C0..0x8DF): llamadas al firmware del motor (por ejemplo la
            // macro WriteHardwareReg de deko3d, que escribe un registro interno de PGRAPH). No
            // tenemos ese firmware: lo damos por hecho y, como el de verdad, ponemos
            // MmeFirmwareArgs[0] (0xD00) = 1, que es lo que la macro espera en un bucle.
            if (method >= 0x8C0 && method < 0x8E0) m_regs[0xD00] = m_shadow[0xD00] = 1;
            else if (method >= 0x8E4 && method <= 0x8F3) LoadConstbuf(arg);
            else if (method >= 0x904 && method < 0x904 + 5 * 8 && ((method - 0x904) & 7) == 0)
                BindConstbuf((method - 0x904) / 8, arg);
            break;
    }
}

Surface Maxwell3D::RenderTarget(u32 index) const {
    const u32 base = 0x200 + index * 0x10;
    Surface s;
    s.address = Iova(m_regs[base], m_regs[base + 1]);
    s.bytes_per_pixel = ColorFormatBytes(m_regs[base + 4]);
    const u32 tile = m_regs[base + 5];
    s.linear = (tile >> 12) & 1;
    if (s.linear) {
        s.pitch = m_regs[base + 2];
        s.width = s.bytes_per_pixel ? s.pitch / s.bytes_per_pixel : 0;
    } else {
        s.width = m_regs[base + 2];
        s.block_height_log2 = (tile >> 4) & 0xF;
    }
    s.height = m_regs[base + 3];
    return s;
}

void Maxwell3D::ClearBuffers(u32 arg) {
    const bool clear_z = arg & 1, clear_s = (arg >> 1) & 1;
    const u32 comp_mask = (arg >> 2) & 0xF;
    const u32 target = (arg >> 6) & 0xF;
    const u32 layer = (arg >> 10) & 0x7FF;
    auto& mm = m_gpu.MemoryManager();

    // Zona a borrar: tamano del destino, recortado por el "screen scissor" y (si se pide) el scissor 0.
    // Con MSAA el destino mide en muestras y los scissors en pixeles.
    u32 msx, msy;
    MsaaSampleGrid(m_regs[0x574], msx, msy);
    auto clip_px = [&](u32 w, u32 h, u32& x0, u32& y0, u32& x1, u32& y1) {
        x0 = 0; y0 = 0; x1 = w; y1 = h;
        const u32 sh = m_regs[0x3FD], sv = m_regs[0x3FE];
        if (sh >> 16) { x0 = std::max(x0, sh & 0xFFFF); x1 = std::min(x1, (sh & 0xFFFF) + (sh >> 16)); }
        if (sv >> 16) { y0 = std::max(y0, sv & 0xFFFF); y1 = std::min(y1, (sv & 0xFFFF) + (sv >> 16)); }
        if (((m_regs[0x43E] >> 8) & 1) && (m_regs[0x380] & 1)) {
            const u32 h0 = m_regs[0x381], v0 = m_regs[0x382];
            x0 = std::max(x0, h0 & 0xFFFF); x1 = std::min(x1, h0 >> 16);
            y0 = std::max(y0, v0 & 0xFFFF); y1 = std::min(y1, v0 >> 16);
        }
    };
    auto clip = [&](u32 w, u32 h, u32& x0, u32& y0, u32& x1, u32& y1) {
        clip_px(w / msx, h / msy, x0, y0, x1, y1);
        x0 *= msx; x1 *= msx; y0 *= msy; y1 *= msy;
    };

    if (comp_mask && target < 8) {
        Surface rt = RenderTarget(target);
        const u32 format = m_regs[0x200 + target * 0x10 + 4];
        u8 px[16];
        float color[4];
        std::memcpy(color, &m_regs[0x360], 16);
        if (rt.address && rt.bytes_per_pixel && EncodeColor(format, color, px)) {
            rt.address += u64(layer) * u64(m_regs[0x200 + target * 0x10 + 7]) * 4;
            u32 x0, y0, x1, y1;
            clip(rt.width, rt.height, x0, y0, x1, y1);
            const u32 bpp = rt.bytes_per_pixel;
            const u32 byte_mask = ColorWriteByteMask(format, comp_mask);
            const bool full = byte_mask == (1u << bpp) - 1;
            if (x1 > x0 && y1 > y0) {
                std::vector<u8> row(size_t(x1 - x0) * bpp);
                for (u32 y = y0; y < y1; ++y) {
                    if (!full) ReadRow(mm, rt, x0 * bpp, y, row.data(), u32(row.size()));
                    for (u32 x = 0; x < x1 - x0; ++x)
                        for (u32 b = 0; b < bpp; ++b)
                            if (byte_mask & (1u << b)) row[size_t(x) * bpp + b] = px[b];
                    WriteRow(mm, rt, x0 * bpp, y, row.data(), u32(row.size()));
                }
            }
            ++m_gpu.GetStats().clears;
        } else if (rt.address) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "borrado: formato de color 0x%X no soportado", format);
            m_gpu.Warn("rtfmt" + std::to_string(format), buf);
        }
    }

    if ((clear_z || clear_s) && (m_regs[0x54E] & 1)) {
        const u32 format = m_regs[0x3FA];
        Surface zt;
        zt.address = Iova(m_regs[0x3F8], m_regs[0x3F9]) + u64(layer) * u64(m_regs[0x3FC]) * 4;
        zt.bytes_per_pixel = DepthFormatBytes(format);
        zt.width = m_regs[0x48A];
        zt.height = m_regs[0x48B];
        zt.block_height_log2 = (m_regs[0x3FB] >> 4) & 0xF;
        float depth;
        std::memcpy(&depth, &m_regs[0x364], 4);
        const u8 stencil = u8(m_regs[0x368]);
        if (zt.address && zt.bytes_per_pixel) {
            u32 x0, y0, x1, y1;
            clip(zt.width, zt.height, x0, y0, x1, y1);
            const u32 bpp = zt.bytes_per_pixel;
            if (x1 > x0 && y1 > y0) {
                std::vector<u8> row(size_t(x1 - x0) * bpp);
                for (u32 y = y0; y < y1; ++y) {
                    ReadRow(mm, zt, x0 * bpp, y, row.data(), u32(row.size()));
                    for (u32 x = 0; x < x1 - x0; ++x)
                        EncodeDepthStencil(format, depth, stencil, clear_z, clear_s, &row[size_t(x) * bpp]);
                    WriteRow(mm, zt, x0 * bpp, y, row.data(), u32(row.size()));
                }
            }
            ++m_gpu.GetStats().clears;
        }
    }
}

void Maxwell3D::ReportSemaphore() {
    const u32 ctl = m_regs[0x6C3];
    const u64 addr = Iova(m_regs[0x6C0], m_regs[0x6C1]);
    auto& mm = m_gpu.MemoryManager();
    const u32 op = ctl & 3;
    const bool one_word = (ctl >> 28) & 1;
    if (op == 0) {            // release: escribe el valor
        mm.Write<u32>(addr, m_regs[0x6C2]);
        if (!one_word) { mm.Write<u32>(addr + 4, 0); mm.Write<u64>(addr + 8, 0); }
    } else if (op == 2) {     // counter: contador (0: no contamos nada) + marca de tiempo
        const u32 counter = (ctl >> 23) & 0x1F;
        mm.Write<u32>(addr, counter == 0 ? m_regs[0x6C2] : 0);
        if (!one_word) { mm.Write<u32>(addr + 4, 0); mm.Write<u64>(addr + 8, 0); }
    }
    // acquire / trap: la GPU es sincrona, no hay nada que esperar
}

void Maxwell3D::BindConstbuf(u32 stage, u32 arg) {
    // Enlaza el constbuf elegido con ConstbufSelector (0x8E0..0x8E2) al hueco c[indice]
    const u32 index = (arg >> 4) & 0x1F;
    if (stage >= 5 || index >= 18) return;
    ConstbufBinding& b = m_constbufs[stage][index];
    b.valid = arg & 1;
    b.address = Iova(m_regs[0x8E1], m_regs[0x8E2]);
    b.size = m_regs[0x8E0] & 0x1FFFF;
}

void Maxwell3D::DoDraw(bool indexed, u32 first, u32 count, u32 topology) {
    if (count == 0) return;
    DrawCall dc;
    dc.topology = topology;
    dc.indexed = indexed;
    dc.first = first;
    dc.count = count;
    dc.vertex_offset = s32(m_regs[0x446]);                            // VertexIdBase
    dc.vertex_id_base = ((m_regs[0x593] >> 12) & 1) ? first : 0;      // DrawArraysAddStart
    dc.instance = m_instance;
    dc.base_instance = m_regs[0x50E];                                 // DrawBaseInstance
    m_raster.Draw(m_regs.data(), m_constbufs, dc);
}

void Maxwell3D::LoadConstbuf(u32 arg) {
    // ConstbufSelectorSize (0x8E0), Addr (0x8E1/0x8E2), offset (0x8E3) que avanza 4 bytes
    const u64 base = Iova(m_regs[0x8E1], m_regs[0x8E2]);
    const u32 offset = m_regs[0x8E3];
    m_gpu.MemoryManager().Write<u32>(base + offset, arg);
    m_regs[0x8E3] = offset + 4;
}

// ============================================================================
//  Copias (DMA)
// ============================================================================

void MaxwellDma::CallMethod(u32 method, u32 arg, bool) {
    if (method < m_regs.size()) m_regs[method] = arg;
    if (method == 0xC0) Launch(arg);
}

void MaxwellDma::Launch(u32 arg) {
    auto& mm = m_gpu.MemoryManager();
    const bool src_pitch = (arg >> 7) & 1, dst_pitch = (arg >> 8) & 1;
    const bool multiline = (arg >> 9) & 1, remap = (arg >> 10) & 1;
    const u64 src = Iova(m_regs[0x100], m_regs[0x101]);
    const u64 dst = Iova(m_regs[0x102], m_regs[0x103]);
    const u32 len = m_regs[0x106];
    const u32 lines = multiline ? std::max<u32>(m_regs[0x107], 1) : 1;

    // Elementos: sin remap son bytes; con remap, componentes de 1-4 bytes
    const u32 rc = m_regs[0x1C2];
    const u32 comp_size = remap ? ((rc >> 16) & 3) + 1 : 1;
    const u32 src_comps = remap ? ((rc >> 20) & 3) + 1 : 1;
    const u32 dst_comps = remap ? ((rc >> 24) & 3) + 1 : 1;
    const u32 src_elem = comp_size * src_comps, dst_elem = comp_size * dst_comps;

    auto make_surface = [&](bool pitch, u64 addr, u32 base, u32 elem, u32 pitch_reg) {
        Surface s;
        s.address = addr;
        s.bytes_per_pixel = 1;
        s.linear = pitch;
        s.pitch = m_regs[pitch_reg];
        s.width = m_regs[base + 1] * elem;            // ancho en bytes
        s.height = m_regs[base + 2];
        s.block_height_log2 = (m_regs[base] >> 4) & 0xF;
        return s;
    };
    const Surface ss = make_surface(src_pitch, src, 0x1CA, src_elem, 0x104);
    const Surface ds = make_surface(dst_pitch, dst, 0x1C3, dst_elem, 0x105);
    const u32 sx = (m_regs[0x1CF] & 0xFFFF) * src_elem, sy = m_regs[0x1CF] >> 16;
    const u32 dx = (m_regs[0x1C8] & 0xFFFF) * dst_elem, dy = m_regs[0x1C8] >> 16;

    std::vector<u8> in(size_t(len) * src_elem), out(size_t(len) * dst_elem);
    for (u32 y = 0; y < lines; ++y) {
        // Leer la fila
        bool need_src = !remap;
        if (remap)
            for (u32 c = 0; c < dst_comps; ++c) need_src |= ((rc >> (c * 4)) & 7) < 4;
        if (need_src) {
            if (src_pitch) mm.ReadBlock(src + u64(y) * ss.pitch, in.data(), in.size());
            else           ReadRow(mm, ss, sx, sy + y, in.data(), u32(in.size()));
        }
        // Reordenar componentes / constantes
        if (!remap) out = in;
        else {
            for (u32 x = 0; x < len; ++x)
                for (u32 c = 0; c < dst_comps; ++c) {
                    const u32 sel = (rc >> (c * 4)) & 7;
                    u8* o = &out[size_t(x) * dst_elem + c * comp_size];
                    if (sel < 4)      std::memcpy(o, &in[size_t(x) * src_elem + std::min(sel, src_comps - 1) * comp_size], comp_size);
                    else if (sel < 6) std::memcpy(o, &m_regs[0x1C0 + (sel - 4)], comp_size);
                    // 6 = no escribir: se queda lo que hubiera (se lee abajo si hace falta)
                }
        }
        if (dst_pitch) mm.WriteBlock(dst + u64(y) * ds.pitch, out.data(), out.size());
        else           WriteRow(mm, ds, dx, dy + y, out.data(), u32(out.size()));
    }
    ++m_gpu.GetStats().copies;

    // Semaforo al terminar (bits 3..4: 1 = una palabra, 2 = cuatro)
    const u32 sem = (arg >> 3) & 3;
    if (sem) {
        const u64 addr = Iova(m_regs[0x90], m_regs[0x91]);
        mm.Write<u32>(addr, m_regs[0x92]);
        if (sem == 2) { mm.Write<u32>(addr + 4, 0); mm.Write<u64>(addr + 8, 0); }
    }
}

// ============================================================================
//  2D: copiar un rectangulo de una imagen a otra (con escalado por punto)
// ============================================================================

void Fermi2D::CallMethod(u32 method, u32 arg, bool) {
    if (method < m_regs.size()) m_regs[method] = arg;
    if (method == 0x237) Blit();
}

void Fermi2D::Blit() {
    auto& mm = m_gpu.MemoryManager();
    auto surface = [&](u32 base) {   // base 0x80 = destino, 0x8C = origen (misma disposicion)
        Surface s;
        const bool dst = base == 0x80;
        s.bytes_per_pixel = std::max<u32>(ColorFormatBytes(m_regs[base]), 1);
        s.linear = m_regs[base + 1] & 1;
        s.block_height_log2 = (m_regs[base + 2] >> 4) & 7;
        s.pitch = m_regs[dst ? 0x85 : 0x91];
        s.width = m_regs[dst ? 0x86 : 0x92];
        s.height = m_regs[dst ? 0x87 : 0x93];
        s.address = dst ? Iova(m_regs[0x88], m_regs[0x89]) : Iova(m_regs[0x94], m_regs[0x95]);
        return s;
    };
    const Surface d = surface(0x80), s = surface(0x8C);
    const u32 dfmt = m_regs[0x80], sfmt = m_regs[0x8C];
    if (d.bytes_per_pixel != s.bytes_per_pixel) {
        m_gpu.Warn("2dfmt", "2D: copia entre formatos de distinto tamano no soportada");
        return;
    }
    // Cambiar R y B entre RGBA8 y BGRA8
    const bool rgba = (dfmt == 0xD5 || dfmt == 0xD6 || dfmt == 0xF9) , bgra_s = (sfmt == 0xCF || sfmt == 0xD0 || sfmt == 0xE6);
    const bool bgra = (dfmt == 0xCF || dfmt == 0xD0 || dfmt == 0xE6), rgba_s = (sfmt == 0xD5 || sfmt == 0xD6 || sfmt == 0xF9);
    const bool swap_rb = (rgba && bgra_s) || (bgra && rgba_s);

    const s64 dudx = (s64(s32(m_regs[0x231])) << 32) | m_regs[0x230];
    const s64 dvdy = (s64(s32(m_regs[0x233])) << 32) | m_regs[0x232];
    const s64 u0 = (s64(s32(m_regs[0x235])) << 32) | m_regs[0x234];
    const s64 v0 = (s64(s32(m_regs[0x237])) << 32) | m_regs[0x236];
    const u32 dx0 = m_regs[0x22C], dy0 = m_regs[0x22D], w = m_regs[0x22E], h = m_regs[0x22F];
    const u32 bpp = d.bytes_per_pixel;
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return;

    std::vector<u8> srow(size_t(s.width) * bpp), drow(size_t(w) * bpp);
    for (u32 y = 0; y < h; ++y) {
        const s64 sv = (v0 + s64(y) * dvdy) >> 32;
        if (sv < 0 || sv >= s64(s.height) || dy0 + y >= d.height) continue;
        ReadRow(mm, s, 0, u32(sv), srow.data(), u32(srow.size()));
        for (u32 x = 0; x < w; ++x) {
            s64 su = (u0 + s64(x) * dudx) >> 32;
            su = std::clamp<s64>(su, 0, s64(s.width) - 1);
            std::memcpy(&drow[size_t(x) * bpp], &srow[size_t(su) * bpp], bpp);
            if (swap_rb) std::swap(drow[size_t(x) * bpp], drow[size_t(x) * bpp + 2]);
        }
        const u32 cw = std::min(w, d.width > dx0 ? d.width - dx0 : 0);
        if (cw) WriteRow(mm, d, dx0 * bpp, dy0 + y, drow.data(), cw * bpp);
    }
    ++m_gpu.GetStats().copies;
}

// ============================================================================
//  Compute (fase 1)
// ============================================================================

void KeplerCompute::CallMethod(u32 method, u32 arg, bool) {
    if (method < m_regs.size()) m_regs[method] = arg;
    if (m_i2m.CallMethod(m_regs.data(), method, arg)) return;
    if (method == 0xAF) {   // LAUNCH: necesita shaders
        m_gpu.Warn("compute", "lanzamiento de compute ignorado: los shaders llegan en la fase 2");
        ++m_gpu.GetStats().draws_skipped;
    }
}

} // namespace NeXo2::GPU
