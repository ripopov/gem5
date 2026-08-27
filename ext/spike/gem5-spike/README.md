# gem5 Spike adapter

A small C++ adapter that presents Spike (`riscv-isa-sim`) to gem5 through a
plain C ABI, so gem5's `RiscvSpikeCPU` can execute guest instructions in the
RISC-V reference simulator while gem5 keeps the platform.

```text
ext/spike/repo/           pinned riscv-isa-sim submodule, unmodified
ext/spike/gem5-spike/     this adapter
    gem5-spike.h          the C ABI gem5 loads with dlopen
    gem5-spike.cc         one simif_t + processor_t per gem5 CPU
    gem5-spike-smoke.cc   standalone unit test, no gem5 involved
util/spikecpu/build-spike.sh   builds both into build/spike/
```

Build it with:

```sh
git submodule update --init ext/spike/repo
util/spikecpu/build-spike.sh    # -> build/spike/libgem5-spike.so
```

The helper configures and builds Spike out of tree, compiles the adapter
against it, then compiles and runs `gem5-spike-smoke`, which covers two
independent harts, direct-mapped RAM, an MMIO access, the gem5
pseudo-instruction exit, bounded instruction accounting and translation
invalidation.

## What the adapter does

Spike's `simif_t` is exactly the seam gem5 needs, so the adapter implements
it and nothing else has to be reimplemented:

| `simif_t` member | gem5 side |
| --- | --- |
| `addr_to_mem` | a host pointer into gem5's backing store, so Spike's software TLB reaches guest RAM with no callback at all |
| `mmio_load` / `mmio_store` | a packet on the CPU's data port, which runs the gem5 device model inline |
| `get_cfg`, `get_harts` | one hart per gem5 CPU, built from the ISA string gem5 reports |

Four details are worth knowing:

- **gem5 pseudo-instructions.** m5ops are RISC-V opcode `0x7b`, which Spike
  would take an illegal-instruction trap on. The adapter registers a custom
  instruction over the whole opcode and throws out of `processor_t::step()`
  from it, leaving the program counter on the m5op. gem5 then executes the
  pseudo-instruction against its own thread context and retires it, which is
  the same contract the QEMU backend uses.
- **Ending a batch early.** `processor_t::step()` cannot be interrupted from
  the inside, so a batch runs in chunks of 64 instructions and consults the
  `should_stop` callback between them. This is the counterpart of QEMU's
  `cpu_exit()`, which likewise only takes effect at the end of the
  translation block that performed the MMIO access.
- **Time.** gem5's `mtime` is constant for the whole batch, because a batch
  executes at one simulated tick, so it is sampled once into Spike's `time`
  CSR at the start. That is equivalent to servicing every `rdtime`.
- **Instruction counting.** The count comes from `minstret`. A guest that
  sets `mcountinhibit.IR` would freeze it, so the adapter falls back to
  single-stepping in that case. Instructions executed in the same chunk
  before an m5op are not counted, because the exception unwinds before Spike
  commits them; that is at most 63 instructions per m5op.

Spike is BSD licensed, so unlike QEMU it could have been linked into gem5
directly. It is loaded at run time anyway: gem5 then depends on one C ABI
rather than on Spike's C++ headers and autotools build, both functional
backends look the same from gem5, and either can be swapped without
rebuilding gem5.
