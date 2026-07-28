#include "dev/hsakmt-service/service/rocjitsu_kmd_server.hh"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "base/logging.hh"
#include "base/pollevent.hh"
#include "debug/RocjitsuKmd.hh"
#include "dev/amdgpu/amdgpu_defines.hh"
#include "dev/amdgpu/amdgpu_device.hh"
#include "dev/amdgpu/amdgpu_vm.hh"
#include "dev/amdgpu/ih_mmio.hh"
#include "dev/amdgpu/interrupt_handler.hh"
#include "dev/amdgpu/pm4_packet_processor.hh"
#include "dev/amdgpu/sdma_engine.hh"
#include "dev/hsa/hsa_packet_processor.hh"
#include "dev/hsakmt-service/service/hsakmt_server_platform.hh"
#include "gpu-compute/gpu_command_processor.hh"
#include "gpu-compute/shader.hh"
#include "sim/cur_tick.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace hsa
{
namespace
{

constexpr uint64_t PageBytes = 4096;
constexpr uint64_t PageMask = PageBytes - 1;
constexpr uint64_t IhMmioBase = 0x4280;
constexpr uint64_t IhMmioDwordBytes = 4;

struct InterruptCookie
{
    uint32_t clientId : 8;
    uint32_t sourceId : 8;
    uint32_t ringId : 8;
    uint32_t vmId : 4;
    uint32_t reserved1 : 3;
    uint32_t vmidType : 1;
    uint32_t timestampLo;
    uint32_t timestampHi : 16;
    uint32_t reserved2 : 15;
    uint32_t timestampSource : 1;
    uint32_t pasid : 16;
    uint32_t nodeId : 8;
    uint32_t reserved3 : 7;
    uint32_t pasidSource : 1;
    uint32_t sourceDataDw1;
    uint32_t sourceDataDw2;
    uint32_t sourceDataDw3;
    uint32_t sourceDataDw4;
};
static_assert(sizeof(InterruptCookie) == INTR_COOKIE_SIZE);

template <typename T>
T
abiStruct()
{
    T value = {};
    value.abi_version = RJ_KMD_SERVER_ABI_VERSION;
    value.struct_size = sizeof(T);
    return value;
}

uint64_t
ihMmioOffset(uint32_t reg)
{
    return IhMmioBase + static_cast<uint64_t>(reg) * IhMmioDwordBytes;
}

rj_kmd_device_info_t
makeDeviceInfo(const RocjitsuKmdServerParams &params)
{
    auto info = abiStruct<rj_kmd_device_info_t>();
    info.architecture = params.architecture;
    info.gpu_id = params.gpu_id;
    info.gfx_target_version = params.gfx_target_version;
    info.vendor_id = params.vendor_id;
    info.device_id = params.device_id;
    info.family_id = params.family_id;
    info.drm_render_minor = params.drm_render_minor;
    info.simd_count = params.simd_count;
    info.max_waves_per_simd = params.max_waves_per_simd;
    info.num_shader_engines = params.num_shader_engines;
    info.num_shader_arrays_per_engine =
        params.num_shader_arrays_per_engine;
    info.num_cu_per_sh = params.num_cu_per_sh;
    info.simd_per_cu = params.simd_per_cu;
    info.wave_front_size = params.wave_front_size;
    info.num_xcc = params.num_xcc;
    info.max_slots_scratch_cu = params.max_slots_scratch_cu;
    info.local_mem_size = params.local_mem_size;
    info.lds_size_kb = params.lds_size_kb;
    info.num_sdma_engines = params.num_sdma_engines;
    info.num_cp_queues = params.num_cp_queues;
    info.capability = params.capability;
    info.capability2 = params.capability2;
    const auto length = std::min(
        params.marketing_name.size(), sizeof(info.marketing_name) - 1);
    std::memcpy(info.marketing_name, params.marketing_name.data(), length);
    return info;
}

gem5_hsakmt::GpuVmManagerConfig
makeGpuVmConfig(const RocjitsuKmdServerParams &params)
{
    gem5_hsakmt::GpuVmManagerConfig config;
    config.pageTableOffset = params.page_table_offset;
    config.pageTableBytes = params.page_table_bytes;
    config.vmId = 1;
    return config;
}

gem5_hsakmt::QueueWorkEngineConfig
makeQueueConfig(const RocjitsuKmdServerParams &params)
{
    gem5_hsakmt::QueueWorkEngineConfig config;
    config.setupMemoryBase = params.setup_memory_base;
    config.interruptRingOffset = params.interrupt_ring_offset;
    config.interruptRingBytes = params.interrupt_ring_bytes;
    config.interruptWptrOffset = params.interrupt_wptr_offset;
    config.interruptDoorbellOffset = params.interrupt_doorbell_offset;
    return config;
}

} // anonymous namespace

class RocjitsuKmdServer::FdEvent : public PollEvent
{
  public:
    FdEvent(RocjitsuKmdServer &owner, int fd, int events, bool notification)
        : PollEvent(fd, events), owner(owner), notification(notification)
    {}

    void
    process(int revents) override
    {
        owner.processFd(pfd.fd, revents, notification);
    }

    int
    requestedEvents() const
    {
        return pfd.events;
    }

    int
    fileDescriptor() const
    {
        return pfd.fd;
    }

  private:
    RocjitsuKmdServer &owner;
    const bool notification;
};

RocjitsuKmdServer::RocjitsuKmdServer(
    const RocjitsuKmdServerParams &params)
    : SimObject(params),
      runtimeRoot(params.runtime_root),
      socketPath(params.socket_path),
      autoStart(params.auto_start),
      exitOnDisconnect(params.exit_on_disconnect),
      framebufferBarBase(params.framebuffer_bar_pci_base),
      doorbellBarBase(params.doorbell_bar_pci_base),
      mmioBarBase(params.mmio_bar_pci_base),
      setupMemoryBase(params.setup_memory_base),
      interruptRingOffset(params.interrupt_ring_offset),
      interruptRingBytes(params.interrupt_ring_bytes),
      interruptWptrOffset(params.interrupt_wptr_offset),
      interruptDoorbellOffset(params.interrupt_doorbell_offset),
      directDoorbellBase(params.queue_doorbell_base),
      interruptPollInterval(params.interrupt_poll_interval),
      processPasid(params.process_pasid),
      deviceInfo(makeDeviceInfo(params)),
      gpuDevice(params.gpu_device),
      sdmaEngines(params.sdma_engines),
      serverPlatform(params.server_platform),
      hsapp(params.hsapp),
      pm4PktProc(params.pm4_pkt_proc),
      gpuCmdProc(params.gpu_cmd_proc),
      shader(params.shader),
      gpuVmManager(
          makeGpuVmConfig(params),
          gem5_hsakmt::GpuVmHardware{
              [this](uint64_t address, uint64_t value, unsigned size) {
                  serverPlatform->framebufferSetupWrite(
                      address, value, size);
              },
              [this](uint16_t vm_id, uint64_t root) {
                  auto &vm = gpuDevice->getVM();
                  vm.setPageTableBase(vm_id, root);
                  vm.setPageTableStart(vm_id, 0);
                  vm.setPageTableEnd(vm_id, MaxAddr);
                  vm.invalidateTLBs();
              },
          },
          serviceStats),
      queueWorkEngine(
          makeQueueConfig(params),
          gem5_hsakmt::QueueWorkHardware{
              [this](uint64_t address, uint64_t value, unsigned size) {
                  serverPlatform->framebufferWrite(address, value, size);
              },
              [this](uint64_t address, const void *data, uint64_t size) {
                  serverPlatform->framebufferWriteBlob(address, data, size);
              },
              [this](uint64_t offset, uint64_t value) {
                  serverPlatform->doorbellWrite(offset, value);
              },
              [this](uint64_t offset, uint32_t value) {
                  serverPlatform->mmioWrite32(offset, value);
              },
              [this] { return pm4PktProc->getMMIORange().start(); },
              [this] {
                  return gpuDevice->getVM()
                      .getMMIORange(GRBM_MMIO_RANGE)
                      .start();
              },
              [this] {
                  return gpuDevice->getGfxVersion() == GfxVersion::gfx942 ||
                         gpuDevice->getGfxVersion() == GfxVersion::gfx950;
              },
              [this] {
                  return gpuDevice->getGfxVersion() == GfxVersion::gfx90a ||
                         gpuDevice->getGfxVersion() == GfxVersion::gfx942 ||
                         gpuDevice->getGfxVersion() == GfxVersion::gfx950;
              },
          },
          serviceStats),
      allocator(params.allocation_offset, params.allocation_bytes,
                params.allocation_alignment),
      doorbellBridge([this](uint32_t offset, uint64_t value) {
          serverPlatform->doorbellWrite(directDoorbellBase + offset, value);
      }),
      backend(makeBackendHandlers()),
      reconcileEvent([this] { reconcilePollEvents(); },
                     name() + ".reconcile_poll_fds", false),
      interruptPollEvent([this] { processInterruptPoll(); },
                         name() + ".interrupt_poll", false)
{
    processApertures = abiStruct<rj_kmd_apertures_t>();
}

RocjitsuKmdServer::~RocjitsuKmdServer()
{
    destroyServer();
}

void
RocjitsuKmdServer::init()
{
    SimObject::init();
    fatal_if(!gpuDevice, "%s requires an AMDGPU device", name());
    fatal_if(!serverPlatform, "%s requires a server platform", name());
    fatal_if(!hsapp, "%s requires an HSA packet processor", name());
    fatal_if(!pm4PktProc, "%s requires a PM4 packet processor", name());
    fatal_if(!gpuCmdProc, "%s requires a GPU command processor", name());
    fatal_if(!shader, "%s requires a shader", name());
    fatal_if(socketPath.empty(), "%s requires a rocjitsu socket path", name());
    fatal_if(!interruptPollInterval,
             "%s requires a nonzero interrupt poll interval", name());
    fatal_if(directDoorbellBase & 7,
             "%s queue doorbell base must be 8-byte aligned", name());
}

void
RocjitsuKmdServer::startup()
{
    SimObject::startup();
    initializePlatform();
    if (autoStart) {
        createServer();
        startListening();
    }
}

void
RocjitsuKmdServer::initializePlatform()
{
    if (platformInitialized) {
        return;
    }
    serverPlatform->enumerateGpu(
        framebufferBarBase, doorbellBarBase, mmioBarBase);
    serverPlatform->installDeviceMemoryRouting(hsapp);
    serverPlatform->installDeviceMemoryRouting(pm4PktProc);
    serverPlatform->installDeviceMemoryRouting(gpuCmdProc);
    serverPlatform->installDeviceMemoryRouting(gpuDevice->getIH());
    for (auto *sdma : sdmaEngines) {
        serverPlatform->installDeviceMemoryRouting(sdma);
    }
    configureInterruptConsumer();

    std::string error;
    fatal_if(!queueWorkEngine.configurePm4ControlQueue(&error),
             "%s failed to configure PM4 control queue: %s",
             name(), error);
    platformInitialized = true;
}

void
RocjitsuKmdServer::configureInterruptConsumer()
{
    const std::vector<uint8_t> zero(interruptRingBytes, 0);
    serverPlatform->framebufferSetupWriteBlob(
        interruptRingOffset, zero.data(), zero.size());
    serverPlatform->framebufferSetupWrite(
        interruptWptrOffset, 0, sizeof(uint32_t));
    serverPlatform->mmioWrite32(ihMmioOffset(mmIH_RB_CNTL), 1);
    serverPlatform->mmioWrite32(
        ihMmioOffset(mmIH_RB_BASE),
        static_cast<uint32_t>((interruptRingOffset >> 8) & 0xffffffffu));
    serverPlatform->mmioWrite32(
        ihMmioOffset(mmIH_RB_BASE_HI),
        static_cast<uint32_t>(interruptRingOffset >> 40));
    serverPlatform->mmioWrite32(ihMmioOffset(mmIH_RB_RPTR), 0);
    serverPlatform->mmioWrite32(ihMmioOffset(mmIH_RB_WPTR), 0);
    serverPlatform->mmioWrite32(
        ihMmioOffset(mmIH_RB_WPTR_ADDR_HI),
        static_cast<uint32_t>(interruptWptrOffset >> 32));
    serverPlatform->mmioWrite32(
        ihMmioOffset(mmIH_RB_WPTR_ADDR_LO),
        static_cast<uint32_t>(interruptWptrOffset));
    serverPlatform->mmioWrite32(
        ihMmioOffset(mmIH_DOORBELL_RPTR),
        0x10000000u |
            static_cast<uint32_t>(interruptDoorbellOffset >> 2));
    interruptRptr = 0;
}

Gem5DeviceBackend::Handlers
RocjitsuKmdServer::makeBackendHandlers()
{
    return {
        [this] { assertEventQueue(); },
        [this](const auto &request) { return openProcess(request); },
        [this](const auto &request) {
            return setProcessClientPid(request);
        },
        [this](const auto &request) { return closeProcess(request); },
        [this](const auto &request, auto &slice) {
            return allocate(request, slice);
        },
        [this](const auto &request) { return free(request); },
        [this](const auto &request) { return map(request); },
        [this](const auto &request) { return unmap(request); },
        [this](const auto &request) {
            return setMappingMemoryType(request);
        },
        [this](const auto &request) { return setApertures(request); },
        [this](const auto &request) { return createQueue(request); },
        [this](const auto &request) { return updateQueue(request); },
        [this](const auto &request) { return destroyQueue(request); },
        [this](const auto &request) { return write64(request); },
        [this] { backendShutdown(); },
    };
}

void
RocjitsuKmdServer::createServer()
{
    if (server) {
        return;
    }
    auto config = abiStruct<rj_kmd_server_config_t>();
    config.runtime_root = runtimeRoot.c_str();
    config.socket_path = socketPath.c_str();
    config.diagnostic = &diagnostic;
    config.diagnostic_context = this;
    auto operations = backend.operations();
    std::array<char, 512> error{};
    const int32_t status = rj_kmd_server_create(
        &deviceInfo, &config, &operations, &server, error.data(),
        error.size());
    fatal_if(status != RJ_KMD_STATUS_SUCCESS || !server,
             "%s failed to create rocjitsu KMD server: %s",
             name(), error.data());
}

void
RocjitsuKmdServer::startListening()
{
    createServer();
    if (accepting) {
        return;
    }
    std::array<char, 512> error{};
    const int32_t status =
        rj_kmd_server_listen(server, error.data(), error.size());
    fatal_if(status != RJ_KMD_STATUS_SUCCESS,
             "%s failed to listen on %s: %s", name(), socketPath,
             error.data());
    accepting = true;
    reconcilePollEvents();
    scheduleInterruptPoll();
    inform("%s listening for rocjitsu KMD RPC on %s\n",
           name(), socketPath);
}

void
RocjitsuKmdServer::stopListening()
{
    if (server && accepting) {
        rj_kmd_server_stop_accepting(server);
    }
    accepting = false;
    scheduleReconcile();
}

void
RocjitsuKmdServer::destroyServer()
{
    shuttingDown = true;
    if (reconcileEvent.scheduled()) {
        deschedule(reconcileEvent);
    }
    if (interruptPollEvent.scheduled()) {
        deschedule(interruptPollEvent);
    }
    notificationEvent.reset();
    fdEvents.clear();
    if (server) {
        rj_kmd_server_destroy(server);
        server = nullptr;
    }
    accepting = false;
    doorbellBridge.clear();
    shuttingDown = false;
}

void
RocjitsuKmdServer::scheduleReconcile()
{
    if (!reconcileEvent.scheduled()) {
        schedule(reconcileEvent, curTick() + 1);
    }
}

void
RocjitsuKmdServer::reconcilePollEvents()
{
    if (!server) {
        fdEvents.clear();
        notificationEvent.reset();
        return;
    }

    size_t count = 0;
    fatal_if(rj_kmd_server_poll_fds(server, nullptr, &count) !=
                 RJ_KMD_STATUS_SUCCESS,
             "%s failed to enumerate KMD poll descriptors", name());
    std::vector<rj_kmd_poll_fd_t> descriptors(count);
    if (count) {
        size_t capacity = count;
        fatal_if(rj_kmd_server_poll_fds(
                     server, descriptors.data(), &capacity) !=
                     RJ_KMD_STATUS_SUCCESS,
                 "%s failed to read KMD poll descriptors", name());
        descriptors.resize(capacity);
    }

    std::unordered_map<int, int> wanted;
    for (const auto &descriptor : descriptors) {
        fatal_if(descriptor.abi_version != RJ_KMD_SERVER_ABI_VERSION ||
                     descriptor.struct_size < sizeof(descriptor) ||
                     descriptor.fd < 0,
                 "%s received an invalid KMD poll descriptor", name());
        wanted.emplace(descriptor.fd, descriptor.events);
    }
    for (auto event = fdEvents.begin(); event != fdEvents.end();) {
        const auto desired = wanted.find(event->first);
        if (desired == wanted.end() ||
            desired->second != event->second->requestedEvents()) {
            event = fdEvents.erase(event);
        } else {
            wanted.erase(desired);
            ++event;
        }
    }
    for (const auto &[fd, events] : wanted) {
        auto event = std::make_unique<FdEvent>(*this, fd, events, false);
        pollQueue.schedule(event.get());
        fdEvents.emplace(fd, std::move(event));
    }

    const int notification_fd = rj_kmd_server_notification_fd(server);
    if (notification_fd >= 0 &&
        (!notificationEvent ||
         notificationEvent->fileDescriptor() != notification_fd)) {
        notificationEvent.reset();
        notificationEvent = std::make_unique<FdEvent>(
            *this, notification_fd, POLLIN | POLLERR, true);
        pollQueue.schedule(notificationEvent.get());
    }
}

void
RocjitsuKmdServer::processFd(int fd, int revents, bool notification)
{
    assertEventQueue();
    if (notification) {
        drainDoorbells();
        return;
    }

    const auto before = serverStats();
    std::array<char, 512> error{};
    const int32_t status = rj_kmd_server_process_fd(
        server, fd, static_cast<int16_t>(revents), error.data(),
        error.size());
    fatal_if(status != RJ_KMD_STATUS_SUCCESS,
             "%s KMD reactor failed on fd %d: %s", name(), fd,
             error.data());
    const auto after = serverStats();
    DPRINTF(RocjitsuKmd,
            "processed fd %d revents %#x: clients %lu -> %lu, "
            "processes %lu -> %lu, responses %lu -> %lu\n",
            fd, revents, before.active_clients, after.active_clients,
            before.active_processes, after.active_processes,
            before.pending_responses, after.pending_responses);
    observeClientState(before);
    observeClientState(after);
    scheduleReconcile();
    maybeSignalDrainDone();
}

void
RocjitsuKmdServer::observeClientState(
    const rj_kmd_server_stats_t &stats)
{
    if (stats.active_clients != 0) {
        if (!sawClient) {
            inform("%s accepted a rocjitsu KMD client\n", name());
        }
        sawClient = true;
        return;
    }

    if (!exitOnDisconnect || !sawClient || disconnectExitScheduled) {
        return;
    }

    const bool drained =
        stats.active_processes == 0 && stats.active_queues == 0 &&
        stats.pending_responses == 0 && stats.pending_doorbells == 0 &&
        allocations.empty() && mappings.empty() && queues.empty() &&
        !activeProcess;
    if (drained) {
        inform("%s rocjitsu KMD state drained\n", name());
    } else {
        warn("%s rocjitsu disconnect left state: processes=%lu queues=%lu "
             "responses=%lu doorbells=%lu allocations=%zu mappings=%zu "
             "local_queues=%zu",
             name(), stats.active_processes, stats.active_queues,
             stats.pending_responses, stats.pending_doorbells,
             allocations.size(), mappings.size(), queues.size());
    }
    disconnectExitScheduled = true;
    inform("%s rocjitsu KMD client disconnected\n", name());
    exitSimLoop("rocjitsu KMD client disconnected");
}

void
RocjitsuKmdServer::drainDoorbells()
{
    while (server) {
        size_t count = 0;
        const int32_t query =
            rj_kmd_server_drain_doorbells(server, nullptr, &count);
        fatal_if(query != RJ_KMD_STATUS_SUCCESS,
                 "%s failed to query pending doorbells", name());
        if (!count) {
            break;
        }
        std::vector<rj_kmd_doorbell_record_t> records(count);
        size_t capacity = count;
        const int32_t status = rj_kmd_server_drain_doorbells(
            server, records.data(), &capacity);
        fatal_if(status != RJ_KMD_STATUS_SUCCESS && status != -ENOSPC,
                 "%s failed to drain doorbell records", name());
        for (size_t index = 0; index < capacity; ++index) {
            const auto &record = records[index];
            fatal_if(record.abi_version != RJ_KMD_SERVER_ABI_VERSION ||
                         record.struct_size < sizeof(record),
                     "%s received an invalid doorbell record", name());
            const bool delivered = doorbellBridge.deliver(
                {record.process_id, record.queue_id, record.offset,
                 record.value});
            warn_if(!delivered,
                    "%s dropped stale doorbell for process %u queue %u",
                    name(), record.process_id, record.queue_id);
        }
    }
    maybeSignalDrainDone();
}

void
RocjitsuKmdServer::scheduleInterruptPoll()
{
    if (server && !interruptPollEvent.scheduled()) {
        schedule(interruptPollEvent, curTick() + interruptPollInterval);
    }
}

void
RocjitsuKmdServer::processInterruptPoll()
{
    if (!server) {
        return;
    }
    observeClientState(serverStats());
    uint32_t wptr = 0;
    serverPlatform->framebufferReadBlob(
        interruptWptrOffset, &wptr, sizeof(wptr));
    fatal_if(wptr > interruptRingBytes || (wptr % INTR_COOKIE_SIZE),
             "%s observed invalid IH write pointer %#x", name(), wptr);
    bool advanced = false;
    while (interruptRptr < wptr) {
        InterruptCookie cookie = {};
        serverPlatform->framebufferReadBlob(
            interruptRingOffset + interruptRptr, &cookie, sizeof(cookie));
        if (activeProcess) {
            const int32_t status = rj_kmd_server_post_interrupt(
                server, *activeProcess, cookie.sourceDataDw1);
            fatal_if(status != RJ_KMD_STATUS_SUCCESS,
                     "%s failed to post IH event %u", name(),
                     cookie.sourceDataDw1);
        }
        interruptRptr += INTR_COOKIE_SIZE;
        advanced = true;
    }
    if (advanced) {
        serverPlatform->doorbellWrite(
            interruptDoorbellOffset, interruptRptr);
    }
    scheduleInterruptPoll();
}

void
RocjitsuKmdServer::assertEventQueue() const
{
    if (!shuttingDown) {
        fatal_if(curEventQueue() != eventQueue(),
                 "%s backend callback escaped gem5's event queue", name());
    }
}

int32_t
RocjitsuKmdServer::openProcess(
    const rj_kmd_process_request_t &request)
{
    if (!request.process_id ||
        (activeProcess && *activeProcess != request.process_id)) {
        return -EBUSY;
    }
    activeProcess = request.process_id;
    activeClientPid = request.client_pid;
    processConfigured = false;
    return 0;
}

int32_t
RocjitsuKmdServer::setProcessClientPid(
    const rj_kmd_process_request_t &request)
{
    if (!activeProcess || *activeProcess != request.process_id) {
        return -ESRCH;
    }
    activeClientPid = request.client_pid;
    return 0;
}

int32_t
RocjitsuKmdServer::closeProcess(
    const rj_kmd_process_request_t &request)
{
    if (!activeProcess || *activeProcess != request.process_id) {
        return 0;
    }
    cleanupProcess(request.process_id);
    activeProcess.reset();
    activeClientPid = 0;
    processConfigured = false;
    maybeSignalDrainDone();
    return 0;
}

int32_t
RocjitsuKmdServer::allocate(
    const rj_kmd_allocation_request_t &request,
    rj_kmd_backing_slice_t &slice)
{
    if (!activeProcess || *activeProcess != request.process_id ||
        request.gpu_id != deviceInfo.gpu_id || !request.size) {
        return -EINVAL;
    }
    const uint64_t page_offset = request.gpu_va & PageMask;
    if (request.size >
        std::numeric_limits<uint64_t>::max() - page_offset) {
        return -EOVERFLOW;
    }
    // The client maps the returned shared-memory slice at the page containing
    // gpu_va. Preserve gpu_va's page offset in the device address so the same
    // bytes are visible through both mappings. The allocator owns the full
    // page-aligned extent; KFD exposes only the requested logical subrange.
    const auto allocation = allocator.allocate(page_offset + request.size);
    if (!allocation) {
        return -ENOMEM;
    }
    const auto backing =
        gpuDevice->getVramBackingStore(allocation->address, allocation->size);
    if (!backing) {
        allocator.release(allocation->id);
        return -ENOTSUP;
    }
    const int fd = fcntl(backing->shmFd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
        allocator.release(allocation->id);
        return -errno;
    }
    const uint64_t offset_in_entry =
        allocation->address - backing->range.start();
    if (backing->shmOffset < 0 ||
        static_cast<uint64_t>(backing->shmOffset) >
            std::numeric_limits<uint64_t>::max() - offset_in_entry) {
        ::close(fd);
        allocator.release(allocation->id);
        return -EOVERFLOW;
    }

    const uint64_t device_paddr = allocation->address + page_offset;
    allocations.emplace(
        allocation->id,
        AllocationState{request.process_id, device_paddr,
                        request.size, request.flags, 0, 0});
    slice.allocation_id = allocation->id;
    slice.device_paddr = device_paddr;
    slice.size = allocation->size;
    slice.fd = fd;
    slice.fd_offset =
        static_cast<uint64_t>(backing->shmOffset) + offset_in_entry;
    return 0;
}

int32_t
RocjitsuKmdServer::free(const rj_kmd_free_request_t &request)
{
    const auto allocation = allocations.find(request.allocation_id);
    if (allocation == allocations.end()) {
        return 0;
    }
    if (allocation->second.processId != request.process_id) {
        return -EPERM;
    }
    const uint64_t allocation_end =
        allocation->second.address + allocation->second.size;
    for (const auto &[id, queue] : queues) {
        (void)id;
        const auto ring = translateRange(
            queue.hardware.ringBaseVa, queue.hardware.ringBytes);
        if (queue.processId == request.process_id && ring &&
            *ring < allocation_end &&
            allocation->second.address < *ring + queue.hardware.ringBytes) {
            return -EBUSY;
        }
    }
    std::string error;
    if (!gpuVmManager.releaseAllocation(request.allocation_id, &error) ||
        !allocator.release(request.allocation_id)) {
        warn("%s failed to release allocation %lu: %s", name(),
             request.allocation_id, error);
        return -EIO;
    }
    if (allocation->second.mappedVa) {
        mappings.erase(allocation->second.mappedVa);
    }
    allocations.erase(allocation);
    return 0;
}

int32_t
RocjitsuKmdServer::map(const rj_kmd_mapping_t &mapping)
{
    const auto allocation = allocations.find(mapping.allocation_id);
    if (allocation == allocations.end() ||
        allocation->second.processId != mapping.process_id ||
        mapping.device_paddr != allocation->second.address ||
        mapping.size > allocation->second.size || !mapping.size) {
        return -EINVAL;
    }
    if (allocation->second.mappedVa &&
        (allocation->second.mappedVa != mapping.gpu_va ||
         allocation->second.mappedSize != mapping.size)) {
        return -EBUSY;
    }
    // Every allocation returned by this backend is an extent of the GPU's
    // exported VRAM backing store. GTT/USERPTR flags describe the client-side
    // mapping contract; they do not turn that extent into host physical
    // memory. Keep the GPUVM PTE device-local so translated traffic reaches
    // the GPU memory network.
    const auto kind = gem5_hsakmt::GpuVmMappingKind::DeviceLocal;
    std::string error;
    if (!gpuVmManager.mapOwned(
            gem5_hsakmt::GpuVmOwner::allocation(mapping.allocation_id),
            mapping.gpu_va, mapping.device_paddr, mapping.size, kind,
            &error)) {
        warn("%s rejected GPUVM mapping: %s", name(), error);
        return -EINVAL;
    }
    allocation->second.mappedVa = mapping.gpu_va;
    allocation->second.mappedSize = mapping.size;
    mappings[mapping.gpu_va] = mapping.allocation_id;
    return 0;
}

int32_t
RocjitsuKmdServer::unmap(const rj_kmd_unmap_request_t &request)
{
    const auto mapping = mappings.find(request.gpu_va);
    if (mapping == mappings.end()) {
        return 0;
    }
    auto allocation = allocations.find(mapping->second);
    if (allocation == allocations.end() ||
        allocation->second.processId != request.process_id ||
        request.size != allocation->second.mappedSize) {
        return -EINVAL;
    }
    std::string error;
    if (!gpuVmManager.releaseAllocation(mapping->second, &error)) {
        warn("%s failed to unmap GPUVM allocation: %s", name(), error);
        return -EIO;
    }
    allocation->second.mappedVa = 0;
    allocation->second.mappedSize = 0;
    mappings.erase(mapping);
    return 0;
}

int32_t
RocjitsuKmdServer::setMappingMemoryType(
    const rj_kmd_mapping_t &mapping)
{
    const auto existing = mappings.find(mapping.gpu_va);
    if (existing == mappings.end()) {
        return -ENOENT;
    }
    // Cache policy changes do not change the physical aperture backing this
    // allocation.
    const auto kind = gem5_hsakmt::GpuVmMappingKind::DeviceLocal;
    std::string error;
    return gpuVmManager.remapOwnedKind(
               gem5_hsakmt::GpuVmOwner::allocation(existing->second),
               mapping.gpu_va, mapping.size, kind, &error)
        ? 0
        : -EINVAL;
}

int32_t
RocjitsuKmdServer::setApertures(const rj_kmd_apertures_t &apertures)
{
    if (processConfigured &&
        (processApertures.lds_base != apertures.lds_base ||
         processApertures.scratch_base != apertures.scratch_base)) {
        return -EBUSY;
    }
    processApertures = apertures;
    return 0;
}

bool
RocjitsuKmdServer::ensureProcessConfigured(std::string *error)
{
    if (processConfigured) {
        return true;
    }
    if (!activeProcess || !gpuVmManager.pageTableRoot()) {
        if (error) {
            *error = "process has no GPUVM page-table root";
        }
        return false;
    }
    if (!queueWorkEngine.configureDirectProcess(
            processPasid, gpuVmManager.pageTableRoot(),
            processApertures.lds_base, processApertures.scratch_base,
            error)) {
        return false;
    }
    processConfigured = true;
    return true;
}

std::optional<uint64_t>
RocjitsuKmdServer::translateRange(uint64_t vaddr, uint64_t size) const
{
    if (!size || vaddr > MaxAddr - (size - 1)) {
        return std::nullopt;
    }
    const auto first = gpuVmManager.translate(vaddr);
    const auto last = gpuVmManager.translate(vaddr + size - 1);
    if (!first || !last ||
        first->physicalAddress > MaxAddr - (size - 1) ||
        last->physicalAddress != first->physicalAddress + size - 1) {
        return std::nullopt;
    }
    return first->physicalAddress;
}

int32_t
RocjitsuKmdServer::createQueue(const rj_kmd_queue_t &queue)
{
    if (!activeProcess || *activeProcess != queue.process_id ||
        queue.queue_id == 0 || queues.count(queue.queue_id) ||
        queue.ring_size == 0 ||
        !translateRange(queue.ring_base_va, queue.ring_size) ||
        !translateRange(queue.read_pointer_va, sizeof(uint64_t)) ||
        !translateRange(queue.write_pointer_va, sizeof(uint64_t)) ||
        (!queue.is_sdma && queue.queue_descriptor_va &&
         !translateRange(queue.queue_descriptor_va, sizeof(uint64_t)))) {
        return -EINVAL;
    }
    std::string error;
    if (!ensureProcessConfigured(&error)) {
        warn("%s could not configure direct process: %s", name(), error);
        return -EIO;
    }
    gem5_hsakmt::DirectHardwareQueue hardware{
        queue.queue_id,
        queue.ring_base_va,
        queue.ring_size,
        queue.read_pointer_va,
        queue.write_pointer_va,
        queue.queue_descriptor_va,
        directDoorbellBase + queue.doorbell_offset,
        queue.is_sdma != 0,
    };
    if (!queueWorkEngine.registerDirectQueue(hardware, &error)) {
        warn("%s rejected direct queue %u: %s", name(), queue.queue_id,
             error);
        return -EINVAL;
    }
    if (!doorbellBridge.registerQueue(
            queue.process_id, queue.queue_id, queue.doorbell_offset)) {
        queueWorkEngine.unregisterDirectQueue(queue.queue_id, nullptr);
        return -EINVAL;
    }
    queues.emplace(queue.queue_id, QueueState{queue.process_id, hardware});
    return 0;
}

int32_t
RocjitsuKmdServer::updateQueue(const rj_kmd_queue_update_t &queue)
{
    auto existing = queues.find(queue.queue_id);
    if (existing == queues.end() ||
        existing->second.processId != queue.process_id ||
        !translateRange(queue.ring_base_va, queue.ring_size)) {
        return -EINVAL;
    }
    std::string error;
    if (!queueWorkEngine.updateDirectQueue(
            queue.queue_id, queue.ring_base_va, queue.ring_size, &error)) {
        warn("%s failed to update direct queue: %s", name(), error);
        return -EIO;
    }
    existing->second.hardware.ringBaseVa = queue.ring_base_va;
    existing->second.hardware.ringBytes = queue.ring_size;
    return 0;
}

int32_t
RocjitsuKmdServer::destroyQueue(const rj_kmd_queue_key_t &queue)
{
    const auto existing = queues.find(queue.queue_id);
    if (existing == queues.end()) {
        return 0;
    }
    if (existing->second.processId != queue.process_id) {
        return -EPERM;
    }
    std::string error;
    if (!queueWorkEngine.unregisterDirectQueue(queue.queue_id, &error)) {
        warn("%s failed to destroy direct queue: %s", name(), error);
        return -EIO;
    }
    doorbellBridge.unregisterQueue(queue.process_id, queue.queue_id);
    queues.erase(existing);
    return 0;
}

int32_t
RocjitsuKmdServer::write64(const rj_kmd_write64_t &write)
{
    if (!activeProcess || *activeProcess != write.process_id) {
        return -ESRCH;
    }
    const auto paddr = translateRange(write.gpu_va, sizeof(uint64_t));
    if (!paddr) {
        return -EFAULT;
    }
    serverPlatform->framebufferWrite(
        *paddr, write.value, sizeof(write.value));
    return 0;
}

void
RocjitsuKmdServer::cleanupProcess(uint32_t process_id)
{
    std::vector<uint32_t> queue_ids;
    for (const auto &[id, queue] : queues) {
        if (queue.processId == process_id) {
            queue_ids.push_back(id);
        }
    }
    for (const uint32_t id : queue_ids) {
        auto key = abiStruct<rj_kmd_queue_key_t>();
        key.process_id = process_id;
        key.queue_id = id;
        (void)destroyQueue(key);
    }

    if (processConfigured) {
        std::string error;
        if (!queueWorkEngine.unconfigureDirectProcess(
                processPasid, &error)) {
            warn("%s failed to unconfigure direct process: %s",
                 name(), error);
        }
        processConfigured = false;
    }

    std::vector<uint64_t> allocation_ids;
    for (const auto &[id, allocation] : allocations) {
        if (allocation.processId == process_id) {
            allocation_ids.push_back(id);
        }
    }
    for (const uint64_t id : allocation_ids) {
        auto request = abiStruct<rj_kmd_free_request_t>();
        request.process_id = process_id;
        request.allocation_id = id;
        (void)free(request);
    }
}

void
RocjitsuKmdServer::backendShutdown()
{
    if (activeProcess) {
        cleanupProcess(*activeProcess);
        activeProcess.reset();
    }
}

rj_kmd_server_stats_t
RocjitsuKmdServer::serverStats() const
{
    auto stats = abiStruct<rj_kmd_server_stats_t>();
    if (server) {
        const int32_t status = rj_kmd_server_get_stats(server, &stats);
        fatal_if(status != RJ_KMD_STATUS_SUCCESS,
                 "%s failed to read rocjitsu server state", name());
    }
    return stats;
}

bool
RocjitsuKmdServer::serverEmpty() const
{
    const auto stats = serverStats();
    return stats.active_clients == 0 && stats.active_processes == 0 &&
        stats.active_queues == 0 && stats.pending_responses == 0 &&
        stats.pending_doorbells == 0 && allocations.empty() &&
        mappings.empty() && queues.empty() && !activeProcess;
}

DrainState
RocjitsuKmdServer::drain()
{
    stopListening();
    if (reconcileEvent.scheduled()) {
        deschedule(reconcileEvent);
    }
    reconcilePollEvents();
    if (!serverEmpty()) {
        return DrainState::Draining;
    }
    if (interruptPollEvent.scheduled()) {
        deschedule(interruptPollEvent);
    }
    return DrainState::Drained;
}

void
RocjitsuKmdServer::maybeSignalDrainDone()
{
    if (drainState() == DrainState::Draining && serverEmpty()) {
        if (interruptPollEvent.scheduled()) {
            deschedule(interruptPollEvent);
        }
        signalDrainDone();
    }
}

void
RocjitsuKmdServer::drainResume()
{
    if (autoStart && server && !accepting) {
        startListening();
    }
}

void
RocjitsuKmdServer::serialize(CheckpointOut &) const
{
    fatal_if(!serverEmpty(),
             "%s cannot checkpoint a live rocjitsu KMD connection",
             name());
}

void
RocjitsuKmdServer::diagnostic(
    void *context, int32_t status, const char *message)
{
    auto *self = static_cast<RocjitsuKmdServer *>(context);
    warn("%s rocjitsu KMD diagnostic (%d): %s",
         self ? self->name() : "rocjitsu_kmd", status,
         message ? message : "unknown error");
}

} // namespace hsa
} // namespace gem5
