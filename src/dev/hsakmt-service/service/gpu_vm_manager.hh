#ifndef __DEV_HSAKMT_SERVICE_GPU_VM_MANAGER_HH__
#define __DEV_HSAKMT_SERVICE_GPU_VM_MANAGER_HH__

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "dev/hsakmt-service/service/service_stats.hh"

namespace gem5_hsakmt
{

enum class GpuVmMappingKind
{
    HostControlDma,
    DeviceLocal,
};

enum class GpuVmResourceKind
{
    Service,
    Allocation,
};

struct GpuVmOwner
{
    GpuVmResourceKind kind = GpuVmResourceKind::Service;
    uint64_t id = 0;

    bool
    operator==(const GpuVmOwner &other) const
    {
        return kind == other.kind && id == other.id;
    }
    bool
    operator!=(const GpuVmOwner &other) const
    {
        return !(*this == other);
    }

    static GpuVmOwner
    allocation(uint64_t handle)
    {
        return {GpuVmResourceKind::Allocation, handle};
    }
};

struct GpuVmManagerConfig
{
    uint64_t pageTableOffset = 0;
    uint64_t pageTableBytes = 0;
    uint16_t vmId = 1;
};

struct GpuVmHardware
{
    std::function<void(uint64_t address, uint64_t value, unsigned size)>
        writeValue;
    std::function<void(uint16_t vm_id, uint64_t root)> configurePageTable;
};

struct GpuVmTranslation
{
    uint64_t virtualPage = 0;
    uint64_t physicalPage = 0;
    uint64_t physicalAddress = 0;
    GpuVmMappingKind kind = GpuVmMappingKind::DeviceLocal;
    GpuVmOwner owner;
};

/**
 * Owns service-created GPU page tables for rocjitsu allocations.
 *
 * The manager maps exact shared-VRAM extents and tracks their allocation
 * owner. It deliberately contains no trace snapshots, mirrors, code-object
 * placement, or packet-staging policy.
 */
class GpuVmManager
{
  public:
    static constexpr uint64_t PageBytes = 4096;
    static constexpr uint64_t PageMask = PageBytes - 1;

    GpuVmManager(GpuVmManagerConfig config, GpuVmHardware hardware,
                 ServiceStats &stats);

    bool map(uint64_t virtual_address, uint64_t physical_address,
             uint64_t size, GpuVmMappingKind kind, std::string *error);
    bool mapOwned(GpuVmOwner owner, uint64_t virtual_address,
                  uint64_t physical_address, uint64_t size,
                  GpuVmMappingKind kind, std::string *error);
    bool remapOwnedKind(GpuVmOwner owner, uint64_t virtual_address,
                        uint64_t size, GpuVmMappingKind kind,
                        std::string *error);
    bool releaseOwner(GpuVmOwner owner, std::string *error = nullptr);
    bool releaseAllocation(uint64_t handle, std::string *error = nullptr);
    std::optional<GpuVmTranslation> translate(uint64_t address) const;

    uint64_t
    pageTableRoot() const
    {
        return pageTableRootAddress;
    }
    size_t
    mappingCount() const
    {
        return mappings.size();
    }
    size_t ownedMappingCount(GpuVmOwner owner) const;

  private:
    struct Mapping
    {
        uint64_t physicalPage = 0;
        GpuVmMappingKind kind = GpuVmMappingKind::DeviceLocal;
        GpuVmOwner owner;

        bool
        operator==(const Mapping &other) const
        {
            return physicalPage == other.physicalPage &&
                kind == other.kind && owner == other.owner;
        }
        bool
        operator!=(const Mapping &other) const
        {
            return !(*this == other);
        }
    };

    struct StateSnapshot
    {
        uint64_t nextPageTableAddress = 0;
        uint64_t pageTableRootAddress = 0;
        std::vector<uint64_t> freePageTableAddresses;
        std::unordered_map<uint64_t, uint64_t> pageTableChildren;
        std::unordered_map<uint64_t, Mapping> mappings;
    };

    static uint64_t alignDown(uint64_t value);
    static bool alignUp(uint64_t value, uint64_t *result);
    static void setError(std::string *error, const std::string &message);

    bool allocatePageTablePage(uint64_t *page, std::string *error);
    bool ensurePageTableChild(uint64_t table, uint64_t index, uint64_t *child,
                              std::string *error);
    bool ensurePageTableRoot(std::string *error);
    std::optional<uint64_t> pteAddress(uint64_t page_virtual) const;
    void writeMappingPte(uint64_t page_virtual, const Mapping &mapping);
    void invalidatePageTable();
    void reclaimEmptyPageTables(uint64_t page_virtual);
    StateSnapshot snapshot() const;
    void rollback(StateSnapshot state);

    const GpuVmManagerConfig config;
    const GpuVmHardware hardware;
    ServiceStats &stats;
    uint64_t nextPageTableAddress = 0;
    uint64_t pageTableRootAddress = 0;
    std::vector<uint64_t> freePageTableAddresses;
    std::unordered_map<uint64_t, uint64_t> pageTableChildren;
    std::unordered_map<uint64_t, Mapping> mappings;
};

} // namespace gem5_hsakmt

#endif // __DEV_HSAKMT_SERVICE_GPU_VM_MANAGER_HH__
