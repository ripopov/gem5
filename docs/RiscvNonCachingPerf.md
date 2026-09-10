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

`RiscvNonCachingPerf.html` in this directory is an interactive companion:
one page per patch with the problem, the mechanism, why guest behaviour is
unchanged and the measured effect, tied to a chart of the series and a
diagram of the per-instruction loop. Open it in a browser.

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
| instruction fetch from a cached host page | 24.38 | 16.83 |
| SimpleThread final, PC-event check inlined | 25.30 | 17.17 |
| cached translations answered before translate(), ISA pointer kept | 25.50 | 17.31 |

Spike, same host: 746 and 295 MIPS.

The final binary on the full workloads, and with two build-level changes
that compose with the code changes: Ubuntu's GCC enables the stack
protector and CET landing pads by default, and every one of the small
functions on gem5's per-instruction path pays for a canary load, a compare
and an `endbr64`; link-time optimization inlines across the many
translation units the path crosses. Neither is turned on by default here
because they are a distribution and build-system choice, not a change to
the model, but a build meant for throughput should use both.

| binary | CoreMark, 5000 iterations (1.56 G instructions) | Linux boot + `ls` (160 M instructions) |
|---|---|---|
| `build/RISCV/gem5.fast`, default flags | 64.4 s, **24.2 MIPS** | 9.5 s, **16.8 MIPS** |
| `--with-lto`, `CCFLAGS_EXTRA='-fno-stack-protector -fcf-protection=none'` | 61.9 s, **25.2 MIPS** | 9.1 s, **17.5 MIPS** |
| Spike (`SpikePerf.md`, same host) | 2.1 s, 746 MIPS | 0.39 s, 295 MIPS |

Whole-process wall clock is within 0.4 s of `hostSeconds` in every case, so
the MIPS figures are comparable with Spike's, which time from `main()`.

## Optional L1/L2/L3 hierarchy and four memory controllers

The results above used the default **cacheless** topology in
[`noncaching_fs.py`](../configs/example/riscv/noncaching_fs.py): CPU
instruction, data and page-table-walker ports → `SystemXBar` → `SimpleMemory`
with `latency="0ns"`. There were no caches between the CPU and memory, so
backdoor requests could reach the backing store.

`--cache-hierarchy classic --caches` instantiates this classic hierarchy:

```text
CPU instruction port → L1I ─┐
CPU data port ────────→ L1D ┼→ L2XBar → L2 → L2XBar → L3 → SystemXBar
CPU page-table walkers ────┘                                  ├→ MemCtrl 0
                                                             ├→ MemCtrl 1
                                                             ├→ MemCtrl 2
                                                             └→ MemCtrl 3
```

On-chip HiFive devices remain attached to `SystemXBar`; off-chip devices are
behind its 50 ns bridge and `IOXBar`. The system still has one hart at 1 GHz,
1 GiB of RAM starting at `0x80000000`, and 64-byte cache lines.

Tag, data and response stages each use the latency shown for classic caches.

| Cache | Capacity | Associativity | Latency per stage | MSHRs |
|---|---|---|---|---|
| L1I | 64 KiB | 4-way | 2 cycles | 8 |
| L1D | 64 KiB | 8-way | 2 cycles | 16 |
| L2 | 1 MiB | 8-way | 12 cycles | 32 |
| L3 | 8 MiB | 16-way | 30 cycles | 64 |

These are generic capacities for a modern performance-oriented system, not a
calibrated model of a particular RISC-V processor. All caches run at the CPU
clock, with 16 targets per MSHR and 16 write buffers. Override capacities with
`--l1i-size`, `--l1d-size`, `--l2-size` and `--l3-size`.

`--memory ddr4` creates `MemCtrl` objects with `DDR4_2400_8x8` interfaces.
`--num-mem-ctrls` accepts 1, 2 or 4, defaulting to 1. Multiple channels are
interleaved every 64 bytes using `RoRaBaCoCh` mapping; four channels select on
address bits 7:6. The benchmark retains 1 GiB total RAM (256 MiB per channel),
so gem5 warns that each assigned range is smaller than the DRAM model's
nominal 16 GiB device capacity. The modeled device geometry is unchanged.
`--caches` and `--memory ddr4` are independent in classic mode; omitting both
preserves the original topology and defaults.

