# RISC-V SpikeCPU

`RiscvSpikeCPU` is a fast functional CPU backed by Spike, the RISC-V
reference simulator (`riscv-isa-sim`). It is the second implementation of the
external-functional-backend CPU model that `RiscvJitCPU` introduced, and it
exists for the same reason: to skip uninteresting execution, such as a
full-system Linux boot, before switching the same harts to a detailed timing
model.

The two models differ only in which engine executes the guest.

| | `RiscvJitCPU` | `RiscvSpikeCPU` |
| --- | --- | --- |
| Engine | QEMU RISC-V TCG, a dynamic translator | Spike, an interpreter |
| Speed | fastest | slower, see the comparison report |
| Authority | fast, widely used, not normative | the golden model the ISA specification is validated against |
| License | GPL-2.0-or-later, hence a separate build | BSD-3-Clause |
| Backend size | a QEMU tree | one `.so` and a 400-line adapter |

`FuncBackendCPU.md` at the repository root compares them against each other
and against `AtomicSimpleCPU` on a measured BusyBox boot.

## What is shared

`src/cpu/funcbackend/` holds `RiscvBackendCPU`, the entire model: the
architectural state transfer, the batch budget against the event queue,
instruction accounting, physical memory and device access, m5
pseudo-instructions, WFI handling and CPU takeover. `src/cpu/jit/README.md`
specifies all of it, and everything it says applies here.

`RiscvSpikeCPU` adds only the `RiscvBackend` implementation over Spike's C
ABI, plus the ISA description Spike needs to build its hart: the ISA string,
the privilege-mode set and the widest supported virtual address, all derived
from this CPU's own gem5 `RiscvISA` object rather than configured twice.

`ext/spike/gem5-spike/README.md` documents the adapter on the other side of
that ABI, including how a Spike interpreter is made to stop mid-batch and how
gem5 pseudo-instructions are intercepted.

## Building and running

```sh
git submodule update --init ext/spike/repo
util/spikecpu/build-spike.sh                     # -> build/spike/libgem5-spike.so
scons build/RISCV/gem5.opt USE_SPIKECPU=y -j$(nproc)
```

`USE_SPIKECPU=y` builds the loader and links `libdl`; the gem5 binary has no
compile-time dependency on Spike. `backend_path` selects the shared library
at run time, and `USE_SPIKECPU=n` removes the model entirely.

The full-system configuration is shared with JitCPU. With the Linux
artifacts from `util/jitcpu/build-linux-image.sh` in place:

```sh
build/RISCV/gem5.opt -d m5out/spike \
    tests/gem5/jitcpu/configs/jitcpu_linux.py \
    build/jitcpu-linux/fw_jump.elf build/spike/libgem5-spike.so \
    --cpu spike \
    --kernel build/jitcpu-linux/vmlinux \
    --initrd tests/test-progs/jitcpu-smoke/src/jitcpu-linux-init.cpio
```

`--cpu jit`, `--cpu spike` and `--cpu noncaching` select the three models on
an otherwise identical system, which is what makes the comparison report
meaningful. `tests/gem5/jitcpu/README.md` covers the interactive BusyBox
session, the one-way switch to `RiscvO3CPU` and the Ruby CHI runs; all of
them accept `--cpu spike`.

## Limitations

Everything in `src/cpu/jit/README.md` section 11 applies. In addition:

- Spike's own devices are not used. gem5 owns the CLINT, PLIC, UART and
  every other device, so Spike sees only RAM and MMIO callbacks. This is
  deliberate: the platform must be gem5's, or a switch to a detailed CPU
  would not resume against the same machine.
- The hypervisor and vector extensions are refused by the shared model, not
  by Spike, which supports both. Lifting that needs the state transfer in
  `RiscvBackendCPU` extended first.
- Instructions retired in the same 64-instruction chunk as an m5op are not
  counted; see the adapter README. m5ops are rare enough that this is
  invisible in totals.
