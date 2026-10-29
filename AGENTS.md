# AGENTS.md

This repository contains a GPU microarchitecture lab with three explicit
execution backends: rocjitsu with the simdojo functional simulator, rocjitsu
attached to gem5's no-guest HSAKMT service, and gem5 GPU full-system simulation.

## Project context

- Active branch focus: `src/dev/hsakmt-service/`.
- Main assignment document: `gpu-lab/GpuAssignmentRU.adoc`; the committed
  `gpu-lab/GpuAssignmentRU.pdf` is rendered from it with `asciidoctor-pdf`.
- Main lab bundle: `gpu-lab/workspace/`
- Canonical backend names: `simdojo`, `gem5-kmd`, and `full-system`.
- Provided sample apps live in `gpu-lab/workspace/samples/`; helper scripts
  live in `gpu-lab/workspace/scripts/`. Lab launcher examples below assume
  `gpu-lab/workspace/`; commands starting with `make -C src/` assume the
  repository root.
- The canonical interactive architecture introduction is
  [`ROCJITSU-KFD-GEM5.html`](ROCJITSU-KFD-GEM5.html). It explains how the GPU
  executes work, how a driver and the GPU interact, KFD RPC, and the gem5
  HSAKMT service.
- Large/generated lab artifacts such as `work/`, `images/`, `docker/`, `disk/`, `kernel/`, and `verification/` are intentionally ignored.
- Prefer offline/reproducible workflows; do not add network-dependent steps unless explicitly requested.

## Docker access from the Codex sandbox

In this workspace, ordinary sandboxed shell commands cannot access the Docker
socket. Run `docker`, `docker compose`, and repository scripts that invoke
Docker with escalated sandbox permissions from the first attempt; do not spend
an initial probe on an unprivileged invocation. Once escalation is active,
treat any remaining daemon, image, platform, or container error as a separate
problem rather than as a sandbox-permission failure.

## Documentation style

Keep `ROCJITSU-KFD-GEM5.html` up to date with the current architecture.
Any change to execution backends, upstream integration, ownership boundaries,
C ABI or RPC, memory/queue/event lifecycle, packaging, or supported limitations
must update the affected explanations and interactive diagrams in the same
change. Verify claims against the implementation, distinguishing upstream
capabilities from our fork additions and generic GPU-model fixes from service
behavior. Do not present illustrative animation as measured simulation data.

Keep this introduction a single, self-contained HTML/CSS/JavaScript page that
works offline without external libraries, fonts, or assets. Preserve keyboard
access, reduced-motion support, and readable content without JavaScript.
Its KFD RPC message layouts must match `rpc.h` byte for byte. After changing
it, check desktop and mobile layouts, light and dark themes, every interactive
figure, the no-JavaScript fallback, and local links. Keep procedures in the
practical guides and link to them; do not restore the superseded Markdown
document or the replaced `ROCJITSU_GEM5_BACKEND.html`.

Write documentation as a continuous explanation for its intended reader, not
as a collection of locally correct paragraphs.

- Preserve narrative order: introduce the object and motivation first, then
  the relationships and alternatives, and only then implementation details or
  instructions. Never refer to options, components, or constraints before the
  reader has been given the context needed to understand them.
- Be concise without removing the transitions that make the text readable.
  After editing, read the whole section in sequence and check that each
  paragraph follows naturally from the previous one.
- Apply a strict relevance test to every fact. Keep it only if it helps the
  reader understand the subject, make a decision, perform the task, interpret
  a result, or avoid a likely error. Internal class names and implementation
  details do not belong in conceptual material unless they change one of those
  outcomes.
- Separate concepts from procedures. Main assignment documents should explain
  what is modeled, why it matters, and how the pieces relate. Put command
  lines, environment setup, exact paths, and troubleshooting procedures in
  dedicated practical guides and link to them instead of duplicating them.
- Do not add generic filler, exhaustive enumerations, or precautionary caveats
  by default. Include a caveat only when it materially changes how a result
  should be interpreted or what the reader should do, and verify that claim
  against the implementation when possible.
- When several workflows lead to the same measured component, distinguish the
  setup or delivery path from the behavior being measured. Do not imply that
  the workflows change the measured result without concrete evidence.
- In student-facing documents, address the reader consistently as “you” rather
  than describing an abstract “student.”

## Committing changes

