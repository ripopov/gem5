/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "systemc/chi_testbench/driver_base.hh"

#include <vector>

#include "base/logging.hh"
#include "systemc/ext/core/sc_main.hh"
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
      finish_barrier(p.finish_barrier),
      next_handle(1)
{
    // Spawn the test thread. sc_spawn binds through std::function so
    // virtual dispatch into the derived class's run() works correctly.
    sc_core::sc_spawn([this]() { this->thread_entry(); }, "run");

    // Register the TLM backward-path callback so async_* requests get
    // a BEGIN_RESP notification.
    iSocket.register_nb_transport_bw(this, &ChiDriverBase::nb_transport_bw);
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

// ---------------------------------------------------------------------------
// Non-blocking (AT) path
// ---------------------------------------------------------------------------

ChiDriverBase::Handle
ChiDriverBase::allocate_handle(tlm::tlm_generic_payload *trans)
{
    const Handle h = next_handle++;
    auto *ev = new sc_core::sc_event(); // default-named; lives inside driver
    inflight.emplace(h, InFlight{trans, ev, false});
    trans_to_handle[trans] = h;
    return h;
}

void
ChiDriverBase::release_handle(Handle h)
{
    auto it = inflight.find(h);
    if (it == inflight.end()) {
        panic("ChiDriverBase %s: resolve() on unknown handle %lu", name(),
              (unsigned long)h);
    }
    trans_to_handle.erase(it->second.trans);
    delete it->second.done;
    // Payload lifetime is managed by the memory manager; release()
    // decrements the refcount and (when it hits zero) returns the
    // payload to the mm free-list.
    it->second.trans->release();
    inflight.erase(it);
}

ChiDriverBase::Handle
ChiDriverBase::submit_async(tlm::tlm_generic_payload *trans)
{
    const Handle h = allocate_handle(trans);

    tlm::tlm_phase phase = tlm::BEGIN_REQ;
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    auto status = iSocket->nb_transport_fw(*trans, phase, delay);

    // Handle the synchronous-completion case (rare but legal):
    // bridge may return TLM_COMPLETED/TLM_UPDATED+BEGIN_RESP right
    // away. Treat both as "response in hand" and mark the handle as
    // completed so resolve() returns immediately.
    if (status == tlm::TLM_COMPLETED ||
        (status == tlm::TLM_UPDATED && phase == tlm::BEGIN_RESP)) {
        auto &ife = inflight[h];
        ife.completed = true;
        ife.done->notify();
    }
    return h;
}

ChiDriverBase::Handle
ChiDriverBase::async_read(uint64_t addr, uint8_t *data_out, uint32_t len)
{
    // mm.allocate() returns a payload with mm set (acquire/release works).
    auto *trans = mm.allocate();
    trans->acquire();
    trans->set_address(addr);
    trans->set_data_length(len);
    trans->set_streaming_width(len);
    trans->set_command(tlm::TLM_READ_COMMAND);
    trans->set_data_ptr(data_out);
    trans->set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
    return submit_async(trans);
}

ChiDriverBase::Handle
ChiDriverBase::async_write(uint64_t addr, const uint8_t *data, uint32_t len)
{
    auto *trans = mm.allocate();
    trans->acquire();
    trans->set_address(addr);
    trans->set_data_length(len);
    trans->set_streaming_width(len);
    trans->set_command(tlm::TLM_WRITE_COMMAND);
    trans->set_data_ptr(const_cast<uint8_t *>(data));
    trans->set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
    return submit_async(trans);
}

void
ChiDriverBase::resolve(Handle h)
{
    auto it = inflight.find(h);
    if (it == inflight.end()) {
        panic("ChiDriverBase %s: resolve() on unknown handle %lu", name(),
              (unsigned long)h);
    }
    // If not yet completed, block until nb_transport_bw fires the
    // handle's sc_event. Reading `completed` and entering wait()
    // cannot be interleaved with other threads (SC_THREAD is
    // cooperative), so there is no lost-wakeup race.
    if (!it->second.completed) {
        sc_core::wait(*it->second.done);
    }
    if (it->second.trans->is_response_error()) {
        panic("ChiDriverBase %s: async transaction to %#lx failed: %s", name(),
              (unsigned long)it->second.trans->get_address(),
              it->second.trans->get_response_string().c_str());
    }
    release_handle(h);
}

void
ChiDriverBase::resolve_all()
{
    // Snapshot the handle list because release_handle() mutates
    // `inflight`.
    std::vector<Handle> keys;
    keys.reserve(inflight.size());
    for (auto &kv : inflight) {
        keys.push_back(kv.first);
    }
    for (Handle h : keys) {
        resolve(h);
    }
}

tlm::tlm_sync_enum
ChiDriverBase::nb_transport_bw(tlm::tlm_generic_payload &trans,
                               tlm::tlm_phase &phase, sc_core::sc_time & /*t*/)
{
    if (phase != tlm::BEGIN_RESP) {
        // Only BEGIN_RESP is meaningful on the initiator's backward
        // path for the simple AT protocol we use.
        return tlm::TLM_ACCEPTED;
    }
    auto it = trans_to_handle.find(&trans);
    if (it == trans_to_handle.end()) {
        panic("ChiDriverBase %s: nb_transport_bw for unknown trans %p", name(),
              (void *)&trans);
    }
    auto &ife = inflight[it->second];
    ife.completed = true;
    ife.done->notify(); // immediate; the waiter runs on its next turn

    // Signal to the bridge that we accept the response and it should
    // consider the transaction done (no END_RESP follow-up).
    phase = tlm::END_RESP;
    return tlm::TLM_COMPLETED;
}

} // namespace chi_testbench
} // namespace gem5
