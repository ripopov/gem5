#include "dev/hsakmt-service/service/gpu_vm_manager.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gem5_hsakmt
{
namespace
{

constexpr uint64_t VegaSystemPteFlags =
    0x1u | 0x2u | 0x4u | 0x10u | 0x20u | 0x40u;
constexpr uint64_t VegaDevicePteFlags =
    0x1u | 0x4u | 0x10u | 0x20u | 0x40u;
constexpr uint64_t VegaPdeFlags = 0x1u | 0x4u;

bool
rangeEnd(uint64_t begin, uint64_t size, uint64_t *end)
{
    if (!end || size > std::numeric_limits<uint64_t>::max() - begin) {
        return false;
    }
    *end = begin + size;
    return true;
}

} // anonymous namespace

GpuVmManager::GpuVmManager(GpuVmManagerConfig manager_config,
                           GpuVmHardware manager_hardware,
                           ServiceStats &service_stats)
    : config(std::move(manager_config)),
      hardware(std::move(manager_hardware)),
      stats(service_stats)
{
    if (!hardware.writeValue || !hardware.configurePageTable) {
        throw std::invalid_argument(
            "GPUVM manager requires complete hardware callbacks");
    }
    if ((config.pageTableOffset & PageMask) ||
        (config.pageTableBytes & PageMask) || !config.pageTableBytes) {
        throw std::invalid_argument(
            "GPUVM page-table arena must be nonempty and page aligned");
    }
}

uint64_t
GpuVmManager::alignDown(uint64_t value)
{
    return value & ~PageMask;
}

bool
GpuVmManager::alignUp(uint64_t value, uint64_t *result)
{
    if (!result || value > std::numeric_limits<uint64_t>::max() - PageMask) {
        return false;
    }
    *result = (value + PageMask) & ~PageMask;
    return true;
}

void
GpuVmManager::setError(std::string *error, const std::string &message)
{
    if (error) {
        *error = message;
    }
}

bool
GpuVmManager::allocatePageTablePage(uint64_t *page, std::string *error)
{
    if (!page) {
        setError(error, "GPUVM page-table allocation output is null");
        return false;
    }
    if (!freePageTableAddresses.empty()) {
        *page = freePageTableAddresses.back();
        freePageTableAddresses.pop_back();
    } else {
        if (!nextPageTableAddress) {
            nextPageTableAddress = config.pageTableOffset;
        }
        uint64_t arena_end = 0;
        if (!rangeEnd(config.pageTableOffset, config.pageTableBytes,
                      &arena_end) ||
            nextPageTableAddress > arena_end ||
            PageBytes > arena_end - nextPageTableAddress) {
            setError(error, "exhausted GPUVM page-table arena");
            return false;
        }
        *page = nextPageTableAddress;
        nextPageTableAddress += PageBytes;
    }

    stats.record(ServiceCounter::PageTablePages);
    for (uint64_t offset = 0; offset < PageBytes; offset += sizeof(uint64_t)) {
        hardware.writeValue(*page + offset, 0, sizeof(uint64_t));
    }
    return true;
}

bool
GpuVmManager::ensurePageTableChild(uint64_t table, uint64_t index,
                                   uint64_t *child, std::string *error)
{
    if (!child || index >= 512) {
        setError(error, "invalid GPUVM page-table child request");
        return false;
    }
    const uint64_t entry = table + index * sizeof(uint64_t);
    const auto existing = pageTableChildren.find(entry);
    if (existing != pageTableChildren.end()) {
        *child = existing->second;
        return true;
    }
    if (!allocatePageTablePage(child, error)) {
        return false;
    }
    hardware.writeValue(entry, (*child & ~PageMask) | VegaPdeFlags,
                        sizeof(uint64_t));
    pageTableChildren.emplace(entry, *child);
    return true;
}

bool
GpuVmManager::ensurePageTableRoot(std::string *error)
{
    if (pageTableRootAddress) {
        return true;
    }
    if (!allocatePageTablePage(&pageTableRootAddress, error)) {
        return false;
    }
    hardware.configurePageTable(config.vmId, pageTableRootAddress);
    return true;
}

std::optional<uint64_t>
GpuVmManager::pteAddress(uint64_t page_virtual) const
{
    if (!pageTableRootAddress) {
        return std::nullopt;
    }
    const uint64_t logical = page_virtual >> 12;
    const uint64_t pde2 = (logical >> 27) & 0x1ffu;
    const uint64_t pde1 = (logical >> 18) & 0x1ffu;
    const uint64_t pde0 = (logical >> 9) & 0x1ffu;
    const uint64_t pte = logical & 0x1ffu;

    const auto pde1_page = pageTableChildren.find(
        pageTableRootAddress + pde2 * sizeof(uint64_t));
    if (pde1_page == pageTableChildren.end()) {
        return std::nullopt;
    }
    const auto pde0_page = pageTableChildren.find(
        pde1_page->second + pde1 * sizeof(uint64_t));
    if (pde0_page == pageTableChildren.end()) {
        return std::nullopt;
    }
    const auto pte_page = pageTableChildren.find(
        pde0_page->second + pde0 * sizeof(uint64_t));
    if (pte_page == pageTableChildren.end()) {
        return std::nullopt;
    }
    return pte_page->second + pte * sizeof(uint64_t);
}

void
GpuVmManager::writeMappingPte(uint64_t page_virtual, const Mapping &mapping)
{
    const auto pte = pteAddress(page_virtual);
    if (!pte) {
        return;
    }
    const uint64_t flags = mapping.kind == GpuVmMappingKind::HostControlDma
                               ? VegaSystemPteFlags
                               : VegaDevicePteFlags;
    hardware.writeValue(*pte, (mapping.physicalPage & ~PageMask) | flags,
                        sizeof(uint64_t));
}

void
GpuVmManager::invalidatePageTable()
{
    hardware.configurePageTable(config.vmId, pageTableRootAddress);
}

GpuVmManager::StateSnapshot
GpuVmManager::snapshot() const
{
    return {
        nextPageTableAddress,
        pageTableRootAddress,
        freePageTableAddresses,
        pageTableChildren,
        mappings,
    };
}

void
GpuVmManager::rollback(StateSnapshot state)
{
    for (const auto &[page, mapping] : mappings) {
        const auto old = state.mappings.find(page);
        if (old != state.mappings.end() && old->second == mapping) {
            continue;
        }
        if (old != state.mappings.end()) {
            writeMappingPte(page, old->second);
        } else if (const auto pte = pteAddress(page)) {
            hardware.writeValue(*pte, 0, sizeof(uint64_t));
        }
    }
    for (const auto &[entry, child] : pageTableChildren) {
        const auto old = state.pageTableChildren.find(entry);
        if (old == state.pageTableChildren.end()) {
            hardware.writeValue(entry, 0, sizeof(uint64_t));
        } else if (old->second != child) {
            hardware.writeValue(
                entry, (old->second & ~PageMask) | VegaPdeFlags,
                sizeof(uint64_t));
        }
    }

    nextPageTableAddress = state.nextPageTableAddress;
    pageTableRootAddress = state.pageTableRootAddress;
    freePageTableAddresses = std::move(state.freePageTableAddresses);
    pageTableChildren = std::move(state.pageTableChildren);
    mappings = std::move(state.mappings);
    invalidatePageTable();
}

bool
GpuVmManager::map(uint64_t virtual_address, uint64_t physical_address,
                  uint64_t size, GpuVmMappingKind kind, std::string *error)
{
    return mapOwned({}, virtual_address, physical_address, size, kind, error);
}

bool
GpuVmManager::mapOwned(GpuVmOwner owner, uint64_t virtual_address,
                       uint64_t physical_address, uint64_t size,
                       GpuVmMappingKind kind, std::string *error)
{
    if (!size) {
        setError(error, "GPUVM mapping range is empty");
        return false;
    }
    if ((virtual_address & PageMask) != (physical_address & PageMask)) {
        setError(error, "GPUVM virtual and physical page offsets differ");
        return false;
    }
    uint64_t range_end = 0;
    uint64_t end_virtual = 0;
    if (!rangeEnd(virtual_address, size, &range_end) ||
        !alignUp(range_end, &end_virtual)) {
        setError(error, "GPUVM mapping range overflows");
        return false;
    }

    uint64_t page_virtual = alignDown(virtual_address);
    uint64_t page_physical = alignDown(physical_address);
    while (page_virtual < end_virtual) {
        const auto existing = mappings.find(page_virtual);
        if (existing != mappings.end() &&
            existing->second.physicalPage != page_physical) {
            setError(error, "GPUVM mapping overlaps a different allocation");
            return false;
        }
        if (existing != mappings.end() && existing->second.owner != owner) {
            setError(error, "GPUVM mapping belongs to a different resource");
            return false;
        }
        page_virtual += PageBytes;
        page_physical += PageBytes;
    }

    StateSnapshot before = snapshot();
    if (!ensurePageTableRoot(error)) {
        rollback(std::move(before));
        return false;
    }

    page_virtual = alignDown(virtual_address);
    page_physical = alignDown(physical_address);
    bool changed_any = false;
    while (page_virtual < end_virtual) {
        auto existing = mappings.find(page_virtual);
        const bool changed = existing == mappings.end() ||
            existing->second.kind != kind ||
            existing->second.owner != owner;
        if (changed) {
            const uint64_t logical = page_virtual >> 12;
            const uint64_t pde2 = (logical >> 27) & 0x1ffu;
            const uint64_t pde1 = (logical >> 18) & 0x1ffu;
            const uint64_t pde0 = (logical >> 9) & 0x1ffu;
            const uint64_t pte = logical & 0x1ffu;
            uint64_t pde1_page = 0;
            uint64_t pde0_page = 0;
            uint64_t pte_page = 0;
            if (!ensurePageTableChild(
                    pageTableRootAddress, pde2, &pde1_page, error) ||
                !ensurePageTableChild(
                    pde1_page, pde1, &pde0_page, error) ||
                !ensurePageTableChild(
                    pde0_page, pde0, &pte_page, error)) {
                rollback(std::move(before));
                return false;
            }
            const Mapping mapping{page_physical, kind, owner};
            const uint64_t flags =
                kind == GpuVmMappingKind::HostControlDma
                    ? VegaSystemPteFlags
                    : VegaDevicePteFlags;
            hardware.writeValue(
                pte_page + pte * sizeof(uint64_t),
                (page_physical & ~PageMask) | flags, sizeof(uint64_t));
            mappings[page_virtual] = mapping;
            stats.record(ServiceCounter::MappedPages);
            changed_any = true;
        }
        page_virtual += PageBytes;
        page_physical += PageBytes;
    }
    if (changed_any) {
        invalidatePageTable();
    }
    return true;
}

bool
GpuVmManager::remapOwnedKind(GpuVmOwner owner, uint64_t virtual_address,
                             uint64_t size, GpuVmMappingKind kind,
                             std::string *error)
{
    if (!size) {
        setError(error, "GPUVM mapping-role range is empty");
        return false;
    }
    uint64_t range_end = 0;
    uint64_t end_virtual = 0;
    if (!rangeEnd(virtual_address, size, &range_end) ||
        !alignUp(range_end, &end_virtual)) {
        setError(error, "GPUVM mapping-role range overflows");
        return false;
    }

    for (uint64_t page = alignDown(virtual_address); page < end_virtual;
         page += PageBytes) {
        const auto mapping = mappings.find(page);
        if (mapping == mappings.end()) {
            setError(error, "GPUVM mapping-role range is not resident");
            return false;
        }
        if (mapping->second.owner != owner) {
            setError(error, "GPUVM mapping-role range has a different owner");
            return false;
        }
    }

    bool changed = false;
    for (uint64_t page = alignDown(virtual_address); page < end_virtual;
         page += PageBytes) {
        auto &mapping = mappings.at(page);
        if (mapping.kind == kind) {
            continue;
        }
        mapping.kind = kind;
        writeMappingPte(page, mapping);
        changed = true;
    }
    if (changed) {
        invalidatePageTable();
    }
    return true;
}

void
GpuVmManager::reclaimEmptyPageTables(uint64_t page_virtual)
{
    if (!pageTableRootAddress) {
        return;
    }
    const uint64_t logical = page_virtual >> 12;
    const uint64_t pde2 = (logical >> 27) & 0x1ffu;
    const uint64_t pde1 = (logical >> 18) & 0x1ffu;
    const uint64_t pde0 = (logical >> 9) & 0x1ffu;
    const uint64_t pde1_entry =
        pageTableRootAddress + pde2 * sizeof(uint64_t);
    const auto pde1_child = pageTableChildren.find(pde1_entry);
    if (pde1_child == pageTableChildren.end()) {
        return;
    }
    const uint64_t pde1_page = pde1_child->second;
    const uint64_t pde0_entry = pde1_page + pde1 * sizeof(uint64_t);
    const auto pde0_child = pageTableChildren.find(pde0_entry);
    if (pde0_child == pageTableChildren.end()) {
        return;
    }
    const uint64_t pde0_page = pde0_child->second;
    const uint64_t pte_entry = pde0_page + pde0 * sizeof(uint64_t);
    const auto pte_child = pageTableChildren.find(pte_entry);
    if (pte_child == pageTableChildren.end()) {
        return;
    }
    const uint64_t pte_page = pte_child->second;

    const bool pte_in_use = std::any_of(
        mappings.begin(), mappings.end(), [&](const auto &mapping) {
            const auto address = pteAddress(mapping.first);
            return address && *address >= pte_page &&
                *address < pte_page + PageBytes;
        });
    if (pte_in_use) {
        return;
    }
    hardware.writeValue(pte_entry, 0, sizeof(uint64_t));
    pageTableChildren.erase(pte_entry);
    freePageTableAddresses.push_back(pte_page);

    const auto has_children = [&](uint64_t table) {
        return std::any_of(
            pageTableChildren.begin(), pageTableChildren.end(),
            [&](const auto &child) {
                return child.first >= table &&
                    child.first < table + PageBytes;
            });
    };
    if (has_children(pde0_page)) {
        return;
    }
    hardware.writeValue(pde0_entry, 0, sizeof(uint64_t));
    pageTableChildren.erase(pde0_entry);
    freePageTableAddresses.push_back(pde0_page);

    if (has_children(pde1_page)) {
        return;
    }
    hardware.writeValue(pde1_entry, 0, sizeof(uint64_t));
    pageTableChildren.erase(pde1_entry);
    freePageTableAddresses.push_back(pde1_page);
}

bool
GpuVmManager::releaseOwner(GpuVmOwner owner, std::string *error)
{
    if (owner.kind == GpuVmResourceKind::Service || !owner.id) {
        setError(error, "cannot release the service GPUVM owner");
        return false;
    }

    std::vector<uint64_t> released_pages;
    for (auto mapping = mappings.begin(); mapping != mappings.end();) {
        if (mapping->second.owner != owner) {
            ++mapping;
            continue;
        }
        if (const auto pte = pteAddress(mapping->first)) {
            hardware.writeValue(*pte, 0, sizeof(uint64_t));
        }
        released_pages.push_back(mapping->first);
        mapping = mappings.erase(mapping);
    }
    for (const uint64_t page : released_pages) {
        reclaimEmptyPageTables(page);
    }
    if (!released_pages.empty()) {
        invalidatePageTable();
    }
    return true;
}

bool
GpuVmManager::releaseAllocation(uint64_t handle, std::string *error)
{
    return releaseOwner(GpuVmOwner::allocation(handle), error);
}

size_t
GpuVmManager::ownedMappingCount(GpuVmOwner owner) const
{
    return std::count_if(
        mappings.begin(), mappings.end(),
        [&](const auto &mapping) { return mapping.second.owner == owner; });
}

std::optional<GpuVmTranslation>
GpuVmManager::translate(uint64_t address) const
{
    const uint64_t page = alignDown(address);
    const auto mapping = mappings.find(page);
    if (mapping == mappings.end()) {
        return std::nullopt;
    }
    return GpuVmTranslation{
        page,
        mapping->second.physicalPage,
        mapping->second.physicalPage + (address & PageMask),
        mapping->second.kind,
        mapping->second.owner,
    };
}

} // namespace gem5_hsakmt
