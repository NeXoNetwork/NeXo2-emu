// Bucle con SUBS + B.NE: suma 1..10 = 55
    mov  x0, #0
    mov  x1, #10
1:  add  x0, x0, x1
    subs x1, x1, #1
    b.ne 1b
    // comparaciones con signo
    mov  x2, #-5
    cmp  x2, #3
    cset x3, lt                 // -5 < 3  -> 1
    cset x4, lo                 // sin signo: 0xFFF..FB < 3 -> 0
    cmp  x2, #-5
    cset x5, eq                 // 1
    brk #0
