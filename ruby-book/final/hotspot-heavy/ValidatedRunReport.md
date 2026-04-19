# Hotspot Heavy Summary

## Result

- Status: `PASS`
- m5out directory: `/home/ripopov/work/riscv/gem5/m5out/rbook-hotspot-heavy-20260418-185639`
- Console log: `/home/ripopov/work/riscv/gem5/m5out/rbook-hotspot-heavy-20260418-185639/console.log`
- Stats blocks observed: `2` (report uses the first dumped window)
- Stream pages: `2048`
- Mapped space per stream: `8 MiB`
- Read streams: `1`
- Write streams: `0`
- Targeted HNF15 lines per page across all streams: `4`
- Hot line offset: `0x3c0`
- Main CPU: `0`
- gem5 started Apr 18 2026 18:56:39
- Command line: `/home/ripopov/work/riscv/gem5/build/RISCV/gem5.opt -d /home/ripopov/work/riscv/gem5/m5out/rbook-hotspot-heavy-20260418-185639 /home/ripopov/work/riscv/gem5/configs/example/rbook_mesh_config.py --mem-size=512MiB --cmd=/home/ripopov/work/riscv/gem5/ruby-book/final/hotspot-heavy/rbook_test_hotspot_heavy '--options=2048 1 0'`

## Measured Window

| Threads | Ops | Cycles | Line ops/cycle | HNF15 accesses | HNF15 accesses/cycle | HNF15 / ops | Net flits/cycle | HNF15 ext flits/cycle | HNF15 ext flits/op | Router15 reads/cycle | Avg flit queueing | Avg flit network |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 8192 | 127243 | 0.064381 | 8199 | 0.064436 | 1.001 | 1.618 | 0.585 | 9.085 | 0.779 | 46011.32 | 17115.81 |
