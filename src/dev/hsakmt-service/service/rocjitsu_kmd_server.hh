#ifndef __DEV_HSAKMT_SERVICE_ROCJITSU_KMD_SERVER_HH__
#define __DEV_HSAKMT_SERVICE_ROCJITSU_KMD_SERVER_HH__

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rocjitsu/kmd/server.h>

#include "base/types.hh"
#include "dev/hsakmt-service/service/doorbell_bridge.hh"
#include "dev/hsakmt-service/service/gem5_device_backend.hh"
#include "dev/hsakmt-service/service/gpu_vm_manager.hh"
#include "dev/hsakmt-service/service/queue_work_engine.hh"
#include "dev/hsakmt-service/service/service_stats.hh"
#include "dev/hsakmt-service/service/shared_region_allocator.hh"
#include "params/RocjitsuKmdServer.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class AMDGPUDevice;
class GPUCommandProcessor;
class HSAPacketProcessor;
class PM4PacketProcessor;
class SDMAEngine;
class Shader;

namespace hsa
{

class HsakmtServerPlatform;

/**
 * Hosts rocjitsu's KMD daemon reactor on gem5's event queue.
 *
 * All application memory remains in exported gem5 VRAM. The SimObject owns
 * physical extents, GPUVM mappings, hardware queue installation, doorbell
 * MMIO, and IH-ring consumption; rocjitsu remains the KFD policy owner.
 */
class RocjitsuKmdServer : public SimObject
{
  public:
    explicit RocjitsuKmdServer(const RocjitsuKmdServerParams &params);
    ~RocjitsuKmdServer() override;

    void init() override;
    void startup() override;
    DrainState drain() override;
    void drainResume() override;
    void serialize(CheckpointOut &cp) const override;

  private:
    class FdEvent;

    struct AllocationState
    {
        uint32_t processId = 0;
        uint64_t address = 0;
        uint64_t size = 0;
        uint32_t flags = 0;
        uint64_t mappedVa = 0;
        uint64_t mappedSize = 0;
    };

    struct QueueState
    {
        uint32_t processId = 0;
        gem5_hsakmt::DirectHardwareQueue hardware;
    };

    void initializePlatform();
    void createServer();
    void destroyServer();
    void startListening();
    void stopListening();
    void scheduleReconcile();
    void reconcilePollEvents();
    void processFd(int fd, int revents, bool notification);
    void observeClientState(const rj_kmd_server_stats_t &stats);
    void drainDoorbells();
    void configureInterruptConsumer();
    void scheduleInterruptPoll();
    void processInterruptPoll();
    bool serverEmpty() const;
    void maybeSignalDrainDone();
    bool ensureProcessConfigured(std::string *error = nullptr);

    Gem5DeviceBackend::Handlers makeBackendHandlers();
    int32_t openProcess(const rj_kmd_process_request_t &request);
    int32_t setProcessClientPid(const rj_kmd_process_request_t &request);
    int32_t closeProcess(const rj_kmd_process_request_t &request);
    int32_t allocate(const rj_kmd_allocation_request_t &request,
                     rj_kmd_backing_slice_t &slice);
    int32_t free(const rj_kmd_free_request_t &request);
    int32_t map(const rj_kmd_mapping_t &mapping);
    int32_t unmap(const rj_kmd_unmap_request_t &request);
    int32_t setMappingMemoryType(const rj_kmd_mapping_t &mapping);
    int32_t setApertures(const rj_kmd_apertures_t &apertures);
    int32_t createQueue(const rj_kmd_queue_t &queue);
    int32_t updateQueue(const rj_kmd_queue_update_t &queue);
    int32_t destroyQueue(const rj_kmd_queue_key_t &queue);
    int32_t write64(const rj_kmd_write64_t &write);
    void backendShutdown();
    void assertEventQueue() const;

    std::optional<uint64_t> translateRange(uint64_t vaddr,
                                           uint64_t size) const;
    void cleanupProcess(uint32_t process_id);
    rj_kmd_server_stats_t serverStats() const;

    static void diagnostic(void *context, int32_t status,
                           const char *message);

    const std::string runtimeRoot;
    const std::string socketPath;
    const bool autoStart;
    const bool exitOnDisconnect;
    const Addr framebufferBarBase;
    const Addr doorbellBarBase;
    const Addr mmioBarBase;
    const Addr setupMemoryBase;
    const Addr interruptRingOffset;
    const uint64_t interruptRingBytes;
    const Addr interruptWptrOffset;
    const Addr interruptDoorbellOffset;
    const Addr directDoorbellBase;
    const Tick interruptPollInterval;
    const uint16_t processPasid;
    const rj_kmd_device_info_t deviceInfo;

    AMDGPUDevice *const gpuDevice;
    const std::vector<SDMAEngine *> sdmaEngines;
    HsakmtServerPlatform *const serverPlatform;
    HSAPacketProcessor *const hsapp;
    PM4PacketProcessor *const pm4PktProc;
    GPUCommandProcessor *const gpuCmdProc;
    Shader *const shader;

    gem5_hsakmt::ServiceStats serviceStats;
    gem5_hsakmt::GpuVmManager gpuVmManager;
    gem5_hsakmt::QueueWorkEngine queueWorkEngine;
    gem5_hsakmt::SharedRegionAllocator allocator;
    gem5_hsakmt::DoorbellBridge doorbellBridge;
    Gem5DeviceBackend backend;
    rj_kmd_server_t *server = nullptr;

    std::unordered_map<uint64_t, AllocationState> allocations;
    std::map<uint64_t, uint64_t> mappings;
    std::unordered_map<uint32_t, QueueState> queues;
    std::optional<uint32_t> activeProcess;
    int32_t activeClientPid = 0;
    rj_kmd_apertures_t processApertures{};
    bool platformInitialized = false;
    bool processConfigured = false;
    bool accepting = false;
    bool sawClient = false;
    bool disconnectExitScheduled = false;
    bool shuttingDown = false;
    uint32_t interruptRptr = 0;

    std::unordered_map<int, std::unique_ptr<FdEvent>> fdEvents;
    std::unique_ptr<FdEvent> notificationEvent;
    EventFunctionWrapper reconcileEvent;
    EventFunctionWrapper interruptPollEvent;
};

} // namespace hsa
} // namespace gem5

#endif // __DEV_HSAKMT_SERVICE_ROCJITSU_KMD_SERVER_HH__