This branch uses the `gpu-lab:` commit-message prefix for lab-bundle work
(scripts, docs, samples). That tag is not in gem5's `MAINTAINERS.yaml`, so the
`gem5-commit-msg-checker` pre-commit hook rejects it. When committing changes
scoped to `gpu-lab/` with a `gpu-lab:` prefix, commit with `--no-verify` to
keep the branch convention consistent with prior commits. Do not use
`--no-verify` to bypass other hooks (whitespace, formatting, large-file, etc.)
or to silence failures unrelated to the invalid-tag check.

For commits that touch gem5 source under a tracked path (`src/`, `configs/`,
`tests/`, etc.), use a valid gem5 tag from `MAINTAINERS.yaml` instead and let
the hooks run normally.

## Current branch work

In this branch, assume the main implementation work belongs under:

```sh
src/dev/hsakmt-service/
```

This directory contains the gem5-side HSAKMT service, embedded rocjitsu KMD
adapter, GPU-facing work orchestration, and CPU-less platform used by the
no-OS HSAKMT path. rocjitsu and the pinned ROCr source remain separate under
`ext/hsakmt-client/rocm-systems`.

Keep edits scoped to `src/dev/hsakmt-service/` unless the change truly needs a
supporting update elsewhere. Common supporting locations are:

- `configs/example/gpufs/` for wiring the service into GPUFS runs.
- `src/dev/hsa/` for generic HSA packet-processor behavior.
- `src/dev/amdgpu/` and `src/gpu-compute/` only for generic GPU-model behavior
  that would also make sense outside the HSAKMT server.
- `gpu-lab/` for lab documentation and assignment notes.
- `ext/hsakmt-client/rocm-systems/emulation/rocjitsu/` for the KFD RPC,
  reusable server, interposer, and device-backend abstraction.
- `ext/hsakmt-client/rocm-systems/projects/rocr-runtime/` for pinned ROCr.

## Preserve the rocjitsu/service split

Keep the process and build-system boundary between these two trees clean:

- `ext/hsakmt-client/rocm-systems/emulation/rocjitsu/` owns the Linux/ROCr
  side: the KFD and DRM interposer loaded into the application, KMD RPC codec
  and transport, `RemoteDriver`, `SimulatedKfd`, events, topology,
  reusable KMD server, device-backend abstraction, public C embedding API,
  and rocjitsu tests. It is a standalone CMake project.
- `src/dev/hsakmt-service/` owns the gem5 side: SimObjects, CPU-less
  platform responsibilities, the C callback adapter, GPUVM, allocation,
  queue, doorbell and interrupt orchestration, and service tests. It uses the
  standard gem5 SCons build. Do not restore a service-local `CMakeLists.txt`
  or a gem5 copy of rocjitsu's RPC.

The dependency direction is one way. `ext/hsakmt-client/` must not include
gem5 headers, link gem5 libraries, consume generated SimObject files, invoke
SCons, or depend on anything under `src/dev/hsakmt-service/`. The service may
include rocjitsu's installed `rocjitsu/kmd/server.h` and link the narrowly
exported `librocjitsu_kmd_server.so`. No C++ type, exception, gem5 header, or
SimObject crosses that C ABI.

The current embedding ABI is version 2 (`librocjitsu_kmd_server.so.2`), and
KMD RPC is version 4. The C API is an in-process boundary; the Unix socket is
the inter-process boundary. Reuse upstream KFD policy and daemon transport
for both backends. `SimdojoDeviceBackend` supplies functional execution;
`CallbackDeviceBackend` dispatches external-device operations to gem5's
owner thread. Keep shared-backing slices, allocation leases, and teardown
semantics explicit. Unsupported external-device operations must fail, not
silently claim simulated mappings or completions.

Keep build and packaging ownership aligned with that boundary:

- build and test rocjitsu with its CMake project;
- build the gem5 service and service tests through its `SConscript`;
- use `src/dev/hsakmt-service/Makefile` only as an orchestration and
  verification entry point across the two native build systems; and
- package full rocjitsu and ROCr in the SDK image, but package the server-only
  KMD library with gem5 in the runner image. Do not put gem5 service binaries
  or sources into the SDK image.

When changing the public C ABI, KMD RPC, shared backing, or device-backend
contract, verify the full rocjitsu CTest suite and the SCons service tests,
then run the end-to-end workload gate below.

Do not put HSAKMT-server policy into shared AMDGPU or GPU compute internals.
Prefer modeling the missing platform/KFD behavior in `src/dev/hsakmt-service/`
and drive the GPU through PCI config, BAR MMIO, doorbells, GPUVM page tables,
DMA, and interrupt-ring handling.

