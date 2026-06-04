/*
 * Copyright 2019 Google, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Copyright (c) 2015, University of Kaiserslautern
 * Copyright (c) 2016, Dresden University of Technology (TU Dresden)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "systemc/tlm_bridge/gem5_to_tlm.hh"

#include <utility>

#include "params/Gem5ToTlmBridge32.hh"
#include "params/Gem5ToTlmBridge64.hh"
#include "params/Gem5ToTlmBridge128.hh"
#include "params/Gem5ToTlmBridge256.hh"
#include "params/Gem5ToTlmBridge512.hh"
#include "sim/eventq.hh"
#include "sim/system.hh"
#include "systemc/tlm_bridge/sc_ext.hh"
#include "systemc/tlm_bridge/sc_mm.hh"

using namespace gem5;

namespace sc_gem5
{

/**
 * Helper function to help set priority of phase change events of tlm
 * transactions. This is to workaround the uncertainty of gem5 eventq if
 * multiple events are scheduled at the same timestamp.
 */
static EventBase::Priority
getPriorityOfTlmPhase(const tlm::tlm_phase& phase)
{
    // In theory, for all phase change events of a specific TLM base protocol
    // transaction, only tlm::END_REQ and tlm::BEGIN_RESP would be scheduled at
    // the same time in the same queue. So we only need to ensure END_REQ has a
    // higher priority (less in pri value) than BEGIN_RESP.
    if (phase == tlm::END_REQ) {
        return EventBase::Default_Pri - 1;
    }
    return EventBase::Default_Pri;
}

/**
 * Instantiate a tlm memory manager that takes care about all the
 * tlm transactions in the system.
 */
Gem5SystemC::MemoryManager mm;

namespace
{
/**
 * Hold all the callbacks necessary to convert a gem5 packet to tlm payload.
 */
std::vector<PacketToPayloadConversionStep> extraPacketToPayloadSteps;
}  // namespace

/**
 * Notify the Gem5ToTlm bridge that we need an extra step to properly convert a
 * gem5 packet to tlm payload. This can be useful when there exists a SystemC
 * extension that requires information in gem5 packet. For example, if a user
 * defined a SystemC extension the carries stream_id, the user may add a step
 * here to read stream_id out and set the extension properly. Steps should be
 * idempotent.
 */
void
addPacketToPayloadConversionStep(PacketToPayloadConversionStep step)
{
    extraPacketToPayloadSteps.push_back(std::move(step));
}

/**
 * Convert a gem5 packet to TLM payload by copying all the relevant information
 * to new payload. If the transaction is initiated by TLM model, we would use
 * the original payload.
 * The return value is the payload pointer.
 */
tlm::tlm_generic_payload *
packet2payload(PacketPtr packet)
{
    tlm::tlm_generic_payload *trans = nullptr;
    auto *tlmSenderState =
        packet->findNextSenderState<Gem5SystemC::TlmSenderState>();

    // If there is a SenderState, we can pipe through the original transaction.
    // Otherwise, we generate a new transaction based on the packet.
    if (tlmSenderState != nullptr) {
        // Sync the address which could have changed.
        trans = &tlmSenderState->trans;
        trans->set_address(packet->getAddr());
        if (trans->has_mm()) {
            trans->acquire();
        }
        // Apply all conversion steps necessary in this specific setup.
        for (auto &step : extraPacketToPayloadSteps) {
            step(packet, *trans);
        }
        return trans;
    }

    trans = mm.allocate();
    trans->acquire();

    trans->set_address(packet->getAddr());
    trans->set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    /* Check if this transaction was allocated by mm */
    sc_assert(trans->has_mm());

    unsigned int size = packet->getSize();
    unsigned char *data = packet->getPtr<unsigned char>();

    trans->set_data_length(size);
    trans->set_streaming_width(size);
    trans->set_data_ptr(data);

    if ((packet->req->getFlags() & Request::NO_ACCESS) != 0) {
        /* Do nothing */
        trans->set_command(tlm::TLM_IGNORE_COMMAND);
    } else if (packet->isRead()) {
        trans->set_command(tlm::TLM_READ_COMMAND);
    } else if (packet->isWrite()) {
        trans->set_command(tlm::TLM_WRITE_COMMAND);
    } else {
        trans->set_command(tlm::TLM_IGNORE_COMMAND);
    }

    // Attach the packet pointer to the TLM transaction to keep track.
    auto *extension = new Gem5SystemC::Gem5Extension(packet);
    trans->set_auto_extension(extension);

    if (packet->isAtomicOp()) {
        auto *atomic_ex = new Gem5SystemC::AtomicExtension(
            std::shared_ptr<AtomicOpFunctor>(
                packet->req->getAtomicOpFunctor()->clone()),
            packet->req->isAtomicReturn());
        trans->set_auto_extension(atomic_ex);
    }

    // Apply all conversion steps necessary in this specific setup.
    for (auto &step : extraPacketToPayloadSteps) {
        step(packet, *trans);
    }

    return trans;
}

