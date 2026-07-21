.section .text.init
.globl _start
_start:
    li sp, 0x02100000
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la t0, __bss_start
    la t1, __bss_end
1:
    bgeu t0, t1, 2f
    sd zero, 0(t0)
    addi t0, t0, 8
    j 1b
2:
    call main
3:
    wfi
    j 3b
