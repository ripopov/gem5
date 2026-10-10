# Floating-point reduction and pipelined DMA

This experiment executes a resident scalar RISC-V reduction kernel on **16
accelerators by default**; `--accelerator-cores` selects 1-16. Each has one
`RiscvO3CPU` hart, 64 KiB Code TCM and 192 KiB
Data TCM, an 8 KiB / 2-way L1I and a 16 KiB / 4-way L1D. The host is another
`RiscvO3CPU`. RVV is not used. One MiB is 2^20 bytes; every input is an IEEE
754 binary32 float. `--accelerator-caches=none` selects the original
cacheless integration for comparisons.

`configs/example/rt_accel_reduction.py` creates an ELF memory image in Python:
16 MiB of consecutive little-endian `1.0f` values at `0x82000000`. The memory
image's segments are appended to the host boot ELF, retaining the firmware's
entry and symbols. `RiscvBareMetal` loads the combined image through the system
physical proxy during initialization, before simulated CPU execution. Host
firmware discovers the accelerator count and creates one descriptor per core,
assigning accelerator i the region
`[0x82000000 + i*2^20, 0x82000000 + (i+1)*2^20)`. It does not initialize or
scan the input. Descriptors and partial sums occupy uncached host RAM at
`0x81000000`; there are no cached aliases of DMA outputs.

Each kernel uses two statically allocated 64 KiB tiles (128 KiB total) in Data
TCM. It waits for the first DMA, starts fetching the next tile into the other
buffer, and sums the current tile using eight independent scalar binary32
accumulators. It processes 16 tiles per accelerator, then DMA-writes one
binary32 partial sum and issues an MSI after that write completes. The host
waits for every MSI, checks every terminal status and every partial against
`262144.0f`, then adds the partials and checks `cores * 262144.0f`:
`2097152.0f` for eight cores or `4194304.0f` for sixteen. These integers are
exactly representable in binary32, including the intermediate sums here.

The reduction descriptor is 24 little-endian bytes: `u64 input`, `u64 output`,
`u32 bytes`, `u32 mode`. Mode 0 is reduction, 1 is the copy throughput probe,
2 is the patterned DMA protocol test, and 3 is the DMA fault test. The 64-byte
host descriptor stride prevents cache-line sharing. The kernel rejects
unsupported modes/sizes and addresses outside its RAM contract.

## Timing configuration

CPUs, DMA engines and shared RAM use the **1 GHz host/system clock domain**:
one cycle is 1 ns, or 1,000 gem5 ticks with the configured 1 THz tick frequency.
Shared `SimpleMemory.latency` is **100 ns**, explicitly **100 host/system
cycles**, with zero latency variation. `--memory-cycles` converts cycles in
this fixed domain to nanoseconds. This is memory service latency; crossbars,
transport and destination writes add further delay.

Both the shared and local 64-byte crossbars use separate **4 GHz router
clocks**. The 100-cycle RAM setting is independent of these router clocks.
Data TCM has 1 ns service latency and 128 GiB/s nominal bandwidth. Each shared
RAM controller has 32 GiB/s nominal bandwidth for reduction and 64 GiB/s for
the separate single-core DMA probe. `SimpleMemory` rounds bandwidth to integer
ticks per byte: shared RAM uses 29 ticks/byte (1,856 ticks per 64-byte packet)
for reduction, and 15 ticks/byte (960 ticks per packet) for the probe. Data TCM
uses 7 ticks/byte. The saved `config.json` records these actual values.

The original cacheless TCM load path consumes about one float per core cycle;
16 such cores cannot saturate 64 GiB/s shared RAM. The baseline validation
harness explicitly uses one controller at 32 GiB/s so shared-memory traffic
limits completion. Direct runs default to four controllers at 32 GiB/s each.
The single-core copy probe provisions 64 GiB/s per controller to test the DMA
target with sufficient bandwidth.

Each transport direction takes five CPU/device cycles (5 ns). The
synchronization quantum equals the per-direction `--transport-delay`, the
largest safe value for these links: 5 ns by default. Wrapper credits and DMA
chunk credits are both 512.
A host read round trip includes roughly
110 accelerator cycles of transport plus RAM service, so these credits hide
that latency during a tile stream. Both queue modes retain the same clocks,
credits, arbitration priorities, links, memory path and resident firmware.

