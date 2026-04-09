# Stage 3b Plan: `rbook_test_hop_latency.c`

## Problem Statement

Implement Stage 3b from `ruby-book/Ch17_FinalProject.md`:

- Add `ruby-book/final/hop_latency/rbook_test_hop_latency.c`.
- Run it on the existing 16-core CHI 4x4 mesh.
- Confirm the chapter's expectations:
  - far HN-F loads are measurably slower than near HN-F loads
  - the extra latency is roughly the expected multi-hop network cost
  - isolated network stats show local traffic for the near case and diagonal
    mesh traffic for the far case

## Current Repo State

- The Chapter 17 config already exists:
  - `configs/example/rbook_mesh_config.py`
  - `configs/example/noc_config/rbook_4x4.py`
- The Stage 3a smoke test already exists:
  - `ruby-book/final/rbook_test_smoke.c`
  - `ruby-book/final/check_smoke.py`
- `build/RISCV/gem5.opt` already exists, so no simulator build is required
  before running Stage 3b.
- The Stage 3b sources now live under `ruby-book/final/hop_latency/`.
- The parent `ruby-book/final/Makefile` delegates the Stage 3b build into that
  dedicated subdirectory.

## Key Technical Findings

### Address selection is deterministic

- HN-F selection is physical address bits `[9:6]` in `CHI_config.py`.
- Those bits are just the cache-line offset within a 4 KiB page.
- That means one page-aligned 4 KiB target page is enough to pick exact HN-Fs:
  - HNF 0: offset `0x000`
  - HNF 15: offset `0x3c0` (`15 * 64`)
- This avoids any dependence on unknown physical page frame allocation.

Relevant refs:

- `ruby-book/extra/SEModeIntro.md:443-489`
- `configs/ruby/CHI_config.py:636-653`

### The chapter's example eviction size is too small for this config

- Current cache defaults from `configs/common/Options.py`:
  - `L1D = 64KiB`
  - `L2 = 2MiB`
  - `L3 = 16MiB`
- So the `512KiB` example eviction sweep in `SEModeIntro.md` is not enough
  here.
- A `4MiB` eviction sweep is a good starting point:
  - large enough to evict private `L1/L2`
  - still small relative to LLC capacity
- Because addresses are interleaved across 16 HN-F slices, a `4MiB`
  sequential sweep spreads to about `256KiB` per LLC slice, which is far below
  `16MiB`.

Relevant refs:

- `configs/common/Options.py:186-199`
- `configs/ruby/CHI.py:105-109`
- `ruby-book/extra/SEModeIntro.md:658-679`

### `m5_reset_stats` / `m5_dump_reset_stats` are feasible

- RISC-V `m5ops` work via pseudo-instructions.
- No config change to `rbook_mesh_config.py` is needed.
- This lets the benchmark create isolated stats windows for a near-only probe
  and a far-only probe.

Relevant refs:

- `util/m5/src/abi/riscv/m5op.S:39-55`
- `src/arch/riscv/isa/formats/m5ops.isa:39-55`
- `src/sim/pseudo_inst.hh:189-199`

### Whole-run link stats will be misleading unless we isolate them

- A cache-eviction sweep generates a lot of normal demand traffic.
- If we only inspect the final cumulative `stats.txt`, the sweep will swamp
  the path-specific evidence we want.
- The benchmark should therefore separate:
  - repeated timed loads for latency averages
  - single isolated probes bracketed by stats reset/dump for link analysis

### Useful path/link IDs are known

From `CustomMesh.py` link creation order and the saved Chapter 17 config:

- `ext_links48` = `hnf00`
- `ext_links63` = `hnf15`

Request path for router `0 -> 15` under XY routing:

- `int_links00`, `01`, `02`
- `int_links33`, `34`, `35`

Relevant refs:

- `configs/topologies/CustomMesh.py:73-163`
- saved config mapping in
  `m5out/rbook-latency-test-20260409-202044/config.ini`

## Pitfalls To Avoid

- Do not use `cbo.flush` or other privileged cache-management instructions.
- Do not rely on virtual address identity mapping.
- Do not use an unaligned target buffer if the goal is deterministic HN-F
  selection.
- Do not use a whole-run stats file as proof of path behavior.
- Do not assume gem5 will leave a `simout` file in `m5out`; current runs do
  not show one, so the run command should capture stdout to `console.log`.
- Do not make the eviction sweep so large that it turns the test into an LLC or
  DRAM miss benchmark.
- Do not hardcode a brittle latency threshold before seeing one real run.

## Final Solution Architecture

### 1. Benchmark binary: `ruby-book/final/hop_latency/rbook_test_hop_latency.c`

The benchmark will be single-threaded and run only on core 0.

It will contain:

- one page-aligned `target_page[4096]`
- one `4MiB` `eviction_buffer`
- an inline `rdcycle()` helper
- a `flush_private_caches()` sweep over `eviction_buffer`
- timed loads for:
  - near line at `target_page + 0x000`
  - far line at `target_page + 0x3c0`
- a warm-up step that loads both target lines once before measurement so later
  timed loads should hit in LLC, not DRAM
- output in a machine-parsable format, for example:
  - `NEAR_SAMPLES ...`
  - `FAR_SAMPLES ...`
  - `NEAR_AVG ...`
  - `FAR_AVG ...`
  - `DELTA_AVG ...`

It will also do two extra isolated probes:

- `near` isolated probe:
  - flush private caches
  - `m5_reset_stats(0, 0)`
  - load near line once
  - `m5_dump_reset_stats(0, 0)`
