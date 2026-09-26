    .code32
    .globl _start
_start:                         # at phys 0x10000
    movl $0x20, %eax            # CR4.PAE
    movl %eax, %cr4
    movl $0x1000, %eax
    movl %eax, %cr3
    movl $0xC0000080, %ecx
    rdmsr
    orl  $0x100, %eax           # LME
    wrmsr
    movl %cr0, %eax
    orl  $0x80000001, %eax
    movl %eax, %cr0
    ljmp $0x08, $(0x10000 + (long64 - _start))
    .code64
long64:
    movabs $0xffffffff80020000, %rax
    jmp *%rax
