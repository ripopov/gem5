#include "dev/hsakmt-service/service/hsakmt_server_platform.hh"

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "base/logging.hh"
#include "dev/amdgpu/amdgpu_device.hh"
#include "dev/pci/pcireg.h"
#include "mem/abstract_mem.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "sim/system.hh"

namespace gem5
{
namespace hsa
{
namespace
{

constexpr uint16_t PciCommandMemorySpace = 0x2;

bool
isSupportedAccessSize(unsigned size)
{
    return size == sizeof(uint8_t) || size == sizeof(uint16_t) ||
        size == sizeof(uint32_t) || size == sizeof(uint64_t);
}

uint16_t
barConfigOffset(uint32_t bar)
{
    switch (bar) {
      case FRAMEBUFFER_BAR:
        return PCI0_BASE_ADDR0;
      case DOORBELL_BAR:
        return PCI0_BASE_ADDR2;
      case MMIO_BAR:
        return PCI0_BASE_ADDR5;
      default:
        panic("Unsupported AMDGPU BAR%u for HSAKMT server platform", bar);
    }
}

bool
hasUpperBar(uint32_t bar)
{
    return bar == FRAMEBUFFER_BAR || bar == DOORBELL_BAR;
}

void
registerDeviceMemoryForRequestor(
    System &system, RequestorID requestor_id,
    const std::vector<memory::AbstractMemory *> &memories)
{
    for (auto *memory : memories) {
        system.addDeviceMemory(requestor_id, memory);
    }
}

} // anonymous namespace

HsakmtServerPlatform::HostRequestPort::HostRequestPort(
    HsakmtServerPlatform &platform)
    : RequestPort(platform.name() + ".host_port"), platform(platform)
{
}

bool
HsakmtServerPlatform::HostRequestPort::recvTimingResp(PacketPtr pkt)
{
    platform.noteTimingResponse(pkt);
    return true;
}

void
HsakmtServerPlatform::HostRequestPort::recvReqRetry()
{
    platform.noteReqRetry();
}

HsakmtServerPlatform::HsakmtServerPlatform(
    const HsakmtServerPlatformParams &params)
    : SimObject(params),
      hostPort(*this),
      system(params.system),
      gpuDevice(params.gpu_device),
      gpuMemoryBackends(params.device_memories),
      pciConfigBase(params.pci_config_base),
      pciConfigDeviceBits(params.pci_config_device_bits),
      gpuPciBus(params.gpu_pci_bus),
      requestorId(params.system->getRequestorId(this))
{
}

void
HsakmtServerPlatform::init()
{
    SimObject::init();

    fatal_if(!system, "%s requires a System reference", name());
    fatal_if(!gpuDevice, "%s requires an AMDGPU device reference", name());
    fatal_if(!hostPort.isConnected(),
             "%s host_port must be connected to the host PIO path", name());
    fatal_if(pciConfigDeviceBits >= 32,
             "%s pci_config_device_bits=%u is unsupported",
             name(), pciConfigDeviceBits);
}

Port &
HsakmtServerPlatform::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "host_port") {
        return hostPort;
    }

    return SimObject::getPort(if_name, idx);
}

void
HsakmtServerPlatform::enumerateGpu(Addr framebuffer_base, Addr doorbell_base,
                                   Addr mmio_base)
{
    configureBar(FRAMEBUFFER_BAR, framebuffer_base);
    configureBar(DOORBELL_BAR, doorbell_base);
    configureBar(MMIO_BAR, mmio_base);

    const uint16_t command =
        static_cast<uint16_t>(readConfig(PCI_COMMAND, sizeof(uint16_t)));
    writeConfig(PCI_COMMAND, command | PciCommandMemorySpace,
                sizeof(uint16_t));

    cacheAndValidateBar(FRAMEBUFFER_BAR, framebuffer_base);
    cacheAndValidateBar(DOORBELL_BAR, doorbell_base);
    cacheAndValidateBar(MMIO_BAR, mmio_base);
}

