# Bare-metal host and rendering reference

This stage runs one `RiscvO3CPU` in machine mode, with one unified host cache
and gem5 `NoncoherentXBar` interconnects. Configurable accelerator
subsystems are connected through MMIO adapters; the host writes their
doorbells to submit jobs. The flows run without an OS or syscall emulation.
The CPU creates the serialized Scene and Job Descriptors in system memory,
then calls the rendering consumer.
The consumer decodes the records into fixed local storage and renders through
a 64x64 RGBA tile buffer. Jobs larger than that buffer use multiple subtiles.

The fixed scene is a 96x96 Cornell box with red and green walls, two rotated
boxes, two diffuse spheres represented by triangle meshes, and two emissive
panels. It uses eight materials and 278 triangles. Spheres remain compatible
with the initial triangle-only descriptor format; normals are flat per face.

## Build and run

The firmware needs RV64 bare-metal GCC/binutils and picolibc headers and
libraries. `PICOLIBC` and `CROSS` can override the paths in the Makefile.
Picolibc supplies math routines; the firmware provides its own entry point,
BSS initialization, fixed stack, and linker script. There is no heap or dynamic
allocation. A linker assertion limits the entire firmware, including the host
scene, output buffer, reference scratch, and 16 KiB stack, to 256 KiB.

From the repository root:

```sh
make -C util/rt_reference
build/RISCV/gem5.opt --outdir=m5out/rt-smoke \
    configs/example/rt_accel.py --backend=gem5 --render-mode=1
build/RISCV/gem5.opt --outdir=m5out/rt-path \
    configs/example/rt_accel.py --backend=gem5 --render-mode=0 \
    --samples-per-pixel=4 --max-secondary-depth=4 --seed=1
```

The four render options also accept underscore spellings. Modes are 0 for
path tracing and 1 for the deterministic direct-light smoke test. Path tracing
defaults to four samples and depth 4; smoke defaults to one sample and depth 0.
Explicit incompatible smoke parameters fail rather than being overridden.
Limits are 16 materials, 512 objects, 4096 samples, and 32 secondary rays.
Seeds accept decimal or hexadecimal unsigned 64-bit values. Invalid options
fail before simulation starts. The configuration reports rendering progress
without imposing a simulation-time limit on valid rendering parameters.

Configuration parameters reach the guest through `m5_read_file`. The guest,
not the Python configuration, creates the descriptors and image. It exports
`scene.bin`, `job.bin`, `image.rgba`, and `image.bmp` with `m5_write_file`,
then terminates with `m5_exit`. Firmware errors use `m5_fail` and fail the run.
`parameters.bin` and `parameters.json` record the requested configuration.
The BMP contains the kernel's linear RGB values without gamma correction or
tone mapping, as required by the specification.

## Numerical and memory contract

Serialization uses the explicit little-endian offsets in
[SPEC/Accelerator.md](../../SPEC/Accelerator.md), including bus addresses and
the 56-byte Job Descriptor. Compile-time size assertions supplement the
explicit serializers. The consumer validates counts, exact scene size,
finite fields, material ranges, camera basis, nondegenerate triangles, mode,
depth, samples, tile bounds, alignment, stride, address overflow, and output
separation from descriptors. Bare-metal accesses must fit the 64 MiB RAM
window starting at `0x80000000`. Validation precedes output writes.

The statically reserved BVH uses median splits, at most four triangles per
leaf, and a fixed 16-entry traversal stack. It is derived local data and adds
no fields to the descriptor. Intersection uses two-sided Moller-Trumbore with
an absolute determinant cutoff of `1e-8`. Positive hits must exceed `1e-4`
world units; spawned origins move `1e-4` along the oriented normal. Exact
shared-edge distance ties choose the lowest object index. Bounds expand by
`1e-4`; direction components below `1e-20` are treated as parallel. Smoke
shadow rays aim at each emitter's centroid and stop `2e-4` before it, excluding
the destination surface. Shading distances and cosines use the actual hit.

