    .code64
    .globl _start
_start:                         # at phys 0x20000, runs at 0xffffffff80020000
    movl $0x1020, %edx
    orl  $0x40, %edx
    movq %cr4, %rcx
    andl %edx, %ecx
    btsl $4, %ecx
    movq %rcx, %cr4
    movl $0x42, %ebx
    hlt