Historical cache and memory-controller measurements below used 100 ns
transport delays and quantum. The current quick 8/16-core DES versus PDES
comparison uses 5 ns; see [REDUCTION_RESULTS.md](REDUCTION_RESULTS.md).

`--queues=1` puts everything on queue 0. `--queues=17` keeps the host, cache,
shared RAM, crossbar, MSI target and host adapters on queue 0, and puts each
complete accelerator subsystem on its own queue 1 through 16.

`--memory-controllers=2` replaces the single memory with two independent
`SimpleMemory` controllers on queue 0, interleaved at 64-byte cache-line
boundaries using address bit 6. `--memory-controllers=4` uses four controllers
and address bits 6 and 7. Each has the full `--memory-bandwidth` setting and
`--memory-cycles` latency; total RAM capacity stays 64 MiB. The default is four
controllers. These fixed-latency models do not model DRAM row/bank timing.
The physical-proxy boot loader initializes the interleaved backing memory.
See [MEMORY_CONTROLLERS.md](MEMORY_CONTROLLERS.md) for the measured one/two
controller comparison, including an equal nominal aggregate bandwidth control.

A historical cacheless four-controller run on 17 queues, at 32 GiB/s per
controller and 100-cycle latency, completed the batch in **306,403 cycles**, or
**54.755 input B/cycle**. That is **1.025x** the two-controller modeled speed,
consistent with the scalar/TCM path becoming the limit. Simulation wall time
was **5.261 s** (whole process **7.384 s**); this single sample should not be
treated as a wall-time trend against the earlier two-controller median of
4.894 s. Every controller served exactly 4 MiB of input plus 96 descriptor
bytes, and all inputs, descriptors and reduction outputs matched the earlier
two-controller run byte for byte. The exact command, source hashes, traffic
and timing evidence are in
[MEMORY_CONTROLLERS_4.json](MEMORY_CONTROLLERS_4.json).

## Historical cacheless four-controller bottleneck isolation

The historical cacheless four-controller configuration is primarily limited
by load backpressure at the **O3 CPU to local crossbar interface**.
A diagnostic zero-delay queued `Bridge` between the CPU data port and its
existing local crossbar absorbs CPU bursts and retries at router timing.
With all other resources unchanged, this reduces batch completion from
306,403 to 160,054 cycles (~1.914x speedup).
This is a diagnostic intervention, not a change to the default architecture.

## Private L1 cache measurement

The defaults now include **8 KiB L1I / 2-way** and **16 KiB L1D / 4-way**
per accelerator. Both have 64-byte lines, parallel one-cycle tag/data lookup
and one-cycle response latency at 1 GHz. L1I has four MSHRs and 16 targets
per MSHR; L1D has 16 MSHRs, 32 targets per MSHR and 16 write buffers.
There is no prefetcher. L1D snoops and destination clean/invalidate requests
make DMA coherent with cached Data TCM; host RAM and MMIO remain uncached in
the accelerator CPU. The local fabric is a native `CoherentXBar`.

The saved cache measurements use the same firmware and inputs on both paths,
four 32 GiB/s controllers, 100 ns RAM latency, 100 ns transport/quantum,
and 512 DMA/transport credits. Each row contains three untraced samples
per queue mode, executed sequentially with alternating configuration/order:

| Path | Cycles | B/cycle | Median q1 (s) | Median q17 (s) | PDES |
| --- | ---: | ---: | ---: | ---: | ---: |
| Cacheless | 306,407 | 54.755 | 23.128 | 5.208 | 4.441x |
| L1I/L1D | 150,360 | 111.580 | 18.318 | 3.309 | 5.536x |

The caches improve modeled throughput by **2.038x**. With 17 queues,
median simulation wall time improves by **1.574x**. Wall time covers the
`simulate()` interval described in `simulation-wall.json`; it includes boot,
batch execution and validation/export. Process time is recorded separately.
Host process checks exclude competing gem5 runs, but do not eliminate other
host load or frequency variation.

Every partial remains `262144.0f` and the final sum remains `4194304.0f`.
Inputs, descriptors and outputs match between cached/cacheless configurations.
Every repeat has identical modeled statistics; the queue comparison checks
36,788 simulated counters for each cached pair. The fresh cacheless reference
is four cycles longer than the historical 306,403-cycle result; this
comparison uses the updated DMA-test firmware on both paths.

