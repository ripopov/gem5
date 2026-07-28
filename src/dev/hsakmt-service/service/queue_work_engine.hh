#ifndef __DEV_HSAKMT_SERVICE_QUEUE_WORK_ENGINE_HH__
#define __DEV_HSAKMT_SERVICE_QUEUE_WORK_ENGINE_HH__

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "dev/hsakmt-service/service/service_stats.hh"

namespace gem5_hsakmt
{

struct QueueWorkEngineConfig
{
    uint64_t setupMemoryBase = 0;
    uint64_t interruptRingOffset = 0;
    uint64_t interruptRingBytes = 0;
    uint64_t interruptWptrOffset = 0;
    uint64_t interruptDoorbellOffset = 0;
};

struct QueueWorkHardware
{
    std::function<void(uint64_t, uint64_t, unsigned)> writeValue;
    std::function<void(uint64_t, const void *, uint64_t)> writeBlob;
    std::function<void(uint64_t, uint64_t)> writeDoorbell;
    std::function<void(uint64_t, uint32_t)> writeMmio;
    std::function<uint64_t()> pm4MmioBase;
    std::function<uint64_t()> vmMmioBase;
    std::function<bool()> usesMi300AgpRegisters;
    std::function<bool()> usesMapProcessV2;
};

/** KFD queue metadata translated into hardware MQD and PM4 packets. */
struct DirectHardwareQueue
{
    uint64_t queueId = 0;
    uint64_t ringBaseVa = 0;
    uint64_t ringBytes = 0;
    uint64_t readPointerVa = 0;
    uint64_t writePointerVa = 0;
    uint64_t queueDescriptorVa = 0;
    uint64_t doorbellOffset = 0;
    bool isSdma = false;
};

/**
 * Installs process and queue state through the modeled GPU's PM4 interface.
 *
 * The engine owns a private PM4 control ring and service-owned MQD storage.
 * It does not stage application packets or reconstruct trace semantics:
 * rocjitsu owns KFD policy and application queues remain in shared VRAM.
 */
class QueueWorkEngine
{
  public:
    QueueWorkEngine(QueueWorkEngineConfig config, QueueWorkHardware hardware,
                    ServiceStats &stats);

    bool configurePm4ControlQueue(std::string *error = nullptr);
    bool configureDirectProcess(uint16_t pasid, uint64_t page_table_root,
                                uint64_t lds_base, uint64_t scratch_base,
                                std::string *error = nullptr);
    bool unconfigureDirectProcess(uint16_t pasid,
                                  std::string *error = nullptr);
    bool registerDirectQueue(const DirectHardwareQueue &queue,
                             std::string *error = nullptr);
    bool updateDirectQueue(uint64_t queue_id, uint64_t ring_base,
                           uint64_t ring_size,
                           std::string *error = nullptr);
    bool unregisterDirectQueue(uint64_t queue_id,
                               std::string *error = nullptr);

    bool submitPm4ControlPacket(uint32_t opcode, const void *payload,
                                uint64_t payload_size,
                                std::string *error = nullptr);
    uint64_t controlWriteSequence() const;
    void releaseControlThrough(uint64_t sequence);
    uint64_t controlReservedDwords() const;
    uint64_t registeredQueueCount() const;

    uint64_t interruptPm4RingOffset() const;
    uint64_t interruptPm4RingVaddr() const;
    uint64_t interruptPm4ReleaseMemOffset() const;
    uint64_t interruptPm4DoorbellOffset() const;

  private:
    bool chooseServerQueueSlot(uint64_t queue_id, uint32_t *slot,
                               std::string *error) const;
    bool configurePm4Aperture(std::string *error);
    bool submitQueueUnmap(uint64_t doorbell_offset, bool is_sdma,
                          std::string *error);
    bool submitDirectQueueMap(const DirectHardwareQueue &queue, uint32_t slot,
                              std::string *error);
    void writePm4RingPadding(uint32_t dwords);
    uint64_t queueMqdFullOffset(uint32_t server_queue_slot) const;
    uint64_t queueMqdFullVaddr(uint32_t server_queue_slot) const;
    uint64_t queueMqdDescOffset(uint32_t server_queue_slot) const;
    uint64_t queueMqdRegionEndOffset() const;
    uint64_t pm4MmioOffset(uint32_t reg) const;
    void writePm4Mmio(uint32_t reg, uint32_t value);
    static bool reject(std::string *error, const std::string &message);

    const QueueWorkEngineConfig config;
    const QueueWorkHardware hardware;
    ServiceStats &stats;

    bool pm4ControlQueueConfigured = false;
    uint64_t pm4ControlWptrDwords = 0;
    uint64_t pm4ControlReclaimDwords = 0;
    std::unordered_map<uint64_t, uint32_t> registeredQueueSlots;
    std::unordered_map<uint64_t, DirectHardwareQueue> directQueues;
};

} // namespace gem5_hsakmt

#endif // __DEV_HSAKMT_SERVICE_QUEUE_WORK_ENGINE_HH__
