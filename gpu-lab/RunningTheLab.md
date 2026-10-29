# Running the GPU lab

All commands in this guide run from `gpu-lab/workspace`. The same three scripts
build and execute every supported backend; `--backend` is required so that a
run cannot silently use the wrong simulator.

## Requirements

All backends require Docker and enough free space for their images and run
outputs. `gem5-kmd` additionally requires Docker Compose v2; its matched SDK
and runner containers must use the same Linux Docker engine.

`simdojo` and `gem5-kmd` run on `linux/amd64` or `linux/arm64` and do not need
KVM. `full-system` requires an x86-64 Linux host with hardware virtualization
enabled and `/dev/kvm` accessible to Docker.

The scripts use only local images and run workload containers without network
access. They do not pull a missing image automatically.

## Choose a backend

`simdojo` runs the application through rocjitsu's functional GPU model. It
executes AMD GPU ISA and is the quickest way to validate a kernel, but it does
not produce gem5 timing statistics.

`gem5-kmd` runs the application through rocjitsu on the gem5 AMDGPU model
without booting a guest. rocjitsu supplies the KFD/DRM interface to ROCr and
forwards queue, memory, doorbell, and event operations to the KMD server
embedded in gem5. This backend produces gem5 configuration and statistics and
does not require KVM.

`full-system` is the original GPUFS workflow. gem5 boots Linux, and the
application reaches the AMDGPU model through the guest ROCm stack and
`amdgpu`/KFD driver. It requires an x86-64 host with `/dev/kvm`.

The two gem5 backends use the same modeled GPU pipeline, but their platform
setup and software paths differ. Do not combine their statistics in one
baseline/optimized comparison. Record the selected backend in your report.

In `full-system`, ROCr treats the modeled MI300 as a full-profile HSA agent and
uses compute blit kernels for the sample's `hipMemcpy` calls. Setting
`GPU_FORCE_BLIT_COPY_SIZE=0` and `HSA_ENABLE_SDMA=1` does not override this:
ROCr restricts its SDMA blit objects to base-profile agents. Account for these
internal kernels when interpreting aggregate CU statistics.

## Model size and interpretation

Both gem5 lab launchers default to **8 CUs with a fixed 2.1 GHz GPU clock**.
Each CU has four SIMDs, 64-lane waves, 32 wave slots, 512 KiB of vector registers,
12.5 KiB of scalar registers, 64 KiB LDS and a 32 KiB vector L1. CU pairs share
a 64 KiB instruction cache and a separate 16 KiB scalar cache. The shared L2
is **1 MiB**, a rounded proportional reduction from an MI300X XCD.

See the [MI300X configuration audit](MI300XConfiguration.md) for sources,
clock wiring, retained timing assumptions and the reduced memory topology.
The single default HBM interface still peaks at 16 GB/s; eight CUs can be
bandwidth-limited. Historical timing studies predate this configuration.

For scaling experiments, pass `--num-compute-units=16` after the application's
`--` separator. Keep CU pairs for the default cache sharing and inspect the
resulting `config.ini`; L2 does not automatically scale with a CU override.
The KMD service reports modeled resources. The packaged full-system guest
still reports a larger CU topology, so derive study grids from explicit inputs.
Simdojo retains its own functional device topology.

Rebuild the runner image after configuration changes. The full-system launcher
overlays this checkout's `mi300.py` and its `amd` helpers; the other imported
modules and Ruby configuration come from the branch-built runner image.

## Prepare the local images

Build the matched SDK and runner images from the checked-out gem5 and rocjitsu
sources. The runner also supplies gem5 for current `full-system` runs:

```sh
./scripts/build-hsakmt-images.sh
```

The result is `gem5-gpu-hsakmt-sdk:local` plus
`gem5-gpu-hsakmt-runner:local`. Both builds are local; runtime containers use
`--network none`.
The Dockerfiles, build helpers, and container entrypoints for this pair live
under `gpu-lab/workspace/dockerfiles/`; the builds use gem5 source and the
checked-out `ext/hsakmt-client/rocm-systems` submodule.

The `full-system` guest disk, matching kernel, and application builder come
from a separate GPUFS offline bundle. They are not built by
`build-hsakmt-images.sh`. The bundle also includes an older GPUFS runtime image
for historical runs. That image predates the LDS counter fix and does not pass
the current six-sample gate. Its files are:

```text
MANIFEST.sha256
images/gem5-gpu-fs-v25-1.tar.gz
images/gem5-gpufs-v25.1.0.1.tar.gz
disk/x86-ubuntu-rocm70
kernel/vmlinux-rocm70
```

Verify the unpacked bundle in its own directory. Then put its four image,
disk, and kernel files in the corresponding paths under `gpu-lab/workspace/`.
Copies work; symlinks avoid duplicating the large files. From the workspace,
set `GPUFS_BUNDLE` to the absolute path of the unpacked bundle and run:

