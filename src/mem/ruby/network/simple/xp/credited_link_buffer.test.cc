/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <ostream>
#include <set>
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

using MessagePredicate = std::function<bool(const Message&)>;

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

MessagePredicate
idIs(int id)
{
    return [id](const Message &message) {
        return asTestMessage(message).id() == id;
    };
}

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
               bool enable_ooo_pop = true,
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
        bufferParams.enable_ooo_pop = enable_ooo_pop;

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
    selectedId(MessageBuffer &buffer,
               MessageBuffer::Handle handle)
    {
        return asTestMessage(buffer.peekAt(handle)).id();
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
    EXPECT_TRUE(buf.enableOooPop());
}

TEST_F(MessageBufferCreditTest, RejectsInvalidCreditedConfiguration)
{
    EXPECT_ANY_THROW(makeBuffer(2, 1, Cycles(1)));
    EXPECT_ANY_THROW(makeBuffer(2, 2, Cycles(0)));
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

TEST_F(MessageBufferCreditTest, CreditedAvailabilityWaitsForCreditReturn)
{
    auto &buf = makeBuffer(1, 2, Cycles(1));

    EXPECT_TRUE(buf.areNSlotsAvailable(1, 0));
    buf.enqueue(makeMessage(1), 0, 1, false, false);
    EXPECT_FALSE(buf.areNSlotsAvailable(1, 0));

    advanceTo(1);
    auto selected = buf.selectEligible(MessagePredicate(), 1);
    ASSERT_TRUE(selected.valid());
    buf.popAt(selected, 1, 5);

    EXPECT_FALSE(buf.areNSlotsAvailable(1, 1));
    EXPECT_FALSE(buf.areNSlotsAvailable(1, 2));

    advanceTo(6);
    EXPECT_TRUE(buf.areNSlotsAvailable(1, 6));
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
    EXPECT_EQ(buf.pendingCreditReturns(), 1);

    advanceTo(2);
    EXPECT_EQ(buf.availableCredits(), 1);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);
}

TEST_F(MessageBufferCreditTest, PopAtWithoutDecrementDoesNotReturnCredit)
{
    auto &buf = makeBuffer(1, 1, Cycles(1));

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    EXPECT_EQ(buf.availableCredits(), 0);

    advanceTo(1);
    auto selected = buf.selectEligible(MessagePredicate(), 1);
    ASSERT_TRUE(selected.valid());
    buf.popAt(selected, 1, 5, 1, false);

    EXPECT_TRUE(buf.isEmpty());
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);

    advanceTo(6);
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);
}

TEST_F(MessageBufferCreditTest, UncreditedDequeueDoesNotReturnCredits)
{
    auto &buf = makeBuffer(0, 0, Cycles(0));

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    advanceTo(1);
    ASSERT_TRUE(buf.isReady(1));

    buf.dequeue(1);
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);

    advanceTo(10);
    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);
}

TEST_F(MessageBufferCreditTest, DequeueSchedulesImmediateCreditReturn)
{
    auto &buf = makeBuffer(1, 1, Cycles(3));

    buf.enqueue(makeMessage(1), 0, 5, false, false);
    EXPECT_EQ(buf.availableCredits(), 0);

    advanceTo(5);
    ASSERT_TRUE(buf.isReady(5));
    buf.dequeue(5);

    EXPECT_EQ(buf.availableCredits(), 0);
    EXPECT_EQ(buf.pendingCreditReturns(), 1);

    advanceTo(5);
    EXPECT_EQ(buf.availableCredits(), 1);
    EXPECT_EQ(buf.pendingCreditReturns(), 0);
}

