// Tests de los mandos emulados (memoria compartida de hid).
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "system.hpp"
#include "hle/input.hpp"

#ifndef NEXO2_TEST_DATA_DIR
#define NEXO2_TEST_DATA_DIR "tests/generated"
#endif

using namespace NeXo2;
using namespace NeXo2::HLE;

namespace {
template <typename T> T Get(const u8* p, size_t off) { T v; std::memcpy(&v, p + off, sizeof(T)); return v; }

// Lee el estado mas reciente de una cola igual que libnx (_hidGetStates con count = 1):
// entrada = (tail + 18 - 1) % 17 = tail.
struct ReadState { u64 sampling; u64 buttons; s32 lx, ly; u32 attributes; u64 count; };
ReadState Latest(const u8* lifo) {
    const u64 count = Get<u64>(lifo, 0x18);
    const u64 tail = Get<u64>(lifo, 0x10);
    const s64 total = count > 0 ? 1 : 0;
    const u64 pos = ((tail + (HidLayout::LIFO_ENTRIES + 1)) - total) % HidLayout::LIFO_ENTRIES;
    const u8* e = lifo + HidLayout::LIFO_STORAGE + pos * HidLayout::LIFO_ENTRY_SIZE;
    return {Get<u64>(e, 0), Get<u64>(e, 0x10), Get<s32>(e, 0x18), Get<s32>(e, 0x1C), Get<u32>(e, 0x28), count};
}
} // namespace

TEST(Input_LifoLikeLibnx) {
    Core::Memory mem;
    std::vector<u8> shm;
    InputState input;
    input.InitSharedMemory(shm);

    const size_t no1 = HidLayout::NPAD;
    const size_t handheld = HidLayout::NPAD + HidLayout::NPAD_HANDHELD * HidLayout::NPAD_ENTRY_SIZE;
    CHECK_EQ(Get<u32>(shm.data(), no1), 1);        // estilo FullKey (mando Pro)
    CHECK_EQ(Get<u32>(shm.data(), handheld), 2);   // estilo Handheld
    CHECK_EQ(Latest(shm.data() + handheld + HidLayout::HANDHELD_LIFO).attributes & 1, 1); // conectado desde el inicio

    // 20 estados: la cola da la vuelta (17 huecos) y siempre se lee el ultimo
    for (u64 i = 0; i < 20; ++i) {
        PadInput pad;
        pad.buttons = i;
        input.Update(shm, mem, 0, pad);
    }
    const auto full = Latest(shm.data() + no1 + HidLayout::FULL_KEY_LIFO);
    const auto hand = Latest(shm.data() + handheld + HidLayout::HANDHELD_LIFO);
    CHECK_EQ(full.buttons, 19);
    CHECK_EQ(hand.buttons, 19);
    CHECK_EQ(full.count, 17);
    CHECK_EQ(full.sampling, 20);
    CHECK_EQ(full.attributes & 1, 1);

    // Las 17 entradas tienen numeros de muestra consecutivos (libnx lo comprueba)
    const u8* lifo = shm.data() + no1 + HidLayout::FULL_KEY_LIFO;
    const u64 tail = Get<u64>(lifo, 0x10);
    bool consecutive = true;
    for (u64 i = 1; i < 17; ++i) {
        const u64 a = Get<u64>(lifo + 0x20 + ((tail + 17 - i) % 17) * 0x30, 0);
        const u64 b = Get<u64>(lifo + 0x20 + ((tail + 17 - i + 1) % 17) * 0x30, 0);
        if (b - a != 1) consecutive = false;
    }
    CHECK(consecutive);
}

TEST(Input_StickButtons) {
    PadInput pad;
    pad.lx = -STICK_MAX; pad.ly = STICK_MAX; pad.ry = -STICK_MAX;
    CHECK_EQ(InputState::StickButtons(pad), NpadButton::StickLLeft | NpadButton::StickLUp | NpadButton::StickRDown);
    pad = {};
    pad.lx = STICK_MAX / 4;   // poca inclinacion: no cuenta
    CHECK_EQ(InputState::StickButtons(pad), 0);
}

TEST(Input_ReachesProgramMemory) {
    // libnx_init.nro mapea la memoria de hid; despues pulsamos A y lo buscamos en su memoria
    std::ifstream f(std::string(NEXO2_TEST_DATA_DIR) + "/libnx_init.nro", std::ios::binary);
    std::vector<u8> nro((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Core::System sys;
    CHECK(sys.LoadNro(nro, "libnx_init.nro"));
    sys.GetCpu().Run(5'000'000);

    u64 hid_addr = 0;
    for (const auto& [base, r] : sys.GetMemory().Regions())
        if (r.name == "compartida: hid") hid_addr = r.base;
    CHECK(hid_addr != 0);

    PadInput pad;
    pad.buttons = NpadButton::A | NpadButton::Plus;
    sys.GetKernel().SetPadInput(pad);

    std::vector<u8> lifo(HidLayout::LIFO_SIZE);
    const u64 handheld = hid_addr + HidLayout::NPAD + HidLayout::NPAD_HANDHELD * HidLayout::NPAD_ENTRY_SIZE;
    sys.GetMemory().ReadBytes(handheld + HidLayout::HANDHELD_LIFO, lifo.data(), lifo.size());
    CHECK_EQ(Latest(lifo.data()).buttons, NpadButton::A | NpadButton::Plus);
}
