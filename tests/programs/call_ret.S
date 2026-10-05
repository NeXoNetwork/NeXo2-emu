// BL / RET: llamada a funcion con la pila (STP/LDP)
    mov  x0, #6
    bl   square
    mov  x19, x0                // 36
    mov  x0, #7
    adr  x9, square
    blr  x9
    mov  x20, x0                // 49
    brk  #0
square:
    paciasp                     // Pointer Authentication: NOP para nosotros
    stp  x29, x30, [sp, #-16]!
    mov  x29, sp
    mul  x0, x0, x0
    ldp  x29, x30, [sp], #16
    autiasp
    ret
