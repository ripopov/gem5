/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "base/gtest/logging.hh"
#include "enums/MessageRandomization.hh"
#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/slicc_interface/Message.hh"
#include "params/ClockDomain.hh"
#include "params/ClockedObject.hh"
#include "params/MessageBuffer.hh"
#include "sim/clock_domain.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/fst_trace/fst_trace.hh"
#include "sim/power/power_model.hh"
#include "sim/root.hh"
#include "sim/transaction_trace/ftr_trace.hh"
#include "sim/voltage_domain.hh"

namespace gem5
{

// MessageBuffer and clock support reference optional tracing, root, voltage,
// and power-model hooks from code paths this standalone unit test never uses.
// Keep the test binary narrow by satisfying only those link-time references.
void
FstTrace::recordMessageBufferPush(const ruby::MessageBuffer *buffer, Tick tick)
{
}

void
FstTrace::recordMessageBufferPop(const ruby::MessageBuffer *buffer, Tick tick)
{
}

FtrTrace *FtrTrace::instance = nullptr;

Root *Root::_root = nullptr;

bool
VoltageDomain::sanitiseVoltages()
{
    return false;
}

void
PowerModel::setClockedObject(ClockedObject *object)
{
}

void
FtrTrace::stampEvent(TraceId id, std::string_view event_kind,
                     std::string_view object_name, Tick tick,
                     const tx_trace::AttrList &attrs)
{
}

namespace ruby
{
namespace
{

constexpr Tick ClockPeriod = 1;

int
nextTestId()
{
    static int id = 0;
    return id++;
}

class TestClockDomain : public ClockDomain
{
  public:
    TestClockDomain(const ClockDomainParams &params, Tick period)
        : ClockDomain(params, nullptr)
    {
        _clockPeriod = period;
    }
};

class TestConsumer : public Consumer
{
  public:
    explicit TestConsumer(ClockedObject *object)
        : Consumer(object)
    {
    }

    void
    wakeup() override
    {
        ++wakeups;
    }

    void
    print(std::ostream &out) const override
    {
        out << "TestConsumer";
    }

    void
    storeEventInfo(int info) override
    {
        storedEventInfo.push_back(info);
    }

    int wakeups = 0;
    std::vector<int> storedEventInfo;
};

class TestMessage : public Message
{
  public:
    TestMessage(Tick cur_time, int id)
        : Message(cur_time, 64, nullptr), _id(id)
    {
    }

    MsgPtr
    clone() const override
    {
        return std::make_shared<TestMessage>(*this);
    }

    void
    print(std::ostream &out) const override
    {
        out << "TestMessage(" << _id << ")";
    }

    int
    id() const
    {
        return _id;
    }

  private:
    int _id;
};

const TestMessage &
asTestMessage(const Message &message)
{
    return dynamic_cast<const TestMessage&>(message);
}

const TestMessage &
asTestMessage(const MsgPtr &message)
{
    return asTestMessage(*message);
}

// The credited / in-order MessageBuffer contract exercised here mirrors how
// the XP simple-network switch uses it (see MessageBufferCredited.md):
//   * credits are spent on enqueue() and gate areNSlotsAvailable(),
//   * dequeue() is strict-FIFO head-only and returns NO credit,
//   * returnCredit() is the decoupled, explicitly-scheduled credit return the
//     consumer calls when a message actually departs downstream.
class MessageBufferCreditTest : public ::testing::Test
{
  protected:
    void
    SetUp() override
    {
        queue = getEventQueue(0);
        curEventQueue(queue);
        drainQueue();
        queue->setCurTick(0);
        gtestLogOutput.str("");
        gtestLogOutput.clear();

        testId = nextTestId();

        clockDomainParams = {};
        clockDomainParams.name = name("clock_domain");
        clockDomainParams.eventq_index = 0;
        clockDomain = std::make_unique<TestClockDomain>(
            clockDomainParams, ClockPeriod);

        clockedObjectParams = {};
        clockedObjectParams.name = name("clocked_object");
        clockedObjectParams.eventq_index = 0;
        clockedObjectParams.clk_domain = clockDomain.get();
        clockedObjectParams.power_state = nullptr;
        clockedObject = std::make_unique<ClockedObject>(
            clockedObjectParams);

        consumer = std::make_unique<TestConsumer>(clockedObject.get());
    }

    void
    TearDown() override
    {
        if (queue) {
            drainQueue();
        }

        buffer.reset();
        consumer.reset();
        clockedObject.reset();
        clockDomain.reset();

        if (queue) {
            queue->setCurTick(0);
        }
    }

    std::string
    name(const char *suffix) const
    {
        return "message_buffer_credit_test." + std::to_string(testId) + "." +
               suffix;
    }

    void
    drainQueue()
    {
        while (queue && !queue->empty()) {
            queue->serviceOne();
        }
    }

    void
    advanceTo(Tick tick)
    {
        ASSERT_LE(queue->getCurTick(), tick);
        queue->serviceEvents(tick);
    }

