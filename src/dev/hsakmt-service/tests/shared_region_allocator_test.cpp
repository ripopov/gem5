#include "dev/hsakmt-service/service/shared_region_allocator.hh"

#include <gtest/gtest.h>

namespace gem5_hsakmt
{
namespace
{

TEST(SharedRegionAllocator, AlignsAndReportsRequestedSize)
{
    SharedRegionAllocator allocator(0x1003, 0x10000, 0x1000);
    const auto allocation = allocator.allocate(1);
    ASSERT_TRUE(allocation);
    EXPECT_EQ(allocation->address, 0x2000);
    EXPECT_EQ(allocation->size, 0x1000);
    EXPECT_EQ(allocation->requestedSize, 1);
}

TEST(SharedRegionAllocator, ExhaustionDoesNotPublishAllocation)
{
    SharedRegionAllocator allocator(0, 0x2000);
    ASSERT_TRUE(allocator.allocate(0x1000));
    ASSERT_TRUE(allocator.allocate(0x1000));
    EXPECT_FALSE(allocator.allocate(1));
    EXPECT_EQ(allocator.allocationCount(), 2);
    EXPECT_EQ(allocator.bytesInUse(), 0x2000);
}

TEST(SharedRegionAllocator, ReleaseCoalescesAndReusesExtent)
{
    SharedRegionAllocator allocator(0x10000, 0x4000);
    const auto first = allocator.allocate(0x1000);
    const auto second = allocator.allocate(0x1000);
    const auto third = allocator.allocate(0x2000);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(third);

    EXPECT_TRUE(allocator.release(second->id));
    EXPECT_TRUE(allocator.release(first->id));
    const auto replacement = allocator.allocate(0x2000);
    ASSERT_TRUE(replacement);
    EXPECT_EQ(replacement->address, 0x10000);
    EXPECT_FALSE(allocator.release(second->id));
}

TEST(SharedRegionAllocator, RejectsInvalidAlignmentAndOverflow)
{
    SharedRegionAllocator allocator(0, 0x4000);
    EXPECT_FALSE(allocator.allocate(0));
    EXPECT_FALSE(allocator.allocate(1, 3));
    EXPECT_FALSE(allocator.allocate(UINT64_MAX));
}

} // anonymous namespace
} // namespace gem5_hsakmt
