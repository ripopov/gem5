/*
 * QEMU RISC-V TCG adapter for gem5 JitCPU
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GEM5_QEMU_JIT_H
#define GEM5_QEMU_JIT_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define GEM5_QEMU_JIT_API __attribute__((visibility("default")))
#else
#define GEM5_QEMU_JIT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GEM5_QEMU_JIT_ABI_VERSION 3u

typedef int (*Gem5QemuJitMemoryRead)(void *opaque, uint64_t address,
                                     uint8_t *data, size_t size);
typedef int (*Gem5QemuJitMemoryWrite)(void *opaque, uint64_t address,
                                      const uint8_t *data, size_t size);
typedef int (*Gem5QemuJitMemoryMap)(void *opaque, size_t index,
                                    uint64_t *guest_address, uint64_t *size,
                                    uint8_t **host_address, int *writable);
typedef void (*Gem5QemuJitRunBoundary)(void *opaque);
typedef uint64_t (*Gem5QemuJitReadTime)(void *opaque);
typedef int (*Gem5QemuJitShouldStop)(void *opaque);

typedef struct Gem5QemuJitCallbacks {
    uint32_t abi_version;
    Gem5QemuJitMemoryRead memory_read;
    Gem5QemuJitMemoryWrite memory_write;
    Gem5QemuJitMemoryMap memory_map;
    Gem5QemuJitRunBoundary run_begin;
    Gem5QemuJitRunBoundary run_end;
    Gem5QemuJitReadTime read_time;
    Gem5QemuJitShouldStop should_stop;
    void *opaque;
} Gem5QemuJitCallbacks;

enum Gem5QemuJitExitReason {
    GEM5_QEMU_JIT_EXIT_BUDGET = 0,
    GEM5_QEMU_JIT_EXIT_HALTED = 1,
    GEM5_QEMU_JIT_EXIT_INTERRUPT = 2,
    GEM5_QEMU_JIT_EXIT_EXCEPTION = 3,
    GEM5_QEMU_JIT_EXIT_ERROR = 4,
    GEM5_QEMU_JIT_EXIT_M5OP = 5,
};

typedef struct Gem5QemuJitRunResult {
    uint64_t instructions;
    int qemu_exception;
    enum Gem5QemuJitExitReason reason;
    uint32_t m5_function;
} Gem5QemuJitRunResult;

/*
 * The PoC backend is deliberately a process-wide singleton. It supports one
 * RV64 hart and must be initialized exactly once.
 */
GEM5_QEMU_JIT_API int gem5_qemu_jit_init(
    const Gem5QemuJitCallbacks *callbacks);
GEM5_QEMU_JIT_API int gem5_qemu_jit_run(
    uint64_t max_instructions, Gem5QemuJitRunResult *result);

GEM5_QEMU_JIT_API uint64_t gem5_qemu_jit_get_gpr(unsigned index);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_gpr(unsigned index, uint64_t value);
GEM5_QEMU_JIT_API uint64_t gem5_qemu_jit_get_fpr(unsigned index);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_fpr(unsigned index, uint64_t value);
GEM5_QEMU_JIT_API uint64_t gem5_qemu_jit_get_pc(void);
GEM5_QEMU_JIT_API void gem5_qemu_jit_set_pc(uint64_t value);
GEM5_QEMU_JIT_API unsigned gem5_qemu_jit_get_priv(void);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_priv(unsigned value);
GEM5_QEMU_JIT_API int gem5_qemu_jit_get_csr(unsigned csr, uint64_t *value);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_csr(unsigned csr, uint64_t value);
GEM5_QEMU_JIT_API uint64_t gem5_qemu_jit_get_mip(void);
GEM5_QEMU_JIT_API void gem5_qemu_jit_set_mip(uint64_t value);
GEM5_QEMU_JIT_API void gem5_qemu_jit_invalidate_translations(void);

#ifdef __cplusplus
}
#endif

#endif /* GEM5_QEMU_JIT_H */
