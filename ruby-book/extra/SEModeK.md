# Understanding gem5 SE (Syscall Emulation) Mode

This document provides a comprehensive reference for gem5's SE mode, essential for writing test programs for Chapter 17's 16-core CHI system.

---

## 1. SE Mode Overview

### What is SE Mode?

Syscall Emulation (SE) mode provides a lightweight execution environment where gem5 emulates the target application's system calls rather than running a full operating system.

**Key Characteristics:**

| Aspect | SE Mode | FS (Full System) Mode |
|--------|---------|----------------------|
| OS Kernel | Emulated system calls | Real Linux kernel |
| Boot Process | Direct process creation | Firmware → bootloader → kernel → init |
| Context Switch | Cooperative only | Preemptive scheduling |
| Device Models | Minimal file emulation | Full device emulation |
| Startup Time | Instant | Minutes |
| Thread Migration | Not supported | Supported |

**When to use SE mode:**

- Benchmarking CPU microarchitecture
- Memory system studies (cache, coherence, NoC)
- Protocol validation
- Single/multi-threaded application simulation

**Limitations:**

- No kernel context switches (cooperative scheduling only)
- No virtual memory subsystem
- No device drivers beyond basic file I/O
- Static thread-to-core assignment (no migration)

### Architecture

SE mode consists of three key components:

```
┌─────────────────────────────────────────────────────────────┐
│                    SE Workload Layer                        │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐    │
│  │ Process  │  │ Process  │  │ Process  │  │  ...     │    │
│  │  (PID 1) │  │  (PID 2) │  │  (PID N) │  │          │    │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └──────────┘    │
│       │             │             │                         │
│       └─────────────┴─────────────┘                         │
│                   SEWorkload                                 │
│              (System call dispatch)                          │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────┴────────────────────────────────────┐
│                    gem5 Core Layer                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐    │
│  │ ThreadCtx│  │ ThreadCtx│  │ ThreadCtx│  │  ...     │    │
│  │   (CPU0) │  │   (CPU1) │  │   (CPU2) │  │          │    │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └──────────┘    │
│       │             │             │                         │
│       └─────────────┴─────────────┘                         │
│                    System                                    │
│         (Physical memory, event queue)                       │
└─────────────────────────────────────────────────────────────┘
```

**Code anchors:**

- `src/sim/se_workload.hh` / `.cc` — SEWorkload class
- `src/sim/process.hh` / `.cc` — Process management
- `src/sim/syscall_emul.hh` / `.cc` — System call implementations

---

## 2. Memory Allocation and Layout

### Virtual Address Space Layout

RISC-V 64-bit SE mode uses a fixed virtual memory layout:

```
Virtual Address Space (RISC-V 64-bit)

High Addresses
0x7FFFFFFFFFFFFFFF ┌─────────────────────┐
                   │        Stack        │ ← Grows downward
                   │    (user space)     │   From stack_base
                   │                     │   Default: 8GB - 1
                   ├─────────────────────┤
                   │                     │
                   │        ...          │   Unmapped
                   │                     │
                   ├─────────────────────┤
                   │        Heap         │ ← Grows upward
                   │   (program break)   │   From brk_point
                   ├─────────────────────┤   (next page after
                   │   BSS/Data/Code     │    code/data)
                   │   (ELF segments)    │
0x0000000000400000 ├─────────────────────┤ ← Typical text start
                   │                     │
                   │        ...          │   Reserved
                   │                     │
0x4000000000000000 ├─────────────────────┤ ← mmap region start
                   │   Memory-mapped     │ ← Grows upward
                   │     regions         │   (files, anonymous)
                   └─────────────────────┘
Low Addresses

Source: src/arch/riscv/process.cc:75-81
```

**Key addresses (RISC-V 64-bit):**

| Region | Start Address | End Address | Notes |
|--------|---------------|-------------|-------|
| Stack | Dynamic | 0x7FFFFFFFFFFFFFFF | Grows downward |
| Mmap | 0x4000000000000000 | Dynamic | Grows upward |
| Code/Data | ELF-defined | ELF-defined | Loaded from binary |
| Heap | ELF max + page | Dynamic | brk point |

**Code anchors:**

