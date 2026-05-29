/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/sequences/memset.hh"

#include <algorithm>
#include <string>
#include <vector>

#include "base/cprintf.hh"
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

struct RoiRecord
{
    std::string name;
    uint32_t tile_id;
    uint64_t bytes;
    Tick clock_period;
};

class MemsetRoiCoordinator
{
  public:
    void wait_for_warmup(ChiSeqDriver &drv, uint32_t participants);
    void wait_for_start(ChiSeqDriver &drv, uint32_t participants);
    void wait_for_end(ChiSeqDriver &drv, uint32_t participants,
                      uint64_t roi_bytes);

  private:
    void configure(uint32_t participants);
    void release_waiters();
    void report() const;

    uint32_t expected = 0;
    uint32_t warmup_arrivals = 0;
    uint32_t start_arrivals = 0;
    uint32_t end_arrivals = 0;
    Tick roi_start = 0;
    Tick roi_end = 0;
    std::vector<SeqThread *> waiters;
    std::vector<RoiRecord> records;
};

MemsetRoiCoordinator roi_coordinator;

void
MemsetRoiCoordinator::configure(uint32_t participants)
{
    panic_if(participants == 0,
             "Memset ROI synchronization requires participants > 0");
    if (expected == 0) {
        expected = participants;
        records.reserve(expected);
        waiters.reserve(expected);
    } else {
        panic_if(expected != participants,
                 "Memset ROI participant mismatch: expected %u got %u",
                 expected, participants);
    }
}

void
MemsetRoiCoordinator::release_waiters()
{
    for (auto *waiter : waiters) {
        waiter->drv().schedule_xfer_wake();
    }
    waiters.clear();
}

void
MemsetRoiCoordinator::wait_for_warmup(ChiSeqDriver &drv,
                                      uint32_t participants)
{
    configure(participants);
    warmup_arrivals++;

    if (warmup_arrivals == expected) {
        const Tick warmup_end = curTick();
        cprintf("memset L3 warmup: dump stats at tick %llu\n",
                (unsigned long long)warmup_end);
        // statistics::dump();
        cprintf("memset L3 warmup: reset stats at tick %llu\n",
                (unsigned long long)curTick());
        statistics::reset();
        release_waiters();
        return;
    }

    panic_if(warmup_arrivals > expected,
             "Memset L3 warmup barrier received too many arrivals");
    waiters.push_back(&drv.thread());
    drv.thread().yield_to_primary();
}

void
MemsetRoiCoordinator::wait_for_start(ChiSeqDriver &drv,
                                     uint32_t participants)
{
    configure(participants);
    start_arrivals++;

    if (start_arrivals == expected) {
        roi_start = curTick();
        cprintf("memset ROI: reset stats at tick %llu\n",
                (unsigned long long)roi_start);
        statistics::reset();
        release_waiters();
        return;
    }

    panic_if(start_arrivals > expected,
             "Memset ROI start barrier received too many arrivals");
    waiters.push_back(&drv.thread());
    drv.thread().yield_to_primary();
}

void
MemsetRoiCoordinator::wait_for_end(ChiSeqDriver &drv, uint32_t participants,
                                   uint64_t roi_bytes)
{
    configure(participants);
    records.push_back(
        {drv.name(), drv.tile_id(), roi_bytes, drv.clockPeriod()});
    end_arrivals++;

    if (end_arrivals == expected) {
        roi_end = curTick();
        cprintf("memset ROI: dump stats at tick %llu\n",
                (unsigned long long)roi_end);
        statistics::dump();
        report();
        release_waiters();
        return;
    }

    panic_if(end_arrivals > expected,
             "Memset ROI end barrier received too many arrivals");
    waiters.push_back(&drv.thread());
    drv.thread().yield_to_primary();
}

void
MemsetRoiCoordinator::report() const
{
    const Tick elapsed = roi_end - roi_start;
    panic_if(elapsed == 0, "Memset ROI elapsed time is zero");

    std::vector<RoiRecord> sorted = records;
    std::sort(sorted.begin(), sorted.end(),
              [](const RoiRecord &a, const RoiRecord &b) {
                  return a.tile_id < b.tile_id;
              });

    double sum_bw = 0.0;
    cprintf("memset ROI bandwidth: ticks=%llu participants=%u\n",
            (unsigned long long)elapsed, expected);
    for (const auto &record : sorted) {
        const double clocks =
            static_cast<double>(elapsed) / record.clock_period;
        const double bw = static_cast<double>(record.bytes) / clocks;
        sum_bw += bw;
        cprintf("%s memset ROI BW: %.6f Bytes/Clock "
                "(%llu bytes over %.0f clocks)\n",
                record.name.c_str(), bw,
                (unsigned long long)record.bytes, clocks);
    }
    cprintf("memset ROI average BW: %.6f Bytes/Clock per core\n",
            sum_bw / expected);
}

