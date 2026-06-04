# TrafficGen DDR4-2400 4Gb x8 Memory Backend Comparison

This directory contains a synthetic memory testbench that drives TrafficGen
requests directly into a memory backend through gem5's classic no-cache path:

```text
TrafficGen -> SystemXBar/NoCache -> memory
```

It is configured for an apples-to-apples comparison of three memory backends
on one shared DDR4-2400 4Gb x8, single-channel, single-rank, 4GiB device:

- `gem5`     — gem5 native `MemCtrl` + a custom `DDR4_2400_8x8_4GiB`
               `DRAMInterface` (timing from the DRAMSys reference memspec).
- `dramsys`  — DRAMSys with the JEDEC DDR4-2400 4Gb x8 memspec.
- `dramsim3` — DRAMSim3 with the derived `DDR4_4Gb_x8_2400_1rank_4GiB.ini`.

The reference device specification and the apples-to-apples requirements are in
`DDR4-2400-4Gb-x8-single channel-single rank-4GiB-study.md` at the repo root.

## Reproduce from a fresh clone

These steps take a clean gem5 checkout to a regenerated report. The custom
backend configs (the DRAMSim3 `.ini`, the DRAMSys `gem5_configs/`, and the gem5
interface in `trafficgen-ddr4.py`) are already tracked in the repo, so only the
two external simulators have to be fetched.

Host prerequisites: a C++ toolchain supported by gem5, Python 3, SCons, and
**CMake >= 3.24** (required by DRAMSys).

### 1. Clone the external memory simulators

```sh
# DRAMSim3 -> ext/dramsim3/DRAMsim3  (note the lower-case "sim")
git clone https://github.com/umd-memsys/DRAMsim3.git ext/dramsim3/DRAMsim3

# DRAMSys -> ext/dramsys/DRAMSys  (pinned to the verified v5.3.1)
git clone https://github.com/tukl-msd/DRAMSys --branch v5.3.1 --depth 1 \
    ext/dramsys/DRAMSys
```

Both directories are gitignored, so they stay out of the gem5 tree.

### 2. Build the DRAMSim3 shared library

```sh
cd ext/dramsim3/DRAMsim3
mkdir -p build && cd build
# On CMake 4.x add -DCMAKE_POLICY_VERSION_MINIMUM=3.5 (DRAMSim3 still
# declares a pre-3.5 minimum policy):
cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ..
make -j"$(nproc)"
cd ../../../..
```

This produces `ext/dramsim3/DRAMsim3/libdramsim3.so`. DRAMSys needs no separate
build step — gem5 compiles it in-tree.

### 3. Build gem5

SCons auto-detects both backends from `ext/` (no extra flags), so just build:

```sh
scons build/RISCV/gem5.opt -j"$(nproc)"
```

### 4. Run the sweep and regenerate the report

```sh
python3 util/trafficgen_memory_sweep/run_trafficgen_memory_sweep.py
```

The full sweep is 5 patterns x 3 backends x 10 rates = 150 short runs and
writes everything under `m5out/ddr4-2400-4gb-x8-memory-sweep/`:
`report.md`, `results.csv`, `results.json`, and `plots/*.svg`. Re-run after any
code change to refresh the committed report.

> **Note — DRAMSys TLM-bridge fix.** DRAMSys is attached through gem5's SystemC
> TLM bridge (`Gem5ToTlmBridge`). The bridge previously serialized request
> admission and capped DRAMSys at ~8.5 GB/s (~44% of peak) for every pattern.
> It now stages requests (new `request_queue_depth` parameter, default 16; the
> `DRAMSysMem` component uses 32) and applies only the residual crossbar header
> delay, so DRAMSys reaches ~19 GB/s and its bandwidth tracks the access
> pattern. A depth of 1 reproduces the legacy behavior. See the generated
> report's "Apples-to-Apples Notes" section for the full analysis.

## Single run

```sh
build/RISCV/gem5.opt \
    configs/example/gem5_library/trafficgen_memory/trafficgen-ddr4.py \
    --memory-backend gem5 \
    --traffic-pattern linear-read \
    --rate 8GiB/s
```

`--memory-backend` accepts `gem5`, `dramsys`, or `dramsim3`. All three expose
one 4 GiB range, one channel, one rank, eight x8 devices, and DDR4-2400 timing
(tCK = 0.833 ns).

## Sweep and report

The sweep helper runs all default patterns and rates across all three backends
and writes `results.csv`, `results.json`, SVG plots, and a Markdown report with
latency in nanoseconds and bandwidth in decimal GB/s:

```sh
python3 util/trafficgen_memory_sweep/run_trafficgen_memory_sweep.py
```

Outputs land in `m5out/ddr4-2400-4gb-x8-memory-sweep/` by default.

## Build dependencies

gem5 must be built with both DRAMSys and DRAMSim3 support. See
[Reproduce from a fresh clone](#reproduce-from-a-fresh-clone) above for the
clone, build, run, and report steps.

## Address-mapping alignment

The three backends default to different address mappings. To keep the
comparison fair they are aligned so banks/bank-groups interleave at cache-line
granularity:

- gem5: `RoCoRaBaCh`
- DRAMSim3: `rochrababgco`
- DRAMSys: `am_ddr4_4Gbx8_1rank_bginterleave.json`

See the generated report for the full list of matched settings and the
remaining unavoidable model differences.
