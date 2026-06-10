#!/bin/sh
# Build optimized RISC-V gem5 with the CHI protocol (this branch's testbench).
scons --ignore-style build/RISCV/gem5.opt PROTOCOL=CHI -j"$(nproc)"
