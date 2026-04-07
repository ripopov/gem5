# gem5 SE Mode Internals for the Ch17 Test Programs

This document covers everything needed to productively write C test programs
for the 16-core CHI mesh system described in Chapter 17.
It traces the full path from binary loading through thread creation,
memory allocation, and physical address routing to the two DDR controllers.

---

## 1. What SE Mode Is

gem5 has two execution modes:

- **Full System (FS):** boots a real OS kernel, models all hardware
  (interrupt controllers, UART, boot ROM, MMU page walks).
  Programs run on top of Linux inside the simulator.
- **Syscall Emulation (SE):** no kernel.
  gem5 loads a statically linked ELF binary directly into simulated memory,
  sets the program counter to the ELF entry point,
  and **intercepts system calls** at the instruction level.
  When the guest executes an `ecall` (RISC-V),
  gem5 catches it and runs a C++ handler that emulates the Linux syscall.

SE mode is faster to set up and avoids booting a kernel,
but it only works for user-space binaries.
Our Ch17 test programs are all statically linked C programs
compiled with `riscv64-linux-gnu-gcc -static`, so SE mode is ideal.

### Key classes

| Class | File | Role |
|-------|------|------|
| `SEWorkload` | `src/sim/se_workload.hh:38` | System-wide object: manages physical memory pools, dispatches syscalls |
| `EmuLinux` (RISC-V) | `src/arch/riscv/linux/se_workload.cc:94` | RISC-V-specific: reads syscall number from register, routes to descriptor table |
| `Process` | `src/sim/process.hh:66` | Per-process state: page table, MemState (VMA list), file descriptors, PIDs |
| `RiscvProcess64` | `src/arch/riscv/process.cc:71` | RISC-V 64-bit address space layout (stack, heap, mmap addresses) |
| `MemState` | `src/sim/mem_state.hh:67` | Tracks brk point, stack bounds, mmap region, VMA list |
| `EmulationPageTable` | `src/mem/page_table.hh:53` | Hash-map page table: virtual page → physical page |

### How a binary runs

1. **Config script** creates a `Process` object with the binary path
   (`configs/example/rbook_mesh_config.py:80`).
2. `ProcessParams::create()` calls `loader::createObjectFile()` to parse the ELF.
   For RISC-V Linux, this creates a `RiscvProcess64` (`src/arch/riscv/linux/se_workload.cc:67`).
3. `Process::initState()` writes ELF segments (`.text`, `.data`, `.bss`) into
   simulated memory via `SETranslatingPortProxy` (`src/sim/process.cc:306`).
4. `RiscvProcess64::initState()` calls `argsInit<uint64_t>()` to build the
   initial stack: argc, argv pointers, envp, auxiliary vector
   (`src/arch/riscv/process.cc:97–102`).
5. The stack pointer is set to the bottom of the stack region,
   and PC is set to the ELF entry point (`src/arch/riscv/process.cc:259–260`).
6. `m5.simulate()` starts the event loop.
   The CPU fetches instructions, executes them, and when it hits `ecall`,
   `SEWorkload::syscall()` dispatches to the appropriate C++ handler.

---

## 2. Virtual Address Space Layout (RISC-V 64-bit)

`RiscvProcess64` sets up the following layout
(`src/arch/riscv/process.cc:71–82`):

```
High addresses
┌──────────────────────────────────────┐
│  0x7FFFFFFFFFFFFFFF                  │ ← stack_base
│  Stack (grows ↓)                     │
│  max_stack_size = 64 MiB default     │
├──────────────────────────────────────┤
│  0x7FFFFFFFFFFFFFF - 64MiB           │ ← next_thread_stack_base
│  (Reserved for thread stacks         │
│   from pthread_create via mmap)      │
│                                      │
│         ... large gap ...            │
│                                      │
├──────────────────────────────────────┤
│  0x4000000000000000                  │ ← mmap_end (grows ↓)
│  mmap region (grows ↓)               │
│  Thread stacks allocated here via    │
│  mmap in glibc's pthread_create      │
│                                      │
│         ... large gap ...            │
│                                      │
├──────────────────────────────────────┤
│  brk_point (grows ↑)                 │ ← first page after image.maxAddr()
│  Heap (via brk/sbrk/malloc)          │
├──────────────────────────────────────┤
│  .bss (zero-initialized data)        │
│  .data (initialized data)            │
│  .text (code)                        │
│  .rodata, etc.                       │
└──────────────────────────────────────┘
Low addresses (typically ~0x10000 for RISC-V)
```

