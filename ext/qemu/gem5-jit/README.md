# gem5 QEMU JIT integration

This directory is owned by the gem5 repository. It contains the narrow C ABI
adapter, its standalone smoke test, and `qemu.patch`, which provides the
minimal QEMU build and RISC-V translation hooks required by `RiscvJitCPU`.

ABI version 5 supports multiple gem5 JitCPU objects in one process. The first
adapter initialization fixes the backend instance count and creates one QEMU
RISC-V vCPU per instance; subsequent initializations provide a unique instance
ID, gem5 hart ID, and callbacks for that vCPU. The vCPUs keep independent
architectural and transient state while sharing one QEMU physical address
space. They execute serially through `tcg,thread=single`, as required by the
current embedded runtime.

The QEMU source checkout is the clean, pinned submodule at `../repo`. Do not
place adapter files in that checkout or patch it in place. The build helper at
`util/jitcpu/build-qemu-jit.sh` copies the clean QEMU checkout, without Git
metadata, into a build source snapshot; applies `qemu.patch` there; and builds
the adapter against the sources in this directory. Copying the checkout also
preserves populated QEMU Meson subprojects for offline rebuilds. This keeps
submodule status meaningful and makes all gem5-owned integration changes
visible in the parent repository.

Run the helper from the gem5 repository root:

```sh
git submodule update --init ext/qemu/repo
util/jitcpu/build-qemu-jit.sh
```

The helper also runs `gem5-qemu-jit-smoke`, which initializes two harts with
different `mhartid` values and checks that executing either hart does not alter
the other hart's PC or integer-register state.

The adapter and integration patch are GPL-2.0-or-later. A build containing or
linking this QEMU-derived backend must be handled as a GPL-covered build.
