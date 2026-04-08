# Makefile for ruby-book final project smoke test (Stage 3a)

CC = riscv64-linux-gnu-gcc
CFLAGS = -O2 -static -Wall -Wextra
GEM5_DIR = $(realpath $(PWD)/../../..)
GEM5 = $(GEM5_DIR)/build/RISCV/gem5.opt
CONFIG = $(GEM5_DIR)/configs/example/rbook_mesh_config.py
OUTPUT_DIR = $(GEM5_DIR)/m5out/gl47-3a-smoke-test

# Test binaries
BINS = rbook_test_smoke

# Default target
all: $(BINS)

# Build the smoke test binary
rbook_test_smoke: rbook_test_smoke.c
	$(CC) $(CFLAGS) -o $@ $<

# Clean build artifacts
clean:
	rm -f $(BINS)

# Run the smoke test simulation with proper statistics output
run-smoke: rbook_test_smoke
	@echo "=== Running smoke test simulation ==="
	@mkdir -p $(GEM5_DIR)/m5out
	$(GEM5) \
		-d $(OUTPUT_DIR) \
		$(CONFIG) \
		--cmd=$(PWD)/$<
	@echo "=== Simulation complete ==="
	@echo "stats.txt in: $(OUTPUT_DIR)/"

# Analyze the smoke test results
analyze:
	@if [ ! -f $(OUTPUT_DIR)/stats.txt ]; then \
		echo "Error: stats.txt not found. Run 'make run-smoke' first."; \
		exit 1; \
	fi
	@echo "=== Analyzing smoke test results ==="
	python3 analyze_smoke_test.py $(OUTPUT_DIR)/stats.txt

# Run and analyze in one step
test: run-smoke analyze

.PHONY: all clean run-smoke analyze test
