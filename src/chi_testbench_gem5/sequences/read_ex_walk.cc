/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/sequences/read_ex_walk.hh"

#include <cstring>
#include <vector>

#include "base/logging.hh"
#include "base/trace.hh"
#include "chi_testbench_gem5/driver.hh"
#include "debug/ChiTestbenchGem5.hh"

namespace gem5
{
namespace chi_gem5tb
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
 */

namespace
{

void
role_a(ChiSeqDriver &drv, const ReadExWalkSequenceParams &p)
{
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
role_b(ChiSeqDriver &drv, const ReadExWalkSequenceParams &p)
{
    std::vector<uint8_t> buf(p.access_size, 0);

    drv.wait_on("after_a");

    DPRINTF(ChiTestbenchGem5, "%s B: step 2 plain read\n", drv.name());
    drv.read(p.target_addr, buf.data(), p.access_size);
    drv.notify("after_b");
}

void
role_c(ChiSeqDriver &drv, const ReadExWalkSequenceParams &p)
{
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

} // namespace

void
ReadExWalkSequence::run(ChiSeqDriver &drv)
{
    if (!drv.event_bus()) {
        panic("%s read_ex_walk: event_bus param is required", drv.name());
    }
    const std::string &role = _p.role;
    if (role == "A") {
        role_a(drv, _p);
    } else if (role == "B") {
        role_b(drv, _p);
    } else if (role == "C") {
        role_c(drv, _p);
    } else {
        panic("%s read_ex_walk: role must be one of A/B/C (got '%s')",
              drv.name(), role.c_str());
    }
}

} // namespace chi_gem5tb
} // namespace gem5
