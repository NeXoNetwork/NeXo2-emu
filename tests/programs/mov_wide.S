// MOVZ / MOVK / MOVN en 64 y 32 bits
    movz x0, #0x1234
    movk x0, #0xabcd, lsl #16
    movk x0, #0x5678, lsl #48
    movn x1, #0                 // x1 = 0xFFFF_FFFF_FFFF_FFFF
    mov  x2, #-1
    movn w2, #0                 // escribir W pone a cero la parte alta
    movz w3, #0xffff, lsl #16
    brk #0
