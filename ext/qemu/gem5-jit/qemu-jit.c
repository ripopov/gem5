/*
 * QEMU RISC-V TCG adapter for gem5 JitCPU
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "qemu-jit.h"

#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/tcg-accel-ops-icount.h"
#include "exec/icount.h"
#include "exec/tb-flush.h"
#include "exec/translation-block.h"
#include "hw/core/cpu.h"
#include "qemu-main.h"
#include "qemu/main-loop.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/replay.h"
#include "system/system.h"
#include "target/riscv/cpu.h"

#include <limits.h>

typedef struct Gem5QemuJitState {
    Gem5QemuJitCallbacks callbacks;
    CPUState *cpu;
    RISCVCPU *riscv_cpu;
    MemoryRegion memory;
    MemoryRegion ram[32];
    size_t ram_count;
    bool initialized;
} Gem5QemuJitState;

static Gem5QemuJitState jit;

/* system/main.c owns this symbol in a normal QEMU executable. The embedded
 * library has no QEMU main function, but display backends still reference it. */
int (*qemu_main)(void);

static void
jit_maybe_stop(Gem5QemuJitState *state)
{
    if (state->callbacks.should_stop &&
        state->callbacks.should_stop(state->callbacks.opaque)) {
        cpu_exit(state->cpu);
    }
}

static uint64_t
jit_read_time(void *opaque)
{
    Gem5QemuJitState *state = opaque;

    if (!state->callbacks.read_time) {
        return 0;
    }
    return state->callbacks.read_time(state->callbacks.opaque);
}

static MemTxResult
jit_memory_read(void *opaque, hwaddr address, uint64_t *value,
                unsigned size, MemTxAttrs attrs)
{
    Gem5QemuJitState *state = opaque;
    uint8_t data[8] = { 0 };
    uint64_t result = 0;
    unsigned i;

    if (size > sizeof(data) ||
        state->callbacks.memory_read(state->callbacks.opaque, address,
                                     data, size) != 0) {
        return MEMTX_ERROR;
    }
    jit_maybe_stop(state);

    for (i = 0; i < size; ++i) {
        result |= (uint64_t)data[i] << (i * 8);
    }
    *value = result;
    return MEMTX_OK;
}

static MemTxResult
jit_memory_write(void *opaque, hwaddr address, uint64_t value,
                 unsigned size, MemTxAttrs attrs)
{
    Gem5QemuJitState *state = opaque;
    uint8_t data[8];
    unsigned i;

    if (size > sizeof(data)) {
        return MEMTX_ERROR;
    }
    for (i = 0; i < size; ++i) {
        data[i] = value >> (i * 8);
    }

    if (state->callbacks.memory_write(state->callbacks.opaque, address,
                                      data, size) != 0) {
        return MEMTX_ERROR;
    }
    jit_maybe_stop(state);
    return MEMTX_OK;
}