`--cache-hierarchy ruby` instead builds CHI with split L1I/L1D caches, a
private L2, one HNF containing the L3, and one SNF per memory controller.
It uses the same capacities and associativities as the table, with CHI's
controller defaults for latency and buffering. All Ruby controllers and the
SimpleNetwork run at the CPU clock. The network has a Crossbar topology and
four virtual networks for requests, snoops, responses and data. Ruby always
instantiates its caches, without needing `--caches`.

Both on-chip and off-chip HiFive devices connect to `IOXBar` in Ruby mode.
CPU data and page-table-walker ports connect to the data sequencer; the
instruction port has a separate sequencer. `RubyPortProxy` handles image
loading. `access_backing_store=False` keeps a single RAM image in the memory
controllers, without a separate Ruby reference-memory allocation.

These benchmarks still use `NonCachingSimpleCPU` and `atomic_noncaching`.
Classic caches forward atomic packets without cache lookups, but do not
forward backdoor requests. Ruby's atomic path goes from `RubyPort` to the
selected SNF's memory controller, skipping cache controllers and network
messages. Neither path grants the CPU a backdoor here; interleaved memory
ranges also lack backdoors. The comparison therefore measures functional
access overhead with different routing and memory organization. It does not
measure cache-hit performance, CHI network throughput or DDR4 timing. CPU
instruction/data stall simulation remains disabled.

`--cpu-type timing` activates either hierarchy. `--cpu-type atomic` activates
classic caches, but uses `atomic_noncaching` with Ruby, as required by Ruby's
atomic request path.

Reproduce the comparison from the repository root with the recorded binary.
That binary predates the config's `riscv_profile` parameter, so the measurement
used this loader to omit only that assignment. The repository config remains
unchanged, and every topology uses the same compiled ISA defaults (including
`enable_stateen=False`), VLEN 256 and `MHSU` privilege modes. These results
measure the existing binary, not a rebuilt current-source RVA23 implementation.

```bash
cat > /tmp/noncaching-perf-config.py <<'PYCONFIG'
from pathlib import Path
import sys

__file__ = str(Path("configs/example/riscv/noncaching_fs.py").resolve())
sys.path[0] = str(Path(__file__).parent)
source = Path(__file__).read_text()
assignment = 'isa.riscv_profile = "RVA23S64"'
assert source.count(assignment) == 1
source = source.replace(assignment, "")
exec(compile(source, __file__, "exec"), globals())
PYCONFIG

out=/tmp/noncaching-perf-repeat
mkdir -p "$out"
for topology in simple classic ruby; do
    extra=()
    if [ "$topology" != simple ]; then
        extra=(--cache-hierarchy "$topology" --memory ddr4
               --num-mem-ctrls 4)
        if [ "$topology" = classic ]; then
            extra+=(--caches)
        fi
    fi
    for run in 1 2 3; do
        /usr/bin/time -p taskset -c 2 build/RISCV/gem5.fast \
            --outdir="$out/$topology-coremark-$run" \
            /tmp/noncaching-perf-config.py baremetal \
            build/riscv-bench/coremark-200.elf "${extra[@]}" \
            > "$out/$topology-coremark-$run.log" 2>&1
    done
    /usr/bin/time -p taskset -c 2 build/RISCV/gem5.fast \
        --outdir="$out/$topology-linux" \
        /tmp/noncaching-perf-config.py linux \
        --bootloader build/jitcpu-linux/fw_jump.elf \
        --kernel build/jitcpu-linux-rva23/vmlinux \
        --initrd build/riscv-bench/bench-initramfs.cpio "${extra[@]}" \
        > "$out/$topology-linux.log" 2>&1
done
```