void
HsakmtServerPlatform::installDeviceMemoryRouting(RequestorID requestor_id)
{
    fatal_if(!system,
             "%s cannot install device-memory routing without System",
             name());
    fatal_if(gpuMemoryBackends.empty(),
             "%s cannot install device-memory routing without memories",
             name());
    registerDeviceMemoryForRequestor(*system, requestor_id, gpuMemoryBackends);
}

void
HsakmtServerPlatform::installDeviceMemoryRouting(const SimObject *requestor)
{
    fatal_if(!requestor,
             "%s cannot install device-memory routing for a null requestor",
             name());
    fatal_if(!system,
             "%s cannot install device-memory routing without System",
             name());

    const RequestorID requestor_id = system->lookupRequestorId(requestor);
    fatal_if(requestor_id == Request::invldRequestorId,
             "%s could not find RequestorID for %s",
             name(), requestor->name());
    installDeviceMemoryRouting(requestor_id);
}

void
HsakmtServerPlatform::framebufferWrite(Addr offset, uint64_t value,
                                       unsigned size)
{
    writePio(barAddr(FRAMEBUFFER_BAR, offset, size), value, size);
}

uint64_t
HsakmtServerPlatform::framebufferRead(Addr offset, unsigned size) const
{
    return readPio(barAddr(FRAMEBUFFER_BAR, offset, size), size);
}

void
HsakmtServerPlatform::framebufferWriteBlob(Addr offset, const void *data,
                                           uint64_t size)
{
    writePioBlob(barAddr(FRAMEBUFFER_BAR, offset, size), data, size);
}

void
HsakmtServerPlatform::framebufferReadBlob(Addr offset, void *data,
                                          uint64_t size) const
{
    readPioBlob(barAddr(FRAMEBUFFER_BAR, offset, size), data, size);
}

void
HsakmtServerPlatform::framebufferSetupWrite(Addr offset, uint64_t value,
                                            unsigned size)
{
    fatal_if(!isSupportedAccessSize(size),
             "%s framebuffer setup write has unsupported size %u",
             name(), size);

    std::array<uint8_t, sizeof(value)> bytes{};
    for (unsigned i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    }
    framebufferSetupWriteBlob(offset, bytes.data(), size);
}

void
HsakmtServerPlatform::framebufferSetupWriteBlob(Addr offset, const void *data,
                                                uint64_t size)
{
    if (size == 0) {
        return;
    }

    fatal_if(!data, "%s framebuffer setup write has null data", name());

    // Validate the setup write against the enumerated BAR aperture even
    // though setup-only data loading writes the device-memory backing store
    // directly. Control registers and doorbells continue to use PIO.
    (void)barAddr(FRAMEBUFFER_BAR, offset, size);

    const auto *bytes = static_cast<const uint8_t *>(data);
    uint64_t written = 0;
    while (written < size) {
        const Addr current = offset + written;
        const auto memory = std::find_if(
            gpuMemoryBackends.begin(), gpuMemoryBackends.end(),
            [current](const memory::AbstractMemory *candidate) {
                return candidate->getAddrRange().contains(current);
            });
        fatal_if(memory == gpuMemoryBackends.end(),
                 "%s has no device memory for framebuffer offset %#lx",
                 name(), current);

        MemBackdoorPtr backdoor = nullptr;
        (*memory)->getBackdoor(backdoor);
        fatal_if(!backdoor || !backdoor->ptr() || !backdoor->writeable(),
                 "%s device memory %s has no writeable setup backdoor",
                 name(), (*memory)->name());

        const AddrRange &range = backdoor->range();
        fatal_if(!range.contains(current),
                 "%s device-memory backdoor %s does not contain %#lx",
                 name(), range.to_string().c_str(), current);

        uint64_t contiguous = range.end() - current;
        if (range.interleaved()) {
            const uint64_t granularity = range.granularity();
            contiguous = std::min(
                contiguous,
                granularity - ((current - range.start()) % granularity));
        }
        const uint64_t chunk = std::min(contiguous, size - written);
        fatal_if(chunk == 0,
                 "%s device-memory backdoor made no progress at %#lx",
                 name(), current);

        std::memcpy(backdoor->ptr() + current - range.start(),
                    bytes + written, chunk);
        written += chunk;
    }
}

