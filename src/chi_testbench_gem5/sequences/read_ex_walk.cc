/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cstring>
#include <vector>

#include "base/logging.hh"
#include "base/trace.hh"
#include "chi_testbench_gem5/driver.hh"
#include "chi_testbench_gem5/sequence_context.hh"
#include "chi_testbench_gem5/sequences/registry.hh"
#include "debug/ChiTestbenchGem5.hh"

namespace gem5
{
namespace chi_gem5tb
{
namespace
{

/*
 * Three-tile scripted walk demonstrating CHI exclusive-ownership
 * acquisition and invalidation. The scenario exercises
 * `MemCmd::ReadExReq` (via role C's first call), but since gem5's
 * Ruby CHI currently collapses ReadExReq to plain ReadShared at the
 * sequencer (see ChiSeqDriver::read_exclusive docstring), the
 * observable CHI ReadUnique / CleanUnique opcode actually comes out
 * of role C's *write* — which is the natural way to force exclusive
 * ownership in gem5's CHI today.
 *
 * Role A: plain read     → CHI ReadShared. Line ends up Shared@A.
 * Role B: plain read     → CHI ReadShared. Line ends up Shared@{A,B}.
 * Role C: read_exclusive → intended ReadUnique; in current CHI emits
 *                          ReadShared, so C becomes Shared@{A,B,C}.
 * Role C: write          → triggers CleanUnique upgrade at C,
 *                          invalidates A and B via SnpCleanInvalid.
 * Role A: re-read        → fresh miss (A was invalidated).
 * Role C: write again    → L1 hit, no additional interconnect traffic.
 *
 * Rendezvous events on the shared ChiEventBus:
 *   after_a       — A signals after its initial read
 *   after_b       — B signals after its read
 *   after_c_read  — C signals after its read_exclusive
 *   after_c_write — C signals after its CleanUnique-triggering write
 *   after_a_reread — A signals after its post-invalidation re-read
 *
 * Expected stats (cpu0 = A, cpu8 = B, cpu15 = C):
 *   cpu0.l1d  hits=0  misses=2   (step 1 cold miss, step 6 post-invalidation
 *                                  miss)
 *   cpu0.l1d.SnpCleanInvalid=1   (invalidation from C's CleanUnique)
 *   cpu8.l1d  hits=0  misses=1   (step 2 cold miss; no re-read)
 *   cpu8.l1d.SnpCleanInvalid=1   (invalidation from C's CleanUnique)
 *   cpu15.l1d hits=1  misses=2   (step 3 ReadShared, step 5 upgrade,
 *                                  step 5b local hit after upgrade)
 *   cpu15.l1d.SendReadShared=1   (step 3)
 *   cpu15.l1d.SendCleanUnique=1  (step 5 upgrade — demonstrates the
 *                                  Shared→Unique transition that is the
 *                                  point of this scenario)
 */

void
role_a(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    const auto &p = drv.params();
    std::vector<uint8_t> buf(p.access_size, 0);

    DPRINTF(ChiTestbenchGem5, "%s A: step 1 plain read\n", drv.name());
    drv.read(p.target_addr, buf.data(), p.access_size);
    drv.notify("after_a");

    // Wait for B to also take a Shared copy, for C's read_exclusive
    // to complete, and crucially for C's WRITE to complete — that's
    // the step that actually invalidates A in current CHI.
    drv.wait_on("after_c_write");

    DPRINTF(ChiTestbenchGem5,
            "%s A: step 6 re-read (expect fresh miss after invalidation)\n",
            drv.name());
    drv.read(p.target_addr, buf.data(), p.access_size);
    drv.notify("after_a_reread");
}

void
role_b(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    const auto &p = drv.params();
    std::vector<uint8_t> buf(p.access_size, 0);

    drv.wait_on("after_a");

    DPRINTF(ChiTestbenchGem5, "%s B: step 2 plain read\n", drv.name());
    drv.read(p.target_addr, buf.data(), p.access_size);
    drv.notify("after_b");
}

void
role_c(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    const auto &p = drv.params();
    std::vector<uint8_t> buf(p.access_size, 0xC0);

    drv.wait_on("after_b");

    // Step 3: read_exclusive. API intent: "read with exclusive
    // intent" (→ ReadUnique). Current CHI reality: collapses to
    // plain ReadShared. C ends up with a Shared copy.
    DPRINTF(ChiTestbenchGem5, "%s C: step 3 read_exclusive\n", drv.name());
    drv.read_exclusive(p.target_addr, buf.data(), p.access_size);
    drv.notify("after_c_read");

    // Step 5: write. C holds Shared, so CHI emits CleanUnique to
    // upgrade, which fans out SnpCleanInvalid to A and B.
    std::memset(buf.data(), 0xC5, p.access_size);
    DPRINTF(ChiTestbenchGem5,
            "%s C: step 5 write (triggers CleanUnique + invalidations)\n",
            drv.name());
    drv.write(p.target_addr, buf.data(), p.access_size);

    // Step 5b: write again. Do this BEFORE notifying — while A is
    // still blocked on "after_c_write", the HNF can't fire a
    // downgrade snoop at C (no one has a competing request). So the
    // line is still Unique@C and this write should hit in L1 with
    // zero additional interconnect traffic. That's the observable
    // "upgrade-free" assertion of this scenario.
    std::memset(buf.data(), 0xC5, p.access_size);
    DPRINTF(ChiTestbenchGem5,
            "%s C: step 5b local write (line already Unique)\n", drv.name());
    drv.write(p.target_addr, buf.data(), p.access_size);

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
} // namespace chi_gem5tb
} // namespace gem5
