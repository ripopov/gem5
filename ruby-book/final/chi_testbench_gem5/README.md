# chi_testbench_gem5

CPU-less CHI/Garnet testbench focused on two sequences:

| Scenario | Sequence | Role |
| --- | --- | --- |
| `memset` | `MemsetSequence` | Tile 7 issues pipelined burst writes. |
| `ping_pong` | `PingPongSequence` | Tiles 0 and 15 hand off one shared cache line. |

## Build

```sh
scons build/RISCV/gem5.opt -j$(nproc) PROTOCOL=CHI
```

## Run

```sh
make -C ruby-book/final/chi_testbench_gem5 run-memset
make -C ruby-book/final/chi_testbench_gem5 run-ping_pong
make -C ruby-book/final/chi_testbench_gem5 run-all
```

Pass `RN_MODE=rni` or `RN_MODE=rnf_l2` to select the per-tile request
node mode. The default is `rnf_l2`.

Each run creates a timestamped output directory under top-level `m5out/`
and updates `ruby-book/final/chi_testbench_gem5/m5out/last-<scenario>-<mode>`.

## Layout

| Path | Role |
| --- | --- |
| `driver/rbook_testbench_gem5.py` | Top-level config: 4x4 CHI mesh, scenario dispatch, RN-mode wiring. |
| `driver/cfg_rn.py` | Tile-local CHI request-node wiring. |
| `driver/address_planner.py` | HNF-aware cache-line address math. |
| `scenarios/memset.py` | Builds the memset workload. |
| `scenarios/ping_pong.py` | Builds the ping-pong workload. |
| `src/chi_testbench_gem5/sequences/memset.cc` | Memset sequence implementation. |
| `src/chi_testbench_gem5/sequences/ping_pong.cc` | Ping-pong sequence implementation. |
