# False-Sharing Summary

## Result

- Status: `PASS`
- m5out directory: `/home/ripopov/work/riscv/gem5/m5out/rbook-false-sharing-20260410-074612`
- Console log: `/home/ripopov/work/riscv/gem5/m5out/rbook-false-sharing-20260410-074612/console.log`
- Stats blocks observed: `2`
- Iterations per participant: `128`
- Shared-line offset: `0x3c0`
- gem5 started Apr 10 2026 07:46:13
- Command line: `/home/ripopov/work/riscv/gem5/build/RISCV/gem5.opt -d /home/ripopov/work/riscv/gem5/m5out/rbook-false-sharing-20260410-074612 /home/ripopov/work/riscv/gem5/configs/example/rbook_mesh_config.py --cmd=/home/ripopov/work/riscv/gem5/ruby-book/final/false_sharing/rbook_test_false_sharing --options=128`

## Benchmark Output

- CPU 0 participant: `0`
- CPU 15 participant: `15`
- CPU 0 loop cycles: `80377`
- CPU 15 loop cycles: `5839`
- Final value0: `128`
- Final value1: `128`

## L1D Demand Activity

| CPU | Accesses | Misses |
| --- | ---: | ---: |
| `cpu0` | 4994 | 13 |
| `cpu15` | 389 | 19 |

## CHI Counters

| Counter | Total |
| --- | ---: |
| `Store::total` | 2319 |
| `ReadUnique::total` | 10 |
| `CompAck::total` | 160 |
| `SendCompAck::total` | 158 |

## Forward Diagonal Path

| Link | Flits |
| --- | ---: |
| `int_links00` | 200 |
| `int_links01` | 189 |
| `int_links02` | 178 |
| `int_links33` | 169 |
| `int_links34` | 199 |
| `int_links35` | 253 |

## Reverse Diagonal Path

| Link | Flits |
| --- | ---: |
| `int_links47` | 31 |
| `int_links46` | 16 |
| `int_links45` | 6 |
| `int_links14` | 8 |
| `int_links13` | 16 |
| `int_links12` | 29 |

## Interpretation

- Both participants touched the same cache line from opposite corners of the mesh.
- Nonzero `ReadUnique` and `CompAck` counters indicate coherence-mediated ownership transfers.
- Nonzero flits on both diagonal directions show that requests and invalidation/response traffic crossed the mesh rather than staying local.

## Conclusion

- Stage 3c passes for this run.
- The measured window shows the expected two-core false-sharing pattern: both L1s participate, CHI ownership changes occur, and mesh traffic appears in both diagonal directions.
