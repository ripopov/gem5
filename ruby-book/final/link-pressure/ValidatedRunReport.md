# Link Pressure Summary

## Result

- Status: `PASS`
- m5out directory: `/home/ripopov/work/riscv/gem5/m5out/rbook-link-pressure-v1b-pervnet-20260416-232515`
- Console log: `/home/ripopov/work/riscv/gem5/m5out/rbook-link-pressure-v1b-pervnet-20260416-232515/console.log`
- Stream pages: `1024` (buffer `4 MiB`, `65536` cache lines)
- Stats blocks observed: `6` (report uses the first 5 dumped windows)
- Main CPU: `0`
- Worker CPUs: `1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15`
- gem5 started Apr 16 2026 23:25:15
- Command line: `/home/ripopov/work/riscv/gem5/build/RISCV/gem5.opt -d /home/ripopov/work/riscv/gem5/m5out/rbook-link-pressure-v1b-pervnet-20260416-232515 /home/ripopov/work/riscv/gem5/configs/example/rbook_mesh_config.py --mem-size=512MiB --cmd=/home/ripopov/work/riscv/gem5/ruby-book/final/link-pressure/rbook_test_link_pressure --options=1024 --per-vnet-links`

## Window Sweep

| Threads | Ops | Cycles | Line ops/cycle | Avg flit queueing | Avg flit network | Flits received | HNF max | HNF min | HNF spread (max/min) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 65536 | 898200 | 0.072964 | 53046.41 | 22132.36 | 1509151 | 4127 | 4096 | 1.008 |
| 2 | 131072 | 707176 | 0.185346 | 3052.82 | 29547.20 | 2558914 | 8226 | 8192 | 1.004 |
| 4 | 262144 | 714533 | 0.366875 | 5786.64 | 26952.45 | 5113673 | 16420 | 16383 | 1.002 |
| 8 | 524288 | 860799 | 0.609071 | 17013.69 | 19965.79 | 10226117 | 32807 | 32768 | 1.001 |
| 16 | 1048576 | 950610 | 1.103056 | 26531.56 | 15208.52 | 20542739 | 65678 | 65550 | 1.002 |

## Per-HNF Demand Accesses (16-thread window)

| HNF | Demand accesses |
| ---: | ---: |
| 0 | 65678 |
| 1 | 65637 |
| 2 | 65633 |
| 3 | 65650 |
| 4 | 65598 |
| 5 | 65556 |
| 6 | 65616 |
| 7 | 65621 |
| 8 | 65600 |
| 9 | 65636 |
| 10 | 65593 |
| 11 | 65620 |
| 12 | 65624 |
| 13 | 65550 |
| 14 | 65641 |
| 15 | 65639 |
