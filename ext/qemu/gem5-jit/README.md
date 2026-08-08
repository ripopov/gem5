# gem5 QEMU JIT backend

This directory is owned by the gem5 repository. It contains the narrow C
adapter and its standalone smoke test. The pinned `gem5-jit` branch in the
QEMU submodule provides the minimal build and RISC-V translation hooks required
by `RiscvJitCPU`.

The adapter supports multiple gem5 JitCPU objects in one process. The first
adapter initialization fixes the instance count and creates one QEMU RISC-V
vCPU per instance. Each vCPU receives a unique instance ID, gem5 hart ID, and
callback set. The vCPUs keep independent architectural and transient state
while sharing one QEMU physical address space and executing serially through
`tcg,thread=single`.

The embedded CPU deliberately disables SSTC. QEMU's SSTC implementation uses
its own asynchronous virtual timer, whereas gem5 owns simulated time and the
CLINT interrupt path. Linux therefore uses OpenSBI's timer service, which
keeps timer interrupts synchronized with gem5 before and after a CPU switch.

The register API explicitly transfers FFLAGS and FRM even when
`mstatus.FS=Off`; ordinary architectural CSR helpers reject that valid switch
state. Translation invalidation flushes both the vCPU TLB and the shared TCG
translation-block cache before reverse takeover resumes.

The QEMU source checkout is the pinned fork commit at `../repo`. The build
helper copies it, without Git metadata, to a source snapshot and builds against
the adapter in this directory. Adapter changes remain in gem5 while the QEMU
integration hooks are versioned on the fork's `gem5-jit` branch.

On Ubuntu, the backend's direct build dependencies are:

```sh
sudo apt install \
  build-essential git tar meson ninja-build flex bison pkg-config \
  python3-dev libglib2.0-dev libpixman-1-dev libfdt-dev libffi-dev
```

On Apple Silicon macOS with Homebrew, install:

```sh
brew install \
  meson ninja pkgconf glib pixman dtc \
  riscv-gnu-toolchain isl libmpc mpfr
```

Run the helper from the gem5 repository root:

```sh
git submodule update --init --depth 1 ext/qemu/repo
util/jitcpu/build-qemu-jit.sh
```

The helper also runs `gem5-qemu-jit-smoke`. It initializes two harts with
different `mhartid` values and checks independent PC/GPR state, execution,
FFLAGS/FRM transfer while FS is Off, and translation invalidation. The final
backend path ends in `.so` on Linux and `.dylib` on macOS.

The adapter and QEMU integration changes are GPL-2.0-or-later. Dynamic loading
is an engineering boundary, not a guarantee that distributing gem5 with the
QEMU-derived backend avoids GPL obligations. See `src/cpu/jit/README.md` for
the complete architecture, validation, limitations, and licensing statement.