Path sampling uses SplitMix64's mixer. Initial state is
`mix(seed XOR mix(y*image_width+x) XOR mix(sample_index+0x100000000))`.
Each draw adds `0x9e3779b97f4a7c15`, mixes, and maps the upper 24 bits to
`[0,1)` by dividing by `16777216`. Draw order is subpixel x, subpixel y,
then radius and azimuth for each cosine-weighted bounce. Primary depth is
zero, and the final permitted hit still contributes emission. The integrator
uses emitter hits, with no next-event estimation or Russian roulette, exactly
as specified. Sampling depends on global pixel coordinates, not tile order.
Floating-point contraction is disabled in both RISC-V and native builds.

## Validation

```sh
make -C util/rt_reference check out/native
python3 util/rt_reference/validate_backend.py --runs=2
```

The C checks cover serialization, invalid fields, analytic intersections,
back faces, misses, rendering modes, nonaligned host tiling, partial subtiles,
and preservation of row padding. A closed emitting room checks bounce
throughput and final-depth emission against an analytic energy series. To
compare BVH and linear traversal on 10,000 additional rays through the full
scene, pass any exported scene dump:

```sh
util/rt_reference/out/test \
    m5out/rt_backend/validated/q1-0/scene.bin
```

The Python validator in [validate_backend.py](validate_backend.py) runs real
RISC-V O3 simulations with the default eight cached accelerators: one small
traced protocol workload plus repeated direct-lighting smoke pairs on one and
on `accelerator-cores + 1` event queues. It checks the SimObject wiring,
transport timing, scene and Job Descriptor records against ELF symbols, raw
RGBA/BMP agreement, the golden smoke image hash, an independent
double-precision analytic smoke reference, and equal simulated counters
across queue modes. Repeated gem5 runs require identical image hashes.
Different math libraries can move rare paths across mesh edges; native
comparison requires mean byte error below 0.2 and fewer than 1% of bytes
differing by more than 1. `--inspect-only` rechecks completed runs without
launching simulations, and `--protocol-only` runs only the traced workload.
A passing run prints `PASS:` with the measured speedup and writes
`validation.json` under the output directory.

[RESULTS.md](RESULTS.md) and the `golden/` images are historical records of
the removed reference-backend validation matrix, retained for provenance.
Measured native-backend results and their machine-readable evidence live in
[RAYTRACE_RESULTS.md](RAYTRACE_RESULTS.md) and
[REDUCTION_RESULTS.md](REDUCTION_RESULTS.md).

## Configurable gem5 backend

The gem5 accelerator backend is the only `rt_accel.py` implementation; the
historical host-only `--backend=reference` mode and its mock SimObject were
removed. To render directly on the host while eight real gem5 accelerator
CPUs remain in WFI, build `out/host-render.elf` and select `--host-render`.
The extra host baseline in
[RAYTRACE_RESULTS.md](RAYTRACE_RESULTS.md) uses this hardware topology.
Its measured render and export intervals require zero accelerator
instructions, CPU cycles, DMA and TCM traffic, with WFI suspension and
absence of wakeups verified by sparse `Quiesce` logging:

```sh
make -C util/rt_reference out/host-render.elf
python3 util/rt_reference/measure_host_render.py \
    --output=m5out/rt_backend/l1-mc4-host-idle-8
```

`--accelerator-cores=N` selects 1-64 accelerator cores (default 8).
Each host MMIO window is 4 KiB at `0x10000000 + 0x1000*i`, without gaps.
The host reads the hardware count from `0x20000108` and partitions the
96x96 image into one nonoverlapping rectangle per detected core. Integer
boundaries cover every pixel for odd core counts too. Eight cores use the
original 2-column by 4-row grid of 48x24 tiles; sixteen cores use a 4x4 grid
of 24x24 tiles. All rays and shading execute on the accelerator cores.
The default `--accelerator-cpu=timing` uses `RiscvTimingSimpleCPU`; select
`--accelerator-cpu=o3` to use `RiscvO3CPU`. Each core has one hart and executes
the same scalar RV64IMAFDC resident ELF. Both models fetch through L1I from
Code TCM and route L1D misses through the local bus to constant data,
Data TCM, control registers or host RAM. SMT, RVV, and the custom VPU remain
future work. The host remains `RiscvO3CPU`.
Rendering defaults to four independent `SimpleMemory` RAM controllers,
interleaved at 64-byte boundaries. Each retains the rendering example's
30 ns latency and 12.8 GiB/s bandwidth. Select `--memory-controllers=1` or
`--memory-controllers=2` for fewer controllers.

