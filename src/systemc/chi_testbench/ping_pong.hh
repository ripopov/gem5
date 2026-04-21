/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_PING_PONG_HH__
#define __SYSTEMC_CHI_TESTBENCH_PING_PONG_HH__

#include "params/PingPongDriver.hh"
#include "systemc/chi_testbench/driver_base.hh"
#include "systemc/chi_testbench/event_bus.hh"

namespace gem5
{
namespace chi_testbench
{

/**
 * Two instances of this driver at different tiles cooperate to play
 * ownership ping-pong on a single cache line via named events on a
 * shared ChiEventBus:
 *
 *   driver with initiator=true:
 *     loop:
 *       write(line, data)
 *       bus.notify(post_event)
 *       bus.wait_on(wait_event)
 *
 *   driver with initiator=false:
 *     loop:
 *       bus.wait_on(wait_event)
 *       write(line, data)
 *       bus.notify(post_event)
 *
 * The initiator's first action is the b_transport write which
 * suspends; that guarantees the peer reaches its wait before the
 * initiator's first notify fires.
 *
 * Per-iteration latency (wall-clock time from starting a write to its
 * response) is printed via the driver's own log.
 */
class PingPongDriver : public ChiDriverBase
{
  public:
    using Params = PingPongDriverParams;
    PingPongDriver(const Params &p, const sc_core::sc_module_name &mn);

  protected:
    void run() override;

  private:
    const uint64_t line_addr;
    const uint32_t access_size;
    const uint32_t iterations;
    const bool initiator;
    const std::string wait_event;
    const std::string post_event;
    ChiEventBus *const bus;
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_PING_PONG_HH__
