CMAKE ?= cmake
CTEST ?= ctest
SCONS ?= scons
PYTHON ?= python3
JOBS ?= 20

GEM5_ROOT := $(abspath $(CURDIR)/../../..)
ROCJITSU_SOURCE := $(GEM5_ROOT)/ext/hsakmt-client/rocm-systems/emulation/rocjitsu
WORKSPACE := $(GEM5_ROOT)/gpu-lab/workspace

BUILD_DIR ?= $(CURDIR)/build
KMD_BUILD_DIR ?= $(BUILD_DIR)/rocjitsu-kmd
KMD_PREFIX ?= $(BUILD_DIR)/rocjitsu-kmd-prefix
ROCJITSU_BUILD_DIR ?= $(BUILD_DIR)/rocjitsu

SERVICE_TEST_NAMES := \
	service-stats-test \
	shared-region-allocator-test \
	doorbell-bridge-test \
	gpu-vm-manager-test \
	queue-work-engine-test \
	gem5-device-backend-test
SERVICE_TEST_TARGETS := \
	$(addprefix build/VEGA_X86/dev/hsakmt-service/,\
	$(addsuffix .opt,$(SERVICE_TEST_NAMES)))
SERVICE_TEST_BIN_DIR := $(GEM5_ROOT)/build/VEGA_X86/dev/hsakmt-service

.PHONY: all check clean configure-kmd install-kmd rocjitsu-tests \
	service-tests verify verify-images verify-sample-workloads

all: check

configure-kmd:
	$(CMAKE) -S "$(ROCJITSU_SOURCE)" -B "$(KMD_BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DCMAKE_INSTALL_PREFIX="$(KMD_PREFIX)" \
		-DRJ_KMD_SERVER_ONLY=ON \
		-DRJ_LOG_GROUPS=OFF

install-kmd: configure-kmd
	$(CMAKE) --build "$(KMD_BUILD_DIR)" --parallel "$(JOBS)"
	$(CMAKE) --install "$(KMD_BUILD_DIR)"

service-tests: install-kmd
	cd "$(GEM5_ROOT)" && $(SCONS) $(SERVICE_TEST_TARGETS) \
		ROCJITSU_KMD_PREFIX="$(KMD_PREFIX)" -j "$(JOBS)"
	@set -e; \
	for test in $(SERVICE_TEST_NAMES); do \
		"$(SERVICE_TEST_BIN_DIR)/$$test.opt"; \
	done

rocjitsu-tests:
	$(CMAKE) -S "$(ROCJITSU_SOURCE)" -B "$(ROCJITSU_BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DRJ_LOG_GROUPS=OFF
	$(CMAKE) --build "$(ROCJITSU_BUILD_DIR)" --parallel "$(JOBS)"
	$(CTEST) --test-dir "$(ROCJITSU_BUILD_DIR)" --output-on-failure \
		--parallel "$(JOBS)"

check: service-tests rocjitsu-tests
	$(PYTHON) -m py_compile \
		"$(GEM5_ROOT)/configs/example/gpufs/hsakmt_server.py"
	bash -n \
		"$(GEM5_ROOT)/util/dockerfiles/gpu-hsakmt-runner/gem5-hsakmt-runner" \
		"$(GEM5_ROOT)/util/dockerfiles/gpu-hsakmt-sdk/rocjitsu-run" \
		"$(WORKSPACE)/scripts/common.sh" \
		"$(WORKSPACE)/scripts/build-samples.sh" \
		"$(WORKSPACE)/scripts/run-app.sh" \
		"$(WORKSPACE)/scripts/run-sample.sh" \
		"$(WORKSPACE)/scripts/run-all-samples.sh" \
		"$(WORKSPACE)/scripts/verify-run.sh"

verify-images:
	"$(WORKSPACE)/scripts/build-hsakmt-images.sh"

verify-sample-workloads:
	cd "$(WORKSPACE)" && \
		./scripts/run-all-samples.sh --backend gem5-kmd

verify: check verify-sample-workloads

clean:
	$(CMAKE) -E remove_directory "$(KMD_BUILD_DIR)"
	$(CMAKE) -E remove_directory "$(KMD_PREFIX)"
	$(CMAKE) -E remove_directory "$(ROCJITSU_BUILD_DIR)"
