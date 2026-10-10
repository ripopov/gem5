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

#include "dev/rt_accel/accelerator_wrapper.hh"

#include <utility>

#include "base/logging.hh"
#include "dev/rt_accel/host_adapter.hh"
#include "dev/rt_accel/transport.hh"

namespace gem5
{
namespace rt_accel
{

AcceleratorWrapper::AcceleratorWrapper(const Params &p)
    : ClockedObject(p),
      host(*p.host),
      queueDepth(p.queue_depth),
      requestDelay(p.request_delay),
      responseDelay(p.response_delay),
      accelerator(createAccelerator(
          {host.range(), clockPeriod(), queueDepth, host.requester()})),
      cycleEvent([this] { tick(); }, name() + ".cycle", false, CyclePriority)
{
    fatal_if(!queueDepth || !requestDelay || !responseDelay,
             "%s requires nonzero capacity and transport delays", name());
    host.attach(*this);
    accelerator->connectNonCoherentBus(*this);
    const LinkTiming timing{requestDelay, responseDelay};
    accelerator->useExternalTransport(timing, timing);
}

void
AcceleratorWrapper::init()
{
    ClockedObject::init();
    if (eventQueue() != host.eventQueue()) {
        fatal_if(!simQuantum || requestDelay < simQuantum ||
                     responseDelay < simQuantum,
                 "%s requires 0 < sim_quantum <= each transport delay",
                 name());
    }
}

void
AcceleratorWrapper::startup()
{
    wake();
}

void
AcceleratorWrapper::wake()
{
    if (!cycleEvent.scheduled()) {
        schedule(cycleEvent, clockEdge());
    }
}

void
AcceleratorWrapper::receiveMmio(PacketPtr pkt)
{
    mmioRequests.push_back(pkt);
    wake();
}

void
AcceleratorWrapper::tick()
{
    while (!mmioRequests.empty()) {
        const auto pkt = mmioRequests.front();
        if (!accelerator->trySubmit(
                curTick(), pkt, [this](Tick when, PacketPtr pkt) {
                    panic_if(when != curTick(), "Invalid model timestamp");
                    post(host, when + responseDelay, name() + ".mmio_resp",
                         [this, pkt] { host.completeMmio(pkt); });
                })) {
            break;
        }
        mmioRequests.pop_front();
    }
    accelerator->runCycle(curTick());
    checkDrain();
    // Keep stepping while other SimObjects drain, preserving the library's
    // cycle sequence even when the wrapper has no outstanding requests.
    schedule(cycleEvent, clockEdge(Cycles(1)));
}

bool
AcceleratorWrapper::trySubmit(Tick arrival, PacketPtr pkt, Completion done)
{
    panic_if(arrival != curTick() || !pkt || !done || !pkt->isRequest() ||
                 !pkt->needsResponse(),
             "Invalid accelerator master request");
    if (masterCompletions.size() == queueDepth) {
        return false;
    }
    panic_if(masterCompletions.count(pkt), "Packet is already outstanding");
    masterCompletions.emplace(pkt, std::move(done));
    post(host, arrival + requestDelay, name() + ".master_req",
         [this, pkt] { host.receiveMaster(pkt); });
    return true;
}

void
AcceleratorWrapper::completeMaster(PacketPtr pkt)
{
    auto it = masterCompletions.find(pkt);
    panic_if(it == masterCompletions.end(), "Unexpected master response");
    auto done = std::move(it->second);
    masterCompletions.erase(it);
    done(curTick(), pkt);
    checkDrain();
}

bool
AcceleratorWrapper::idle() const
{
    return mmioRequests.empty() && masterCompletions.empty() &&
           accelerator->idle();
}

void
AcceleratorWrapper::checkDrain()
{
    if (drainState() == DrainState::Draining && idle()) {
        signalDrainDone();
    }
}

DrainState
AcceleratorWrapper::drain()
{
    if (!idle()) {
        return DrainState::Draining;
    }
    return DrainState::Drained;
}

void
AcceleratorWrapper::drainResume()
{
    wake();
}

} // namespace rt_accel
} // namespace gem5