```sh
make -C util/rt_reference all check backend-tests out/native
build/RISCV/gem5.opt --outdir=m5out/rt_backend/serial \
    configs/example/rt_accel.py --backend=gem5 --queues=1 \
    --render-mode=1 --samples-per-pixel=1 --max-secondary-depth=0 --seed=1 \
    --trace
build/RISCV/gem5.opt --outdir=m5out/rt_backend/parallel \
    configs/example/rt_accel.py --backend=gem5 --queues=9 \
    --render-mode=1 --samples-per-pixel=1 --max-secondary-depth=0 --seed=1 \
    --trace
python3 util/rt_reference/validate_backend.py --runs=4 \
    --require-idle-machine
python3 util/rt_reference/validate_backend.py --accelerator-cpu=o3 --runs=2 \
    --require-idle-machine --output=m5out/rt_backend/o3
```

Select `--queues=1` for a shared queue or `--queues=N+1` for the host on
queue 0 and accelerator systems on queues 1 through N. The count is
discovered through MMIO by the same firmware binary in either mode.

Both queue modes use the same system, binaries, 1 GHz clocks, memory timings,
and two real transport stages: a request link and a response link, each
5 ns (five CPU/device cycles) by default. `--transport-delay` changes both
link stages in both modes and sets the synchronization quantum to the same
delay, the largest safe value for these links. These stages precede local
register sampling and follow local responses respectively; they add to
crossbar, TCM and system-memory
service latencies. Both modes set the same `Root.sim_quantum`, also preserving
gem5's quantum-delayed global exit behavior in serial mode. No delivery is
rounded to a quantum boundary. Equal-time transport
arrivals on the host arbitrate by accelerator index, using event priorities
following the synchronization event. The same priorities apply in serial
mode. Crossbar contention remains on the owning queue.

Ray-tracing measurements in [RAYTRACE_RESULTS.md](RAYTRACE_RESULTS.md) use
the optimized simulator, fresh 5 ns DES baselines for eight and sixteen
accelerators, and PDES transport/quantum sweeps at 1, 5, 10, 20, 50, 100
and 1000 ns. The table also includes the host renderer with eight idle
accelerators. Run `measure_raytrace.py --output=<fresh-directory>` to capture
exactly one attempt per configuration; add `--inspect-only` to recheck a
completed sweep without simulations.
Historical full reduction measurements used 100 ns links and quantum. Their
timing and speedup results must be remeasured for the current 5 ns defaults.
[REDUCTION_RESULTS.md](REDUCTION_RESULTS.md) provides the current quick
8/16-core comparison.

Each accelerator is an isolated `RiscvSystem` containing its wrapper, scalar
core, private L1I/L1D, local `CoherentXBar`, control/DMA device and two TCMs
totaling 256 KiB. Code TCM occupies `0x40000000–0x4000ffff` (64 KiB); Data TCM
occupies `0x40010000–0x4003ffff` (192 KiB). The CPU instruction port connects
through L1I to Code TCM. Its second port serves constant reads through L1D
and the local crossbar. Both ports use one backing store, so functional ELF
loading through the crossbar also initializes the instruction path. Writes
and atomics reaching Code TCM return an error; functional initialization
writes are allowed. Data TCM holds initialized writable data, BSS, descriptor copies,
rendering scratch, the 64x64 RGBA buffer and the 16 KiB stack. The linker
checks each TCM's capacity separately, including alignment and stack space.