Never add HSAKMT-service workarounds to the real GPU model code under
`src/dev/amdgpu/`, `src/gpu-compute/`, or `src/dev/hsa/`. This includes
service-specific conditionals, bypasses, fake completions, special address or
requestor handling, injected statistics, and callbacks that short-circuit the
normal GPU pipeline. The CPU-less platform deliberately lacks the CPU,
firmware, OS, and kernel-mode driver (KMD) that configure and operate a real
GPU. `src/dev/hsakmt-service/` must model those missing software/platform
responsibilities and drive the unmodified GPU model through realistic hardware
interfaces. If the service cannot yet perform a required operation, extend the
service or its platform emulation instead of patching the GPU to accommodate
the service.

A shared GPU-model change is acceptable only when it is independently correct
for normal GPUFS or another realistic platform and does not encode knowledge of
the HSAKMT service.

HSAKMT-service refactorings must also be verified end to end with the service
sample workloads:

```sh
make -C src/dev/hsakmt-service verify-sample-workloads
```

This gate runs the HIP vector-add, HIP matmul, HIP kernel-chain, and rainbow-image applications
through `configs/example/gpufs/hsakmt_server.py`, exercising the external ROCr
runtime, rocjitsu KFD RPC, embedded server, service, and GPU work path
together. Check that the applications pass, all submitted AQL packets retire,
no frontend/RPC errors are reported, and no KMD or service resources remain.
This service-specific flow uses `X86TimingSimpleCPU` and does not require
`/dev/kvm`; the generic GPUFS sample runner below boots a full Linux guest
with `X86KvmCPU` and does.

## Docker images

Two locally built images cover the current lab workflows. Both are tagged
`:local`; the launchers do not pull missing images from a registry.

| Image | Built by | Contains |
| --- | --- | --- |
| `gem5-gpu-hsakmt-sdk:local` | `gpu-lab/workspace/dockerfiles/gpu-hsakmt-sdk/build.sh` | HIP 7.1 toolchain, ROCr 1.21 from the pinned submodule, and full rocjitsu |
| `gem5-gpu-hsakmt-runner:local` | `gpu-lab/workspace/dockerfiles/gpu-hsakmt-runner/build.sh` | `gem5.opt`, the HSAKMT service, `librocjitsu_kmd_server.so`, configs, and launcher |

`gpu-lab/workspace/scripts/build-hsakmt-images.sh` builds the pair together and
checks that both record the current gem5 revision. Override either name with
`HSAKMT_SDK_IMAGE` and `HSAKMT_RUNNER_IMAGE`, which the Compose topology and
every launcher script honour:

```sh
HSAKMT_SDK_IMAGE=gem5-gpu-hsakmt-sdk:test \
  ./scripts/run-sample.sh --backend gem5-kmd hip-vector-add
```

The two must share a platform; the launcher reads it from each image and fails
on a mismatch. The build scripts support `linux/amd64` and `linux/arm64`;
validate the selected platform rather than treating an x86-64 pass as AArch64
runtime evidence. Note that the launcher resolves platform through the image
ID rather than the tag, because `docker image inspect` on a buildx manifest
list reports empty `Os` and `Architecture` fields.

The SDK image supports the two rocjitsu-based backends. `rocjitsu-run` without
`--attach` uses simdojo; `rocjitsu-run --attach` connects to the KMD server
embedded in gem5. The `full-system` backend uses the runner image's gem5
binary with the packaged GPUFS guest disk, kernel, and ROCm stack; it does not
start the embedded KMD service. The archived GPUFS runtime remains available
through `FULL_SYSTEM_IMAGE` for historical runs.

## Running provided samples

The canonical commands and prerequisites for all three backends are in
`gpu-lab/RunningTheLab.md`. The shared scripts require an explicit `--backend`
and keep builds separate under `work/apps/<backend>/`. `full-system` requires
an x86-64 host with `/dev/kvm`; `gem5-kmd` and `simdojo` do not.

Changes to the shared build, launch, or verification scripts must pass all
three end-to-end sample gates:

```sh
./scripts/run-all-samples.sh --backend simdojo
./scripts/run-all-samples.sh --backend gem5-kmd
./scripts/run-all-samples.sh --backend full-system
```

Each gate rebuilds and verifies HIP vector-add, HIP matmul, HIP kernel-chain,
and rainbow-image on the selected backend. Do not use a successful run from one
backend as evidence for another.

## Building ROCr from `ext/hsakmt-client/rocm-systems`

