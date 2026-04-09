# Hop-Distance Latency Summary

## Result

- Status: `PASS`
- m5out directory: `/home/ripopov/work/riscv/gem5/m5out/rbook-hop-latency-20260409-231615`
- Console log: `/home/ripopov/work/riscv/gem5/m5out/rbook-hop-latency-20260409-231615/console.log`
- Stats blocks observed: `3`
- gem5 started Apr  9 2026 23:16:15
- Command line: `/home/ripopov/work/riscv/gem5/build/RISCV/gem5.opt -d /home/ripopov/work/riscv/gem5/m5out/rbook-hop-latency-20260409-231615 /home/ripopov/work/riscv/gem5/configs/example/rbook_mesh_config.py --cmd=/home/ripopov/work/riscv/gem5/ruby-book/final/hop_latency/rbook_test_hop_latency`

## Measurement Setup

- Samples per class: `8`
- Near line offset: `0x0`
- Far line offset: `0x3c0`
- Timed loads are bracketed by a 4 MiB private-cache eviction sweep.
- Two isolated probes are wrapped with `m5_reset_stats()` and `m5_dump_reset_stats()`.

## Latency

| Metric | Cycles |
| --- | ---: |
| Near average | 88.00 |
| Far average | 184.00 |
| Delta average | 96.00 |
| Near isolated probe | 88 |
| Far isolated probe | 184 |

## Sample Vectors

- Near samples: `88 88 88 88 88 88 88 88`
- Far samples: `184 184 184 184 184 184 184 184`

## Isolated HN-F Accesses

| Block | Demand accesses | Nonzero slices |
| --- | --- | --- |
| Near | `hnf0=1`, `hnf15=0` | hnf0=1, hnf5=1 |
| Far | `hnf0=0`, `hnf15=1` | hnf5=1, hnf12=1, hnf15=1 |

## Isolated Ext-Link Totals

| Block | HNF0 ext-link | HNF15 ext-link |
| --- | ---: | ---: |
| Near | 10 | 0 |
| Far | 0 | 10 |

## Expected Far XY Path

| Link | Near block flits | Far block flits |
| --- | ---: | ---: |
| `int_links00` | 3 | 6 |
| `int_links01` | 0 | 3 |
| `int_links02` | 0 | 3 |
| `int_links33` | 0 | 3 |
| `int_links34` | 0 | 3 |
| `int_links35` | 0 | 3 |

## Interpretation

- The far load is slower by `96.00` cycles.
- The isolated near probe stays local to `hnf0` and does not traverse the full diagonal path.
- The isolated far probe reaches `hnf15` and activates all expected XY mesh links.
- Small extra slice accesses can still appear inside the isolated windows because the benchmark itself executes instructions and accesses stack data while stats are enabled.

## Conclusion

- Stage 3b passes for this run.
- The observed latency delta matches the expected hop-distance penalty and the isolated network evidence matches the intended near-versus-far interpretation.