void
setPacketResponse(PacketPtr pkt, tlm::tlm_generic_payload &trans)
{
    pkt->makeResponse();

    auto resp = trans.get_response_status();
    switch (resp) {
      case tlm::TLM_OK_RESPONSE:
        break;
      case tlm::TLM_COMMAND_ERROR_RESPONSE:
        pkt->setBadCommand();
        break;
      default:
        pkt->setBadAddress();
        break;
    }
}

template <unsigned int BITWIDTH>
void
Gem5ToTlmBridge<BITWIDTH>::pec(
        tlm::tlm_generic_payload &trans, const tlm::tlm_phase &phase)
{
    sc_core::sc_time delay;

    if (phase == tlm::END_REQ ||
            (&trans == blockingRequest && phase == tlm::BEGIN_RESP)) {
        sc_assert(&trans == blockingRequest);
        blockingRequest = nullptr;

        // The request channel is free again. Immediately issue the next staged
        // BEGIN_REQ (if any) without waiting for a gem5 round trip, and send a
        // request retry if popping the queue freed space for a stalled gem5
        // requestor. This is what decouples request injection from the
        // gem5<->bridge round trip and lets multiple transactions pipeline.
        sendNextRequest();
    }
    if (phase == tlm::BEGIN_RESP) {
        PacketPtr packet = packetMap[&trans];

        sc_assert(!blockingResponse);
        sc_assert(packet);

        bool need_retry = false;

        // If there is another gem5 model under the receiver side, and already
        // make a response packet back, we can simply send it back. Otherwise,
        // we make a response packet before sending it back to the initiator
        // side gem5 module.
        if (packet->needsResponse()) {
            setPacketResponse(packet, trans);
        }
        if (packet->isResponse()) {
            need_retry = !bridgeResponsePort.sendTimingResp(packet);
        }

        if (need_retry) {
            blockingResponse = &trans;
        } else {
            // Send END_RESP and we're finished:
            tlm::tlm_phase fw_phase = tlm::END_RESP;
            sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
            socket->nb_transport_fw(trans, fw_phase, delay);
            // Release the transaction with all the extensions.
            packetMap.erase(&trans);
            if (trans.has_mm()) {
                trans.release();
            }
        }
    }
}

template <unsigned int BITWIDTH>
MemBackdoorPtr
Gem5ToTlmBridge<BITWIDTH>::getBackdoor(tlm::tlm_generic_payload &trans)
{
    sc_dt::uint64 start = trans.get_address();
    sc_dt::uint64 end = start + trans.get_data_length();

    // Check for a back door we already know about.
    AddrRange r(start, end);
    auto it = backdoorMap.contains(r);
    if (it != backdoorMap.end())
        return it->second;

    // If not, ask the target for one.
    tlm::tlm_dmi dmi_data;
    if (!socket->get_direct_mem_ptr(trans, dmi_data))
        return nullptr;

    // If the target gave us one, translate it to a gem5 MemBackdoor and
    // store it in our cache.
    // The address range presented by tlm::tlm_dmi is inclusive, so when
    // converting to gem5::MemBackdoor, we should + 1 on end address.
    AddrRange dmi_r(dmi_data.get_start_address(),
                    dmi_data.get_end_address() + 1);
    auto backdoor = new MemBackdoor(
            dmi_r, dmi_data.get_dmi_ptr(), MemBackdoor::NoAccess);
    backdoor->readable(dmi_data.is_read_allowed());
    backdoor->writeable(dmi_data.is_write_allowed());

    backdoorMap.insert(dmi_r, backdoor);

    return backdoor;
}

// Similar to TLM's blocking transport (LT)
template <unsigned int BITWIDTH>
Tick
Gem5ToTlmBridge<BITWIDTH>::recvAtomic(PacketPtr packet)
{
    panic_if(packet->cacheResponding(),
             "Should not see packets where cache is responding");

    // Prepare the transaction.
    auto *trans = packet2payload(packet);

    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;

    if (trans->get_command() != tlm::TLM_IGNORE_COMMAND) {
        // Execute b_transport:
        socket->b_transport(*trans, delay);
    }

    if (packet->needsResponse())
        setPacketResponse(packet, *trans);

    if (trans->has_mm()) {
        trans->release();
    }

    return delay.value();
}

