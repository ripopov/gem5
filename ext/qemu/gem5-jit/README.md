# gem5 QEMU JIT integration

This directory is owned by the gem5 repository. It contains the narrow C ABI
adapter, its standalone smoke test, and `qemu.patch`, which provides the
minimal QEMU build and RISC-V translation hooks required by `RiscvJitCPU`.

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

The adapter and integration patch are GPL-2.0-or-later. A build containing or
linking this QEMU-derived backend must be handled as a GPL-covered build.