The active ROCr source is the `ext/hsakmt-client/rocm-systems` submodule, with ROCr under:

```sh
ext/hsakmt-client/rocm-systems/projects/rocr-runtime
```

The successful local full ROCr build used LLVM/Clang 22 for host/device compilation and the installed ROCm device bitcode from LLVM 21. The relevant apt packages are:

```sh
sudo apt install \
  cmake build-essential pkg-config \
  libnuma-dev libdrm-dev libelf-dev \
  libclang-22-dev lld-22 rocm-device-libs-21 \
  libedit-dev libcurl4-openssl-dev
```

Configure outside the source tree:

```sh
cmake -S ext/hsakmt-client/rocm-systems/projects/rocr-runtime \
  -B /tmp/rocm-systems-rocr-full-build-llvm22-hipdevlib \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DEXPORT_TO_USER_PACKAGE_REGISTRY=OFF \
  -DClang_DIR=/usr/lib/llvm-22/lib/cmake/clang \
  -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm
```

Build with `HIP_DEVICE_LIB_PATH` set, otherwise Clang 22 does not find the installed ROCm device libraries:

```sh
HIP_DEVICE_LIB_PATH=/usr/lib/llvm-21/lib/clang/21/amdgcn/bitcode \
  cmake --build /tmp/rocm-systems-rocr-full-build-llvm22-hipdevlib -j 20
```

Expected main artifact:

```sh
/tmp/rocm-systems-rocr-full-build-llvm22-hipdevlib/rocr/lib/libhsa-runtime64.so
```

Do not switch this build to LLVM 21 just to match `rocm-device-libs-21`: current ROCr `develop` gfx12 trap-handler assembly failed with LLVM 21 in this environment. The working combination was LLVM/Clang 22 plus `HIP_DEVICE_LIB_PATH` pointing at the LLVM 21 ROCm device bitcode directory.

## rocjitsu-based backends

rocjitsu provides the lab's common application-side execution flow and is
vendored in the same pinned submodule as ROCr under
`ext/hsakmt-client/rocm-systems/emulation/rocjitsu`. Its local backend uses the
simdojo-based functional AMDGPU simulator, which executes the GPU ISA in the
application’s own process and needs no server, socket, or second container.
Attach mode uses the same interposed application path with gem5's HSAKMT
service and timing model.

Upstream already provides simdojo daemon mode and Unix-domain KFD RPC. Our
fork makes that KFD/server infrastructure reusable with an external device
and exports the embedding C API; it does not introduce a parallel IPC stack.

Both rocjitsu-based backends use rocjitsu's `/dev/kfd` interposer. Local mode starts
rocjitsu's simdojo-backed KMD implementation; attach mode connects
`RemoteDriver` to the existing socket published by gem5. There is no ROCr
model-mode client or `HSA_MODEL_TOPOLOGY` path in the active integration.

`ROCJITSU_CONFIG` takes a bare filename, resolved against
`/opt/rocjitsu/share/rocjitsu/configs`, or an absolute path. Use the `_kmd`
configs: they carry the `drm_render_minor` field the interposer's DRM emulation
needs. Attach mode gets device identity and topology from the embedded server
and does not require a simdojo config. Pass `--daemon` as the first argument
to `rocjitsu-run` to host the simulator in its own process, which multi-process
workloads such as `torchrun` require.

The gem5-kmd service currently supports one attached process and one
MI300X/gfx942 GPU. External SVM attributes/migration, queue suspension,
non-temporal mapping policy, cross-process IPC workloads, and connected
checkpoints are unsupported. Cold checkpoints precede listener publication
and external resources. Do not infer these limitations apply to simdojo.

Match the config to the code object. rocjitsu's simulation maturity differs by
target: gfx950 (CDNA4) is beta, gfx94x (CDNA3) and the RDNA targets are
experimental. Its cross-ISA translator can bridge a mismatch, but that is a
separate and less mature path.

Verify a run by its result rather than its exit code. `hip-vector-add` prints a
checksum equal to 3·n(n−1)/2, so a wrong or skipped kernel is visible in the
output.

Two constraints apply when changing rocjitsu itself. It is inside a submodule,
so fixes there need their own commit in `ext/hsakmt-client/rocm-systems` and do
not travel with a gem5-side commit. And the Dockerfile installs the CLI and
`librocjitsu.so` into the `bin`/`lib` pair under one prefix because the CLI
locates its interposer relative to its own binary; splitting them breaks the
launcher.