`RTAccelCodeTCM` reuses `AbstractMemory` for storage, loading, statistics and
checkpoints, and `SimpleTimingPort` for pipelined reads, response backpressure
and draining. Each read port has 1 ns response latency and accepts outstanding
requests independently. Data TCM reuses `SimpleMemory`, with 1 ns latency and
128 GiB/s aggregate read/write bandwidth. The 64-byte-wide data crossbar runs
at 4 GHz with zero added frontend/forward/response stages. gem5 still charges
router arbitration and payload serialization: one header slot per 250 ps,
plus payload transfer for writes/responses. This allows several data requests
per 1 GHz CPU cycle and keeps instruction fetch independent of data/DMA
contention. These are explicit idealized TCM/router timings, identical in
serial and parallel mode; the CPU's execution resources still limit IPC.
Each accelerator defaults to an 8 KiB / 2-way L1I and 16 KiB / 4-way L1D,
with 64-byte lines and parallel one-cycle tag/data lookup. L1I has four MSHRs
and 16 targets per MSHR; L1D has 16 MSHRs, 32 targets and 16 write buffers.
The caches run at 1 GHz, on their accelerator's event queue. No prefetcher is
installed. `--accelerator-caches=none` restores the cacheless local path for
comparisons. The host retains its separate unified 64 KiB cache.

DMA reads snoop dirty L1D lines. An independent coherence port sends a
`CleanInvalidReq` before each Data TCM write, preserving dirty neighboring
bytes and invalidating stale copies. The DMA chunk retains its credit until
the actual destination write responds. Coherence requests retry at router
timing, with at most one accepted request per nanosecond; data read/write
ports retain their device-clock issue limits. Host RAM and MMIO stay uncached
in the accelerator. Firmware must not access an active DMA destination or
modify the immutable code/constant image.

The wrapper's MMIO request port and system-memory response port both connect
to the local crossbar. CPU data accesses and all DMA traverse this bus; the
CPU can also reach host RAM through the wrapper. DMA permits only Data TCM
and host RAM, rejecting Code TCM in either direction, cross-aperture ranges,
zero lengths and address overflow before issuing a packet. Each accelerator
sees local registers at `0x10000000`; the wrapper maps its host MMIO aperture
to this address. DMA copies use serialized read/write pairs of at most
64 bytes, split at source and destination cache-line boundaries. Remote
reads are followed by Data TCM writes; output rows use Data TCM reads followed
by remote writes. There are no direct TCM pointer accesses by the device,
scene fetches during ray traversal, output reads, or intermediate
system-memory spills. Only the queue-0 host adapter accesses the host bus.
The firmware uses `fence iorw, iorw` to order buffer/parameter writes before
DMA start, start before status polling, completed DMA before CPU buffer
reads, and output completion before MSI. Volatile MMIO alone cannot provide
these ordering guarantees on O3 with the faster local memory path.

Host descriptors and pixels occupy an uncached PMA range starting at
`0x81000000`, with no cached aliases. This also covers lines shared by adjacent
tiles. The host fences descriptor creation before submission. A local machine
external interrupt wakes the resident accelerator from `WFI`; it acknowledges
that level before loading the job. The DMA engine acknowledges every output
write before the completion command can issue its MSI. The host MSI target
has one word per accelerator at `0x20000000 + 4*i`, a low 32-bit pending
bitmap at offset `0x100`, write-one acknowledgement at `0x104`, and a
read-only core-count register at `0x108`. Systems with more than 32 cores
add a high pending bitmap at `0x10c` and its write-one acknowledgement at
`0x110` for cores 32-63. The low bank and discovery offsets are unchanged;
all 64 MSI words fit below `0x100`. Rebuild the host firmware together with
gem5. Locally enabled, globally masked interrupts
wake `WFI`; firmware handles levels through these registers
without asynchronous simulator callbacks. The host accumulates and
acknowledges notifications, waits for all detected cores, then checks every
terminal status before accessing image bytes or exporting descriptors and
images.

The accelerator register map is:

