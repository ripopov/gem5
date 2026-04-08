# KI25-3A Deliverables Index

## Assignment
**Chapter 17, Stage 3a** - Single-core smoke test for 4x4 CHI mesh
**Directory:** `ruby-book/final/ki25-3a/`
**Status:** ✅ Complete

## File Inventory

### Core Files

| File | Type | Description | Lines |
|------|------|-------------|-------|
| `rbook_test_smoke.c` | Source | Test program - single core exercises 16 LLC slices | ~100 |
| `rbook_test_smoke` | Binary | Compiled RISC-V executable | - |
| `Makefile` | Build | Cross-compilation rules | ~15 |
| `ki25-3a_smoke_test_analysis.py` | Script | Simulation results analyzer | ~250 |
| `run_smoke_test.sh` | Script | Convenient simulation launcher | ~50 |

### Documentation

| File | Description |
|------|-------------|
| `README.md` | Complete user guide with build/run/analyze instructions |
| `SUMMARY.md` | Final report with detailed results and validation |
| `QUICKREF.md` | Quick reference card for common operations |
| `INDEX.md` | This file - deliverables index |

### Simulation Outputs

| Directory | Contents |
|-----------|----------|
| `m5out-smoke-v2/` | Latest simulation results |
| ├─ `stats.txt` | gem5 statistics (2M+ lines) |
| ├─ `config.ini` | System configuration dump |
| ├─ `config.dot` | Topology graph (DOT format) |
| ├─ `config.dot.svg` | Topology diagram (SVG) |
| ├─ `analysis_report.txt` | Generated analysis report |
| └─ `citations.bib` | References |
| `m5out-smoke-20260408-115248/` | Previous run (for comparison) |
| `simulation_v2.log` | Console output from latest run |
| `simulation.log` | Console output from first run |

## Test Summary

### What Was Tested
- **Goal:** Verify all 16 LLC slices (HN-F controllers) are reachable from single core
- **Method:** Sequential access to 131,072 cache lines across 8 MB array
- **Expected:** Each HN-F receives ~8,192 accesses (balanced distribution)

### Results

✅ **ALL VALIDATION CHECKS PASSED**

| Metric | Target | Actual | Status |
|--------|--------|--------|--------|
| HNF controllers active | 16 | 16 | ✅ |
| Min accesses per HNF | >0 | 16,461 | ✅ |
| Max deviation | <25% | 0.2% | ✅ |
| Test program output | "PASS" | "PASS" | ✅ |
| Simulation completion | No errors | Clean exit | ✅ |

### Statistics

- **Simulated time:** 0.017283 seconds
- **Instructions:** 1,312,662
- **Total HNF accesses:** 263,869
- **Average per HNF:** 16,491.8
- **Host time:** 44.74 seconds

## Key Achievements

1. ✅ Test program compiles cleanly (no warnings)
2. ✅ Simulation runs successfully with CHI protocol
3. ✅ All 16 LLC slices receive traffic
4. ✅ Access distribution is balanced (0.2% deviation)
5. ✅ Analysis script correctly parses stats and validates
6. ✅ Documentation is complete (README, SUMMARY, QUICKREF)
7. ✅ Build and run automation scripts provided

## Quick Validation

```bash
cd ruby-book/final/ki25-3a

# Verify files exist
ls -la rbook_test_smoke.c ki25-3a_smoke_test_analysis.py README.md

# Build test
make

# Run simulation (optional - takes ~45 seconds)
./run_smoke_test.sh

# Analyze results
python3 ki25-3a_smoke_test_analysis.py
```

## Code Quality Highlights

### Test Program (`rbook_test_smoke.c`)
- Clear phase structure (allocation, initialization, access)
- Extensive comments explaining the test methodology
- Proper error handling (malloc failure check)
- No magic numbers - uses defined constants
- Volatile keyword prevents compiler optimization
- Diagnostic output for debugging

### Analysis Script (`ki25-3a_smoke_test_analysis.py`)
- Modular design with separate functions
- Type hints for better code clarity
- Comprehensive validation logic
- Auto-detection of simulation outputs
- Both console and file output
- Clear PASS/FAIL indicators

## Dependencies

- `riscv64-linux-gnu-gcc` (RISC-V cross-compiler)
- `python3` (for analysis script)
- `gem5` built with CHI protocol (`build/RISCV/gem5.opt`)
- Existing configuration files:
  - `configs/example/rbook_mesh_config.py`
  - `configs/example/noc_config/rbook_4x4.py`

## Next Steps (for user)

1. Review `README.md` for detailed instructions
2. Check `SUMMARY.md` for complete results
3. Use `QUICKREF.md` for day-to-day operations
4. Proceed to Stage 3b (hop latency test) if desired

## Contact & References

- **Assignment:** Chapter 17, Stage 3a of Ruby Book
- **System:** 4x4 CHI mesh with Garnet network
- **Protocol:** CHI (Coherent Hub Interface)
- **Architecture:** 16 RISC-V cores, 16 LLC slices, 2 DDR controllers

---

*Deliverables complete and validated - April 8, 2026*
