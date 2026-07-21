# Standalone RTL Co-simulation Runtime

This directory contains the standalone RTL co-simulation runtime. Stage 1
loads vendor V1 shared libraries, validates discovered interfaces, transacts
APB, AXI3, and AXI4 in both directions, and exercises models without gem5.
Stage 2 adds the SCR1 reference vendor integration under `ext/rtl/scr1`.
Stage 3 adds the wider PULP C910 reference under `ext/rtl/pulp-c910`.
Stage 4 adds the gem5 adapter and a two-core SCR1 validation system.
Stage 5 adds a direct-memory, single-core C910 portability test.
AXI3-ACE support is intentionally limited to structural profile validation.

The vendor ABI is the self-contained header
`include/gem5/rtl_cosim/api_v1.hh`. Runtime and checker code uses only that
header, C++20, operating-system shared-library APIs, and sources in this
directory. PULP and Verilator types are confined to the fixture adapters.

## Build and test

The base build needs CMake 3.20 or newer and a C++20 compiler. It uses the
GoogleTest checkout already present under `ext/googletest` when available.

```bash
cmake -S src/rtl -B build/rtl-cosim \
  -DRTL_COSIM_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/rtl-cosim -j
ctest --test-dir build/rtl-cosim --output-on-failure
```

The base build produces:

- `rtl_cosim_runtime`: loader, validation, image handling, and transactors.
- `rtl_cosim_checker_support`: checker configuration and neutral test drivers.
- `rtl-cosim-check`: standalone vendor-library checker.
- `rtl_cosim_test_vendor`: deterministic C++ V1 conformance fixture.
- `rtl_cosim_tests`: protocol, loader, checker, and malformed-input tests.

Enable the pinned PULP/Verilator endpoints after initializing the submodules:

```bash
git submodule update --init ext/rtl/pulp/axi ext/rtl/pulp/apb
cmake -S src/rtl -B build/rtl-cosim-pulp \
  -DRTL_COSIM_BUILD_TESTS=ON \
  -DRTL_COSIM_BUILD_PULP_FIXTURES=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/rtl-cosim-pulp -j
ctest --test-dir build/rtl-cosim-pulp --output-on-failure
```

This adds four independently loadable vendor libraries: APB master, APB
slave, AXI4 master, and AXI4 slave. The tests run each library through
`rtl-cosim-check` and connect each PULP master directly to its matching PULP
slave through the neutral transaction interfaces. No gem5 or SCR1 library is
linked. If Verilator's CMake integration selects an unusable `ccache`, add
`-DOBJCACHE_ENABLED=OFF` when configuring.

Enable the SCR1 reference adapter and its bare-metal scenarios after
initializing the pinned submodule:

```bash
git submodule update --init ext/rtl/scr1/repo
cmake -S src/rtl -B build/rtl-cosim-scr1 \
  -DRTL_COSIM_BUILD_TESTS=ON \
  -DRTL_COSIM_BUILD_SCR1=ON \
  -DOBJCACHE_ENABLED=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/rtl-cosim-scr1 -j
ctest --test-dir build/rtl-cosim-scr1 --output-on-failure
```

This requires Verilator and a `riscv64-unknown-elf-gcc` or
`riscv32-unknown-elf-gcc` toolchain. The SCR1 shared library itself also has
an independent CMake build that needs neither the runtime nor gem5; see
`ext/rtl/scr1/README.md`.

Enable the PULP C910 reference adapter and its RV64 scenarios after
initializing its pinned RTL dependencies:

```bash
git submodule update --init \
  ext/rtl/pulp-c910/repo ext/rtl/pulp/axi \
  ext/rtl/pulp/common_cells ext/rtl/pulp/tech_cells_generic
cmake -S src/rtl -B build/rtl-cosim-pulp-c910 \
  -DRTL_COSIM_BUILD_TESTS=ON \
  -DRTL_COSIM_BUILD_PULP_C910=ON \
  -DOBJCACHE_ENABLED=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/rtl-cosim-pulp-c910 -j
ctest --test-dir build/rtl-cosim-pulp-c910 --output-on-failure
```