- `src/arch/riscv/process.cc:71-82` — RiscvProcess64 initialization
- `src/sim/mem_state.hh:67-82` — MemState constructor parameters

### Memory Region Types

#### 1. Code and Static Data

Loaded from the ELF binary during `Process::initState()`:

```cpp
// In Process::initState() (src/sim/process.cc:288-308)
void Process::initState() {
    ThreadContext *tc = system->threads[contextIds[0]];
    tc->activate();

    // Create virtual-to-physical translator
    initVirtMem.reset(new SETranslatingPortProxy(tc, ...));

    // Write ELF segments to simulated memory
    image.write(*initVirtMem);
    interpImage.write(*initVirtMem);
}
```

**Allocation mechanism:**

- Virtual addresses: Defined in ELF program headers
- Physical pages: Allocated on demand when first accessed
- Page table entries: Created during image loading

#### 2. Stack

Initialized by `argsInit()` in architecture-specific Process class:

```cpp
// From src/arch/riscv/process.cc:135-200
const Addr stack_base = 0x7FFFFFFFFFFFFFFFL;
const Addr max_stack_size = params.maxStackSize;

// Stack layout (grows downward from stack_base):
//   [Random bytes for AT_RANDOM]
//   [argv strings]
//   [envp strings]
//   [argv array (pointers)]
//   [envp array (pointers)]
//   [auxiliary vector (auxv)]
//   [argc]
```

**Stack initialization flow:**

1. Calculate required size (args + env + auxv + metadata)
2. Map stack region in VMA list
3. Write argv/envp strings
4. Write pointer arrays
5. Set stack pointer register

**Thread stacks:** Each thread gets its own stack region:

```
Thread 0 stack: [stack_base - 0*max_stack_size] to [stack_base - 1*max_stack_size]
Thread 1 stack: [stack_base - 1*max_stack_size] to [stack_base - 2*max_stack_size]
Thread N stack: [stack_base - N*max_stack_size] to [stack_base - (N+1)*max_stack_size]
```

**Code anchors:**

- `src/arch/riscv/process.cc:135-263` — RiscvProcess::argsInit()
- `src/sim/mem_state.cc:1-100` — Stack region management

#### 3. Heap

Managed by the `brk` (program break) system call:

```cpp
// brk() syscall handler (src/sim/syscall_emul.hh:163-164)
SyscallReturn brkFunc(SyscallDesc *desc, ThreadContext *tc, VPtr<> new_brk);

// Updates memState->_brkPoint
void MemState::updateBrkRegion(Addr old_brk, Addr new_brk) {
    if (new_brk > old_brk) {
        // Expanding heap - allocate new pages
        allocateMem(old_brk, new_brk - old_brk);
    } else if (new_brk < old_brk) {
        // Shrinking heap - deallocate pages
        deallocateMem(new_brk, old_brk - new_brk);
    }
}
```

**Heap characteristics:**

- Starts at `roundUp(image.maxAddr(), PageBytes)`
- Grows upward toward higher addresses
- Allocated in page-size chunks on demand
- Pages zero-filled on first access

#### 4. Memory-Mapped Regions (mmap)

Created via `mmap()` system call:

```cpp
// mmap() syscall handler (src/sim/syscall_emul.hh)
SyscallReturn mmapFunc(..., Addr start, size_t length, ...);
```

**mmap region:**

- Starts at `0x4000000000000000` (RISC-V 64-bit)
- Grows upward
- Can be file-backed or anonymous
- Supports MAP_SHARED and MAP_PRIVATE

### Physical Page Allocation

#### MemPools Architecture

Physical memory is managed by `MemPools`, which contains one `MemPool` per memory controller:

```cpp
// From src/sim/mem_pool.hh:45-112
class MemPools : public Serializable {
    std::vector<MemPool> pools;  // One per memory range

    // Allocate from specific pool (default: pool 0)
    Addr allocPhysPages(int npages, int pool_id=0);
    void deallocPhysPages(Addr page_addr, int npages, int pool_id=0);
};

class MemPool {
    Counter startPageNum;       // First page number
    Counter _totalPages;        // Total pages in pool
    FreeList<Addr> freePhysPages;  // Available pages

    Addr allocate(Addr npages);  // Returns physical address
    void deallocate(Addr start, Addr npages);
};
```

