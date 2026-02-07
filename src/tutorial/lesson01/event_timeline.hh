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

#ifndef __TUTORIAL_LESSON01_EVENT_TIMELINE_HH__
#define __TUTORIAL_LESSON01_EVENT_TIMELINE_HH__

#include <cstdint>
#include <string>
#include <vector>

#include "sim/eventq.hh"

namespace gem5
{

namespace tutorial
{

namespace lesson01
{

/**
 * A minimal event-driven timeline used by Lesson 1.
 *
 * The class uses gem5's EventManager API to schedule named callbacks and keeps
 * a textual trace of what happened at each tick. It intentionally demonstrates
 * two fundamental behaviors:
 *
 * 1) Event ordering by time.
 * 2) Event ordering by priority when time is identical.
 */
class EventTimeline : public EventManager
{
  public:
    explicit EventTimeline(EventQueue *event_queue);

    /** Schedule the initial "bootstrap" event that seeds the timeline. */
    void prime();

    /** Service events until this event queue becomes empty. */
    void runToCompletion();

    const std::vector<std::string> &trace() const { return eventTrace; }
    std::uint64_t callbacksExecuted() const { return callbackCount; }

  private:
    /** Store a normalized trace line, e.g., "tick=7 label=phase-A". */
    void appendTrace(const std::string &label);

    void onBootstrap();
    void onHighPriorityPhase();
    void onDefaultPriorityPhase();
    void onLowPriorityPulse();

    std::vector<std::string> eventTrace;
    std::uint64_t callbackCount = 0;
    int pulseCount = 0;

    EventFunctionWrapper bootstrapEvent;
    EventFunctionWrapper highPriorityEvent;
    EventFunctionWrapper defaultPriorityEvent;
    EventFunctionWrapper lowPriorityPulseEvent;
};

} // namespace lesson01

} // namespace tutorial

} // namespace gem5

#endif // __TUTORIAL_LESSON01_EVENT_TIMELINE_HH__
