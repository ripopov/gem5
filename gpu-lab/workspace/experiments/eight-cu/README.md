# Eight-CU evaluation probes

These probes evaluate the teaching configuration separately from the naive
student sample. `tiled16` and `register64` compute the same fixed
`128×128 × 128×256` product and verify every output. `lds1` and `lds32` compare
32-bit LDS access strides; `stream` reads 32 MiB once and verifies each thread's
sum. Device properties are printed to expose topology mismatches.

From `gpu-lab/workspace`, compile with the local SDK (the host needs access to
the Docker socket):

```sh
docker run --rm --network none \
  --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/workspace" -w /workspace \
  gem5-gpu-hsakmt-sdk:local \
  hipcc --offload-arch=gfx942 -O2 -std=c++17 \
  experiments/eight-cu/main.cpp -o work/apps/gem5-kmd/eight-cu-probe

./scripts/run-app.sh --backend gem5-kmd --run-name study-tiled16 \
  work/apps/gem5-kmd/eight-cu-probe tiled16
./scripts/run-app.sh --backend gem5-kmd --run-name study-register64 \
  work/apps/gem5-kmd/eight-cu-probe register64
./scripts/run-app.sh --backend gem5-kmd --run-name study-stream \
  work/apps/gem5-kmd/eight-cu-probe stream
```

Use `lds1` or `lds32` in place of `tiled16` for the LDS probes. For memory
sensitivity experiments, append configuration arguments after `--`:

- `--dgpu-mem-type=HBM_1000_4H_1x64`: a single 8 GB/s interface. This also
  changes bank count, row size, and refresh timing; it is not an isolated
  bandwidth parameter change.
- `--bw-scalor=1`: a wider interconnect. This option also changes shared L2
  capacity and banking; inspect `config.ini` before interpreting the result.
- Both options together: the wider fabric with the 8 GB/s memory interface.

For an isolated model sensitivity test, the service now honors gem5's standard
`--param` option. This changes only burst time, keeping the default interface,
bank count, capacity, and fabric:

```sh
./scripts/run-app.sh --backend gem5-kmd --run-name study-stream-burst2ns \
  work/apps/gem5-kmd/eight-cu-probe stream -- \
  '--param=system.gpu_mem_ctrls[0].dram.tBURST="2ns"'
```

The 2 ns burst is a synthetic sensitivity setting, not a physical HBM profile
or a new default. Check the final parameter in `config.ini`.

For each completed run, validate the result, packet retirement, and clean
teardown, and extract timing with:

```sh
python3 experiments/eight-cu/summarize.py work/runs/<run-id>
```

`shaderActiveTicks` includes every GPU kernel in that process, including ROCr
copy kernels. The small LDS probes launched two kernels in the evaluated
runtime; GEMM and streaming launched one. The streaming rate is useful input
bytes divided by GPU-active seconds, not a whole-run DRAM utilization counter.

The [evaluation report](../../../EightCuEvaluation.md) records results and
limitations, including the current LDS bank-address issue. These are diagnostic
probes, not tuned GEMM implementations or hardware-performance references.
