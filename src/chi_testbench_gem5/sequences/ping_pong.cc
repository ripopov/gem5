/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/sequences/ping_pong.hh"

#include <cstdint>
#include <vector>

#include "base/cprintf.hh"
#include "base/logging.hh"
#include "base/statistics.hh"
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

bool
is_roi_round(const PingPongSequence::Params &p, uint32_t round)
{
    return round == WarmupRounds + p.roi_iteration;
}

void
write_turn(ChiSeqDriver &drv, const PingPongSequence::Params &p, uint8_t turn)
{
    if (!p.full_line_writes) {
        drv.write(p.line_addr, &turn, sizeof(turn));
        return;
    }

    std::vector<uint8_t> line(p.line_size, 0);
    line[0] = turn;
    drv.write(p.line_addr, line.data(), p.line_size);
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
    panic_if(_p.roi_iteration >= iters,
             "%s ping_pong: roi_iteration must be smaller than iterations",
             drv.name());
    panic_if(_p.l3_clock == 0, "%s ping_pong: l3_clock must be non-zero",
             drv.name());
    panic_if(_p.full_line_writes && _p.line_size == 0,
             "%s ping_pong: line_size must be non-zero", drv.name());

    DPRINTF(ChiTestbenchGem5,
            "%s ping_pong: role=%s line=%#llx iters=%u l3_clock=%llu "
            "full_line_writes=%d line_size=%u\n",
            drv.name(), _p.initiator ? "pinger" : "ponger",
            (unsigned long long)_p.line_addr, iters,
            (unsigned long long)_p.l3_clock, _p.full_line_writes,
            _p.line_size);

    const uint32_t rounds = iters + WarmupRounds;

    if (_p.initiator) {
        write_turn(drv, _p, PingerTurn);

        Tick start = 0;
        for (uint32_t i = 0; i < rounds; i++) {
            if (i == WarmupRounds) {
                start = curTick();
            }
            const bool roi = is_roi_round(_p, i);
            const Tick roi_start = curTick();
            if (roi) {
                cprintf("ping_pong_roi: begin tick %llu tile %u line %#llx "
                        "iteration %u\n",
                        (unsigned long long)roi_start, drv.tile_id(),
                        (unsigned long long)_p.line_addr,
                        i - WarmupRounds);
            }
            write_turn(drv, _p, PongerTurn);
            wait_for_turn(drv, _p.line_addr, PingerTurn);
            if (roi) {
                const Tick roi_ticks = curTick() - roi_start;
                cprintf("ping_pong_roi: end tick %llu tile %u line %#llx "
                        "iteration %u latency %.2f L3 clocks\n",
                        (unsigned long long)curTick(), drv.tile_id(),
                        (unsigned long long)_p.line_addr,
                        i - WarmupRounds,
                        double(roi_ticks) / double(_p.l3_clock));
                if (_p.dump_roi_stats) {
                    statistics::dump();
                }
            }
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
            if (is_roi_round(_p, i)) {
                cprintf("ping_pong_roi: ponger_wait tick %llu tile %u "
                        "line %#llx iteration %u\n",
                        (unsigned long long)curTick(), drv.tile_id(),
                        (unsigned long long)_p.line_addr,
                        i - WarmupRounds);
                if (_p.dump_roi_stats) {
                    statistics::reset();
                }
            }
            wait_for_turn(drv, _p.line_addr, PongerTurn);
            if (is_roi_round(_p, i)) {
                cprintf("ping_pong_roi: ponger_seen tick %llu tile %u "
                        "line %#llx iteration %u\n",
                        (unsigned long long)curTick(), drv.tile_id(),
                        (unsigned long long)_p.line_addr,
                        i - WarmupRounds);
            }
            write_turn(drv, _p, PingerTurn);
        }
    }
}

} // namespace chi_gem5tb
} // namespace gem5