static const MemoryRegionOps jit_memory_ops = {
    .read_with_attrs = jit_memory_read,
    .write_with_attrs = jit_memory_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

typedef struct Gem5QemuJitRunRequest {
    uint64_t max_instructions;
    Gem5QemuJitRunResult result;
} Gem5QemuJitRunRequest;

static void
jit_run_on_vcpu(CPUState *cpu, run_on_cpu_data data)
{
    Gem5QemuJitRunRequest *request = data.host_ptr;
    int64_t before;
    int64_t after;
    int64_t budget;

    /* cpu_exec() is normally entered by QEMU's round-robin TCG loop with the
     * BQL dropped. Keep exactly the same locking and icount protocol here. */
    qatomic_set(&cpu->exit_request, false);
    qatomic_set(&cpu->neg.icount_decr.u16.high, 0);
    bql_unlock();
    if (jit.callbacks.run_begin) {
        jit.callbacks.run_begin(jit.callbacks.opaque);
    }
    before = icount_get_raw();
    budget = MIN(request->max_instructions, (uint64_t)INT32_MAX);
    icount_prepare_for_run(cpu, budget);
    request->result.qemu_exception = cpu_exec(cpu);
    icount_process_data(cpu);
    after = icount_get_raw();
    if (jit.callbacks.run_end) {
        jit.callbacks.run_end(jit.callbacks.opaque);
    }
    bql_lock();

    request->result.instructions = after - before;
    if (request->result.qemu_exception == EXCP_HLT &&
        (jit.riscv_cpu->env.bins & 0x01ffffff) == 0x7b) {
        request->result.reason = GEM5_QEMU_JIT_EXIT_M5OP;
        request->result.m5_function = jit.riscv_cpu->env.bins >> 25;
    } else if (request->result.instructions == budget) {
        request->result.reason = GEM5_QEMU_JIT_EXIT_BUDGET;
    } else if (cpu->halted) {
        request->result.reason = GEM5_QEMU_JIT_EXIT_HALTED;
    } else if (request->result.qemu_exception == EXCP_INTERRUPT) {
        request->result.reason = GEM5_QEMU_JIT_EXIT_INTERRUPT;
    } else {
        request->result.reason = GEM5_QEMU_JIT_EXIT_EXCEPTION;
    }
}

int
gem5_qemu_jit_init(const Gem5QemuJitCallbacks *callbacks)
{
    uint64_t guest_address;
    uint64_t size;
    uint8_t *host_address;
    int writable;
    size_t index;
    char *argv[] = {
        (char *)"gem5-qemu-jit",
        (char *)"-machine", (char *)"none",
        (char *)"-cpu",
        (char *)"rv64,v=false,h=false,zicbom=false,zicboz=false",
        (char *)"-accel", (char *)"tcg,thread=single",
        (char *)"-icount", (char *)"shift=0,sleep=off",
        (char *)"-S",
        (char *)"-nodefaults",
        (char *)"-no-user-config",
        (char *)"-display", (char *)"none",
        (char *)"-monitor", (char *)"none",
        (char *)"-serial", (char *)"none",
    };

    if (jit.initialized || !callbacks ||
        callbacks->abi_version != GEM5_QEMU_JIT_ABI_VERSION ||
        !callbacks->memory_read || !callbacks->memory_write) {
        return -1;
    }

    jit.callbacks = *callbacks;
    qemu_init(G_N_ELEMENTS(argv), argv);

    jit.cpu = first_cpu;
    if (!jit.cpu || CPU_NEXT(jit.cpu)) {
        return -1;
    }
    jit.riscv_cpu = RISCV_CPU(jit.cpu);
    riscv_gem5_jit_enabled = true;
    jit.riscv_cpu->env.rdtime_fn = jit_read_time;
    jit.riscv_cpu->env.rdtime_fn_arg = &jit;

    /* qemu_init() returns with both locks held. The normal QEMU main() drops
     * them before entering its loop. This adapter retains the BQL so that
     * synchronous run_on_cpu() calls can use it, but icount owns replay_lock
     * while executing a batch. */
    replay_mutex_unlock();

    memory_region_init_io(&jit.memory, NULL, &jit_memory_ops, &jit,
                          "gem5-jit-physical-memory", UINT64_MAX);
    memory_region_add_subregion(get_system_memory(), 0, &jit.memory);

    if (jit.callbacks.memory_map) {
        for (index = 0; ; ++index) {
            char *name;

            if (jit.callbacks.memory_map(jit.callbacks.opaque, index,
                                         &guest_address, &size,
                                         &host_address, &writable) != 0) {
                break;
            }
            if (!size || !host_address) {
                return -1;
            }

            if (jit.ram_count == G_N_ELEMENTS(jit.ram)) {
                return -1;
            }
            name = g_strdup_printf("gem5-jit-ram-%zu", jit.ram_count);
            memory_region_init_ram_ptr(&jit.ram[jit.ram_count], NULL, name,
                                       size, host_address);
            memory_region_set_readonly(&jit.ram[jit.ram_count], !writable);
            memory_region_add_subregion_overlap(
                get_system_memory(), guest_address,
                &jit.ram[jit.ram_count], 1);
            ++jit.ram_count;
        }
    }

    jit.initialized = true;
    return 0;
}

int
gem5_qemu_jit_run(uint64_t max_instructions, Gem5QemuJitRunResult *result)
{
    Gem5QemuJitRunRequest request = {
        .max_instructions = max_instructions,
    };

    if (!jit.initialized || !result || max_instructions == 0) {
        return -1;
    }

    run_on_cpu(jit.cpu, jit_run_on_vcpu, RUN_ON_CPU_HOST_PTR(&request));
    *result = request.result;
    return 0;
}

uint64_t
gem5_qemu_jit_get_gpr(unsigned index)
{
    if (!jit.initialized || index >= 32) {
        return 0;
    }
    return index == 0 ? 0 : jit.riscv_cpu->env.gpr[index];
}

int
gem5_qemu_jit_set_gpr(unsigned index, uint64_t value)
{
    if (!jit.initialized || index >= 32) {
        return -1;
    }
    if (index != 0) {
        jit.riscv_cpu->env.gpr[index] = value;
    }
    return 0;
}

uint64_t
gem5_qemu_jit_get_fpr(unsigned index)
{
    if (!jit.initialized || index >= 32) {
        return 0;
    }
    return jit.riscv_cpu->env.fpr[index];
}

int
gem5_qemu_jit_set_fpr(unsigned index, uint64_t value)
{
    if (!jit.initialized || index >= 32) {
        return -1;
    }
    jit.riscv_cpu->env.fpr[index] = value;
    return 0;
}

uint64_t
gem5_qemu_jit_get_pc(void)
{
    return jit.initialized ? jit.riscv_cpu->env.pc : 0;
}

void
gem5_qemu_jit_set_pc(uint64_t value)
{
    if (jit.initialized) {
        jit.riscv_cpu->env.pc = value;
        jit.cpu->halted = 0;
    }
}

unsigned
gem5_qemu_jit_get_priv(void)
{
    return jit.initialized ? jit.riscv_cpu->env.priv : 0;
}

int
gem5_qemu_jit_set_priv(unsigned value)
{
    if (!jit.initialized || value > PRV_M) {
        return -1;
    }
    riscv_cpu_set_mode(&jit.riscv_cpu->env, value, false);
    return 0;
}

int
gem5_qemu_jit_get_csr(unsigned csr, uint64_t *value)
{
    if (!jit.initialized || !value || csr >= 0x1000) {
        return -1;
    }
    return riscv_csr_read_i64(&jit.riscv_cpu->env, csr, value) ==
        RISCV_EXCP_NONE ? 0 : -1;
}

int
gem5_qemu_jit_set_csr(unsigned csr, uint64_t value)
{
    if (!jit.initialized || csr >= 0x1000) {
        return -1;
    }
    return riscv_csr_write_i64(&jit.riscv_cpu->env, csr, value) ==
        RISCV_EXCP_NONE ? 0 : -1;
}

uint64_t
gem5_qemu_jit_get_mip(void)
{
    return jit.initialized ? jit.riscv_cpu->env.mip : 0;
}

void
gem5_qemu_jit_set_mip(uint64_t value)
{
    if (jit.initialized) {
        jit.riscv_cpu->env.mip = value;
    }
}

void
gem5_qemu_jit_invalidate_translations(void)
{
    if (jit.initialized) {
        queue_tb_flush(jit.cpu);
    }
}
