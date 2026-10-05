// Atomicos: LDXR/STXR, LDADD, SWP, CAS, LDAR/STLR
    movz x1, #0x9000, lsl #16
    mov  x2, #10
    str  x2, [x1]
1:  ldaxr x3, [x1]
    add   x3, x3, #5
    stlxr w4, x3, [x1]
    cbnz  w4, 1b                // reintentar si falla
    ldr  x5, [x1]               // 15
    mov  x6, #3
    ldadd x6, x7, [x1]          // x7 = 15, mem = 18
    mov  x8, #100
    swp  x8, x9, [x1]           // x9 = 18, mem = 100
    mov  x10, #100
    mov  x11, #200
    cas  x10, x11, [x1]         // mem == 100 -> mem = 200, x10 = 100
    mov  x12, #5
    cas  x12, x11, [x1]         // mem != 5 -> no cambia, x12 = 200
    stlr x2, [x1, #0]
    ldar x13, [x1]              // 10
    brk  #0
