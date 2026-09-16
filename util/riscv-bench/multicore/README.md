# Bare-metal multicore memory regressions

These tests exercise `DirectMemorySimpleCPU` and its shared atomic-memory
fallbacks with actual concurrent RISC-V harts. They require an existing
RISC-V gem5 binary and `riscv64-linux-gnu-gcc` supporting V and Zicboz.
They build freestanding ELF files locally; no guest OS or downloads are needed.

From the repository root:

```sh
# All 16 scenarios, with 2/4 harts and 1/4 interleaved memory owners.
python3 util/riscv-bench/multicore/run.py --outdir /tmp/direct-flat

# Classic caches and four interleaved DDR4 controllers.
python3 util/riscv-bench/multicore/run.py --outdir /tmp/direct-classic \
    --cores 4 --channels 4 --topology classic --memory ddr4

# Run each scenario before, between and after two CPU switches.
python3 util/riscv-bench/multicore/run.py --outdir /tmp/direct-switch \
    --cores 2 4 --channels 4 --switches 2

# Ruby CHI requires a CHI-enabled gem5 build.
python3 util/riscv-bench/multicore/run.py --outdir /tmp/direct-ruby \
    --cores 4 --channels 4 --topology ruby --memory ddr4 --switches 2

# Exercise dirty-cache writeback on the return from timing execution.
python3 util/riscv-bench/multicore/run.py --outdir /tmp/direct-timing \
    --cores 4 --channels 4 --topology classic --switches 2 --switch-to timing

# Mix direct and upstream NonCachingSimpleCPU harts, then switch all harts.
python3 util/riscv-bench/multicore/run.py --outdir /tmp/direct-mixed \
    --cores 8 --channels 4 --cpu mixed --switches 2 --switch-to noncaching

# Control: the same tests with only the upstream CPU.
python3 util/riscv-bench/multicore/run.py --outdir /tmp/noncaching-control \
    --cores 2 --channels 1 --cpu noncaching \
    --switches 2 --switch-to noncaching
```

Use `--gem5 PATH`, `--cc COMPILER`, and `--cases NAME ...` to select a different
binary, compiler or subset. Outputs contain ELF files, per-run logs and gem5
statistics, and `results.json` with exact build/run commands and pass status.
Both the guest and host enforce time limits. Only a successful guest exit
counts as a pass; traps, timeout, nonzero exit and unexpected switch markers
fail the run. The runner exits nonzero if any scenario fails.

## Scenarios

| Name | Check |
|---|---|
| `reservation-same-value` | A peer store of the existing value still invalidates the reservation. |
| `reservation-aba` | A peer writes another value and restores the original; SC must fail. |
| `reservation-crossline` | A split store crosses into the reserved cache line, including across interleaved owners. |
| `reservation-self-store` | Preserve memory-side same-hart store invalidation before/after switching; allow either architectural outcome during cached timing execution. |
| `reservation-store` | An observed store from another hart makes SC fail. |
| `reservation-amo` | An observed AMO from another hart makes SC fail. |
| `reservation-vector` | A vector store invalidates a later reservation granule. |
| `reservation-zero` | `cbo.zero` invalidates a later reservation granule. |
| `reservation-unaligned` | A misaligned store invalidates the granule it crosses into. |
| `reservation-reuse` | Another hart's new LR cannot revive a reservation invalidated by its store. |
| `reservation-compete` | Multiple harts reserve one word; this memory implementation permits exactly one SC winner. |
| `lrsc-counter` | Contending LR/SC increments retain every update on all four lines. |
| `mixed-counter` | Mixed AMO and LR/SC increments retain every update. |
| `producer-consumer` | A ring of producers publishes and verifies 512-byte messages using fences and a handoff flag. |
| `readonly-amo` | AMOs return ROM contents without changing them. |
| `instruction-publish` | Harts publish new instructions and execute them after `fence.i`. |

Reservation tests use explicit ready/done handshakes on separate cache lines,
so the conflicting write happens strictly between LR and SC. The target moves
across four 64-byte lines, exercising every channel in a four-owner backing
store. Wide-store tests reserve bytes beyond the first 16-byte reservation
granule. Counter loops keep LR/add/SC together; handshakes deliberately use
longer sequences to test required failure after an observed conflicting write.