For a binary supporting `riscv_profile`, use `noncaching_fs.py` directly and
record a new baseline. The reported metric follows `bench.sh`: divide
`simInsts` by `hostSeconds` and by one million. For CoreMark, use the mean
host time of the three runs. The whole-process times below were measured
with Python's monotonic `time.perf_counter()` around each subprocess; the
commands above report the corresponding wall time with `/usr/bin/time`.

Measurements taken on **2026-09-10** on the same Core Ultra 7 265K, pinned
to host CPU 2, with all workloads run sequentially. All three topologies use
`build/RISCV/gem5.fast`, compiled 2026-09-08 09:36:56, SHA-256:

```text
2a119c211ef7018fc182ca5c75b14de8c22b96d5d6e76394d5fa36a0e517c0bc
```

The [measurement record](RiscvNonCachingPerf-20260910.json) contains every
run's results, artifact/config hashes and verification outcomes. The current
configuration has an explicit 10 MHz RTC connection and the adapter retains
the old binary's disabled state-enable setting. These differ from the saved
2026-09-09 configuration, so all baselines were remeasured; the older Linux
instruction count is not the reference for this comparison.

CoreMark uses 200 iterations and three runs; Linux boot + `ls` uses one run.
Times are host simulation seconds; MIPS excludes startup and image loading.

| Topology | CoreMark mean s | CoreMark MIPS | Linux s | Linux MIPS |
|---|---|---|---|---|
| Cacheless, one zero-latency SimpleMemory | 2.587 | 24.13 | 9.900 | 16.46 |
| Classic L1/L2/L3, four DDR4 | 15.630 | 3.99 | 49.630 | 3.28 |
| Ruby CHI L1/L2/L3, four DDR4 | 9.323 | 6.69 | 30.680 | 5.31 |

CoreMark's `hostSeconds` values were 2.59/2.59/2.58 for SimpleMemory,
15.59/15.63/15.67 for classic, and 9.25/9.48/9.24 for Ruby. Ruby was **1.68×**
as fast as classic on CoreMark and **1.62×** on Linux by host simulation time.
The cacheless SimpleMemory baseline remained fastest on both workloads.
Ruby's shorter atomic path avoids the chain of classic cache and crossbar
forwarders; these measurements do not isolate the cost of each component.

| Topology | CoreMark whole-process mean s | Linux whole-process s |
|---|---|---|
| Cacheless SimpleMemory | 2.804 | 10.239 |
| Classic, four DDR4 controllers | 15.906 | 50.099 |
| Ruby CHI, four DDR4 controllers | 10.140 | 31.847 |

All three topologies retired exactly **62,405,796 instructions** in
**73,783,313,000 ticks** for CoreMark, and **162,998,103 instructions** in
**210,036,085,000 ticks** for Linux. Every CoreMark run produced CRCs
`0xe714`, `0x1fd7` and `0x8e3a`; every Linux run reached `RISCV-BENCH: done`.
Classic cache tag/data access counters and Ruby cache demand-access counters
remained zero. Ruby's network forwarded zero messages. All four DRAM
controllers recorded reads and writes in both full-hierarchy configurations.
Thus these runs exercised functional memory routing while leaving both cache
hierarchies bypassed, with identical guest instruction counts and simulated
time across the three configurations.

## Correctness requirements

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

### Instruction fetch from a cached host page

Every fetch still set up a `Request`, ran it through the MMU's translate
chain and looked up a backdoor before copying four bytes. `BaseTLB` gains
two queries for CPU models that want to keep a host pointer per
instruction page: `translationEpoch()`, a value that changes whenever any
translation could, and `stableFetchPage()`, which after a successful fetch
translation says whether every fetch in that page translates identically
while the epoch holds. The RISC-V TLB answers both from its translation
cache; the defaults promise nothing. `NonCachingSimpleCPU` overrides the
new `AtomicSimpleCPU::fetchInstruction()`: while the fetch PC stays inside
the cached page and the epoch is unchanged, a fetch is an epoch query, two
compares and a copy from the host pointer. Only the translation is
cached; the bytes are read from the backing store on every fetch, so
self-modifying code behaves as before. Fetches served this way do not
consult the instruction TLB, so its hit/access statistics count only the
fetches that reach it (4.4 M instead of 198 M on the Linux boot).