### Static vs dynamic memory

| Segment | When allocated | How |
|---------|---------------|-----|
| `.text`, `.data`, `.rodata` | At process load | `image.write(*initVirtMem)` eagerly allocates physical pages |
| `.bss` | At process load | Eagerly allocated, zero-filled |
| Heap (brk) | On `brk()`/`malloc()` | `MemState::updateBrkRegion()` creates VMA; pages allocated lazily on first access via `fixupFault()` |
| Stack (main) | At process load | Pre-allocated in `argsInit()` for initial stack frame; grows on demand via `fixupFault()` up to `maxStackSize` (64 MiB) |
| mmap regions | On `mmap()` syscall | `MemState::extendMmap()` finds free VA; pages allocated lazily on first access |
| Thread stacks | On `pthread_create` → `clone` | glibc calls `mmap(MAP_ANONYMOUS)` to allocate thread stack before calling `clone`; gem5 handles the mmap normally |

### Page table: not identity-mapped

SE mode does **not** use identity mapping.
The `EmulationPageTable` is a hash map (`std::unordered_map<Addr, Entry>`)
that maps virtual page numbers to arbitrary physical page numbers
(`src/mem/page_table.cc:142–150`):

```cpp
bool EmulationPageTable::translate(Addr vaddr, Addr &paddr)
{
    const Entry *entry = lookup(vaddr);
    if (!entry) return false;
    paddr = pageOffset(vaddr) + entry->paddr;
    return true;
}
```

Physical pages are allocated from a `MemPool` managed by `SEWorkload`.
The pool is initialized from the system's `mem_ranges`
(`src/sim/se_workload.cc:42–54`):

```cpp
void SEWorkload::setSystem(System *sys) {
    Workload::setSystem(sys);
    AddrRangeList memories = sys->getPhysMem().getConfAddrRanges();
    memPools.populate(memories);
}
```

With `mem_ranges=[AddrRange("512MiB")]` in our config (which defaults to
`[0x0, 0x20000000)`), the physical page pool spans `0x0` to `0x20000000`.
Pages are allocated sequentially from this pool by `MemPool::allocate()`
(`src/sim/mem_pool.cc:96–102`).

---

## 3. How Memory Maps to the Two DDR Controllers

Our system has `--num-dirs=2`, placing DDR controllers at routers 0 and 15.
The address routing has two independent interleaving stages:

### Stage A: HN-F (directory/LLC) interleaving — 16-way

`CHI_config.py:636–652` sets up 16-way interleaving across HN-F nodes:

```python
block_size_bits = int(math.log(cache_line_size, 2))  # = 6 (64B lines)
llc_bits = int(math.log(len(hnfs), 2))               # = 4 (16 HNFs)
numa_bit = block_size_bits + llc_bits - 1             # = 9

for i, hnf in enumerate(hnfs):
    addr_range = AddrRange(
        r.start, size=r.size(),
        intlvHighBit=9,      # bits [9:6] select HN-F
        intlvBits=4,          # 4 bits → 16 HN-Fs
        intlvMatch=i,         # HN-F i handles lines where bits[9:6] == i
    )
```

This means **bits [9:6]** of the physical address select which of the 16 HN-F
nodes (and their LLC slices) owns a cache line.
Consecutive 64-byte cache lines cycle through HN-F 0, 1, 2, ..., 15, 0, 1, ...

### Stage B: SN-F (DDR controller) interleaving — 2-way

`Ruby.py:134–202` sets up 2-way interleaving for the memory controllers.
With `--num-dirs=2` and default `intlv_size=cacheline_size=64`:

```python
intlv_size = 64                              # = cacheline_size
intlv_low_bit = int(math.log(64, 2))         # = 6
intlv_bits = int(math.log(2, 2))             # = 1
# intlvHighBit = intlv_low_bit + intlv_bits - 1 = 6
```

So `MemConfig.create_mem_intf()` creates:

- **DDR0** (router 0): `intlvHighBit=6, intlvBits=1, intlvMatch=0`
  → handles lines where bit 6 = 0
- **DDR1** (router 15): `intlvHighBit=6, intlvBits=1, intlvMatch=1`
  → handles lines where bit 6 = 1

