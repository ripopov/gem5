# Accumulator-resident MFMA check

Build with a HIP toolchain supporting gfx942:

```sh
hipcc --offload-arch=gfx942 -O2 -std=c++17 main.cpp -o hip-mfma-accumulator
```

Run the executable through an AMDGPU timing-model workload launcher. It checks
every element of a 16×16 FP32 product against a CPU reference. The K loop keeps
its partial sums in accumulator registers across global loads and repeated
MFMA instructions. Inputs are small integers, so the comparison is exact.

The final result is explicitly copied into VGPRs before storing. This isolates
accumulator arithmetic and register dependencies from direct AGPR memory
operands, which the current gfx942 memory-instruction model does not implement.
The expected result is `hip_mfma_accumulator: PASS`.

Correctness alone does not detect false dependencies between VGPRs and AGPRs.
For timing validation, use a `GPUTrace,GPUMem` trace of a prefetched GEMM and
verify that arithmetic can issue while a load into an unrelated VGPR remains
pending. Accumulator operands must use the descriptor's `ACCUM_OFFSET` in
dependency tracking as well as functional execution.
