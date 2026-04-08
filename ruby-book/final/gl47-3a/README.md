# Smoke Test (Stage 3a) - Implementation Summary

## Overview
Successfully implemented and validated the single-core smoke test for the 4×4 CHI mesh system. The test verifies that all 16 LLC slices are reachable from a single core and that the address distribution is balanced.

## Files Created

### 1. Test Program: `rbook_test_smoke.c`
- Allocates a 1 MiB array (16,384 cache lines)
- Initializes array with a pattern (0-255 repeating)
- Sequentially reads every cache line
- Verifies correct number of reads and sum (accounting for uint8_t overflow)
- Prints "PASS" on success

**Key Design Decisions:**
- Array size: 1 MiB (originally 256 MiB, reduced for faster simulation)
  - Original 256 MiB (4,194,304 cache lines) was too slow (took >5 minutes)
  - Revised 1 MiB (16,384 cache lines) completes in ~2.1B ticks (~3 seconds)
  - Still sufficient to generate ~1,100 accesses per HNF slice for balance verification

**Simulation Performance:**
- 263,480 committed instructions
- 4,256,799 simulation cycles (2.13B ticks at 1THz clock)
- IPC: 0.0619 (intentionally low due to cold cache misses and printf I/O)

### 2. Analysis Script: `analyze_smoke_test.py`
Python script that parses gem5 statistics and generates a comprehensive report:
- CPU Statistics: instructions, cycles, IPC, L1/L2 cache hit rates
- LLC Slice (HN-F) Distribution: accesses per slice, hit rate, balance metrics
- DRAM Controller Statistics: read/write requests per DDR controller
- Garnet Network Statistics: total flits/packets
- Final summary with PASS/FAIL verdict

**Statistics Paths Discovered:**
- HN-F controllers: `system.ruby.hnf{i}.cntrl.cache.m_demand_hits/misses`
- DRAM controllers: `system.mem_ctrls0/1.readReqs/writeReqs`
- CPU instructions: `system.cpu0.commitStats0.numInsts`
- CPU cycles: `system.cpu0.numCycles`

### 3. Makefile
Build and run automation:
- `make rbook_test_smoke` - Build the test binary
- `make run-smoke` - Run gem5 simulation
- `make analyze` - Analyze statistics
- `make test` - Run simulation + analysis
- `make clean` - Clean build artifacts

## Results

### Test Execution
```
Smoke Test: Verifying all 16 LLC slices are reachable
Array size: 1 MiB (16,384 cache lines)
Reading 16,384 cache lines sequentially...
Read 16384 cache lines
Sum verification correct: 2088960
Verification successful!
PASS
```

### LLC Slice Access Distribution
- Total accesses: 17,627
- Average per slice: 1,101.69
- Range: 1,090 - 1,113 (min - max)
- Coefficient of variation: 0.0049 (0.49%)
- ✓ All 16 slices were accessed
- ✓ Well balanced (CV < 5%)

### DRAM Controller Balance
- DDR0 (router 0): 8,815 reads (50.01%)
- DDR1 (router 15): 8,812 reads (49.99%)
- ✓ Well balanced (imbalance < 0.1%)

### CPU and Cache Statistics
- Committed instructions: 263,480
- Simulation cycles: 4,256,799
- IPC: 0.0619
- L1I hit rate: 99.88%
- L1D hit rate: 38.65% (expected for streaming, cold misses to DRAM)
- L2 hits: 16,483
- L2 misses: 17,627

## Validation

The test successfully validates:
1. **Address Interleaving**: All 16 HNF slices receive accesses, confirming the 4-bit interleaving (bits [9:6]) works correctly
2. **LLC Distribution**: Accesses are evenly distributed across all slices (CV = 0.49%)
3. **DRAM Balance**: Load is evenly split between DDR0 and DDR1 (50.01% vs 49.99%)
4. **System Connectivity**: Core 0 at router 0 can access all 16 LLC slices through the Garnet mesh

## Troubleshooting

### Issue 1: Initial Simulation Too Slow
- **Symptom**: 256 MiB array (4,194,304 cache lines) simulation took >5 minutes
- **Root Cause**: Every cache line access was a cold miss to DRAM in cycle-accurate simulation
- **Solution**: Reduced array to 1 MiB (16,384 cache lines), still sufficient for balance verification

### Issue 2: All Statistics Initially Zero
- **Symptom**: Analysis showed zero accesses across all HNF slices
- **Root Cause**: Incorrect statistics paths in analysis script
- **Fix**: Updated paths from `system.ruby.cntrl{i}_Controller` to `system.ruby.hnf{i}.cntrl.cache`

### Issue 3: CPU Statistics Not Available
- **Symptom**: Commit stats and cache stats returned zero
- **Root Cause**: Wrong stat name (`committedInsts` vs `commitStats0.numInsts`)
- **Fix**: Corrected CPU stat paths to match gem5's actual naming

## Lessons Learned

1. **Simulation Time Management**: Large memory workloads in cycle-accurate simulation run slowly. Use minimal but representative test sizes.
2. **Statistics Path Discovery**: gem5 statistics naming varies by protocol (CHI vs MI). Always verify paths with `grep stats.txt`.
3. **Address Interleaving**: With `--num-dirs=2`, consecutive cache lines alternate between DDR controllers, providing natural load balancing.
4. **Cold-Miss Performance**: Streaming access patterns have low L1D hit rates (~38%) because they don't reuse data, but this is expected and correct.

## Conclusion

The smoke test (Stage 3a) is complete and validates the correct operation of:
- 4×4 CHI mesh topology
- Address interleaving across 16 HNF slices
- DRAM controller interleaving (2 controllers at routers 0 and 15)
- Garnet network connectivity

All files are located in `ruby-book/final/gl47-3a/`:
- `rbook_test_smoke.c` - Test program
- `analyze_smoke_test.py` - Analysis script
- `Makefile` - Build and run automation

The test can be run with: `make test`
