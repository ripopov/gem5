# Hop-Distance Latency Summary

## Result

- Status: `PASS`
- m5out directory: `/home/ripopov/work/riscv/gem5/m5out/rbook-hop-latency-20260421-074743`
- Console log: `/home/ripopov/work/riscv/gem5/m5out/rbook-hop-latency-20260421-074743/console.log`
- Stats blocks observed: `3`
- gem5 started Apr 21 2026 07:47:43
- Command line: `/home/ripopov/work/riscv/gem5/build/RISCV/gem5.opt -d /home/ripopov/work/riscv/gem5/m5out/rbook-hop-latency-20260421-074743 /home/ripopov/work/riscv/gem5/configs/example/rbook_mesh_config.py --cmd=/home/ripopov/work/riscv/gem5/ruby-book/final/hop_latency/rbook_test_hop_latency`

## Measurement Setup

- Samples per class: `8`
- Near line offset: `0x0`
- Far line offset: `0x3c0`
- Timed loads are bracketed by a 4 MiB private-cache eviction sweep.
- Two isolated probes are wrapped with `m5_reset_stats()` and `m5_dump_reset_stats()`.

## Latency

| Metric | Cycles |
| --- | ---: |
| Near average | 83.25 |
| Far average | 179.00 |
| Delta average | 95.75 |
| Near isolated probe | 83 |
| Far isolated probe | 179 |

## Sample Vectors

- Near samples: `83 83 84 83 83 84 83 83`
- Far samples: `179 179 179 179 179 179 179 179`

## Isolated HN-F Accesses

| Block | Demand accesses | Nonzero slices |
| --- | --- | --- |
| Near | `hnf0=1`, `hnf15=0` | hnf0=1, hnf4=1, hnf5=1 |
| Far | `hnf0=0`, `hnf15=1` | hnf4=1, hnf5=1, hnf13=1, hnf15=1 |

## Isolated Ext-Link Totals

| Block | HNF0 ext-link | HNF15 ext-link |
| --- | ---: | ---: |
| Near | 10 | 0 |
| Far | 0 | 7 |

## Expected Far XY Path

| Link | Near block flits | Far block flits |
| --- | ---: | ---: |
| `int_links00` | 2 | 7 |
| `int_links01` | 0 | 3 |
| `int_links02` | 0 | 2 |
| `int_links33` | 0 | 2 |
| `int_links34` | 0 | 2 |
| `int_links35` | 0 | 1 |

## Interpretation

- The far load is slower by `95.75` cycles.
- The isolated near probe stays local to `hnf0` and does not traverse the full diagonal path.
- The isolated far probe reaches `hnf15` and activates all expected XY mesh links.
- Small extra slice accesses can still appear inside the isolated windows because the benchmark itself executes instructions and accesses stack data while stats are enabled.

## Conclusion

- Stage 3b passes for this run.
- The observed latency delta matches the expected hop-distance penalty and the isolated network evidence matches the intended near-versus-far interpretation.
