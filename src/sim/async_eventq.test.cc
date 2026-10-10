/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <array>
#include <barrier>
#include <thread>

#include "sim/eventq.hh"
#include "sim/global_event.hh"

namespace gem5
{
namespace
{

class AsyncEventQueueTest : public ::testing::Test
{
  protected:
    EventQueue source{"source"};
    EventQueue destination{"destination"};
    EventQueue *previous = curEventQueue();
    bool wasParallel = inParallelMode;

    void
    SetUp() override
    {
        source.setNextSimQuantum(0);
        destination.setNextSimQuantum(0);
        curEventQueue(&source);
        inParallelMode = true;
    }

    void
    TearDown() override
    {
        curEventQueue(previous);
        inParallelMode = wasParallel;
    }

    void
    collect()
    {
        curEventQueue(&destination);
        destination.handleAsyncInsertions();
        curEventQueue(&source);
    }
};

TEST_F(AsyncEventQueueTest, WorkerPublicationAndPriority)
{
    EventQueue::ScopedAsyncBatching batching(&source);
    int order = 0;
    EventFunctionWrapper low([&] { EXPECT_EQ(++order, 2); }, "low");
    EventFunctionWrapper high([&] { EXPECT_EQ(++order, 1); }, "high", false,
                              Event::Minimum_Pri);
    destination.schedule(&low, 10);
    destination.schedule(&high, 10);
    collect();
    EXPECT_TRUE(destination.empty());
    EventQueue::publishAsyncBatches();
    collect();
    curEventQueue(&destination);
    destination.serviceOne();
    destination.serviceOne();
    EXPECT_EQ(order, 2);
}

TEST_F(AsyncEventQueueTest, ExternalThreadDoesNotShareWorkerStaging)
{
    EventQueue::ScopedAsyncBatching batching(&source);
    EventFunctionWrapper worker([] {}, "worker");
    EventFunctionWrapper external([] {}, "external");
    destination.schedule(&worker, 10);
    std::thread other([&] {
        EventQueue external_source("external source");
        curEventQueue(&external_source);
        destination.schedule(&external, 20);
    });
    other.join();
    collect();
    EXPECT_EQ(destination.getHead(), &external);
    EventQueue::publishAsyncBatches();
    collect();
    curEventQueue(&destination);
    destination.deschedule(&worker);
    destination.deschedule(&external);
}

TEST_F(AsyncEventQueueTest, GlobalPostsKeepTheirSharedOrder)
{
    EventFunctionWrapper first([] {}, "first");
    EventFunctionWrapper second([] {}, "second");
    {
        EventQueue::ScopedAsyncBatching batching(&source);
        destination.schedule(&first, 10, true);
    }
    {
        EventQueue other("other");
        EventQueue::ScopedAsyncBatching batching(&other);
        destination.schedule(&second, 10, true);
    }
    collect();
    // Equal-key events use gem5's existing LIFO insertion rule.
    EXPECT_EQ(destination.getHead(), &second);
    curEventQueue(&destination);
    destination.deschedule(&second);
    destination.deschedule(&first);
}

TEST_F(AsyncEventQueueTest, ConcurrentExternalPostsPreserveProducerOrder)
{
    constexpr unsigned Producers = 5;
    constexpr unsigned Events = 128;
    for (unsigned round = 1; round <= 8; ++round) {
        std::array<unsigned, Producers> received{};
        std::barrier start(Producers);
        std::vector<std::thread> threads;
        for (unsigned producer = 0; producer < Producers; ++producer) {
            threads.emplace_back([&, producer] {
                EventQueue external("external source");
                curEventQueue(&external);
                start.arrive_and_wait();
                for (unsigned i = 0; i < Events; ++i) {
                    auto *event = new EventFunctionWrapper(
                        [&, producer, i] {
                            // Equal-key events retain the queue's LIFO rule,
                            // including each producer's posting order.
                            EXPECT_EQ(received[producer]++, Events - 1 - i);
                        },
                        "external message", true);
                    destination.schedule(event, round);
                }
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
        collect();
        curEventQueue(&destination);
        while (!destination.empty()) {
            destination.serviceOne();
        }
        for (unsigned count : received) {
            EXPECT_EQ(count, Events);
        }
        curEventQueue(&source);
    }
}

TEST_F(AsyncEventQueueTest, SuspendedBatchingPublishesCallbackPostsDirectly)
{
    EventQueue::ScopedAsyncBatching batching(&source);
    EventFunctionWrapper worker([] {}, "worker");
    EventFunctionWrapper callback([] {}, "callback");
    destination.schedule(&worker, 10);
    {
        EventQueue::ScopedAsyncBatching callback_scope(nullptr);
        destination.schedule(&callback, 20);
    }
    collect();
    EXPECT_EQ(destination.getHead(), &callback);
    EventQueue::publishAsyncBatches();
    collect();
    curEventQueue(&destination);
    destination.deschedule(&worker);
    destination.deschedule(&callback);
}

TEST_F(AsyncEventQueueTest, ArbitraryConnectivityAndRepeatedCollection)
{
    constexpr unsigned Queues = 5;
    constexpr unsigned Epochs = 100;
    std::vector<std::unique_ptr<EventQueue>> queues;
    for (unsigned i = 0; i < Queues; ++i) {
        queues.emplace_back(new EventQueue("mesh"));
        queues.back()->setNextSimQuantum(0);
    }
    std::barrier barrier(Queues);
    std::vector<std::thread> threads;
    for (unsigned source = 0; source < Queues; ++source) {
        threads.emplace_back([&, source] {
            auto *queue = queues[source].get();
            curEventQueue(queue);
            EventQueue::ScopedAsyncBatching batching(queue);
            unsigned received = 0;
            for (unsigned epoch = 1; epoch <= Epochs; ++epoch) {
                for (unsigned target = 0; target < Queues; ++target) {
                    if (source == target) {
                        continue;
                    }
                    auto *event =
                        new EventFunctionWrapper([] {}, "mesh message", true);
                    queues[target]->schedule(event, epoch);
                }
                EventQueue::publishAsyncBatches();
                barrier.arrive_and_wait();
                queue->handleAsyncInsertions();
                while (!queue->empty() && queue->nextTick() == epoch) {
                    queue->serviceOne();
                    ++received;
                }
                EXPECT_EQ(received, epoch * (Queues - 1));
                barrier.arrive_and_wait();
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
}

class PostingGlobalEvent : public GlobalEvent
{
  private:
    EventQueue &destination;
    Event &posted;

  public:
    PostingGlobalEvent(EventQueue &destination, Event &posted)
        : GlobalEvent(Event::Default_Pri, 0),
          destination(destination),
          posted(posted)
    {}

    void
    process() override
    {
        destination.schedule(&posted, 20);
    }
    const char *
    description() const override
    {
        return "posting global";
    }
};

TEST_F(AsyncEventQueueTest, OrdinaryBarrierFlushesBeforeExitAndCallback)
{
    // One participant is enough to exercise the actual completion path.
    auto *main = getEventQueue(0);
    curEventQueue(main);
    EventQueue::ScopedAsyncBatching batching(main);
    EventFunctionWrapper worker([] {}, "worker");
    EventFunctionWrapper callback([] {}, "callback");
    destination.schedule(&worker, 10);
    PostingGlobalEvent event(destination, callback);
    GlobalEvent::BarrierEvent local(&event, Event::Default_Pri, 0);
    {
        std::lock_guard<EventQueue> lock(*main);
        local.process();
    }
    collect();
    EXPECT_EQ(destination.getHead(), &worker);
    curEventQueue(&destination);
    destination.deschedule(&worker);
    EXPECT_EQ(destination.getHead(), &callback);
    destination.deschedule(&callback);
}

} // anonymous namespace
} // namespace gem5