The ROM is uncacheable so its memory owner's write protection also applies
when the test switches to timing execution. Ruby gives it a separate boot-memory
controller. Bare-metal startup supplies per-hart stacks and a trap handler.
Guest failure codes are 1 (unexpected LR value), 10–13 (invalid SC success),
20–23 (lost counter updates), 30 (message mismatch), 40–43 (SC winner count),
50 (ROM modification), 60 (stale instructions), 90/91 (handshake/barrier timeout),
or 128 plus `mcause` for traps.

## Bugs reproduced and fixed

The pre-fix binary at `0c0917c3f0` fails `reservation-amo`,
`reservation-vector`, `reservation-zero`, `reservation-unaligned`,
`mixed-counter` and `readonly-amo` with two harts and one memory owner.
It also fails `reservation-reuse` after a CPU switch.

- `AbstractMemory::access()` bypassed `writeOK()` for AMOs and swaps. They
  now apply write protection and invalidate reservations when they write;
  a failed conditional swap does not invalidate reservations.
- `AbstractMemory::checkLockedAddrList()` only invalidated the first 16-byte
  granule of a write. It now covers the entire packet's address range.
- `AtomicSimpleCPU::takeOverFrom()` inherited thread contexts without
  refreshing its reused requests. Replacement CPUs could all send context
  ID zero, allowing a hart to consume another hart's memory-side reservation.
  The request IDs now follow the inherited thread context.

At the original 12-scenario revision, the seven commands above passed 132
configurations in total after these fixes.
This includes classic Direct → Timing → Direct and repeated Ruby Direct →
Direct switching. These results cover the listed configurations, not arbitrary
coherence protocols or parallel host event queues.

