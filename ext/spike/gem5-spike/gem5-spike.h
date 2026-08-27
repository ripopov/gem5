/*
 * Spike (riscv-isa-sim) adapter for the gem5 RISC-V SpikeCPU
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The ABI mirrors ext/qemu/repo/contrib/gem5-jit/qemu-jit.h so that both
 * functional backends present the same contract to gem5: gem5 owns the
 * physical address space, the interrupt controllers and simulated time, and
 * the backend executes one bounded batch of guest instructions at a time.
 */

#ifndef GEM5_SPIKE_H
#define GEM5_SPIKE_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define GEM5_SPIKE_API __attribute__((visibility("default")))
#else
#define GEM5_SPIKE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*Gem5SpikeMemoryRead)(void *opaque, uint64_t address,
                                   uint8_t *data, size_t size);
typedef int (*Gem5SpikeMemoryWrite)(void *opaque, uint64_t address,
                                    const uint8_t *data, size_t size);
typedef int (*Gem5SpikeMemoryMap)(void *opaque, size_t index,
                                  uint64_t *guest_address, uint64_t *size,
                                  uint8_t **host_address, int *writable);
typedef void (*Gem5SpikeRunBoundary)(void *opaque);
typedef uint64_t (*Gem5SpikeReadTime)(void *opaque);
typedef int (*Gem5SpikeShouldStop)(void *opaque);

typedef struct Gem5SpikeCallbacks {
    uint32_t instance_id;
    uint32_t instance_count;
    uint64_t hart_id;
    /* Spike ISA and privilege-mode strings, e.g. "rv64imafdc_zicsr"/"MSU". */
    const char *isa_string;
    const char *privilege_string;
    /* Widest supported virtual address: 0 (bare), 39, 48 or 57. Spike takes
     * this from a device tree; gem5 reports it from its own ISA object. */
    uint32_t max_vaddr_bits;
    Gem5SpikeMemoryRead memory_read;
    Gem5SpikeMemoryWrite memory_write;
    Gem5SpikeMemoryMap memory_map;
    Gem5SpikeRunBoundary run_begin;
    Gem5SpikeRunBoundary run_end;
    Gem5SpikeReadTime read_time;
    Gem5SpikeShouldStop should_stop;
    void *opaque;
} Gem5SpikeCallbacks;

enum Gem5SpikeExitReason {
    GEM5_SPIKE_EXIT_BUDGET = 0,
    GEM5_SPIKE_EXIT_HALTED = 1,
    GEM5_SPIKE_EXIT_INTERRUPT = 2,
    GEM5_SPIKE_EXIT_EXCEPTION = 3,
    GEM5_SPIKE_EXIT_ERROR = 4,
    GEM5_SPIKE_EXIT_M5OP = 5,
};

typedef struct Gem5SpikeRunResult {
    uint64_t instructions;
    int spike_exception;
    enum Gem5SpikeExitReason reason;
    uint32_t m5_function;
} Gem5SpikeRunResult;

/*
 * Each instance ID selects one independent Spike hart: its own processor_t,
 * architectural state, TLB and decoded-instruction cache. Unlike QEMU's TCG
 * runtime there is no shared engine, so instances interact only through the
 * gem5 physical address space they are given.
 */
GEM5_SPIKE_API int gem5_spike_init(const Gem5SpikeCallbacks *callbacks);
GEM5_SPIKE_API int gem5_spike_run(uint32_t instance_id,
                                  uint64_t max_instructions,
                                  Gem5SpikeRunResult *result);

GEM5_SPIKE_API uint64_t gem5_spike_get_gpr(uint32_t instance_id,
                                           unsigned index);
GEM5_SPIKE_API int gem5_spike_set_gpr(uint32_t instance_id, unsigned index,
                                      uint64_t value);
GEM5_SPIKE_API uint64_t gem5_spike_get_fpr(uint32_t instance_id,
                                           unsigned index);
GEM5_SPIKE_API int gem5_spike_set_fpr(uint32_t instance_id, unsigned index,
                                      uint64_t value);
GEM5_SPIKE_API uint64_t gem5_spike_get_pc(uint32_t instance_id);
GEM5_SPIKE_API void gem5_spike_set_pc(uint32_t instance_id, uint64_t value);
GEM5_SPIKE_API unsigned gem5_spike_get_priv(uint32_t instance_id);
GEM5_SPIKE_API int gem5_spike_set_priv(uint32_t instance_id, unsigned value);
GEM5_SPIKE_API int gem5_spike_get_csr(uint32_t instance_id, unsigned csr,
                                      uint64_t *value);
GEM5_SPIKE_API int gem5_spike_set_csr(uint32_t instance_id, unsigned csr,
                                      uint64_t value);
GEM5_SPIKE_API uint64_t gem5_spike_get_mip(uint32_t instance_id);
GEM5_SPIKE_API void gem5_spike_set_mip(uint32_t instance_id, uint64_t value);
/* Also clears software-TLB, decode cache, WFI and LR/SC transient state. */
GEM5_SPIKE_API void gem5_spike_invalidate_translations(uint32_t instance_id);

#ifdef __cplusplus
}
#endif

#endif /* GEM5_SPIKE_H */