| Offset | Width | Register |
| ---: | ---: | --- |
| 0 | 64 | Host doorbell / accepted job address |
| 8 | 64 | MSI destination |
| 16 | 32 | MSI payload |
| 20 | 32 | Idle/busy/done/error status (0/1/2/3) |
| 24 | 32 | Local submission interrupt acknowledgement (write 1) |
| 32 | 64 | Accepted job address for resident firmware |
| 40 | 32 | Complete job (write 0 for success, nonzero for error) |
| 44 | 32 | Completion MSI still outstanding |
| 48 | 64 | DMA source |
| 56 | 64 | DMA destination |
| 64 | 32 | DMA byte length |
| 68 | 32 | Start DMA (write 1) |
| 72 | 32 | DMA idle/busy/done/error status (0/1/2/3) |

Doorbells and MSI configuration writes while busy return an MMIO error. The
transport preserves the same packet and payload throughout each request and
response. Host MMIO uses bounded adapter credits; native CPU/DMA requests use
bounded wrapper credits, returned only after the local response is accepted.
Both directions preserve timing-port request and response retries. Atomic and
functional cross-queue accesses and checkpointing are outside this PoC.

Gem5 output directories include `core_count.bin` (the detected LE u32 count),
parameters, `scene.bin`, one `job<i>.bin` per detected core, `job.bin` for the
whole-image producer template, raw RGBA, BMP, statistics, and optional
complete transaction traces. The backend validator adds linear `image.png`,
sRGB `preview.png` and `validation.json`. It requires the published smoke
RGBA hash, independent
lighting checks, byte-identical exports, identical simulated counters, and
identical request/response timing, payloads, ordering and MSI visibility in
all `2*N+1` trace files. By default the first pair records traces; subsequent
runs omit trace I/O and alternate queue-mode order. Every run is sequential.
The reported speedup divides median single-queue process wall time by median
parallel process wall time, excluding traced correctness runs. The target
is greater than 2x; measurements and profiling are recorded separately.
The host resets statistics with `m5_reset_stats(0, 0)` after preparing
the scene and tile descriptors, immediately before batch submission. gem5
defers statistics events by one quantum; firmware reads that quantum in
nanoseconds through `m5_init_param(0, 0)` and quiesces past the reset before
ringing any doorbell. After all completions and status checks it calls
`m5_dump_reset_stats(0, 0)` and waits past that event before checking or
exporting output. Statistics callbacks execute on queue 0 between two
global barriers, keeping Python access on the interpreter thread.

The first statistics block measures the submitted batch, including
descriptor/scene DMA, accelerator setup, rendering, output DMA and MSI.
Boot and host scene/descriptor preparation are excluded. The elapsed
region includes the short synchronization gaps at its boundaries; it is
not a per-core render-kernel interval. The last block covers host export.
The validator uses the first block for IPC and compares every block
between queue modes. Process throughput uses the complete simulation
timeline rather than the reset statistics interval.
See [RAYTRACE_RESULTS.md](RAYTRACE_RESULTS.md) for region IPC, saved machine,
commands and correctness evidence for the current four-controller, cached
O3 system with eight and sixteen accelerators. The report contains one
untraced 96x96 smoke-scene timing sample per core-count/queue combination,
plus one host-renderer sample with eight idle accelerators. These single
samples do not establish wall-time variance. Six additional eight-core,
nine-queue rows record the transport/quantum sweep. The 1000 ns row uses
the successful retry; the earlier crash is retained in the evidence. Their
speedups use the existing 5 ns serial wall time; serial runs at the larger
delays are not part of that sweep. Run only those six samples with:

```sh
python3 util/rt_reference/measure_transport.py \
    --baseline=m5out/rt_backend/raytrace-5ns-20261011/accel8 \
    --output=m5out/rt_backend/raytrace-transport-new \
    --quantums 10 20 50 100 200 1000
```

The sweep checks the executable, firmware and source against the baseline,
rejects competing gem5 processes, validates each image and checks the
instantiated quantum and both link delays. `--inspect-only` rechecks saved
results without running simulations. Pass the completed directory as
`--transport-sweep` to `report_raytrace.py` to include it in the same table.
Pass `--transport-retry` with the separate completed retry directory to use
its validated 1000 ns sample while preserving the original failed attempt.

