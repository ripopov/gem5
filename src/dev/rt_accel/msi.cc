/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "dev/rt_accel/msi.hh"

#include "cpu/base.hh"
#include "dev/rt_accel/trace.hh"
#include "mem/packet_access.hh"

namespace gem5
{
namespace rt_accel
{
MSITarget::MSITarget(const Params &p)
    : ClockedObject(p),
      trace(p.trace_file),
      pio(*this),
      cpu(*p.cpu),
      registers(p.registers),
      acceleratorCores(p.accelerator_cores)
{
    fatal_if(!acceleratorCores || acceleratorCores > 32 ||
                 registers.size() < 0x10c,
             "MSI target requires 1-32 cores and discovery registers");
}
void
MSITarget::init()
{
    ClockedObject::init();
    fatal_if(!pio.isConnected(), "MSI target requires a host bus connection");
    pio.sendRangeChange();
}
Port &
MSITarget::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "pio") {
        return pio;
    }
    return ClockedObject::getPort(if_name, idx);
}
void
MSITarget::access(PacketPtr pkt)
{
    tracePacket(trace, "msi_access", pkt);
    const Addr offset = pkt->getAddr() - registers.start();
    bool ok = pkt->getSize() == 4 && offset % 4 == 0;
    if (ok && pkt->isWrite() && offset < 4 * acceleratorCores) {
        ok = pkt->getLE<uint32_t>() == offset / 4 + 1;
        if (ok) {
            pending |= 1U << (offset / 4);
            cpu.postInterrupt(0, 11, 0);
        }
    } else if (ok && pkt->isRead() && offset == 0x100) {
        pkt->setLE<uint32_t>(pending);
    } else if (ok && pkt->isWrite() && offset == 0x104) {
        pending &= ~pkt->getLE<uint32_t>();
        if (!pending) {
            cpu.clearInterrupt(0, 11, 0);
        }
    } else if (ok && pkt->isRead() && offset == 0x108) {
        pkt->setLE<uint32_t>(acceleratorCores);
    } else {
        ok = false;
    }
    pkt->makeResponse();
    tracePacket(trace, "msi_result", pkt);
    if (!ok) {
        pkt->setBadAddress();
    }
}
} // namespace rt_accel
} // namespace gem5
