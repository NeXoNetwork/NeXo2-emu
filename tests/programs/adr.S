// ADR / ADRP
    adr  x0, label              // PC + 8
    adrp x1, label              // pagina de 4 KB
    nop
label:
    brk  #0
