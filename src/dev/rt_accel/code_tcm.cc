/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "dev/rt_accel/code_tcm.hh"

namespace gem5
{
namespace rt_accel
{

CodeTCM::CodeTCM(const Params &p)
    : AbstractMemory(p),
      latency(p.latency),
      instruction(name() + ".instruction", *this),
      constants(name() + ".constants", *this)
{}

void
CodeTCM::init()
{
    AbstractMemory::init();
    fatal_if(!instruction.isConnected() || !constants.isConnected(),
             "Code TCM requires instruction and constant-read connections");
    instruction.sendRangeChange();
    constants.sendRangeChange();
}

Port &
CodeTCM::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "instruction") {
        return instruction;
    }
    if (if_name == "constants") {
        return constants;
    }
    return AbstractMemory::getPort(if_name, idx);
}

CodeTCM::ReadPort::ReadPort(const std::string &name, CodeTCM &memory)
    : SimpleTimingPort(name, &memory), memory(memory)
{}

AddrRangeList
CodeTCM::ReadPort::getAddrRanges() const
{
    return {memory.getAddrRange()};
}

Tick
CodeTCM::ReadPort::recvAtomic(PacketPtr pkt)
{
    const Tick delay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;
    if (pkt->isCleanEviction()) {
        // The read-only L1I may discard a line. No response or write is
        // needed.
        return delay;
    }
    if (pkt->isRead() && !pkt->isWrite()) {
        memory.access(pkt);
    } else {
        // Runtime code and constant data are immutable, including atomics.
        pkt->makeResponse();
        pkt->setBadCommand();
    }
    return delay + memory.latency;
}

void
CodeTCM::ReadPort::recvFunctional(PacketPtr pkt)
{
    // Initialization/debug accesses may write the single shared image.
    memory.functionalAccess(pkt);
}

} // namespace rt_accel
} // namespace gem5
