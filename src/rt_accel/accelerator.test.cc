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

#include <gtest/gtest.h>

#include <memory>

#include "base/gtest/cur_tick_fake.hh"
#include "rt_accel/accelerator.hh"

namespace gem5
{
namespace rt_accel
{
namespace
{

class Bus : public PortAPI
{
  public:
    bool
    trySubmit(Tick, PacketPtr, Completion) override
    {
        return false;
    }
};

class AcceleratorTest : public ::testing::Test
{
  protected:
    GTestTickHandler tickHandler;
    Bus bus;
    Config config{AddrRange(0x1000, 0x1100), 10, 1};

    std::unique_ptr<Accelerator>
    model()
    {
        auto result = createAccelerator(config);
        result->connectNonCoherentBus(bus);
        result->useExternalTransport({50, 50}, {50, 50});
        return result;
    }

    std::unique_ptr<Packet>
    packet(MemCmd command, Addr addr = 0x1000)
    {
        auto req = std::make_shared<Request>(addr, 2, 0, 0);
        auto pkt = std::make_unique<Packet>(req, command);
        pkt->allocate();
        return pkt;
    }
};

TEST_F(AcceleratorTest, TimedOwnershipAndBackpressure)
{
    auto accelerator = model();
    auto first = packet(MemCmd::ReadReq);
    auto second = packet(MemCmd::ReadReq);
    int completed = 0;
    auto done = [&](Tick time, PacketPtr pkt) {
        EXPECT_EQ(time, 20);
        EXPECT_EQ(pkt, first.get());
        EXPECT_TRUE(pkt->isResponse());
        ++completed;
    };
    EXPECT_TRUE(accelerator->trySubmit(3, first.get(), done));
    EXPECT_FALSE(accelerator->trySubmit(3, second.get(), [](Tick, PacketPtr) {
        FAIL() << "Rejected request completed";
    }));
    EXPECT_TRUE(second->isRequest());
    EXPECT_EQ(completed, 0);
    accelerator->runCycle(10);
    EXPECT_EQ(completed, 0);
    accelerator->runCycle(20);
    EXPECT_EQ(completed, 1);
    EXPECT_TRUE(accelerator->idle());
    accelerator->runCycle(30);
    EXPECT_EQ(completed, 1);
}

TEST_F(AcceleratorTest, MaskedWriteAndIndependentInstances)
{
    auto a = model();
    auto b = model();
    auto write = packet(MemCmd::WriteReq);
    write->getPtr<uint8_t>()[0] = 0x42;
    write->getPtr<uint8_t>()[1] = 0xff;
    write->req->setByteEnable({true, false});
    ASSERT_TRUE(a->trySubmit(0, write.get(), [](Tick, PacketPtr) {}));
    a->runCycle(10);

    auto read_a = packet(MemCmd::ReadReq);
    auto read_b = packet(MemCmd::ReadReq);
    ASSERT_TRUE(a->trySubmit(10, read_a.get(), [](Tick, PacketPtr) {}));
    ASSERT_TRUE(b->trySubmit(10, read_b.get(), [](Tick, PacketPtr) {}));
    a->runCycle(20);
    b->runCycle(20);
    EXPECT_EQ(read_a->getConstPtr<uint8_t>()[0], 0x42);
    EXPECT_EQ(read_a->getConstPtr<uint8_t>()[1], 0);
    EXPECT_EQ(read_b->getConstPtr<uint8_t>()[0], 0);
}

TEST_F(AcceleratorTest, CompletionMayDeletePacket)
{
    auto accelerator = model();
    auto pkt = packet(MemCmd::ReadReq).release();
    bool completed = false;
    ASSERT_TRUE(accelerator->trySubmit(0, pkt, [&](Tick, PacketPtr response) {
        EXPECT_EQ(response, pkt);
        delete response;
        completed = true;
    }));
    accelerator->runCycle(10);
    EXPECT_TRUE(completed);
    EXPECT_TRUE(accelerator->idle());
}

TEST_F(AcceleratorTest, OutOfRangeIsAnErrorResponse)
{
    auto accelerator = model();
    auto pkt = packet(MemCmd::ReadReq, 0x10ff);
    ASSERT_TRUE(accelerator->trySubmit(0, pkt.get(), [](Tick, PacketPtr) {}));
    accelerator->runCycle(10);
    EXPECT_TRUE(pkt->isError());
}

} // anonymous namespace
} // namespace rt_accel
} // namespace gem5