```sh
GPUFS_BUNDLE=/absolute/path/to/gpu-lab-offline-v25.1.0.1
(cd "${GPUFS_BUNDLE}" && sha256sum -c MANIFEST.sha256)
mkdir -p images disk kernel
ln -s "${GPUFS_BUNDLE}/images/gem5-gpu-fs-v25-1.tar.gz" images/
ln -s "${GPUFS_BUNDLE}/images/gem5-gpufs-v25.1.0.1.tar.gz" images/
ln -s "${GPUFS_BUNDLE}/disk/x86-ubuntu-rocm70" disk/
ln -s "${GPUFS_BUNDLE}/kernel/vmlinux-rocm70" kernel/
./scripts/load-images.sh
```

The workspace ignores these large inputs. `load-images.sh` loads the GPUFS
application builder and tags the archived runtime for optional historical
runs. The default `full-system` launcher uses the branch-built runner image.

To build a new guest disk instead of using the packaged one, follow the
upstream guides for the
[full-system AMD GPU model](https://www.gem5.org/documentation/general_docs/gpu_models/gpufs),
the [GPUFS application-build Docker image](https://github.com/gem5/gem5/tree/stable/util/dockerfiles/gpu-fs),
and the [GPUFS guest disk and kernel](https://github.com/gem5/gem5-resources/blob/stable/src/x86-ubuntu-gpu-ml/BUILDING.md).
The upstream disk builder requires `unzip` and `qemu-system-x86_64`, downloads
Packer and Ubuntu/ROCm packages, and runs `./build.sh` in
`gem5-resources/src/x86-ubuntu-gpu-ml/`. As of September 2026, its
[install script](https://github.com/gem5/gem5-resources/blob/stable/src/x86-ubuntu-gpu-ml/scripts/rocm-install.sh)
selects ROCm 7.0 and installs PyTorch; remove the PyTorch install line before
building if you want a guest without it like the packaged disk. The output is
`disk-image/x86-ubuntu-gpu-ml` and `vmlinux-gpu-ml`; place them under the
workspace's `disk/x86-ubuntu-rocm70` and `kernel/vmlinux-rocm70` names. Pair
the disk with the kernel extracted during that build, and match the app
builder's ROCm version to the guest. The upstream recipe builds a compatible
guest; it does not reproduce the packaged disk exactly. The runner image
supplies the current gem5 executable.

## Build and run the samples

Builds are stored per backend because the no-guest backends may use a native
ARM64 application while the full-system guest always needs an x86-64
application:

```sh
./scripts/build-samples.sh --backend simdojo
./scripts/build-samples.sh --backend gem5-kmd
./scripts/build-samples.sh --backend full-system
```

Run one sample with the same backend used for its build:

```sh
./scripts/run-sample.sh --backend simdojo hip-vector-add
./scripts/run-sample.sh --backend gem5-kmd hip-vector-add
./scripts/run-sample.sh --backend full-system hip-vector-add
```

To build, run, and verify all provided samples on one backend:

```sh
./scripts/run-all-samples.sh --backend gem5-kmd
```

Add `--no-build` to reuse `work/apps/<backend>/`. The samples are
`hip-vector-add`, `hip-matmul`, `hip-matmul-lds`, `hip-matmul-regtiles`,
`hip-kernel-chain`, and `rainbow-image`.

A successful complete run ends with:

```text
all <backend> samples passed
```

The verifier checks the application exit status and these reference results:

| Sample | Expected result |
| --- | --- |
| `hip-vector-add` | checksum `6048` |
| `hip-matmul` | `m=256 n=256 k=128`, row-major `B`, checksum `102760448` |
| `hip-matmul-lds`, `hip-matmul-regtiles` | same dimensions and checksum, `seed=0`, `tiles_per_workgroup` all `4` |
| `hip-kernel-chain` | checksum `40768` |
| `rainbow-image` | `PASS` and a BMP matching the backend's reference image |

For `gem5-kmd`, verification also requires the expected kernel count (the
optimized samples launch one extra runtime blit kernel to copy back their
per-workgroup tile counters), equality
of submitted and retired AQL packets, successful gem5 termination, and no
remaining KMD or service resources. Treat the verifier's final result, rather
than an application exit code alone, as the sample outcome.

The default `hip-matmul` computes `A[256][128] × B[128][256] → C[256][256]`
with one output per thread and 16×16-thread workgroups (256 workgroups).
Keep these matrix dimensions for baseline/optimized comparisons. The
optimized samples split the output into 32 tiles of 64×32, which eight
persistent workgroups walk four at a time with eight reduction steps at K
tile depth 16. The sample also accepts `--size S` to set all three dimensions
to `S` for square debug inputs, and `--transpose-b` to change B's storage
order. Pass these options through `run-app.sh`; `run-sample.sh` verifies the
defaults.

### Naive matmul timing reference

The current GEMM study measured the samples on the revised configuration at
2.1 GHz with `gfx942`, `-O2` and the dynamic register allocator, which is
now the lab default (see the configuration audit). At the default
dimensions, naive GEMM takes **107.3 s** of launcher wall time excluding
compilation and **672,600 GPU-active cycles** (rounded). This is one run;
launcher time includes startup, execution, verification and cleanup. The
delivered LDS and cache/register samples take median GPU-active times of
300,264 and 327,086 cycles across five runs each, against 321,898 for the
best conventional one-tile-per-workgroup LDS kernel and 317,488 for the
cooperative persistent double buffer. The specialised pipeline's 5.4% lead
over that cooperative control has nonoverlapping five-run ranges; see
[GemmOptimization.md](GemmOptimization.md) for the controls, traces, model
findings and reproducible commands.

For your own run, divide `shaderActiveTicks` by `simFreq` from `stats.txt` to
obtain active seconds. Divide the same ticks by the shader clock period from
`config.ini` to obtain GPU cycles (here, 476 ticks per cycle). This measures
the time during which at least one CU is active; it is not a sum of per-CU
cycles. The reference run launched one kernel, retired all four submitted AQL packets,
and drained KMD state. Whole-run `simSeconds` includes service time outside
GPU execution and is not the kernel duration.

## Run your own application

Keep the executable somewhere below the workspace root, then select its backend
explicitly:

```sh
./scripts/run-app.sh \
  --backend gem5-kmd \
  --run-name my-baseline \
  work/apps/gem5-kmd/my-app \
  --size 64
```

The interface is:

```text
run-app.sh --backend BACKEND [--run-name NAME] [--gem5-exe PATH] \
  [--gem5-arg ARG] APP [application arguments...] [-- config arguments...]
```

Arguments before `--` belong to the application. Arguments after `--` belong
to the selected Python configuration in `gem5-kmd` and `full-system`. This is
where you pass model parameters such as `--gpu-clock`:

```sh
./scripts/run-app.sh \
  --backend gem5-kmd \
  --run-name faster-clock \
  work/apps/gem5-kmd/hip-vector-add \
  -- \
  --gpu-clock=1.2GHz
```

Top-level gem5 options must precede the Python configuration, so pass each one
with `--gem5-arg`:

```sh
./scripts/run-app.sh \
  --backend gem5-kmd \
  --run-name wavefront-trace \
  --gem5-arg=--debug-flags=GPUExec \
  --gem5-arg=--debug-file=wavefront.trace.gz \
  work/apps/gem5-kmd/hip-vector-add
```

The equivalent full-system command uses its backend-specific executable:

```sh
./scripts/run-app.sh \
  --backend full-system \
  --run-name wavefront-trace \
  --gem5-arg=--debug-flags=GPUExec \
  --gem5-arg=--debug-file=wavefront.trace.gz \
  work/apps/full-system/hip-vector-add \
  -- \
  --gpu-clock=1.2GHz
```

Use `GPUKernelInfo` when you need to distinguish the application kernel from
runtime blit kernels in a full-system run:

```sh
./scripts/run-app.sh \
  --backend full-system \
  --run-name kernel-check \
  --gem5-arg=--debug-flags=GPUKernelInfo \
  work/apps/full-system/hip-vector-add
```

`simdojo` has no gem5 configuration and rejects both forms of gem5 argument.
`--gem5-exe` is available only for `full-system`; it mounts a host-built gem5
executable together with the configuration tree from the same checkout.

The provided Makefile uses
`--offload-arch=gfx942 -O2 -std=c++17`. Build custom programs in the same SDK
image used by the selected backend. Do not reuse an ARM64 no-guest executable
in the x86-64 full-system guest.

## Read the result

Every run has a unique directory under `work/runs/`. `run.env` records the
backend, executable, and target. Application output is in `app.stdout` for
`simdojo` and `gem5-kmd`; full-system guest output is normally in
`system.pc.com_1.device`.

Both gem5 backends produce `config.ini`, `config.json`, `stats.txt`, and
`gem5.log`. `simdojo` intentionally does not produce those files. The rainbow
BMP is placed directly in the run directory in every backend. A gem5 run also
stores its top-level and configuration arguments in the NUL-delimited
`gem5.args` and `config.args` files.

`exit-status.txt` records the application and simulator status where applicable.
Keep the entire run directory for any result used in the report.

## Configuration

The most useful overrides are:

- `GPU_TARGET` and `ROCJITSU_CONFIG` for the code object and simdojo device;
- `HSAKMT_SDK_IMAGE`, `HSAKMT_RUNNER_IMAGE`, and `HSAKMT_PLATFORM` for the
  local SDK and runner images;
- `FULL_SYSTEM_SDK_IMAGE` for the packaged GPUFS application builder, and
  `FULL_SYSTEM_IMAGE` to override the default branch-built runner with a
  historical GPUFS runtime image; and
- `GEM5_EXE` as the environment equivalent of `--gem5-exe`.

The selected rocjitsu configuration must match the code object's GPU target.
The `_kmd` configurations contain the DRM metadata required by the interposer.

## Troubleshooting

If Docker access is denied, use the Docker setup approved for the lab host.
The scripts do not pull missing images.

If `full-system` reports that `/dev/kvm` is missing, enable virtualization and
load the KVM modules, or use `gem5-kmd`.

If a no-guest executable has the wrong host architecture, rebuild it with
`build-samples.sh --backend ...`; the launcher checks the ELF machine before
starting a simulation.

If a sample fails, inspect its `exit-status.txt`, application output, and
`gem5.log` before changing the verifier. A checksum mismatch is evidence of a
different result, not a reason to update the expected value.
