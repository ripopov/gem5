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
[`src/cpu/simple/noncaching.cc:87`](../src/cpu/simple/noncaching.cc#L87)

and then builds a DMI cache on top of it:

- every atomic access is a `sendAtomicBackdoor`; a granted backdoor is filed in
  an `AddrRangeMap` with an invalidation callback that erases it and clears any
  window pointing at it
  ([`noncaching.cc:283-303`](../src/cpu/simple/noncaching.cc#L283-L303),
  [`noncaching.hh:61`](../src/cpu/simple/noncaching.hh#L61))
- `hostAddr()` resolves an address against a cached `BackdoorWindow`, falling
  back to the map on a window miss and handling interleaved ranges per-access
  ([`noncaching.cc:94-113`](../src/cpu/simple/noncaching.cc#L94-L113))
- `tryBackdoorAccess()` serves plain loads and stores with a direct `memcpy`,
  no packet and no port call
  ([`noncaching.cc:117-140`](../src/cpu/simple/noncaching.cc#L117-L140),
  dispatched at [`noncaching.cc:279`](../src/cpu/simple/noncaching.cc#L279)).
  It deliberately refuses LR/SC, atomics, swaps and masked writes — those rely
  on the memory's own locked-address bookkeeping — and refuses stores when
  another requester (a JitCPU sharing the memory) hooks the port path
- instruction fetch caches a whole translated page when the TLB promises
  uniform translation and one backdoor covers it, keyed by a translation epoch
  ([`noncaching.cc:325-350`](../src/cpu/simple/noncaching.cc#L325-L350))

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
    _drain_after_mem_writeback()
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

An implementation could forward backdoor requests through caches only while
`bypassCaches()` is true. It would also need to ensure that all requesters stop
using bypass pointers when caches become active, including DMA requesters that
may retain backdoors across a CPU switch. A reverse switch into noncaching mode
requires the writeback/invalidate maintenance described above. These are
implementation requirements, not a reason the one-way fast-boot workflow is
inherently unsafe.

JitCPU obtains host memory mappings directly from the physical backing store,
outside the port backdoor mechanism. Its direct mapping therefore does not
demonstrate backdoor forwarding through classic caches or Ruby.

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

That property is exactly what the JitCPU regressions assert after every switch:
a JIT phase must produce no CHI messages and no RNF cache accesses
([`tests/gem5/jitcpu/configs/jitcpu_common.py:60-95`](../tests/gem5/jitcpu/configs/jitcpu_common.py#L60-L95)).

### 2.3 No backdoor support in Ruby

`RubyPort` implements none of the backdoor methods, so a DMI request through
Ruby always degrades to the default no-op in
[`src/mem/port.cc`](../src/mem/port.cc). A CPU that
needs a host pointer under Ruby must obtain it another way — which is why the
CHI configs set `access_backing_store` when there are multiple directories and
let QEMU map Ruby's functional backing store directly:

```python
# Several DRAM controllers expose interleaved backing ranges, which QEMU
# cannot map as one host-contiguous region. Ruby's canonical functional
# backing store preserves CHI timing while giving JitCPU a direct map.
options.access_backing_store = num_dirs > 1
```
[`tests/gem5/jitcpu/configs/jitcpu_common.py:31-34`](../tests/gem5/jitcpu/configs/jitcpu_common.py#L31-L34)

---

## 3. Summary

| Layer | Mechanism | Cost of an access |
|---|---|---|
| Classic cache | port-level short-circuit to `memSidePort` | one extra virtual call, no lookup |
| Classic xbar | `snoop_caches = false`, snoop filter skipped | no coherence traffic |
| Classic memory | `MemBackdoor` granted to CPU / DMA if the request path supports it; classic caches block it | host `memcpy` for eligible accesses after a grant |
| Ruby | `recvAtomic` routed direct to the directory controller | one `sendAtomic` to the memory port |
| Ruby | no backdoor; direct host map of the backing store instead | host access, arranged outside gem5 |

Two invariants hold throughout:

1. **Bypass never flushes.** Cache maintenance is the caller's job, and
   `switchCpus` does it on the way *into* the mode.
2. **Caches never grant backdoors,** in either memory system. DMI and a
   coherent cache are mutually exclusive unless the cache implements the full
   invalidate protocol, and gem5's caches deliberately do not.
