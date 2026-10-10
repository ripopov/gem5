/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "sim/async_event_batch.hh"

namespace gem5
{
namespace
{

// These opaque tokens are transported but never dereferenced.
Event *
token(uintptr_t value)
{
    return reinterpret_cast<Event *>(value);
}

TEST(AsyncEventBatchTest, PublicationAndRepeatedPublicationPreserveOrder)
{
    AsyncEventBatch batch;
    std::vector<Event *> received;
    auto consume = [&](Event *event) { received.push_back(event); };
    EXPECT_TRUE(batch.append(token(1)));
    EXPECT_FALSE(batch.append(token(2)));
    batch.drain(consume);
    EXPECT_TRUE(received.empty());
    batch.publish();
    EXPECT_TRUE(batch.append(token(3)));
    batch.publish();
    batch.drain(consume);
    EXPECT_EQ(received, (std::vector<Event *>{token(1), token(2), token(3)}));
    batch.publish();
    batch.drain(consume);
    EXPECT_EQ(received.size(), 3);
}

TEST(AsyncEventBatchTest, PublicationCanOverlapConsumption)
{
    AsyncEventBatch batch;
    batch.append(token(1));
    batch.publish();
    // The callback runs without the channel lock, and publication cannot
    // mutate the batch currently being consumed.
    batch.drain([&](Event *event) {
        EXPECT_EQ(event, token(1));
        batch.append(token(2));
        batch.publish();
    });
    unsigned count = 0;
    batch.drain([&](Event *event) {
        EXPECT_EQ(event, token(2));
        ++count;
    });
    EXPECT_EQ(count, 1);
}

TEST(AsyncEventBatchTest, ConcurrentGrowthAndBufferReuse)
{
    AsyncEventBatch batch;
    constexpr uintptr_t Count = 100000;
    std::atomic<bool> finished{false};
    std::thread producer([&] {
        for (uintptr_t i = 1; i <= Count; ++i) {
            batch.append(token(i));
            if (i % 13 == 0) {
                batch.publish();
            }
        }
        batch.publish();
        finished.store(true, std::memory_order_release);
    });
    uintptr_t next = 1;
    auto consume = [&](Event *event) { EXPECT_EQ(event, token(next++)); };
    while (!finished.load(std::memory_order_acquire)) {
        batch.drain(consume);
        std::this_thread::yield();
    }
    batch.drain(consume);
    producer.join();
    EXPECT_EQ(next, Count + 1);
}

} // anonymous namespace
} // namespace gem5