**Allocation flow:**

```
1. Process::allocateMem(vaddr, size)
        ↓
2. SEWorkload::allocPhysPages(npages, pool_id=0)
        ↓
3. MemPools::allocPhysPages(npages, pool_id)
        ↓
4. MemPool::allocate(npages)
        ↓
5. Return physical address: page_number << pageShift
        ↓
6. pTable->map(vaddr, paddr, size)  // Create V→P mapping
```

**Page size:** Determined by architecture (4KB default for RISC-V).

**Code anchors:**

- `src/sim/mem_pool.cc:155-165` — MemPools::populate() and allocPhysPages()
- `src/sim/process.cc:317-345` — Process::allocateMem()

#### Virtual-to-Physical Translation

```cpp
// From src/mem/page_table.hh
class EmulationPageTable {
    std::unordered_map<Addr, Entry> table;  // VPN → Entry

    struct Entry {
        Addr paddr;      // Physical page number
        uint64_t flags;  // Permissions, cacheability
    };

    bool translate(Addr vaddr, Addr &paddr) {
        Addr vpn = vaddr >> pageShift;
        auto it = table.find(vpn);
        if (it == table.end()) return false;
        paddr = (it->second.paddr << pageShift) | (vaddr & pageMask);
        return true;
    }
};
```

**Page fault handling:**

```cpp
// From src/sim/mem_state.cc:409+
bool MemState::fixupFault(Addr vaddr) {
    // 1. Check if vaddr is in valid VMA
    // 2. Allocate physical page(s)
    // 3. Map in page table
    // 4. Return true if fixed
}
```

---

## 3. Memory Mapping to Multiple DDR Controllers

### Physical Memory Organization

In SE mode, physical memory is a flat address space mapped to one or more memory controllers:

```
Physical Address Space with 2 DDR Controllers

Controller 0 (SN-F at router 0):
├─ Range: 0x00000000 - 0x7FFFFFFF (2GB)
└─ Handles addresses where interleave_bit = 0

Controller 1 (SN-F at router 15):
├─ Range: 0x80000000 - 0xFFFFFFFF (2GB)
└─ Handles addresses where interleave_bit = 1

Interleaving scheme (--num-dirs=2):
Cache line size = 64 bytes → log2(64) = 6 bits
Interleave bit = bit 6 (line offset bits: 0-5)

Address layout:
[ Tag | Controller select (bit 6) | Line offset (bits 0-5) ]

Physical pages allocated from MemPools:
- MemPool 0: Controller 0's range
- MemPool 1: Controller 1's range
```

### How SE Mode Maps to Controllers

**Initialization flow:**

```cpp
// From src/sim/se_workload.cc:42-54
void SEWorkload::setSystem(System *sys) {
    // Query all memory ranges from PhysicalMemory
    AddrRangeList memories = sys->getPhysMem().getConfAddrRanges();

    // Create one MemPool per address range
    memPools.populate(memories);
}
```

**Dual-channel example:**

```python
# From config script using stdlib
from gem5.components.memory import DualChannelDDR4_2400

memory = DualChannelDDR4_2400(size="4GiB")
# Creates 2 controllers, each with 2GiB
# Contiguous physical address space split evenly
```

**PhysicalMemory address map:**

```cpp
// From src/mem/physical.hh:145
AddrRangeMap<AbstractMemory*, 1> addrMap;

// Maps physical address ranges to memory controller instances
// Multiple non-contiguous ranges supported
// Interleaved ranges merged for backing store
```

### NUMA Awareness

SE mode has limited NUMA support:

```cpp
// getcpu() syscall (src/sim/syscall_emul.cc:1442-1454)
SyscallReturn getcpuFunc(...) {
    if (cpu)
        *cpu = htog(tc->contextId(), ...);  // Returns thread context ID
    if (node)
        *node = 0;  // Always reports NUMA node 0
    return 0;
}
```

**Current limitations:**

- All memory appears as single NUMA node (node 0)
- No automatic first-touch allocation
- Manual pool selection via pool_id parameter (rarely used)

