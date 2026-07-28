#include "dev/hsakmt-service/service/doorbell_bridge.hh"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace gem5_hsakmt
{
namespace
{

TEST(DoorbellBridge, DeliversRegisteredRecordsInOrder)
{
    std::vector<std::pair<uint32_t, uint64_t>> writes;
    DoorbellBridge bridge(
        [&](uint32_t offset, uint64_t value) {
            writes.emplace_back(offset, value);
        });
    ASSERT_TRUE(bridge.registerQueue(1, 7, 0x4000));
    EXPECT_TRUE(bridge.deliver({1, 7, 0x4000, 2}));
    EXPECT_TRUE(bridge.deliver({1, 7, 0x4000, 3}));
    EXPECT_EQ(writes, (std::vector<std::pair<uint32_t, uint64_t>>{
                          {0x4000, 2}, {0x4000, 3}}));
}

TEST(DoorbellBridge, RejectsStaleOrMismatchedRecord)
{
    uint64_t writes = 0;
    DoorbellBridge bridge(
        [&](uint32_t, uint64_t) { ++writes; });
    ASSERT_TRUE(bridge.registerQueue(1, 7, 0x4000));
    EXPECT_FALSE(bridge.deliver({1, 8, 0x4000, 1}));
    EXPECT_FALSE(bridge.deliver({1, 7, 0x4008, 1}));
    ASSERT_TRUE(bridge.unregisterQueue(1, 7));
    EXPECT_FALSE(bridge.deliver({1, 7, 0x4000, 1}));
    EXPECT_EQ(writes, 0);
}

} // anonymous namespace
} // namespace gem5_hsakmt
