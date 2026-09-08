# Making gem5's RISC-V NonCachingSimpleCPU faster

Spike, the RISC-V reference interpreter, executes the two workloads this
series uses as its functional-CPU benchmarks at **295 MIPS** (Linux boot) and
**746 MIPS** (bare-metal CoreMark) on this machine
(`riscv-isa-sim/linux-test/SpikePerf.md`). gem5's `NonCachingSimpleCPU` is
the fastest way gem5 executes RISC-V code without an external backend. This
document records how far that model can be pushed on top of upstream
`develop` while keeping gem5's structure (the StaticInst/ExecContext
protocol, eager statistics, the Request/Packet memory interface, per-cycle
timing): one change per commit, each re-measured, with the profile that
motivated it.

Host: Intel Core Ultra 7 265K, Ubuntu 25.10, GCC 15.2, `build/RISCV/gem5.fast`
built with the default flags, one hart, `atomic_noncaching` memory, the
RVA23S64 profile reported to the guest and the hypervisor extension enabled.
Numbers are `simInsts / hostSeconds` from `util/riscv-bench/bench.sh`:
CoreMark is the 200-iteration image (62.4 M instructions, mean of three
runs), Linux is OpenSBI 1.8 plus a Linux 6.12 boot to userspace and `ls`
(one run). The whole-process figure, comparable to Spike's `--stats`, is
about 5% lower.

## Results

| step | CoreMark | Linux boot |
|---|---|---|
| baseline: upstream develop plus this commit | 3.76 | 2.29 |
| interrupt check decided from local pending bits | 4.38 | 2.60 |
| no byte-enable allocation per access | 4.72 | 2.71 |
| backdoor data path with cached bounds | 5.58 | 3.09 |
| translation cache in front of TLB::translate() | 11.31 | 8.33 |
| PC-indexed decoder cache | 12.58 | 9.27 |
| consecutive cycles inside one tick event | 13.76 | 10.33 |
| decoder: no PCState copy, encoding assembled in registers | 14.42 | 10.65 |
| PC-event bitmap filter | 14.49 | 10.95 |
| PC state advanced in place | 16.15 | 11.90 |
| per-instruction helpers inlined, probe argument guarded | 17.08 | 12.58 |
| direct plain load/store path | 18.04 | 13.51 |
| instruction counts folded into statistics at dump time | 21.08 | 15.05 |

Spike, same host: 746 and 295 MIPS.

Every step must leave guest behaviour unchanged: CoreMark and the Linux boot
have to exit at the same tick with the same instruction count as the
baseline, CoreMark's three data CRCs must match, and `AtomicSimpleCPU` and
`TimingSimpleCPU`, which share most of the changed code, must keep
producing the same CRCs, ticks and instruction counts.

## Method

Each round: profile with gperftools (`perf` is locked on this host, see
`util/riscv-bench/README.md`), pick the largest cost that a contained change
can remove, implement it, rebuild, run CoreMark three times and check the
CRCs, boot Linux and compare `simTicks` and `simInsts` with the previous
build, then commit with the numbers. Changes that measure within noise are
dropped rather than kept.

## Where the time goes

### Baseline

At the baseline the model spends about 200 ns per retired instruction, and
the profile is almost entirely overhead around a few percent of instruction
execution:

* **Address translation, 24%**, even in M-mode with paging off.
  `TLB::translate()` re-derives the effective privilege, virtualization state
  and translation mode from several CSRs (each through `ISA::readMiscReg`'s
  side-effect switch), scans the PMP table and searches the PMA range lists
  for every fetch, load and store.
* **Memory access, 20%.** Every fetch and data access builds a `Packet`,
  crosses the bus to `AbstractMemory::access()`, looks its backdoor up in an
  `AddrRangeMap` through a `std::function`, and allocates two
  `std::vector<bool>` byte-enable masks on the way.
* **Decode, 13%.** An `unordered_map` lookup keyed by the 64-bit extended
  encoding for every instruction, plus store-forwarding stalls from writing
  32-bit fields of the encoding union and reading it whole.
* **The event queue, 11%.** One event per cycle: an insertion and a
  `serviceOne()` dispatch per instruction.