This requires a 64-bit RISC-V bare-metal GCC. The independently buildable C910
DLL, signal map, low-power contract, and test inventory are documented in
`ext/rtl/pulp-c910/README.md`.

## gem5 integration

Build gem5 with the Ruby protocol used by the common two-core configuration:

```bash
scons setconfig build/RISCV RUBY_PROTOCOL_MESI_Two_Level=y
scons build/RISCV/gem5.opt -j
```

The same `configs/example/rtl_cosim/scr1_two_core.py` script selects either
`--memory-system classic` or `--memory-system ruby`. It creates two SCR1
cores, connects their named instruction and data AXI4 buses, and uses either
private classic L1 caches with a shared L2 or Ruby `MESI_Two_Level` with
`SimpleNetwork`.

For example:

```bash
build/RISCV/gem5.opt configs/example/rtl_cosim/scr1_two_core.py \
  --library build/rtl-cosim-scr1/scr1/librtl_cosim_scr1.so \
  --image build/rtl-cosim-scr1/scr1/programs/gem5_boot_memory.elf \
  --memory-system ruby --validation idle
```

The validation images cover boot and memory traffic, deterministic multicore
execution, Peterson mutual exclusion, interrupt wake-up, reset, and AXI error
propagation. Success prints `RTL_COSIM_PASS memory_system=classic` or
`RTL_COSIM_PASS memory_system=ruby`. Run the complete matrix through gem5's
test infrastructure with:

```bash
tests/main.py run --length long --isa RISCV --variant opt \
  tests/gem5/rtl_cosim
```

The same test directory also builds and runs the Stage 5 C910 system. It
connects the model's single 128-bit AXI4 initiator through a noncoherent xbar
to one memory controller, without using the SCR1 standard-library wrapper or
any core-specific gem5 code. The RV64 C benchmark performs deterministic
compute and external-memory traffic, writes an eight-byte signature, and
enters drained WFI. Run it directly with:

```bash
build/RISCV/gem5.opt configs/example/rtl_cosim/c910_single_core.py \
  --library \
    build/rtl-cosim-pulp-c910/pulp-c910/librtl_cosim_pulp_c910.so \
  --image \
    build/rtl-cosim-pulp-c910/pulp-c910/programs/gem5_benchmark.elf
```

Success prints `RTL_COSIM_C910_PASS`. The validation controller exits with a
nonzero status on timeout or a byte-exact signature mismatch; packet or
transactor protocol failures remain fatal simulation errors.

For AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake -S src/rtl -B build/rtl-cosim-sanitize \
  -DRTL_COSIM_BUILD_TESTS=ON \
  -DRTL_COSIM_BUILD_PULP_FIXTURES=ON \
  -DRTL_COSIM_ENABLE_SANITIZERS=ON \
  -DOBJCACHE_ENABLED=OFF \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build/rtl-cosim-sanitize -j
