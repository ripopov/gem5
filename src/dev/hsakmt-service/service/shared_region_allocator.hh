#ifndef __DEV_HSAKMT_SERVICE_SHARED_REGION_ALLOCATOR_HH__
#define __DEV_HSAKMT_SERVICE_SHARED_REGION_ALLOCATOR_HH__

#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>

namespace gem5_hsakmt
{

/**
 * Owns aligned, non-overlapping extents in one device-physical memory region.
 *
 * The allocator does not own the underlying bytes. Its allocation identity is
 * stable until release(), which lets the KMD layer keep handle policy separate
 * from gem5's physical-address ownership.
 */
class SharedRegionAllocator
{
  public:
    struct Allocation
    {
        uint64_t id = 0;
        uint64_t address = 0;
        uint64_t size = 0;
        uint64_t requestedSize = 0;
    };

    SharedRegionAllocator(uint64_t base, uint64_t size,
                          uint64_t minimum_alignment = 4096);

    std::optional<Allocation> allocate(uint64_t size,
                                       uint64_t alignment = 0);
    bool release(uint64_t id);
    std::optional<Allocation> find(uint64_t id) const;

    uint64_t
    bytesInUse() const
    {
        return bytes_in_use;
    }

    size_t
    allocationCount() const
    {
        return allocations.size();
    }

    uint64_t
    capacity() const
    {
        return region_size;
    }

  private:
    static bool isPowerOfTwo(uint64_t value);
    static bool alignUp(uint64_t value, uint64_t alignment,
                        uint64_t *result);
    void insertFreeExtent(uint64_t address, uint64_t size);

    const uint64_t region_base;
    const uint64_t region_size;
    const uint64_t minimum_alignment;
    uint64_t next_id = 1;
    uint64_t bytes_in_use = 0;
    std::map<uint64_t, uint64_t> free_extents;
    std::unordered_map<uint64_t, Allocation> allocations;
};

} // namespace gem5_hsakmt

#endif // __DEV_HSAKMT_SERVICE_SHARED_REGION_ALLOCATOR_HH__
