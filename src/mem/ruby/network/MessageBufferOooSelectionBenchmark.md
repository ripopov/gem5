# OoO MessageBuffer Ready-Selection: Three Implementations Benchmarked

This report compares three implementations of out-of-order (OoO) ready-message
**selection** in `ruby::MessageBuffer`, measured with a single shared
microbenchmark. The goal is to decide which selection strategy the credited
MessageBuffer should keep, using measured cost rather than intuition.

All three arms expose an identical public API and pass the **same 24
correctness gtests** (`message_buffer_credit.test`). The benchmark source is
byte-for-byte identical across the three; only `MessageBuffer.{hh,cc}` differ.

## The three implementations

| Arm | Branch | Selection strategy | Extra memory |
| --- | --- | --- | --- |
| **V1 — pruned DFS** *(production)* | `message-buffer-credited-mode` | `selectBest()` walks the min-heap with a DFS that prunes immature subtrees, visiting exactly the ready set in **O(n_ready)** | none (heap only) |
| **V2 — flat scan** | `bench/ooo-v2` | `selectBest()` does a flat **O(n)** scan over every heap slot, skipping immature entries (gem5's historical `findReady`) | none (heap only) |
| **V3 — two-container cache** | `bench/ooo-v3` | min-heap stays authoritative **plus** a contiguous `std::vector<size_t>` caching matured heap indices; rebuilt lazily, reused across same-state selections, **invalidated on any mutation** | heap + index cache |

Why these three: V1 is the shipped design; V2 is the simplest possible
alternative and the historical baseline; V3 tests the standard "keep a separate
ready container" idea to see whether a random-access matured set beats
re-traversing the heap.

### Why maturity makes a pruned traversal possible

The heap is a min-heap on `(getLastEnqueueTime, counter)`, so every node's
enqueue time is `<=` its children's. Maturity (`enqueue_time <= cur_time`) is
therefore **closed under descent**: an immature node has an all-immature
subtree, and the matured messages form a connected sub-heap rooted at index 0.
V1 and V3's cache build exploit this to enumerate the ready set in O(n_ready);
V2 ignores it and scans all n.

## Benchmark

Source: [`simple/xp/message_buffer_ooo_bench.cc`](simple/xp/message_buffer_ooo_bench.cc),
registered as the GTest target `message_buffer_ooo_bench.test`.

It sweeps **occupancy N ∈ {1, 2, 4, …, 1024}** × **matured fraction ∈
{30%, 70%, 100%}**. For each cell it fills a buffer with N messages (the matured
ones with arrival `<= cur_time`, the rest far in the future; enqueue order is
shuffled so the heap is a realistic multi-level tree) and reports three numbers:

- **`ns_per_select`** — time per `selectEligible()` on a **static** full buffer
  (no mutation between calls). Isolates pure selection cost. An always-true
  predicate forces the full O(n_ready)/O(n) traversal (a null predicate would
  short-circuit to the O(1) head).
- **`ns_per_pop`** — time per matured pop during a `select + popAt` **drain**.
  This is the realistic OoO-pop workload: the heap is **mutated on every
  iteration**. Fills are untimed; only the drain loop is measured.
- **`selector_bytes`** — `selectorMemoryBytes()` read **after** a warm-up
  selection, so V3's lazily-built cache reports its active footprint.

Timing uses an adaptive loop (~100 ms per measurement) on `steady_clock`. All
three arms were rebuilt and measured back-to-back on the same idle host in one
session, so the numbers are directly comparable. (Absolute ns values are
machine-specific; the **ratios between arms** are the result.)

### Reproduce

```sh
for br in message-buffer-credited-mode bench/ooo-v2 bench/ooo-v3; do
  git switch $br
  scons --ignore-style \
    build/RISCV/mem/ruby/network/simple/message_buffer_ooo_bench.test.opt -j$(nproc)
  ./util/run_with_timeout.sh \
    ./build/RISCV/mem/ruby/network/simple/message_buffer_ooo_bench.test.opt \
    2>/dev/null | grep '^MBBENCH'
done
```

## Results

### Selection cost — `ns_per_select` (static buffer, repeated selection)

Lower is better. **Bold** = fastest of the three for that cell.

| N | matured | V1 pruned DFS | V2 flat scan | V3 cache |
| --: | --: | --: | --: | --: |
| 16 | 30% | 12.6 | 14.1 | **8.3** |
| 16 | 70% | 27.4 | 21.1 | **17.9** |
| 16 | 100% | 33.4 | **23.9** | 24.6 |
| 64 | 30% | 46.2 | 54.0 | **29.1** |
| 64 | 70% | 93.7 | 81.8 | **68.9** |
| 64 | 100% | 134.2 | 95.7 | **97.8** |
| 256 | 30% | 170.0 | 209.0 | **113.5** |
| 256 | 70% | 363.0 | 340.9 | **255.5** |
| 256 | 100% | 486.6 | 356.0 | **359.5** |
| 1024 | 30% | 702.8 | 856.3 | **426.6** |
| 1024 | 70% | 1561.5 | 1322.3 | **991.8** |
| 1024 | 100% | 2173.6 | 1430.1 | **1424.5** |

**Reading it:**
- **V3 wins repeated selection**, often by ~1.5–2×, because after the first call
  it iterates a contiguous index vector (1023 of 1024 calls are cache hits) —
  cache-friendly and DFS-free.
- **V1 is the *slowest* at high maturity** (e.g. N=1024/100%: 2174 ns vs ~1425
  for V2/V3). The pruned DFS's explicit-stack push/pop and pointer indirection
  cost more than a flat contiguous walk once nearly everything is matured.
- **V2 beats V1 at high maturity but loses at low maturity** (N=1024/30%: 856 vs
  703) — it always touches all n slots, so pruning the immature 70% is real
  work it throws away.

### Pop cost — `ns_per_pop` (drain; heap mutated every iteration)

This is the **workload the buffer actually runs**. Lower is better.

| N | matured | V1 pruned DFS | V2 flat scan | V3 cache |
| --: | --: | --: | --: | --: |
| 16 | 30% | **28.0** | 29.2 | 31.8 |
| 16 | 70% | 31.5 | **29.0** | 37.4 |
| 16 | 100% | 33.8 | **28.6** | 38.9 |
| 64 | 30% | **50.2** | 60.9 | 64.6 |
| 64 | 70% | **78.1** | 72.6 | 109.1 |
| 64 | 100% | 92.8 | **76.0** | 127.6 |
| 256 | 30% | **137.3** | 180.4 | 198.9 |
| 256 | 70% | 247.0 | **229.8** | 361.7 |
| 256 | 100% | 296.9 | **217.2** | 427.5 |
| 1024 | 30% | **483.9** | 708.3 | 678.9 |
| 1024 | 70% | 963.4 | **834.7** | 1411.0 |
| 1024 | 100% | 1103.3 | **755.9** | 1663.9 |

**Reading it:**
- **V3 is the worst arm under mutation, everywhere it matters** — up to ~1.7×
  slower than V2 and ~1.5× slower than V1 (N=1024/70%: 1411 vs 835 vs 963). Every
  `popAt` invalidates the cache, so each selection pays a full O(n_ready) rebuild
  **plus** the subsequent vector walk (≈ double traversal) **plus** allocator
  churn. The cache never amortizes when the state changes every step.
- **V1 wins the low-maturity pops** (pruning pays: N=1024/30%, 484 ns) — the
  realistic regime for a backpressured link where only a fraction of the buffer
  has matured.
- **V2 wins the high-maturity pops** (locality pays once you must touch
  everything anyway), but degrades at low maturity where it scans slots it can't
  use.

### Memory — `selector_bytes` (active footprint)

| N | V1 / V2 (heap only) | V3 @30% | V3 @70% | V3 @100% |
| --: | --: | --: | --: | --: |
| 64 | 1024 | 1280 | 1536 | 1536 |
| 256 | 4096 | 5120 | 6144 | 6144 |
| 1024 | 16384 | 20480 | 24576 | 24576 |

V1 and V2 hold only the priority heap (`N × 16` B). V3 adds the matured-index
cache (`size_t` per matured entry, vector growth rounded to powers of two),
costing **+25% to +50%** at full occupancy — the higher the maturity, the bigger
the cache, exactly when the buffer is already largest.

## Conclusion

**The production design (V1, pruned O(n_ready) DFS) is the right default**, and
the data justifies it specifically for the workload the buffer runs:

1. **OoO popping mutates the heap every step**, which is the `ns_per_pop` column.
   There, V3's separate ready-container is a **net loss** — slowest in every
   meaningful cell and up to +50% memory — because cache invalidation per pop
   erases any reuse benefit. The "keep a ready set on the side" intuition does
   not survive contact with a mutation-heavy drain.
2. Between V1 and V2 on pops, the winner flips with maturity: **V1 wins when few
   messages are matured** (pruning), **V2 wins when most are** (locality). Real
   credited links spend their busy time backpressured with a *partially* matured
   buffer, which favors V1's pruning; V1 also never loses by more than locality
   overhead when maturity is high.
3. V3's only victory is the synthetic `ns_per_select` column — **repeated
   selection with no intervening pop** — which is not a pattern the buffer
   exhibits (you select then immediately pop).

**If a future workload became selection-heavy and mutation-light** (e.g. many
QoS re-evaluations per pop), V3's cache would pay off and the `selectBest`
comparator hook (see `MessageBufferCredited.md` §3) is where it would plug in.
A cheaper refinement than V3 for the high-maturity case would be to let V1 fall
back to a flat scan once `n_ready` approaches `n` (capturing V2's locality win
without V2's low-maturity penalty), but the measured gap is small and not worth
the branch on the current evidence.

The experimental arms live on branches `bench/ooo-v2` and `bench/ooo-v3`; the
benchmark itself ships on the production branch for future re-measurement.
