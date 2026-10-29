#include "dev/hsakmt-service/service/shared_region_allocator.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gem5_hsakmt
{

SharedRegionAllocator::SharedRegionAllocator(
    uint64_t base, uint64_t size, uint64_t alignment)
    : region_base(base),
      region_size(size),
      minimum_alignment(alignment)
{
    if (!size || !isPowerOfTwo(alignment) ||
        base > std::numeric_limits<uint64_t>::max() - size) {
        throw std::invalid_argument("invalid shared-region allocator range");
    }
    free_extents.emplace(base, size);
}

bool
SharedRegionAllocator::isPowerOfTwo(uint64_t value)
{
    return value && !(value & (value - 1));
}

bool
SharedRegionAllocator::alignUp(uint64_t value, uint64_t alignment,
                               uint64_t *result)
{
    if (!result || !isPowerOfTwo(alignment) ||
        value > std::numeric_limits<uint64_t>::max() - (alignment - 1)) {
        return false;
    }
    *result = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

std::optional<SharedRegionAllocator::Allocation>
SharedRegionAllocator::allocate(uint64_t requested_size, uint64_t alignment)
{
    if (alignment && !isPowerOfTwo(alignment)) {
        return std::nullopt;
    }
    alignment = alignment ? std::max(alignment, minimum_alignment)
                          : minimum_alignment;
    uint64_t size = 0;
    if (!requested_size || !isPowerOfTwo(alignment) ||
        !alignUp(requested_size, minimum_alignment, &size)) {
        return std::nullopt;
    }

    for (auto extent = free_extents.begin(); extent != free_extents.end();
         ++extent) {
        const uint64_t begin = extent->first;
        const uint64_t extent_size = extent->second;
        uint64_t address = 0;
        if (!alignUp(begin, alignment, &address) || address < begin ||
            address - begin > extent_size ||
            size > extent_size - (address - begin)) {
            continue;
        }

        const uint64_t prefix = address - begin;
        const uint64_t suffix = extent_size - prefix - size;
        free_extents.erase(extent);
        if (prefix) {
            free_extents.emplace(begin, prefix);
        }
        if (suffix) {
            free_extents.emplace(address + size, suffix);
        }

        while (!next_id || allocations.count(next_id)) {
            ++next_id;
        }
        Allocation allocation{
            next_id++, address, size, requested_size,
        };
        allocations.emplace(allocation.id, allocation);
        bytes_in_use += size;
        return allocation;
    }
    return std::nullopt;
}

bool
SharedRegionAllocator::release(uint64_t id)
{
    const auto allocation = allocations.find(id);
    if (allocation == allocations.end()) {
        return false;
    }
    const auto value = allocation->second;
    allocations.erase(allocation);
    bytes_in_use -= value.size;
    insertFreeExtent(value.address, value.size);
    return true;
}

std::optional<SharedRegionAllocator::Allocation>
SharedRegionAllocator::find(uint64_t id) const
{
    const auto allocation = allocations.find(id);
    if (allocation == allocations.end()) {
        return std::nullopt;
    }
    return allocation->second;
}

void
SharedRegionAllocator::insertFreeExtent(uint64_t address, uint64_t size)
{
    auto next = free_extents.lower_bound(address);
    if (next != free_extents.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == address) {
            address = previous->first;
            size += previous->second;
            free_extents.erase(previous);
        }
    }
    next = free_extents.lower_bound(address);
    if (next != free_extents.end() && address + size == next->first) {
        size += next->second;
        free_extents.erase(next);
    }
    free_extents.emplace(address, size);
}

} // namespace gem5_hsakmt
