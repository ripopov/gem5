/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "systemc/chi_testbench/driver_base.hh"

#include "base/logging.hh"
#include "systemc/ext/core/sc_spawn.hh"
#include "systemc/ext/core/sc_time.hh"

namespace gem5
{
namespace chi_testbench
{

ChiDriverBase::ChiDriverBase(const Params &p,
                             const sc_core::sc_module_name &mn)
    : sc_core::sc_module(mn),
      iSocket("iSocket"),
      iSocketWrapper(nullptr),
      finish_barrier(p.finish_barrier)
{
    // Spawn the test thread. sc_spawn binds through std::function so
    // virtual dispatch into the derived class's run() works correctly.
    sc_core::sc_spawn([this]() { this->thread_entry(); }, "run");
}

void
ChiDriverBase::thread_entry()
{
    // Call the derived-class run(); on return, optionally signal the
    // shared completion barrier. The barrier terminates the SystemC
    // kernel (via sc_stop) once the configured number of drivers have
    // signaled.
    run();
    if (finish_barrier) {
        finish_barrier->signal_finish();
    }
}

gem5::Port &
ChiDriverBase::gem5_getPort(const std::string &if_name, int idx)
{
    if (if_name == "iSocket") {
        if (!iSocketWrapper) {
            iSocketWrapper = new sc_gem5::TlmInitiatorWrapper<CHI_TB_BUSWIDTH>(
                iSocket, std::string(name()) + ".iSocket",
                gem5::InvalidPortID);
        }
        return *iSocketWrapper;
    }
    return sc_core::sc_module::gem5_getPort(if_name, idx);
}

void
ChiDriverBase::read(uint64_t addr, uint8_t *data, uint32_t len)
{
    tlm::tlm_generic_payload trans;
    trans.set_address(addr);
    trans.set_data_length(len);
    trans.set_streaming_width(len);
    trans.set_command(tlm::TLM_READ_COMMAND);
    trans.set_data_ptr(data);
    trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    iSocket->b_transport(trans, delay);

    if (trans.is_response_error()) {
        panic("ChiDriverBase::read(%#x) failed: %s", addr,
              trans.get_response_string().c_str());
    }

    // Apply any protocol-added delay before returning.
    if (delay != sc_core::SC_ZERO_TIME) {
        sc_core::wait(delay);
    }
}

void
ChiDriverBase::write(uint64_t addr, const uint8_t *data, uint32_t len)
{
    tlm::tlm_generic_payload trans;
    trans.set_address(addr);
    trans.set_data_length(len);
    trans.set_streaming_width(len);
    trans.set_command(tlm::TLM_WRITE_COMMAND);
    trans.set_data_ptr(const_cast<uint8_t *>(data));
    trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    iSocket->b_transport(trans, delay);

    if (trans.is_response_error()) {
        panic("ChiDriverBase::write(%#x) failed: %s", addr,
              trans.get_response_string().c_str());
    }

    if (delay != sc_core::SC_ZERO_TIME) {
        sc_core::wait(delay);
    }
}

} // namespace chi_testbench
} // namespace gem5