void
run_write_phase(ChiSeqDriver &drv, uint64_t dst_base, uint32_t first_line,
                uint32_t num_lines, uint32_t line_size, uint32_t write_size,
                uint32_t depth, const std::vector<uint8_t> &buffer,
                bool store_line)
{
    uint32_t next_issue = 0;
    uint32_t retired = 0;

    while (retired < num_lines) {
        while (next_issue < num_lines && drv.outstanding() < depth &&
               drv.request_ready()) {
            const uint64_t line = first_line + next_issue;
            const uint64_t addr = dst_base + line * line_size;
            if (store_line) {
                drv.async_store_line_req(addr, buffer.data(), write_size);
            } else {
                drv.async_write_req(addr, buffer.data(), write_size);
            }
            next_issue++;
        }

        if (auto resp = drv.try_read_resp()) {
            retired++;
            continue;
        }

        if (drv.outstanding() != 0) {
            drv.wait_cycles(Cycles(1));
        }
    }
}

void
quiesce_before_stats_reset(ChiSeqDriver &drv, Cycles cycles)
{
    if (cycles != Cycles(0)) {
        drv.wait_cycles(cycles);
    }
}

} // anonymous namespace

void
MemsetSequence::run(ChiSeqDriver &drv)
{
    const uint32_t num_lines = _p.num_lines;
    const uint32_t line_size = _p.line_size;
    const uint32_t write_size = _p.write_size ? _p.write_size : line_size;
    uint32_t depth = _p.num_outstanding_reqs;
    if (depth == 0) {
        depth = _p.pipeline_depth;
    }
    if (depth == 0) {
        depth = 1;
    }
    const uint64_t dst_base = _p.dst_base;

    if (num_lines == 0) {
        return;
    }

    panic_if(write_size == 0 || write_size > line_size,
             "%s memset: write_size=%u must be in [1, line_size=%u]",
             drv.name(), write_size, line_size);

    std::vector<uint8_t> buffer(
        write_size, static_cast<uint8_t>(_p.fill_byte));
    std::vector<uint8_t> warmup_buffer(
        line_size, static_cast<uint8_t>(_p.fill_byte));

    DPRINTF(ChiTestbenchGem5,
            "%s memset: dst_base=%#llx num_lines=%u depth=%u "
            "line_size=%u write_size=%u\n",
            drv.name(), (unsigned long long)dst_base, num_lines, depth,
            line_size, write_size);

    if (_p.warmup_l3) {
        DPRINTF(ChiTestbenchGem5,
                "%s memset L3 warmup: %u full-line stores\n",
                drv.name(), num_lines);
        run_write_phase(drv, dst_base, 0, num_lines, line_size, line_size,
                        depth, warmup_buffer, true);
        quiesce_before_stats_reset(drv, _p.stats_quiesce_cycles);
        roi_coordinator.wait_for_warmup(drv, _p.roi_participants);
    }

    cprintf("memset L3 warmup: reset stats at tick %llu\n",
        (unsigned long long)curTick());
    statistics::reset();

    const Tick t_start = curTick();

    if (_p.roi_stats) {
        const uint32_t ramp_up_lines = num_lines / 10;
        const uint32_t ramp_down_lines = num_lines / 10;
        const uint32_t roi_first = ramp_up_lines;
        const uint32_t roi_lines =
            num_lines - ramp_up_lines - ramp_down_lines;
        const uint32_t ramp_down_first = roi_first + roi_lines;

        cprintf("%s memset ROI split: ramp-up=%u ROI=%u "
                "ramp-down=%u transactions\n",
                drv.name(), ramp_up_lines, roi_lines, ramp_down_lines);

        // Drain warm-up before the reset so in-flight warm-up responses
        // cannot leak into the ROI stats window. Ramp-down is not issued
        // until after the ROI dump for the same reason.
        run_write_phase(drv, dst_base, 0, ramp_up_lines, line_size,
                        write_size, depth, buffer, false);
        quiesce_before_stats_reset(drv, _p.stats_quiesce_cycles);
        roi_coordinator.wait_for_start(drv, _p.roi_participants);

        run_write_phase(drv, dst_base, roi_first, roi_lines, line_size,
                        write_size, depth, buffer, false);
        roi_coordinator.wait_for_end(
            drv, _p.roi_participants, (uint64_t)roi_lines * line_size);

        run_write_phase(drv, dst_base, ramp_down_first, ramp_down_lines,
                        line_size, write_size, depth, buffer, false);
    } else {
        run_write_phase(drv, dst_base, 0, num_lines, line_size, write_size,
                        depth, buffer, false);
    }

    const Tick elapsed = curTick() - t_start;
    const uint64_t bytes = (uint64_t)num_lines * line_size;
    cprintf("%s memset: %u lines (%llu counted bytes, %u-byte stores) "
            "in %llu ticks depth=%u\n",
            drv.name(), num_lines, (unsigned long long)bytes, write_size,
            (unsigned long long)elapsed, depth);
}

} // namespace chi_gem5tb
} // namespace gem5
