# CHI RN-I Memory Traffic Sweep

This directory contains a synthetic memory testbench for driving TrafficGen
requests through a cacheless CHI RN-I path:

```text
TrafficGen -> CHI RN-I -> CHI HNF directory -> CHI SNF -> memory
```

The config script supports gem5's internal single-channel DDR5 MemCtrl backend
and the DRAMSys backend:

```sh
build/RISCV/gem5.opt \
    configs/example/gem5_library/chi_rni_memory/chi-rni-ddr5.py \
    --memory-backend gem5 \
    --traffic-pattern linear-read \
    --rate 8GiB/s
```

The sweep/report helper runs all default traffic patterns and writes CSV, JSON,
SVG plots, and a Markdown report:

```sh
python3 util/chi_memory_sweep/run_chi_memory_sweep.py \
    --outdir m5out/chi-rni-memory-sweep \
    --duration 5us \
    --addr-range 256MiB \
    --mem-size 4GiB
```

DRAMSys v5.3.1, the version currently verified by `ext/dramsys/README`, builds
with gem5 but does not include DRAMSys DDR5 model source files. The default
DRAMSys runs therefore use `ext/dramsys/gem5_configs/ddr4-gem5-se.json`.
A DDR5-capable DRAMSys checkout and matching JSON can be supplied with
`--dramsys-config` without changing the CHI testbench.
