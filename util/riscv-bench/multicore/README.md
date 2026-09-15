# Bare-metal multicore memory regressions

These tests exercise `DirectMemorySimpleCPU` and its shared atomic-memory
fallbacks with actual concurrent RISC-V harts. They require an existing
RISC-V gem5 binary and `riscv64-linux-gnu-gcc` supporting V and Zicboz.
They build freestanding ELF files locally; no guest OS or downloads are needed.

From the repository root:

```sh
# All 12 scenarios, with 2/4 harts and 1/4 interleaved memory owners.
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

The seven commands above passed 132 configurations in total after these fixes.
This includes classic Direct → Timing → Direct and repeated Ruby Direct →
Direct switching. These results cover the listed configurations, not arbitrary
coherence protocols or parallel host event queues.

The required LR/SC failure behavior is defined in the
[RISC-V atomic extension](https://docs.riscv.org/reference/isa/unpriv/a-st-ext.html).
The exact-winner test additionally checks gem5's deterministic memory behavior;
it is not a general architectural test forbidding spurious SC failure.
