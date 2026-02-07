/*
 * Copyright (c) 2026 The Regents of the University of California
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

#include "tutorial/lesson01/event_timeline.hh"

#include <sstream>

#include "sim/cur_tick.hh"

namespace gem5
{

namespace tutorial
{

namespace lesson01
{

EventTimeline::EventTimeline(EventQueue *event_queue)
    : EventManager(event_queue),
      bootstrapEvent(
          [this]() { onBootstrap(); },
          "tutorial.lesson01.bootstrap"),
      highPriorityEvent(
          [this]() { onHighPriorityPhase(); },
          "tutorial.lesson01.high_priority",
          false,
          EventBase::Delayed_Writeback_Pri),
      defaultPriorityEvent(
          [this]() { onDefaultPriorityPhase(); },
          "tutorial.lesson01.default_priority",
          false,
          EventBase::Default_Pri),
      lowPriorityPulseEvent(
          [this]() { onLowPriorityPulse(); },
          "tutorial.lesson01.low_priority_pulse",
          false,
          EventBase::Progress_Event_Pri)
{
}

void
EventTimeline::prime()
{
    schedule(bootstrapEvent, 3);
}

void
EventTimeline::runToCompletion()
{
    while (!eventQueue()->empty()) {
        eventQueue()->serviceOne();
    }
}

void
EventTimeline::appendTrace(const std::string &label)
{
    std::ostringstream entry;
    entry << "tick=" << curTick() << " label=" << label;
    eventTrace.push_back(entry.str());
    callbackCount++;
}

void
EventTimeline::onBootstrap()
{
    appendTrace("bootstrap");

    // These three callbacks all run at tick 7. Their priorities determine
    // execution order: high, then default, then low.
    schedule(highPriorityEvent, 7);
    schedule(defaultPriorityEvent, 7);
    schedule(lowPriorityPulseEvent, 7);
}

void
EventTimeline::onHighPriorityPhase()
{
    appendTrace("high-priority-phase");
}

void
EventTimeline::onDefaultPriorityPhase()
{
    appendTrace("default-priority-phase");
}

void
EventTimeline::onLowPriorityPulse()
{
    appendTrace("low-priority-pulse");
    pulseCount++;

    // Keep one event chain going to show repeated self-scheduling.
    if (pulseCount < 3) {
        schedule(lowPriorityPulseEvent, curTick() + 2);
    }
}

} // namespace lesson01

} // namespace tutorial

} // namespace gem5
