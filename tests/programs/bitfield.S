// Inmediatos logicos y bitfield (LSL/LSR/ASR/UBFX/SBFX/BFI/SXTW/UXTB)
    movz x0, #0xbeef
    movk x0, #0xdead, lsl #16
    movk x0, #0x8000, lsl #48   // x0 = 0x80000000DEADBEEF
    and  x1, x0, #0xff          // 0xEF
    orr  x2, xzr, #0xaaaaaaaaaaaaaaaa
    eor  x3, x0, #0xffff0000ffff0000
    lsl  x4, x0, #4
    lsr  x5, x0, #60            // 8
    asr  x6, x0, #60            // 0xFFFFFFFFFFFFFFF8
    ubfx x7, x0, #8, #8         // 0xBE
    sbfx x8, x0, #12, #4        // 0xB -> negativo: -5
    mov  x9, #0
    bfi  x9, x0, #16, #8        // 0xEF0000
    sxtw x10, w0                // 0xFFFFFFFFDEADBEEF
    uxtb w11, w0                // 0xEF
    sxth x12, w0                // 0xFFFFFFFFFFFFBEEF
    ror  x13, x0, #4
    tst  x0, #0x8000000000000000
    cset x14, ne                // 1
    brk  #0
