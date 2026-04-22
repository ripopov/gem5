/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cstring>
#include <vector>

#include "base/logging.hh"
#include "base/trace.hh"
#include "chi_testbench_gem5_v2/driver.hh"
#include "chi_testbench_gem5_v2/sequence_context.hh"
#include "chi_testbench_gem5_v2/sequences/registry.hh"
#include "debug/ChiTestbenchGem5V2.hh"

namespace gem5
{
namespace chi_gem5tb_v2
{
namespace
{

/*
 * Three-tile scripted walk. The driver has no local cache, but each
 * tile does (rnf_l2 mode), so the test exercises tile-side Shared →
 * Invalidated transitions triggered by the third tile's write.
 *
 * Role A (tile 0)  : ReadShared        → tile-A becomes SC
 * Role B (tile 8)  : ReadShared        → tile-B becomes SC
 * Role C (tile 15) : WriteUniqueFull   → HN sends SnpCleanInvalid to
 *                                         tile-A and tile-B, writes
 *                                         the line from tile-C's side.
 * Role A : ReadShared again            → tile-A refills from HN.
 *
 * Rendezvous events on the shared ChiGem5V2EventBus:
 *   after_a        A signals after its initial read
 *   after_b        B signals after its read
 *   after_c_write  C signals after the invalidating write
 *   after_a_reread A signals after its post-invalidation re-read
 */

void
role_a(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    const auto &p = drv.params();
    std::vector<uint8_t> buf(p.access_size, 0);

    DPRINTF(ChiTestbenchGem5V2, "%s A: step 1 ReadShared\n", drv.name());
    drv.read_shared(p.target_addr, buf.data(), p.access_size);
    drv.notify("after_a");

    drv.wait_on("after_c_write");

    DPRINTF(ChiTestbenchGem5V2,
            "%s A: step 4 ReadShared (post-invalidation)\n", drv.name());
    drv.read_shared(p.target_addr, buf.data(), p.access_size);
    drv.notify("after_a_reread");
}

void
role_b(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    const auto &p = drv.params();
    std::vector<uint8_t> buf(p.access_size, 0);

    drv.wait_on("after_a");

    DPRINTF(ChiTestbenchGem5V2, "%s B: step 2 ReadShared\n", drv.name());
    drv.read_shared(p.target_addr, buf.data(), p.access_size);
    drv.notify("after_b");
}

void
role_c(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    const auto &p = drv.params();
    std::vector<uint8_t> buf(p.access_size, 0xC5);

    drv.wait_on("after_b");

    std::memset(buf.data(), 0xC5, p.access_size);
    DPRINTF(ChiTestbenchGem5V2,
            "%s C: step 3 WriteUniqueFull (invalidates A and B)\n",
            drv.name());
    drv.write_unique_full(p.target_addr, buf.data(), p.access_size);

    drv.notify("after_c_write");
    drv.wait_on("after_a_reread");
}

void
read_ex_walk_seq(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    if (!drv.event_bus()) {
        panic("%s read_ex_walk: event_bus param is required", drv.name());
    }
    const std::string &role = drv.params().role;
    if (role == "A") {
        role_a(ctx);
    } else if (role == "B") {
        role_b(ctx);
    } else if (role == "C") {
        role_c(ctx);
    } else {
        panic("%s read_ex_walk: role must be one of A/B/C (got '%s')",
              drv.name(), role.c_str());
    }
}

[[maybe_unused]] Registrar _r("read_ex_walk", &read_ex_walk_seq);

} // namespace
} // namespace chi_gem5tb_v2
} // namespace gem5