TEST_F(MessageBufferCreditTest, PopAtSchedulesDelayedGroupedReturns)
{
    auto &buf = makeBuffer(3, 3, Cycles(1));

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    buf.enqueue(makeMessage(2), 0, 1, false, false);
    buf.enqueue(makeMessage(3), 0, 1, false, false);
    EXPECT_EQ(buf.availableCredits(), 0);

    advanceTo(1);
    auto first = buf.selectEligible(MessagePredicate(), 1);
    ASSERT_TRUE(first.valid());
    buf.popAt(first, 1, 6, 2);

    auto second = buf.selectEligible(MessagePredicate(), 1);
    ASSERT_TRUE(second.valid());
    buf.popAt(second, 1, 4, 1);

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

TEST_F(MessageBufferCreditTest, SelectHeadHonorsReadinessAndPredicate)
{
    auto &buf = makeBuffer(2, 2, Cycles(1));

    buf.enqueue(makeMessage(1), 0, 5, false, false);
    buf.enqueue(makeMessage(2), 0, 10, false, false);

    EXPECT_FALSE(buf.selectHead(MessagePredicate(), 4).valid());

    advanceTo(5);
    auto head = buf.selectHead(MessagePredicate(), 5);
    ASSERT_TRUE(head.valid());
    EXPECT_EQ(selectedId(buf, head), 1);

    EXPECT_FALSE(buf.selectHead(idIs(2), 5).valid());
    EXPECT_TRUE(buf.selectHead(idIs(1), 5).valid());
}

TEST_F(MessageBufferCreditTest, SelectEligibleCanPopMatchingMessageOutOfOrder)
{
    auto &buf = makeBuffer(3, 3, Cycles(1));

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    buf.enqueue(makeMessage(2), 0, 2, false, false);
    buf.enqueue(makeMessage(3), 0, 3, false, false);

    advanceTo(3);
    EXPECT_FALSE(buf.selectHead(idIs(2), 3).valid());

    auto selected = buf.selectEligible(idIs(2), 3);
    ASSERT_TRUE(selected.valid());
    EXPECT_EQ(selectedId(buf, selected), 2);
    buf.popAt(selected, 3, 10);

    auto head = buf.selectHead(MessagePredicate(), 3);
    ASSERT_TRUE(head.valid());
    EXPECT_EQ(selectedId(buf, head), 1);
}

TEST_F(MessageBufferCreditTest, SelectEligibleFallsBackToHeadWhenOooDisabled)
{
    auto &buf = makeBuffer(2, 2, Cycles(1), false);

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    buf.enqueue(makeMessage(2), 0, 2, false, false);

    advanceTo(2);
    EXPECT_FALSE(buf.selectEligible(idIs(2), 2).valid());

    auto selected = buf.selectEligible(idIs(1), 2);
    ASSERT_TRUE(selected.valid());
    EXPECT_EQ(selectedId(buf, selected), 1);
}

TEST_F(MessageBufferCreditTest, MaxDequeueRateBlocksSameCycleSelection)
{
    auto &buf = makeBuffer(2, 2, Cycles(1), true, 1);

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    buf.enqueue(makeMessage(2), 0, 1, false, false);

    advanceTo(1);
    auto selected = buf.selectEligible(MessagePredicate(), 1);
    ASSERT_TRUE(selected.valid());
    buf.popAt(selected, 1, 10);

    EXPECT_FALSE(buf.selectEligible(MessagePredicate(), 1).valid());

    advanceTo(2);
    EXPECT_TRUE(buf.selectEligible(MessagePredicate(), 2).valid());
}

TEST_F(MessageBufferCreditTest, FiniteCapacityUsesNextCyclePopVisibility)
{
    auto &buf = makeBuffer(0, 2, Cycles(0));

    EXPECT_TRUE(buf.areNSlotsAvailable(2, 0));
    buf.enqueue(makeMessage(1), 0, 5, false, false);
    buf.enqueue(makeMessage(2), 0, 5, false, false);

    EXPECT_FALSE(buf.areNSlotsAvailable(1, 0));

    advanceTo(5);
    auto selected = buf.selectEligible(MessagePredicate(), 5);
    ASSERT_TRUE(selected.valid());
    buf.popAt(selected, 5, 10);

    EXPECT_FALSE(buf.areNSlotsAvailable(1, 5));
    EXPECT_TRUE(buf.areNSlotsAvailable(1, 6));
}

TEST_F(MessageBufferCreditTest, OutOfOrderRemovalPreservesHeapOrder)
{
    auto &buf = makeBuffer(8, 8, Cycles(1));

    // Arrival time == id, so the oldest message is always the smallest id.
    std::set<int> remaining;
    for (int id = 1; id <= 8; ++id) {
        buf.enqueue(makeMessage(id), 0, id, false, false);
        remaining.insert(id);
    }

    advanceTo(8);

    // Remove messages out of order from assorted interior heap positions.
    // This drives siftHeapEntry in both directions; after each removal the
    // head must still be the oldest remaining message, proving the heap
    // invariant survived the O(log n) sift (vs. the old make_heap rebuild).
    for (int id : {5, 2, 8, 3, 6}) {
        auto selected = buf.selectEligible(idIs(id), 8);
        ASSERT_TRUE(selected.valid()) << "select id " << id;
        EXPECT_EQ(selectedId(buf, selected), id);
        buf.popAt(selected, 8, 10);
        remaining.erase(id);

        auto head = buf.selectHead(MessagePredicate(), 8);
        ASSERT_TRUE(head.valid()) << "after removing " << id;
        EXPECT_EQ(selectedId(buf, head), *remaining.begin())
            << "after removing " << id;
    }

    // Whatever is left must drain in strict ascending age order.
    std::vector<int> drained;
    for (;;) {
        auto head = buf.selectHead(MessagePredicate(), 8);
        if (!head.valid()) {
            break;
        }
        drained.push_back(selectedId(buf, head));
        buf.popAt(head, 8, 10);
    }

    EXPECT_EQ(drained, std::vector<int>({1, 4, 7}));
}

TEST_F(MessageBufferCreditTest, OutOfOrderRemovalOfLastHeapEntry)
{
    // Exercise the index == last branch (plain pop_back, no sift) by removing
    // the youngest message, which is a heap leaf, then confirm the rest still
    // drain oldest-first.
    auto &buf = makeBuffer(4, 4, Cycles(1));

    for (int id = 1; id <= 4; ++id) {
        buf.enqueue(makeMessage(id), 0, id, false, false);
    }

    advanceTo(4);

    auto youngest = buf.selectEligible(idIs(4), 4);
    ASSERT_TRUE(youngest.valid());
    buf.popAt(youngest, 4, 10);

    std::vector<int> drained;
    for (;;) {
        auto head = buf.selectHead(MessagePredicate(), 4);
        if (!head.valid()) {
            break;
        }
        drained.push_back(selectedId(buf, head));
        buf.popAt(head, 4, 10);
    }

    EXPECT_EQ(drained, std::vector<int>({1, 2, 3}));
}

TEST_F(MessageBufferCreditTest, SelectEligibleEmptyPredicateReturnsHead)
{
    // The no-predicate fast path must answer with the heap head (oldest
    // matured message) without walking the ready set.
    auto &buf = makeBuffer(3, 3, Cycles(1));

    buf.enqueue(makeMessage(7), 0, 1, false, false);  // oldest -> head
    buf.enqueue(makeMessage(8), 0, 2, false, false);
    buf.enqueue(makeMessage(9), 0, 3, false, false);

    advanceTo(3);
    auto sel = buf.selectEligible(MessagePredicate(), 3);
    ASSERT_TRUE(sel.valid());
    EXPECT_EQ(sel.index, 0u);
    EXPECT_EQ(selectedId(buf, sel), 7);
}

TEST_F(MessageBufferCreditTest, SelectEligibleSkipsRejectedHeadToOldestMatch)
{
    // Pruned-traversal parity with the old O(n) flat scan: a predicate that
    // rejects the oldest entries must still yield the oldest *matching* one.
    auto &buf = makeBuffer(5, 5, Cycles(1));

    for (int id = 1; id <= 5; ++id) {
        buf.enqueue(makeMessage(id), 0, id, false, false);
    }

    advanceTo(5);
    auto pred = [](const Message &message) {
        return asTestMessage(message).id() >= 4;
    };
    auto sel = buf.selectEligible(pred, 5);
    ASSERT_TRUE(sel.valid());
    EXPECT_EQ(selectedId(buf, sel), 4);
}

TEST_F(MessageBufferCreditTest, SelectEligibleFindsOldestAmongManyReady)
{
    // A multi-level heap with a mix of matured and immature messages. The
    // O(n_ready) traversal must visit every matured node (so it finds the
    // oldest even id) and must never descend into the immature subtree.
    auto &buf = makeBuffer(0, 0, Cycles(0));

    for (int id = 1; id <= 12; ++id) {
        buf.enqueue(makeMessage(id), 0, id, false, false);
    }

    advanceTo(8);  // ids 1..8 matured; 9..12 still immature

    auto evens = [](const Message &message) {
        return (asTestMessage(message).id() % 2) == 0;
    };
    auto sel = buf.selectEligible(evens, 8);
    ASSERT_TRUE(sel.valid());
    EXPECT_EQ(selectedId(buf, sel), 2);

    // id 10 matches the predicate but is immature: it must be pruned, not
    // selected, until its arrival time passes.
    EXPECT_FALSE(buf.selectEligible(idIs(10), 8).valid());

    advanceTo(10);
    auto ten = buf.selectEligible(idIs(10), 10);
    ASSERT_TRUE(ten.valid());
    EXPECT_EQ(selectedId(buf, ten), 10);
}

TEST_F(MessageBufferCreditTest, SelectEligibleNeverReturnsImmatureMessage)
{
    auto &buf = makeBuffer(4, 4, Cycles(1));

    buf.enqueue(makeMessage(1), 0, 10, false, false);  // matures at 10
    buf.enqueue(makeMessage(2), 0, 20, false, false);  // matures at 20

    advanceTo(5);
    EXPECT_FALSE(buf.selectEligible(MessagePredicate(), 5).valid());
    EXPECT_FALSE(buf.selectBest(MessagePredicate(),
                                MessageBuffer::MessageRank(), 5).valid());

    advanceTo(10);
    EXPECT_FALSE(buf.selectEligible(idIs(2), 10).valid());
    auto sel = buf.selectEligible(idIs(1), 10);
    ASSERT_TRUE(sel.valid());
    EXPECT_EQ(selectedId(buf, sel), 1);
}

TEST_F(MessageBufferCreditTest, SelectBestPicksHighestPriorityThenOldest)
{
    // QoS-style ranking: prefer higher priority, break ties by age. Among the
    // high-priority ready messages {id 2, id 3}, id 2 is older and must win,
    // even though id 1 is the global oldest and id 3 is also high priority.
    auto &buf = makeBuffer(6, 6, Cycles(1));

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    buf.enqueue(makeMessage(2), 0, 2, false, false);
    buf.enqueue(makeMessage(3), 0, 3, false, false);
    buf.enqueue(makeMessage(4), 0, 4, false, false);

    advanceTo(4);
    auto priority = [](const Message &message) {
        int id = asTestMessage(message).id();
        return (id == 2 || id == 3) ? 2 : 1;
    };
    MessageBuffer::MessageRank rank =
        [&](const Message &a, const Message &b) {
            int pa = priority(a);
            int pb = priority(b);
            if (pa != pb) {
                return pa > pb;
            }
            return a.getLastEnqueueTime() < b.getLastEnqueueTime();
        };

    auto best = buf.selectBest(MessagePredicate(), rank, 4);
    ASSERT_TRUE(best.valid());
    EXPECT_EQ(selectedId(buf, best), 2);
}

TEST_F(MessageBufferCreditTest, SelectBestRespectsMaxDequeueRate)
{
    auto &buf = makeBuffer(2, 2, Cycles(1), true, 1);

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    buf.enqueue(makeMessage(2), 0, 1, false, false);

    advanceTo(1);
    MessageBuffer::MessageRank oldest =
        [](const Message &a, const Message &b) {
            return a.getLastEnqueueTime() < b.getLastEnqueueTime();
        };

    auto first = buf.selectBest(MessagePredicate(), oldest, 1);
    ASSERT_TRUE(first.valid());
    buf.popAt(first, 1, 10);

    EXPECT_FALSE(buf.selectBest(MessagePredicate(), oldest, 1).valid());

    advanceTo(2);
    EXPECT_TRUE(buf.selectBest(MessagePredicate(), oldest, 2).valid());
}

TEST_F(MessageBufferCreditTest, SelectBestUnchangedWhenOooDisabled)
{
    // selectBest still walks the matured set regardless of enable_ooo_pop;
    // the OoO toggle only governs selectEligible's head-vs-oldest behavior.
    // Confirm selectEligible stays head-only while selectBest does not, so
    // disabling OoO does not silently change either contract.
    auto &buf = makeBuffer(3, 3, Cycles(1), false);

    buf.enqueue(makeMessage(1), 0, 1, false, false);
    buf.enqueue(makeMessage(2), 0, 2, false, false);
    buf.enqueue(makeMessage(3), 0, 3, false, false);

    advanceTo(3);

    // OoO disabled: selectEligible cannot skip the head for id 3.
    EXPECT_FALSE(buf.selectEligible(idIs(3), 3).valid());
    auto head = buf.selectEligible(MessagePredicate(), 3);
    ASSERT_TRUE(head.valid());
    EXPECT_EQ(selectedId(buf, head), 1);

    // selectBest is the explicit OoO primitive and still reaches id 3.
    MessageBuffer::MessageRank oldest =
        [](const Message &a, const Message &b) {
            return a.getLastEnqueueTime() < b.getLastEnqueueTime();
        };
    auto picked = buf.selectBest(idIs(3), oldest, 3);
    ASSERT_TRUE(picked.valid());
    EXPECT_EQ(selectedId(buf, picked), 3);
}

TEST_F(MessageBufferCreditTest, MultiplePopsPerCycleReSelectOneHandleAtATime)
{
    // Selecting and popping repeatedly within one cycle (one live handle at a
    // time) must drain the high-priority class first, then fall through to the
    // rest, exercising several O(n_ready) selections after interior removals.
    auto &buf = makeBuffer(6, 6, Cycles(1));

    for (int id = 1; id <= 6; ++id) {
        buf.enqueue(makeMessage(id), 0, id, false, false);
    }

    advanceTo(6);
    auto priority = [](const Message &message) {
        int id = asTestMessage(message).id();
        return (id == 3 || id == 5) ? 2 : 1;  // high-priority class
    };
    MessageBuffer::MessageRank rank =
        [&](const Message &a, const Message &b) {
            int pa = priority(a);
            int pb = priority(b);
            if (pa != pb) {
                return pa > pb;
            }
            return a.getLastEnqueueTime() < b.getLastEnqueueTime();
        };

    std::vector<int> popped;
    for (;;) {
        auto h = buf.selectBest(MessagePredicate(), rank, 6);
        if (!h.valid()) {
            break;
        }
        popped.push_back(selectedId(buf, h));
        buf.popAt(h, 6, 10);  // h is dead after this; loop re-selects
    }

    // High priority (3 then 5) drains first, then the rest oldest-first.
    EXPECT_EQ(popped, std::vector<int>({3, 5, 1, 2, 4, 6}));
}

} // anonymous namespace
} // namespace ruby
} // namespace gem5