**For Chapter 17:** The 2 DDR controllers at routers 0 and 15 use address interleaving, not NUMA partitioning. Consecutive cache lines alternate between controllers.

---

## 4. Thread Model in SE Mode

### Thread Creation and Management

#### Thread States

```cpp
// From src/cpu/thread_context.hh:99-117
enum Status {
    Active,      // Running normally
    Suspended,   // Temporarily inactive (quiesced)
    Halting,     // Trying to exit
    Halted       // Permanently shut down
};
```

#### Creating Threads

Threads are created via the `clone()` system call:

```cpp
// From src/sim/process.cc:167-263
void Process::clone(ThreadContext *old_tc, ThreadContext *new_tc,
                    Process *new_p, RegVal flags) {
    if (CLONE_VM & flags) {
        // Share memory address space
        new_p->pTable = pTable;  // Same page table
        new_p->memState = memState;  // Shared memory state
    } else {
        // Copy memory address space (fork)
        // Replicate all page mappings
    }

    if (CLONE_FILES & flags) {
        // Share file descriptors
        new_p->fds = fds;
    }

    if (CLONE_THREAD & flags) {
        // Same thread group
        new_p->_tgid = _tgid;
    }
}
```

**pthread_create flow:**

```
1. Application calls pthread_create()
        ↓
2. glibc calls clone(CLONE_VM | CLONE_FS | CLONE_FILES | ...)
        ↓
3. SE mode clone handler creates new Process
        ↓
4. Process assigned to available ThreadContext
        ↓
5. Thread starts executing at entry point
```

#### Thread Synchronization

**Futex-based synchronization:**

```cpp
// Futex syscalls (src/sim/syscall_emul.hh:381-510)
SyscallReturn futexFunc(..., int op, int val, ...);

// Operations:
// - FUTEX_WAIT: Atomically check value and sleep if match
// - FUTEX_WAKE: Wake up N waiters
// - FUTEX_REQUEUE: Move waiters to different futex
```

**SE mode futex implementation:**

```cpp
// From src/sim/futex_map.hh
class FutexMap {
    std::unordered_map<Addr, FutexList> futexes;

    // Key is physical address of futex variable
    // Value is list of waiting thread contexts
};
```

### Thread Scheduling

**Cooperative scheduling only:**

```cpp
// Suspend current thread (src/cpu/thread_context.cc:167-171)
void ThreadContext::quiesce() {
    if (status() == Active) {
        system->threads.quiesce(contextId());
    }
}

// Resume a thread
void ThreadContext::activate() {
    system->threads.resume(contextId());
}
```

**Scheduling primitives:**

| Primitive | Behavior | Use Case |
|-----------|----------|----------|
| `quiesce()` | Suspend until woken | FUTEX_WAIT, I/O wait |
| `activate()` | Resume execution | FUTEX_WAKE, timer expiry |
| `quiesceTick(tick)` | Suspend until tick | Timed sleep |

**No preemptive scheduling:** Threads run until they explicitly yield (system call, futex wait, etc.).

---

## 5. Determining Which Core a Thread Runs On

### ThreadContext Identifiers

Each thread has multiple identifiers:

```cpp
// From src/cpu/thread_context.hh:121-133
class ThreadContext {
    virtual int cpuId() const = 0;           // Physical core number
    virtual uint32_t socketId() const = 0;   // NUMA socket (usually 0)
    virtual int threadId() const = 0;        // SMT thread ID (0 for non-SMT)
    virtual ContextID contextId() const = 0; // Global context ID
};
```

**Identifier meanings:**

| Identifier | Type | Meaning | Range |
|------------|------|---------|-------|
| `contextId()` | `ContextID` (int) | Global thread index | 0 to num_cores-1 |
| `cpuId()` | int | Physical CPU core | 0 to num_cores-1 |
| `threadId()` | int | HW thread within core | 0 (SE mode) |
| `socketId()` | uint32_t | NUMA socket | 0 (SE mode) |

### Accessing from Application Code

**Method 1: getcpu() syscall**

```c
#include <sched.h>

int cpu, node;
getcpu(&cpu, &node);  // Returns context ID as CPU, 0 as node

// Example: Print which core we're running on
printf("Running on core %d\n", cpu);
```

