/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_DRIVER_BASE_HH__
#define __SYSTEMC_CHI_TESTBENCH_DRIVER_BASE_HH__

#include <cstdint>
#include <string>

#include "params/ChiDriverBase.hh"
#include "systemc/chi_testbench/finish_barrier.hh"
#include "systemc/ext/core/sc_module.hh"
#include "systemc/ext/core/sc_module_name.hh"
#include "systemc/ext/tlm_core/2/generic_payload/gp.hh"
#include "systemc/ext/tlm_utils/simple_initiator_socket.h"
#include "systemc/tlm_port_wrapper.hh"

namespace gem5
{
namespace chi_testbench
{

// Bus width for all TLM sockets in the testbench (bits).
static constexpr unsigned int CHI_TB_BUSWIDTH = 64;

/**
 * Base class for every SystemC CHI test driver.
 *
 * Derived classes implement run(), which is executed as an SC_THREAD.
 * Inside run(), the author calls blocking helpers (read, write, flush)
 * that wrap TLM b_transport on the iSocket. Each blocking call
 * suspends the SC_THREAD until the CHI/Garnet response returns.
 *
 * Non-blocking helpers (async_read/async_write/resolve) are declared
 * here and implemented in Stage 2.
 */
class ChiDriverBase : public sc_core::sc_module
{
  public:
    using Params = ChiDriverBaseParams;

    // Must match the name="iSocket" in ChiDrivers.py so Python-side
    // drv.iSocket = bridge.tlm binds into this socket.
    tlm_utils::simple_initiator_socket<ChiDriverBase, CHI_TB_BUSWIDTH> iSocket;

    ChiDriverBase(const Params &p, const sc_core::sc_module_name &mn);

    // Exposes iSocket to gem5 as a bindable Port.
    gem5::Port &gem5_getPort(const std::string &if_name,
                             int idx = -1) override;

  protected:
    // Derived classes override this. Runs as an SC_THREAD in each
    // instance; calls sc_core::sc_stop() (or simply returns) when
    // done.
    virtual void run() = 0;

    // --- Blocking helpers (one outstanding per SC_THREAD) ---

    // Read `len` bytes starting at addr; buffer caller-provided.
    // Suspends until response.
    void read(uint64_t addr, uint8_t *data, uint32_t len);

    // Write `len` bytes starting at addr. Suspends until response.
    void write(uint64_t addr, const uint8_t *data, uint32_t len);

  private:
    // Trampoline registered with SC_THREAD; dispatches to virtual run()
    // and — if a finish_barrier was supplied — signals completion.
    void thread_entry();

    // Local wrapper that exposes iSocket as a gem5::Port for Python
    // binding. Constructed lazily (on first gem5_getPort call) because
    // iSocket has its SystemC name by then.
    sc_gem5::TlmInitiatorWrapper<CHI_TB_BUSWIDTH> *iSocketWrapper;

    // Optional completion barrier; nullptr when the scenario does not
    // need one.
    ChiFinishBarrier *finish_barrier;
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_DRIVER_BASE_HH__
