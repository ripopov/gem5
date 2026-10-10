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

#ifndef __DEV_RT_ACCEL_TRANSPORT_HH__
#define __DEV_RT_ACCEL_TRANSPORT_HH__

#include <functional>
#include <string>

#include "base/logging.hh"
#include "mem/packet.hh"
#include "sim/cur_tick.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace rt_accel
{

// Common queue boundary for the library and native gem5 backends.
class TransportEndpoint
{
  public:
    virtual ~TransportEndpoint() = default;
    virtual EventManager &manager() = 0;
    virtual Tick requestLatency() const = 0;
    virtual Tick responseLatency() const = 0;
    virtual void receiveMmio(PacketPtr pkt) = 0;
    virtual void completeMaster(PacketPtr pkt) = 0;
};

// At a quantum boundary, merge asynchronous insertions before deliveries,
// then sample accelerator inputs. Use these phases in serial mode as well.
constexpr Event::Priority DeliveryPriority = Event::Progress_Event_Pri + 1;
constexpr Event::Priority CyclePriority = Event::Progress_Event_Pri + 2;

// Publishing transfers exclusive packet access. Only the destination event
// may dereference captured packets; no accelerator callbacks cross queues.
inline void
post(EventManager &target, Tick when, const std::string &name,
     std::function<void()> action, Event::Priority priority = DeliveryPriority)
{
    panic_if(when < curTick(), "RT transport delivery is in the past");
    panic_if(target.eventQueue() != curEventQueue() &&
                 when - curTick() < simQuantum,
             "RT transport delay is smaller than the simulation quantum");
    target.schedule(new EventFunctionWrapper(action, name, true, priority),
                    when);
}

} // namespace rt_accel
} // namespace gem5

#endif // __DEV_RT_ACCEL_TRANSPORT_HH__
