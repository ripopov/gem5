/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __DEV_RT_ACCEL_TRACE_HH__
#define __DEV_RT_ACCEL_TRACE_HH__

#include <iomanip>
#include <ostream>

#include "mem/packet.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
namespace rt_accel
{
inline void
tracePacket(std::ostream &stream, const char *stage, PacketPtr pkt)
{
    if (!stream) {
        return;
    }
    stream << std::dec << curTick() << ' ' << stage << ' '
           << pkt->cmd.toString() << ' ' << std::hex << pkt->getAddr() << ' '
           << std::dec << pkt->getSize() << ' ';
    if (!pkt->isError() &&
        (pkt->isWrite() || (pkt->isRead() && pkt->isResponse()))) {
        for (unsigned i = 0; i < pkt->getSize(); ++i) {
            stream << std::hex << std::setw(2) << std::setfill('0')
                   << unsigned(pkt->getConstPtr<uint8_t>()[i]);
        }
    } else {
        stream << '-';
    }
    stream << '\n';
    stream.flush();
}
} // namespace rt_accel
} // namespace gem5
#endif
