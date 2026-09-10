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

Sections 1–3 describe current behavior. Section 4 records the **desired,
not yet implemented** direct-backing-store mode for `NonCachingSimpleCPU`.

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

An alternative implementation could forward backdoor requests through caches only while
`bypassCaches()` is true. It would also need to ensure that all requesters stop
using bypass pointers when caches become active, including DMA requesters that
may retain backdoors across a CPU switch. A reverse switch into noncaching mode
requires the writeback/invalidate maintenance described above. These are
implementation requirements, not a reason the one-way fast-boot workflow is
inherently unsafe.

The desired design for this repository instead follows KvmCPU's direct
backing-store mapping approach; see §4. It does not require cache backdoor
forwarding.

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
options.access_backing_store = num_dirs > 1
```
[`tests/gem5/jitcpu/configs/jitcpu_common.py:31-34`](../tests/gem5/jitcpu/configs/jitcpu_common.py#L31-L34)

The nearby config comment about interleaving should not be read as a limit
on physical allocation: `PhysicalMemory` already merges compatible interleaved
controller ranges into a contiguous host allocation. The per-controller
backdoor interface does not expose that allocation. Ruby's optional reference
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
| Ruby | no backdoor; direct host map of the backing store instead | host access, arranged outside gem5 |

Two invariants hold throughout:

1. **Bypass never flushes.** Cache maintenance is the caller's job, and
   `switchCpus` does it on the way *into* the mode.
2. **The current cache/Ruby ports do not grant or forward backdoors.** This
   is an implementation limitation during bypass, not a prohibition on mapping
   RAM while the hierarchy is inactive.

## 4. Desired design: direct backing-store access like KvmCPU

**Status: design only; section reviewed against this checkout on 2026-09-10.**
Add an optional C++ mode to `NonCachingSimpleCPU`, exposed by
the Python config, retaining today's port-backdoor behavior as the default.
The objective is fast functional boot with the complete classic L1/L2/L3
and 1/2/4-controller topology instantiated, followed by timing CPU takeover.
The requirements below preserve the existing access semantics. Packet fallback
and a single-thread store gate are initial implementation choices; dedicated
fast handling can replace them if it preserves those semantics.

### 4.1 Reuse the allocation and mapping interface

The backing store is the host allocation holding guest bytes. Controllers
model address ownership and access behavior; their interleaving need not imply
interleaved host allocations. `PhysicalMemory` merges compatible channel
ranges, allocates the span and points each controller at that same allocation.
This abstraction explicitly supports changing controller organization without
changing the physical-memory image.
Sources: [`physical.hh`](../src/mem/physical.hh),
[`PhysicalMemory::createBackingStore`](../src/mem/physical.cc).

KvmCPU's `KvmVM::delayedStartup()` enumerates `getBackingStore()`, selects
`kvmMap` entries and registers their guest base, host pointer and length as KVM
memory slots. It discovers these mappings without requests through caches.
Sources: [`KvmVM`](../src/cpu/kvm/vm.cc),
[upstream KvmVM](https://github.com/gem5/gem5/blob/stable/src/cpu/kvm/vm.cc).
The [kernel memory-slot API][kvm-memory-slot]
maps userspace memory into guest physical ranges and separately supports
read-only and dirty-tracking flags. Reusing gem5's allocation interface does
not require KVM, a host matching the guest ISA, or those ioctls.

[kvm-memory-slot]:
  https://docs.kernel.org/virt/kvm/api.html#kvm-set-user-memory-region

Use this existing allocation, without copying RAM, changing controller
interleaving, adding a second classic RAM image or rewiring ports. A CPU-local
mapping cache stores physical bounds and a host base pointer. PhysicalMemory
remains the owner; cached pointers are never checkpointed or freed by the CPU.

### 4.2 Access selection and initial scope

| Condition | Desired path |
|---|---|
| Eligible RAM fetch/load by the active noncaching CPU | Direct access |
| Eligible ordinary RAM store satisfying §4.3 | Direct access |
| MMIO, local accessor or special request | Existing handler |
| Mapping miss or ineligible direct store | Atomic packet |
| Inactive CPU or mode other than `atomic_noncaching` | No direct access |

MMIO fallback executes the device's atomic access and side effects through
the normal ports, while the system remains in `atomic_noncaching`. A handoff
to a timing CPU changes the memory mode. KvmCPU likewise handles MMIO exits
through gem5; see [`BaseKvmCPU::doMMIOAccess`](../src/cpu/kvm/base.cc).

Initial qualification should cover static classic RAM, serialized gem5
execution, the existing single-hart benchmark, and compatible CPU takeover.
Mappings outside that scope use packets. If the configuration cannot safely
mix direct and packet accesses, leave the new mode disabled or reject an
explicit request to enable it. Ruby's separate memory images are discussed in
§4.5. Concurrent host execution and address-transforming interconnects are
outside the initial scope.

### 4.3 LR/SC reservations survive individual atomic accesses

`atomic_noncaching` completes each memory request synchronously. It does not
make an LR → computation → SC sequence indivisible. A reservation records
whether the later conditional store may succeed; it does not block other
accesses. A conflicting store from another hart between LR and SC must make
SC fail, even if all three requests execute serially.
See the [RISC-V LR/SC specification][riscv-lrsc].

KVM guest atomics/exclusives execute under the host ISA and virtualization
machinery. Their ordinary RAM operations do not enter gem5's
`AbstractMemory::lockedAddrList`. Reusing KVM's storage selection in a software
CPU does not provide that native atomic-execution mechanism.

`NonCachingSimpleCPU` instead uses software ISA reservation state and the
memory packet implementation. In this checkout:

- `AbstractMemory::trackLoadLocked()` records the reservation and invalidates
  its `MemBackdoor`. `getBackdoor()` withholds grants while locks remain.
- Ordinary packet stores pass through `writeOK()` / `checkLockedAddrList()`,
  which remove matching reservations and notify other contexts as appropriate.
- RISC-V also tracks the reserving address in ISA state;
  `globalClearExclusive()` clears it and wakes the CPU.

Sources: [`AbstractMemory`](../src/mem/abstract_mem.cc),
[`getBackdoor` and `writeOK`](../src/mem/abstract_mem.hh),
[`RISC-V ISA reservation handlers`](../src/arch/riscv/isa.cc).

**A raw backing pointer does not receive the existing backdoor invalidation.**
Keeping LR/SC on packets alone is insufficient: LR → ordinary direct store to
the reserved location → SC could retain a lock that the packet store would
have removed. In this checkout, an ordinary store by the reserving hart also
removes its matching memory-side reservation. The single-hart example therefore
matters for preserving the current packet behavior; RISC-V does not universally
require same-hart ordinary stores to invalidate reservations. Checking only the
ISA reservation address misses memory-side records, including restored ones.

An ordinary direct store must either perform the required reservation
invalidation and notifications or establish that none apply. The initial
implementation can use the latter approach: retain the existing conservative
`system->threads.size() == 1` gate and allow raw stores only while the relevant
memory has no reservations. For a merged allocation, a single eligibility flag
must cover every constituent memory owner. New and restored reservations must
invalidate that eligibility before another direct store. Use a memory-owned
query or invalidation mechanism to avoid scanning lock lists on every access.
Otherwise, fall back to packet stores. Do not clear reservations just to enable
the fast path.

Keep LR/SC, AMOs and swaps on their current handlers initially: they implement
conditional-store status or read-modify-write behavior that an ordinary byte
copy cannot supply. These operations can have dedicated fast implementations;
packet transport itself is not an architectural requirement. Multi-hart direct
stores also need the notifications currently provided by packet accesses before
the thread-count restriction can be relaxed.
Source: [`storesBypassPort()`](../src/cpu/simple/noncaching.cc).

[riscv-lrsc]: https://docs.riscv.org/reference/isa/unpriv/a-st-ext.html

### 4.4 Access checks and cache-management instructions

Direct access to ordinary RAM must preserve these existing behaviors:

- **Address ownership and permissions.** `BackingStoreEntry` contains no
  read/write permissions; `kvmMap` selects mappings for KVM. Determine direct
  eligibility from the memory owners and port topology. Preserve ROM write
  behavior and route MMIO, local accessors and addresses requiring mapping or
  device handling through their existing paths.
- **Translation and request attributes.** Keep MMU translation and permission
  checks, including PMP/PMA handling. Inspect attributes added by translation
  before using a host pointer. Initially fall back for uncacheable, strictly
  ordered and special requests, as the current CPU does.
- **Access bounds.** Preserve fragmentation and fault behavior for accesses
  crossing pages or other access boundaries. Check the entire host span with
  overflow-safe bounds. A channel-selection change within a qualified linear
  merged allocation does not itself require a packet or another allocation.
- **Store notifications.** Raw stores bypass packet snoops and CPU address
  monitors. The initial thread-count gate limits this problem; extending direct
  stores to multiple contexts requires preserving reservation invalidation and
  wakeups.
- **Instruction fetch.** Retain the existing translation-epoch and uniform-page
  checks. Cache the host location, and read instruction bytes afresh on each
  fetch. Preserve decoder and `fence.i` behavior when memory changes.
- **Accounting.** Direct accesses skip port latency and memory-system packet
  statistics/traces. Initially require instruction/data stall simulation to be
  disabled and document which counters are bypassed. Preserve CPU instruction
  accounting; use packet mode when port instrumentation is required.

Cache clean/invalidate requests are separate from the reservation issue in
§4.3. Once caches have been flushed and bypassed, there is no cached data to
clean or invalidate. `AbstractMemory::access()` explicitly gives these packets
no data effect. There is no need to flush caches on each such instruction or
invalidate a RAM pointer merely because the guest requests cache maintenance.

The instruction's translation, access checks and exceptions still matter.
These requests enter through the CPU's write interface but must not be treated
as ordinary RAM stores. Initially retain the existing handling, which already
excludes `CLEAN` and `INVALIDATE` from `plainAccess()`. A dedicated fast path
could complete them locally after preserving the applicable checks and
completion behavior. By contrast, `cbo.zero` writes zeros to memory and must
preserve real store semantics, including reservation handling.
See the [RISC-V cache-block operation specification][riscv-cmo].

The initial design assumes serialized gem5 execution. Supporting concurrent
host writers would require synchronization; `memcpy` alone does not provide
that. This is a scope restriction, not extra locking required for the initial
single-thread execution model.

Sources: [`BackingStoreEntry`](../src/mem/physical.hh),
[`AbstractMemory::access`](../src/mem/abstract_mem.cc),
[`writeOK`](../src/mem/abstract_mem.hh),
[`NonCachingSimpleCPU` access/fetch paths](../src/cpu/simple/noncaching.cc),
[`AtomicSimpleCPU` accesses and snoops](../src/cpu/simple/atomic.cc),
[`RISC-V cache-block requests`](../src/arch/riscv/isa/decoder.isa).

[riscv-cmo]: https://docs.riscv.org/reference/isa/unpriv/cmo.html

### 4.5 Ruby needs a single authoritative image for mixed accesses

With `access_backing_store`, `configs/ruby/Ruby.py` creates a separate
`ruby.phys_mem` and marks the DRAM interfaces `kvm_map=False`. This selects
the reference image for KVM mappings. Functional accesses and timing
completion use that reference memory. However, Ruby's atomic request path
first accesses a directory's memory controller and then accesses the reference
memory. They are distinct allocations, not aliases.

This creates a concrete hazard for the proposed software CPU: a direct store
can update the reference image alone, while a later packet AMO/SC passes
through controller data and reservation state as well. Reference-memory
selection alone does not prove read-modify-write results, failure status or
reservation invalidation are correct. KVM's native RAM atomics do not exercise
that same mixed software packet path.

Ruby is outside the initial classic-memory scope. To enable it later, establish
which image and reservation state each direct or fallback operation uses, and
make their results agree. A reference-store pointer alone is insufficient.
This is an identified design risk, not a reproduced Ruby bug. Sources:
[`Ruby configuration`](../configs/ruby/Ruby.py),
[`RubyPort` access paths](../src/mem/ruby/system/RubyPort.cc).

### 4.6 Switching, checkpoints and pointer lifetime

Before entering bypass mode from a caching mode, dirty data must reach backing
memory and cached copies must be invalidated. Current `switchCpus()` drains,
switches out old CPUs, calls `memWriteback()` and `memInvalidate()`, changes
the mode and performs takeover. It has no second drain after writeback. If a
supported hierarchy posts asynchronous writebacks, their completion must be
handled before invalidation and bypass execution. A system starting with empty
caches has no dirty data to write back.

On exit, stop the old CPU and clear its direct/fetch windows before the timing
CPU resumes. Classic RAM needs no memory-image copy. Guard direct-pointer use
by CPU activity and memory mode; `System::setMemoryMode()` itself only checks
drain state and assigns the mode. Guest clean/invalidate instructions during
bypass do not replace the maintenance needed when entering that mode.

Rebuild mappings and eligibility after restore/takeover. Current
`PhysicalMemory::unserializeStore()` restores bytes into an existing
allocation, but raw addresses must never be serialized or assumed valid in a
new process.
Static allocation is the initial contract; remapping/hotplug needs explicit
invalidation or must be unsupported. Do not cache eligibility before restored
lock records and image loading have completed.

Test takeover and restore with an outstanding LR, including a conflicting write
before the resumed SC. Preserve the existing CPU-pair behavior and ensure that
memory-side records cannot leave direct-store eligibility stale. KVM mapping
reuse does not establish software reservation-transfer correctness.
Sources: [`switchCpus`](../src/python/m5/simulate.py),
[`System::setMemoryMode`](../src/sim/system.cc),
[`PhysicalMemory::unserialize`](../src/mem/physical.cc),
[`KvmCPU switch/takeover`](../src/cpu/kvm/base.cc).

### 4.7 Acceptance checks before enabling the feature

- Compare direct and existing packet/backdoor modes for CoreMark and Linux:
  CRCs, memory results, instruction counts and simulated ticks, with cacheless
  and L1/L2/L3 topologies and 1/2/4 controllers. Check that direct-hit counters
  increase; a fallback-only run does not validate acceleration.
- Exercise LR → same-hart store → SC, DMA stores, AMOs after direct stores and
  restored locks. Include different channel selections within one merged
  allocation. In multi-context configurations, verify that the initial gate
  keeps stores on packets; competing-hart direct stores and wakeups need tests
  if that restriction is later relaxed.
- Exercise MMIO side effects, ROM writes, unmapped/device-overlay addresses,
  boundary-crossing and misaligned accesses, PMP/page faults, page-table
  updates, translation changes and self-modifying code. Check that
  clean/invalidate requests leave RAM unchanged while preserving existing
  fault behavior, and
  that `cbo.zero` writes zeros with the same store effects as the existing
  path.
- Switch both ways repeatedly with dirty timing caches, outstanding reservation
  state and timer/DMA activity; checkpoint/restore and rerun. Check that bypass
  phases leave cache counters untouched and timing phases generate cache
  traffic.
- Verify fallback or rejection outside the initial scope. Future Ruby support
  must additionally compare mixed direct/packet atomic results against the
  existing mode; successful Linux boot alone does not exercise those cases.

These checks are future implementation acceptance criteria. This documentation
change neither implements the feature nor claims those tests have passed.
