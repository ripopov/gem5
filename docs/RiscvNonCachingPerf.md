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