* **Interrupt check, 9%.** `globalMask()` reads five CSRs per instruction to
  find that nothing is pending.
* **Statistics and bookkeeping, 15%.** About thirty `statistics::Scalar`
  increments per instruction, most of them recording the same fact for
  several stat groups, behind a run of data-dependent branches; PC-event
  lookups; out-of-line helpers.
* **PC state, 8%.** Four copies of a polymorphic `PCState` per instruction.

### Interrupt check

`Interrupts::checkInterrupts()` runs before every instruction. It computed
`globalMask()` unconditionally, reading the status, misa, mideleg, privilege
and (with H) hideleg/vsstatus CSRs through `ISA::readMiscReg`'s side-effect
switch, looked up whether the ISA reports Smrnmi by string comparison,
fetched the ISA pointer through the thread context and read the NMI bits
through two more virtual calls, all to find that nothing was pending. Now
`(ip | hvip) & ie` decides the common case locally; the privilege and
delegation state is consulted only when something is pending and enabled,
and the ISA pointer, the Smrnmi flag and the NMI bits are cached or read
directly.

### Byte-enable masks

`AtomicSimpleCPU::genMemFragmentRequest()` built a `std::vector<bool>` of
byte enables for every fragment and copied it into the reused request, a
heap allocation and free per load and store although the mask is all-ones
for every ordinary instruction. `Request` now accepts an empty mask as
"unmasked", which `isMasked()` and every consumer of `getByteEnable()`
already treat that way, and the CPU only materializes a mask when some
byte is actually disabled.

### Backdoor data path

`NonCachingSimpleCPU` already read instructions straight from the memory
backdoors that `AbstractMemory` hands out, but every load and store still
went through `Packet` -> port -> `CoherentXBar::recvAtomicBackdoor()` ->
`findPort()` -> `SimpleMemory` -> `AbstractMemory::access()`, and every
fetch looked its backdoor up in the `AddrRangeMap`, whose `find()` walks a
small list through a `std::function` on each call, then resolved the
offset through `AddrRange::contains()`/`getOffset()`, which branch on
interleaving.

Plain `ReadReq`/`WriteReq` packets are now completed directly in the
backing store when a recorded backdoor covers them; LR/SC, atomics, swaps
and masked writes stay on the port, as do all stores when another hart
could hold a reservation in the memory's locked-address list. The fetch
and data paths each remember the last backdoor as plain start/end bounds
and a host base pointer, so the common case is two compares and an add,
and the 1/2/4/8-byte copies compile to single loads and stores instead of
a `memcpy` call.

### Translation cache

The largest single item in the baseline profile. `TLB::translate()` now
keeps a direct-mapped cache of complete results per page and access mode:
physical page, the request flags the slow path added (PHYSICAL,
UNCACHEABLE, STRICT_ORDER) and whether the PMA permits misaligned access
there. An entry is filled only after the slow path succeeded on an
ordinary access and only when the page is uniform for every check the
fast path skips (`PMP::homogeneous()`, `PMAChecker::uniformAttributes()`,
no local accessor), and it is tagged with a generation that the ISA
advances whenever a translation-relevant CSR (privilege, status,
satp/vsatp/hgatp, envcfg, PMP, V, NMIE, misa) changes value and the TLB
advances on every invalidation, so no cached result outlives the state it
was derived from.

The first version showed no effect at all: reading `mstatus` normalizes it
and writes the CSR back on every read, which advanced the generation each
time. The counter only moves when a value actually changes.

### Decoder cache

`Decoder::decode()` looked every instruction up in the
`ExtMachInst -> StaticInst` `unordered_map`: a hash of the 64-bit extended
encoding, a bucket walk and a compare. A direct-mapped cache of 8192
decoded instructions indexed by the halfword address now sits in front of
it. Decoding is a pure function of the extended machine instruction (which
carries the vector configuration and XLEN), so a hit requires the encoding
to match as well as the address and entries never need invalidating;
self-modifying code simply misses on the changed bits.

### One event per cycle

