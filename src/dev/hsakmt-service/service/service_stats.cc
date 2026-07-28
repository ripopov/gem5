#include "dev/hsakmt-service/service/service_stats.hh"

#include <stdexcept>

namespace gem5_hsakmt
{
namespace
{

template <class Enum, size_t Size>
size_t
checkedIndex(Enum value, const std::array<uint64_t, Size> &)
{
    const size_t index = static_cast<size_t>(value);
    if (index >= Size) {
        throw std::out_of_range("invalid HSAKMT service statistic");
    }
    return index;
}

} // anonymous namespace

uint64_t
ServiceStatsSnapshot::counter(ServiceCounter counter) const
{
    return counters[checkedIndex(counter, counters)];
}

void
ServiceStats::record(ServiceCounter counter, uint64_t amount)
{
    data.counters[checkedIndex(counter, data.counters)] += amount;
}

ServiceStatsSnapshot
ServiceStats::snapshot() const
{
    return data;
}

} // namespace gem5_hsakmt
