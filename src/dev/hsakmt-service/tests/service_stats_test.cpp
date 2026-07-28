#include "dev/hsakmt-service/service/service_stats.hh"

#include <gtest/gtest.h>

TEST(ServiceStatsTest, SnapshotsAreImmutable)
{
    using gem5_hsakmt::ServiceCounter;

    gem5_hsakmt::ServiceStats stats;
    stats.record(ServiceCounter::MappedPages);
    stats.record(ServiceCounter::MappedPages, 4);
    const auto first = stats.snapshot();

    stats.record(ServiceCounter::MappedPages);
    const auto second = stats.snapshot();

    EXPECT_EQ(first.counter(ServiceCounter::MappedPages), 5);
    EXPECT_EQ(second.counter(ServiceCounter::MappedPages), 6);
}
