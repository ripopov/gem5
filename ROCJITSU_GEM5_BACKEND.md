# gem5-hosted rocjitsu KMD server

This document records the implemented HSAKMT architecture for the GPU lab.
gem5 now hosts rocjitsu's KMD RPC server and supplies its device behavior
through a narrow C callback ABI. The previous ROCr model-ABI client, semantic
trace protocol, memory snapshots, reconstructed dispatches, and injected
completions have been removed.

## 1. Architecture and ownership

The application follows the ordinary ROCr-to-KFD boundary. rocjitsu's
`LD_PRELOAD` interposer catches `/dev/kfd` and DRM operations, and its
`RemoteDriver` sends the existing KMD RPC over a Unix-domain socket. The
server is embedded in `gem5.opt`; there is no separate daemon and no second
device protocol.

```text
SDK container                              runner container

HIP application
  │
ROCr and libhsakmt
  │
rocjitsu interposer
  │
RemoteDriver ───── KMD RPC + SCM_RIGHTS ──► KmdRpcServer
                                             │
                                      SimulatedDriver
                                             │
                                      Gem5DeviceBackend
                                             │ C callbacks
                                             ▼
                                      RocjitsuKmdServer
                                             │
                                platform, GPUVM, queues, GPU
```

`SimulatedDriver` remains responsible for KFD policy: ioctl validation,
process and handle identity, allocations, queues, events, topology, wait
semantics, and RPC lifetime. `RocjitsuKmdServer` owns the behavior that a
CPU-less platform and KMD would normally perform: physical extent allocation,
GPU page-table programming, queue installation, doorbell writes, interrupt
ring consumption, and resource teardown. The AMDGPU and GPU-compute models
remain unaware of this service.

The build and packaging boundary follows those responsibilities:

| Owner | Content |
| --- | --- |
| `ext/hsakmt-client/rocm-systems/emulation/rocjitsu/` | interposer, KMD RPC, `SimulatedDriver`, device-backend interface, reusable server, and C ABI |
| `src/dev/hsakmt-service/` | gem5 SimObjects, callback adapter, platform setup, allocation, GPUVM, queue, doorbell, and lifecycle state |
| SDK image | HIP, the pinned ROCr build, rocjitsu CLI, and `librocjitsu.so` |
| runner image | `gem5.opt`, `librocjitsu_kmd_server.so`, configs, and launcher |

rocjitsu is a standalone CMake project compiled as C++20. gem5 is built by
SCons as C++17 and links only the installed `rocjitsu/kmd/server.h` API and
`librocjitsu_kmd_server.so`. No C++ object, exception, standard-library
container, gem5 header, or SimObject crosses that boundary. The KMD library
exports only its versioned `extern "C"` surface and does not contain the
interposer or simdojo ISA simulator.

## 2. rocjitsu implementation

The rocjitsu refactor introduces a device-independent KFD core without
changing the established application-side RPC.

`DeviceBackend` describes process setup, allocation, mapping, apertures,
queues, doorbells, memory writes, scratch, and interrupt delivery.
`SimdojoDeviceBackend` preserves rocjitsu's existing functional-simulator
behavior. `Gem5DeviceBackend` translates the same operations into the C
callback table supplied by gem5. `SimulatedDriver` therefore owns one KFD
implementation while the two backends provide different devices.

`KmdRpcServer` is the reusable, nonblocking listener and connection reactor
extracted from the rocjitsu CLI. It supports partial input and output,
descriptor passing, orderly EOF, client teardown, and doorbell notification
without blocking gem5's event queue. The CLI uses it for ordinary `--daemon`
mode; the shared library exposes the same core to gem5.

The public C ABI provides:

- server construction, listening, poll-FD enumeration, and FD processing;
- process, allocation, mapping, aperture, queue, and write callbacks;
- backing slices containing allocation identity, device physical address,
  FD, FD offset, and bounded length;
- notification-FD and doorbell-record draining;
- interrupt posting and server state counters; and
- a diagnostic callback with errors converted to stable status values.

Every public structure carries an ABI version and structure size. Tests build
a C consumer and a C++17 consumer, inspect the exported-symbol allowlist, and
exercise invalid arguments and lifecycle behavior.

The KMD RPC is version 5. Its memory response preserves both the descriptor
and a nonzero backing offset, so multiple allocations can safely be slices of
one gem5-owned shared backing store. `RemoteDriver` validates the FD extent
before mapping it and applies the offset exactly once.

`rocjitsu --attach -- <application>` connects to the endpoint created by
gem5. Attach mode no longer needs a simdojo configuration file because device
identity and topology come from the embedded server. A configurable runtime
root places the socket and generated KFD/DRM trees in a directory visible to
both containers.

