/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_DRIVER_BASE_HH__
#define __SYSTEMC_CHI_TESTBENCH_DRIVER_BASE_HH__

#include <cstdint>
#include <string>
#include <unordered_map>

#include "params/ChiDriverBase.hh"
#include "systemc/chi_testbench/finish_barrier.hh"
#include "systemc/ext/core/sc_event.hh"
#include "systemc/ext/core/sc_module.hh"
#include "systemc/ext/core/sc_module_name.hh"
#include "systemc/ext/tlm_core/2/generic_payload/gp.hh"
#include "systemc/ext/tlm_utils/simple_initiator_socket.h"
#include "systemc/tlm_bridge/sc_mm.hh"
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

    // --- Non-blocking helpers (many outstanding per SC_THREAD) ---
    //
    // Issue the request via the TLM-2 AT (approximately-timed) path
    // (nb_transport_fw) and return a handle immediately. Call
    // resolve(handle) to wait for that specific transaction to
    // retire, or resolve_all() to wait for every pending handle.
    //
    // The caller-supplied buffers (data for async_write, data_out for
    // async_read) must remain valid until the matching resolve()
    // returns.

    using Handle = uint64_t;

    Handle async_read(uint64_t addr, uint8_t *data_out, uint32_t len);
    Handle async_write(uint64_t addr, const uint8_t *data, uint32_t len);
    void resolve(Handle h);
    void resolve_all();
    std::size_t
    outstanding() const
    {
        return inflight.size();
    }

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

    // --- nb_transport bookkeeping ---

    // One entry per in-flight async_* transaction.
    struct InFlight
    {
        tlm::tlm_generic_payload *trans;
        sc_core::sc_event *done;
        bool completed;
    };

    Handle next_handle;
    std::unordered_map<Handle, InFlight> inflight;
    std::unordered_map<tlm::tlm_generic_payload *, Handle> trans_to_handle;

    // Memory manager for async payloads. TlmToGem5Bridge calls
    // acquire()/release() on the payload, which requires a non-null
    // tlm_mm_interface to be set on the payload.
    Gem5SystemC::MemoryManager mm;

    // Helpers: allocate/free an InFlight entry. `setup_payload` fills
    // the generic_payload with caller-provided fields (addr, len,
    // command, data ptr). `submit_async` issues BEGIN_REQ and records
    // the handle.
    Handle allocate_handle(tlm::tlm_generic_payload *trans);
    void release_handle(Handle h);
    Handle submit_async(tlm::tlm_generic_payload *trans);

    // TLM backward-path callback. Fires when the bridge raises
    // BEGIN_RESP on a pending transaction.
    tlm::tlm_sync_enum nb_transport_bw(tlm::tlm_generic_payload &trans,
                                       tlm::tlm_phase &phase,
                                       sc_core::sc_time &t);
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_DRIVER_BASE_HH__