The required LR/SC failure behavior is defined in the
[RISC-V atomic extension](https://docs.riscv.org/reference/isa/unpriv/a-st-ext.html).
The exact-winner test additionally checks gem5's deterministic memory behavior;
it is not a general architectural test forbidding spurious SC failure.

## Deeper audit

`audit.py` adds directed protection, vector and lifecycle tests. Run from the
repository root with an existing binary:

```sh
python3 util/riscv-bench/multicore/audit.py --outdir /tmp/audit-direct
python3 util/riscv-bench/multicore/audit.py --outdir /tmp/audit-stagger \
    --cores 8 --channels 4 --width 4 --stagger
python3 util/riscv-bench/multicore/audit.py --outdir /tmp/audit-noncaching \
    --cpu noncaching --cores 2 --channels 1
python3 util/riscv-bench/multicore/audit.py --outdir /tmp/audit-timing \
    --cpu timing --topology classic
python3 util/riscv-bench/multicore/audit.py --outdir /tmp/audit-ruby \
    --topology ruby --width 2 --check-rejections

for cpu in direct timing minor o3; do
    python3 util/riscv-bench/multicore/audit.py \
        --outdir /tmp/audit-vector-$cpu --cpu "$cpu" \
        --cores 2 --channels 1 --topology classic \
        --cases masked-pmp masked-store-pmp masked-fault-first \
        unmasked-fault-first zero-fault-first masked-lmul8-load \
        masked-lmul8-store masked-unmapped masked-conflict
done

# Existing scenarios with varied scheduling and repeated switching.
python3 util/riscv-bench/multicore/run.py --outdir /tmp/regress-flat \
    --cores 2 4 8 --channels 1 4 --width 4 --stagger
python3 util/riscv-bench/multicore/run.py --outdir /tmp/regress-classic \
    --cores 4 --channels 4 --topology classic --memory ddr4 \
    --switches 2 --switch-to timing
python3 util/riscv-bench/multicore/run.py --outdir /tmp/regress-ruby \
    --cores 4 --channels 4 --topology ruby --memory ddr4 \
    --switches 2 --width 4
```

At the original 31-audit/12-regression-scenario revision, those commands passed
300 checks after the follow-up fixes. Checkpoint save
and restore are counted separately. `--check-rejections` now adds five expected
startup/resume failures and verifies their diagnostics; it requires the direct CPU.
The `--stagger` clocks are 1000, 1137, 1274, ... MHz, all on the same host
event queue. `--width` controls instructions per tick on atomic-derived CPUs.

| Case group | Checks |
|---|---|
| `misaligned-{lr,sc,amo,amo-crossline}` | Correct guest exception and trap address after warming ordinary memory accesses; no host panic. |
| `pmp-partial-{s,m}`, `pmp-revoke-{data,fetch}` | First-match PMP priority, partial overlap, effective privilege and fast-path permission revocation. |
| `sv39-{data,fetch}-remap` | A changed leaf PTE followed by `sfence.vma` invalidates cached data/fetch translations. |
| `sv39-high-{data,fetch}-remap` | Repeat remapping at a negative canonical Sv39 address, including returns from high-address code to low-address code. |
| `reservation-return`, `reservation-zero`, `checkpoint-zero` | A consumed ISA reservation cannot revive through CPU reuse, an initially empty map or restore at physical address zero. |
| `masked-{pmp,store-pmp,unmapped,lmul8-load,lmul8-store}` | Inactive unit-stride vector elements cause no access or fault; active elements and undisturbed destination bytes are correct. |
| `{masked,unmasked,zero}-fault-first` | Accessible prefixes, suppressed faults, repeated execution with changed masks, and VL zero. |
| `masked-conflict`, `word-counter`, `spinlock` | Masked writes invalidate reservations; mixed AMO.W/LR.W/SC.W preserves the neighboring word; a contended lock protects a 64-word payload. |
| `secondary-{ram,excluded}` | Producer-consumer accesses and AMOs to a second allocation separated from main RAM by an address hole. |
| `readonly-store` | Direct CPU writes to a cacheable-marked ROM still respect the memory owner's write protection; other CPU modes retain the normal ROM PMA marking. |
| `checkpoint-reservations` | Memory contents survive restore and conflicting peer writes prevent SC success; direct/noncaching snapshots must contain every hart's memory-side reservation. |
| `software-interrupt`, `timer-interrupt`, `wfi-global-disabled`, `interrupt-after-switch` | WFI wakeup, traps, conflicting peer writes across sleep, and interrupt wiring after takeover. |

New failures use codes 70 (alignment), 71 (PMP), 72 (reservation lifetime),
73 (translation), 74 (interrupt), 75 (word counter), 76 (spinlock),
77 (secondary memory), 78 (checkpoint), 79/80 (vector protection/data).
Each invocation has a host timeout of 60 seconds and a simulated bound of
10 billion ticks. Trap handlers and handshakes fail explicitly. Minor/O3 were
checked on the nine vector scenarios above; the full lifecycle matrix is
not claimed for those models.

To reproduce the new failures, save a binary built from `f48a0bd118` and
provide it with `--gem5 /path/to/gem5-before.fast`. Use a separate output
directory for each binary. For example:

```sh
python3 util/riscv-bench/multicore/audit.py \
    --gem5 /path/to/gem5-before.fast --outdir /tmp/audit-before \
    --cases misaligned-lr misaligned-sc misaligned-amo \
    misaligned-amo-crossline pmp-partial-s pmp-partial-m \
    reservation-return reservation-zero checkpoint-zero \
    masked-unmapped masked-pmp masked-store-pmp masked-fault-first \
    unmasked-fault-first masked-lmul8-load masked-lmul8-store
```

These cases fail on that baseline. See
[the audit findings and limitations](../../../docs/AtomicNoncachingBypass.md#49-deeper-bare-metal-audit)
for causes, fixes, reference-model results and application smoke checks.

## Multicore direct stores

The direct CPU now allows ordinary multicore stores to bypass packets when
all backing owners are writable and have empty reservation lists. Shared
memory owners and all registered harts must use the same event queue. The
hart check also runs on resume after switching CPUs. Direct stores do not
invalidate reservations themselves; a nonempty list still causes packet
fallback.

The new cases above cover same-value writes, ABA writes, cross-line writes,
same-hart invalidation and owner-level ROM protection. Negative tests reject
a noncaching peer on another event queue, both at startup and after takeover.
The latter explicitly resumes simulation: `switchCpus()` alone leaves the
system drained and would not exercise the resume check.

Focused reproduction:

```sh
python3 util/riscv-bench/multicore/run.py --outdir /tmp/direct-stores \
    --cores 2 4 8 --channels 1 2 4 --width 4 --stagger \
    --cases reservation-same-value reservation-aba reservation-crossline \
    reservation-self-store producer-consumer
python3 util/riscv-bench/multicore/audit.py --outdir /tmp/direct-store-audit \
    --cases readonly-store checkpoint-reservations reservation-return \
    --check-rejections
```

For a before/after path check, run `producer-consumer` with four harts, four
channels, width four and staggered clocks using both binaries. Sum the main
memory owners' `numWrites::total` counters. The pre-change binary produced
26,090 packet writes; the updated binary produced zero, with identical
`simTicks=87991000` and `simInsts=simOps=1363706`. This demonstrates bypass
coverage; it is not a host-speed benchmark.
