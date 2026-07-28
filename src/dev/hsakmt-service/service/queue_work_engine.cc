#include "dev/hsakmt-service/service/queue_work_engine.hh"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#ifndef GEM5_PACKED
#define GEM5_PACKED [[gnu::packed]]
#endif

#include "dev/amdgpu/pm4_defines.hh"
#include "dev/amdgpu/pm4_mmio.hh"
#include "dev/amdgpu/pm4_queues.hh"

namespace gem5_hsakmt
{
namespace
{

constexpr uint64_t PageBytes = 4096;
constexpr uint64_t PageMask = PageBytes - 1;
constexpr uint32_t MaxQueues = 64;
constexpr uint16_t GpuVmId = 1;
constexpr uint64_t ControlRingBytes = PageBytes;
constexpr uint64_t ControlDoorbellStride = 0x100;
constexpr uint32_t ControlQueueSize = 9;
constexpr uint64_t QueueDescOffsetDwords = 96;
constexpr uint64_t QueueDescOffsetBytes =
    QueueDescOffsetDwords * sizeof(uint32_t);
constexpr uint32_t MaxDoorbellDwordOffset = (1u << 26) - 1;
constexpr uint32_t VegaAgpTopRegister = 0x0982;
constexpr uint32_t VegaAgpBottomRegister = 0x0983;
constexpr uint32_t VegaAgpBaseRegister = 0x0984;
constexpr uint32_t Mi300AgpTopRegister = 0x095e;
constexpr uint32_t Mi300AgpBottomRegister = 0x095f;
constexpr uint32_t Mi300AgpBaseRegister = 0x0960;

bool
isPowerOfTwo(uint64_t value)
{
    return value && ((value & (value - 1)) == 0);
}

uint32_t
log2PowerOfTwo(uint64_t value)
{
    uint32_t result = 0;
    while (value > 1) {
        value >>= 1;
        ++result;
    }
    return result;
}

} // anonymous namespace

QueueWorkEngine::QueueWorkEngine(QueueWorkEngineConfig config,
                                 QueueWorkHardware hardware,
                                 ServiceStats &stats)
    : config(std::move(config)), hardware(std::move(hardware)), stats(stats)
{
    if (!this->hardware.writeValue || !this->hardware.writeBlob ||
        !this->hardware.writeDoorbell || !this->hardware.writeMmio ||
        !this->hardware.pm4MmioBase || !this->hardware.vmMmioBase ||
        !this->hardware.usesMi300AgpRegisters ||
        !this->hardware.usesMapProcessV2) {
        throw std::invalid_argument(
            "QueueWorkEngine requires complete hardware callbacks");
    }
    if ((this->config.setupMemoryBase & 0xffffff) ||
        !this->config.interruptRingBytes) {
        throw std::invalid_argument(
            "QueueWorkEngine has invalid control-memory configuration");
    }
}

uint64_t
QueueWorkEngine::interruptPm4RingOffset() const
{
    const uint64_t interrupt_end =
        config.interruptRingOffset + config.interruptRingBytes;
    const uint64_t after_wptr = config.interruptWptrOffset + sizeof(uint32_t);
    return (std::max(interrupt_end, after_wptr) + PageMask) & ~PageMask;
}

uint64_t
QueueWorkEngine::interruptPm4RingVaddr() const
{
    return config.setupMemoryBase + interruptPm4RingOffset();
}

uint64_t
QueueWorkEngine::interruptPm4ReleaseMemOffset() const
{
    return interruptPm4RingOffset() + ControlRingBytes;
}

uint64_t
QueueWorkEngine::queueMqdFullOffset(uint32_t slot) const
{
    if (slot >= MaxQueues) {
        throw std::out_of_range("queue slot exceeds the MQD arena");
    }
    return interruptPm4ReleaseMemOffset() + PageBytes +
           static_cast<uint64_t>(slot) * PageBytes;
}

uint64_t
QueueWorkEngine::queueMqdFullVaddr(uint32_t slot) const
{
    return config.setupMemoryBase + queueMqdFullOffset(slot);
}

uint64_t
QueueWorkEngine::queueMqdDescOffset(uint32_t slot) const
{
    return queueMqdFullOffset(slot) + QueueDescOffsetBytes;
}

uint64_t
QueueWorkEngine::queueMqdRegionEndOffset() const
{
    return interruptPm4ReleaseMemOffset() + PageBytes +
           static_cast<uint64_t>(MaxQueues) * PageBytes;
}

uint64_t
QueueWorkEngine::interruptPm4DoorbellOffset() const
{
    return config.interruptDoorbellOffset + ControlDoorbellStride;
}

uint64_t
QueueWorkEngine::pm4MmioOffset(uint32_t reg) const
{
    return hardware.pm4MmioBase() +
           static_cast<uint64_t>(reg) * sizeof(uint32_t);
}

void
QueueWorkEngine::writePm4Mmio(uint32_t reg, uint32_t value)
{
    hardware.writeMmio(pm4MmioOffset(reg), value);
}

bool
QueueWorkEngine::configurePm4Aperture(std::string *error)
{
    if (config.setupMemoryBase & 0xffffff) {
        return reject(error, "control memory base is not 16MiB aligned");
    }
    const bool mi300 = hardware.usesMi300AgpRegisters();
    const uint32_t base_reg =
        mi300 ? Mi300AgpBaseRegister : VegaAgpBaseRegister;
    const uint32_t bot_reg =
        mi300 ? Mi300AgpBottomRegister : VegaAgpBottomRegister;
    const uint32_t top_reg = mi300 ? Mi300AgpTopRegister : VegaAgpTopRegister;
    const uint64_t mmio_base = hardware.vmMmioBase();
    const uint64_t agp_top =
        config.setupMemoryBase + queueMqdRegionEndOffset() - 1;
    hardware.writeMmio(mmio_base + base_reg * sizeof(uint32_t), 0);
    hardware.writeMmio(mmio_base + bot_reg * sizeof(uint32_t),
                       static_cast<uint32_t>(config.setupMemoryBase >> 24));
    hardware.writeMmio(mmio_base + top_reg * sizeof(uint32_t),
                       static_cast<uint32_t>(agp_top >> 24));
    return true;
}

bool
QueueWorkEngine::configurePm4ControlQueue(std::string *error)
{
    if (pm4ControlQueueConfigured) {
        return true;
    }
    if ((interruptPm4RingOffset() & 0xff) ||
        (interruptPm4DoorbellOffset() & (sizeof(uint64_t) - 1))) {
        return reject(error, "private PM4 control queue is misaligned");
    }

    const std::vector<uint8_t> zero(ControlRingBytes, 0);
    hardware.writeBlob(interruptPm4RingOffset(), zero.data(), zero.size());
    hardware.writeValue(interruptPm4ReleaseMemOffset(), 0, sizeof(uint32_t));
    if (!configurePm4Aperture(error)) {
        return false;
    }

    const uint64_t queue_base = interruptPm4RingVaddr() >> 8;
    writePm4Mmio(mmCP_HQD_ACTIVE, 0);
    writePm4Mmio(mmCP_HQD_VMID, 0);
    writePm4Mmio(mmCP_HQD_PQ_BASE, static_cast<uint32_t>(queue_base));
    writePm4Mmio(mmCP_HQD_PQ_BASE_HI, static_cast<uint32_t>(queue_base >> 32));
    writePm4Mmio(mmCP_HQD_PQ_RPTR, 0);
    writePm4Mmio(mmCP_HQD_PQ_CONTROL, ControlQueueSize);
    writePm4Mmio(mmCP_HQD_PQ_DOORBELL_CONTROL,
                 static_cast<uint32_t>(interruptPm4DoorbellOffset()));
    writePm4Mmio(mmCP_HQD_ACTIVE, 1);
    pm4ControlWptrDwords = 0;
    pm4ControlReclaimDwords = 0;
    pm4ControlQueueConfigured = true;
    return true;
}

bool
QueueWorkEngine::configureDirectProcess(uint16_t pasid,
                                        uint64_t page_table_root,
                                        uint64_t lds_base,
                                        uint64_t scratch_base,
                                        std::string *error)
{
    if (!pasid || !page_table_root || (page_table_root & PageMask) ||
        (lds_base & 0xffffffffffffULL) ||
        (scratch_base & 0xffffffffffffULL)) {
        return reject(error, "direct process metadata is invalid");
    }

    const uint32_t sh_mem_bases =
        (static_cast<uint32_t>(lds_base >> 48) << 16) |
        static_cast<uint32_t>(scratch_base >> 48);
    if (hardware.usesMapProcessV2()) {
        gem5::PM4MapProcessV2 map = {};
        map.pasid = pasid;
        map.processQuantum = 1;
        map.ptBase = page_table_root;
        map.shMemBases = sh_mem_bases;
        map.numQueues = MaxQueues;
        map.sdma_enable = 1;
        return submitPm4ControlPacket(
            gem5::IT_MAP_PROCESS, &map, sizeof(map), error);
    }

    gem5::PM4MapProcess map = {};
    map.pasid = pasid;
    map.processQuantum = 1;
    map.ptBase = page_table_root;
    map.shMemBases = sh_mem_bases;
    map.numQueues = MaxQueues;
    return submitPm4ControlPacket(
        gem5::IT_MAP_PROCESS, &map, sizeof(map), error);
}

bool
QueueWorkEngine::unconfigureDirectProcess(
    uint16_t pasid, std::string *error)
{
    if (!pasid) {
        return reject(error, "direct process PASID is invalid");
    }
    gem5::PM4UnmapQueues unmap = {};
    unmap.action = 0;
    unmap.queueSel = 1;
    unmap.pasid = pasid;
    return submitPm4ControlPacket(
        gem5::IT_UNMAP_QUEUES, &unmap, sizeof(unmap), error);
}

bool
QueueWorkEngine::submitDirectQueueMap(const DirectHardwareQueue &queue,
                                      uint32_t slot, std::string *error)
{
    if (!queue.queueId || !queue.ringBaseVa || !queue.ringBytes ||
        !isPowerOfTwo(queue.ringBytes) || (queue.ringBaseVa & 0xff) ||
        (queue.doorbellOffset & 3) ||
        (queue.doorbellOffset >> 2) > MaxDoorbellDwordOffset) {
        return reject(error, "direct queue metadata is invalid");
    }

    const uint64_t ring_dwords = queue.ringBytes / sizeof(uint32_t);
    if (!isPowerOfTwo(ring_dwords) || ring_dwords < 2) {
        return reject(error, "direct queue ring has invalid dword count");
    }

    gem5::PM4MapQueues map = {};
    map.queueSel = 1;
    map.vmid = GpuVmId;
    map.queueSlot = slot & 0x7;
    map.pipe = (slot >> 3) & 0x3;
    map.me = (slot >> 5) & 0x1;
    map.numQueues = 1;
    map.checkDisable = 1;
    map.doorbellOffset = static_cast<uint32_t>(queue.doorbellOffset >> 2);
    map.mqdAddr = queueMqdFullVaddr(slot);

    if (queue.isSdma) {
        gem5::SDMAQueueDesc mqd = {};
        mqd.sdmax_rlcx_rb_cntl =
            static_cast<uint32_t>(log2PowerOfTwo(ring_dwords) << 1);
        mqd.rb_base = queue.ringBaseVa >> 8;
        mqd.sdmax_rlcx_rb_rptr_addr_hi =
            static_cast<uint32_t>(queue.readPointerVa >> 32);
        mqd.sdmax_rlcx_rb_rptr_addr_lo =
            static_cast<uint32_t>(queue.readPointerVa);
        mqd.sdmax_rlcx_rb_wptr_poll_addr_hi =
            static_cast<uint32_t>(queue.writePointerVa >> 32);
        mqd.sdmax_rlcx_rb_wptr_poll_addr_lo =
            static_cast<uint32_t>(queue.writePointerVa);
        mqd.sdmax_rlcx_doorbell_offset =
            static_cast<uint32_t>(queue.doorbellOffset >> 2);
        hardware.writeBlob(queueMqdFullOffset(slot), &mqd, sizeof(mqd));
        map.engineSel = 2;
    } else {
        gem5::QueueDesc mqd = {};
        mqd.mqdBase = queueMqdFullVaddr(slot);
        mqd.hqd_active = 1;
        mqd.hqd_vmid = GpuVmId;
        mqd.base = queue.ringBaseVa >> 8;
        mqd.aqlRptr = queue.readPointerVa;
        mqd.hqd_pq_wptr_poll_addr_lo =
            static_cast<uint32_t>(queue.writePointerVa);
        mqd.hqd_pq_wptr_poll_addr_hi =
            static_cast<uint32_t>(queue.writePointerVa >> 32);
        mqd.doorbell = static_cast<uint32_t>(queue.doorbellOffset);
        mqd.hqd_pq_control = log2PowerOfTwo(ring_dwords) - 1;
        mqd.aql = 1;
        hardware.writeBlob(queueMqdDescOffset(slot), &mqd, sizeof(mqd));
        map.engineSel = 1;
    }

    return submitPm4ControlPacket(gem5::IT_MAP_QUEUES, &map, sizeof(map),
                                  error);
}

bool
QueueWorkEngine::registerDirectQueue(const DirectHardwareQueue &queue,
                                     std::string *error)
{
    const auto existing = directQueues.find(queue.queueId);
    if (existing != directQueues.end()) {
        return existing->second.doorbellOffset == queue.doorbellOffset ||
            reject(error, "direct queue changed its doorbell");
    }

    uint32_t slot = 0;
    if (!chooseServerQueueSlot(queue.queueId, &slot, error) ||
        !submitDirectQueueMap(queue, slot, error)) {
        return false;
    }
    registeredQueueSlots.emplace(queue.queueId, slot);
    directQueues.emplace(queue.queueId, queue);
    stats.record(ServiceCounter::RegisteredQueues);
    return true;
}

bool
QueueWorkEngine::updateDirectQueue(uint64_t queue_id, uint64_t ring_base,
                                   uint64_t ring_size, std::string *error)
{
    auto queue = directQueues.find(queue_id);
    if (queue == directQueues.end()) {
        return reject(error, "cannot update an unknown direct queue");
    }
    DirectHardwareQueue replacement = queue->second;
    replacement.ringBaseVa = ring_base;
    replacement.ringBytes = ring_size;
    if (!submitQueueUnmap(
            replacement.doorbellOffset, replacement.isSdma, error)) {
        return false;
    }
    const auto slot = registeredQueueSlots.find(queue_id);
    if (slot == registeredQueueSlots.end() ||
        !submitDirectQueueMap(replacement, slot->second, error)) {
        return false;
    }
    queue->second = replacement;
    return true;
}

bool
QueueWorkEngine::unregisterDirectQueue(uint64_t queue_id, std::string *error)
{
    const auto queue = directQueues.find(queue_id);
    if (queue == directQueues.end()) {
        return true;
    }
    if (!submitQueueUnmap(
            queue->second.doorbellOffset, queue->second.isSdma, error)) {
        return false;
    }
    const auto slot = registeredQueueSlots.find(queue_id);
    if (slot != registeredQueueSlots.end()) {
        const std::vector<uint8_t> zero(PageBytes, 0);
        hardware.writeBlob(queueMqdFullOffset(slot->second), zero.data(),
                           zero.size());
        registeredQueueSlots.erase(slot);
    }
    directQueues.erase(queue);
    return true;
}

bool
QueueWorkEngine::submitQueueUnmap(
    uint64_t doorbell_offset, bool is_sdma, std::string *error)
{
    if (doorbell_offset & (sizeof(uint32_t) - 1) ||
        (doorbell_offset >> 2) > MaxDoorbellDwordOffset) {
        return reject(error, "registered queue has an invalid doorbell");
    }
    gem5::PM4UnmapQueues unmap = {};
    unmap.action = 0;
    unmap.queueSel = 0;
    unmap.engineSel = is_sdma ? 2 : 1;
    // gem5 selects a single queue from offset3 when numQueues is four.
    unmap.numQueues = 4;
    unmap.doorbellOffset3 = static_cast<uint32_t>(doorbell_offset >> 2);
    return submitPm4ControlPacket(gem5::IT_UNMAP_QUEUES, &unmap, sizeof(unmap),
                                  error);
}

bool
QueueWorkEngine::submitPm4ControlPacket(uint32_t opcode, const void *payload,
                                        uint64_t payload_size,
                                        std::string *error)
{
    if (!configurePm4ControlQueue(error)) {
        return false;
    }
    if (!payload || !payload_size || (payload_size & (sizeof(uint32_t) - 1))) {
        return reject(error,
                      "PM4 control payload is not a nonzero dword sequence");
    }
    const uint64_t payload_dwords = payload_size / sizeof(uint32_t);
    if (payload_dwords > 0x4000) {
        return reject(error, "PM4 control payload has too many dwords");
    }
    const uint32_t packet_dwords = static_cast<uint32_t>(
        sizeof(gem5::PM4Header) / sizeof(uint32_t) + payload_dwords);
    const uint32_t ring_dwords = ControlRingBytes / sizeof(uint32_t);
    if (packet_dwords > ring_dwords) {
        return reject(error, "PM4 control packet exceeds its private ring");
    }

    const uint32_t position =
        static_cast<uint32_t>(pm4ControlWptrDwords % ring_dwords);
    const uint32_t remaining = ring_dwords - position;
    const bool wrap =
        remaining < packet_dwords || remaining - packet_dwords == 1;
    const uint64_t reservation = packet_dwords + (wrap ? remaining : 0);
    if (controlReservedDwords() > ring_dwords ||
        reservation > ring_dwords - controlReservedDwords()) {
        return reject(error, "private PM4 control ring is still in use");
    }
    if (wrap) {
        if (remaining < 2) {
            return reject(error,
                          "PM4 control ring reached an invalid wrap point");
        }
        writePm4RingPadding(remaining);
        pm4ControlWptrDwords += remaining;
    }

    gem5::PM4Header header = {};
    header.type = 3;
    header.opcode = opcode;
    header.count = static_cast<uint16_t>(payload_dwords - 1);
    const uint64_t offset =
        interruptPm4RingOffset() +
        (pm4ControlWptrDwords % ring_dwords) * sizeof(uint32_t);
    hardware.writeBlob(offset, &header, sizeof(header));
    hardware.writeBlob(offset + sizeof(header), payload, payload_size);
    pm4ControlWptrDwords += packet_dwords;
    hardware.writeDoorbell(interruptPm4DoorbellOffset(), pm4ControlWptrDwords);
    return true;
}

uint64_t
QueueWorkEngine::controlWriteSequence() const
{
    return pm4ControlWptrDwords;
}

void
QueueWorkEngine::releaseControlThrough(uint64_t sequence)
{
    pm4ControlReclaimDwords = std::max(
        pm4ControlReclaimDwords, std::min(sequence, pm4ControlWptrDwords));
}

uint64_t
QueueWorkEngine::controlReservedDwords() const
{
    return pm4ControlWptrDwords - pm4ControlReclaimDwords;
}

uint64_t
QueueWorkEngine::registeredQueueCount() const
{
    return directQueues.size();
}

void
QueueWorkEngine::writePm4RingPadding(uint32_t dwords)
{
    gem5::PM4Header header = {};
    header.type = 3;
    header.opcode = gem5::IT_NOP;
    header.count = static_cast<uint16_t>(dwords - 2);
    const uint32_t ring_dwords = ControlRingBytes / sizeof(uint32_t);
    const uint64_t offset =
        interruptPm4RingOffset() +
        (pm4ControlWptrDwords % ring_dwords) * sizeof(uint32_t);
    hardware.writeBlob(offset, &header, sizeof(header));
    if (dwords > 1) {
        const std::vector<uint8_t> zero((dwords - 1) * sizeof(uint32_t), 0);
        hardware.writeBlob(offset + sizeof(header), zero.data(), zero.size());
    }
}

bool
QueueWorkEngine::chooseServerQueueSlot(uint64_t queue_id, uint32_t *slot,
                                       std::string *error) const
{
    if (!slot) {
        return reject(error, "server queue slot output is null");
    }
    const auto existing = registeredQueueSlots.find(queue_id);
    if (existing != registeredQueueSlots.end()) {
        *slot = existing->second;
        return true;
    }
    for (uint32_t candidate = 0; candidate < MaxQueues; ++candidate) {
        const bool occupied = std::any_of(
            registeredQueueSlots.begin(), registeredQueueSlots.end(),
            [&](const auto &registration) {
                return registration.second == candidate;
            });
        if (!occupied) {
            *slot = candidate;
            return true;
        }
    }
    return reject(error, "rocjitsu backend exhausted hardware queue slots");
}

bool
QueueWorkEngine::reject(std::string *error, const std::string &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

} // namespace gem5_hsakmt
