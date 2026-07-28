#include "dev/hsakmt-service/service/queue_work_engine.hh"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace
{

using namespace gem5_hsakmt;

struct FakeHardware
{
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> blobs;
    std::vector<std::pair<uint64_t, uint64_t>> doorbells;
    uint64_t mmioWrites = 0;
    bool mapProcessV2 = false;

    QueueWorkHardware
    callbacks()
    {
        return {
            [this](uint64_t address, uint64_t value, unsigned size) {
                std::vector<uint8_t> bytes(size);
                std::memcpy(bytes.data(), &value, size);
                blobs.emplace_back(address, std::move(bytes));
            },
            [this](uint64_t address, const void *data, uint64_t size) {
                const auto *bytes = static_cast<const uint8_t *>(data);
                blobs.emplace_back(address,
                                   std::vector<uint8_t>(bytes, bytes + size));
            },
            [this](uint64_t offset, uint64_t value) {
                doorbells.emplace_back(offset, value);
            },
            [this](uint64_t, uint32_t) { ++mmioWrites; },
            [] { return uint64_t{0x100000}; },
            [] { return uint64_t{0x200000}; },
            [] { return false; },
            [this] { return mapProcessV2; },
        };
    }
};

QueueWorkEngineConfig
makeConfig()
{
    QueueWorkEngineConfig config;
    config.setupMemoryBase = 0x1000000;
    config.interruptRingOffset = 0;
    config.interruptRingBytes = 0x1000;
    config.interruptWptrOffset = 0x1000;
    config.interruptDoorbellOffset = 0x2000;
    return config;
}

DirectHardwareQueue
makeQueue(uint64_t id = 7)
{
    DirectHardwareQueue queue;
    queue.queueId = id;
    queue.ringBaseVa = 0x200000;
    queue.ringBytes = 4096;
    queue.readPointerVa = 0x300000;
    queue.writePointerVa = 0x300008;
    queue.queueDescriptorVa = 0x310000;
    queue.doorbellOffset = 0x100 + id * 4;
    return queue;
}

TEST(QueueWorkEngineTest, ConfiguresPrivatePm4QueueOnce)
{
    ServiceStats stats;
    FakeHardware fake;
    QueueWorkEngine engine(makeConfig(), fake.callbacks(), stats);
    std::string error;

    ASSERT_TRUE(engine.configurePm4ControlQueue(&error)) << error;
    EXPECT_EQ(fake.mmioWrites, 11);
    EXPECT_EQ(fake.blobs.size(), 2);
    EXPECT_TRUE(fake.doorbells.empty());

    ASSERT_TRUE(engine.configurePm4ControlQueue(&error)) << error;
    EXPECT_EQ(fake.mmioWrites, 11);
}

TEST(QueueWorkEngineTest, UsesHardwareSpecificMapProcessPacketSize)
{
    ServiceStats stats;
    FakeHardware v1;
    QueueWorkEngine v1Engine(makeConfig(), v1.callbacks(), stats);
    std::string error;
    ASSERT_TRUE(v1Engine.configureDirectProcess(
        0x8000, 0x4000, 0x1000000000000, 0x2000000000000, &error))
        << error;
    EXPECT_EQ(v1Engine.controlWriteSequence(), 16);

    FakeHardware v2;
    v2.mapProcessV2 = true;
    QueueWorkEngine v2Engine(makeConfig(), v2.callbacks(), stats);
    ASSERT_TRUE(v2Engine.configureDirectProcess(
        0x8000, 0x4000, 0x1000000000000, 0x2000000000000, &error))
        << error;
    EXPECT_EQ(v2Engine.controlWriteSequence(), 21);
}

TEST(QueueWorkEngineTest, RegistersUpdatesAndDestroysComputeQueue)
{
    ServiceStats stats;
    FakeHardware fake;
    QueueWorkEngine engine(makeConfig(), fake.callbacks(), stats);
    DirectHardwareQueue queue = makeQueue();
    std::string error;

    ASSERT_TRUE(engine.registerDirectQueue(queue, &error)) << error;
    EXPECT_EQ(engine.registeredQueueCount(), 1);
    EXPECT_EQ(fake.mmioWrites, 11);
    ASSERT_FALSE(fake.doorbells.empty());
    const size_t firstDoorbells = fake.doorbells.size();

    ASSERT_TRUE(engine.registerDirectQueue(queue, &error)) << error;
    EXPECT_EQ(fake.doorbells.size(), firstDoorbells);

    DirectHardwareQueue changed = queue;
    changed.doorbellOffset += 4;
    EXPECT_FALSE(engine.registerDirectQueue(changed, &error));
    EXPECT_EQ(error, "direct queue changed its doorbell");

    ASSERT_TRUE(engine.updateDirectQueue(
        queue.queueId, 0x400000, 8192, &error))
        << error;
    EXPECT_EQ(engine.registeredQueueCount(), 1);
    EXPECT_EQ(fake.doorbells.size(), firstDoorbells + 2);

    ASSERT_TRUE(engine.unregisterDirectQueue(queue.queueId, &error)) << error;
    EXPECT_EQ(engine.registeredQueueCount(), 0);
    EXPECT_EQ(fake.doorbells.size(), firstDoorbells + 3);
    ASSERT_TRUE(engine.unregisterDirectQueue(queue.queueId, &error)) << error;
}

TEST(QueueWorkEngineTest, InstallsSdmaQueueDescriptor)
{
    ServiceStats stats;
    FakeHardware fake;
    QueueWorkEngine engine(makeConfig(), fake.callbacks(), stats);
    DirectHardwareQueue queue = makeQueue();
    queue.isSdma = true;
    std::string error;

    ASSERT_TRUE(engine.registerDirectQueue(queue, &error)) << error;
    EXPECT_EQ(engine.registeredQueueCount(), 1);
    ASSERT_GE(fake.blobs.size(), 5);
}

TEST(QueueWorkEngineTest, RejectsMalformedQueueMetadata)
{
    ServiceStats stats;
    FakeHardware fake;
    QueueWorkEngine engine(makeConfig(), fake.callbacks(), stats);
    DirectHardwareQueue queue = makeQueue();
    queue.ringBytes = 6144;
    std::string error;

    EXPECT_FALSE(engine.registerDirectQueue(queue, &error));
    EXPECT_EQ(error, "direct queue metadata is invalid");
    EXPECT_EQ(engine.registeredQueueCount(), 0);
}

TEST(QueueWorkEngineTest, RecyclesHardwareQueueSlots)
{
    ServiceStats stats;
    FakeHardware fake;
    QueueWorkEngine engine(makeConfig(), fake.callbacks(), stats);
    std::string error;

    for (uint64_t id = 1; id <= 96; ++id) {
        ASSERT_TRUE(engine.registerDirectQueue(makeQueue(id), &error))
            << "queue " << id << ": " << error;
        ASSERT_TRUE(engine.unregisterDirectQueue(id, &error))
            << "queue " << id << ": " << error;
        engine.releaseControlThrough(engine.controlWriteSequence());
    }
    EXPECT_EQ(engine.registeredQueueCount(), 0);
    EXPECT_EQ(engine.controlReservedDwords(), 0);
}

} // anonymous namespace
