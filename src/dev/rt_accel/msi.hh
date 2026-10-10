/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __DEV_RT_ACCEL_MSI_HH__
#define __DEV_RT_ACCEL_MSI_HH__

#include <fstream>

#include "dev/rt_accel/register_port.hh"
#include "params/RTAccelMSI.hh"
#include "sim/clocked_object.hh"

namespace gem5
{
class BaseCPU;
namespace rt_accel
{
class MSITarget : public ClockedObject
{
  private:
    std::ofstream trace;
    RegisterPort<MSITarget> pio;
    BaseCPU &cpu;
    const AddrRange registers;
    const unsigned acceleratorCores;
    uint32_t pending = 0;

  public:
    using Params = RTAccelMSIParams;
    explicit MSITarget(const Params &p);
    void init() override;
    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    const AddrRange &
    range() const
    {
        return registers;
    }
    void access(PacketPtr pkt);
};
} // namespace rt_accel
} // namespace gem5
#endif