**Implementation:**

```cpp
// src/sim/syscall_emul.cc:1442-1454
SyscallReturn getcpuFunc(...) {
    if (cpu)
        *cpu = htog(tc->contextId(), ...);  // Returns context ID
    if (node)
        *node = 0;  // Always node 0
    return 0;
}
```

**Method 2: rdcycle (RISC-V CSR)**

```c
// Read cycle counter (architecture-specific)
static inline uint64_t rdcycle() {
    uint64_t cycle;
    asm volatile ("rdcycle %0" : "=r" (cycle));
    return cycle;
}
```

**Method 3: gem5 pseudo-instructions (for debugging)**

```c
#include "gem5/asm/generic/m5ops.h"

// Get current tick count (useful for timing)
m5_reset_stats(0, 0);
m5_dump_stats(0, 0);
```

### Per-Core Data Structures

**Pattern for NUMA-aware programming:**

```c
// Array indexed by core ID
struct per_core_data {
    uint64_t counter;
    char pad[64 - sizeof(uint64_t)];  // Cache line padding
} core_data[MAX_CORES];

void thread_main() {
    int cpu = sched_getcpu();  // Get current core

    // Access data local to this core
    core_data[cpu].counter++;
}
```

---

## 6. Core Assignment and Selection

### Static Assignment

In SE mode, thread-to-core assignment is **static** — threads never migrate:

```
Configuration Time:
┌─────────────┐     ┌─────────────┐
│  Thread 0   │────→│   Core 0    │
│  Thread 1   │────→│   Core 1    │
│  Thread 2   │────→│   Core 2    │
│     ...     │     │     ...     │
└─────────────┘     └─────────────┘

Runtime:
Thread N always runs on Core N (no migration)
```

**Configuration in Python:**

```python
# From se_binary_workload.py
# Each CPU gets one thread context
for core in processor.get_cores():
    core.set_workload(process)  # Static 1:1 mapping
```

### Creating Multi-Threaded Workloads

**Shared process model:**

```python
# All cores share the same Process (shared memory)
process = Process(pid=100)
process.executable = binary_path
process.cmd = [binary_path] + args

for core in processor.get_cores():
    core.set_workload(process)  # Same process for all
```

**Thread startup sequence:**

```
1. Main thread starts on Core 0
2. pthread_create() called
3. SE mode clone handler invoked
4. New thread context assigned to next available core
5. Thread begins execution at specified start routine
```

### Core Selection for Chapter 17 Tests

**Pinning threads to specific cores:**

```c
// Set CPU affinity
#define _GNU_SOURCE
#include <sched.h>

void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

// Usage: Force thread to run on core 15
pin_to_core(15);
```

**Why this matters for Chapter 17:**

- **Test 3b (Hop latency):** Core 0 vs Core 15 distance measurement
- **Test 3c (False sharing):** Pin threads to opposite corners
- **Test 3d (Producer-consumer):** Producer on core 0, consumer on core 15
- **Test 3e (Barrier):** All 16 cores participate

---

## 7. Essential Topics for Chapter 17 Test Programs

### Binary Loading and Startup

**Compilation requirements:**

```makefile
# Makefile for Chapter 17 test programs
CC = riscv64-linux-gnu-gcc
CFLAGS = -O2 -static -Wall
LDFLAGS = -lpthread

# Static linking required for SE mode
# Pthread support for multi-threaded tests
```

**Binary sections loaded:**

| Section | Content | Permissions |
|---------|---------|-------------|
| `.text` | Code | R-X |
| `.rodata` | Read-only data | R-- |
| `.data` | Initialized data | RW- |
| `.bss` | Zero-initialized data | RW- |

### Memory Allocation Patterns

**Stack allocation:**

```c
// Automatic (stack) allocation
void stack_alloc_example() {
    char buffer[1024];  // Lives on stack
    // Automatically freed when function returns
}
```

**Heap allocation:**

```c
#include <stdlib.h>

void heap_alloc_example() {
    char *buffer = malloc(1024);  // Lives on heap
    // Physical pages allocated on first write (demand paging)
    buffer[0] = 1;  // Page fault → allocate → map
    free(buffer);
}
```