**Bit 6** of the physical address selects the DDR controller.
Even-numbered cache lines (address bit 6 = 0) go to DDR0;
odd-numbered cache lines (bit 6 = 1) go to DDR1.

### The complete path

```
Guest virtual address
    │
    ▼
EmulationPageTable::translate()    ← hash-map lookup
    │
    ▼
Physical address
    │
    ├─ bits [9:6] ──► select 1 of 16 HN-F nodes (directory + LLC slice)
    │
    └─ bit [6] ─────► select DDR0 (bit=0) or DDR1 (bit=1)
```

Note that bit 6 is shared between both interleaving schemes.
This is intentional: HN-F nodes 0, 2, 4, 6, 8, 10, 12, 14 (even `intlvMatch`)
will generate DDR misses to DDR0, while HN-F nodes 1, 3, 5, 7, 9, 11, 13, 15
(odd `intlvMatch`) go to DDR1.
The result is balanced DDR traffic regardless of access pattern.

### Practical consequence for test programs

You do **not** control which physical pages your program gets.
Physical pages are allocated sequentially from the pool.
But because interleaving operates at cache-line granularity (64 bytes),
any page (4 KiB = 64 cache lines) spans all 16 HN-F slices multiple times
and both DDR controllers.
A sequential sweep of even a single page touches every HN-F and both DDRs.

---

## 4. How Threads Work in SE Mode

### The key constraint: ThreadContexts are pre-allocated

Each CPU has a fixed number of `ThreadContext` objects created at boot.
Our config creates 16 `TimingSimpleCPU` instances, each with 1 thread
(`rbook_mesh_config.py:66`).
This gives 16 `ThreadContext` objects in the system.

At boot, one `ThreadContext` (on CPU 0) runs the `main()` function.
The other 15 start in the **Halted** state.

### pthread_create → clone → findFree

When the program calls `pthread_create()`:

