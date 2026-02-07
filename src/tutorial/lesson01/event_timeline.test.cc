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

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "sim/eventq.hh"
#include "tutorial/lesson01/event_timeline.hh"

namespace gem5
{

namespace tutorial
{

namespace lesson01
{

class EventTimelineTest : public ::testing::Test
{
  protected:
    EventQueue queue{"TutorialLesson01EventQueue"};
    EventQueue *savedQueue = nullptr;

    void
    SetUp() override
    {
        savedQueue = curEventQueue();
        curEventQueue(&queue);
    }

    void
    TearDown() override
    {
        curEventQueue(savedQueue);
    }
};

TEST_F(EventTimelineTest, ProcessesCallbacksByTickThenPriority)
{
    EventTimeline timeline(&queue);

    timeline.prime();
    ASSERT_FALSE(queue.empty());

    timeline.runToCompletion();
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.getCurTick(), 11);
    EXPECT_EQ(timeline.callbacksExecuted(), 6);

    const std::vector<std::string> expectedTrace = {
        "tick=3 label=bootstrap",
        "tick=7 label=high-priority-phase",
        "tick=7 label=default-priority-phase",
        "tick=7 label=low-priority-pulse",
        "tick=9 label=low-priority-pulse",
        "tick=11 label=low-priority-pulse",
    };

    EXPECT_EQ(timeline.trace(), expectedTrace);
}

} // namespace lesson01

} // namespace tutorial

} // namespace gem5
