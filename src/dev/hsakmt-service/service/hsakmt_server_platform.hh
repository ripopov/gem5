#ifndef __DEV_HSA_HSAKMT_SERVER_PLATFORM_HH__
#define __DEV_HSA_HSAKMT_SERVER_PLATFORM_HH__

#include <array>
#include <cstdint>
#include <vector>

#include "base/addr_range.hh"
#include "base/types.hh"
#include "dev/amdgpu/amdgpu_defines.hh"
#include "mem/port.hh"
#include "mem/request.hh"
#include "params/HsakmtServerPlatform.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class AMDGPUDevice;
class System;

namespace memory
{
class AbstractMemory;
}

namespace hsa
{

class HsakmtServerPlatform : public SimObject
{
  public:
    explicit HsakmtServerPlatform(const HsakmtServerPlatformParams &params);

    void init() override;
    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;

    void enumerateGpu(Addr framebuffer_base, Addr doorbell_base,
                      Addr mmio_base);
    void installDeviceMemoryRouting(RequestorID requestor_id);
    void installDeviceMemoryRouting(const SimObject *requestor);

    void framebufferWrite(Addr offset, uint64_t value, unsigned size);
    uint64_t framebufferRead(Addr offset, unsigned size) const;
    void framebufferWriteBlob(Addr offset, const void *data, uint64_t size);
    void framebufferReadBlob(Addr offset, void *data, uint64_t size) const;
    void framebufferSetupWrite(Addr offset, uint64_t value, unsigned size);
    void framebufferSetupWriteBlob(Addr offset, const void *data,
                                   uint64_t size);
    void doorbellWrite(Addr offset, uint64_t value);
    void mmioWrite32(Addr offset, uint32_t value);
    uint32_t mmioRead32(Addr offset) const;

  private:
    class HostRequestPort : public RequestPort
    {
      public:
        explicit HostRequestPort(HsakmtServerPlatform &platform);

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;

      private:
        HsakmtServerPlatform &platform;
    };

    struct BarState
    {
        Addr base = 0;
        AddrRange range;
        bool valid = false;
    };

    Addr configAddr(uint8_t bus, uint8_t dev, uint8_t func,
                    uint16_t offset) const;
    uint64_t readPio(Addr addr, unsigned size) const;
    void writePio(Addr addr, uint64_t value, unsigned size) const;
    void writePioBlob(Addr addr, const void *data, uint64_t size) const;
    void readPioBlob(Addr addr, void *data, uint64_t size) const;
    uint64_t readConfig(uint16_t offset, unsigned size) const;
    void writeConfig(uint16_t offset, uint64_t value, unsigned size) const;
    void configureBar(uint32_t bar, Addr base);
    void cacheAndValidateBar(uint32_t bar, Addr base);
    const BarState &barState(uint32_t bar) const;
    Addr barAddr(uint32_t bar, Addr offset, uint64_t size) const;
    bool rangesExpose(const AddrRangeList &ranges,
                      const AddrRange &required) const;
    AddrRange findGpuRange(Addr base) const;
    void noteTimingResponse(PacketPtr pkt);
    void noteReqRetry();

    mutable HostRequestPort hostPort;
    System *const system;
    AMDGPUDevice *const gpuDevice;
    const std::vector<memory::AbstractMemory *> gpuMemoryBackends;
    const Addr pciConfigBase;
    const uint8_t pciConfigDeviceBits;
    const uint8_t gpuPciBus;
    const RequestorID requestorId;
    std::array<BarState, 6> bars;
};

} // namespace hsa
} // namespace gem5

#endif // __DEV_HSA_HSAKMT_SERVER_PLATFORM_HH__
