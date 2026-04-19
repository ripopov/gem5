# Link Pressure Summary

## Result

- Status: `PASS`
- m5out directory: `/home/ripopov/work/riscv/gem5/m5out/rbook-link-pressure-pervnet-20260418-190438`
- Console log: `/home/ripopov/work/riscv/gem5/m5out/rbook-link-pressure-pervnet-20260418-190438/console.log`
- Stream pages: `1536` (buffer `6 MiB`, `98304` cache lines)
- Stats blocks observed: `6` (report uses the first 5 dumped windows)
- Main CPU: `0`
- Worker CPUs: `1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15`
- gem5 started Apr 18 2026 19:04:38
- Command line: `/home/ripopov/work/riscv/gem5/build/RISCV/gem5.opt -d /home/ripopov/work/riscv/gem5/m5out/rbook-link-pressure-pervnet-20260418-190438 /home/ripopov/work/riscv/gem5/configs/example/rbook_mesh_config.py --mem-size=512MiB --cmd=/home/ripopov/work/riscv/gem5/ruby-book/final/link-pressure/rbook_test_link_pressure --options=1536 --per-vnet-links`

## Window Sweep

| Threads | Ops | Cycles | Line ops/cycle | Avg flit queueing | Avg flit network | Flits received | HNF max | HNF min | HNF spread (max/min) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 98304 | 1246275 | 0.078878 | 37407.92 | 25286.38 | 2164549 | 6175 | 6144 | 1.005 |
| 2 | 196608 | 1061642 | 0.185192 | 3004.77 | 29701.04 | 3869326 | 12321 | 12288 | 1.003 |
| 4 | 393216 | 1069058 | 0.367815 | 5812.35 | 26978.05 | 7735362 | 24611 | 24576 | 1.001 |
| 8 | 786432 | 1297821 | 0.605963 | 16832.84 | 20038.50 | 15469619 | 49200 | 49152 | 1.001 |
| 16 | 1572864 | 1413600 | 1.112666 | 27157.02 | 15121.50 | 30974531 | 98456 | 98318 | 1.001 |

## Per-HNF Demand Accesses (16-thread window)

| HNF | Demand accesses |
| ---: | ---: |
| 0 | 98456 |
| 1 | 98405 |
| 2 | 98407 |
| 3 | 98417 |
| 4 | 98373 |
| 5 | 98325 |
| 6 | 98388 |
| 7 | 98380 |
| 8 | 98369 |
| 9 | 98406 |
| 10 | 98355 |
| 11 | 98390 |
| 12 | 98390 |
| 13 | 98318 |
| 14 | 98412 |
| 15 | 98424 |