**Large array allocation (for LLC tests):**

```c
// For test 3a (Smoke test) — allocate 16× LLC size
size_t LLC_SIZE = 4 * 1024 * 1024;  // 4MB per slice
size_t ARRAY_SIZE = 16 * LLC_SIZE;
char *array = malloc(ARRAY_SIZE);

// Access one byte per cache line to force LLC allocation
for (size_t i = 0; i < ARRAY_SIZE; i += 64) {
    sum += array[i];
}
```

### Cache Flush Operations

**For cold-cache measurements (Test 3b):**

```c
// RISC-V cache flush instruction (CBO.FLUSH)
// Flush cache line containing 'addr'
static inline void flush_line(void *addr) {
    asm volatile ("cbo.flush %0" :: "r" (addr));
}

// Flush entire L1D cache (simplified)
void flush_l1_cache() {
    char buffer[L1_SIZE];
    for (size_t i = 0; i < L1_SIZE; i += 64) {
        flush_line(&buffer[i]);
    }
}
```

### Synchronization Primitives

**Atomic operations:**

```c
#include <stdatomic.h>

atomic_int counter = 0;

// Atomic increment
atomic_fetch_add(&counter, 1);

// Atomic compare-and-swap
int expected = 0;
atomic_compare_exchange_strong(&counter, &expected, 1);
```

**Memory fences (for producer-consumer):**

```c
// Release store (producer)
data->value = new_value;
atomic_thread_fence(memory_order_release);
flag->ready = 1;

// Acquire load (consumer)
while (!flag->ready);  // Spin
atomic_thread_fence(memory_order_acquire);
use(data->value);
```

### Statistics Collection

**Key statistics for Chapter 17:**

```
System-level:
- sim_seconds          # Total simulation time
- sim_insts            # Total instructions

Per-HN-F (LLC controller):
- m_demand_hits        # Cache hits
- m_demand_misses      # Cache misses

Garnet network:
- flits_received       # Total flits
- average_flit_latency # Network latency
- buffer_occupancy     # Router congestion

DRAM controllers:
- num_reads            # Read commands issued
- num_writes           # Write commands issued
- average_latency      # DRAM access latency
```

**Reading statistics:**

```bash
# After simulation, stats are in m5out/stats.txt
grep "system.ruby.l3_cntrl[0-9]*.m_demand_hits" m5out/stats.txt
```

### Debugging Techniques

**Enable debug flags:**

```bash
# Protocol-level debugging
./build/RISCV/gem5.opt --debug-flags=Ruby,CHI,RubySlicc ...

# Network-level debugging
./build/RISCV/gem5.opt --debug-flags=Garnet ...

# Memory debugging
./build/RISCV/gem5.opt --debug-flags=Memory,PageTable ...
```

**Common issues and solutions:**

| Symptom | Likely Cause | Solution |
|---------|-------------|----------|
| "Out of memory" | Heap exhausted | Increase `--mem-size` parameter |
| Page fault on valid address | VMA not mapped | Check mmap/brk return values |
| Thread not starting | Context not activated | Verify thread context assignment |
| Unexpected core ID | Wrong context mapping | Check `getcpu()` usage |
| All traffic to one DDR | Wrong num_dirs | Use `--num-dirs=2` |

### SE Mode vs FS Mode Considerations

**When writing test programs for Chapter 17:**

**SE Mode (what we use):**

- No kernel boot time
- No dynamic thread migration
- Cooperative scheduling
- Direct core ID mapping
- Fast iteration

**FS Mode (not used here):**

- Full OS stack
- Preemptive scheduling
- Thread migration possible
- Virtual CPUs may != physical cores
- Longer setup time

**Key implication:** In SE mode, `sched_getcpu()` returns the actual hardware context ID (0-15 for 16 cores). This makes the Chapter 17 tests deterministic — thread A on core 0 stays there for the entire simulation.

---

## 8. Code Reference Summary

### Key Files and Their Purposes

