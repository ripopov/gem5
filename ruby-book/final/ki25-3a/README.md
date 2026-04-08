# KI25-3A: Single-Core Smoke Test for 4x4 CHI Mesh

## Overview

This directory contains the implementation, simulation, and analysis for **Stage 3a** of the Ruby Book Chapter 17 Final Project: a single-core smoke test to verify that all 16 LLC slices (HN-F controllers) in a 4x4 CHI mesh are reachable.

## Files

| File | Description |
|------|-------------|
| `rbook_test_smoke.c` | Test program - single core allocates 8MB array and accesses every cache line |
| `Makefile` | Build configuration for cross-compiling to RISC-V |
| `ki25-3a_smoke_test_analysis.py` | Python analysis script for simulation results |
| `README.md` | This file |
| `m5out-smoke-*/` | Simulation output directories (auto-generated) |
| `simulation.log` | Raw simulation output log |

## Test Program: rbook_test_smoke.c

### Purpose
Verifies that the 4x4 CHI mesh system boots correctly and that all 16 LLC slices are reachable from a single core through address interleaving.

### How It Works
1. **Allocation**: Core 0 allocates an 8 MB array (2x the total LLC capacity to ensure coverage)
2. **Initialization**: Writes one byte per cache line (64-byte stride) to touch all 131,072 cache lines
3. **Sequential Sweep**: Reads every cache line, causing LLC lookups
4. **Verification**: Prints "PASS" on successful completion

### Key Parameters
- Cache line size: 64 bytes
- LLC slice size: 256 KB (16 slices = 4 MB total)
- Array size: 8 MB (2x coverage)
- Cache line accesses: 131,072

## Building and Running

### 1. Build the Test Program
```bash
make
```

### 2. Run Simulation
```bash
# From repository root
./build/RISCV/gem5.opt \
    -d ruby-book/final/ki25-3a/m5out-smoke-$(date +%Y%m%d-%H%M%S) \
    configs/example/rbook_mesh_config.py \
    --cmd=ruby-book/final/ki25-3a/rbook_test_smoke
```

Or use the provided run script:
```bash
cd ruby-book/final/ki25-3a
./run_simulation.sh
```

### 3. Analyze Results
```bash
python3 ki25-3a_smoke_test_analysis.py [path/to/stats.txt]
```

If no path is provided, the script automatically finds the most recent simulation output.

## Expected Results

### Simulation Output
```
Smoke test: Single-core LLC reachability test
Allocating 8 MB array...
Initializing array with 131072 cache lines...
Reading all cache lines...
Completed 131072 cache line accesses
PASS
```

### Analysis Results

The analysis script validates:

1. **All 16 HN-F controllers receive traffic**
   - Each HN-F should have ~16,485 total accesses (hits + misses)
   - Example: hnf0: 8,216 hits + 8,289 misses = 16,505 total

2. **Balanced distribution**
   - Max deviation from average: < 1% (ideally < 0.5%)
   - Min: 16,476, Max: 16,505, Avg: 16,485.4

3. **Cache hierarchy behavior**
   - L1D hit rate: ~8.6% (streaming access pattern)
   - L2 miss rate: ~100% (cold cache, capacity misses)

## Verification Checklist

- [x] Simulation completes without errors
- [x] Test program prints "PASS"
- [x] All 16 HN-F controllers show nonzero access counts
- [x] HNF access distribution is balanced (max deviation < 25%)
- [x] L2 miss rate is high (expected for cold cache streaming access)

## Results Summary (Sample Run)

```
Simulated time:     0.017528 seconds
Simulated ticks:    17,527,663,500
Instructions:       1,300,430
Host time:          44.25 seconds

HNF Access Distribution:
  All 16 HN-F controllers active
  Min accesses: 16,476 (hnf4, hnf6)
  Max accesses: 16,505 (hnf0)
  Average: 16,485.4
  Deviation: 0.1% (excellent balance)

Cache Hierarchy:
  L1D: 24,835 hits, 263,279 misses (8.6% hit rate)
  L2:  25 hits, 263,767 misses (0.01% hit rate - expected for cold cache)
```

## What This Test Validates

### 1. System Wiring Correctness
- All 16 RN-F (cores) connected to mesh routers
- All 16 HN-F (LLC slices) connected to mesh routers
- 2 SN-F (DDR controllers) at opposite corners (routers 0 and 15)
- MN (Miscellaneous Node) present

### 2. Address Interleaving
- Cache lines distributed evenly across all 16 LLC slices
- Interleaving function working correctly (uses address bits to select HN-F)

### 3. Basic Coherence Protocol
- CHI protocol correctly handles read requests
- Memory responses route properly through mesh

### 4. NoC Topology
- Mesh connectivity allows reachability from any router to any HN-F
- XY routing correctly delivers packets

## Troubleshooting

### Issue: HNF shows zero accesses
**Cause**: Address mapping or router binding misconfiguration
**Fix**: Check `rbook_4x4.py` noc_config - ensure router_list arrays are correct

### Issue: Simulation crashes
**Cause**: Protocol or memory configuration error
**Fix**: Verify gem5 built with CHI protocol; check memory size matches config

### Issue: Unbalanced HNF distribution
**Cause**: Incorrect LLC slice count or interleaving bits
**Fix**: Ensure `--num-l3caches=16` passed; check address interleaving in Ruby.py

## References

- Chapter 17 of the Ruby Book: "Final Project — Build and Simulate a 4×4 CHI Mesh"
- System config: `configs/example/rbook_mesh_config.py`
- NoC config: `configs/example/noc_config/rbook_4x4.py`
- Base classes: `configs/ruby/CHI_config.py`

## Notes

- This is a **smoke test** - validates basic functionality, not performance
- SE (Syscall Emulation) mode - no OS, direct syscall handling
- TimingSimpleCPU models - simple timing for memory system focus
- 2GHz clock domain, 2GHz Ruby clock
- 512 MiB memory size (256 MiB per DDR controller with interleaving)

---

*Generated: April 8, 2026*