void
HsakmtServerPlatform::doorbellWrite(Addr offset, uint64_t value)
{
    writePio(barAddr(DOORBELL_BAR, offset, sizeof(value)),
             value, sizeof(value));
}

void
HsakmtServerPlatform::mmioWrite32(Addr offset, uint32_t value)
{
    writePio(barAddr(MMIO_BAR, offset, sizeof(value)), value, sizeof(value));
}

uint32_t
HsakmtServerPlatform::mmioRead32(Addr offset) const
{
    return static_cast<uint32_t>(
        readPio(barAddr(MMIO_BAR, offset, sizeof(uint32_t)),
                sizeof(uint32_t)));
}

Addr
HsakmtServerPlatform::configAddr(uint8_t bus, uint8_t dev, uint8_t func,
                                 uint16_t offset) const
{
    const Addr bus_addr = (static_cast<Addr>(bus) << 8) |
        (static_cast<Addr>(dev) << 3) | static_cast<Addr>(func);
    return pciConfigBase + (bus_addr << pciConfigDeviceBits) + offset;
}

uint64_t
HsakmtServerPlatform::readPio(Addr addr, unsigned size) const
{
    fatal_if(!isSupportedAccessSize(size),
             "%s PIO read has unsupported size %u", name(), size);

    RequestPtr req = std::make_shared<Request>(addr, size, 0, requestorId);
    PacketPtr pkt = Packet::createRead(req);
    pkt->allocate();
    hostPort.sendAtomic(pkt);
    fatal_if(pkt->isError(), "%s PIO read failed at %#lx size %u",
             name(), addr, size);
    const uint64_t value = pkt->getUintX(ByteOrder::little);
    delete pkt;
    return value;
}

void
HsakmtServerPlatform::writePio(Addr addr, uint64_t value, unsigned size) const
{
    fatal_if(!isSupportedAccessSize(size),
             "%s PIO write has unsupported size %u", name(), size);

    RequestPtr req = std::make_shared<Request>(addr, size, 0, requestorId);
    PacketPtr pkt = Packet::createWrite(req);
    pkt->allocate();
    pkt->setUintX(value, ByteOrder::little);
    hostPort.sendAtomic(pkt);
    fatal_if(pkt->isError(), "%s PIO write failed at %#lx size %u",
             name(), addr, size);
    delete pkt;
}

void
HsakmtServerPlatform::writePioBlob(Addr addr, const void *data,
                                   uint64_t size) const
{
    if (size == 0) {
        return;
    }

    fatal_if(!data, "%s PIO blob write has null data", name());

    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    uint64_t written = 0;
    while (written < size) {
        const Addr current = addr + written;
        uint64_t value = 0;
        unsigned chunk = 1;
        if ((current % sizeof(uint64_t)) == 0 &&
            size - written >= sizeof(uint64_t)) {
            chunk = sizeof(uint64_t);
        } else if ((current % sizeof(uint32_t)) == 0 &&
                   size - written >= sizeof(uint32_t)) {
            chunk = sizeof(uint32_t);
        } else if ((current % sizeof(uint16_t)) == 0 &&
                   size - written >= sizeof(uint16_t)) {
            chunk = sizeof(uint16_t);
        }

        for (unsigned i = 0; i < chunk; i++) {
            value |= static_cast<uint64_t>(bytes[written + i]) << (8 * i);
        }

        writePio(current, value, chunk);
        written += chunk;
    }
}

void
HsakmtServerPlatform::readPioBlob(Addr addr, void *data, uint64_t size) const
{
    if (size == 0) {
        return;
    }

    fatal_if(!data, "%s PIO blob read has null data", name());

    uint8_t *bytes = static_cast<uint8_t *>(data);
    uint64_t read = 0;
    while (read < size) {
        const Addr current = addr + read;
        unsigned chunk = 1;
        if ((current % sizeof(uint64_t)) == 0 &&
            size - read >= sizeof(uint64_t)) {
            chunk = sizeof(uint64_t);
        } else if ((current % sizeof(uint32_t)) == 0 &&
                   size - read >= sizeof(uint32_t)) {
            chunk = sizeof(uint32_t);
        } else if ((current % sizeof(uint16_t)) == 0 &&
                   size - read >= sizeof(uint16_t)) {
            chunk = sizeof(uint16_t);
        }

        const uint64_t value = readPio(current, chunk);
        for (unsigned i = 0; i < chunk; i++) {
            bytes[read + i] = static_cast<uint8_t>(value >> (8 * i));
        }

        read += chunk;
    }
}

