# Functional RISC-V CPU models in gem5: Atomic, QEMU and Spike

gem5 now has three ways to execute RISC-V guest instructions functionally in
full system: its own `AtomicSimpleCPU`, `RiscvJitCPU` on QEMU's TCG
translator, and `RiscvSpikeCPU` on Spike, the RISC-V reference simulator.
This report measures all three booting the same Linux image on the same
machine and sets out what each one is good for.

- `src/cpu/funcbackend/` -- the model both external backends share
- `src/cpu/jit/README.md` -- the JitCPU specification, which covers both
- `src/cpu/spike/README.md` -- what SpikeCPU adds
- `ext/spike/gem5-spike/README.md` -- the Spike adapter

## 1. Headline

`RiscvSpikeCPU` boots RISC-V Linux to userspace in **9.1 host seconds**,
against **1.2 s** for `RiscvJitCPU` and **108 s** for `AtomicSimpleCPU`.

- SpikeCPU is **11.9x faster** than gem5's atomic CPU, and **7.6x slower**
  than the QEMU-backed JitCPU.
- Most of SpikeCPU's advantage over the atomic CPU comes from the model it
  shares with JitCPU -- batched execution and direct-mapped RAM -- not from
  Spike being a faster interpreter than gem5's.
- SpikeCPU costs a **third** of JitCPU's host memory, pulls an **11 MiB**
  submodule against 493 MiB, and needs **no fork**: JitCPU carries 13 lines
  patched into QEMU's own sources, SpikeCPU carries none. Spike is BSD, so
  it could even be linked into gem5.
- Both integrations share one gem5 model. SpikeCPU is 321 lines of gem5 code
  and a 548-line adapter on top of it.

The two are complements, not alternatives: keep JitCPU for throughput and
SpikeCPU for authority, since Spike is the model the RISC-V specification is
validated against and swapping between them is one command-line word.

## 2. What was measured

One RISC-V full-system Linux boot, from the OpenSBI reset vector to the
first userspace instruction, which is an `m5_exit` in the initramfs `/init`.
The three models run on a system that is identical in every other respect:
the same `tests/gem5/jitcpu/configs/jitcpu_linux.py`, the same HiFive
platform, the same generated device tree, the same 256 MiB of memory, the
same classic `atomic_noncaching` memory hierarchy and the same guest
binaries.

| | |
| --- | --- |
| Guest | Linux 6.12 `defconfig` + initramfs, OpenSBI `fw_jump`, BusyBox-less `/init` that executes `m5_exit` |
| Machine | 1 hart, rv64imafdc + Zicsr/Zifencei/Zicntr/Zihpm/Zba/Zbb/Zbs, Sv39, M/S/U, 1 GHz, lazy CLINT at 1 MHz |
| Host | Intel Core Ultra 7 265K, 20 cores, 91 GiB, Ubuntu 26.04, GCC 15.2 |
| gem5 | `DEVELOP-FOR-25.1`, `build/RISCV/gem5.opt`, `USE_JITCPU=y USE_SPIKECPU=y` |
| QEMU | fork of v11.1.0-rc0-102, `riscv64-softmmu`, meson `-O2` |
| Spike | `riscv-isa-sim` 1.1.1-dev at `650c1a25`, unmodified, `-O2` |
| Repetitions | 3 per model, back to back, on an otherwise idle machine |

Host cost is `hostSeconds` from `stats.txt` rather than wall-clock time, so
process start-up and the ELF loading that precedes simulation are excluded.
Wall-clock times are reported alongside and track it closely.

## 3. Results

### Host cost of one Linux boot

Three repetitions each; `hostSeconds` from `stats.txt`, median in bold.

| Model | Host seconds | Median | Wall clock (median) | vs. atomic |
| --- | --- | --- | --- | --- |
| `RiscvJitCPU` | 1.32, 1.20, 1.14 | **1.20** | 1.58 s | **90x** |
| `RiscvSpikeCPU` | 9.22, 8.97, 9.11 | **9.11** | 9.48 s | **11.9x** |
| `AtomicSimpleCPU` | 108.18, 107.95, 109.39 | **108.18** | 108.5 s | 1.0x |

### Simulation rate and footprint

| Model | Guest instructions | Host inst rate (median) | vs. atomic | Peak host memory | Simulated time |
| --- | --- | --- | --- | --- | --- |
| `RiscvJitCPU` | 343,482,657 | 286.5 Minst/s | 107x | 1.55 GiB | 0.4620 s |
| `RiscvSpikeCPU` | 345,252,132 | 37.9 Minst/s | 14.1x | 495 MiB | 0.4637 s |
| `AtomicSimpleCPU` | 290,059,216 | 2.68 Minst/s | 1.0x | 482 MiB | 0.5276 s |

