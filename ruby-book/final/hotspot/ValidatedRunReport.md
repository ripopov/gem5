# Hotspot Backpressure Summary

## Result

- Status: `PASS`
- m5out directory: `/home/ripopov/work/riscv/gem5/m5out/rbook-hotspot-20260421-075058`
- Console log: `/home/ripopov/work/riscv/gem5/m5out/rbook-hotspot-20260421-075058/console.log`
- Stats blocks observed: `6` (checker uses the first `5` dumped windows)
- Stream pages per active CPU: `2048`
- Measured working set per active CPU: `8 MiB`
- HNF15 lines touched per page: `4`
- Hot line offset: `0x3c0`
- Main CPU: `0`
- Worker CPUs: `1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15`
- gem5 started Apr 21 2026 07:50:58
- Command line: `/home/ripopov/work/riscv/gem5/build/RISCV/gem5.opt -d /home/ripopov/work/riscv/gem5/m5out/rbook-hotspot-20260421-075058 /home/ripopov/work/riscv/gem5/configs/example/rbook_mesh_config.py --cmd=/home/ripopov/work/riscv/gem5/ruby-book/final/hotspot/rbook_test_hotspot --options=2048`

## Window Sweep

| Threads | Ops | Cycles | Throughput (ops/cycle) | Avg flit queueing | Avg flit network | HNF15 accesses | HNF15 / ops | HNF15 ext-link flits | Router15 buffer reads | Router15 buffer writes | Inbound router15 flits |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 8192 | 126955 | 0.064527 | 45643.53 | 17184.89 | 8209 | 1.002 | 74868 | 99580 | 99580 | 60391 |
| 2 | 16384 | 252661 | 0.064846 | 21522.40 | 20534.38 | 16407 | 1.001 | 250055 | 274840 | 274840 | 123696 |
| 4 | 32768 | 419613 | 0.078091 | 10768.56 | 21085.33 | 32800 | 1.001 | 507394 | 556934 | 556934 | 241631 |
| 8 | 65536 | 845135 | 0.077545 | 11177.79 | 20336.48 | 65589 | 1.001 | 1014143 | 1112777 | 1112777 | 483003 |
| 16 | 131072 | 1680183 | 0.078011 | 15655.38 | 15290.29 | 131223 | 1.001 | 2032493 | 2256238 | 2256238 | 992263 |

## Interpretation

- The queueing latency stays elevated across the sweep, with `45643.53` at 1 thread and `15655.38` at 16 threads.
- The HNF15 ext-link flits rise from `74868` to `2032493` across the sweep.
- Destination pressure per requested line rises from `9.14` to `15.51` HNF15 ext-link flits/op.
- HNF15 demand accesses track the intended data-stream ops closely, from `1.002` to `1.001` accesses per requested line.
- Per-thread throughput efficiency falls from `0.064527` to `0.004876` ops/cycle/thread.
- Each measured line is private to one CPU but shares the same HNF15 home-node offset, so the traffic increase reflects many-to-one pressure rather than false sharing.

## Inbound Router15 Links

| Threads | Link | Flits |
| ---: | --- | ---: |
| 1 | `int_links11` | 47 |
| 1 | `int_links35` | 60308 |
| 1 | `int_links78` | 36 |
| 2 | `int_links11` | 44 |
| 2 | `int_links35` | 123630 |
| 2 | `int_links78` | 22 |
| 4 | `int_links11` | 45 |
| 4 | `int_links35` | 241564 |
| 4 | `int_links78` | 22 |
| 8 | `int_links11` | 64 |
| 8 | `int_links35` | 482917 |
| 8 | `int_links78` | 22 |
| 16 | `int_links11` | 221863 |
| 16 | `int_links35` | 696397 |
| 16 | `int_links78` | 74003 |