### Thread accessors resolved at compile time

The profile after the previous step showed the per-instruction protocol
itself as the largest item, and a good part of it was indirect calls into
`SimpleThread`: `pcState()` from the tick loop, the fetch path and
`postExecute()`, `getIsaPtr()` from the commit counting, and `getReg()`
and `setReg()` behind every operand read and write. These methods are
virtual only because `SimpleThread` implements the `ThreadContext`
interface; the simple CPUs hold the thread by its own type and nothing
derives from it. Marking the class `final` lets the compiler resolve
every call made through a `SimpleThread` pointer, and `postExecute()`
now reads the PC through that pointer instead of the `ThreadContext`
array. The PC-event check, which every instruction makes and which on
the Linux boot found a non-empty queue and then went out of line to test
the filter, is split so the empty-queue test is inline and only the
servicing loop is a call.

### Translation hits answered at the top of the chain

A data access that hits the translation cache still went through
`BaseMMU::translateAtomic()`, `TLB::translateAtomic()` and into
`TLB::translate()`, whose frame is sized for the slow path, before
reaching `translateCached()`, and every one of these lookups, as well as
every fetch-page epoch check, asked the thread context for its ISA
pointer through a virtual call to add its generation to the TLB's
invalidation epoch. `translateAtomic()` now tests the cache itself before
calling `translate()`, and the TLB keeps the ISA pointer of the thread
it last translated for, so the generation is two loads and an add. This
is worth about 1% on both workloads; it is kept because the fetch-page
check and the data path both go through it and it removes the last
indirect call from a translation hit.

## What remains

After these steps the profile is flat. At 24.4 MIPS an instruction costs
about 41 ns, and the remaining items, in order:

| share | what | why it stays |
|---|---|---|
| ~15% | `AtomicSimpleCPU::tick()` | the per-instruction protocol itself: virtual `pcState()`, `fetchInstruction()`, `checkInterrupts()`, decoder calls, drain and idle checks, cycle-counter probes |
| ~13% | `Decoder::decode()`, `moreBytes()` | the decoder's per-instruction PC and vector-configuration bookkeeping, and on Linux the hash-map fallback when the 8K-entry PC cache misses (a 32K cache measured +2% on Linux for 1 MiB per hart and was not taken) |
| ~10% | `postExecute()`, `countCommitInst()`, `probeInstCommit()` | what is left of instruction accounting: a dozen integer adds, the property tests, four probe notifies and a virtual `inUserMode()` call |
| ~9% | `getRegOperand()`/`setRegOperand()` | virtual ExecContext calls with a RegId translation and a counter per operand |
| ~8% | data loads/stores | request setup and the three-level `translateAtomic()` -> `translate()` -> `translateCached()` chain around a cache hit |
| ~5% | fetch | the cached-page check and 4-byte copy |
| ~4% | `advancePC()` | two virtual levels around a PC update |
| ~3% | `Interrupts::checkInterrupts()` | evaluated per instruction, always finding nothing |
| ~6% | the `execute()` bodies | the instruction semantics themselves |

The next steps in order of payoff would be: driving the interrupt check
from state changes instead of polling it; and guarding the commit probes with
`hasListeners()`.
Together they are plausibly another 1.3x. Beyond that the remaining cost
is the StaticInst/ExecContext calling convention itself: every operand read
and write is a virtual call with a RegId translation, and every instruction
an indirect `execute()` call with the fault-return protocol around it.
Getting toward Spike's 1.3 ns per instruction means threaded dispatch and
handlers that touch the register file directly, which is a new CPU model
rather than a change to this one. Where that line sits is the useful
result: about 30x from Spike on CoreMark and 17x on the Linux boot, with
6.5x and 7.3x recovered from the starting point by changes that stay
inside the existing model.