template <unsigned int BITWIDTH>
Tick
Gem5ToTlmBridge<BITWIDTH>::recvAtomicBackdoor(
        PacketPtr packet, MemBackdoorPtr &backdoor)
{
    panic_if(packet->cacheResponding(),
             "Should not see packets where cache is responding");

    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;

    // Prepare the transaction.
    auto *trans = packet2payload(packet);

    if (trans->get_command() != tlm::TLM_IGNORE_COMMAND) {
        // Execute b_transport:
        socket->b_transport(*trans, delay);
        // If the hint said we could use DMI, set that up.
        if (trans->is_dmi_allowed())
            backdoor = getBackdoor(*trans);
    } else {
        // There's no transaction to piggy back on, so just request the
        // backdoor normally.
        backdoor = getBackdoor(*trans);
    }

    // Always set success response in Backdoor case.
    if (packet->needsResponse())
        packet->makeResponse();

    if (trans->has_mm()) {
        trans->release();
    }

    return delay.value();
}

template <unsigned int BITWIDTH>
void
Gem5ToTlmBridge<BITWIDTH>::recvFunctionalSnoop(PacketPtr packet)
{
    // Snooping should be implemented with tlm_dbg_transport.
    SC_REPORT_FATAL("Gem5ToTlmBridge",
            "unimplemented func.: recvFunctionalSnoop");
}

// Similar to TLM's non-blocking transport (AT).
template <unsigned int BITWIDTH>
bool
Gem5ToTlmBridge<BITWIDTH>::recvTimingReq(PacketPtr packet)
{
    panic_if(packet->cacheResponding(),
             "Should not see packets where cache is responding");

    // We should never get a second request after noting that a retry is
    // required.
    sc_assert(!needToSendRequestRetry);

    // The TLM base-protocol exclusion rule only allows one transaction in the
    // BEGIN_REQ -> END_REQ window on the socket at a time. Rather than forcing
    // the gem5 requestor to participate in that handshake (which serializes
    // injection to one request per gem5<->bridge round trip), accept gem5
    // requests into a local staging queue and feed BEGIN_REQs into the socket
    // back-to-back from sendNextRequest(). The gem5 side is only back-pressured
    // when the staging queue is full.
    if (requestQueue.size() >= requestQueueDepth) {
        needToSendRequestRetry = true;
        return false;
    }

    /*
     * Pay for annotated transport delays.
     *
     * The header delay marks the point in time when the packet first is seen
     * by the transactor, i.e. when its BEGIN_REQ should become visible to the
     * SystemC world. Record that absolute tick now and clear the annotation;
     * the residual delay (if any) is applied when the BEGIN_REQ is actually
     * issued from sendNextRequest(). Because the packet may wait in the staging
     * queue, that header delay generally elapses in real sim time before the
     * BEGIN_REQ is sent, so it is not re-paid per request (which would
     * re-serialize admission to one request per header-delay interval).
     *
     * NOTE: We drop the payload delay here. Normally, the receiver would be
     *       responsible for handling the payload delay. In this case, however,
     *       the receiver is a SystemC module and has no notion of the gem5
     *       transport protocol and we cannot simply forward the payload delay
     *       to the receiving module. Instead, we expect the receiving SystemC
     *       module to model the payload delay by deferring the END_REQ.
     */
    gem5::Tick beginReqTick = curTick() + packet->headerDelay;
    packet->payloadDelay = 0;
    packet->headerDelay = 0;

    requestQueue.emplace(packet, beginReqTick);
    sendNextRequest();
    return true;
}

