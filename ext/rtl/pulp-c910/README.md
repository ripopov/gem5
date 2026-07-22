# PULP C910 RTL Co-simulation Reference Adapter

This directory is the second reference vendor implementation of the RTL
co-simulation V1 API. It Verilates the pinned PULP `c910_axi_wrap`, flattens
its normalized AXI4 port, and packages the model as a loadable shared library.
The library includes no gem5 or standalone-runtime code. Upstream sources stay
pristine under `repo`; the adapter, integration wrapper, tests, and CMake build
live in the parent directory.

## Build the vendor library

Initialize the C910 and PULP RTL dependencies, then run the independent build:

```bash
git submodule update --init \
  ext/rtl/pulp-c910/repo \
  ext/rtl/pulp/axi \
  ext/rtl/pulp/common_cells \
  ext/rtl/pulp/tech_cells_generic
cmake -S ext/rtl/pulp-c910 -B build/rtl-cosim-pulp-c910-vendor \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBJCACHE_ENABLED=OFF
cmake --build build/rtl-cosim-pulp-c910-vendor -j
```

The output is `librtl_cosim_pulp_c910.so` on ELF platforms, with the
corresponding platform name on macOS or Windows. A guarded CMake compatibility
fix corrects two LFB input widths missed by pinned upstream commit `896e5d3`;
configuration fails rather than silently applying it if upstream changes.

## Build and run standalone validation

GoogleTests and `rtl-cosim-check` scenarios are enabled through the runtime
build. They require Verilator and `riscv64-unknown-elf-gcc`:

```bash
cmake -S src/rtl -B build/rtl-cosim-pulp-c910 \
  -DRTL_COSIM_BUILD_TESTS=ON \
  -DRTL_COSIM_BUILD_PULP_C910=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBJCACHE_ENABLED=OFF
cmake --build build/rtl-cosim-pulp-c910 -j
ctest --test-dir build/rtl-cosim-pulp-c910 --output-on-failure
```

For ASan and UBSan validation, configure a separate Debug build with
`-DRTL_COSIM_ENABLE_SANITIZERS=ON` and run the same CTest command. If the test
environment uses `ptrace`, LeakSanitizer must be disabled because it cannot
initialize under a tracer; ASan and UBSan remain active.

The tests cover library lifetime and configuration errors; discovery; reset;
40-bit addresses, 128-bit data, 8-bit IDs, and USER signals; independent RTC
settling; RV64 boot; AXI reads, writes, and error responses; callbacks; drained
WFI detection; main-clock suppression; timer-interrupt wakeup; and return to
idle. They also validate malformed-state rejection and exact PC, integer, and
floating-point import before resumed execution. Checker scenarios run assembly
boot/WFI, exception-on-AXI-error, and freestanding C from external memory.

## gem5 single-core validation

Stage 5 uses `configs/example/rtl_cosim/c910_single_core.py` to instantiate
the vendor library as one `RtlCoreSimObject`. Its unified AXI4 initiator is
connected through a noncoherent xbar to one gem5 memory controller. The
configuration maps every discovered reset, interrupt, JTAG, debug, RTC, and
low-power signal by name; all C910-specific knowledge remains here and in the
configuration, outside the generic packet backend and transactor.

The `gem5_benchmark` RV64 image exercises compiled C arithmetic and repeated
reads and writes to external memory before storing the little-endian signature
`0xdc2efb8acc3994ff` at `0x01800020` and entering drained WFI. The generic
validation controller checks the expected bytes only after the RTL core,
transactor, and packet backend are all idle. See `src/rtl/README.md` for the
direct command and gem5 test-infrastructure command.

## AtomicSimpleCPU to C910 switching

The adapter optionally exposes `RtlCpuState` with schema `riscv64/v1` and one
architectural context. Import uses the C910 HAD/JTAG mechanism to halt a reset,
idle core, restore PMP, machine/supervisor CSRs, floating-point state, integer
registers, privilege, and PC, and then resume without another reset. Generated,
guarded build copies reconnect PULP's otherwise removed core-0 HAD router and
debug request; the pinned repository and generic framework remain unchanged.

`configs/example/rtl_cosim/c910_cpu_switch.py` creates a real
`RiscvAtomicSimpleCPU`, a switched-out `RtlCpuSimObject`, and a preconnected
C910 AXI runtime. The first CPU executes through an `m5_switch_cpu`
instruction; gem5 drains it, transfers its `ThreadContext`, imports the
canonical state, and starts C910 at the following instruction. The matrix
covers exact GPR/PC and FPR/FCSR contents, compiled C state, traps and M/S CSRs,
U-mode continuation, active S-mode Sv39 and PMP, pre-switch memory, AMOs before
and after handover, RTL LR/SC, timer and external interrupts, and WFI wake.
Linux boot and switching back are not supported yet; RVV is disabled because
the initial canonical schema does not transfer vector state.

OpenC910 requires its private memory-mode and data-cache control bits before
AMO or LR/SC execution. It also treats `0x1000_0000` through `0x13ff_ffff` as
cacheable, so the atomic fixtures place their shared data in that native PMA
window. These policies are test/core-specific and do not appear in
`RtlCpuSimObject` or the generic AXI transactor. AXI exclusives use portable
neutral transaction flags and map to gem5 load-locked/store-conditional
requests. That path has independent transactor and backend tests; C910
completes the cacheable LR/SC fixture in its private data cache.

## Exposed model

The adapter discovers one AXI4 initiator bus named `memory`. It maps the full
V1 AXI4 role set exposed by the normalized wrapper. Address, data, strobe, ID,
and USER widths come from the physical model bindings; AXI5 ATOP stays tied to
zero inside the upstream wrapper and is not added to V1. ACE response cleanup,
evict absorption, wrapping-burst conversion, and decrementing-burst conversion
remain in `c910_axi_wrap`, so the generic AXI4 transactor is unchanged.
The integration also completes OpenC910's reserved ID-31 legacy AxBAR
transactions locally after older AXI traffic drains. This preserves their
ordering semantics without exposing them as ordinary address-zero writes on
the standard AXI4 port.

Standalone bindings include active-low core and JTAG resets; software, timer,
PLIC, and 40 external interrupts; RTC; debug request; JTAG; and the two-bit
`lpmd_b` low-power status. The integration-only SystemVerilog wrapper surfaces
the connected internal CP0 `lpmd` output without modifying the pinned
repository because the generated public monitor is undriven.
PULP C910 has no TCM in this configuration, so all images use external AXI
memory and the V1 backdoor list is empty.

`isIdle()` is true only when reset is released, `lpmd_b` reports a drained WFI
state, the external AXI channels have no pending request or response, and no
raw interrupt, debug request, or JTAG clock wake is asserted. The RTC is a
separate input and can be pulsed with `setValue()` plus `settle()` while main
clock calls remain suppressed. A raw wake source makes the core non-idle
before the next main clock, allowing the scheduler to ungate it safely.

The optional adapter configuration is:

```json
{"instance_name": "c910-0"}
```

Use `{}` for the default name. Unknown fields, malformed JSON, and invalid
names fail core construction with a manager diagnostic.
