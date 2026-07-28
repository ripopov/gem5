SAMPLES := hip-vector-add hip-matmul hip-kernel-chain rainbow-image

OUT_DIR ?= ../work/apps
SAVE_TEMPS_DIR ?= $(OUT_DIR)/save-temps
DISASM_DIR ?= $(OUT_DIR)/disasm
GPU_TARGET ?= gfx942
HIPCC ?= hipcc
LLVM_OBJDUMP ?= /opt/rocm/llvm/bin/llvm-objdump

HIPFLAGS ?= --offload-arch=$(GPU_TARGET) -O2 -std=c++17
DISASM_OUTPUTS := $(foreach sample,$(SAMPLES),$(DISASM_DIR)/$(sample)/kernels.$(GPU_TARGET).disasm)

.PHONY: all clean $(SAMPLES)

all: $(addprefix $(OUT_DIR)/,$(SAMPLES)) $(DISASM_OUTPUTS)

$(OUT_DIR):
	mkdir -p $@

$(OUT_DIR)/hip-vector-add: hip-vector-add/main.cpp | $(OUT_DIR)
	mkdir -p $(SAVE_TEMPS_DIR)/hip-vector-add
	cd $(SAVE_TEMPS_DIR)/hip-vector-add && \
		$(HIPCC) $(HIPFLAGS) --save-temps=cwd $(abspath $<) -o $(abspath $@)

$(OUT_DIR)/hip-matmul: hip-matmul/main.cpp | $(OUT_DIR)
	mkdir -p $(SAVE_TEMPS_DIR)/hip-matmul
	cd $(SAVE_TEMPS_DIR)/hip-matmul && \
		$(HIPCC) $(HIPFLAGS) --save-temps=cwd $(abspath $<) -o $(abspath $@)

$(OUT_DIR)/hip-kernel-chain: hip-kernel-chain/main.cpp | $(OUT_DIR)
	mkdir -p $(SAVE_TEMPS_DIR)/hip-kernel-chain
	cd $(SAVE_TEMPS_DIR)/hip-kernel-chain && \
		$(HIPCC) $(HIPFLAGS) --save-temps=cwd $(abspath $<) -o $(abspath $@)

$(OUT_DIR)/rainbow-image: rainbow-image/main.cpp | $(OUT_DIR)
	mkdir -p $(SAVE_TEMPS_DIR)/rainbow-image
	cd $(SAVE_TEMPS_DIR)/rainbow-image && \
		$(HIPCC) $(HIPFLAGS) --save-temps=cwd $(abspath $<) -o $(abspath $@)

$(DISASM_DIR)/%/kernels.$(GPU_TARGET).disasm: $(OUT_DIR)/%
	mkdir -p $(@D)
	cd $(OUT_DIR) && $(LLVM_OBJDUMP) --offloading $* >/dev/null
	@set -- "$(OUT_DIR)/$*".*.hipv4-amdgcn-amd-amdhsa--"$(GPU_TARGET)"; \
		test -f "$$1" || { echo "no $(GPU_TARGET) GPU code object found in $<" >&2; exit 1; }; \
		$(LLVM_OBJDUMP) --disassemble --demangle "$$@" > $@; \
		rm -f "$$@" "$(OUT_DIR)/$*".*.host-*

hip-vector-add: $(OUT_DIR)/hip-vector-add $(DISASM_DIR)/hip-vector-add/kernels.$(GPU_TARGET).disasm
hip-matmul: $(OUT_DIR)/hip-matmul $(DISASM_DIR)/hip-matmul/kernels.$(GPU_TARGET).disasm
hip-kernel-chain: $(OUT_DIR)/hip-kernel-chain $(DISASM_DIR)/hip-kernel-chain/kernels.$(GPU_TARGET).disasm
rainbow-image: $(OUT_DIR)/rainbow-image $(DISASM_DIR)/rainbow-image/kernels.$(GPU_TARGET).disasm

clean:
	rm -rf $(OUT_DIR)
