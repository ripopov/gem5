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

gem5 must be built with both DRAMSys and DRAMSim3 support. DRAMSim3 must be
cloned and built as a shared library first:

```sh
cd ext/dramsim3/DRAMsim3 && mkdir -p build && cd build
cmake .. && make
```

DRAMSys is built from `ext/dramsys/DRAMSys`. Rebuild gem5 afterwards
(`scons build/RISCV/gem5.opt`).

## Address-mapping alignment

The three backends default to different address mappings. To keep the
comparison fair they are aligned so banks/bank-groups interleave at cache-line
granularity:

- gem5: `RoCoRaBaCh`
- DRAMSim3: `rochrababgco`
- DRAMSys: `am_ddr4_4Gbx8_1rank_bginterleave.json`

See the generated report for the full list of matched settings and the
remaining unavoidable model differences.