Every model produced a bit-identical result across its three repetitions:
same `simTicks`, same `simInsts`, same guest console output. Run-to-run host
time varies by under 2%.

All three reach the same point in the same boot and their kernel logs agree
line for line, but they retire different instruction counts: the two external
backends execute about 19% more guest instructions than the atomic CPU, in
0.06 s less simulated time. Section 4 shows this is *not* a batching
artifact. Because of it, "instructions per host second" and "host seconds per
boot" give slightly different ratios (107x and 90x for JitCPU); the boot-time
ratio is the one to quote, since all three complete the same boot.

### Correctness checks alongside the measurement

- All three models reach `Run /init as init process` and then the guest's
  `m5_exit`, with no kernel panic, oops, fault or "Illegal instruction" in
  any console log. The configuration fails the run if any appears.
- Both external backends hand the booted machine to `RiscvO3CPU` and the
  detailed CPU makes userspace progress on the timing hierarchy, which
  exercises the state transfer in both directions.
- The Spike adapter's standalone unit test passes: two independent harts,
  direct-mapped RAM, MMIO, the pseudo-instruction exit, bounded instruction
  accounting and translation invalidation, with no gem5 involved.

### Integration cost

| | JitCPU | SpikeCPU |
| --- | --- | --- |
| Backend licence | GPL-2.0-or-later | BSD-3-Clause |
| Linkable into gem5 | no, licence forbids it | yes; loaded at run time by choice |
| Upstream changes needed | 13 lines in `target/riscv` | none, the submodule is stock |
| Submodule size | 493 MiB | 11 MiB |
| Clean backend build | 43 s on 20 cores | 54 s on 20 cores |
| Build fetches from network | yes, meson subprojects | no |
| Backend artifact, stripped | 19 MiB | 14 MiB |
| Adapter (C ABI + implementation) | 653 lines | 548 lines |
| gem5-side model | 303 lines | 321 lines |
| Shared gem5 model | 926 lines, used by both | |

Build time and code size are a wash. The differences that matter are the
licence, the absence of a fork, and the 45x smaller submodule.

## 4. Why the numbers come out this way

All three models execute functionally, with functional memory and no timing
model, so the difference is not "detailed versus functional". Three things
separate them:

1. **Host work per guest instruction.** gem5 decodes into a `StaticInst` and
   calls `execute()` on it; Spike calls one C++ function per instruction
   through a pointer taken from a decode cache; QEMU translates a block once
   into host machine code and then runs the host code.
2. **gem5 events per guest instruction.** `AtomicSimpleCPU` schedules a tick
   event per instruction. Both external backends run a batch of up to
   `batch_size` instructions per event, so the event queue is touched about
   once per 65 536 instructions rather than once per instruction.
3. **gem5 packets per guest memory access.** `AtomicSimpleCPU` builds a
   `Request` and a `Packet` for every access. Both external backends resolve
   RAM to a host pointer once, through gem5's backing store, so a guest load
   from RAM becomes a host load; only MMIO comes back into gem5.

Points 2 and 3 are what `src/cpu/funcbackend/` implements, and they are
shared. That is why SpikeCPU, an interpreter like gem5's own CPU, is much
faster than it: the interpreter loop is only part of the cost gem5 pays.

```text
                       atomic          spike           jit
per instruction:   gem5 decode +   decoded-insn    host machine code
                   StaticInst      function call
memory:            gem5 Packet     host pointer    host pointer
gem5 events:       1 / insn        1 / batch       1 / batch
```

### Batching is most of it

Sweeping JitCPU's `batch_size` over the same boot separates the two effects.
The backend and the guest are unchanged; only how many instructions run per
gem5 event varies:

| `batch_size` | Host seconds | Guest instructions |
| --- | --- | --- |
| 65 536 (default) | 1.19 | 343,482,657 |
| 4 096 | 1.89 | 345,129,854 |
| 256 | 9.70 | 347,142,876 |
| 16 | 125.93 | 344,853,579 |

At a batch of 16, the QEMU translator is **slower than gem5's own atomic
CPU** -- 126 s against 108 s -- because it now pays a gem5 event, a full
architectural state transfer and a cross-library call per 16 instructions.
The translator's speed only becomes visible once that per-event cost is
amortised. This is the single most important thing to understand about both
external backends: the win is the batch, and the engine is what is left.

