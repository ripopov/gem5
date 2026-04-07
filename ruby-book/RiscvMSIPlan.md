# RISC-V MSI Example — Implementation Plan

Port the learning_gem5 Part 3 MSI example from x86 to RISC-V.
The RISC-V version lives in its own directory, leaving the original x86 files untouched.

---

## What Exists Today (x86)

| File | Role |
|------|------|
| `tests/test-progs/threads/src/threads.cpp` | Test binary: parallel vector add with false sharing |
| `tests/test-progs/threads/bin/x86/linux/threads` | Pre-built x86 ELF |
| `tests/test-progs/threads/src/Makefile` | Trivial `g++` build (native x86 only) |
| `configs/learning_gem5/part3/simple_ruby.py` | Top-level config: 2× `X86TimingSimpleCPU`, SE mode, loads `threads` binary |
| `configs/learning_gem5/part3/msi_caches.py` | `MyCacheSystem`: L1Cache controllers + DirController + SimpleNetwork mesh |
| `configs/learning_gem5/part3/ruby_caches_MI_example.py` | Same structure but for MI_example protocol |
| `configs/learning_gem5/part3/test_caches.py` | RubyRandomTester variant (ISA-independent) |
| `src/learning_gem5/part3/*.sm`, `MSI.slicc` | SLICC protocol files (ISA-independent, no changes needed) |

---

## Staged Plan

### Stage 1: Cross-compile the test binary for RISC-V

**Goal:** Produce `tests/test-progs/threads/bin/riscv/linux/threads` (a statically-linked RV64 ELF).

**Files to create:**

| File | Description |
|------|-------------|
| `tests/test-progs/threads/src/Makefile.riscv` | Cross-compile Makefile |
| `tests/test-progs/threads/bin/riscv/linux/threads` | Resulting binary (committed to repo) |

**Makefile.riscv content:**

```makefile
CROSS_COMPILE ?= riscv64-unknown-linux-gnu-
CXX = $(CROSS_COMPILE)g++
CXXFLAGS = -static -pthread -std=c++11 -O2

../bin/riscv/linux/threads: threads.cpp
	mkdir -p ../bin/riscv/linux
	$(CXX) $(CXXFLAGS) -o $@ $<
```

Key decisions:
- **`-static`** is mandatory — gem5 SE mode does not emulate a dynamic linker for RISC-V.
- **`-pthread`** links libpthread statically; `std::thread` uses pthreads underneath.
- The source `threads.cpp` is ISA-independent C++ — no code changes needed.
- The toolchain prefix `riscv64-unknown-linux-gnu-` matches what `apt install g++-riscv64-linux-gnu` or a standard RISC-V GNU toolchain provides. Some distros use `riscv64-linux-gnu-` instead; the `CROSS_COMPILE` variable lets users override.

**Verification:** `file ../bin/riscv/linux/threads` should report `ELF 64-bit LSB executable, UCB RISC-V, ... statically linked`.

> **Note:** The distro package on Ubuntu/Debian is `g++-riscv64-linux-gnu`, which installs the prefix `riscv64-linux-gnu-g++` (no `unknown`). The Makefile's `CROSS_COMPILE ?=` lets either work:
> ```bash
> make -f Makefile.riscv CROSS_COMPILE=riscv64-linux-gnu-
> ```

---

### Stage 2: Create the RISC-V config scripts

**Goal:** A self-contained directory `configs/learning_gem5/part3/riscv/` with RISC-V-specific configs that reuse the protocol-agnostic cache system from the parent directory.

**Files to create:**

| File | Description |
|------|-------------|
| `configs/learning_gem5/part3/riscv/simple_ruby_riscv.py` | Top-level config (RISC-V equivalent of `simple_ruby.py`) |
| `configs/learning_gem5/part3/riscv/msi_caches_riscv.py` | Cache system adapted for RISC-V |

#### `simple_ruby_riscv.py`

Changes from the x86 version:

1. **CPU type:** `X86TimingSimpleCPU` → `RiscvTimingSimpleCPU`
2. **Binary path:** `threads/bin/x86/linux/threads` → `threads/bin/riscv/linux/threads`
3. **Interrupt controller:** RISC-V `createInterruptController()` works the same way — no change needed.
4. **Remove `config_filesystem`:** The pseudo filesystem setup (`FileSystemConfig.config_filesystem`) is an x86 SE-mode convenience for `/proc/cpuinfo` emulation. RISC-V SE mode does not need it. Remove the import and call.
5. **Import cache system:** `from msi_caches_riscv import MyCacheSystem`

Sketch:

```python
import m5
from m5.objects import *
from msi_caches_riscv import MyCacheSystem

system = System()
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]

system.cpu = [RiscvTimingSimpleCPU() for i in range(2)]

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]

for cpu in system.cpu:
    cpu.createInterruptController()

system.caches = MyCacheSystem()
system.caches.setup(system, system.cpu, [system.mem_ctrl])

thispath = os.path.dirname(os.path.realpath(__file__))
binary = os.path.join(
    thispath, "../../../../",
    "tests/test-progs/threads/bin/riscv/linux/threads",
)

process = Process()
process.cmd = [binary]
for cpu in system.cpu:
    cpu.workload = process
    cpu.createThreads()

system.workload = SEWorkload.init_compatible(binary)

root = Root(full_system=False, system=system)
m5.instantiate()
print("Beginning simulation!")
exit_event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
```

#### `msi_caches_riscv.py`

This is a copy of `msi_caches.py` with one change:

- **`sendEvicts()` returns `False`** — RISC-V does not have x86's `mwait` instruction or ARM's local exclusive monitor. For `TimingSimpleCPU`, eviction notifications are not needed. (The O3 CPU case would still need `True`, but this example uses `TimingSimpleCPU`.)

Everything else (L1Cache size, DirController, MyNetwork topology, virtual network count, message buffer wiring) stays identical.

**Alternative considered:** Instead of copying, import from parent and subclass to override `sendEvicts`. This is cleaner but couples the RISC-V example to the x86 file's internal structure. Since this is a teaching example where readers should see all the pieces in one place, a standalone copy is better.

---

### Stage 3: Build gem5 with MSI protocol for RISC-V and run

**Build:**

```bash
scons build/RISCV/gem5.opt -j$(nproc) PROTOCOL=MSI
```

The MSI protocol must be enabled in the Kconfig configuration. The SLICC files in `src/learning_gem5/part3/` are ISA-independent — no changes needed.

**Run:**

```bash
./build/RISCV/gem5.opt -d m5out/riscv-msi-test-$(date +%Y%m%d-%H%M%S) \
    configs/learning_gem5/part3/riscv/simple_ruby_riscv.py
```

**Expected output:**
```
Running on 2 cores. with 100 values
Waiting for other threads to complete
Validating...Success!
Exiting @ tick ... because exiting with last active thread context
```

**Verification checklist:**
- [ ] Binary loads without `SEWorkload` errors
- [ ] Both CPUs execute (check `stats.txt` for non-zero committed instructions on both CPUs)
- [ ] `Validating...Success!` appears in stdout
- [ ] No `SLICC protocol error` panics
- [ ] No deadlocks (simulation completes, does not hang)

---

### Stage 4: Verify with ProtocolTrace

Run with the `ProtocolTrace` debug flag to confirm MSI transitions are exercised:

```bash
./build/RISCV/gem5.opt -d m5out/riscv-msi-trace-$(date +%Y%m%d-%H%M%S) \
    --debug-flags=ProtocolTrace \
    configs/learning_gem5/part3/riscv/simple_ruby_riscv.py
```

Expect to see `I→S`, `I→M`, `S→M` transitions and invalidation traffic in the trace.
The false-sharing pattern in `threads.cpp` should produce visible `GetM`/`Inv`/`InvAck` sequences between the two L1 caches.

---

## File Summary

| Action | Path |
|--------|------|
| **Create** | `tests/test-progs/threads/src/Makefile.riscv` |
| **Create** | `tests/test-progs/threads/bin/riscv/linux/threads` (built binary) |
| **Create** | `configs/learning_gem5/part3/riscv/simple_ruby_riscv.py` |
| **Create** | `configs/learning_gem5/part3/riscv/msi_caches_riscv.py` |
| No changes | `src/learning_gem5/part3/*.sm` (SLICC files are ISA-independent) |
| No changes | Original x86 files in `configs/learning_gem5/part3/` |
| No changes | `tests/test-progs/threads/src/threads.cpp` (portable C++) |

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| RISC-V SE mode may not fully support `std::thread` / `clone` syscall | gem5's RISC-V SE mode supports `clone` and `futex`. The `hello` test binary works; threads should too. If not, fall back to a single-threaded test first to isolate the issue. |
| Static linking bloats the binary | Acceptable for a test binary. The x86 version is dynamically linked, but RISC-V SE mode requires static linking. |
| `riscv64-unknown-linux-gnu-g++` not installed | Document the toolchain requirement in Makefile comments. Ubuntu/Debian: `apt install g++-riscv64-linux-gnu`. |
| MSI protocol not enabled in Kconfig for RISC-V build | Verify Kconfig has `RUBY_PROTOCOL_MSI=y`. The MSI protocol is ISA-independent but may not be enabled by default. |
