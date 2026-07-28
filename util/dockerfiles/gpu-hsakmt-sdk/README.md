# HSAKMT GPU SDK image

`gem5-gpu-hsakmt-sdk:local` is the application container for the GPU lab. It
contains the HIP 7.1 toolchain, ROCr 1.21 built from the pinned
`rocm-systems` source, and full rocjitsu. It does not contain gem5, service
sources, a model-ABI client, or a hand-maintained topology.

The same image supports two launch modes. In local mode, rocjitsu supplies
both KFD behavior and functional GPU execution. In attach mode, its
interposer and `RemoteDriver` connect to the KMD server embedded in gem5, so
the application ISA executes in gem5's timing GPU model.

## Build

Build the SDK and matching runner together from the repository root:

```sh
gpu-lab/workspace/scripts/build-hsakmt-images.sh
```

Or build only this image:

```sh
util/dockerfiles/gpu-hsakmt-sdk/build.sh
```

The build script creates a temporary context from the tracked
`rocr-runtime` and `rocjitsu` submodule sources plus their current dirty
overlays. It does not send unrelated submodule build products to Docker. ROCr
uses Clang 22 and the installed LLVM 21 ROCm device bitcode; rocjitsu uses its
native CMake build.

The final image records the gem5, ROCr, and rocjitsu revisions as labels.
The lab launcher requires the SDK and runner to use the same platform and
rocjitsu revision.

## Build the samples

From `gpu-lab/workspace`, use the shared backend-aware builder. The two
rocjitsu modes use the same SDK image but keep their host executables in
separate directories:

```sh
./scripts/build-samples.sh --backend simdojo
./scripts/build-samples.sh --backend gem5-kmd
```

`run-all-samples.sh` performs the corresponding build automatically unless
you pass `--no-build`.

## Run through gem5

Use the Compose launcher because attach mode needs the Unix socket, topology,
and shared descriptors supplied by the runner container:

```sh
cd gpu-lab/workspace
./scripts/run-sample.sh --backend gem5-kmd hip-vector-add
```

For an application built in `work/apps/gem5-kmd`, the command inside the SDK
container is equivalent to:

```sh
rocjitsu-run --attach /workspace/apps/hip-vector-add
```

Attach mode does not accept or need a simdojo configuration. Device metadata
comes from gem5's embedded KMD server.

## Run through the functional rocjitsu simulator

Use the same workspace launcher with the functional backend:

```sh
cd gpu-lab/workspace
./scripts/run-sample.sh --backend simdojo hip-vector-add
```

The launcher selects `gfx942_cdna3_kmd.json` by default. Override
`ROCJITSU_CONFIG` when the code-object target changes. Direct users of
`rocjitsu-run` may pass `--daemon` as its first argument when the functional
simulator must run in a separate process. This mode is independent of gem5
attach mode; it creates its own rocjitsu server backed by simdojo.

Verify either backend by application output rather than exit status alone.
For the default vector-add workload, the required checksum is `6048`.
