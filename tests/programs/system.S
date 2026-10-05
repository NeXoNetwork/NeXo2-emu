// SVC y registros de sistema
    mov  x0, #42
    svc  #0x26                  // el test instala un manejador que suma 1 a x0
    mrs  x1, tpidrro_el0
    mrs  x2, cntfrq_el0         // 31.25 MHz (Switch 2)
    mov  x3, #0x1234
    msr  tpidr_el0, x3
    mrs  x4, tpidr_el0
    cmp  x3, x3
    mrs  x5, nzcv               // Z y C activos -> 0x60000000
    dmb  ish
    nop
    brk  #0
