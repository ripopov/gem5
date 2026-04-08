# KI25-3A Smoke Test - Final Summary Report

**Date:** April 8, 2026
**Assignment:** Chapter 17, Stage 3a - Single-core Smoke Test
**Location:** `ruby-book/final/ki25-3a/`

## Executive Summary

Successfully implemented and validated the single-core smoke test for the 4x4 CHI mesh system. All 16 LLC slices (HN-F controllers) are confirmed reachable with balanced access distribution.

### Test Results: ✅ PASS

- **All 16 HN-F controllers** received traffic
- **Access distribution:** Balanced (max deviation 0.2%)
- **Simulation time:** 0.017 seconds simulated, 44.74 seconds host
- **Instructions executed:** 1,312,662

## Deliverables

### 1. Test Program (`rbook_test_smoke.c`)
A single-core RISC-V binary that:
- Allocates an 8 MB array (2x total LLC capacity)
- Performs sequential write-read access to all 131,072 cache lines
- Exercises address interleaving across all 16 HN-F slices
- Outputs diagnostic information and PASS/FAIL status

**Key features:**
- Well-commented with phase breakdowns
- Validates minimum access thresholds
- Clear error reporting
- Static linking for SE mode compatibility

### 2. Build System (`Makefile`)
Cross-compilation setup using `riscv64-linux-gnu-gcc`:
```bash
make          # Build test binary
make clean    # Clean build artifacts
```

### 3. Run Script (`run_smoke_test.sh`)
Convenient simulation launcher:
```bash
./run_smoke_test.sh [suffix]  # Run with optional output suffix
```

### 4. Analysis Script (`ki25-3a_smoke_test_analysis.py`)
Comprehensive Python analysis tool that:
- Parses gem5 stats.txt files
- Extracts per-HNF controller statistics
- Validates balanced access distribution
- Generates detailed reports
- Reports PASS/FAIL status

**Usage:**
```bash
python3 ki25-3a_smoke_test_analysis.py [path/to/stats.txt]
```

### 5. Documentation (`README.md`)
Complete user guide covering:
- Test purpose and methodology
- Build and run instructions
- Expected results and interpretation
- Troubleshooting guide
- Architecture overview

## Technical Details

### System Configuration
- **Topology:** 4x4 Garnet mesh (16 routers)
- **Nodes:** 16 RN-F (cores), 16 HN-F (LLC slices), 2 SN-F (DDR), 1 MN
- **DDR Placement:** Routers 0 and 15 (diagonally opposite corners)
- **Protocol:** CHI (Coherent Hub Interface)
- **Network:** Garnet with XY routing
- **Memory:** 512 MiB total (256 MiB per DDR, interleaved)

### Address Interleaving
With 16 HN-F slices and 64-byte cache lines, addresses distribute via:
- Lower 6 bits: cache line offset
- Upper bits: select HN-F via interleaving
- Result: Consecutive cache lines map to different HN-Fs

### Simulation Parameters
```python
num_cpus=16
num_l3caches=16
num_dirs=2
topology="CustomMesh"
network="garnet"
cpu_type="RiscvTimingSimpleCPU"
mem_size="512MiB"
```

## Test Execution Results

### Simulation Output
```
Smoke test: Single-core LLC reachability test
System: 4x4 CHI mesh with 16 HN-F (LLC) + 2 SN-F (DDR)
Test: Core 0 accesses 131072 cache lines across 8 MB array
Expected: ~8192 accesses per HN-F slice

Phase 1: Initializing array (write every cache line)...
  Wrote 131072 cache lines

Phase 2: Reading all cache lines (LLC lookup test)...
  Read 131072 cache lines

Validation:
  Total cache line accesses: 131072
  Expected per HN-F slice:   ~8192
  Minimum for pass:          4096

PASS
All 16 LLC slices should show nonzero access counts in stats.
```

### Analysis Results

| Metric | Value |
|--------|-------|
| Simulated Time | 0.017283 seconds |
| Simulated Ticks | 17,283,180,000 |
| Instructions | 1,312,662 |
| Host Time | 44.74 seconds |

**HNF Access Distribution:**

