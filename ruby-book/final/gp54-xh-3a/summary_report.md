# Stage 3a Smoke Test Report

Run directory: `/home/ripopov/work/riscv/gem5/ruby-book/final/gp54-xh-3a/m5out-smoke-20260408-120208`
Measurement block: 1 of 2
PASS observed in run log: yes
Exit status: `Exiting @ tick 25521964500 because exiting with last active thread context`
Workload output: `PASS array_bytes=8388608 lines=131072 passes=2 slice_bytes=524288`

## Summary

- L3 slices with nonzero demand accesses: 16/16
- Total L3 demand accesses: 262,169
- Per-slice average: 16,385.56 accesses
- Per-slice min/max: 16,384 / 16,403
- Max-min spread: 0.12% of the mean
- Result: all 16 HN-F slices were reached by the sequential sweep.
- Balance check: the slice totals are close enough to call the distribution roughly balanced for this smoke test.

## Per-Slice Demand Accesses

| Slice | Hits | Misses | Total |
| --- | ---: | ---: | ---: |
| 0 | 16,312 | 72 | 16,384 |
| 1 | 16,302 | 82 | 16,384 |
| 2 | 16,314 | 70 | 16,384 |
| 3 | 16,309 | 75 | 16,384 |
| 4 | 16,299 | 86 | 16,385 |
| 5 | 16,302 | 82 | 16,384 |
| 6 | 16,314 | 70 | 16,384 |
| 7 | 16,306 | 78 | 16,384 |
| 8 | 16,304 | 80 | 16,384 |
| 9 | 16,314 | 75 | 16,389 |
| 10 | 16,322 | 81 | 16,403 |
| 11 | 16,297 | 87 | 16,384 |
| 12 | 16,313 | 71 | 16,384 |
| 13 | 16,310 | 74 | 16,384 |
| 14 | 16,314 | 70 | 16,384 |
| 15 | 16,311 | 73 | 16,384 |
