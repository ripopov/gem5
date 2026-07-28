#include "dev/hsakmt-service/service/gpu_vm_manager.hh"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

using namespace gem5_hsakmt;

struct FakeHardware
{
    void
    writeValue(uint64_t address, uint64_t value, unsigned size)
    {
        std::vector<uint8_t> bytes(size);
        std::memcpy(bytes.data(), &value, size);
        writes[address] = std::move(bytes);
    }

    void
    configure(uint16_t configured_vm_id, uint64_t root)
    {
        vmId = configured_vm_id;
        pageTableRoot = root;
        ++invalidations;
    }

    std::unordered_map<uint64_t, std::vector<uint8_t>> writes;
    uint16_t vmId = 0;
    uint64_t pageTableRoot = 0;
    uint64_t invalidations = 0;
};

GpuVmManagerConfig
config(uint64_t page_table_pages = 8)
{
    GpuVmManagerConfig value;
    value.pageTableOffset = 0x800000;
    value.pageTableBytes = page_table_pages * 4096;
    value.vmId = 3;
    return value;
}

GpuVmHardware
callbacks(FakeHardware &hardware)
{
    return {
        [&](uint64_t address, uint64_t value, unsigned size) {
            hardware.writeValue(address, value, size);
        },
        [&](uint16_t vm_id, uint64_t root) {
            hardware.configure(vm_id, root);
        },
    };
}

class GpuVmManagerTest : public testing::Test
{
  protected:
    FakeHardware hardware;
    ServiceStats stats;
    GpuVmManager manager{config(), callbacks(hardware), stats};
    std::string error;
};

TEST_F(GpuVmManagerTest, MapsExactSharedVramRange)
{
    const auto owner = GpuVmOwner::allocation(7);
    ASSERT_TRUE(manager.mapOwned(
        owner, 0x100001123, 0x400123, 5000,
        GpuVmMappingKind::DeviceLocal, &error))
        << error;

    const auto first = manager.translate(0x100001123);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->physicalAddress, 0x400123);
    EXPECT_EQ(first->owner, owner);
    EXPECT_EQ(first->kind, GpuVmMappingKind::DeviceLocal);

    const auto last = manager.translate(0x1000024aa);
    ASSERT_TRUE(last);
    EXPECT_EQ(last->physicalAddress, 0x4014aa);
    EXPECT_EQ(manager.mappingCount(), 2);
    EXPECT_EQ(manager.ownedMappingCount(owner), 2);
    EXPECT_EQ(hardware.vmId, 3);
    EXPECT_EQ(hardware.pageTableRoot, 0x800000);
    EXPECT_GE(hardware.invalidations, 2);
    EXPECT_EQ(stats.snapshot().counter(ServiceCounter::MappedPages), 2);
}

TEST_F(GpuVmManagerTest, RejectsOffsetOverlapAndOwnerConflicts)
{
    const auto first = GpuVmOwner::allocation(1);
    const auto second = GpuVmOwner::allocation(2);
    EXPECT_FALSE(manager.mapOwned(
        first, 0x100001123, 0x400456, 4096,
        GpuVmMappingKind::DeviceLocal, &error));
    EXPECT_EQ(error, "GPUVM virtual and physical page offsets differ");

    ASSERT_TRUE(manager.mapOwned(
        first, 0x100000000, 0x400000, 4096,
        GpuVmMappingKind::DeviceLocal, &error))
        << error;
    EXPECT_FALSE(manager.mapOwned(
        first, 0x100000000, 0x500000, 4096,
        GpuVmMappingKind::DeviceLocal, &error));
    EXPECT_NE(error.find("overlaps"), std::string::npos);
    EXPECT_FALSE(manager.mapOwned(
        second, 0x100000000, 0x400000, 4096,
        GpuVmMappingKind::DeviceLocal, &error));
    EXPECT_NE(error.find("different resource"), std::string::npos);
}

TEST_F(GpuVmManagerTest, ChangesPteKindOnlyForItsOwner)
{
    const auto owner = GpuVmOwner::allocation(3);
    ASSERT_TRUE(manager.mapOwned(
        owner, 0x200000000, 0x600000, 8192,
        GpuVmMappingKind::DeviceLocal, &error))
        << error;
    ASSERT_TRUE(manager.remapOwnedKind(
        owner, 0x200000008, sizeof(uint64_t),
        GpuVmMappingKind::HostControlDma, &error))
        << error;
    ASSERT_TRUE(manager.translate(0x200000000));
    EXPECT_EQ(manager.translate(0x200000000)->kind,
              GpuVmMappingKind::HostControlDma);

    EXPECT_FALSE(manager.remapOwnedKind(
        GpuVmOwner::allocation(4), 0x200000000, 4096,
        GpuVmMappingKind::DeviceLocal, &error));
    EXPECT_NE(error.find("different owner"), std::string::npos);
}

TEST_F(GpuVmManagerTest, ReleasesMappingsAndRecyclesPageTables)
{
    for (uint64_t id = 1; id <= 32; ++id) {
        const uint64_t address = id << 39;
        ASSERT_TRUE(manager.mapOwned(
            GpuVmOwner::allocation(id), address, 0x400000, 4096,
            GpuVmMappingKind::DeviceLocal, &error))
            << "cycle " << id << ": " << error;
        ASSERT_TRUE(manager.releaseAllocation(id, &error))
            << "cycle " << id << ": " << error;
        EXPECT_FALSE(manager.translate(address));
        EXPECT_EQ(manager.mappingCount(), 0);
    }
}

TEST(GpuVmManagerStandaloneTest, ExhaustionLeavesNoMapping)
{
    FakeHardware hardware;
    ServiceStats stats;
    GpuVmManager manager(config(3), callbacks(hardware), stats);
    std::string error;

    EXPECT_FALSE(manager.mapOwned(
        GpuVmOwner::allocation(1), 0x300000000, 0x600000, 4096,
        GpuVmMappingKind::DeviceLocal, &error));
    EXPECT_NE(error.find("exhausted"), std::string::npos);
    EXPECT_EQ(manager.mappingCount(), 0);
    EXPECT_EQ(manager.pageTableRoot(), 0);
}

} // anonymous namespace
