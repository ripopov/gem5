/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 * All rights reserved.
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
 */

#include "dev/rt_accel/host_adapter.hh"

#include "base/logging.hh"
#include "dev/rt_accel/trace.hh"
#include "dev/rt_accel/transport.hh"
#include "sim/system.hh"

namespace gem5
{
namespace rt_accel
{

namespace
{

struct RequestorState : Packet::SenderState
{
    const RequestorID original;
    explicit RequestorState(RequestorID id) : original(id) {}
};

} // anonymous namespace

HostAdapter::HostAdapter(const Params &p)
    : SimObject(p),
      trace(p.trace_file),
      mmioPort(*this),
      masterPort(*this),
      mmioRange(p.mmio_range),
      queueDepth(p.queue_depth),
      requestorId(p.system->getRequestorId(this))
{
    fatal_if(!queueDepth || !mmioRange.valid() || mmioRange.interleaved(),
             "%s requires a capacity and a non-interleaved MMIO range",
             name());
}

void
HostAdapter::attach(TransportEndpoint &wrapper)
{
    fatal_if(accelerator, "%s already has an accelerator", name());
    accelerator = &wrapper;
}

void
HostAdapter::init()
{
    SimObject::init();
    fatal_if(!accelerator || !mmioPort.isConnected(),
             "%s requires an accelerator and an MMIO connection", name());
    mmioPort.sendRangeChange();
}

Port &
HostAdapter::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "pio") {
        return mmioPort;
    }
    if (if_name == "dma") {
        return masterPort;
    }
    return SimObject::getPort(if_name, idx);
}

HostAdapter::MmioPort::MmioPort(HostAdapter &adapter)
    : ResponsePort(adapter.name() + ".pio"), owner(adapter)
{}

AddrRangeList
HostAdapter::MmioPort::getAddrRanges() const
{
    return {owner.mmioRange};
}

bool
HostAdapter::MmioPort::recvTimingReq(PacketPtr pkt)
{
    panic_if(!pkt->needsResponse() || pkt->cacheResponding(),
             "RT MMIO requires a noncoherent request with a response");
    if (owner.mmioOutstanding == owner.queueDepth) {
        tracePacket(owner.trace, "mmio_reject", pkt);
        owner.mmioRetry = true;
        return false;
    }
    tracePacket(owner.trace, "mmio_accept", pkt);
    ++owner.mmioOutstanding;
    auto &target = *owner.accelerator;
    const Tick when = curTick() + pkt->headerDelay + pkt->payloadDelay +
                      target.requestLatency();
    pkt->headerDelay = pkt->payloadDelay = 0;
    post(target.manager(), when, owner.name() + ".mmio_req",
         [&target, pkt] { target.receiveMmio(pkt); });
    return true;
}

Tick
HostAdapter::MmioPort::recvAtomic(PacketPtr)
{
    panic("RT accelerator MMIO requires timing accesses");
}

void
HostAdapter::MmioPort::recvFunctional(PacketPtr)
{
    panic("RT accelerator functional MMIO is not implemented");
}

void
HostAdapter::completeMmio(PacketPtr pkt)
{
    tracePacket(trace, "mmio_return", pkt);
    responses.push_back(pkt);
    if (!responseBlocked) {
        sendResponses();
    }
}

void
HostAdapter::sendResponses()
{
    while (!responses.empty()) {
        tracePacket(trace, "mmio_send", responses.front());
        if (!mmioPort.sendTimingResp(responses.front())) {
            responseBlocked = true;
            return;
        }
        responses.pop_front();
        --mmioOutstanding;
        if (mmioRetry) {
            mmioRetry = false;
            mmioPort.sendRetryReq();
        }
    }
    checkDrain();
}

void
HostAdapter::MmioPort::recvRespRetry()
{
    owner.responseBlocked = false;
    owner.sendResponses();
}

HostAdapter::MasterPort::MasterPort(HostAdapter &adapter)
    : RequestPort(adapter.name() + ".dma"), owner(adapter)
{}

void
HostAdapter::receiveMaster(PacketPtr pkt)
{
    panic_if(!masterPort.isConnected(), "%s DMA port is unconnected", name());
    // Accelerator Systems have independent requestor namespaces. Attribute
    // host accesses to this adapter, then restore the local ID on return.
    pkt->pushSenderState(new RequestorState(pkt->requestorId()));
    pkt->req->requestorId(requestorId);
    tracePacket(trace, "master_arrival", pkt);
    ++masterOutstanding;
    requests.push_back(pkt);
    if (!masterBlocked) {
        sendMasterRequests();
    }
}

void
HostAdapter::sendMasterRequests()
{
    while (!requests.empty()) {
        tracePacket(trace, "master_issue", requests.front());
        if (!masterPort.sendTimingReq(requests.front())) {
            masterBlocked = true;
            return;
        }
        requests.pop_front();
    }
}

void
HostAdapter::MasterPort::recvReqRetry()
{
    owner.masterBlocked = false;
    owner.sendMasterRequests();
}

bool
HostAdapter::MasterPort::recvTimingResp(PacketPtr pkt)
{
    panic_if(!owner.masterOutstanding, "Unexpected RT DMA response");
    auto *state = dynamic_cast<RequestorState *>(pkt->popSenderState());
    panic_if(!state, "RT DMA response lost its requestor state");
    pkt->req->requestorId(state->original);
    delete state;
    tracePacket(owner.trace, "master_return", pkt);
    auto &target = *owner.accelerator;
    const Tick when = curTick() + pkt->headerDelay + pkt->payloadDelay +
                      target.responseLatency();
    pkt->headerDelay = pkt->payloadDelay = 0;
    --owner.masterOutstanding;
    post(target.manager(), when, owner.name() + ".master_resp",
         [&target, pkt] { target.completeMaster(pkt); });
    owner.checkDrain();
    return true;
}

DrainState
HostAdapter::drain()
{
    return mmioOutstanding || masterOutstanding ? DrainState::Draining
                                                : DrainState::Drained;
}

void
HostAdapter::checkDrain()
{
    if (drainState() == DrainState::Draining && !mmioOutstanding &&
        !masterOutstanding) {
        signalDrainDone();
    }
}

} // namespace rt_accel
} // namespace gem5
