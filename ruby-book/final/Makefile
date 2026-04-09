CC = riscv64-linux-gnu-gcc
CFLAGS = -O2 -static
HOP_LATENCY_DIR = hop_latency

BINS = trivial rbook_test_smoke

all: $(BINS) rbook_test_hop_latency

trivial: trivial.c
	$(CC) $(CFLAGS) -o $@ $<

rbook_test_smoke: rbook_test_smoke.c
	$(CC) $(CFLAGS) -o $@ $<

rbook_test_hop_latency:
	@$(MAKE) -C $(HOP_LATENCY_DIR) rbook_test_hop_latency

hop_latency_report:
	@$(MAKE) -C $(HOP_LATENCY_DIR) report

clean:
	rm -f $(BINS)
	$(MAKE) -C $(HOP_LATENCY_DIR) clean

.PHONY: all clean rbook_test_hop_latency hop_latency_report