- `far` isolated probe:
  - flush private caches
  - `m5_reset_stats(0, 0)`
  - load far line once
  - `m5_dump_reset_stats(0, 0)`

This produces separate stats blocks for the helper script.

### 2. Build integration: `ruby-book/final/Makefile`

Update the Makefile to:

- add `rbook_test_hop_latency` to `BINS`
- compile the new test with:
  - include path to `../../include`
  - RISC-V m5ops assembly `../../util/m5/src/abi/riscv/m5op.S`

I recommend using the assembly wrapper directly instead of adding a `libm5`
build dependency.

### 3. Analysis helper: `ruby-book/final/hop_latency/check_hop_latency.py`

Because you asked for a reusable checker, add a script that accepts:

```text
python3 ruby-book/final/hop_latency/check_hop_latency.py \
    <m5out-dir> <console-log>
```

The script will:

- parse `<console-log>` for:
  - `NEAR_AVG`
  - `FAR_AVG`
  - `DELTA_AVG`
- parse `<m5out-dir>/stats.txt`
- split `stats.txt` into stats blocks using:
  - `---------- Begin Simulation Statistics ----------`
  - `---------- End Simulation Statistics ----------`
- treat:
  - first dumped block as isolated near probe
  - second dumped block as isolated far probe

The script should report PASS/FAIL and print the key counters it used.

## Implementation Strategy

### Step 1. Add the benchmark source

Use:

- `volatile` accesses so the compiler emits real loads
- rare `asm volatile("" ::: "memory")` barriers around timed loads if needed
- page alignment for the target page
- cache-line stride `64`

Start with:

- `SAMPLES = 8` or `16`
- `EVICT_BYTES = 4 * 1024 * 1024`

### Step 2. Update the Makefile

Minimal change:

- keep the existing targets
- add one target for `rbook_test_hop_latency`
- add shared variables for gem5 include path and m5ops source

### Step 3. Add the checker script

The checker should enforce:

- `far_avg > near_avg`
- `delta_avg` is clearly positive and reasonably close to the chapter
  expectation
- isolated near probe shows local behavior
- isolated far probe shows far-path mesh behavior

I recommend making the latency threshold conservative at first:

- hard fail if `far_avg <= near_avg`
- hard fail if `delta_avg` is too small to support the hop-distance claim
- report deviation from the nominal `~96 cycles` without over-tightening until
  we see one real run

## Testing And Run Strategy

### Build

```bash
make -C ruby-book/final/hop_latency rbook_test_hop_latency
```

### Run

Capture console output explicitly:

```bash
outdir="m5out/rbook-hop-latency-$(date +%Y%m%d-%H%M%S)"
./build/RISCV/gem5.opt -d "$outdir" \
    configs/example/rbook_mesh_config.py \
    --cmd=ruby-book/final/hop_latency/rbook_test_hop_latency \
    > "$outdir/console.log" 2>&1
```

### Check

```bash
python3 ruby-book/final/hop_latency/check_hop_latency.py \
    "$outdir" "$outdir/console.log"
```

### Manual spot-check after the script

Look for:

- `NEAR_AVG`
- `FAR_AVG`
- `DELTA_AVG`

Then spot-check `stats.txt`:

- near isolated block:
  - `ext_links48` active
  - `ext_links63` inactive
  - `int_links00..47` mesh-hop links inactive
- far isolated block:
  - `ext_links63` active
  - request-path links active:
    - `int_links00`
    - `int_links01`
    - `int_links02`
    - `int_links33`
    - `int_links34`
    - `int_links35`

For the first version of the checker, I would make those request-path links
hard checks and keep return-path link checks as reported evidence rather than
hard failures until we see the exact real counters.

## Acceptance Criteria

The implementation is successful if all of these hold:

- `make -C ruby-book/final rbook_test_hop_latency` succeeds.
- gem5 run completes successfully.
- console output shows `far_avg > near_avg`.
- measured delta is meaningfully positive and broadly consistent with the
  chapter's `~96-cycle` extra round-trip network cost.
- isolated near stats show no mesh-hop inter-router traffic.
- isolated far stats show nonzero traffic on the expected XY mesh path to
  router 15.
- the checker script returns PASS with a clear summary.

## Likely Failure Diagnosis If Results Deviate

- If `delta_avg` is too small:
  - the eviction sweep is not evicting `L2`
  - increase eviction size or make the sweep less cache-friendly
- If `delta_avg` is much too large:
  - the target is being evicted from LLC and the test is measuring DRAM service
    too
  - reduce sweep size or re-check the warm-up strategy
- If near isolated stats show mesh-hop traffic:
  - stats reset is happening too early
  - some unrelated traffic is still inside the measured window
- If far isolated stats do not show the expected path:
  - the script is reading the wrong stats block
  - the benchmark output markers and block ordering need adjustment
- If `m5ops` calls do not link:
  - the Makefile path to `util/m5/src/abi/riscv/m5op.S` is wrong

## Recommended Next Execution Pass

When implementation starts, the work should proceed in this order:

1. Add `rbook_test_hop_latency.c`.
2. Update `ruby-book/final/hop_latency/Makefile`.
3. Add `ruby-book/final/hop_latency/check_hop_latency.py`.
4. Build the test binary.
5. Run the simulation with `console.log` capture.
6. Run the checker.
7. If the first run is close but noisy, tune only:
   - sample count
   - eviction size
   - checker thresholds
