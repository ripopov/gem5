/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "accelerator_runtime.h"
#include "renderer.h"

static _Alignas(8) uint8_t local_job[56];
static _Alignas(8) uint8_t local_scene[SCENE_CAPACITY];

#ifdef ACCEL_TEST
static const volatile uint64_t code_probe = 0x123456789abcdef0UL;
static volatile uint64_t data_probe = 0xfedcba9876543210UL;
extern uint8_t __data_start[], __stack_top[];

static int
check_tcm(void)
{
    /* Exercise ELF initialized data, shared code/constant storage, BSS,
     * writable data and DMA aperture rejection before submitting work. */
    if (code_probe != 0x123456789abcdef0UL ||
        data_probe != 0xfedcba9876543210UL || local_job[0]) {
        return 26;
    }
    data_probe++;
    if (data_probe != 0xfedcba9876543211UL) {
        return 27;
    }
    const uint64_t sources[] = {
        (uintptr_t)&code_probe,
        (uintptr_t)local_job,
        (uintptr_t)__data_start - 1,
        0x4003ffffUL,
        UINT64_MAX,
    };
    const uint64_t destinations[] = {
        (uintptr_t)local_job, (uintptr_t)&code_probe, (uintptr_t)local_job,
        (uintptr_t)local_job, (uintptr_t)local_job,
    };
    for (unsigned i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        start_dma(sources[i], destinations[i], 8);
        if (REG32(72) != 3 || code_probe != 0x123456789abcdef0UL ||
            local_job[0]) {
            return 28;
        }
    }
    if ((uintptr_t)__stack_top > 0x40040000UL) {
        return 29;
    }
    return 0;
}
#endif

static int
write_row(uint64_t address, const uint8_t *row, uint32_t length)
{
    return copy_dma((uintptr_t)row, address, length);
}
static int
run_job(uint64_t address)
{
    if (address % 8 || address < 0x80000000 || address > 0x84000000 - 56) {
        return 21;
    }
    int error = copy_dma(address, (uintptr_t)local_job, 56);
    if (error) {
        return error;
    }
    Job job;
    decode_job(local_job, &job);
    if (job.scene_addr % 8 || job.scene_addr < 0x80000000 ||
        job.scene_addr > 0x84000000 - 56) {
        return 22;
    }
    error = copy_dma(job.scene_addr, (uintptr_t)local_scene, 56);
    if (error) {
        return error;
    }
    uint32_t materials = read32(local_scene);
    uint32_t objects = read32(local_scene + 4);
    if (!materials || materials > MAX_MATERIALS || !objects ||
        objects > MAX_OBJECTS) {
        return 23;
    }
    uint32_t size = 56 + 24 * materials + 40 * objects;
    if (job.scene_addr > 0x84000000 - size) {
        return 24;
    }
    error =
        copy_dma(job.scene_addr + 56, (uintptr_t)local_scene + 56, size - 56);
    if (error) {
        return error;
    }
    return render_local(local_job, local_scene, size, address, write_row);
}
static int
execute(uint64_t job)
{
#ifdef ACCEL_TEST
    static int checked_tcm, tcm_error;
    if (!checked_tcm) {
        tcm_error = check_tcm();
        checked_tcm = 1;
    }
#endif
    int error = run_job(job);
#ifdef ACCEL_TEST
    if (tcm_error) {
        error = tcm_error;
    }
#endif
    return error;
}

int
main(void)
{
    accelerator_loop(execute);
}
