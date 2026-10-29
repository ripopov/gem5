# rocjitsu-backed HSAKMT service

This directory contains the gem5 half of the no-guest HSAKMT path. gem5
embeds rocjitsu's KMD RPC server, models the platform work normally performed
by firmware and a kernel driver, and drives the regular AMDGPU model through
PCI configuration, BAR MMIO, GPUVM page tables, queues, doorbells, DMA, and
the interrupt ring.

The application side uses ROCr normally. rocjitsu's interposer forwards the
ordinary KFD and DRM operations to the embedded server. No semantic trace,
model-ABI client, packet reconstruction, memory snapshot, or synthetic
completion exists in this path.

The lab configuration defaults to eight CUs. `hsakmt_server.py` derives the
reported ROCr CU/SIMD/wave topology and VRAM/LDS sizes from its model options,
using one logical XCC. Use `--num-compute-units` for CU-count studies and inspect
`config.ini`; standard `--param` overrides are applied before instantiation for
controlled parameter studies. The current setup-memory path supports one GPU
memory interface: interleaved multi-interface setup cannot obtain its writable
backdoor. See the [eight-CU evaluation](../../../gpu-lab/EightCuEvaluation.md)
for measured workloads and model limitations.

## Architecture

rocjitsu owns KFD policy and its existing wire protocol. Its
`SimulatedKfd` validates ioctls and owns process, handle, allocation,
queue, event, topology, and wait state. The installed
`librocjitsu_kmd_server.so` exposes that implementation through a versioned C
ABI (version 2). The server-only library uses the same KFD and daemon code
as the simdojo backend without linking the simulator.

`RocjitsuKmdServer` integrates the nonblocking server with gem5's event queue
and composes the device-facing behavior:

- `HsakmtServerPlatform` configures the CPU-less GPU through its modeled PCI
  and BAR interfaces.
- `SharedRegionAllocator` assigns aligned extents from shared-backed VRAM.
- `GpuVmManager` owns the process page tables and exact allocation mappings.
- `QueueWorkEngine` programs compute and SDMA queues and its private PM4
  control ring.
- `DoorbellBridge` converts rocjitsu doorbell notifications into event-queue
  work.
- `Gem5DeviceBackend` converts the C callbacks into checked C++ handlers.

The service supports one attached process and one MI300X/gfx942 GPU. It
allocates application-visible memory from exported VRAM, installs queue
descriptors with the process VMID, writes modeled doorbells, and reports KFD
events after consuming real interrupt-ring entries. The service rejects a
second process and unsupported resource types rather than changing the GPU
model to accommodate them.

Generic GPU behavior needed by this route belongs to its normal subsystem.
The service must never add a rocjitsu-specific bypass, fake completion,
special requestor, or injected result under `src/dev/amdgpu/`,
`src/gpu-compute/`, or `src/dev/hsa/`.

## Build boundary

rocjitsu is built by CMake as C++20. gem5 and these service sources are built
by SCons as C++17. The only shared boundary is the installed
`rocjitsu/kmd/server.h` C header and `librocjitsu_kmd_server.so`.

Build and install the narrow server library into the local private prefix:

```sh
make -C src/dev/hsakmt-service install-kmd
```

Build and run the focused SCons tests:

```sh
make -C src/dev/hsakmt-service service-tests
```

Run rocjitsu's complete CTest suite, including its retained simdojo tests:

```sh
make -C src/dev/hsakmt-service rocjitsu-tests
```

Run both native suites and the script/configuration syntax checks:

```sh
make -C src/dev/hsakmt-service check
```

`SConscript` enables `RocjitsuKmdServer` only when SCons receives
`ROCJITSU_KMD_PREFIX`. This keeps ordinary gem5 builds free of an undeclared
rocjitsu dependency. The Makefile is orchestration only: SCons does not invoke
CMake, and rocjitsu does not invoke SCons.

## End-to-end verification

Build the image pair and run all four applications through the embedded KMD
server:

```sh
make -C src/dev/hsakmt-service verify-images
make -C src/dev/hsakmt-service verify-sample-workloads
```

The workload gate checks exact application results, modeled kernel counts,
nonzero generic AQL submission with every packet retired, clean KMD and
service teardown, and the rainbow BMP digest. It also rejects gem5 faults,
reactor errors, stale doorbells, timeouts, or pending resource state.

The config used by this gate is
`configs/example/gpufs/hsakmt_server.py`. It uses
`X86TimingSimpleCPU`, so the no-guest route does not require KVM.

## Memory contract

KFD allocations and USERPTR registrations receive bounded slices of the same
shared VRAM backing store used by gem5. Each descriptor includes its file
offset and extent, so allocations can share one FD without sharing storage.
The backend retains an allocation until rocjitsu releases its last lease;
closing a process first stops its queues and removes its GPU mappings.

USERPTR registration preserves the original CPU address and data even when
the GPU virtual address differs. ROCr supplies a page-aligned containing
range. Deregistration and connection teardown restore private caller-owned
storage before the server can reuse the backing. The application must keep
registered CPU ranges valid until deregistration.

The external backend rejects SVM attribute operations with `ENOTSUP` rather
than recording attributes without establishing device-accessible storage.
ROCr can fall back to ordinary GTT/USERPTR registration. This is not SVM
migration support. The service remains limited to one attached process and
one GPU; it does not provide cross-process IPC workloads or queue suspension.

Mapping policies are translated from the C API's semantic values into Vega
PTE encodings: UC, CC, and RW (including WB). Policy replacement updates the
existing mapping and invalidates translations. NT has no distinct supported
encoding and returns `ENOTSUP`. The unmodified Vega TLB uses the PTE's
cacheability bit, but does not interpret its MTYPE field; programming CC and
RW therefore does not establish distinct modeled cache behavior between them.

Checkpoints are cold only. Create one before the server publishes its listener
and restore it before attaching a fresh application. A connected client or
nonempty service resource set cannot be serialized.

The interactive architecture introduction is
[From a HIP kernel launch to gem5's GPU](../../../ROCJITSU-KFD-GEM5.html). It
covers GPU execution, the driver–GPU interface, KFD RPC message layouts, this
service's platform and callback work, and our upstream integration boundaries.
