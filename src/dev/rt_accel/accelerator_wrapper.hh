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

#ifndef __DEV_RT_ACCEL_ACCELERATOR_WRAPPER_HH__
#define __DEV_RT_ACCEL_ACCELERATOR_WRAPPER_HH__

#include <deque>
#include <memory>
#include <unordered_map>

#include "dev/rt_accel/transport.hh"
#include "params/RTAccelerator.hh"
#include "rt_accel/accelerator.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace rt_accel
{

class HostAdapter;

class AcceleratorWrapper : public ClockedObject,
                           public PortAPI,
                           public TransportEndpoint
{
  private:
    HostAdapter &host;
    const unsigned queueDepth;
    const Tick requestDelay;
    const Tick responseDelay;
    std::unique_ptr<Accelerator> accelerator;
    std::deque<PacketPtr> mmioRequests;
    std::unordered_map<PacketPtr, Completion> masterCompletions;
    EventFunctionWrapper cycleEvent;

    void tick();
    void wake();
    bool idle() const;
    void checkDrain();

  public:
    using Params = RTAcceleratorParams;
    explicit AcceleratorWrapper(const Params &p);
    void init() override;
    void startup() override;
    DrainState drain() override;
    void drainResume() override;

    EventManager &
    manager() override
    {
        return *this;
    }

    // Immutable timing and queue identity may be read by the host adapter.
    Tick
    requestLatency() const
    {
        return requestDelay;
    }
    Tick
    responseLatency() const
    {
        return responseDelay;
    }

    // Called only by delivery events on this wrapper's event queue.
    void receiveMmio(PacketPtr pkt);
    void completeMaster(PacketPtr pkt);
    bool trySubmit(Tick arrival, PacketPtr pkt, Completion done) override;
};

} // namespace rt_accel
} // namespace gem5

#endif // __DEV_RT_ACCEL_ACCELERATOR_WRAPPER_HH__
