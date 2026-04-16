CC = riscv64-linux-gnu-gcc
CFLAGS = -O2 -static
TRIVIAL_DIR = trivial
SMOKE_DIR = smoke
HOP_LATENCY_DIR = hop_latency
FALSE_SHARING_DIR = false_sharing
HOTSPOT_DIR = hotspot
HOTSPOT_HEAVY_DIR = hotspot-heavy

all: trivial_build smoke_build rbook_test_hop_latency rbook_test_false_sharing hotspot_build hotspot_heavy_build

trivial_build:
	@$(MAKE) -C $(TRIVIAL_DIR) build

trivial_run:
	@$(MAKE) -C $(TRIVIAL_DIR) run

smoke_build:
	@$(MAKE) -C $(SMOKE_DIR) build

rbook_test_smoke:
	@$(MAKE) -C $(SMOKE_DIR) report

rbook_test_hop_latency:
	@$(MAKE) -C $(HOP_LATENCY_DIR) rbook_test_hop_latency

hop_latency_report:
	@$(MAKE) -C $(HOP_LATENCY_DIR) report

rbook_test_false_sharing:
	@$(MAKE) -C $(FALSE_SHARING_DIR) rbook_test_false_sharing

false_sharing_report:
	@$(MAKE) -C $(FALSE_SHARING_DIR) report

hotspot_build:
	@$(MAKE) -C $(HOTSPOT_DIR) build

hotspot_run_shared_links:
	@$(MAKE) -C $(HOTSPOT_DIR) run_shared_links

hotspot_run_per_vnet_links:
	@$(MAKE) -C $(HOTSPOT_DIR) run_per_vnet_links

rbook_test_hotspot:
	@$(MAKE) -C $(HOTSPOT_DIR) report

hotspot_report:
	@$(MAKE) -C $(HOTSPOT_DIR) report

hotspot_heavy_build:
	@$(MAKE) -C $(HOTSPOT_HEAVY_DIR) build

hotspot_heavy_run:
	@$(MAKE) -C $(HOTSPOT_HEAVY_DIR) run

rbook_test_hotspot_heavy:
	@$(MAKE) -C $(HOTSPOT_HEAVY_DIR) report

hotspot_heavy_report:
	@$(MAKE) -C $(HOTSPOT_HEAVY_DIR) report

clean:
	rm -rf __pycache__
	$(MAKE) -C $(TRIVIAL_DIR) clean
	$(MAKE) -C $(SMOKE_DIR) clean
	$(MAKE) -C $(HOP_LATENCY_DIR) clean
	$(MAKE) -C $(FALSE_SHARING_DIR) clean
	$(MAKE) -C $(HOTSPOT_DIR) clean
	$(MAKE) -C $(HOTSPOT_HEAVY_DIR) clean

.PHONY: all clean trivial_build trivial_run smoke_build rbook_test_smoke rbook_test_hop_latency hop_latency_report rbook_test_false_sharing false_sharing_report hotspot_build hotspot_run_shared_links hotspot_run_per_vnet_links rbook_test_hotspot hotspot_report hotspot_heavy_build hotspot_heavy_run rbook_test_hotspot_heavy hotspot_heavy_report