## 3. gem5 implementation

`RocjitsuKmdServer` integrates the reusable server with gem5's event loop.
gem5 `PollEvent` objects service the listener, client sockets, and rocjitsu's
notification eventfd. All callbacks that touch SimObjects execute on the
event-queue thread; the doorbell watcher only produces plain records and
signals the eventfd.

The SimObject composes small service-owned components:

| Component | Responsibility |
| --- | --- |
| `HsakmtServerPlatform` | PCI configuration and BAR programming for the CPU-less GPU platform |
| `SharedRegionAllocator` | aligned, non-overlapping extents in exported VRAM |
| `GpuVmManager` | service-owned page tables and exact allocation mappings |
| `QueueWorkEngine` | compute and SDMA MQDs plus direct PM4 setup |
| `DoorbellBridge` | watcher lifecycle and queued doorbell records |
| `Gem5DeviceBackend` | checked conversion from the C ABI to C++ handlers |
| `ServiceStats` | page-table, mapping, and queue resource accounting |

The service supports one attached process and one MI300X/gfx942 device. It
assigns one PASID/VMID, initializes the GPU through PCI and BAR interfaces,
programs page tables and queue descriptors, writes real doorbells, consumes
the modeled interrupt ring, and posts rocjitsu events only after the GPU
produces the corresponding interrupt.

The old semantic path is absent. The service does not receive code objects,
kernargs, packet descriptions, buffer snapshots, expected output, or
completion recipes. AQL and PM4 packets are fetched from application-visible
memory by the normal GPU pipeline.

## 4. Shared memory and visibility

AMDGPU device memory uses gem5's shared `PhysicalMemory` backing-store
mechanism. The service allocator owns a configured VRAM extent and returns a
duplicated FD plus the exact file offset and length for each allocation. The
application, rocjitsu server, DMA engines, and GPU consequently observe the
same bytes instead of synchronizing snapshots.

The current memory contracts are:

| Request | Implemented behavior |
| --- | --- |
| VRAM or GTT KFD allocation | allocate an exported VRAM extent and map it into GPUVM |
| USERPTR allocation | promote the containing client pages to the exported backing, preserving the requested page offset and initial bytes |
| SVM `ACCESS_IN_PLACE` | register a page-aligned exported extent, replace the client range with its shared mapping, and map it into GPUVM |
| SVM access, preferred-location, prefetch, or advice attributes | retain/query KFD metadata; they do not migrate or copy storage |
| DMA-BUF and IPC handles | unsupported |

GTT and USERPTR flags describe the client-visible mapping contract; the
gem5-side physical extent remains device-local VRAM. This is deliberate:
translated GPU traffic must traverse the modeled device-memory path, while
the shared descriptor gives the remote CPU process visibility into the same
storage.

Ruby requests used by the direct GPU path are also applied to the configured
device `AbstractMemory`, so the shared backing reflects completed modeled
writes. The Ruby network constructs its topology after controllers are fully
registered, ensuring the final controller-to-node map is used. These are
generic backing-store correctness changes rather than service conditionals.

The generic GPU model also implements the hardware behavior required by the
unmodified runtime:

- AQL acquire and release scopes are retained through dispatch and enforced
  at activation and completion.
- AMD vendor AQL packets containing PM4 indirect buffers execute through the
  normal PM4 processor and retire from its completion callback.
- PM4 `ACQUIRE_MEM` performs the modeled synchronization operation.
- PM4 and SDMA address translation carries the submitting VMID.
- device-physical DMA translation can use GPU page tables without a CPU
  process page table.
- release, completion-signal update, and interrupt visibility occur in
  end-of-pipe order.
- a coalesced HWScheduler doorbell retains the queue read index and advances
  the write index monotonically.

None of these paths tests for or names the rocjitsu backend. They remain valid
for a realistic GPUFS platform.

## 5. Queue, event, and lifecycle behavior

rocjitsu creates compute and SDMA queues using its normal KFD ioctls. The
backend validates that the ring and read/write pointers are mapped, installs
the appropriate MQD, and associates the queue with the process VMID. The
application writes a shared doorbell page. rocjitsu's watcher records the
process, queue, offset, and value; gem5 drains those records and performs the
modeled BAR doorbell write.

Queue completion follows hardware evidence. Compute packets retire through
`HSAPacketProcessor`; SDMA and PM4 work completes through their modeled
engines; completion signals become visible before interrupt delivery.
`RocjitsuKmdServer` consumes the interrupt ring and uses the process/event
identity to wake the waiting KFD event.

Clean and abrupt disconnect close queues, mappings, allocations, events, and
process state. A normal workload exit is accepted only when the rocjitsu
server and all service-owned resource maps are empty. The runner logs
`rocjitsu KMD state drained` only for that state and exits gem5 after the
single supported client disconnects.

