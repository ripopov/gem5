# False-Sharing Summary

## Result

- Status: `PASS`
- m5out directory: `m5out/rbook-false-sharing-20260410-082010`
- Console log: `m5out/rbook-false-sharing-20260410-082010/console.log`
- Stats blocks observed: `3` (checker uses the first two dumped windows)
- Iterations per participant: `128`
- Shared-line offset: `0x3c0`
- gem5 started Apr 10 2026 08:20:10
- Command line: `/home/ripopov/work/riscv/gem5/build/RISCV/gem5.opt -d /home/ripopov/work/riscv/gem5/m5out/rbook-false-sharing-20260410-082010 /home/ripopov/work/riscv/gem5/configs/example/rbook_mesh_config.py --cmd=/home/ripopov/work/riscv/gem5/ruby-book/final/false_sharing/rbook_test_false_sharing --options=128`

## Benchmark Output

- CPU 0 participant: `0`
- CPU 15 participant: `15`
- Control ping-pong average: `978.15` cycles
- False-sharing ping-pong average: `1774.89` cycles
- Average latency delta: `796.74` cycles
- Control range: `974` .. `1503`
- False-sharing range: `1507` .. `1777`

## Control vs False-Sharing Latency

| Phase | Average | Min | Max |
| --- | ---: | ---: | ---: |
| `control` | 978.15 | 974 | 1503 |
| `false-sharing` | 1774.89 | 1507 | 1777 |

## Control Window

| Metric | Value |
| --- | ---: |
| `cpu0 L1D accesses` | 2972 |
| `cpu15 L1D accesses` | 3074 |
| `Store::total` | 643 |
| `ReadUnique::total` | 3 |
| `CompAck::total` | 1038 |
| `SendCompAck::total` | 1038 |
| `forward diagonal flits` | 7047 |
| `reverse diagonal flits` | 12 |

## False-Sharing Window

| Metric | Value |
| --- | ---: |
| `cpu0 L1D accesses` | 4869 |
| `cpu15 L1D accesses` | 4976 |
| `Store::total` | 643 |
| `ReadUnique::total` | 2 |
| `CompAck::total` | 2050 |
| `SendCompAck::total` | 2050 |
| `forward diagonal flits` | 19990 |
| `reverse diagonal flits` | 2 |

## False-Sharing Path Detail

| Link | Flits |
| --- | ---: |
| `int_links00` | 3969 |
| `int_links01` | 2950 |
| `int_links02` | 2950 |
| `int_links33` | 2950 |
| `int_links34` | 2950 |
| `int_links35` | 4221 |

## Interpretation

- The control ping-pong measures the cost of the turn-taking protocol itself.
- The false-sharing ping-pong adds one ownership transfer of the hot line in each direction.
- The average latency delta therefore estimates the extra coherence cost of bouncing that line between CPU 0 and CPU 15.
- The false-sharing stats window should show stronger CHI and overall diagonal-link activity than the control window.

## Conclusion

- Stage 3c passes for this run.
- The ping-pong benchmark reports a positive latency delta between the control and false-sharing phases.
- The false-sharing window also shows stronger coherence traffic than the control window, which matches the expected ownership-bounce behavior.
