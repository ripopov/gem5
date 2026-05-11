/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/sequences/ping_pong.hh"

#include <cstdint>

#include "base/cprintf.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "chi_testbench_gem5/driver.hh"
#include "debug/ChiTestbenchGem5.hh"

namespace gem5
{
namespace chi_gem5tb
{

namespace
{

constexpr uint8_t PingerTurn = 0;
constexpr uint8_t PongerTurn = 1;
constexpr uint32_t WarmupRounds = 1;

void
write_turn(ChiSeqDriver &drv, uint64_t addr, uint8_t turn)
{
    drv.write(addr, &turn, sizeof(turn));
}

void
wait_for_turn(ChiSeqDriver &drv, uint64_t addr, uint8_t turn)
{
    uint8_t seen = 0;
    do {
        drv.read(addr, &seen, sizeof(seen));
    } while (seen != turn);
}

} // namespace

void
PingPongSequence::run(ChiSeqDriver &drv)
{
    const uint32_t iters = _p.iterations;
    panic_if(iters == 0, "%s ping_pong: iterations must be non-zero",
             drv.name());
    panic_if(_p.l3_clock == 0, "%s ping_pong: l3_clock must be non-zero",
             drv.name());

    DPRINTF(ChiTestbenchGem5,
            "%s ping_pong: role=%s line=%#llx iters=%u l3_clock=%llu\n",
            drv.name(), _p.initiator ? "pinger" : "ponger",
            (unsigned long long)_p.line_addr, iters,
            (unsigned long long)_p.l3_clock);

    const uint32_t rounds = iters + WarmupRounds;

    if (_p.initiator) {
        write_turn(drv, _p.line_addr, PingerTurn);

        Tick start = 0;
        for (uint32_t i = 0; i < rounds; i++) {
            if (i == WarmupRounds) {
                start = curTick();
            }
            write_turn(drv, _p.line_addr, PongerTurn);
            wait_for_turn(drv, _p.line_addr, PingerTurn);
        }

        const Tick elapsed = curTick() - start;
        const double avg_ticks = double(elapsed) / double(iters);
        const double avg_l3 = avg_ticks / double(_p.l3_clock);

        cprintf("ping_pong: tile %u line %#llx iterations %u "
                "avg_iteration %.2f ticks %.2f L3 clocks\n",
                drv.tile_id(), (unsigned long long)_p.line_addr, iters,
                avg_ticks, avg_l3);
    } else {
        for (uint32_t i = 0; i < rounds; i++) {
            wait_for_turn(drv, _p.line_addr, PongerTurn);
            write_turn(drv, _p.line_addr, PingerTurn);
        }
    }
}

} // namespace chi_gem5tb
} // namespace gem5