template <unsigned int BITWIDTH>
void
Gem5ToTlmBridge<BITWIDTH>::sendNextRequest()
{
    // Issue staged requests while the request channel is free. The exclusion
    // rule is still honored: at most one transaction is between BEGIN_REQ and
    // END_REQ at any time (tracked by blockingRequest). A TLM_COMPLETED
    // transaction does not occupy the request channel, so we keep draining.
    while (blockingRequest == nullptr && !requestQueue.empty()) {
        PacketPtr packet = requestQueue.front().first;
        gem5::Tick beginReqTick = requestQueue.front().second;
        requestQueue.pop();

        /*
         * NOTE: normal tlm is blocking here. But in our case we stage the
         * request and tell gem5 when a retry can be done. This is the main
         * difference in the protocol:
         * if (requestInProgress)
         * {
         *     wait(endRequestEvent);
         * }
         * requestInProgress = trans;
         */

        // Prepare the transaction.
        auto *trans = packet2payload(packet);

        // Apply only the residual header delay: the BEGIN_REQ should become
        // visible at beginReqTick (arrival + header delay). Any time the
        // packet spent waiting in the staging queue has already advanced the
        // clock, so under load this residual is zero and BEGIN_REQs are issued
        // back-to-back instead of one per header-delay interval.
        sc_core::sc_time delay = (beginReqTick > curTick()) ?
            sc_core::sc_time::from_value(beginReqTick - curTick()) :
            sc_core::SC_ZERO_TIME;

        // Starting TLM non-blocking sequence (AT) Refer to IEEE1666-2011
        // SystemC Standard Page 507 for a visualisation of the procedure.
        tlm::tlm_phase phase = tlm::BEGIN_REQ;
        tlm::tlm_sync_enum status;
        status = socket->nb_transport_fw(*trans, phase, delay);
        // Check returned value:
        if (status == tlm::TLM_ACCEPTED) {
            sc_assert(phase == tlm::BEGIN_REQ);
            // Accepted but is now blocking until END_REQ (exclusion rule).
            blockingRequest = trans;
            packetMap.emplace(trans, packet);
        } else if (status == tlm::TLM_UPDATED) {
            // The Timing annotation must be honored:
            sc_assert(phase == tlm::END_REQ || phase == tlm::BEGIN_RESP);
            // Accepted but is now blocking until END_REQ (exclusion rule).
            blockingRequest = trans;
            packetMap.emplace(trans, packet);
            auto cb = [this, trans, phase]() { pec(*trans, phase); };
            auto event = new EventFunctionWrapper(
                    cb, "pec", true, getPriorityOfTlmPhase(phase));
            system->schedule(event, curTick() + delay.value());
        } else if (status == tlm::TLM_COMPLETED) {
            // Transaction is over nothing has do be done.
            sc_assert(phase == tlm::END_RESP);
            if (trans->has_mm()) {
                trans->release();
            }
        }
    }

    // Popping from the staging queue may have freed space for a request the
    // gem5 side previously had to retry.
    checkAndSendRetry();
}

template <unsigned int BITWIDTH>
void
Gem5ToTlmBridge<BITWIDTH>::checkAndSendRetry()
{
    if (needToSendRequestRetry && requestQueue.size() < requestQueueDepth) {
        needToSendRequestRetry = false;
        bridgeResponsePort.sendRetryReq();
    }
}

template <unsigned int BITWIDTH>
bool
Gem5ToTlmBridge<BITWIDTH>::recvTimingSnoopResp(PacketPtr packet)
{
    // Snooping should be implemented with tlm_dbg_transport.
    SC_REPORT_FATAL("Gem5ToTlmBridge",
            "unimplemented func.: recvTimingSnoopResp");
    return false;
}

template <unsigned int BITWIDTH>
bool
Gem5ToTlmBridge<BITWIDTH>::tryTiming(PacketPtr packet)
{
    panic("tryTiming(PacketPtr) isn't implemented.");
}

template <unsigned int BITWIDTH>
void
Gem5ToTlmBridge<BITWIDTH>::recvRespRetry()
{
    /* Retry a response */
    sc_assert(blockingResponse);

    tlm::tlm_generic_payload *trans = blockingResponse;
    blockingResponse = nullptr;

    PacketPtr packet = packetMap[trans];
    sc_assert(packet);

    bool need_retry = !bridgeResponsePort.sendTimingResp(packet);

    sc_assert(!need_retry);

    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    tlm::tlm_phase phase = tlm::END_RESP;
    socket->nb_transport_fw(*trans, phase, delay);
    // Release transaction with all the extensions
    packetMap.erase(trans);
    if (trans->has_mm()) {
        trans->release();
    }
}

// Similar to TLM's debug transport.
template <unsigned int BITWIDTH>
void
Gem5ToTlmBridge<BITWIDTH>::recvFunctional(PacketPtr packet)
{
    // Prepare the transaction.
    auto *trans = packet2payload(packet);

    /* Execute Debug Transport: */
    unsigned int bytes = socket->transport_dbg(*trans);
    if (bytes != trans->get_data_length()) {
        SC_REPORT_FATAL("Gem5ToTlmBridge",
                "debug transport was not completed");
    }

    if (trans->has_mm()) {
        trans->release();
    }
}