| File | Purpose | Key Classes/Functions |
|------|---------|----------------------|
| `src/sim/se_workload.hh/.cc` | SE mode entry point | `SEWorkload`, `allocPhysPages()` |
| `src/sim/process.hh/.cc` | Process management | `Process`, `allocateMem()`, `clone()` |
| `src/sim/mem_state.hh/.cc` | Memory state tracking | `MemState`, `fixupFault()` |
| `src/sim/mem_pool.hh/.cc` | Physical page allocation | `MemPools`, `MemPool::allocate()` |
| `src/sim/syscall_emul.hh/.cc` | System call handlers | `brkFunc()`, `mmapFunc()`, `getcpuFunc()` |
| `src/mem/page_table.hh` | V→P translation | `EmulationPageTable::translate()` |
| `src/mem/physical.hh/.cc` | Physical memory | `PhysicalMemory`, `AddrRangeMap` |
| `src/arch/riscv/process.cc` | RISC-V specifics | `RiscvProcess64`, `argsInit()` |
| `src/cpu/thread_context.hh` | Thread abstraction | `ThreadContext::contextId()`, `cpuId()` |
| `src/python/gem5/components/boards/se_binary_workload.py` | Python SE setup | `SEBinaryWorkload` |

### Data Flow Reference

**Memory allocation:**

```
Application:
  malloc() / mmap()
       ↓
C library:
  brk() / mmap() syscalls
       ↓
SE mode handler:
  brkFunc() / mmapFunc()
       ↓
Process::
  allocateMem()
       ↓
SEWorkload::
  allocPhysPages()
       ↓
MemPools::
  allocPhysPages(pool_id=0)
       ↓
MemPool::
  allocate() → physical address
       ↓
Process::
  pTable->map(vaddr, paddr)
```

**Thread identification:**

```
Application:
  getcpu(&cpu, &node)
       ↓
SE mode handler:
  getcpuFunc()
       ↓
ThreadContext::
  contextId()  // Returns 0-15
```

---

## 9. Quick Reference for Test Program Development

### Minimal Test Template

```c
// rbook_test_template.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <unistd.h>

// Get current core ID
static inline int get_core_id() {
    int cpu;
    getcpu(&cpu, NULL);
    return cpu;
}

// Pin to specific core
void pin_to_core(int core) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core, &mask);
    sched_setaffinity(0, sizeof(mask), &mask);
}

int main(int argc, char *argv[]) {
    printf("Running on core %d\n", get_core_id());

    // Test logic here

    printf("PASS\n");
    return 0;
}
```

### Compilation

```bash
# Cross-compile for RISC-V
riscv64-linux-gnu-gcc -O2 -static -o rbook_test_template rbook_test_template.c

# Or use the provided Makefile
make -C ruby-book/final
```

### Running the Test

```bash
./build/RISCV/gem5.opt \
    -d m5out/rbook-test-$(date +%Y%m%d-%H%M%S) \
    configs/example/rbook_mesh_config.py \
    --cmd=ruby-book/final/rbook_test_template
```

### Checking Results

```bash
# Check if simulation completed successfully
tail m5out/rbook-test-*/simout

# View key statistics
grep "PASS\|FAIL" m5out/rbook-test-*/simout
grep "sim_seconds" m5out/rbook-test-*/stats.txt
```

---

## 10. Summary

**SE mode essentials for Chapter 17:**

1. **Static thread-to-core mapping** — Each thread stays on its assigned core for the entire simulation. Use `getcpu()` to determine which core you're on.

2. **Flat physical memory** — Two DDR controllers appear as interleaved physical address ranges. SE mode allocates pages from MemPool 0 by default.

3. **Cooperative scheduling** — Threads run until they make a blocking system call. No preemption.

4. **Memory layout** — Stack at high addresses (grows down), heap in middle (grows up), mmap region at 0x4000000000000000.

5. **Demand paging** — Physical pages allocated on first access (page fault), not at allocation time.

6. **No NUMA awareness** — All memory appears as node 0. DDR controller selection happens via address interleving (bit 6 for 64-byte lines with 2 controllers).

**Key insight for writing test programs:**

SE mode is deterministic. Thread 0 on core 0 will always be on core 0. This allows you to write tests that depend on specific core locations (like the hop latency test measuring diagonal distance across the mesh).
