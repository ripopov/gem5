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

// smoke_read — blocking CHI ReadShared to each configured address,
// `iterations` times. The driver suspends its fiber on each request
// and resumes once CompData has been fully assembled and CompAck sent.
void
smoke_read_seq(SequenceContext &ctx)
{
    ChiDriverNode &d = ctx.drv;
    const auto &p = d.params();
    const uint32_t access_size = p.access_size ? p.access_size : 8;
    const uint32_t iterations = p.iterations ? p.iterations : 1;

    uint8_t buf[64] = {0};

    for (uint32_t it = 0; it < iterations; ++it) {
        for (auto addr : p.addresses) {
            d.read_shared(addr, buf,
                          access_size <= sizeof(buf) ? access_size : 8);
        }
    }
}

[[maybe_unused]] Registrar _r("smoke_read", &smoke_read_seq);

} // namespace
} // namespace chi_gem5tb_v2
} // namespace gem5