It also settles the instruction-count question. The guest instruction count
barely moves across a 4 096x change in batch size -- 343.5 M to 347.1 M,
about 1% -- so the 19% gap against the atomic CPU is a difference between the
three ISA implementations' behaviour during early boot, before Linux's
clocksource is running, and not an artifact of batched execution. Where
exactly the guest spends those instructions was not chased down; it does not
affect any conclusion here, since the models are compared on completing the
same boot.

## 5. The three models

### AtomicSimpleCPU (`RiscvNonCachingSimpleCPU`)

**Pros**

- No extra dependency, no extra build, no submodule. It is gem5.
- It is gem5's own RISC-V ISA implementation, so its architectural
  behaviour is by definition what the detailed models will do. Any
  divergence is a gem5 bug rather than a disagreement between two
  implementations.
- Full gem5 introspection: `--debug-flags=ExecAll`, instruction tracing,
  probe points, PC events, checkpoints, and everything else gem5 offers on
  a per-instruction basis.
- Checkpointing works.
- Correct by construction with respect to gem5's memory system: every access
  is a real packet, so PMAs, PMP and address decoding are exercised exactly
  as a detailed model would.
- Switching to and from any other gem5 CPU is a solved problem.

**Cons**

- Slowest by a wide margin. A boot that costs a second on JitCPU costs
  minutes here, and the gap grows with the workload.
- The cost is structural, not incidental: one event and one packet per
  instruction cannot be optimised away without becoming one of the other
  two models.
- gem5's RISC-V decoder covers less of the ratified extension set than
  either external backend, so a guest built for a recent profile may not
  run at all.

### RiscvJitCPU (QEMU TCG)

**Pros**

- Fastest by a factor of roughly seven over Spike and two orders of
  magnitude over the atomic CPU.
- QEMU's RISC-V target is very widely exercised, so wide guest support in
  practice: distributions boot on it every day.
- Extension coverage is broad and moves quickly.
- The translation cache amortises decode across loops, which is exactly
  where boot and benchmark time is spent.

**Cons**

- GPL-2.0-or-later. It cannot be linked into gem5; it has to be a separately
  built shared object loaded at run time, and that constraint is permanent
  rather than a matter of taste.
- It needs a **fork** of QEMU. The adapter is 954 lines, but 13 of them are
  edits to `target/riscv/cpu.h` and `target/riscv/tcg/translate.c`, so the
  backend cannot be built from an unmodified upstream checkout, and every
  QEMU rebase carries that patch forward.
- Heavyweight dependency: a 493 MiB submodule, a full QEMU configure and
  build, and a shared object of 72 MiB unstripped (19 MiB stripped). The
  build pulls its own subprojects from the network.
- Highest host memory use, roughly three times Spike's, because the QEMU
  runtime and its translation cache come along.
- QEMU is not a specification model. Where QEMU and the RISC-V manual
  disagree, QEMU's behaviour is what the guest sees, and diagnosing that
  from inside gem5 is hard: there is no instruction-level trace to compare
  against.
- QEMU keeps its own vCPU thread, which is a real complication in the
  adapter and rules out some debugging approaches.
- In this configuration gem5's PMA checker traps misaligned accesses and
  QEMU completes them, so a JitCPU boot and a detailed-model boot are not
  exercising quite the same platform.
- Checkpoint and restore do not work, as for SpikeCPU: the architectural
  state lives in the backend between batches.

### RiscvSpikeCPU

**Pros**

- **BSD-3-Clause**, and no fork. The submodule is upstream `riscv-isa-sim`
  at a pinned revision with zero patches; the adapter is 548 lines that live
  in gem5's own tree. Spike could even be linked into gem5 directly -- it is
  loaded at run time only so that both backends look the same to gem5.
