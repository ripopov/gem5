#ifndef __DEV_HSAKMT_SERVICE_SERVICE_STATS_HH__
#define __DEV_HSAKMT_SERVICE_SERVICE_STATS_HH__

#include <array>
#include <cstddef>
#include <cstdint>

namespace gem5_hsakmt
{

enum class ServiceCounter : size_t
{
    PageTablePages,
    MappedPages,
    RegisteredQueues,
    Count,
};

class ServiceStatsSnapshot
{
  public:
    uint64_t counter(ServiceCounter counter) const;

  private:
    friend class ServiceStats;
    std::array<uint64_t, static_cast<size_t>(ServiceCounter::Count)>
        counters{};
};

class ServiceStats
{
  public:
    void record(ServiceCounter counter, uint64_t amount = 1);
    ServiceStatsSnapshot snapshot() const;

  private:
    ServiceStatsSnapshot data;
};

} // namespace gem5_hsakmt

#endif // __DEV_HSAKMT_SERVICE_SERVICE_STATS_HH__
