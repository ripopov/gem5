# Fast Linux Boot to Cycle-Accurate RTL CPU Simulation

## Executive summary

This proof of concept combines two complementary CPU models in one gem5
simulation. `RiscvJitCPU` boots a full RV64 Linux image quickly using a pinned
QEMU/TCG backend. At a controlled userspace boundary, gem5 drains the fast
CPU, transfers its live architectural state, and resumes the same process on
the Verilated PULP C910 RTL CPU.

The end-to-end demonstration proves more than continued memory traffic. Code
running after the handover makes Linux system calls, prints
`Hello world from Linux on the C910 RTL CPU!` through the modeled 8250 UART,
transmits EOT, and stops the simulation automatically. This makes long OS boot
sequences practical while preserving cycle-accurate RTL execution in the
region that matters.

## How it works

The execution path is:

`Linux on JitCPU` → `m5_exit boundary` → `architectural-state import` →
`Linux process on C910 RTL` → `UART hello and EOT`

JitCPU uses QEMU's RISC-V translator as an optional shared-library backend.
Ordinary RAM uses the fast path, while MMIO and gem5 pseudo-instructions
return to gem5. The CPU remains a normal gem5 `BaseCPU`, so the standard
drain and `m5.switchCpus()` machinery controls the transition.

`RtlCpuSimObject` wraps the vendor-neutral `RtlCoreSimObject`. The canonical
`riscv64/v1` state schema transfers PC, integer and floating-point registers,
privilege state, machine and supervisor CSRs, PMP, and `satp`. C910 imports
that state through its vendor adapter and HAD/JTAG implementation, then
continues at the instruction immediately following the switch marker.

The generic RTL-cosim layer handles the C910's 128-bit AXI4 initiator. Masked
writes are converted to gem5 packets covering only active byte lanes. The
demo platform uses the standard device-tree `reg-shift` property for the
byte-wide 8250 registers, allowing full-width AXI reads to select one register
without C910-specific logic in the transaction backend. Architectural
interrupt numbers are mapped to discovered RTL inputs; both live changes and
levels already pending at takeover are forwarded.

## Build

Required tools include a C++20 compiler, CMake, SCons, Ninja, Verilator, and a
RISC-V Linux cross compiler such as `riscv64-linux-gnu-gcc`.

Initialize the pinned QEMU and C910 dependencies:

```sh
git submodule update --init \
  ext/qemu/repo \
  ext/rtl/pulp-c910/repo \
  ext/rtl/pulp/axi \
  ext/rtl/pulp/common_cells \
  ext/rtl/pulp/tech_cells_generic
```

Build and smoke-test the JitCPU backend, then build gem5 with JitCPU enabled:

```sh
QEMU_JIT_LIBRARY=$(util/jitcpu/build-qemu-jit.sh | tail -n 1)
scons setconfig build/RISCV USE_JITCPU=y
scons build/RISCV/gem5.opt -j"$(nproc)"
```

Build the C910 vendor library and standalone validation suite:

```sh
cmake -S src/rtl -B build/rtl-cosim-pulp-c910 \
  -DRTL_COSIM_BUILD_TESTS=ON \
  -DRTL_COSIM_BUILD_PULP_C910=ON \
  -DRTL_COSIM_BUILD_PULP_FIXTURES=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBJCACHE_ENABLED=OFF
cmake --build build/rtl-cosim-pulp-c910 -j
ctest --test-dir build/rtl-cosim-pulp-c910 --output-on-failure
```

## Run the demonstration

Obtain the fixed gem5 `riscv-boot-exit-nodisk` image. The runner creates a
reproducible copy with the small freestanding hello payload installed as PID
1; it never modifies the base image.

```sh
tests/gem5/rtl_cosim/run_linux_hello.py \
  build/RISCV/gem5.opt \
  "$QEMU_JIT_LIBRARY" \
  build/rtl-cosim-pulp-c910/pulp-c910/librtl_cosim_pulp_c910.so \
  /path/to/riscv-boot-exit-nodisk \
  --outdir /tmp/jit-to-c910-linux-hello
```

A successful run prints these markers:

```text
Switched JitCPU -> C910 RTL
Hello world from Linux on the C910 RTL CPU!
JITCPU_TO_C910_HELLO_PASS
JITCPU_TO_C910_HELLO_REGRESSION_PASS
```

The UART text is recorded in
`/tmp/jit-to-c910-linux-hello/m5out/system.platform.terminal`.

## Measured proof point

The following is one optimized run on 22 July 2026 using an Intel Core Ultra
7 265K host, a 50 MHz modeled C910 clock, and the classic gem5 memory system.
Build and image-overlay time are excluded.

| Phase | Simulated time | Host time |
|---|---:|---:|
| Linux boot on JitCPU | 0.543735002 s | 3.912812 s |
| Architectural switch and C910 import | no simulated time | 16.766077 s |
| Linux hello on C910 RTL | 0.005169997 s | 65.080780 s |

The RTL phase executed about 258,500 C910 clock cycles and completed 28,477
post-takeover memory reads before EOT. These are single-run engineering
measurements, not a cross-platform performance claim; repeated median runs
should be used for formal benchmarking.

## Interactive use and current scope

An interactive Linux terminal uses the same architecture: boot an image that
starts a shell, leave gem5's terminal listener enabled, and connect to its
serial port. The automated runner deliberately passes `--listener-mode=off`
for hermetic test environments and validates the transmit path only; an
interactive receive-session regression is future coverage.

The current handover is one-shot and one-way. It supports one RV64 context,
does not transfer vector state, and starts C910 with clean microarchitectural
caches and TLBs. Reverse switching, multicore Linux, RVV state migration, and
Ruby integration for the C910 RTL phase remain outside this proof of concept.