Live external state is not serializable. A checkpoint is permitted only
before the listener is published and before any client or service resource
exists. Restoring such a cold checkpoint and attaching a fresh application is
supported; checkpointing a connected process is rejected.

## 6. Deployment

The lab uses two containers in one Linux kernel. A named volume mounted at
`/run/rocjitsu` carries `daemon.sock` and rocjitsu's generated topology and
DRM trees. `SCM_RIGHTS` transfers the shared-memory descriptors over that
socket. Both services use `network_mode: none`; IP networking is not part of
the design.

This works on native Linux and on Docker Desktop because both containers run
inside the same Linux VM. A native macOS gem5 process cannot exchange a Unix
descriptor with a Linux container and is unsupported.

The SDK command is:

```sh
rocjitsu-run --attach /workspace/apps/hip-vector-add
```

The runner starts `configs/example/gpufs/hsakmt_server.py` with an
`X86TimingSimpleCPU`, publishes the endpoint, and waits for the application.
The launcher verifies that the two images use the same platform and were
built from the same rocjitsu revision before starting them.

Build the image pair and run the three-workload gate from the lab workspace:

```sh
cd gpu-lab/workspace
./scripts/build-hsakmt-images.sh
./scripts/run-all-samples.sh --backend gem5-kmd
```

Run one already-built sample with:

```sh
./scripts/run-sample.sh --backend gem5-kmd hip-vector-add
```

Each run writes application output, gem5 output, configuration, statistics,
exit status, and the rainbow BMP, when applicable, under
`work/runs/<run-id>/`.

## 7. Build and verification ownership

The service Makefile orchestrates the two native build systems without
combining them:

```sh
make -C src/dev/hsakmt-service install-kmd
make -C src/dev/hsakmt-service service-tests
make -C src/dev/hsakmt-service rocjitsu-tests
make -C src/dev/hsakmt-service check
make -C src/dev/hsakmt-service verify-sample-workloads
```

`install-kmd` builds only the embeddable KMD server into a private prefix.
SCons receives that prefix as `ROCJITSU_KMD_PREFIX`; ordinary gem5 builds
without it do not gain an external rocjitsu link dependency. `rocjitsu-tests`
builds and runs rocjitsu's full CTest suite, including its original simulator
tests. `service-tests` builds the focused SCons tests for statistics,
allocation, doorbells, GPUVM, queues, and the callback adapter.

The end-to-end verifier requires all of the following:

- zero application and gem5 exit statuses and no timeout;
- exact application checksum or artifact output;
- the expected generic GPU kernel-launch count;
- nonzero AQL submissions with every submitted packet retired;
- a valid rainbow BMP with its reference SHA-256;
- clean rocjitsu and service resource teardown; and
- no fatal, panic, stale-doorbell, reactor, or RPC failure.

The established results are vector-add checksum `6048`, kernel-chain checksum
`40768`, and rainbow checksum `0x941d233a32bf8839`. The workloads compile for
gfx942 and execute their ISA through gem5's timing GPU model; rocjitsu supplies
KFD behavior, not functional kernel execution.

## 8. Resulting source layout

The production gem5-side implementation is intentionally small:

```text
src/dev/hsakmt-service/
  HsakmtService.py
  SConscript
  Makefile
  service/
    hsakmt_server_platform.*
    shared_region_allocator.*
    gpu_vm_manager.*
    queue_work_engine.*
    doorbell_bridge.*
    gem5_device_backend.*
    rocjitsu_kmd_server.*
    service_stats.*
  tests/
```

The former service-local trace protocol, RPC endpoint, resource registry,
work coordinator, completion engine, response builder, semantic fixtures,
trace tools, and trace launchers were deleted. The model-ABI library,
portable semantic protocol, synthetic topology, and client-only tools were
deleted from `ext/hsakmt-client/`; that directory now owns only the pinned
`rocm-systems` source and its documentation.

The active rocjitsu changes live in the `rocm-systems` submodule and therefore
require their own submodule commit. The gem5 repository records the resulting
submodule revision separately from its service, config, lab, and Docker
changes.

## 9. Deliberate limitations

The implemented lab contract is one client, one process identity, one
MI300X/gfx942 GPU, one Linux-kernel deployment, and cold checkpoints.
Multi-process, multi-GPU, PASID/VMID multiplexing, DMA-BUF, IPC memory,
SVM migration, advice-driven placement, and live reconnectable checkpoints
remain separate projects. Unsupported operations return an error; they do
not fall back to snapshots, semantic replay, simdojo, or service-specific GPU
model behavior.