O3 load-port rejections are **zero on all 16 accelerators**. On accelerator 0,
`lsq0.blockedByCache` falls from 277,523 to zero and load-queue-full events
fall from 162,375 to 17,410. Executed loads fall from 1,139,383 to 302,424.
These are event/instruction counts, not stalled cycles. This verifies removal
of the cacheless request/retry bottleneck; the remaining resources have not
been isolated again under the cached configuration.

Patterned DMA checks cover invalidation of warmed dirty destinations,
sub-line reads of dirty CPU bytes, partial writes preserving dirty neighbors,
and recovery after invalid commands. Their traces and simulated statistics
match on one/two queues, with and without caches. Injected DMA errors also
drain and recover correctly with caches. The separate copy probe sustains
**64 B/cycle in both directions**, including coherent destination handling.
Eight-core rendering protocol tests pass for both TimingSimple and O3 on
one/nine queues, including repeated jobs, golden pixels and L1I evictions.
The four focused quick TestLib DMA/fault suites pass.

Commands, hashes, traffic, counters and all 19 reduction/DMA runs are saved in
[REDUCTION_CACHES.json](REDUCTION_CACHES.json). Reproduce or reinspect with:

```sh
python3 util/rt_reference/measure_reduction_caches.py \
    --output m5out/reduction-l1-new --runs 3
# Add --inspect-only to reinspect a completed directory with matching artifacts.
```

## DMA contract

`RTAccelControl` exposes two data request ports on the local crossbar, one for
reads and one for writes/MSI. Each accepts at most one packet per accelerator
cycle. A bounded chunk credit covers the entire read-to-write lifetime,
including pending/rejected requests and response delivery. Chunks stop at
both source and destination 64-byte boundaries; aligned traffic uses 64-byte
packets. The two ports permit one read and one write in the same cycle. With
L1D enabled, a third port cleans and invalidates each destination Data TCM
line before its write. It accepts at most one coherence request per
nanosecond and retries at router subcycles independently of the data ports.
Dirty CPU bytes can satisfy DMA reads through snoop responses. Chunk credits
include the coherence phase; partial writes preserve dirty neighboring bytes.

Every packet carries its phase and original byte offset in sender state.
Responses can return in a different order without changing destinations.
Independent, nonoverlapping chunks may complete out of order. Partially
overlapping source/destination allocations are rejected; identical ranges
are permitted. The DMA interface is a copy operation, not `memmove`.

A rejected request retains its packet until retry. An accepted packet is owned
by the memory path until its response returns. Chunk credits return only after
the destination write response or after an error has drained. On any response
error, the engine stops issuing new chunks, discards only packets it still
owns, and drains all accepted requests before making DMA status terminal.
Success likewise requires all destination responses. MSI follows terminal
DMA status; a new doorbell or MSI reconfiguration cannot overtake the previous
MSI response. Polling firmware uses I/O fences before start and after finish.

DMA statistics include `readRequests`, `writeRequests` (excluding MSI),
`bytesCopied` (successful destination responses), `busyTicks` (command start
through final response, including fill/drain), `peakChunks`, read/write
rejections, and command errors. A single region reduction copies 1 MiB plus
24 descriptor bytes and 4 result bytes. `writeRetries` includes coherence
port rejections as well as data-write/MSI rejections. Read and write packets
are both counted, while copied bytes count each chunk only once.

## Quick DES versus PDES evaluation

The quick path runs only untraced reduction pairs with eight and sixteen
accelerators, using the current four-controller defaults and 5 ns links and
quantum. Each accelerator processes 1 MiB: 8 MiB or 16 MiB total. It checks
outputs, simulated statistics and instantiated timing in each pair, while
recording simulation/process wall times and executable/firmware/source hashes.
One pair per count is a quick sample, not a repeated performance measurement.

```sh
make -C util/rt_reference reduction
python3 util/rt_reference/validate_reduction.py --quick --runs=1 \
    --output=m5out/reduction-5ns-quick
python3 util/rt_reference/report_reduction.py \
    m5out/reduction-5ns-quick/results.json --output=/tmp/REDUCTION_RESULTS.md
```

## Reproduce the full validation and timing

From the repository root, with the RISC-V cross compiler and picolibc installed:

```sh
make -C util/rt_reference reduction all backend-tests
scons build/RISCV/gem5.opt -j 8
python3 util/rt_reference/validate_reduction.py \
    --gem5 build/RISCV/gem5.opt --output m5out/reduction --runs 3
python3 util/rt_reference/report_reduction.py \
    m5out/reduction/results.json --output /tmp/REDUCTION_RESULTS.md
```

