// Procesado con registros: CSEL, CCMP, MUL, DIV, MULH, CLZ, REV...
    mov  x0, #100
    mov  x1, #7
    udiv x2, x0, x1             // 14
    msub x3, x2, x1, x0         // 100 - 14*7 = 2 (modulo)
    mov  x4, #-100
    sdiv x5, x4, x1             // -14
    udiv x6, x0, xzr            // division entre 0 = 0
    mul  x7, x0, x1             // 700
    mov  x8, #-1
    umulh x9, x8, x8            // 0xFFFFFFFFFFFFFFFE
    smulh x10, x8, x8           // 0
    clz  x11, x1                // 61
    movz x12, #0x0102
    movk x12, #0x0304, lsl #16
    rev  w13, w12               // 0x02010403
    cmp  x0, x1
    csel x14, x0, x1, gt        // 100
    csinc x15, x0, x1, lt       // 8
    cneg x16, x1, hi            // -7
    ccmp x1, #7, #0, hi         // x0>x1 -> compara x1 con 7 -> EQ
    cset x17, eq                // 1
    orr  x18, xzr, x0           // mov x18, x0
    bic  x19, x0, x1            // 100 & ~7 = 96
    add  x20, x0, x1, lsl #2    // 128
    sub  x21, x0, w1, uxtw      // 93
    lsl  x22, x1, x1            // lslv: 7 << 7 = 896
    adds x23, x8, #1            // -1 + 1 = 0 -> C=1, Z=1
    adc  x24, xzr, xzr          // 1 (acarreo)
    brk  #0
