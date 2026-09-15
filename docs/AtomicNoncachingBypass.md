# How `atomic_noncaching` bypasses the cache hierarchy

`atomic_noncaching` is a **system-wide** memory mode, not a CPU property.
Everything below keys off one predicate:

```cpp
bool System::bypassCaches() const { return memoryMode == enums::atomic_noncaching; }
```
[`src/sim/system.hh:285`](../src/sim/system.hh#L285)

Callers never ask "is this CPU non-caching?" — they ask the `System`. So the
mode changes the behaviour of the crossbars, the caches, DMA engines and Ruby
simultaneously, for every requester in the system.

---

Sections 1–3 describe the port/backdoor paths. Section 4 describes
`DirectMemorySimpleCPU`, a sibling of `NonCachingSimpleCPU` under
`AtomicSimpleCPU`.

## 1. Classic memory system

### 1.1 Caches become transparent forwarders

`BaseCache` does not implement a bypass *path*; its **ports** short-circuit
before the cache is ever consulted. Every protocol entry point on the CPU-side
port checks the flag and hands the packet straight to `memSidePort`:

| Entry point | Bypass action | Location |
|---|---|---|
| `recvTimingReq` | `cache.memSidePort.sendTimingReq(pkt)`, asserts success | [`src/mem/cache/base.cc:2626`](../src/mem/cache/base.cc#L2626) |
| `recvAtomic` | `return cache.memSidePort.sendAtomic(pkt)` | [`src/mem/cache/base.cc:2642`](../src/mem/cache/base.cc#L2642) |
| `recvFunctional` | `cache.memSidePort.sendFunctional(pkt)`, no cache lookup | [`src/mem/cache/base.cc:2653`](../src/mem/cache/base.cc#L2653) |
| `tryTiming` | returns `true` unconditionally — never blocks, never retries | [`src/mem/cache/base.cc:2609`](../src/mem/cache/base.cc#L2609) |

Consequences: no tag lookup, no allocation, no MSHR, no writeback, **no cache
latency contribution**, and no cache stats. The comment on the functional path
states the contract plainly — *"The cache should be flushed if we are in cache
bypass mode, so we don't need to check if we need to update anything."*

The snoop-side handlers assert the mode is *not* active
([`src/mem/cache/base.cc:2596`](../src/mem/cache/base.cc#L2596),
[`base.cc:2695`](../src/mem/cache/base.cc#L2695),
[`base.cc:2705`](../src/mem/cache/base.cc#L2705),
[`base.cc:2714`](../src/mem/cache/base.cc#L2714)): in bypass mode no
snoop should ever reach a cache, because the crossbar stops generating them.

### 1.2 Crossbars stop snooping

`CoherentXBar` disables coherence work wholesale:

- `const bool snoop_caches = !system->bypassCaches() && ...` gates snoop
  forwarding on both the timing and atomic paths
  ([`src/mem/coherent_xbar.cc:199`](../src/mem/coherent_xbar.cc#L199),
  [`coherent_xbar.cc:755`](../src/mem/coherent_xbar.cc#L755))
- the snoop filter is skipped on request, response and snoop-response paths
  ([`coherent_xbar.cc:428`](../src/mem/coherent_xbar.cc#L428),
  [`coherent_xbar.cc:484`](../src/mem/coherent_xbar.cc#L484),
  [`coherent_xbar.cc:837`](../src/mem/coherent_xbar.cc#L837),
  [`coherent_xbar.cc:1020`](../src/mem/coherent_xbar.cc#L1020))
- express-snoop and snoop-response handlers `assert(!system->bypassCaches())`
  ([`coherent_xbar.cc:706`](../src/mem/coherent_xbar.cc#L706),
  [`coherent_xbar.cc:943`](../src/mem/coherent_xbar.cc#L943),
  [`coherent_xbar.cc:1072`](../src/mem/coherent_xbar.cc#L1072))

So a request walks CPU → xbar → (cache-as-wire) → xbar → memory controller with
no coherence traffic generated anywhere.

### 1.3 Backdoors (gem5's DMI) — the real fast path

Bypassing the cache still costs a chain of virtual `sendAtomic` calls per
access. The `MemBackdoor` mechanism removes even that. It is gem5's exact
analogue of SystemC TLM-2.0 DMI: the target hands the initiator a raw host
pointer plus an `AddrRange` and a flags set, with an invalidation callback.

**Target side.** `AbstractMemory` owns a `MemBackdoor` describing its backing
store, with `Readable`/`Writeable` flags and an `invalidate()` hook
([`src/mem/abstract_mem.cc:67-70`](../src/mem/abstract_mem.cc#L67-L70),
[`abstract_mem.cc:118-120`](../src/mem/abstract_mem.cc#L118-L120)). Both memory
models expose it:

- `SimpleMemory::recvAtomicBackdoor` / `recvMemBackdoorReq` → `getBackdoor()`
  ([`src/mem/simple_mem.cc:99-128`](../src/mem/simple_mem.cc#L99-L128))
- `MemCtrl::recvAtomicBackdoor` / `recvMemBackdoorReq` → `dram->getBackdoor()`
  ([`src/mem/mem_ctrl.cc:158-161`](../src/mem/mem_ctrl.cc#L158-L161),
  [`mem_ctrl.cc:1384-1391`](../src/mem/mem_ctrl.cc#L1384-L1391))

**Transport side.** Crossbars forward backdoor requests when
`enable_backdoor` is set — default `True`
([`src/mem/XBar.py:96`](../src/mem/XBar.py#L96)); see
`CoherentXBar::recvAtomicBackdoor`
([`src/mem/coherent_xbar.cc:819`](../src/mem/coherent_xbar.cc#L819)) and
`recvMemBackdoorReq`
([`coherent_xbar.cc:1002-1009`](../src/mem/coherent_xbar.cc#L1002-L1009)).

**Classic caches neither grant nor forward backdoors, even in bypass mode.**
`rg -i backdoor src/mem/cache/` returns zero hits.
A backdoor request that reaches a cache falls through to
`ResponsePort::recvAtomicBackdoor`, which DPRINTFs once and degrades to a plain
`recvAtomic` with no backdoor returned
([`src/mem/port.cc:225-233`](../src/mem/port.cc#L225-L233),
[`port.cc:236-244`](../src/mem/port.cc#L236-L244)).
The fallback atomic access still reaches memory: in `atomic_noncaching`, the
cache forwards it with `sendAtomic`, but the request for a backdoor has been
lost. Bypassing cache lookups therefore does **not** enable backdoor forwarding.

The decisive detail is the request's path, not whether the configuration
contains caches:

| Request path | Backdoor support |
|---|---|
| CPU → crossbar → memory, with caches attached elsewhere | Possible if crossbar backdoors are enabled and memory grants one |
| CPU → classic cache → memory | No, including in `atomic_noncaching`; ordinary atomic accesses still work |
| CPU → Ruby → memory | No through Ruby's port interface; see §2.3 |

A raw DRAM pointer cannot observe dirty lines in **active** caches. That does
not prevent backdoors while the entire hierarchy is bypassed and its contents
have been written back and invalidated. Forwarding backdoor requests through
bypassed classic caches is an unimplemented optimization, not a fundamental
restriction imposed by CPU switching.

**Initiator side — `NonCachingSimpleCPU`.** It requires the mode outright:

```cpp
if (!(system->isAtomicMode() && system->bypassCaches()))
    fatal("The direct CPU requires the memory system to be in the "
          "'atomic_noncaching' mode.\n");
```
[`noncaching.cc`](../src/cpu/simple/noncaching.cc)

`NonCachingSimpleCPU` retains the upstream `develop` implementation:

- `sendPacket()` uses `sendAtomicBackdoor()` and records granted backdoors
  in an `AddrRangeMap`. Invalidation callbacks remove revoked entries.
- `fetchInstMem()` can copy instruction bytes through a recorded backdoor.
- Data accesses use the inherited `AtomicSimpleCPU` packet path.

The optimized plain load/store and translated fetch-page paths live in
`DirectMemorySimpleCPU` (§4). Shared CPU and RISC-V optimizations remain
available to both models.

**Initiator side — DMA.** `DmaPort` keeps the same structure: in atomic mode it
picks `sendAtomicBdReq` over `sendAtomicReq` when `sys->bypassCaches()`
([`src/dev/dma_device.cc:406-416`](../src/dev/dma_device.cc#L406-L416)),
maintains its own `memBackdoors` map with invalidation callbacks
([`dma_device.cc:318-348`](../src/dev/dma_device.cc#L318-L348)), and
`DmaReadFifo` selects `resumeFillBypass()` — a single direct transfer — instead
of `resumeFillTiming()`
([`dma_device.cc:531-534`](../src/dev/dma_device.cc#L531-L534),
[`dma_device.cc:541-560`](../src/dev/dma_device.cc#L541-L560)).

### 1.4 Bypass is not a flush

Nothing above evicts anything. Lines already resident, dirty ones included,
stay in the caches and become invisible to bypassing accesses. That is why
`switchCpus` performs maintenance *before* entering the mode:

```python
if memory_mode == MemoryMode("atomic_noncaching").getValue():
    memWriteback(system)
    memInvalidate(system)
```
[`src/python/m5/simulate.py:531-547`](../src/python/m5/simulate.py#L531-L547)

This path is **not** Ruby-gated — it walks the classic hierarchy equally well.
`CoherentXBar::recvAtomicBackdoor` forwards backdoor requests gated only on
`enableBackdoor`, *not* on `bypassCaches()`. This permits obtaining a pointer on
a path through crossbars to memory, but does **not** make an intervening cache
forward backdoor requests. Backdoor availability is not itself a guarantee
that direct access is coherent with active caches.

### 1.5 Fast boot followed by a timing CPU is supported

Booting with `NonCachingSimpleCPU` in `atomic_noncaching` and then switching to
O3 or another compatible timing CPU does not require JitCPU. `switchCpus()`
drains execution, switches out the old CPU, changes the memory mode and hands
over architectural state. Starting from reset in noncaching mode leaves the
caches empty; the timing CPU subsequently begins filling them.

The limitation is the boot phase's **backdoor acceleration**, not the handoff:
if the boot CPU's ports lead through classic caches, its accesses bypass cache
lookups but still use atomic packets. Changing the memory mode does not change
that port topology or add the missing backdoor handlers.

An alternative implementation could forward backdoor requests through caches only while
`bypassCaches()` is true. It would also need to ensure that all requesters stop
using bypass pointers when caches become active, including DMA requesters that
may retain backdoors across a CPU switch. A reverse switch into noncaching mode
requires the writeback/invalidate maintenance described above. These are
implementation requirements, not a reason the one-way fast-boot workflow is
inherently unsafe.

The opt-in implementation in this repository follows KvmCPU's direct
backing-store mapping approach; see §4. It does not require cache backdoor
forwarding.

---

## 2. Ruby

Ruby's relationship to the mode is much simpler, and much stricter.

### 2.1 Atomic mode is *only* legal as `atomic_noncaching`

Both Ruby response ports panic otherwise:

```cpp
// Only atomic_noncaching mode supported!
if (!owner.system->bypassCaches())
    panic("Ruby supports atomic accesses only in noncaching mode\n");
```
[`src/mem/ruby/system/RubyPort.cc:238`](../src/mem/ruby/system/RubyPort.cc#L238)
(PIO) and [`RubyPort.cc:366`](../src/mem/ruby/system/RubyPort.cc#L366) (memory)

### 2.2 The atomic path skips the sequencer, the caches and the network

`RubyPort::MemResponsePort::recvAtomic` does not enqueue a `RubyRequest`. It
routes the address to the owning directory/memory machine and calls straight
into it:

```cpp
MachineID id = owner.m_controller->mapAddressToMachine(pkt->getAddr(), mem_interface_type);
AbstractController *mem_interface = rs->m_abstract_controls[mem_interface_type][id.getNum()];
Tick latency = mem_interface->recvAtomic(pkt);
if (access_backing_store)
    rs->getPhysMem()->access(pkt);
```
[`src/mem/ruby/system/RubyPort.cc:406-415`](../src/mem/ruby/system/RubyPort.cc#L406-L415)

and `AbstractController::recvAtomic` is just
`ticksToCycles(memoryPort.sendAtomic(pkt))`
([`src/mem/ruby/slicc_interface/AbstractController.cc:420-423`](../src/mem/ruby/slicc_interface/AbstractController.cc#L420-L423)).

So the L1/L2 cache controllers, the SLICC state machines, the message buffers
and the interconnect are never touched — **zero network messages, zero cache
accesses, zero router traversals**. Non-PIO addresses that are not physical
memory are punted to the PIO port instead
([`RubyPort.cc:373-386`](../src/mem/ruby/system/RubyPort.cc#L373-L386)).

### 2.3 No backdoor support in Ruby

`RubyPort` implements none of the backdoor methods. A backdoor request
therefore falls back to an ordinary atomic access without returning a host
pointer; see [`port.cc`](../src/mem/port.cc). The optional direct mode in §4
obtains host pointers from `PhysicalMemory` instead.

`PhysicalMemory` merges compatible interleaved controller ranges into a
contiguous host allocation. The per-controller backdoor interface does not
expose that allocation. Ruby's optional reference
memory is an additional, separate data image; see §4.5 for the consequence
when mixing direct accesses with packet-based atomics.

---

## 3. Summary

| Layer | Mechanism | Cost of an access |
|---|---|---|
| Classic cache | port-level short-circuit to `memSidePort` | one extra virtual call, no lookup |
| Classic xbar | `snoop_caches = false`, snoop filter skipped | no coherence traffic |
| Classic memory | `MemBackdoor` granted to CPU / DMA if the request path supports it; classic caches block it | host `memcpy` for eligible accesses after a grant |
| Ruby | `recvAtomic` routed direct to the directory controller | one `sendAtomic` to the memory port |
| Ruby | optional CPU direct backing-store map (§4) | host access for eligible requests |

Two invariants hold throughout:

1. **Bypass never flushes.** Cache maintenance is the caller's job, and
   `switchCpus` does it on the way *into* the mode.
2. **The current cache/Ruby ports do not grant or forward backdoors.** This
   is an implementation limitation during bypass, not a prohibition on mapping
   RAM while the hierarchy is inactive.

## 4. Direct backing-store access like KvmCPU

`DirectMemorySimpleCPU` derives directly from `AtomicSimpleCPU` and has no
backdoor cache or backdoor invalidation callbacks. Select it with
`--cpu-type direct` in `configs/example/riscv/noncaching_fs.py`. It discovers
eligible system RAM automatically; no CPU-side memory list is needed.

This works with cacheless, classic L1/L2/L3, and Ruby CHI/SimpleNetwork
configurations, including 1/2/4 interleaved DDR4 channels. The default
`--cpu-type noncaching` selects the upstream backdoor CPU. The former
`--direct-memory` flag and `direct_memory` CPU parameter have been removed.

### 4.1 Reuse the allocation and mapping interface

Like `KvmVM::delayedStartup()`, the CPU enumerates
`system->getPhysMem().getBackingStore()`. `PhysicalMemory` records the exact
memory owners when constructing each `BackingStoreEntry`. Compatible
interleaved controllers share one contiguous allocation with several owners.

The accessor returns a const reference. Entries are stable after construction,
including across checkpoint restore, until `PhysicalMemory` is destroyed.
The CPU caches only pointers to the last fetch/data entries, without copying
bounds, RAM or owner lists. There is no separate CPU mapping collection.
Host and owner pointers are runtime metadata, never checkpoint data.

`BackingStoreEntry::isDirectAccessible()` selects contiguous, address-mapped,
`kvmMap` allocations with non-null, non-sparse memory owners. Reference-memory
allocations outside the address map, null memory and sparse owners remain on
the packet path. Setting a memory object's `kvm_map=False` excludes it from
direct access as well as KVM. A configuration with no eligible allocation is
rejected. Startup and takeover verify that eligible owners share the CPU's
system and event queue.

Selecting the direct CPU requires static RAM with identity address routing
and one authoritative data image. Address-transforming interconnects,
device overlays within selected RAM, hotplug and concurrent host writers are
outside this contract. Automatic discovery and `kvmMap` alone do not establish
these properties. Sources: [`PhysicalMemory`](../src/mem/physical.cc),
[`KvmVM`](../src/cpu/kvm/vm.cc),
[`direct access initialization`](../src/cpu/simple/direct_memory.cc).

### 4.2 Access selection

| Condition | Path |
|---|---|
| Eligible ordinary RAM fetch/load | Direct host access |
| Eligible ordinary RAM store satisfying §4.3 | Direct host access |
| MMIO, local, uncacheable or strictly ordered request | Existing handler |
| LR/SC, AMO, swap, masked write or cache management | Existing handler |
| Mapping miss or ineligible store | Atomic packet |
| Inactive CPU or mode other than `atomic_noncaching` | No direct access |

Translation, PMP/PMA checks and request attributes still precede host access.
The existing fragment path handles accesses crossing cache lines or pages;
each direct fragment must fit entirely inside its mapping, checked without
address-addition overflow. ROM reads may be direct; writes use packets to
preserve the memory owner's write protection.

Instruction fetch retains the translation-epoch and uniform-page checks.
Only the host location is cached: every fetch reads the current instruction
bytes, preserving self-modifying code behavior.

Instruction/data stall simulation must be disabled. Direct accesses skip
memory-port latency, statistics and tracing; CPU instruction accounting is
unchanged. Use the default port mode when memory-port instrumentation matters.

### 4.3 Preserve software reservations

Completing individual accesses synchronously does not make an LR → computation
→ SC sequence indivisible. A reservation records whether the SC may succeed.
KVM executes guest atomics through native hardware; reusing its storage mapping
does not supply that mechanism to a software CPU.

LR/SC and AMOs retain their packet handlers. Ordinary direct stores are allowed
only with one system thread and no outstanding reservation in **any** memory
owner of the mapping. `BackingStoreEntry::canDirectWrite()` checks every
owner's write permission and checks that its existing lock list is empty,
a constant-time check per controller with no traversal of reserved
addresses. This avoids maintaining a second eligibility flag or installing
invalidation callbacks. Both newly created and restored locks are visible
immediately. A lock causes packet fallback until the existing memory handlers
remove it; the optimization never clears locks itself.

This preserves the current packet behavior of LR → same-hart ordinary store
→ SC: the ordinary store removes the matching memory-side reservation, so the
SC fails. RISC-V does not universally require a same-hart store to invalidate
a reservation, but this implementation preserves gem5's existing result.
Multi-context stores remain on packets for reservation notifications and CPU
address monitors. Sources:
[`AbstractMemory`](../src/mem/abstract_mem.cc),
[`reservation query`](../src/mem/abstract_mem.hh),
[`RISC-V reservation handlers`](../src/arch/riscv/isa.cc).

### 4.4 Cache-management instructions

With empty, bypassed caches, clean/invalidate operations have no cached data
to act on. They do not require extra flushes or invalidation
of the CPU's RAM pointers. Their translation, access checks and exception
behavior still matter, so they retain the existing handlers and cannot become
ordinary byte-copy stores. `cbo.zero` writes real zeros and likewise retains
its existing store handling, including reservation effects.
Source: [`AbstractMemory::access`](../src/mem/abstract_mem.cc).

### 4.5 Ruby uses the controllers' RAM

The example sets `RubySystem.access_backing_store=False`. Direct accesses,
Ruby's atomic packet fallback and functional image loading all use the
controllers' RAM. Interleaving changes address ownership, not this allocation,
so direct stores followed by packet AMOs/SCs see the same data and reservation
state. CHI caches and SimpleNetwork remain bypassed during noncaching
execution.

Ruby's optional separate reference memory is unsupported here. Selecting a
reference-store pointer alone would not make mixed direct and packet atomics
safe: the atomic path can touch both controller and reference images.
Sources: [`example configuration`](../configs/example/riscv/noncaching_fs.py),
[`RubyPort`](../src/mem/ruby/system/RubyPort.cc).

### 4.6 One-way timing takeover and checkpoints

The supported workflow starts with empty caches, boots in noncaching mode,
and switches once to TimingSimpleCPU. Both CPUs use **MSU**, without H. The
example's `--switch-to-timing` option performs this handoff at the first guest
`m5 workbegin` marker. The benchmark Linux build disables KVM and includes
that marker after userspace starts. No RAM copy or cache writeback is needed
on this one-way transition: backing memory already contains the boot state.

The direct CPU inherits atomic switch-out handling; its cached pointers are
unused while switched out and cleared on takeover before it resumes. The
existing `m5.switchCpus()` drains execution, changes the memory mode, and
transfers architectural state and ports. Classic
snoop filters have headroom beyond resident cache capacity to accommodate
outstanding requests when timing caches become active.

Startup after image loading or checkpoint restore revalidates the stores and
clears cached pointers. No raw host pointer is serialized, and store eligibility
always consults current memory-side reservations. Direct CPU takeover also
revalidates the stores and clears its caches.

Returning from dirty timing caches requires writeback and invalidation before
direct access resumes. `m5.switchCpus()` performs that maintenance when entering
`atomic_noncaching`. The example remains a one-way workflow; the multicore
regressions in §4.8 also validate classic Direct → Timing → Direct switching.
Sources:
[`switchCpus`](../src/python/m5/simulate.py),
[`CPU lifecycle`](../src/cpu/simple/direct_memory.cc),
[`Linux builder`](../util/riscv-bench/build-linux.sh).

### 4.7 Validation

The CPU split was checked against upstream `develop` at
`59dc5d8acd40e9c584ee517108e44ae84273c24c`: the NonCachingSimpleCPU C++ and
Python files match that revision exactly. Shared CPU and RISC-V improvements
remain in this branch.

The automatic-discovery change passed 25 runtime checks with the RISC-V
`gem5.fast` build:

- SimpleMemory and classic/Ruby DDR4 with 1, 2 and 4 interleaved controllers,
  including load/store, LR/SC, AMO and reservation invalidation on each channel.
- Read-only, excluded, null and reference memory, plus an access crossing two
  adjacent backing allocations.
- Rejection of an empty eligible set, a foreign event queue and the removed
  `direct_memory` parameter.
- Classic/Ruby checkpoint save and restore with outstanding reservations,
  multiple CPU contexts and direct CPU takeover.
- Full CoreMark and Linux in SimpleMemory, classic/four-channel DDR4 and
  Ruby/four-channel DDR4, plus Linux direct-to-timing takeover with RAM and
  timer checks.

CoreMark and Linux retained the baseline instruction counts and simulated
ticks. Targeted builds of the KVM VM and shared-memory server sources also
passed with the const-reference accessor. KVM hardware execution was not tested.

See the measurements and validation record in
[RiscvNonCachingPerf.md](RiscvNonCachingPerf.md).
Scope extensions, especially concurrent host execution, address transformation
or Ruby reference memory, need separate correctness tests before being enabled.

### 4.8 Multicore bugs and fixes

Bare-metal tests with concurrently executing harts exposed four bugs in the
shared `AtomicSimpleCPU` and `AbstractMemory` paths used by the direct CPU.
Keeping multicore stores on packets was necessary, but those packet handlers
also needed the following fixes. The `NonCachingSimpleCPU` source remains
unchanged and benefits from the same shared fixes.

1. **Stale context IDs after CPU takeover.** Reused fetch, read, write and AMO
   requests retained their pre-takeover context IDs. Replacement CPUs could
   all issue requests as context zero. In a forced sequence of hart 0's LR,
   hart 1's store and new LR, then hart 0's SC, hart 0 could incorrectly use
   hart 1's reservation. `AtomicSimpleCPU::takeOverFrom()` now refreshes all
   four request IDs from the inherited thread context.
2. **AMOs did not invalidate reservations.** `AbstractMemory::access()` handled
   AMOs separately from ordinary stores and skipped `writeOK()`. An observed
   AMO from another hart could therefore leave SC eligible to succeed, losing
   updates in a counter mixing AMO and LR/SC increments. AMOs and successful
   swaps now pass through the memory's write checks and reservation handling.
   A failed conditional swap does not invalidate reservations.
3. **Wide writes invalidated only their first reservation granule.** The
   memory's lock list tracks 16-byte granules, but invalidation compared only
   the start address. Vector stores, `cbo.zero` and misaligned stores could
   overwrite a reserved word later in the packet without invalidating its
   reservation. Invalidation now covers all granules overlapped by the write.
4. **AMOs and swaps bypassed write protection.** The same missing `writeOK()`
   calls allowed these operations to modify read-only backing memory. They
   now preserve its contents; the read-only AMO regression checks both the
   returned value and the unchanged memory value. The test marks ROM
   uncacheable so the memory owner also enforces protection in timing mode.

Sources: [`AtomicSimpleCPU::takeOverFrom`](../src/cpu/simple/atomic.cc),
[`AbstractMemory`](../src/mem/abstract_mem.cc). The required failure of SC after
an observed conflicting write from another hart follows the
[RISC-V atomic extension](https://docs.riscv.org/reference/isa/unpriv/a-st-ext.html).

The [multicore regression suite](../util/riscv-bench/multicore/README.md)
contains 12 scenarios with explicit handshakes, per-hart stacks, trap reporting
and time limits. Besides the failure reproducers, it tests competing SCs,
contended counters, producer-consumer messages and publication of executable
instructions followed by `fence.i`. Reservation targets span four cache lines
to exercise every channel of a four-owner backing allocation.

The pre-fix binary at `0c0917c3f0` fails the AMO, vector, zeroing, misaligned,
mixed-counter and read-only reproducers with two harts and one memory owner.
It also fails the reservation-reuse reproducer after CPU switching.

After the fixes, **132 multicore runs passed** across 2/4/8 harts, 1/4 memory
owners, SimpleMemory and DDR4, classic and Ruby CHI, and mixed direct/noncaching
CPUs. Switching tests execute each scenario before, between and after two
switches, including classic Direct → Timing → Direct and repeated Ruby
Direct → Direct. The RISC-V build, repository checks, single-core CoreMark and
Linux smoke tests also passed. These results cover the listed configurations;
they do not establish support for arbitrary protocols or parallel host event
queues.

### 4.9 Deeper bare-metal audit

The follow-up audit used `f48a0bd118` as the pre-fix baseline. It found
additional bugs in shared RISC-V and simple-CPU paths; the direct CPU's
backing-store lookup did not need another change. `NonCachingSimpleCPU`'s
selected upstream source remains unchanged.

| Finding | Reproducer and correction |
|---|---|
| Invalid reservations at physical address zero; stale reservations when reusing a CPU | A failed SC consumes the ISA reservation but can leave a memory-side lock. An empty ISA map implicitly contained address zero, and a returning CPU retained its previous LR. `reservation-zero`, `reservation-return`, and `checkpoint-zero` incorrectly succeeded on the baseline. Missing entries now start invalid; clear, takeover and restore discard ISA reservations. |
| Partial PMP matches skipped the higher-priority entry | `pmp-partial-s` and `pmp-partial-m` cross a four-byte PMP entry into a later allow-all entry. The baseline allowed the access. The first enabled entry matching any byte must cover the entire operation, including in M-mode. |
| Ordinary misaligned-memory support also enabled misaligned atomics | `misaligned-lr`, `misaligned-sc` and `misaligned-amo` executed instead of trapping. LR/SC and AMOs now retain natural alignment checks. This PMA configuration does not model a misaligned atomicity granule. |
| Cross-line AMOs crashed the host before the guest could trap | `misaligned-amo-crossline` panicked in both simple-CPU execution modes. Atomic and timing CPUs now allow translation to report the guest fault before rejecting an otherwise unsupported cross-line AMO. |
| Masked vector memory requests included inaccessible elements | A fully masked unit-stride load to unmapped memory caused a missing-destination fatal error. Masked loads/stores crossing an eight-byte PMP region also faulted on inactive elements. Masked unit-stride operations now use one request per element and preserve the destination between load microops. Ordinary unmasked operations retain register-sized requests. |
| Fault-only-first loads lost accessible prefixes or retained old fault state | An unmasked load whose first element was accessible incorrectly trapped when later elements failed PMP. These loads now access elements separately, reset fault state on execution, and trim VL using the first suppressed fault's absolute element index. Tests repeat the same instruction with a different mask and also cover VL zero. |
| Timing execution did not honor disabled requests or fault suppression | TimingSimpleCPU now completes all-disabled accesses without translating them. For an unsplit read translation fault it replays instruction initiation with the completed fault, following MinorCPU's approach, so fault-only-first instructions can suppress it before completion. |

Sources: [RISC-V reservation handling](../src/arch/riscv/isa.cc),
[PMP](../src/arch/riscv/pmp.cc), [PMA](../src/arch/riscv/pma_checker.cc),
[vector memory templates](../src/arch/riscv/isa/templates/vector_mem.isa),
[vector-length trimming](../src/arch/riscv/insts/vector.cc),
[AtomicSimpleCPU](../src/cpu/simple/atomic.cc), and
[TimingSimpleCPU](../src/cpu/simple/timing.cc).
The architectural requirements are in the
[atomic extension](https://docs.riscv.org/reference/isa/v20240411/unpriv/a-st-ext.html),
[PMP specification](https://docs.riscv.org/reference/isa/_attachments/riscv-privileged.pdf),
and [vector extension](https://docs.riscv.org/reference/isa/unpriv/v-st-ext).
Reservations may be discarded across a CPU switch or restore; the tests require
failure after a consumed or conflicting reservation, not unconditional SC
success across those transitions.

#### Coverage and reproduction

[`audit.py`](../util/riscv-bench/multicore/audit.py) adds 31 directed scenarios
and optional rejection checks. It records exact compiler/simulator commands,
per-case logs and `results.json`. Each simulator invocation has a 60-second
host timeout and a bounded simulated duration. Checkpoint cases run separate
save and restore processes. For direct/noncaching CPUs the reservation test
also checks the checkpoint's `lal_cid` list for every hart: the four-hart and
eight-hart snapshots retained four and eight memory-side reservations.
Timing caches can discard reservations during drain, which is permitted.

The audit also passed checks for Sv39 data/fetch remapping after `sfence.vma`,
PMP data/execute permission revocation after warming the fast paths, software
and timer interrupts, WFI with global interrupts disabled, post-takeover
interrupts, contended AMO.W/LR.W/SC.W counters, spinlocks, masked conflicting
stores, and discontiguous/excluded memory. The existing suite supplies message
publication, instruction publication with `fence.i`, read-only AMOs and
repeated switching tests.

After the fixes, the following matrix passed **300 checks** (checkpoint save
and restore count separately):

| Configuration | Passed |
|---|---:|
| New audit: direct, four harts/four channels | 33 |
| New audit: direct, eight harts/four channels, width four, staggered clocks | 33 |
| New audit: noncaching, two harts/one channel | 33 |
| New audit: timing with classic caches, four harts/four channels | 33 |
| New audit: direct with Ruby CHI, four harts/four channels, width two; three invalid configurations | 36 |
| Nine vector cases each on Direct, Timing, Minor and O3, two harts/one channel with classic caches | 36 |
| Existing suite: direct, 2/4/8 harts, 1/4 channels, width four and staggered clocks | 72 |
| Existing suite: four-channel DDR4, classic Direct → Timing → Direct | 12 |
| Existing suite: four-channel DDR4, Ruby Direct → Direct → Direct | 12 |

Commands and case names are in the
[multicore README](../util/riscv-bench/multicore/README.md#deeper-audit).
The invalid configurations explicitly reject no eligible RAM, a foreign event
queue and simulated data stalls. This does not enable concurrent host event
queues.

The RISC-V fast build and source/style checks passed. CoreMark's three CRCs,
62,405,796 instructions and 73,783,313,000 ticks match `f48a0bd118`. Linux
completed its memory/timer checks and userspace marker. Linux's vector
microop count and timing change with the corrected element requests:
170,369,391 operations and 228,353,061,000 ticks, versus 170,324,769 and
228,310,447,000 before the fixes. Guest instruction counts were 170,054,284
and 170,063,636 respectively; these runs are not instruction-count identical.

This is directed coverage, not a complete ISA or coherence proof. Vector
checks cover naturally aligned unit-stride accesses at EEW 8/64 and LMUL 1/8;
segmented/indexed accesses, nonzero `vstart`, split timing fault suppression
and arbitrary speculative fault-only-first overlap need separate coverage.
Hardware reset, DMA interference, RV32, hypervisor translation, KVM hardware
execution and other coherence protocols were not exercised in this pass.
