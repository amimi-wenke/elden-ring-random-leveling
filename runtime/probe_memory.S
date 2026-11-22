    .text
    .globl random_leveling_probe_byte
    .seh_proc random_leveling_probe_byte
random_leveling_probe_byte:
    .seh_handler __C_specific_handler, @except
    .seh_endprologue
.Lread_begin:
    movb (%rcx), %al
.Lread_end:
    movl $1, %eax
    ret
.Lread_error:
    xorl %eax, %eax
    ret
    .seh_handlerdata
    .long 1
    .rva .Lread_begin, .Lread_end, .Lread_filter, .Lread_error
    .text
    .seh_endproc

# The exception scope covers only the probe instruction. Other faults retain
# the game's usual exception handling.
.Lread_filter:
    movq (%rcx), %rax
    movl (%rax), %edx
    xorl %eax, %eax
    cmpl $0xc0000005, %edx
    je .Lhandle
    cmpl $0xc0000006, %edx
    jne .Lreturn
.Lhandle:
    movl $1, %eax
.Lreturn:
    ret
