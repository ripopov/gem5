# Hotspot Backpressure Summary

## Result

- Status: `PASS`
- m5out directory: `/home/ripopov/work/riscv/gem5/m5out/rbook-hotspot-20260416-180308`
- Console log: `/home/ripopov/work/riscv/gem5/m5out/rbook-hotspot-20260416-180308/console.log`
- Stats blocks observed: `6` (checker uses the first `5` dumped windows)
- Stream pages per active CPU: `2048`
- Measured working set per active CPU: `8 MiB`
- HNF15 lines touched per page: `4`
- Hot line offset: `0x3c0`
- Main CPU: `0`
- Worker CPUs: `1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15`
- gem5 started Apr 16 2026 18:03:08
- Command line: `/home/ripopov/work/riscv/gem5/build/RISCV/gem5.opt -d /home/ripopov/work/riscv/gem5/m5out/rbook-hotspot-20260416-180308 /home/ripopov/work/riscv/gem5/configs/example/rbook_mesh_config.py --cmd=/home/ripopov/work/riscv/gem5/ruby-book/final/hotspot/rbook_test_hotspot --options=2048`

## Window Sweep

| Threads | Ops | Cycles | Throughput (ops/cycle) | Avg flit queueing | Avg flit network | HNF15 accesses | HNF15 / ops | HNF15 ext-link flits | Router15 buffer reads | Router15 buffer writes | Inbound router15 flits |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 8192 | 148677 | 0.055099 | 2546.45 | 16960.97 | 8209 | 1.002 | 74868 | 99561 | 99561 | 60371 |
| 2 | 16384 | 274236 | 0.059744 | 1835.61 | 19888.81 | 16407 | 1.001 | 250358 | 275281 | 275281 | 123791 |
| 4 | 32768 | 499586 | 0.065590 | 1391.67 | 18138.19 | 32801 | 1.001 | 506916 | 556648 | 556648 | 241478 |
| 8 | 65536 | 958986 | 0.068339 | 1242.07 | 17152.21 | 65588 | 1.001 | 1016407 | 1115358 | 1115358 | 483750 |
| 16 | 131072 | 1776029 | 0.073801 | 2210.84 | 14663.71 | 131212 | 1.001 | 2033547 | 2257984 | 2257984 | 992945 |

## Interpretation

- The queueing latency stays elevated across the sweep, with `2546.45` at 1 thread and `2210.84` at 16 threads.
- The HNF15 ext-link flits rise from `74868` to `2033547` across the sweep.
- Destination pressure per requested line rises from `9.14` to `15.51` HNF15 ext-link flits/op.
- HNF15 demand accesses track the intended data-stream ops closely, from `1.002` to `1.001` accesses per requested line.
- Per-thread throughput efficiency falls from `0.055099` to `0.004613` ops/cycle/thread.
- Each measured line is private to one CPU but shares the same HNF15 home-node offset, so the traffic increase reflects many-to-one pressure rather than false sharing.

## Inbound Router15 Links

| Threads | Link | Flits |
| ---: | --- | ---: |
| 1 | `int_links11` | 43 |
| 1 | `int_links35` | 60306 |
| 1 | `int_links78` | 22 |
| 2 | `int_links11` | 38 |
| 2 | `int_links35` | 123731 |
| 2 | `int_links78` | 22 |
| 4 | `int_links11` | 45 |
| 4 | `int_links35` | 241411 |
| 4 | `int_links78` | 22 |
| 8 | `int_links11` | 53 |
| 8 | `int_links35` | 483675 |
| 8 | `int_links78` | 22 |
| 16 | `int_links11` | 221871 |
| 16 | `int_links35` | 696787 |
| 16 | `int_links78` | 74287 |