ctest --test-dir build/rtl-cosim-sanitize --output-on-failure
```

LeakSanitizer cannot run under some debuggers and managed `ptrace`
environments. In that case, set `ASAN_OPTIONS=detect_leaks=0` while running
the tests; address and undefined-behavior checks remain enabled, but leak
checking must be repeated in an unrestricted environment.

## Architecture

`ModelLoader` owns the shared library, manager, and one core in destruction
order. `validateModel()` converts vendor discovery data into validated bus
profiles before a transactor is created. Signal widths come only from
`Signal::bitWidth()`; JSON never supplies protocol widths.

The two neutral directions are:

- `TransactionBackend`: consumes requests captured from an RTL initiator and
  returns asynchronous responses.
- `TransactionSource`: supplies requests to an RTL target and accepts its
  responses.

APB and AXI transactors use `beforeClock()` to drive all bus inputs. After one
core-wide `RtlCore::settle()`, `afterSettle()` captures only payloads whose
VALID/READY handshake is active. `afterClock()` commits those handshakes after
the vendor advances one full cycle. AXI handling includes fixed, incrementing,
and wrapping bursts; narrow lanes; byte strobes; AXI3 WID; AXI IDs; per-ID
ordering; cross-ID response reordering; error responses; and bounded queues.
Optional AXI4 control signals default to zero when absent.

Neutral request data is compact: each beat occupies `beatBytes` consecutive
bytes in increasing-address order. Transactors map that compact representation
to the correct physical bus lanes. Values crossing the vendor API use
little-endian bytes, with RTL bit zero in bit zero of byte zero.

## Running the checker

```bash
build/rtl-cosim/rtl-cosim-check VENDOR_LIBRARY CHECKER_JSON
```

The checker resolves the V1 factory symbols, creates one core, validates all
discovery metadata, loads configured images, drives inputs and reset, attaches
memory or scripted transactions to every bus, and clocks to idle, finish,
error, or the mandatory cycle limit. Initiator buses share one sparse memory.
Target-bus transactions are keyed by the discovered bus name.

Important JSON fields are:

- `core.config`: object serialized unchanged for `createCore()`.
- `reset.assert_cycles`: number of complete cycles with reset asserted.
- `inputs`: standalone input names mapped to integers or hexadecimal strings.
- `memory`: `base`, `size`, `latency_cycles`, `max_pending`, `error_ranges`,
  and an `images` array containing `path`, `format`, and raw `address`.
- `transactions`: per-target-bus read/write scripts. Requests specify address,
  ID, beat size/count, burst, data, and byte enables. `expect_data` and
  `expect_error` validate responses.
- `run`: `max_cycles`, `stop_on_idle`, and `stop_on_finish`.

Image formats are `raw`, `elf`, `coff`, or `auto`. Relative image paths are
resolved relative to the checker JSON. Loadable segments use a matching TCM
backdoor when available and otherwise use external sparse memory; BSS tails
are zero-filled.

Process status is stable for automation:

| Status | Meaning |
| ---: | --- |
| 0 | Successful idle, finish, or ACE structural-validation stop |
| 1 | Command-line or JSON configuration error |
| 2 | Library, V1 entry-point, or core-construction error |
| 3 | Discovery or protocol-profile validation error |
| 4 | Runtime, signal, clock, or protocol error |
| 5 | Cycle limit reached |

## Vendor adapter checklist

1. Copy `include/gem5/rtl_cosim/api_v1.hh` unchanged.
2. Implement manager, core, bus, and signal ownership exactly as documented in
   `RtlCosimDesign.md`; never allow exceptions across the V1 boundary.
3. Bind physical pins to canonical APB or AXI role IDs. Signal names are only
   diagnostic and are not used to infer protocol meaning.
4. Implement `RtlCore::settle()` as a clock-free combinational evaluation, or
   as an explicit no-op when the model always maintains stable outputs.
5. Toggle the model's single physical clock entirely inside `RtlCore::clock()`
   and return only after outputs stabilize.
6. Notify each subscribed output at most once per settle or clock operation,
   after stabilization, and unregister callbacks safely during teardown.
7. Export `createRtlCoreManagerV1` and `destroyRtlCoreManagerV1`, then run the
   checker before integrating with gem5.

The PULP adapters under `fixtures/pulp` demonstrate this boundary, including
flattening structured SystemVerilog interfaces into portable V1 `Signal`
objects. They are validation fixtures, not a dependency of the runtime.

## Troubleshooting

- A missing factory or destructor is reported as a library error. Check symbol
  visibility and confirm that both names have C linkage.
- A direction error is always expressed from the RTL model's perspective:
  signals driven into RTL are `Input`; signals driven by RTL are `Output`.
- Width failures identify the canonical role. Do not repeat widths in JSON or
  create constant pseudo-signals for optional AXI4 roles.
- A timeout normally means the model cannot prove idle, traffic is still
  outstanding, reset polarity is wrong, or the configured program never
  reaches a terminal state. Increase the limit only after checking diagnostics.
- `WLAST`, `RLAST`, ID, alignment, 4-KiB, and locked-access violations are
  deliberate protocol errors. V1 does not implement exclusive accesses.
- ACE libraries stop after structural validation because transaction and
  coherence behavior needs a future dedicated gem5 adapter.
