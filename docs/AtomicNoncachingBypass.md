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

Sections 1–3 describe the port/backdoor paths. Section 4 describes the
opt-in direct-backing-store mode for `NonCachingSimpleCPU`.

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

In its default port/backdoor mode it caches granted host pointers:

- accesses that miss the CPU's host mapping use `sendAtomicBackdoor`;
  a granted backdoor is filed in
  an `AddrRangeMap` with an invalidation callback that erases it and clears any
  window pointing at it
  ([`noncaching.cc`](../src/cpu/simple/noncaching.cc),
  [`noncaching.hh:61`](../src/cpu/simple/noncaching.hh#L61))
- `hostAddr()` resolves an address against a cached `BackdoorWindow`, falling
  back to the map on a window miss and handling interleaved ranges per-access
  ([`noncaching.cc`](../src/cpu/simple/noncaching.cc))
- `readMem()` and `writeMem()` can copy plain loads/stores directly without
  constructing packets. The packet path also tries host access through
  `tryBackdoorAccess()` before sending to a port.
  These paths refuse LR/SC, atomics, swaps, masked writes and special memory
  attributes so their existing handlers preserve semantics. Stores use
  packets when the system has more than one thread or the memory's backdoor
  is not writeable, including while it holds a reservation.
- instruction fetch caches a whole translated page when the TLB promises
  uniform translation and one backdoor covers it, keyed by a translation epoch
  ([`noncaching.cc`](../src/cpu/simple/noncaching.cc))

This is the mechanism behind the speedup recorded in
[RiscvNonCachingPerf.md](RiscvNonCachingPerf.md).

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

`NonCachingSimpleCPU.direct_memory` optionally names the static RAM owners
that the CPU may access directly. The default empty list retains the existing
port/backdoor behavior. In `configs/example/riscv/noncaching_fs.py`, enable it
with `--direct-memory`; this works with cacheless, classic L1/L2/L3, and Ruby
CHI/SimpleNetwork configurations, including 1/2/4 interleaved DDR4 channels.

### 4.1 Reuse the allocation and mapping interface

Like `KvmVM::delayedStartup()`, the CPU enumerates
`system->getPhysMem().getBackingStore()`. Compatible interleaved controllers
already share one contiguous host allocation. The CPU caches its bounds and
host pointer, without copying RAM, changing interleaving or rewiring ports.
PhysicalMemory owns the allocation; CPU pointers are never checkpointed.

Only contiguous, address-mapped, `kvmMap` allocations whose complete span is
covered by the explicitly selected memory owners qualify. Null-memory, sparse
and unselected mappings use packets. Duplicate owners, owners from another
system or event queue, and configurations with no eligible allocation are
rejected.

The config is responsible for selecting static RAM with identity address
routing and one authoritative data image. Address-transforming interconnects,
device overlays within selected RAM, hotplug and concurrent host writers are
outside this contract. `kvmMap` alone does not establish these properties.
Sources: [`PhysicalMemory`](../src/mem/physical.cc),
[`KvmVM`](../src/cpu/kvm/vm.cc),
[`mapping construction`](../src/cpu/simple/noncaching.cc).

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
owner of the mapping. Each store checks the owners' existing lock lists for
emptiness, a constant-time check per controller with no traversal of reserved
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

Switch-out clears the old CPU's direct mappings and fetch/data windows before
timing execution resumes. The existing `m5.switchCpus()` drains execution,
changes the memory mode, and transfers architectural state and ports. Classic
snoop filters have headroom beyond resident cache capacity to accommodate
outstanding requests when timing caches become active.

Startup after image loading or checkpoint restore rebuilds mappings. No raw
host pointer is serialized, and store eligibility always consults current
memory-side reservations. Noncaching CPU takeover also rebuilds mappings.

Returning from dirty timing caches requires separate writeback/invalidation
support and is outside this example's one-way workflow. The new CPU mapping
does not change that requirement. Sources:
[`switchCpus`](../src/python/m5/simulate.py),
[`CPU lifecycle`](../src/cpu/simple/noncaching.cc),
[`Linux builder`](../util/riscv-bench/build-linux.sh).

### 4.7 Validation

See the measurements and validation record in
[RiscvNonCachingPerf.md](RiscvNonCachingPerf.md).
Scope extensions, especially concurrent host execution, address transformation
or Ruby reference memory, need separate correctness tests before being enabled.