| HNF | Hits | Misses | Total | % of Avg |
|-----|------|--------|-------|----------|
| hnf0 | 8,218 | 8,285 | 16,503 | 100.1% |
| hnf1 | 8,216 | 8,286 | 16,502 | 100.1% |
| hnf2 | 8,217 | 8,293 | 16,510 | 100.1% |
| hnf3 | 8,217 | 8,294 | 16,511 | 100.1% |
| hnf4 | 8,221 | 8,279 | 16,500 | 100.0% |
| hnf5 | 8,215 | 8,283 | 16,498 | 100.0% |
| hnf6 | 8,209 | 8,273 | 16,482 | 99.9% |
| hnf7 | 8,211 | 8,269 | 16,480 | 99.9% |
| hnf8 | 8,201 | 8,260 | 16,461 | 99.8% |
| hnf9 | 8,207 | 8,281 | 16,488 | 100.0% |
| hnf10 | 8,211 | 8,275 | 16,486 | 100.0% |
| hnf11 | 8,213 | 8,278 | 16,491 | 100.0% |
| hnf12 | 8,211 | 8,277 | 16,488 | 100.0% |
| hnf13 | 8,212 | 8,276 | 16,488 | 100.0% |
| hnf14 | 8,210 | 8,278 | 16,488 | 100.0% |
| hnf15 | 8,211 | 8,281 | 16,492 | 100.0% |

**Statistics:**
- Total HNF accesses: 263,869
- Average per HNF: 16,491.8
- Minimum: 16,461 (hnf8)
- Maximum: 16,511 (hnf3)
- Max deviation: 0.2% (excellent balance)

### Cache Hierarchy Analysis

**L1 Data Cache:**
- Hits: 29,264
- Misses: 263,309
- Hit rate: 10.0%

**L2 Cache:**
- Hits: 22
- Misses: 263,869
- Miss rate: 99.99%

*Note: High L2 miss rate is expected because this is a streaming access pattern with no temporal locality - every access misses to LLC.*

## Validation Criteria

### ✅ All Checks Passed

1. **System Boot:** Simulation completed without errors
2. **Test Program:** Printed "PASS" and exited normally
3. **HNF Coverage:** All 16 controllers show nonzero access counts
4. **Distribution Balance:** Max deviation < 1% (threshold: 25%)
5. **Cache Behavior:** L2 miss rate high as expected for cold cache streaming

## Code Quality Assessment

### Test Program (`rbook_test_smoke.c`)
- **Clarity:** Well-documented with extensive comments
- **Structure:** Phased execution (init + read) with progress reporting
- **Robustness:** Error checking for malloc failure
- **Maintainability:** Clear constants, no magic numbers
- **Standards:** Follows C conventions, clean formatting

### Analysis Script (`ki25-3a_smoke_test_analysis.py`)
- **Modularity:** Separate functions for parsing, validation, reporting
- **Error Handling:** Graceful handling of missing files/invalid data
- **Flexibility:** Auto-detects stats files or accepts explicit paths
- **Output:** Both console and file output, well-formatted
- **Validation:** Comprehensive checks with clear PASS/FAIL indicators

## Files Generated

```
ruby-book/final/ki25-3a/
├── rbook_test_smoke.c           # Test source code
├── rbook_test_smoke             # Compiled binary (RISC-V)
├── Makefile                     # Build configuration
├── ki25-3a_smoke_test_analysis.py  # Analysis script
├── run_smoke_test.sh            # Simulation launcher
├── README.md                    # Complete documentation
├── SUMMARY.md                   # This file
├── simulation_v2.log            # Simulation output log
└── m5out-smoke-v2/
    ├── stats.txt                # gem5 statistics
    ├── config.ini               # System configuration
    ├── config.dot               # Topology visualization
    ├── config.dot.svg           # Topology diagram
    └── analysis_report.txt      # Generated analysis report
```

## How to Reproduce

1. **Build the test:**
   ```bash
   cd ruby-book/final/ki25-3a
   make
   ```

2. **Run simulation:**
   ```bash
   ./run_smoke_test.sh
   ```

3. **Analyze results:**
   ```bash
   python3 ki25-3a_smoke_test_analysis.py
   ```

## Conclusion

The single-core smoke test successfully validates the 4x4 CHI mesh configuration. All 16 LLC slices are reachable and receive balanced traffic, confirming correct address interleaving and mesh connectivity. The test infrastructure (build system, run scripts, analysis tools) is complete and ready for the next stages of Chapter 17.

**Status:** ✅ Complete and validated

---

*Report generated: April 8, 2026*
*Test framework version: KI25-3A*