uint64_t
HsakmtServerPlatform::readConfig(uint16_t offset, unsigned size) const
{
    const auto &addr = gpuDevice->devAddr();
    return readPio(configAddr(gpuPciBus, addr.dev, addr.func, offset), size);
}

void
HsakmtServerPlatform::writeConfig(uint16_t offset, uint64_t value,
                                  unsigned size) const
{
    const auto &addr = gpuDevice->devAddr();
    writePio(configAddr(gpuPciBus, addr.dev, addr.func, offset), value, size);
}

void
HsakmtServerPlatform::configureBar(uint32_t bar, Addr base)
{
    const uint16_t offset = barConfigOffset(bar);
    writeConfig(offset, static_cast<uint32_t>(base), sizeof(uint32_t));
    if (hasUpperBar(bar)) {
        writeConfig(offset + sizeof(uint32_t),
                    static_cast<uint32_t>(base >> 32), sizeof(uint32_t));
    }
}

void
HsakmtServerPlatform::cacheAndValidateBar(uint32_t bar, Addr base)
{
    const AddrRange gpu_range = findGpuRange(base);
    fatal_if(!gpu_range.valid(),
             "%s BAR%u base %#lx was not exposed by AMDGPU getAddrRanges()",
             name(), bar, base);

    const AddrRangeList host_ranges = hostPort.getAddrRanges();
    fatal_if(!rangesExpose(host_ranges, gpu_range),
             "%s BAR%u range %s is not visible through host_port",
             name(), bar, gpu_range.to_string().c_str());

    bars[bar].base = base;
    bars[bar].range = gpu_range;
    bars[bar].valid = true;
}

const HsakmtServerPlatform::BarState &
HsakmtServerPlatform::barState(uint32_t bar) const
{
    fatal_if(bar >= bars.size() || !bars[bar].valid,
             "%s BAR%u is not configured", name(), bar);
    return bars[bar];
}

Addr
HsakmtServerPlatform::barAddr(uint32_t bar, Addr offset, uint64_t size) const
{
    const BarState &state = barState(bar);
    fatal_if(size == 0, "%s BAR%u access has zero size", name(), bar);
    fatal_if(offset + size < offset,
             "%s BAR%u access wraps: offset %#lx size %" PRIu64,
             name(), bar, offset, size);

    const Addr addr = state.base + offset;
    fatal_if(addr < state.base || addr + size < addr ||
                 !state.range.contains(addr) ||
                 !state.range.contains(addr + size - 1),
             "%s BAR%u access out of range: offset %#lx size %" PRIu64
             " range %s",
             name(), bar, offset, size, state.range.to_string().c_str());
    return addr;
}

bool
HsakmtServerPlatform::rangesExpose(const AddrRangeList &ranges,
                                   const AddrRange &required) const
{
    return std::any_of(ranges.begin(), ranges.end(),
        [&required](const AddrRange &range) {
            return range.contains(required.start()) &&
                range.contains(required.end() - 1);
        });
}

AddrRange
HsakmtServerPlatform::findGpuRange(Addr base) const
{
    const AddrRangeList ranges = gpuDevice->getAddrRanges();
    for (const AddrRange &range : ranges) {
        if (range.valid() && range.start() == base) {
            return range;
        }
    }

    return AddrRange();
}

void
HsakmtServerPlatform::noteTimingResponse(PacketPtr)
{
    panic("%s does not issue timing PIO requests yet", name());
}

void
HsakmtServerPlatform::noteReqRetry()
{
    panic("%s does not issue timing PIO requests yet", name());
}

} // namespace hsa
} // namespace gem5
