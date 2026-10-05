// CBZ/CBNZ/TBZ/TBNZ y B
    mov  x0, #0
    mov  x1, #0
    cbz  x0, 1f
    mov  x1, #99                // no se ejecuta
1:  mov  x2, #0b100
    tbnz x2, #2, 2f
    mov  x1, #98                // no se ejecuta
2:  tbz  x2, #0, 3f
    mov  x1, #97
3:  cbnz x2, 4f
    mov  x1, #96
4:  b    5f
    mov  x1, #95
5:  mov  x3, #1
    brk  #0