Use a fresh output directory for each experiment. Reinspect the saved evidence
without running simulations with the same command plus `--inspect-only`.
Inspection also checks that recorded executable, firmware and source hashes
match the current artifacts. The harness runs simulations sequentially,
alternates timing pair order, and checks the host process table for competing
active gem5 processes before, during and after every run. Run it in the host
PID namespace so competing simulations are visible. No traced runs contribute
to wall-clock speedup. These measurements do not claim that unrelated host
work or thermal/frequency variation is eliminated.

The baseline harness explicitly selects one memory controller and runs:

1. Patterned unaligned 8,193-byte copies on one/two queues with two transport
   credits and 16 chunk credits, forcing both read and write backpressure.
   Host guard bytes must remain unchanged. Four invalid DMA commands cover
   Code TCM access, address overflow, partial overlap and zero length; a valid
   command must succeed afterward. Full traces and simulated statistics match.
2. Read and write response failures on one/two queues using gem5's `BadAddr`
   responder in the last MiB of the host aperture. There are 64 simultaneous
   requests per failing phase. The traces prove all 128 error responses drain
   before recovery requests and the final MSI. New commands succeed afterward.
   A second pair uses two transport credits and 16 chunk credits, injecting
   errors with rejected packets and queued writes. It checks cancellation of
   packets still owned by DMA, draining and successful recovery.
3. A one-accelerator 1 MiB host-to-TCM-to-host copy with sufficient credits and
   bandwidth. It checks sustained reads and writes in a fixed, preselected
   window: packets 256 through 767 of the first 64 KiB tile in each direction.
   Rate is `64*(512-1)/(last_tick-first_tick)*1000` bytes/accelerator cycle.
   The required rate is at least 63 B/cycle, allowing subcycle endpoint effects.
   Whole-command averages include fill/drain and are reported separately.
4. The same full 16 MiB reduction on one/seventeen queues with traces enabled.
   Every input byte must be DMA-read exactly once from its assigned region;
   every output partial and MSI payload is checked. Result-write responses
   must precede MSI issue, and host status checks follow MSI visibility.
5. Three untraced reduction pairs, alternating execution order. Outputs and
   every simulated statistic in both batch and final dumps must match between
   queue modes and the traced reference. Only host timing and process memory
   usage counters are excluded; transaction traces match byte for byte.
6. A separate one-queue reduction with doubled shared-memory bandwidth and all
   other settings unchanged, confirming completion time is sensitive to the
   shared memory bottleneck. This diagnostic is excluded from PDES timing.

Direct run commands are:

```sh
build/RISCV/gem5.opt --outdir=m5out/reduction-q1 \
    configs/example/rt_accel_reduction.py --workload=reduction --queues=1
build/RISCV/gem5.opt --outdir=m5out/reduction-q17 \
    configs/example/rt_accel_reduction.py --workload=reduction --queues=17
build/RISCV/gem5.opt --outdir=m5out/reduction-mc2-q17 \
    configs/example/rt_accel_reduction.py --workload=reduction --queues=17 \
    --memory-controllers=2
build/RISCV/gem5.opt --outdir=m5out/dma-throughput \
    configs/example/rt_accel_reduction.py --workload=copy --queues=1 --trace
```

Firmware resets statistics after descriptors are prepared and before any
submission. It waits beyond the deferred reset event, submits all jobs, waits
for all completions and checks status, then dumps/resets statistics and waits
again. The first dump therefore includes submission, descriptor DMA, tile
fetches/compute, result DMA, MSI and host status checks; it excludes input
initialization, boot, descriptor preparation, host sum checking and export.
The final dump contains the remaining validation/export work. The saved
`batch_start_tick` and `batch_end_tick` name the actual deferred event bounds.

`simulation-wall.json` measures `time.perf_counter()` from immediately after
`m5.instantiate()` to return from `m5.simulate()`, covering the identical tick
interval from zero through firmware exit. It includes boot, workload,
validation, export and the explicit batch statistics dump. It excludes Python
image creation, construction and instantiation. The automatic exit statistics
dump and simulator cleanup occur after `simulate()` returns and are excluded
from simulation wall time. PDES speedup is the median one-queue
simulation wall time divided by the median seventeen-queue wall time. The
harness also saves whole-process elapsed time (launch through exit, including
the automatic exit dump and cleanup) and its separate speedup. CPU model,
machine, affinity, hashes, commands, all samples,
validation and timing comparisons are in `results.json`.

