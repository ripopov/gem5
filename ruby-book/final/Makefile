CC = riscv64-linux-gnu-gcc
CFLAGS = -O2 -static
SMOKE_DIR = smoke
HOP_LATENCY_DIR = hop_latency

BINS = trivial

all: $(BINS) smoke_build rbook_test_hop_latency

trivial: trivial.c
	$(CC) $(CFLAGS) -o $@ $<

smoke_build:
	@$(MAKE) -C $(SMOKE_DIR) build

rbook_test_smoke:
	@$(MAKE) -C $(SMOKE_DIR) report

rbook_test_hop_latency:
	@$(MAKE) -C $(HOP_LATENCY_DIR) rbook_test_hop_latency

hop_latency_report:
	@$(MAKE) -C $(HOP_LATENCY_DIR) report

clean:
	rm -f $(BINS)
	rm -rf __pycache__
	$(MAKE) -C $(SMOKE_DIR) clean
	$(MAKE) -C $(HOP_LATENCY_DIR) clean

.PHONY: all clean smoke_build rbook_test_smoke rbook_test_hop_latency hop_latency_report