template <unsigned int BITWIDTH>
void
Gem5ToTlmBridge<BITWIDTH>::recvMemBackdoorReq(const MemBackdoorReq &req,
        MemBackdoorPtr &backdoor)
{
    // Create a transaction to send along to TLM's get_direct_mem_ptr.
    tlm::tlm_generic_payload *trans = mm.allocate();
    trans->acquire();
    trans->set_address(req.range().start());
    trans->set_data_length(req.range().size());
    trans->set_streaming_width(req.range().size());
    trans->set_data_ptr(nullptr);

    if (req.writeable())
        trans->set_command(tlm::TLM_WRITE_COMMAND);
    else if (req.readable())
        trans->set_command(tlm::TLM_READ_COMMAND);
    else
        trans->set_command(tlm::TLM_IGNORE_COMMAND);

    backdoor = getBackdoor(*trans);

    trans->release();
}

template <unsigned int BITWIDTH>
tlm::tlm_sync_enum
Gem5ToTlmBridge<BITWIDTH>::nb_transport_bw(tlm::tlm_generic_payload &trans,
    tlm::tlm_phase &phase, sc_core::sc_time &delay)
{
    auto cb = [this, &trans, phase]() { pec(trans, phase); };
    auto event = new EventFunctionWrapper(
            cb, "pec", true, getPriorityOfTlmPhase(phase));
    system->schedule(event, curTick() + delay.value());
    return tlm::TLM_ACCEPTED;
}

template <unsigned int BITWIDTH>
void
Gem5ToTlmBridge<BITWIDTH>::invalidate_direct_mem_ptr(
        sc_dt::uint64 start_range, sc_dt::uint64 end_range)
{
    AddrRange r(start_range, end_range);

    for (;;) {
        auto it = backdoorMap.intersects(r);
        if (it == backdoorMap.end())
            break;

        it->second->invalidate();
        delete it->second;
        backdoorMap.erase(it);
    };
}

template <unsigned int BITWIDTH>
Gem5ToTlmBridge<BITWIDTH>::Gem5ToTlmBridge(
        const Params &params, const sc_core::sc_module_name &mn) :
    Gem5ToTlmBridgeBase(mn),
    bridgeResponsePort(std::string(name()) + ".gem5", *this),
    socket("tlm_socket"),
    wrapper(socket, std::string(name()) + ".tlm", InvalidPortID),
    system(params.system), blockingRequest(nullptr),
    needToSendRequestRetry(false),
    requestQueueDepth(params.request_queue_depth ?
                      params.request_queue_depth : 1),
    blockingResponse(nullptr),
    addrRanges(params.addr_ranges.begin(), params.addr_ranges.end())
{
}

template <unsigned int BITWIDTH>
gem5::Port &
Gem5ToTlmBridge<BITWIDTH>::gem5_getPort(const std::string &if_name, int idx)
{
    if (if_name == "gem5")
        return bridgeResponsePort;
    else if (if_name == "tlm")
        return wrapper;

    return sc_core::sc_module::gem5_getPort(if_name, idx);
}

template <unsigned int BITWIDTH>
void
Gem5ToTlmBridge<BITWIDTH>::before_end_of_elaboration()
{
    bridgeResponsePort.sendRangeChange();

    socket.register_nb_transport_bw(this, &Gem5ToTlmBridge::nb_transport_bw);
    socket.register_invalidate_direct_mem_ptr(
            this, &Gem5ToTlmBridge::invalidate_direct_mem_ptr);
    sc_core::sc_module::before_end_of_elaboration();
}

} // namespace sc_gem5

sc_gem5::Gem5ToTlmBridge<32> *
gem5::Gem5ToTlmBridge32Params::create() const
{
    return new sc_gem5::Gem5ToTlmBridge<32>(
            *this, sc_core::sc_module_name(name.c_str()));
}

sc_gem5::Gem5ToTlmBridge<64> *
gem5::Gem5ToTlmBridge64Params::create() const
{
    return new sc_gem5::Gem5ToTlmBridge<64>(
            *this, sc_core::sc_module_name(name.c_str()));
}

sc_gem5::Gem5ToTlmBridge<128> *
gem5::Gem5ToTlmBridge128Params::create() const
{
    return new sc_gem5::Gem5ToTlmBridge<128>(
            *this, sc_core::sc_module_name(name.c_str()));
}

sc_gem5::Gem5ToTlmBridge<256> *
gem5::Gem5ToTlmBridge256Params::create() const
{
    return new sc_gem5::Gem5ToTlmBridge<256>(
            *this, sc_core::sc_module_name(name.c_str()));
}

sc_gem5::Gem5ToTlmBridge<512> *
gem5::Gem5ToTlmBridge512Params::create() const
{
    return new sc_gem5::Gem5ToTlmBridge<512>(
            *this, sc_core::sc_module_name(name.c_str()));
}
