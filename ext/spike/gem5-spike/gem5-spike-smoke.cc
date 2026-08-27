/*
 * Standalone unit test for the gem5 SpikeCPU adapter
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Exercises the adapter without gem5: two independent harts, direct-mapped
 * guest RAM, an MMIO access routed back to the embedded "platform", the gem5
 * pseudo-instruction exit, and translation invalidation.
 */

#include "gem5-spike.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{

constexpr uint64_t RamBase = 0x80000000ULL;
constexpr uint64_t RamSize = 1ULL << 20;
constexpr uint64_t DeviceBase = 0x10000000ULL;

struct Hart
{
    uint32_t id;
    std::vector<uint8_t> ram;
    uint64_t deviceValue = 0;
    unsigned deviceWrites = 0;
    unsigned stopRequests = 0;

    Hart(uint32_t id) : id(id), ram(RamSize, 0) {}
};

int
memoryRead(void *opaque, uint64_t address, uint8_t *data, size_t size)
{
    Hart *hart = static_cast<Hart *>(opaque);
    if (address < DeviceBase || address >= DeviceBase + 8) {
        return -1;
    }
    std::memcpy(data, &hart->deviceValue, size);
    return 0;
}

int
memoryWrite(void *opaque, uint64_t address, const uint8_t *data, size_t size)
{
    Hart *hart = static_cast<Hart *>(opaque);
    if (address < DeviceBase || address >= DeviceBase + 8) {
        return -1;
    }
    hart->deviceValue = 0;
    std::memcpy(&hart->deviceValue, data, size);
    ++hart->deviceWrites;
    return 0;
}

int
memoryMap(void *opaque, size_t index, uint64_t *guest, uint64_t *size,
          uint8_t **host, int *writable)
{
    Hart *hart = static_cast<Hart *>(opaque);
    if (index != 0) {
        return -1;
    }
    *guest = RamBase;
    *size = RamSize;
    *host = hart->ram.data();
    *writable = 1;
    return 0;
}

uint64_t
readTime(void *)
{
    return 0x1234;
}

int
shouldStop(void *opaque)
{
    ++static_cast<Hart *>(opaque)->stopRequests;
    return 0;
}

void
store32(Hart &hart, uint64_t address, uint32_t value)
{
    std::memcpy(hart.ram.data() + (address - RamBase), &value, sizeof(value));
}

unsigned failures = 0;

void
check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

} // namespace

int
main()
{
    constexpr unsigned HartCount = 2;
    std::vector<Hart *> harts;

    for (uint32_t index = 0; index < HartCount; ++index) {
        Hart *hart = new Hart(index);
        harts.push_back(hart);

        Gem5SpikeCallbacks callbacks = {};
        callbacks.instance_id = index;
        callbacks.instance_count = HartCount;
        callbacks.hart_id = index;
        callbacks.isa_string = "rv64imafdc_zicsr_zifencei";
        callbacks.privilege_string = "MSU";
        callbacks.max_vaddr_bits = 39;
        callbacks.memory_read = memoryRead;
        callbacks.memory_write = memoryWrite;
        callbacks.memory_map = memoryMap;
        callbacks.read_time = readTime;
        callbacks.should_stop = shouldStop;
        callbacks.opaque = hart;

        check(gem5_spike_init(&callbacks) == 0, "init");
    }

    // addi x1, x0, imm ; lui x3, 0x10000 ; sw x1, 0(x3) ; m5op(0x21)
    for (uint32_t index = 0; index < HartCount; ++index) {
        Hart &hart = *harts[index];
        const uint32_t immediate = 5 + index;
        store32(hart, RamBase + 0, 0x00000093 | (immediate << 20));
        store32(hart, RamBase + 4, 0x100001b7);
        store32(hart, RamBase + 8, 0x0011a023);
        store32(hart, RamBase + 12, 0x0000007b | (0x21u << 25));
        gem5_spike_set_pc(index, RamBase);
        gem5_spike_set_priv(index, 3);
    }

    for (uint32_t index = 0; index < HartCount; ++index) {
        Gem5SpikeRunResult result = {};
        check(gem5_spike_run(index, 1000, &result) == 0, "run");
        check(result.reason == GEM5_SPIKE_EXIT_M5OP, "m5op exit reason");
        check(result.m5_function == 0x21, "m5op function");
        check(gem5_spike_get_pc(index) == RamBase + 12, "pc stops on m5op");
        check(gem5_spike_get_gpr(index, 1) == 5 + index, "independent x1");
        check(harts[index]->deviceWrites == 1, "one mmio store");
        check(harts[index]->deviceValue == 5 + index, "mmio store value");
    }

    // Instruction accounting, with neither an m5op nor an MMIO access to end
    // the batch early: a run of nops closed by a self-branch, cut short by
    // the budget alone. Rewriting the guest image needs the decode cache
    // dropped, exactly as a switch back from another CPU model does.
    Hart &hart = *harts[0];
    store32(hart, RamBase + 4, 0x00000013);  // nop
    store32(hart, RamBase + 8, 0x00000013);  // nop
    store32(hart, RamBase + 12, 0x0000006f); // j .
    gem5_spike_invalidate_translations(0);
    gem5_spike_set_pc(0, RamBase);
    Gem5SpikeRunResult result = {};
    check(gem5_spike_run(0, 32, &result) == 0, "bounded run");
    check(result.reason == GEM5_SPIKE_EXIT_BUDGET, "budget exit reason");
    check(result.instructions == 32, "budget honoured exactly");
    check(hart.stopRequests > 0, "should_stop consulted");

    // CSR and interrupt transfer, and translation invalidation.
    uint64_t value = 0;
    check(gem5_spike_set_csr(0, 0x305, RamBase + 0x40) == 0, "set mtvec");
    check(gem5_spike_get_csr(0, 0x305, &value) == 0 && value == RamBase + 0x40,
          "get mtvec");
    gem5_spike_set_mip(0, 0x80);
    check(gem5_spike_get_mip(0) == 0x80, "mip transfer");
    gem5_spike_invalidate_translations(0);
    check(gem5_spike_get_pc(0) == RamBase + 12, "invalidate keeps pc");
    check(harts[0]->deviceWrites == 1, "batch ends at the mmio access");

    std::printf("gem5-spike-smoke: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