`--require-idle-machine` rejects other running gem5 processes before and
during runs, and records those checks with the wall times. Run this command
in the host PID namespace, where `ps` can see all simulations. Four repeats
provide one traced correctness pair and three untraced timing pairs. Use
`--runs=2` for one traced correctness pair and one untraced timing pair. Use
`--runs=1` without the idle-machine flag for correctness checks alone.
For exactly one untraced full-scene run per queue mode, use:

```sh
python3 util/rt_reference/validate_backend.py --accelerator-cores=16 \
    --accelerator-cpu=o3 --runs=1 --timing-only --require-idle-machine \
    --output=m5out/rt_backend/l1-mc4-o3-16
```

`--timing-only` still checks the golden image and all simulated counters.
The small, traced protocol pair checks discovery, completion and error paths
before these timed runs. Its firmware also checks Code TCM constant reads,
Data TCM initialization/BSS/writes, rejected DMA code reads/writes, crossing
TCM boundaries and address overflow. Configuration checks assert the L1
connections, coherent DMA path, data routing, TCM ranges/timings, controller
interleaving and queue ownership.
`--protocol-only` runs only that focused workload.
`--reuse-correctness` extends a completed run with new timing pairs after
checking the executable hash, all four firmware hashes, accelerator CPU model,
quantum and link delay. It rechecks the saved correctness evidence without
rerunning that pair. Timing evidence includes elapsed simulated clock periods
and simulated clocks per wall second, including idle/WFI time.
Host-memory statistics attribute remote requests to the issuing adapter;
the original accelerator requestor ID is restored before local completion.

`profile_backend.py` collects 100 Hz gperftools CPU profiles in both queue
modes and rechecks every simulated counter and transaction trace against a
completed traced reference directory. Supply the gperftools Perl `pprof`
reader and a separate output directory, for example:

```sh
python3 util/rt_reference/profile_backend.py \
    --pprof=/tmp/rt-pprof --reference=m5out/rt_backend/validated/q1-0 \
    --output=m5out/rt_backend/profiles/current
```

These are traced diagnostic runs; their wall times are excluded from the
benchmark. Function-level reports retain inline costs in their caller.

The capability-cache optimization has focused positive and negative guest
checks. The enabled case reads the `mnstatus` reset value and sets NMIE;
the disabled case requires an illegal-instruction trap on `mnstatus` access:

```sh
make -C util/rt_reference extension-tests
build/RISCV/gem5.opt --outdir=m5out/rt_backend/extensions/present \
    util/rt_reference/check_smrnmi.py --enabled
build/RISCV/gem5.opt --outdir=m5out/rt_backend/extensions/absent \
    util/rt_reference/check_smrnmi.py
```

The short protocol workload uses separate test ELFs and one transport credit.
It renders one pixel per detected core, submits an unknown rendering mode
and an excessive scene count, checks that failed jobs leave the output buffer
unchanged, then resubmits valid work. Accelerator CPU reads of host descriptors
contend with DMA and exercise wrapper rejection/retry; immutable host reads
exercise queue-0 CPU/DMA contention. It checks four MSI writes per core and
compares every deterministic statistic and trace between queue modes before
the full-image workload runs. Build these ELFs with `make backend-tests`.

## Memory-bound reduction

The same accelerator hardware also runs a 16-core binary32 sum reduction with
Python-initialized 16 MiB input, 100 system-clock-cycle RAM latency and
pipelined DMA. See [REDUCTION.md](REDUCTION.md) for build/run instructions,
shared workload infrastructure, throughput and fault tests, reproducible
one/seventeen-queue timing comparisons, and measured results.

Reduction configuration defaults to four independent RAM controllers with
64-byte interleaving and the configured bandwidth per controller. Select
`--memory-controllers=1` or `--memory-controllers=2` to override it.
[MEMORY_CONTROLLERS.md](MEMORY_CONTROLLERS.md) compares one and two controllers
with the same workload, latency and per-controller bandwidth.