## Validation notes

An earlier untraced one-queue development run terminated with SIGSEGV in
`gem5::o3::IEW::writebackInsts`. Its log is preserved at
`m5out/reduction-v2/failed-timing-2-q1/run.log`. Subsequent debugger checks
completed 28 runs, including 16 repetitions of the exact failed command and
allocator/ASLR variations. The cause remains unidentified; that failed run
does not contribute to the reported timing samples.

ASan/UBSan validation passed the two-queue patterned transfer, high-credit
and low-credit fault cases, and full one/seventeen-queue reductions with
64 GiB/s shared RAM. The
instrumented reductions produced identical sums and all 31,628 simulated
statistics across both dumps. Evidence and commands are saved under
`m5out/reduction-sanitizers-v2/validation.json`.

The initial instrumented invocation stopped before simulation on the existing
statistics constructor's `DataWrap::self()` downcast at
`src/base/statistics.hh:233`. Diagnostic runs used a runtime suppression
limited to the `vptr` check in that header. Other UBSan checks remained enabled
with halt-on-error, as did ASan address checks. Leak detection was disabled.
The [UBSan runtime suppression documentation](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html#runtime-suppressions)
describes the suppression syntax. Reproduce the instrumented reduction with:

```sh
scons defconfig build/RISCV_ASAN build_opts/RISCV
scons build/RISCV_ASAN/gem5.opt -j 16 --with-asan --with-ubsan
cat > /tmp/rt-reduction-ubsan.supp <<'EOF'
vptr:src/base/statistics.hh
EOF
ASAN_OPTIONS=detect_leaks=0 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:\
suppressions=/tmp/rt-reduction-ubsan.supp \
build/RISCV_ASAN/gem5.opt --outdir=m5out/reduction-asan-q1 \
    configs/example/rt_accel_reduction.py --workload=reduction --queues=1 \
    --memory-bandwidth=64GiB/s
```

Repeat with `--queues=17` and a fresh output directory. The three DMA cases
use `--queues=2`: `--workload=dma-test --queue-depth=2 --dma-outstanding=16`,
`--workload=dma-fault`, and `--workload=dma-fault --queue-depth=2
--dma-outstanding=16`, respectively. Diagnostic wall times are excluded from
the PDES speedup.

## Add a kernel or experiment

`rt_accel_system.py` owns shared SimObject wiring and queue assignment.
`rt_accel_reduction.py` owns workload parameters and initialization.
`accelerator_runtime.[ch]` owns DMA start/wait/copy, fences, job interrupts and
completion. `host_runtime.[ch]` owns submission, MSI wait, status checks and
statistics boundaries. `reduction_accelerator.c` owns the reduction and DMA
probe kernels; `reduction_host.c` owns descriptors, expected values and export.
The rendering workload uses the same configuration and firmware runtimes.
`experiment.py` owns process timing, statistics parsing/comparison and ELF
symbol extraction. Workload validators own their correctness checks and use
these shared helpers; reports consume saved results. Add a resident kernel
and focused config/validator without copying the timing transport or firmware
completion protocol.

`report_reduction.py` produces the results document from the saved evidence.
`compare_memory_controllers.py` compares one/two controllers with three
untraced pairs each and complete traced correctness/queue-equivalence checks.
`report_memory_controllers.py` renders its saved evidence. Both experiments
reuse the same output, statistics, trace, process-timing and provenance checks.
The focused TestLib suites build firmware once and run DMA protocol/fault
checks as quick tests, with both queue modes and all three controller counts
tagged long:

```sh
cd tests
./main.py list gem5/rt_accel --isa=RISCV -q --suites
./main.py run gem5/rt_accel --isa=RISCV --skip-build
./main.py list gem5/rt_accel --isa=RISCV --length=long -q --suites
./main.py run gem5/rt_accel --isa=RISCV --length=long --skip-build
```

Use `--skip-build` only after building the RISCV binary. These suites require
`riscv64-unknown-elf-gcc` and picolibc (the default path or `$PICOLIBC`).
They are not registered when either dependency is absent; other suites run
normally.
See [REDUCTION_RESULTS.md](REDUCTION_RESULTS.md) for the measured results.
