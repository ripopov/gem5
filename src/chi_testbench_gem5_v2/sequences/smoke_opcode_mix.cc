/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cstdint>

#include "chi_testbench_gem5_v2/driver.hh"
#include "chi_testbench_gem5_v2/sequence_context.hh"
#include "chi_testbench_gem5_v2/sequences/registry.hh"

namespace gem5
{
namespace chi_gem5tb_v2
{
namespace
{

// smoke_opcode_mix — deterministic LCG over ReadShared / WriteUniqueFull.
// `percent_reads` sets the ratio; `seed` gives a reproducible sequence.
void
smoke_opcode_mix_seq(SequenceContext &ctx)
{
    ChiDriverNode &d = ctx.drv;
    const auto &p = d.params();
    const uint32_t access_size = p.access_size ? p.access_size : 8;
    const uint32_t iterations = p.iterations ? p.iterations : 1;
    const uint32_t pct_reads = p.percent_reads;

    // Deterministic LCG so runs are reproducible.
    uint32_t state = p.seed ? p.seed : 0xA5A5A5A5u;
    auto next = [&]() {
        state = state * 1664525u + 1013904223u;
        return state;
    };

    uint8_t buf[64] = {0};
    for (uint32_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = (uint8_t)(0x50 + (i & 0xF));
    }

    for (uint32_t it = 0; it < iterations; ++it) {
        for (auto addr : p.addresses) {
            const uint32_t r = next() % 100;
            if (r < pct_reads) {
                d.read_shared(addr, buf,
                              access_size <= sizeof(buf) ? access_size : 8);
            } else {
                d.write_unique_full(
                    addr, buf, access_size <= sizeof(buf) ? access_size : 8);
            }
        }
    }
}

[[maybe_unused]] Registrar _r("smoke_opcode_mix", &smoke_opcode_mix_seq);

} // namespace
} // namespace chi_gem5tb_v2
} // namespace gem5
