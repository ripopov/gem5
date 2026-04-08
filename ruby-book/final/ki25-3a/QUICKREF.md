# Quick Reference Card - KI25-3A Smoke Test

## Quick Start

```bash
cd ruby-book/final/ki25-3a
make                    # Build test
./run_smoke_test.sh     # Run simulation
python3 ki25-3a_smoke_test_analysis.py  # Analyze results
```

## File Guide

| File | Purpose |
|------|---------|
| `rbook_test_smoke.c` | Test source code |
| `rbook_test_smoke` | Compiled binary |
| `Makefile` | Build rules |
| `ki25-3a_smoke_test_analysis.py` | Analysis script |
| `run_smoke_test.sh` | Run simulation |
| `README.md` | Full documentation |
| `SUMMARY.md` | Final report |
| `QUICKREF.md` | This file |

## Commands

### Build
```bash
make                    # Build rbook_test_smoke
make clean             # Clean build artifacts
```

### Run Simulation
```bash
# Using the run script (recommended)
./run_smoke_test.sh [suffix]

# Manual run
../../build/RISCV/gem5.opt \
    -d m5out-smoke-$(date +%Y%m%d-%H%M%S) \
    ../../configs/example/rbook_mesh_config.py \
    --cmd=./rbook_test_smoke
```

### Analyze Results
```bash
# Auto-detect latest simulation
python3 ki25-3a_smoke_test_analysis.py

# Specify specific stats file
python3 ki25-3a_smoke_test_analysis.py m5out-smoke-v2/stats.txt
```

## Expected Output

### Test Program
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
```

### Analysis Script
```
======================================================================
KI25-3A SMOKE TEST ANALYSIS REPORT
======================================================================

[PASS] OK: HNF access distribution is balanced (max deviation: 0.2%)
         Total accesses per HNF: min=16461, max=16511, avg=16491.8

[PASS] Smoke test completed successfully!
       All 16 LLC slices are reachable and received traffic.
       Address interleaving is working correctly.
```

## Validation Checklist

- [ ] Test binary builds without errors
- [ ] Simulation completes (prints "Exiting @ tick...")
- [ ] Test program prints "PASS"
- [ ] All 16 HN-F controllers show nonzero accesses
- [ ] HNF distribution is balanced (< 25% deviation)
- [ ] L2 miss rate is high (cold cache expected)

## Troubleshooting

### Build Errors
```bash
# Check RISC-V toolchain
which riscv64-linux-gnu-gcc

# Install if missing
sudo apt-get install gcc-riscv64-linux-gnu
```

### Simulation Errors
```bash
# Check gem5 is built
ls -la ../../build/RISCV/gem5.opt

# Rebuild if needed
cd ../..
scons build/RISCV/gem5.opt PROTOCOL=CHI
```

### Zero HNF Accesses
- Check `configs/example/noc_config/rbook_4x4.py` router bindings
- Verify `--num-l3caches=16` in config
- Check address interleaving in Ruby.py

## System Specs

| Parameter | Value |
|-----------|-------|
| Topology | 4x4 mesh |
| Cores | 16 (RN-F) |
| LLC Slices | 16 (HN-F) |
| DDR Controllers | 2 (SN-F) |
| Protocol | CHI |
| Network | Garnet |
| Routing | XY |
| Memory | 512 MiB |

## Stats of Interest

```
system.ruby.hnf*.cntrl.cache.m_demand_hits
system.ruby.hnf*.cntrl.cache.m_demand_misses
system.cpu0.l1d.cache.m_demand_hits
system.cpu0.l2.cache.m_demand_misses
simTicks
simInsts
```

## Performance Targets

| Metric | Target | Typical |
|--------|--------|---------|
| HNF accesses per slice | > 4000 | ~16,500 |
| Max deviation | < 25% | < 1% |
| Sim time | < 1 min | ~45 sec |
| L2 miss rate | > 90% | ~100% |

## Help

```bash
# Full documentation
cat README.md

# Detailed report
cat SUMMARY.md

# View topology
dot -Tsvg m5out-smoke-*/config.dot -o topology.svg
```

---
*Quick reference for KI25-3A Smoke Test*