    MessageBuffer &
    makeBuffer(unsigned credits, unsigned buffer_size,
               Cycles credit_return_latency,
               unsigned max_dequeue_rate = 0,
               bool allow_zero_latency = false)
    {
        drainQueue();
        buffer.reset();

        bufferParams = {};
        bufferParams.name = name(
            ("buffer_" + std::to_string(nextBufferId++)).c_str());
        bufferParams.eventq_index = 0;
        bufferParams.allow_zero_latency = allow_zero_latency;
        bufferParams.buffer_size = buffer_size;
        bufferParams.max_dequeue_rate = max_dequeue_rate;
        bufferParams.ordered = true;
        bufferParams.randomization = MessageRandomization::disabled;
        bufferParams.routing_priority = 0;
        bufferParams.port_out_port_connection_count = 0;
        bufferParams.port_in_port_connection_count = 0;
        bufferParams.credit_return_latency = credit_return_latency;
        bufferParams.credits = credits;

        buffer = std::make_unique<MessageBuffer>(bufferParams);
        buffer->setConsumer(consumer.get());
        return *buffer;
    }

    MsgPtr
    makeMessage(int id, Tick cur_time = 0)
    {
        return std::make_shared<TestMessage>(cur_time, id);
    }

    int
    headId(MessageBuffer &buf)
    {
        return asTestMessage(buf.peekMsgPtr()).id();
    }

    EventQueue *queue = nullptr;
    int testId = 0;
    int nextBufferId = 0;

    ClockDomainParams clockDomainParams;
    ClockedObjectParams clockedObjectParams;
    MessageBufferParams bufferParams;

    std::unique_ptr<TestClockDomain> clockDomain;
    std::unique_ptr<ClockedObject> clockedObject;
    std::unique_ptr<TestConsumer> consumer;
    std::unique_ptr<MessageBuffer> buffer;
};

TEST_F(MessageBufferCreditTest, UncreditedConstructionHasInfiniteCredit)
{
    auto &buf = makeBuffer(0, 0, Cycles(0));

    EXPECT_FALSE(buf.isCredited());
    EXPECT_TRUE(buf.hasCredit());
    EXPECT_TRUE(buf.hasCredit(1000));
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.maxCredits(), 0);
    EXPECT_EQ(buf.creditReturnLatency(), Cycles(0));
    EXPECT_EQ(buf.pendingCreditReturns(), 0);
}

TEST_F(MessageBufferCreditTest, RejectsInvalidCreditedConfiguration)
{
    EXPECT_ANY_THROW(makeBuffer(2, 1, Cycles(1)));
    EXPECT_ANY_THROW(makeBuffer(2, 2, Cycles(0)));
}

TEST_F(MessageBufferCreditTest, GetMaxSizeReportsCapacity)
{
    EXPECT_EQ(makeBuffer(2, 4, Cycles(1)).getMaxSize(), 4u);
    EXPECT_EQ(makeBuffer(0, 0, Cycles(0)).getMaxSize(), 0u);
}

TEST_F(MessageBufferCreditTest, CreditedEnqueueConsumesCredits)
{
    auto &buf = makeBuffer(2, 2, Cycles(4));

    EXPECT_EQ(buf.maxCredits(), 2);
    EXPECT_EQ(buf.availableCredits(), 2);
    EXPECT_TRUE(buf.hasCredit(2));

    EXPECT_ANY_THROW(buf.enqueue(makeMessage(0), 0, 0, false, false));
    EXPECT_EQ(buf.availableCredits(), 2);

    buf.enqueue(makeMessage(1), 0, 3, false, false);
    EXPECT_EQ(buf.availableCredits(), 1);
    EXPECT_TRUE(buf.hasCredit());
    EXPECT_FALSE(buf.hasCredit(2));

    buf.enqueue(makeMessage(2), 0, 4, false, false);
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_FALSE(buf.hasCredit());

    EXPECT_ANY_THROW(buf.enqueue(makeMessage(3), 0, 1, false, false));
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.traceState().currentSize, 2);
}

TEST_F(MessageBufferCreditTest, DequeueIsStrictFifoAndKeepsCredits)
{
    auto &buf = makeBuffer(3, 3, Cycles(1));

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    buf.enqueue(makeMessage(2), 0, 2, false, false);
    buf.enqueue(makeMessage(3), 0, 3, false, false);
    EXPECT_EQ(buf.availableCredits(), 0);

    advanceTo(3);

    // Strict-FIFO: the head is always the oldest message, drained in order.
    ASSERT_TRUE(buf.isReady(3));
    EXPECT_EQ(headId(buf), 1);
    buf.dequeue(3);

    EXPECT_EQ(headId(buf), 2);
    buf.dequeue(3);

    EXPECT_EQ(headId(buf), 3);
    buf.dequeue(3);

    EXPECT_TRUE(buf.isEmpty());

    // dequeue() never returns a credit; that is returnCredit()'s job.
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);
}