1. **glibc** allocates a thread stack via `mmap(MAP_ANONYMOUS)` (handled by
   gem5's `mmapFunc`).
2. glibc calls `clone3()` with `CLONE_VM | CLONE_THREAD | CLONE_SIGHAND | ...`.
3. gem5 intercepts the `clone3` syscall (`src/sim/syscall_emul.hh:1985`).
4. `doClone()` calls `threads.findFree()` (`src/sim/system.cc:121–126`):

```cpp
ThreadContext *System::Threads::findFree()
{
    for (auto &thread: threads) {
        if (thread.context->status() == ThreadContext::Halted)
            return thread.context;
    }
    return nullptr;
}
```

This iterates **linearly** through all thread contexts and returns the
first one in `Halted` state.

5. The new thread's Process object shares the parent's page table and MemState
   (because `CLONE_VM` is set):
   ```cpp
   // src/sim/process.cc:183-191
   if (CLONE_VM & flags) {
       delete np->pTable;
       np->pTable = pTable;      // SAME page table pointer
       np->memState = memState;  // SAME MemState pointer
   }
   ```
   The page table is also marked `shared = true` (`src/sim/syscall_emul.hh:1943`).

6. The child's registers are cleared, the stack pointer is set to the new stack
   (allocated by glibc via mmap), and the thread context is activated
   (`src/sim/syscall_emul.hh:1968–1974`).

### How many threads can you create?

With 16 CPUs × 1 thread each = 16 ThreadContexts total.
One is used by `main()`, leaving **15 available for pthreads**.
If you try to create a 16th thread, `findFree()` returns `nullptr`
and `clone` returns `-EAGAIN`.

The test program at `tests/test-progs/threads/src/threads.cpp:84–89` shows
the standard pattern:

```cpp
// Create cpus-1 threads, use main thread for the last chunk
for (int i = 0; i < cpus - 1; i++) {
    threads[i] = new thread(array_add, a, b, c, i, cpus, num_values);
}
array_add(a, b, c, cpus - 1, cpus, num_values);  // main does work too
```

### Thread-to-core assignment

**There is no affinity/pinning mechanism in SE mode.**
`findFree()` does a linear scan and returns the first halted context.
In practice, with 16 CPUs and sequential pthread_create calls,
threads are assigned to CPUs in order: the first `pthread_create` gets CPU 1
(the lowest-numbered halted context), the second gets CPU 2, and so on.

This means:
- Thread 0 (main) → CPU 0 → router 0
- Thread 1 (first pthread_create) → CPU 1 → router 1
- Thread 2 → CPU 2 → router 2
- ...
- Thread 15 → CPU 15 → router 15

This is **deterministic** for our setup: with all 15 halted contexts
available and sequential creation, the assignment is predictable.

For the Ch17 test programs, this means you can reason about which router
a thread runs on based on creation order.
The false-sharing test (3c) relies on this: thread 0 is on CPU 0 (router 0)
and thread 1 is on CPU 1.
To get a thread on CPU 15, you would create 15 threads and use the last one,
or create all 15 and have the main thread on CPU 0.

---

## 5. Synchronization Primitives in SE Mode

### Atomic memory operations (AMO)

RISC-V `amoadd.w`, `amoswap.w`, `lr`/`sc` etc. are fully supported.
The CPU generates a memory request with an `AtomicOpFunctor` attached.
Ruby's coherence protocol (CHI in our case) ensures atomicity:
the atomic operation executes at the cache controller that holds the line
in exclusive state.
This is carried through `WriteMask::performAtomic()`
(`src/mem/ruby/common/WriteMask.hh:259`).

For the barrier test (3e), `amoadd.w` on a shared counter works correctly.

### Futex

The `futex` syscall is emulated (`src/sim/syscall_emul.hh:380–510`).
Supported operations:
- `FUTEX_WAIT`: thread suspends (ThreadContext → Suspended state),
  no busy-waiting in the simulator
- `FUTEX_WAKE`: wakes N suspended threads
- `FUTEX_CMP_REQUEUE`: move waiters between futexes
- `FUTEX_WAKE_OP`: atomic operation + conditional wake

The `FutexMap` (`src/sim/futex_map.hh:109`) tracks waiting threads
per futex address.
Threads that call `FUTEX_WAIT` are actually suspended (they stop consuming
simulation cycles), not spinning.

### pthread_mutex, pthread_barrier, pthread_cond

These are built on `futex` + atomics in glibc.
They work in SE mode because both AMOs and futex are supported.
The key requirements:
1. The program is statically linked (so glibc's pthread implementation
   is included in the binary).
2. Sufficient ThreadContexts exist for all threads.

---

## 6. How the Config Script Wires It Together

Our config (`configs/example/rbook_mesh_config.py`) follows the
**shared Process** pattern:

```python
# One Process object
process = Process(pid=100, executable=binary_path, cmd=[binary_path])

# Same Process assigned to all 16 CPUs
for cpu in system.cpu:
    cpu.workload = process
    cpu.createThreads()

# System-wide SE workload for syscall dispatch
system.workload = SEWorkload.init_compatible(binary_path)
```

This means:
- All 16 CPUs share the same Process object initially.
- CPU 0's ThreadContext runs `main()`.
- CPUs 1–15 start Halted, waiting for `pthread_create` → `clone`.
- When `clone` fires, the child gets a **new** Process object but with the
  **same** page table and MemState (shared pointers).
- The `SEWorkload` object manages the physical memory pool that all
  processes/threads allocate from.

### Memory ranges

```python
system = System(mem_ranges=[AddrRange("512MiB")], ...)
```

This gives a physical address pool of `[0x0, 0x20000000)`.
The `SEWorkload` populates its `MemPool` from this range.
Ruby's 2 SNF controllers split this range by bit 6 interleaving.
The 16 HNF controllers split it by bits [9:6] interleaving.

---

## 7. Practical Guidance for Writing Ch17 Test Programs

### Compiling

All tests are compiled with:
```
riscv64-linux-gnu-gcc -O2 -static -lpthread -o test test.c
```

The `-static` flag is essential: SE mode does not support dynamic linking
in the general case (no `ld-linux-riscv64.so` loaded).

### Threading pattern

```c
#include <pthread.h>
#include <stdio.h>

#define NUM_THREADS 15  // main thread + 15 pthreads = 16 total

void *worker(void *arg) {
    int tid = (int)(long)arg;
    // tid 1..15 → runs on CPU 1..15
    // ... do work ...
    return NULL;
}

int main() {
    pthread_t threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&threads[i], NULL, worker, (void *)(long)(i + 1));

    // main thread is thread 0, runs on CPU 0
    // ... do main's work ...

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    printf("PASS\n");
    return 0;
}
```

### Knowing which core a thread runs on

There is no `sched_getcpu()` equivalent in SE mode.
But thread-to-core assignment is deterministic (linear scan of halted contexts),
so you can infer it from creation order.

If you need to confirm at runtime, you can read the `mhartid` CSR
(RISC-V hardware thread ID) with inline assembly, but this is not standard
in user mode. A simpler approach: just rely on creation order and verify
via per-CPU statistics in the output.

### Measuring cycles

Use the `rdcycle` CSR:
```c
static inline unsigned long rdcycle(void) {
    unsigned long c;
    asm volatile ("rdcycle %0" : "=r"(c));
    return c;
}
```

In SE mode with `TimingSimpleCPU`, `rdcycle` returns the simulated cycle count.
This is the right way to measure latency in the hop-distance test (3b).

### Cache flush for cold-miss testing

SE mode does not support privileged cache flush instructions.
To force cold misses, you have two options:

1. **Access a large working set** that evicts previous data from L1/L2.
   With typical L1 = 32 KiB and L2 = 256 KiB, reading 512 KiB of unrelated
   data between measurements flushes both levels.

2. **Use `m5_dump_reset_stats` / `m5_reset_stats` pseudo-ops** to reset
   statistics between phases, so you can isolate cold-miss traffic.
   Include `#include "gem5/m5ops.h"` and link with the m5 ops library.

### Memory allocation for targeted addresses

To target specific HN-F nodes, you need to control the physical address,
which you cannot directly.
But since interleaving is at cache-line granularity (bits [9:6] select HN-F),
and physical pages are allocated sequentially from the pool,
a large contiguous allocation will naturally span all 16 HN-F slices.

For the smoke test (3a), a sequential sweep of a large array is sufficient.
For the hop-latency test (3b), you can't guarantee *which* HN-F a specific
address maps to, but you can observe the distribution in statistics
and pick addresses empirically after one run.

### Barrier implementation with atomics

For the barrier test (3e), use RISC-V atomics directly:

```c
#include <stdatomic.h>

volatile int barrier_counter = 0;

void barrier_wait(int round, int num_threads) {
    int target = num_threads * (round + 1);
    __atomic_fetch_add(&barrier_counter, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&barrier_counter, __ATOMIC_SEQ_CST) < target)
        ;  // spin
}
```

GCC compiles `__atomic_fetch_add` to `amoadd.w` on RISC-V,
which Ruby/CHI handles correctly.

**Note on spinning:** In SE mode, a spinning thread consumes simulation cycles
on its CPU. This is realistic behavior (it models the actual contention cost).
The futex-based suspension only happens when glibc's pthread mutex/cond
decides to call `futex(WAIT)` after spinning fails.
For a simple atomic barrier like the one above, all threads spin,
which is what we want for measuring contention.

### Verifying results

After simulation, check:
- `stats.txt` for per-controller counters
  (e.g., `system.ruby.hnf0.cntrl.L3cache.m_demand_hits`)
- `stats.txt` for Garnet per-link flit counts
  (e.g., `system.ruby.network.int_links.int_link0.link_utilization`)
- `stats.txt` for DDR controller access counts
  (e.g., `system.mem_ctrls0.readReqs`, `system.mem_ctrls1.readReqs`)
- Program stdout for "PASS" and any printed cycle counts

---

## 8. Summary of Key Constraints

| Topic | Key fact |
|-------|---------|
| Max threads | 16 total (1 main + 15 pthreads) with 16 CPUs |
| Thread→CPU mapping | Deterministic: linear scan of halted contexts, so creation order = CPU order |
| Address space | Shared: all threads see same virtual → physical mapping (CLONE_VM) |
| Physical pages | Allocated sequentially from `[0x0, 0x20000000)` pool |
| DDR interleaving | Bit 6: even cache lines → DDR0 (router 0), odd → DDR1 (router 15) |
| HN-F interleaving | Bits [9:6]: each cache line homed at one of 16 HN-F nodes |
| Atomic ops | `amoadd.w`, `amoswap.w`, `lr`/`sc` all work via Ruby/CHI |
| Futex | Supported: WAIT, WAKE, REQUEUE, WAKE_OP |
| pthreads | Works with `-static -lpthread`; glibc pthreads uses clone + mmap + futex |
| Cache flush | No privileged flush; use large working set to evict |
| Cycle measurement | `rdcycle` CSR works in SE mode |
