// LDR/STR en todas sus formas
    movz x1, #0x9000, lsl #16   // x1 = 0x90000000 (zona de datos)
    movz x2, #0x1122
    movk x2, #0x3344, lsl #16
    movk x2, #0x5566, lsl #32
    movk x2, #0x7788, lsl #48
    str  x2, [x1]
    ldr  x3, [x1]               // 0x7788556633441122
    ldrb w4, [x1]               // 0x22
    ldrh w5, [x1, #2]           // 0x3344
    ldr  w6, [x1, #4]           // 0x77885566
    mov  x7, #-2
    str  w7, [x1, #8]
    ldrsw x8, [x1, #8]          // -2
    ldrsb x9, [x1, #8]          // -2
    ldrb w10, [x1, #8]          // 0xFE
    str  x2, [x1, #16]!         // pre-indice: x1 += 16
    ldr  x11, [x1], #8          // post-indice: x1 += 8 tras leer
    mov  x12, #-1
    ldr  x13, [x1, x12, lsl #3] // registro escalado: [x1 - 8]
    ldur x14, [x1, #-8]
    ldr  x15, literal
    stp  w4, w5, [x1, #32]
    ldp  w16, w17, [x1, #32]
    ldpsw x18, x19, [x1, #-16]  // [x1-16] = -2 y 0
    brk  #0
    .p2align 3
literal:
    .quad 0x0123456789abcdef