TEST_F(MessageBufferCreditTest, DequeueDoesNotReturnCredit)
{
    auto &buf = makeBuffer(1, 1, Cycles(3));

    buf.enqueue(makeMessage(1), 0, 5, false, false);
    EXPECT_EQ(buf.availableCredits(), 0);

    advanceTo(5);
    ASSERT_TRUE(buf.isReady(5));
    buf.dequeue(5);

    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);

    // Even after arbitrary time passes, no credit comes back on its own.
    advanceTo(100);
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);
}

TEST_F(MessageBufferCreditTest, ReturnCreditSchedulesDelayedReturn)
{
    auto &buf = makeBuffer(1, 2, Cycles(1));

    EXPECT_TRUE(buf.areNSlotsAvailable(1, 0));
    buf.enqueue(makeMessage(1), 0, 1, false, false);
    EXPECT_FALSE(buf.areNSlotsAvailable(1, 0));

    advanceTo(1);
    ASSERT_TRUE(buf.isReady(1));
    buf.dequeue(1);

    // Decoupled: pop happened, but the credit only returns when the consumer
    // says the message has departed downstream.
    buf.returnCredit(1, 5);
    EXPECT_EQ(buf.pendingCreditReturns(), 1);
    EXPECT_FALSE(buf.areNSlotsAvailable(1, 1));
    EXPECT_FALSE(buf.areNSlotsAvailable(1, 2));

    advanceTo(6);
    EXPECT_TRUE(buf.areNSlotsAvailable(1, 6));
    EXPECT_EQ(buf.availableCredits(), 1);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);
}

TEST_F(MessageBufferCreditTest, ReturnCreditCoalescesGroupedDelays)
{
    auto &buf = makeBuffer(3, 3, Cycles(1));

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    buf.enqueue(makeMessage(2), 0, 1, false, false);
    buf.enqueue(makeMessage(3), 0, 1, false, false);
    EXPECT_EQ(buf.availableCredits(), 0);

    advanceTo(1);
    buf.dequeue(1);
    buf.dequeue(1);
    buf.dequeue(1);
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);

    buf.returnCredit(1, 6, 2);  // 2 credits visible at tick 7
    buf.returnCredit(1, 4, 1);  // 1 credit  visible at tick 5
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 3);

    advanceTo(4);
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 3);

    advanceTo(5);
    EXPECT_EQ(buf.availableCredits(), 1);
    EXPECT_EQ(buf.pendingCreditReturns(), 2);

    advanceTo(7);
    EXPECT_EQ(buf.availableCredits(), 3);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);
}

TEST_F(MessageBufferCreditTest, UncreditedReturnCreditIsNoOp)
{
    auto &buf = makeBuffer(0, 0, Cycles(0));

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    advanceTo(1);
    ASSERT_TRUE(buf.isReady(1));
    buf.dequeue(1);

    buf.returnCredit(1, 5);
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);

    advanceTo(10);
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);
}

TEST_F(MessageBufferCreditTest, StallMessageDoesNotReturnCredit)
{
    auto &buf = makeBuffer(1, 1, Cycles(1));

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    EXPECT_EQ(buf.availableCredits(), 0);

    advanceTo(1);
    buf.stallMessage(0x100, 1);
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);
    EXPECT_TRUE(buf.hasStalledMsg(0x100));

    buf.reanalyzeMessages(0x100, 2);
    advanceTo(2);
    ASSERT_TRUE(buf.isReady(2));
    buf.dequeue(2);
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);

    buf.returnCredit(2, 1);
    EXPECT_EQ(buf.pendingCreditReturns(), 1);
    advanceTo(3);
    EXPECT_EQ(buf.availableCredits(), 1);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);
}

TEST_F(MessageBufferCreditTest, FiniteCapacityUsesNextCyclePopVisibility)
{
    auto &buf = makeBuffer(0, 2, Cycles(0));

    EXPECT_TRUE(buf.areNSlotsAvailable(2, 0));
    buf.enqueue(makeMessage(1), 0, 5, false, false);
    buf.enqueue(makeMessage(2), 0, 5, false, false);

    EXPECT_FALSE(buf.areNSlotsAvailable(1, 0));

    advanceTo(5);
    ASSERT_TRUE(buf.isReady(5));
    buf.dequeue(5);

    // A pop is not visible to the network's size view until the next cycle.
    EXPECT_FALSE(buf.areNSlotsAvailable(1, 5));
    EXPECT_TRUE(buf.areNSlotsAvailable(1, 6));
}

TEST_F(MessageBufferCreditTest, MaxDequeueRateLimitsPopsPerCycle)
{
    auto &buf = makeBuffer(0, 2, Cycles(0), /*max_dequeue_rate=*/1);

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    buf.enqueue(makeMessage(2), 0, 1, false, false);

    advanceTo(1);
    ASSERT_TRUE(buf.isReady(1));
    buf.dequeue(1);

    // The rate cap blocks a second pop in the same cycle.
    EXPECT_FALSE(buf.isReady(1));

    advanceTo(2);
    EXPECT_TRUE(buf.isReady(2));
    buf.dequeue(2);
    EXPECT_TRUE(buf.isEmpty());
}

} // anonymous namespace
} // namespace ruby
} // namespace gem5
