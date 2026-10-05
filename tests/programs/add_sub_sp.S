// ADD/SUB inmediato, el registro 31 como SP y 32 bits
    mov  x0, sp                 // add x0, sp, #0
    sub  sp, sp, #0x20
    mov  x1, sp
    add  x2, x1, #1, lsl #12    // x2 = x1 + 0x1000
    mov  w3, #0xffffffff
    add  w3, w3, #2             // desborda en 32 bits -> 1
    add  sp, sp, #0x20
    brk #0
