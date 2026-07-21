# SCR1 RTL Co-simulation Reference Adapter

This directory is the reference vendor implementation of the RTL
co-simulation V1 API. It Verilates the pinned `scr1_top_axi` model and packages
it as a loadable shared library without linking gem5 or the standalone
co-simulation runtime. Vendor-specific code is confined to `src/adapter.cc`;
the pristine upstream SCR1 repository remains in `repo`.

## Build the vendor library

The library requires CMake 3.20 or newer, a C++20 compiler, Verilator, and the
initialized SCR1 submodule:

```bash
git submodule update --init ext/rtl/scr1/repo
cmake -S ext/rtl/scr1 -B build/rtl-cosim-scr1-vendor \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBJCACHE_ENABLED=OFF
cmake --build build/rtl-cosim-scr1-vendor -j
```

The output is `librtl_cosim_scr1.so` on ELF platforms, with the corresponding
platform name on macOS or Windows. The adapter includes only
`include/gem5/rtl_cosim/api_v1.hh`, generated SCR1 model headers, and
Verilator runtime headers.

## Build and run the standalone tests

Tests are enabled through the Stage 1 runtime build. They additionally require
GoogleTest and a `riscv64-unknown-elf-gcc` or `riscv32-unknown-elf-gcc`
bare-metal toolchain:

```bash
cmake -S src/rtl -B build/rtl-cosim-scr1 \
  -DRTL_COSIM_BUILD_TESTS=ON \
  -DRTL_COSIM_BUILD_SCR1=ON \
  -DRTL_COSIM_BUILD_PULP_FIXTURES=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBJCACHE_ENABLED=OFF
cmake --build build/rtl-cosim-scr1 -j
ctest --test-dir build/rtl-cosim-scr1 --output-on-failure
```

The GoogleTests cover shared-library and object lifetime, configuration
failures, discovery and variable widths, reset sequencing, output callbacks,
unaligned TCM access, ELF loading, AXI traffic, failure paths, interrupt
wakeup, and idle detection. Three CTest scenarios also run
`rtl-cosim-check` against deterministic RISC-V programs:

```bash
build/rtl-cosim-scr1/rtl-cosim-check \
  build/rtl-cosim-scr1/scr1/librtl_cosim_scr1.so \
  build/rtl-cosim-scr1/scr1/riscv_unit.json

build/rtl-cosim-scr1/rtl-cosim-check \
  build/rtl-cosim-scr1/scr1/librtl_cosim_scr1.so \
  build/rtl-cosim-scr1/scr1/baremetal_tcm.json

build/rtl-cosim-scr1/rtl-cosim-check \
  build/rtl-cosim-scr1/scr1/librtl_cosim_scr1.so \
  build/rtl-cosim-scr1/scr1/c_integration.json
```

All scenarios terminate with `stop: idle after ... cycles` within their
configured cycle limits. The unit scenario executes from external memory. The
bare-metal assembly scenario boots externally, jumps into the TCM, performs an
external data read and write, and reaches an interrupt-wakeable WFI state. The
freestanding C scenario additionally establishes a TCM stack, verifies `.data`
and `.bss` loading, and runs compiled C loops over external AXI memory.

## Exposed model

The adapter discovers two AXI4 initiator buses named `instruction` and `data`.
Each maps all physical SCR1 AXI4 pins to canonical V1 roles; widths are read
from the Verilated ports rather than supplied by JSON. Optional AXI4 USER,
REGION, and QOS signals are exposed when present.

Standalone bindings include the four reset inputs, JTAG reset, system-reset
output, 16 individually addressable interrupt inputs, software interrupt,
hart and ID fuses, test/JTAG signals, RTC clock, and status outputs. The main
clock is intentionally private because `RtlCore::clock()` owns it.

SCR1 has one physical 64-KiB tightly coupled RAM at `0x00480000`. The adapter
publishes aliased `CodeTcm` and `DataTcm` regions over that storage so the
generic image loader can choose either semantic role. Backdoor reads and
writes support arbitrary byte alignment and enforce region bounds.

`isIdle()` reports true only after reset when SCR1 is halted in WFI, both AXI
bridges are idle, no response or request handshake is pending, no interrupt is
asserted, and the internal timer is disabled. Test programs disable the timer
before WFI so checker completion is deterministic. Driving an interrupt makes
the core non-idle and allows clock scheduling to resume.

The optional `instance_name` string is the only adapter configuration field:

```json
{"instance_name": "scr1-0"}
```

Use `{}` for the default name. Unknown fields, malformed JSON, and invalid
names fail core construction with a diagnostic available from the manager.