`AtomicSimpleCPU::tick()` rescheduled itself after every cycle, so each
instruction paid for an event-queue insertion plus a `serviceOne()`
dispatch. After finishing a cycle, `tick()` now looks at the event queue:
if no other event is due at or before the next cycle and no asynchronous
request is pending, it advances the queue's current tick itself and runs
the next cycle without leaving the handler. The tick moves exactly as the
queue would have moved it, so cycle counts, timestamps and ordering with
every other event are unchanged; any event scheduled in the meantime,
another CPU's tick, a drain or an idle transition returns control to the
event loop.

### Decoder store-forwarding stalls

Three costs hidden in `Decoder::moreBytes()` and `decode()`: the PC state
handed to `moreBytes()` was copied (a polymorphic object with a vtable,
vector configuration and Zcmt state) just to read two fields; `moreBytes()`
wrote the 32-bit `instBits` field of the encoding union and `decode()`
immediately read the whole 64-bit union; and `decode()` filled five
bitfields of the union one by one, each a read-modify-write through
memory before the whole value was read again. The first is a reference
now; the union is assembled in a register and stored once, 64 bits wide.

### PC-event lookups

Every instruction asks the thread's `PCEventQueue` whether an event is
registered at the current PC. A full-system Linux run registers a handful
(kernel panic/oops hooks), which made every instruction pay for a binary
search over the event vector. The queue now keeps a 4096-bit filter of
the PCs that have events and rejects almost all lookups against it, and
`checkPcEventQueue()` returns early when no event is registered at all.

### PC state copies

`BaseSimpleCPU` copied the PC state four times per instruction: `preExecute()`
copied it into a temporary for the decoder and copied the result back, and
`advancePC()` went through `StaticInst::advancePC(ThreadContext *)`, which in
every ISA copies the state out, advances it and copies it back. Each copy
is a virtual `update()` of a polymorphic object. `SimpleThread` now offers
its owning CPU in-place access to the PC state; the decoder works on the
thread's copy directly and `advancePC()` uses the `PCStateBase` overload on
it. The `ThreadContext` interface is unchanged.

### Out-of-line helpers

`checkForInterrupts()`, `serviceInstCountEvents()`, `countInst()` and
`countFetchInst()` are each a few instructions but were out-of-line
functions called once per instruction; `advancePC()` asked the PC state
whether the instruction branched (a virtual call that also fetches the
instruction size) even without a branch predictor to tell; and `tick()`
built the Commit probe's argument, which copies a `StaticInstPtr`, whether
or not anyone listened. The helpers are inline now, only the rare
interrupt-taking path stays out of line, `branching()` is evaluated only
for the branch predictor and the probe argument only for a listener.

### Plain loads and stores

`AtomicSimpleCPU::readMem()`/`writeMem()` build a fragment loop with a
byte-enable mask, a `Packet` per fragment and a port call for every access.
For a load or store that stays within one cache line, enables every byte
and carries no LR/SC, atomic, swap, prefetch, cache-maintenance or HTM
semantics, none of that changes the outcome. `NonCachingSimpleCPU` now
overrides both with a path that performs the same steps (trace annotation,
request setup, translation, NO_ACCESS check, latency bookkeeping) and
copies the bytes through the backdoor, building a `Packet` only when the
target has no usable backdoor. Everything else falls back to the generic
implementation.

### Statistics folded at dump time

The simple CPUs updated about thirty `statistics::Scalar` objects per
instruction, spread over the fetch, execute, commit, thread and
exec-context groups, each a load/add/store on a double reached through a
vector of `unique_ptr`, and many of them recording the same fact several
times (instructions are counted five times, ops four, memory references
three). With everything else on the path trimmed this was the largest
remaining cost.

`SimpleExecContext` now keeps the per-instruction accounting in a compact
struct of integers: each property of a retired instruction is counted
once, including the per-op-class and per-control-type histograms and the
per-register-class read/write counts from the operand accessors.
`BaseSimpleCPU::preDumpStats()` folds the pending counts into every
statistic that reports them, and `resetStats()` discards them, so dumps,
dump-and-reset periods and manual resets see exactly the values they saw
before; `stats.txt` is byte-identical. O3 and Minor keep updating the
shared statistics directly. The one visible difference: a live read of one
of these statistics between dumps (the MathExpr power model) sees the
value as of the last fold.
