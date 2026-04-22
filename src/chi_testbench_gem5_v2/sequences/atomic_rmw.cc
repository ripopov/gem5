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
 * atomic_rmw — "fancy" CHI transaction not reachable from v1's
 * CPU/RubySequencer stimulus path: a bare CleanUnique ownership
 * upgrade emitted directly on the wire. The sequencer collapses the
 * MemCmd flavors that would otherwise lead to CleanUnique, so there
 * is no way to issue this from a Store/Load without the Ruby CHI
 * protocol first transitioning the RN through a cached state.
 *
 * Scenario shape (three tiles, rendezvous via ChiGem5V2EventBus):
 *
 *   Role A : ReadShared addr            → tile-A SC
 *   Role B : ReadShared addr            → tile-B SC
 *   Role C : CleanUnique addr (direct)  → HN sends SnpCleanInvalid
 *                                          to tile-A + tile-B, grants
 *                                          Unique to C's request
 *   Role C : WriteUniqueFull addr       → writes new data through HN
 *   Role A : ReadShared addr (re-read)  → fresh miss (tile-A was
 *                                          invalidated by CleanUnique)
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

    DPRINTF(ChiTestbenchGem5V2, "%s A: step 5 ReadShared (refill)\n",
            drv.name());
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
    std::vector<uint8_t> buf(p.access_size, 0);

    drv.wait_on("after_b");

    // Step 3: bare CleanUnique. No data is exchanged — this is a
    // pure ownership-upgrade request.
    DPRINTF(ChiTestbenchGem5V2,
            "%s C: step 3 CleanUnique (direct, no prior cache state)\n",
            drv.name());
    drv.clean_unique(p.target_addr);

    // Step 4: follow up with a real write to exercise the path now
    // that C has ownership. Use a pattern so step 5's re-read sees a
    // data change.
    std::memset(buf.data(), 0xC5, p.access_size);
    DPRINTF(ChiTestbenchGem5V2, "%s C: step 4 WriteUniqueFull\n", drv.name());
    drv.write_unique_full(p.target_addr, buf.data(), p.access_size);

    drv.notify("after_c_write");
    drv.wait_on("after_a_reread");
}

void
atomic_rmw_seq(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    if (!drv.event_bus()) {
        panic("%s atomic_rmw: event_bus param is required", drv.name());
    }
    const std::string &role = drv.params().role;
    if (role == "A") {
        role_a(ctx);
    } else if (role == "B") {
        role_b(ctx);
    } else if (role == "C") {
        role_c(ctx);
    } else {
        panic("%s atomic_rmw: role must be one of A/B/C (got '%s')",
              drv.name(), role.c_str());
    }
}

[[maybe_unused]] Registrar _r("atomic_rmw", &atomic_rmw_seq);

} // namespace
} // namespace chi_gem5tb_v2
} // namespace gem5
