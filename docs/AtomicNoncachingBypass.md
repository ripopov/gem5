# How `atomic_noncaching` bypasses the cache hierarchy

`atomic_noncaching` is a **system-wide** memory mode, not a CPU property.
Everything below keys off one predicate:

```cpp
bool System::bypassCaches() const { return memoryMode == enums::atomic_noncaching; }
```
`src/sim/system.hh:285`

Callers never ask "is this CPU non-caching?" — they ask the `System`. So the
mode changes the behaviour of the crossbars, the caches, DMA engines and Ruby
simultaneously, for every requester in the system.

---

## 1. Classic memory system

### 1.1 Caches become transparent forwarders

`BaseCache` does not implement a bypass *path*; its **ports** short-circuit
before the cache is ever consulted. Every protocol entry point on the CPU-side
port checks the flag and hands the packet straight to `memSidePort`:

| Entry point | Bypass action | Location |
|---|---|---|
| `recvTimingReq` | `cache.memSidePort.sendTimingReq(pkt)`, asserts success | `src/mem/cache/base.cc:2626` |
| `recvAtomic` | `return cache.memSidePort.sendAtomic(pkt)` | `src/mem/cache/base.cc:2642` |
| `recvFunctional` | `cache.memSidePort.sendFunctional(pkt)`, no cache lookup | `src/mem/cache/base.cc:2653` |
| `tryTiming` | returns `true` unconditionally — never blocks, never retries | `src/mem/cache/base.cc:2609` |

Consequences: no tag lookup, no allocation, no MSHR, no writeback, **no cache
latency contribution**, and no cache stats. The comment on the functional path
states the contract plainly — *"The cache should be flushed if we are in cache
bypass mode, so we don't need to check if we need to update anything."*

The snoop-side handlers assert the mode is *not* active
(`src/mem/cache/base.cc:2596`, `:2695`, `:2705`, `:2714`): in bypass mode no
snoop should ever reach a cache, because the crossbar stops generating them.

### 1.2 Crossbars stop snooping

`CoherentXBar` disables coherence work wholesale:

- `const bool snoop_caches = !system->bypassCaches() && ...` gates snoop
  forwarding on both the timing and atomic paths
  (`src/mem/coherent_xbar.cc:199`, `:755`)
- the snoop filter is skipped on request, response and snoop-response paths
  (`:428`, `:484`, `:837`, `:1020`)
- express-snoop and snoop-response handlers `assert(!system->bypassCaches())`
  (`:706`, `:943`, `:1072`)

So a request walks CPU → xbar → (cache-as-wire) → xbar → memory controller with
no coherence traffic generated anywhere.

### 1.3 Backdoors (gem5's DMI) — the real fast path

Bypassing the cache still costs a chain of virtual `sendAtomic` calls per
access. The `MemBackdoor` mechanism removes even that. It is gem5's exact
analogue of SystemC TLM-2.0 DMI: the target hands the initiator a raw host
pointer plus an `AddrRange` and a flags set, with an invalidation callback.

**Target side.** `AbstractMemory` owns a `MemBackdoor` describing its backing
store, with `Readable`/`Writeable` flags and an `invalidate()` hook
(`src/mem/abstract_mem.cc:67-70`, `:118-120`). Both memory models expose it:

- `SimpleMemory::recvAtomicBackdoor` / `recvMemBackdoorReq` → `getBackdoor()`
  (`src/mem/simple_mem.cc:99-128`)
- `MemCtrl::recvAtomicBackdoor` / `recvMemBackdoorReq` → `dram->getBackdoor()`
  (`src/mem/mem_ctrl.cc:158-161`, `:1384-1391`)

**Transport side.** Crossbars forward backdoor requests when
`enable_backdoor` is set — default `True` (`src/mem/XBar.py:96`);
see `CoherentXBar::recvAtomicBackdoor` (`src/mem/coherent_xbar.cc:819`) and
`recvMemBackdoorReq` (`:1002-1009`).

**Caches grant nothing.** `grep -rn Backdoor src/mem/cache/` returns zero hits.
A backdoor request that reaches a cache falls through to
`ResponsePort::recvAtomicBackdoor`, which DPRINTFs once and degrades to a plain
`recvAtomic` with no backdoor returned (`src/mem/port.cc:225-233`, `:236-244`).
This is correct by construction: a raw pointer to DRAM cannot observe a dirty
line held in a cache. SystemC imposes the same rule.

**Initiator side — `NonCachingSimpleCPU`.** It requires the mode outright:

```cpp
if (!(system->isAtomicMode() && system->bypassCaches()))
    fatal("The direct CPU requires the memory system to be in the "
          "'atomic_noncaching' mode.\n");
```
`src/cpu/simple/noncaching.cc:87`

and then builds a DMI cache on top of it:

- every atomic access is a `sendAtomicBackdoor`; a granted backdoor is filed in
  an `AddrRangeMap` with an invalidation callback that erases it and clears any
  window pointing at it (`noncaching.cc:283-303`, `noncaching.hh:61`)
- `hostAddr()` resolves an address against a cached `BackdoorWindow`, falling
  back to the map on a window miss and handling interleaved ranges per-access
  (`noncaching.cc:94-113`)
- `tryBackdoorAccess()` serves plain loads and stores with a direct `memcpy`,
  no packet and no port call (`noncaching.cc:117-140`, dispatched at `:279`).
  It deliberately refuses LR/SC, atomics, swaps and masked writes — those rely
  on the memory's own locked-address bookkeeping — and refuses stores when
  another requester (a JitCPU sharing the memory) hooks the port path
- instruction fetch caches a whole translated page when the TLB promises
  uniform translation and one backdoor covers it, keyed by a translation epoch
  (`noncaching.cc:325-350`)

This is the mechanism behind the speedup recorded in
[RiscvNonCachingPerf.md](RiscvNonCachingPerf.md).

**Initiator side — DMA.** `DmaPort` keeps the same structure: in atomic mode it
picks `sendAtomicBdReq` over `sendAtomicReq` when `sys->bypassCaches()`
(`src/dev/dma_device.cc:406-416`), maintains its own `memBackdoors` map with
invalidation callbacks (`:318-348`), and `DmaReadFifo` selects
`resumeFillBypass()` — a single direct transfer — instead of
`resumeFillTiming()` (`:531-534`, `:541-560`).

### 1.4 Bypass is not a flush

Nothing above evicts anything. Lines already resident, dirty ones included,
stay in the caches and become invisible to bypassing accesses. That is why
`switchCpus` performs maintenance *before* entering the mode:

```python
if memory_mode == MemoryMode("atomic_noncaching").getValue():
    memWriteback(system)
    _drain_after_mem_writeback()
    memInvalidate(system)
```
`src/python/m5/simulate.py:531-547`

This path is **not** Ruby-gated — it walks the classic hierarchy equally well.
Note also that `CoherentXBar::recvAtomicBackdoor` forwards backdoor requests
gated only on `enableBackdoor`, *not* on `bypassCaches()`, so a CPU can obtain a
pointer straight to DRAM past caches that are merely bypassed. That is sound
only because of the writeback/invalidate above, plus the fact that the backing
store never moves.

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
`src/mem/ruby/system/RubyPort.cc:238` (PIO) and `:366` (memory)

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
`src/mem/ruby/system/RubyPort.cc:406-415`

and `AbstractController::recvAtomic` is just
`ticksToCycles(memoryPort.sendAtomic(pkt))`
(`src/mem/ruby/slicc_interface/AbstractController.cc:420-423`).

So the L1/L2 cache controllers, the SLICC state machines, the message buffers
and the interconnect are never touched — **zero network messages, zero cache
accesses, zero router traversals**. Non-PIO addresses that are not physical
memory are punted to the PIO port instead (`:373-386`).

That property is exactly what the JitCPU regressions assert after every switch:
a JIT phase must produce no CHI messages and no RNF cache accesses
(`tests/gem5/jitcpu/configs/jitcpu_common.py:60-95`).

### 2.3 No backdoor support in Ruby

`RubyPort` implements none of the backdoor methods, so a DMI request through
Ruby always degrades to the default no-op in `src/mem/port.cc`. A CPU that
needs a host pointer under Ruby must obtain it another way — which is why the
CHI configs set `access_backing_store` when there are multiple directories and
let QEMU map Ruby's functional backing store directly:

```python
# Several DRAM controllers expose interleaved backing ranges, which QEMU
# cannot map as one host-contiguous region. Ruby's canonical functional
# backing store preserves CHI timing while giving JitCPU a direct map.
options.access_backing_store = num_dirs > 1
```
`tests/gem5/jitcpu/configs/jitcpu_common.py:31-34`

---

## 3. Summary

| Layer | Mechanism | Cost of an access |
|---|---|---|
| Classic cache | port-level short-circuit to `memSidePort` | one extra virtual call, no lookup |
| Classic xbar | `snoop_caches = false`, snoop filter skipped | no coherence traffic |
| Classic memory | `MemBackdoor` granted to CPU / DMA | host `memcpy`, no packet at all |
| Ruby | `recvAtomic` routed direct to the directory controller | one `sendAtomic` to the memory port |
| Ruby | no backdoor; direct host map of the backing store instead | host access, arranged outside gem5 |

Two invariants hold throughout:

1. **Bypass never flushes.** Cache maintenance is the caller's job, and
   `switchCpus` does it on the way *into* the mode.
2. **Caches never grant backdoors,** in either memory system. DMI and a
   coherent cache are mutually exclusive unless the cache implements the full
   invalidate protocol, and gem5's caches deliberately do not.
