/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "dev/rt_accel/gem5_wrapper.hh"

#include "dev/rt_accel/host_adapter.hh"
#include "dev/rt_accel/trace.hh"

namespace gem5
{
namespace rt_accel
{

Gem5Wrapper::Gem5Wrapper(const Params &p)
    : ClockedObject(p),
      trace(p.trace_file),
      host(*p.host),
      requestDelay(p.request_delay),
      responseDelay(p.response_delay),
      queueDepth(p.queue_depth),
      priority(DeliveryPriority + p.index),
      systemRanges(p.system_ranges.begin(), p.system_ranges.end()),
      mmioPort(*this),
      memoryPort(*this)
{
    static_assert(DeliveryPriority + 63 <= Event::Maximum_Pri,
                  "Native transport requires 64 ordered delivery priorities");
    fatal_if(!queueDepth || !requestDelay || !responseDelay || p.index > 63,
             "Invalid native accelerator transport configuration");
    host.attach(*this);
}

void
Gem5Wrapper::init()
{
    ClockedObject::init();
    fatal_if(!mmioPort.isConnected() || !memoryPort.isConnected(),
             "%s requires both local crossbar connections", name());
    fatal_if(eventQueue() != host.eventQueue() &&
                 (!simQuantum || requestDelay < simQuantum ||
                  responseDelay < simQuantum),
             "Native accelerator transport has insufficient lookahead");
    memoryPort.sendRangeChange();
}

Port &
Gem5Wrapper::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "mmio") {
        return mmioPort;
    }
    if (if_name == "memory") {
        return memoryPort;
    }
    return ClockedObject::getPort(if_name, idx);
}

Gem5Wrapper::MmioPort::MmioPort(Gem5Wrapper &o)
    : RequestPort(o.name() + ".mmio"), owner(o)
{}
Gem5Wrapper::MemoryPort::MemoryPort(Gem5Wrapper &o)
    : ResponsePort(o.name() + ".memory"), owner(o)
{}

AddrRangeList
Gem5Wrapper::MemoryPort::getAddrRanges() const
{
    return owner.systemRanges;
}

void
Gem5Wrapper::receiveMmio(PacketPtr pkt)
{
    tracePacket(trace, "mmio_arrival", pkt);
    // Each isolated subsystem uses the same local register map and ELF.
    pkt->setAddr(pkt->getAddr() - host.range().start() + 0x10000000);
    mmioRequests.push_back(pkt);
    if (!mmioBlocked) {
        sendMmio();
    }
}

void
Gem5Wrapper::sendMmio()
{
    while (!mmioRequests.empty()) {
        if (!mmioPort.sendTimingReq(mmioRequests.front())) {
            mmioBlocked = true;
            return;
        }
        mmioRequests.pop_front();
    }
}

void
Gem5Wrapper::MmioPort::recvReqRetry()
{
    owner.mmioBlocked = false;
    owner.sendMmio();
}

bool
Gem5Wrapper::MmioPort::recvTimingResp(PacketPtr pkt)
{
    pkt->setAddr(pkt->getAddr() - 0x10000000 + owner.host.range().start());
    tracePacket(owner.trace, "mmio_return", pkt);
    Tick when =
        curTick() + owner.responseDelay + pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;
    auto *host = &owner.host;
    post(
        *host, when, owner.name() + ".mmio_response",
        [host, pkt] { host->completeMmio(pkt); }, owner.priority);
    return true;
}

bool
Gem5Wrapper::MemoryPort::recvTimingReq(PacketPtr pkt)
{
    panic_if(!pkt->needsResponse() || pkt->cacheResponding(),
             "Native transport requires noncoherent response transactions");
    if (owner.outstanding == owner.queueDepth) {
        tracePacket(owner.trace, "master_reject", pkt);
        owner.masterRetry = true;
        return false;
    }
    tracePacket(owner.trace, "master_accept", pkt);
    ++owner.outstanding;
    Tick when =
        curTick() + owner.requestDelay + pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;
    auto *host = &owner.host;
    post(
        *host, when, owner.name() + ".master_request",
        [host, pkt] { host->receiveMaster(pkt); }, owner.priority);
    return true;
}

void
Gem5Wrapper::completeMaster(PacketPtr pkt)
{
    tracePacket(trace, "master_arrival", pkt);
    masterResponses.push_back(pkt);
    if (!responseBlocked) {
        sendResponses();
    }
}

void
Gem5Wrapper::sendResponses()
{
    while (!masterResponses.empty()) {
        if (!memoryPort.sendTimingResp(masterResponses.front())) {
            responseBlocked = true;
            return;
        }
        masterResponses.pop_front();
        --outstanding;
        if (masterRetry) {
            masterRetry = false;
            memoryPort.sendRetryReq();
        }
    }
    if (drainState() == DrainState::Draining && !outstanding &&
        mmioRequests.empty()) {
        signalDrainDone();
    }
}

void
Gem5Wrapper::MemoryPort::recvRespRetry()
{
    owner.responseBlocked = false;
    owner.sendResponses();
}
Tick
Gem5Wrapper::MemoryPort::recvAtomic(PacketPtr)
{
    panic("Native transport requires timing accesses");
}
void
Gem5Wrapper::MemoryPort::recvFunctional(PacketPtr)
{
    panic("Functional access cannot cross accelerator transport");
}
DrainState
Gem5Wrapper::drain()
{
    return outstanding || !mmioRequests.empty() ? DrainState::Draining
                                                : DrainState::Drained;
}

} // namespace rt_accel
} // namespace gem5
