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

## Prepare the local images

For `simdojo` and `gem5-kmd`, build the matched SDK and runner images from the
checked-out gem5 and rocjitsu sources:

```sh
./scripts/build-hsakmt-images.sh
```

The result is `gem5-gpu-hsakmt-sdk:local` plus
`gem5-gpu-hsakmt-runner:local`. Both builds are local; runtime containers use
`--network none`.

The `full-system` resources are distributed separately from the Git repository.
Unpack them directly into `gpu-lab/workspace/`, alongside this layout:

```text
MANIFEST.sha256
images/app-build-gpu-fs-v25-1.tar.gz
images/gem5-runtime-v25.1.0.1.tar.gz
disk/x86-ubuntu-rocm70
kernel/vmlinux-rocm70
```

The large `images/`, `disk/`, and `kernel/` contents are intentionally ignored
by Git. From `gpu-lab/workspace/`, verify the unpacked bundle and load its two
Docker image archives once:

```sh
sha256sum -c MANIFEST.sha256
./scripts/load-images.sh
```

To build these GPUFS resources yourself instead of using the packaged bundle,
follow the upstream guides for the
[full-system AMD GPU model](https://www.gem5.org/documentation/general_docs/gpu_models/gpufs),
the [GPUFS application-build Docker image](https://github.com/gem5/gem5/tree/stable/util/dockerfiles/gpu-fs),
and the [GPUFS guest disk and kernel](https://github.com/gem5/gem5-resources/blob/stable/src/x86-ubuntu-gpu-ml/BUILDING.md).
Build all components for the same gem5 release: the ROCm version in the
application-build image must match the ROCm stack and driver in the guest disk,
and the disk must be booted with the kernel extracted while it was built.

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
`hip-vector-add`, `hip-matmul`, `hip-kernel-chain`, and `rainbow-image`.

A successful complete run ends with:

```text
all <backend> samples passed
```

The verifier checks the application exit status and these reference results:

| Sample | Expected result |
| --- | --- |
| `hip-vector-add` | checksum `6048` |
| `hip-matmul` | `n=128`, row-major `B`, checksum `25690112` |
| `hip-kernel-chain` | checksum `40768` |
| `rainbow-image` | `PASS` and a BMP matching the backend's reference image |

For `gem5-kmd`, verification also requires the expected kernel count, equality
of submitted and retired AQL packets, successful gem5 termination, and no
remaining KMD or service resources. Treat the verifier's final result, rather
than an application exit code alone, as the sample outcome.

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
  no-guest images;
- `FULL_SYSTEM_SDK_IMAGE` and `FULL_SYSTEM_IMAGE` for the packaged GPUFS
  images; and
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
