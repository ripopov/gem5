# CHI RN-I Memory Traffic Sweep

This directory contains a synthetic memory testbench for driving TrafficGen
requests through a cacheless CHI RN-I path:

```text
TrafficGen -> CHI RN-I -> CHI HNF directory -> CHI SNF -> memory
```

The config script supports gem5's internal single-channel DDR4 MemCtrl backend
and the DRAMSys DDR4 backend:

```sh
build/RISCV/gem5.opt \
    configs/example/gem5_library/chi_rni_memory/chi-rni-ddr4.py \
    --memory-backend gem5 \
    --traffic-pattern linear-read \
    --rate 8GiB/s
```

The sweep/report helper runs all default traffic patterns and writes CSV, JSON,
SVG plots, and a Markdown report:

```sh
python3 util/chi_memory_sweep/run_chi_memory_sweep.py \
    --outdir m5out/chi-rni-ddr4-memory-sweep \
    --duration 5us \
    --addr-range 256MiB \
    --mem-size 4GiB
```

The default gem5 backend uses a local DDR4-1866 x8 4 GiB DRAMInterface whose
capacity and timing assumptions are aligned with
`ext/dramsys/gem5_configs/ddr4-gem5-se.json`. Both backends expose the same
4 GiB single-channel address range by default, use 64-byte traffic blocks, and
can be swept over the same linear, random, read, write, and mixed workloads.
