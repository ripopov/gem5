# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
#
# Entry point for the bare-metal CoreMark image on gem5's RISC-V full-system
# platform. The RiscvBareMetal workload starts every hart at the ELF entry in
# M-mode with nothing set up; this establishes the C environment, runs main and
# hands its return value to gem5 as the simulation exit code.

  .section .text.init, "ax", @progbits
  .globl _start
_start:
  # Park every hart except the first rather than let them race through main.
  csrr  t0, mhartid
  bnez  t0, park

.option push
.option norelax
  la    gp, __global_pointer$
.option pop
  la    sp, _stack_top

  # mstatus.FS comes up Off; CoreMark's result printing uses the FPU.
  # mstatus.VS likewise, for images built with the vector extension.
  li    t0, 0x6600              # FS = Dirty, VS = Dirty
  csrs  mstatus, t0

  la    t0, _bss_start
  la    t1, _bss_end
1:
  bgeu  t0, t1, 2f
  sd    zero, 0(t0)
  addi  t0, t0, 8
  j     1b
2:
  call  main
  call  gem5_exit               # exit status is main's return value in a0

park:
  wfi
  j     park