- Small dependency: an 11 MiB submodule against QEMU's 493 MiB, and a build
  that fetches nothing from the network. (Build time itself is a wash: 54 s
  against QEMU's 43 s on 20 cores.)
- Lowest host memory use of the three, a third of JitCPU's.
- Spike **is** the RISC-V reference model. `riscv-tests`, the architectural
  compatibility suite and most extension bring-up target it, so when a guest
  and a model disagree, Spike is the side that is presumed right. For
  bringing up a new extension, or for arguing that a gem5 result is not an
  ISA bug, that is the whole point.
- Debuggable: Spike has a commit log, a disassembler and an interactive
  debugger, and it executes on gem5's own thread, so a host debugger sees
  one stack from the gem5 event queue down into the guest instruction.
- Spike's misaligned-access behaviour matches gem5's own PMA policy on this
  platform, so a SpikeCPU boot and a detailed-model boot see the same
  machine.
- Extension coverage is excellent and often lands here first, since Spike is
  where new extensions are prototyped.
- The adapter is simple enough to hold in your head: Spike's `simif_t` is
  already the right seam, so the whole backend is one class.

**Cons**

- Slower than QEMU by about 7x. An interpreter that dispatches per
  instruction cannot reach a translator's steady-state cost, and no amount
  of adapter work changes that.
- `processor_t::step()` cannot be interrupted from the inside, so a batch
  runs in chunks of 64 instructions to notice MMIO promptly. That is a
  small, permanent overhead and a small imprecision.
- Instructions retired in the same chunk as an m5 pseudo-instruction are not
  counted, because the adapter unwinds out of `step()` before Spike commits
  them. At most 63 instructions per m5op, which is invisible in totals.
- Spike is a single-hart-at-a-time interpreter with no built-in threading,
  so multi-hart scaling is entirely gem5's problem. (It is for QEMU too, in
  this integration.)
- Fewer eyes on unusual guests than QEMU has.
- Checkpoint and restore do not work, as for JitCPU.
- Spike's `libriscv.so` is built with debug info and is 297 MiB on disk;
  stripped it is 14 MiB, but the build helper does not strip it.

## 6. Which to use

| Situation | Model |
| --- | --- |
| Fast-forwarding a long boot or warm-up before detailed simulation | JitCPU |
| Anything where simulation host time dominates the experiment | JitCPU |
| Bringing up a new ISA extension, or deciding whether a guest fault is an ISA bug or a gem5 bug | SpikeCPU |
| A build that must stay BSD-licensed end to end, or a CI job that cannot afford a QEMU build | SpikeCPU |
| Debugging the functional backend itself, or anything wanted under a host debugger | SpikeCPU |
| Cross-checking a suspicious JitCPU result | SpikeCPU |
| Short runs, or anything needing per-instruction gem5 tracing, probes or checkpoints | AtomicSimpleCPU |
| Verifying that the functional backends are telling the truth | AtomicSimpleCPU |

The two external backends are not really competitors. They share a model and
a configuration flag, and swapping between them is one command-line word, so
the sensible arrangement is to keep both: JitCPU for throughput, SpikeCPU
for authority, and the atomic CPU as the reference both are checked against.

## 7. Caveats

- One workload. A boot is branch-heavy, cold-code-heavy and full of MMIO,
  which is close to the worst case for a translator's amortisation, so a
  long-running compute kernel would widen JitCPU's lead over SpikeCPU.
- One hart. Nothing here says anything about how the three scale across
  cores.
- The external backends retire about 19% more guest instructions than the
  atomic CPU on this boot. Section 4 rules out batching as the cause but
  does not identify it. It matters only for the rate comparison; the
  host-seconds-per-boot comparison is unaffected, because all three complete
  the same boot.
- Attribution of instructions to privilege mode is approximate on the
  external backends, since a batch is charged to the mode in force at its
  boundary: the atomic CPU reports 4 userspace instructions for this boot and
  the backends report tens of thousands.
- Host instruction rate is the right comparison here, but only because all
  three models charge one cycle per instruction. None of them models timing.
- The QEMU and Spike backends were both built at `-O2` with the same
  compiler. Neither was tuned.

## 8. Reproducing

```sh
git submodule update --init ext/qemu/repo ext/spike/repo
util/jitcpu/build-qemu-jit.sh                 # -> build/qemu-jit/
util/spikecpu/build-spike.sh                  # -> build/spike/
util/jitcpu/build-linux-image.sh              # -> build/jitcpu-linux/
scons build/RISCV/gem5.opt USE_JITCPU=y USE_SPIKECPU=y -j$(nproc)
make -C tests/test-progs/jitcpu-smoke/src

for cpu in jit spike noncaching; do
    build/RISCV/gem5.opt -d m5out/$cpu \
        tests/gem5/jitcpu/configs/jitcpu_linux.py \
        build/jitcpu-linux/fw_jump.elf build/spike/libgem5-spike.so \
        --cpu $cpu \
        --kernel build/jitcpu-linux/vmlinux \
        --initrd tests/test-progs/jitcpu-smoke/src/jitcpu-linux-init.cpio
done
```

Pass `build/qemu-jit/libgem5-qemu-jit.so` instead for `--cpu jit`; the
positional backend argument is ignored by `--cpu noncaching`. The numbers
above are `hostSeconds`, `simInsts`, `hostInstRate` and `hostMemory` from
each run's `stats.txt`.
