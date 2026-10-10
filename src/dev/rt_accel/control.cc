/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "dev/rt_accel/control.hh"

#include <algorithm>
#include <cstring>

#include "cpu/base.hh"
#include "mem/packet_access.hh"
#include "sim/system.hh"

namespace gem5
{
namespace rt_accel
{

Control::Control(const Params &p)
    : ClockedObject(p),
      pio(*this),
      dma(*this, ".dma"),
      dmaWrite(*this, ".dma_write"),
      dmaClean(*this, ".dma_clean"),
      cpu(*p.cpu),
      registers(p.registers),
      dataTcmRange(p.data_tcm_range),
      hostRange(p.host_range),
      requestor(p.system->getRequestorId(this)),
      maxOutstanding(p.max_outstanding),
      coherentDma(p.coherent_dma),
      stats(*this),
      stepEvent([this] { send(); }, name() + ".dma_step"),
      cleanEvent([this] { sendClean(); }, name() + ".dma_clean_step")
{
    fatal_if(!maxOutstanding, "DMA requires at least one chunk credit");
}

Control::DmaStats::DmaStats(Control &owner)
    : statistics::Group(&owner),
      ADD_STAT(readRequests, statistics::units::Count::get(),
               "Accepted DMA reads"),
      ADD_STAT(writeRequests, statistics::units::Count::get(),
               "Accepted DMA writes, excluding MSI"),
      ADD_STAT(cleanRequests, statistics::units::Count::get(),
               "Accepted Data TCM clean/invalidate requests"),
      ADD_STAT(bytesCopied, statistics::units::Byte::get(),
               "Bytes with successful destination responses"),
      ADD_STAT(busyTicks, statistics::units::Tick::get(),
               "DMA start to final response ticks, including pipeline drain"),
      ADD_STAT(readRetries, statistics::units::Count::get(),
               "Read port backpressure rejections"),
      ADD_STAT(writeRetries, statistics::units::Count::get(),
               "Write port backpressure rejections"),
      ADD_STAT(peakChunks, statistics::units::Count::get(),
               "Maximum simultaneously live read/write chunks"),
      ADD_STAT(errors, statistics::units::Count::get(), "Failed DMA commands")
{}
void
Control::init()
{
    ClockedObject::init();
    fatal_if(!pio.isConnected() || !dma.isConnected() ||
                 !dmaWrite.isConnected() ||
                 (coherentDma && !dmaClean.isConnected()),
             "Control requires register and local DMA connections");
    pio.sendRangeChange();
}
Port &
Control::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "pio") {
        return pio;
    }
    if (if_name == "dma") {
        return dma;
    }
    if (if_name == "dma_write") {
        return dmaWrite;
    }
    if (if_name == "dma_clean") {
        return dmaClean;
    }
    return ClockedObject::getPort(if_name, idx);
}
Control::MasterPort::MasterPort(Control &o, const std::string &suffix)
    : RequestPort(o.name() + suffix), owner(o)
{}

bool
Control::validRange(Addr addr, uint32_t size) const
{
    return size && addr <= MaxAddr - (size - 1) &&
           (RangeSize(addr, size).isSubset(dataTcmRange) ||
            RangeSize(addr, size).isSubset(hostRange));
}

void
Control::access(PacketPtr pkt)
{
    const Addr offset = pkt->getAddr() - registers.start();
    bool ok = pkt->getAddr() % pkt->getSize() == 0;
    uint64_t value = 0;
    if (pkt->isRead()) {
        switch (offset) {
            case 0:
                value = jobAddress;
                ok &= pkt->getSize() == 8;
                break;
            case 8:
                value = msiAddress;
                ok &= pkt->getSize() == 8;
                break;
            case 16:
                value = msiData;
                ok &= pkt->getSize() == 4;
                break;
            case 20:
                value = status;
                ok &= pkt->getSize() == 4;
                break;
            case 44:
                value = completionPending;
                ok &= pkt->getSize() == 4;
                break;
            case 32:
                value = jobAddress;
                ok &= pkt->getSize() == 8;
                break;
            case 48:
                value = source;
                ok &= pkt->getSize() == 8;
                break;
            case 56:
                value = destination;
                ok &= pkt->getSize() == 8;
                break;
            case 64:
                value = length;
                ok &= pkt->getSize() == 4;
                break;
            case 72:
                value = dmaStatus;
                ok &= pkt->getSize() == 4;
                break;
            default:
                ok = false;
        }
        if (ok) {
            if (pkt->getSize() == 8) {
                pkt->setLE<uint64_t>(value);
            } else {
                pkt->setLE<uint32_t>(value);
            }
        }
    } else if (pkt->isWrite()) {
        if (pkt->getSize() == 8) {
            value = pkt->getLE<uint64_t>();
        } else if (pkt->getSize() == 4) {
            value = pkt->getLE<uint32_t>();
        } else {
            ok = false;
        }
        switch (offset) {
            case 0:
                ok &= pkt->getSize() == 8 && status != 1 &&
                      !completionPending && msiAddress && !(msiAddress % 4);
                if (ok) {
                    jobAddress = value;
                    status = 1;
                    cpu.postInterrupt(0, 11, 0);
                }
                break;
            case 8:
                ok &= pkt->getSize() == 8 && status != 1 && !completionPending;
                if (ok) {
                    msiAddress = value;
                }
                break;
            case 16:
                ok &= pkt->getSize() == 4 && status != 1 && !completionPending;
                if (ok) {
                    msiData = value;
                }
                break;
            case 24:
                ok &= pkt->getSize() == 4 && value == 1 && status == 1;
                if (ok) {
                    cpu.clearInterrupt(0, 11, 0);
                }
                break;
            case 40:
                ok &= pkt->getSize() == 4 && status == 1 && dmaStatus != 1;
                if (ok) {
                    finish(value);
                }
                break;
            case 48:
                ok &= pkt->getSize() == 8 && dmaStatus != 1;
                if (ok) {
                    source = value;
                }
                break;
            case 56:
                ok &= pkt->getSize() == 8 && dmaStatus != 1;
                if (ok) {
                    destination = value;
                }
                break;
            case 64:
                ok &= pkt->getSize() == 4 && dmaStatus != 1;
                if (ok) {
                    length = value;
                }
                break;
            case 68:
                ok &= pkt->getSize() == 4 && value == 1 && status == 1 &&
                      dmaStatus != 1;
                if (ok) {
                    if (!validRange(source, length) ||
                        !validRange(destination, length) ||
                        (source != destination &&
                         (source < destination
                              ? destination - source
                              : source - destination) < length)) {
                        dmaStatus = 3;
                        ++stats.errors;
                    } else {
                        dmaStatus = 1;
                        issued = copied = 0;
                        dmaError = false;
                        dmaStart = curTick();
                        kick();
                    }
                }
                break;
            default:
                ok = false;
        }
    } else {
        ok = false;
    }
    pkt->makeResponse();
    if (!ok) {
        pkt->setBadAddress();
    }
}

void
Control::readChunk()
{
    const unsigned size =
        std::min({length - issued, 64 - unsigned((source + issued) % 64),
                  64 - unsigned((destination + issued) % 64)});
    auto req = std::make_shared<Request>(source + issued, size,
                                         Request::UNCACHEABLE, requestor);
    dma.pending = new Packet(req, MemCmd::ReadReq);
    dma.pending->allocate();
    dma.pending->pushSenderState(new ChunkState(Phase::Read, issued));
    issued += size;
    ++activeChunks;
    stats.peakChunks =
        std::max(stats.peakChunks.value(), double(activeChunks));
}

void
Control::kick()
{
    // Each independent port accepts at most one request per device cycle.
    // Retries retain the same packet and cannot bypass this issue limit.
    Tick when = MaxTick;
    if (!dma.blocked &&
        (dma.pending || (dmaStatus == 1 && !dmaError && issued < length &&
                         activeChunks < maxOutstanding))) {
        when = std::min(when, std::max(clockEdge(Cycles(1)), dma.nextIssue));
    }
    if (!dmaWrite.blocked && (dmaWrite.pending || !writes.empty())) {
        when =
            std::min(when, std::max(clockEdge(Cycles(1)), dmaWrite.nextIssue));
    }
    if (!dmaClean.blocked && (dmaClean.pending || !cleans.empty())) {
        // Coherence retries use the router's callback timing. They must not
        // compete with every data write at the same device clock edge.
        if (!cleanEvent.scheduled()) {
            schedule(cleanEvent, std::max(curTick(), dmaClean.nextIssue));
        }
    }
    if (when != MaxTick && !stepEvent.scheduled()) {
        schedule(stepEvent, when);
    }
}

void
Control::sendPort(MasterPort &port)
{
    if (!port.pending || port.blocked || curTick() < port.nextIssue) {
        return;
    }
    auto *state = static_cast<ChunkState *>(port.pending->senderState);
    const Phase phase = state->phase;
    if (port.sendTimingReq(port.pending)) {
        // Ownership has transferred; do not inspect the packet after send.
        port.pending = nullptr;
        port.nextIssue = &port == &dmaClean ? curTick() + clockPeriod()
                                            : clockEdge(Cycles(1));
        if (phase == Phase::Read) {
            ++stats.readRequests;
        } else if (phase == Phase::Write) {
            ++stats.writeRequests;
        } else if (phase == Phase::Clean) {
            ++stats.cleanRequests;
        }
    } else {
        port.blocked = true;
        if (&port == &dma) {
            ++stats.readRetries;
        } else {
            ++stats.writeRetries;
        }
    }
}

void
Control::sendClean()
{
    if (!dmaClean.pending && !cleans.empty()) {
        dmaClean.pending = cleans.front();
        cleans.pop_front();
    }
    sendPort(dmaClean);
    kick();
}

void
Control::send()
{
    if (!dmaWrite.pending && !writes.empty()) {
        dmaWrite.pending = writes.front();
        writes.pop_front();
    }
    if (!dma.pending && dmaStatus == 1 && !dmaError && issued < length &&
        activeChunks < maxOutstanding) {
        readChunk();
    }
    sendPort(dmaWrite);
    sendPort(dma);
    kick();
}

void
Control::MasterPort::recvReqRetry()
{
    blocked = false;
    // The port callback may be at a router subcycle. Issue on a DMA edge.
    owner.kick();
}

bool
Control::MasterPort::recvTimingResp(PacketPtr pkt)
{
    const Tick when = curTick() + pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;
    owner.schedule(
        new EventFunctionWrapper([this, pkt] { owner.response(pkt); },
                                 owner.name() + ".dma_response", true),
        when);
    return true;
}

void
Control::response(PacketPtr pkt)
{
    auto *state = static_cast<ChunkState *>(pkt->popSenderState());
    const Phase phase = state->phase;
    const uint32_t offset = state->offset;
    PacketPtr write_after_clean = state->write;
    state->write = nullptr;
    delete state;
    if (phase == Phase::MSI) {
        completionPending = false;
        if (pkt->isError()) {
            status = 3;
        }
    } else {
        if (pkt->isError()) {
            dmaError = true;
        }
        if (phase == Phase::Read && !dmaError) {
            auto req =
                std::make_shared<Request>(destination + offset, pkt->getSize(),
                                          Request::UNCACHEABLE, requestor);
            auto *write = new Packet(req, MemCmd::WriteReq);
            write->allocate();
            std::memcpy(write->getPtr<uint8_t>(), pkt->getConstPtr<uint8_t>(),
                        pkt->getSize());
            write->pushSenderState(new ChunkState(Phase::Write, offset));
            if (coherentDma && dataTcmRange.contains(destination + offset)) {
                // Preserve dirty bytes outside a partial DMA write, then
                // invalidate the line before updating its backing TCM.
                auto clean_req = std::make_shared<Request>(
                    (destination + offset) & ~Addr(63), 64,
                    Request::CLEAN | Request::INVALIDATE | Request::DST_POC,
                    requestor);
                auto *clean = new Packet(clean_req, MemCmd::CleanInvalidReq);
                auto *clean_state = new ChunkState(Phase::Clean, offset);
                clean_state->write = write;
                clean->pushSenderState(clean_state);
                cleans.push_back(clean);
            } else {
                writes.push_back(write);
            }
        } else if (phase == Phase::Clean && !dmaError) {
            writes.push_back(write_after_clean);
            write_after_clean = nullptr;
        } else {
            --activeChunks;
            if (phase == Phase::Write && !pkt->isError()) {
                copied += pkt->getSize();
                stats.bytesCopied += pkt->getSize();
            }
        }
        if (dmaError) {
            // Cancel only packets we still own. Accepted transactions drain
            // before exposing error status or allowing an MSI/new command.
            auto cancel = [this](PacketPtr cancelled) {
                delete cancelled->popSenderState();
                delete cancelled;
                --activeChunks;
            };
            for (auto *write : writes) {
                cancel(write);
            }
            writes.clear();
            for (auto *clean : cleans) {
                cancel(clean);
            }
            cleans.clear();
            for (auto *port : {&dma, &dmaWrite, &dmaClean}) {
                if (port->pending) {
                    cancel(port->pending);
                    port->pending = nullptr;
                }
            }
        }
        checkCompletion();
    }
    if (write_after_clean) {
        delete write_after_clean->popSenderState();
        delete write_after_clean;
    }
    delete pkt;
    kick();
    if (drainState() == DrainState::Draining && dmaStatus != 1 &&
        !completionPending) {
        signalDrainDone();
    }
}

void
Control::checkCompletion()
{
    if (!activeChunks && (dmaError || copied == length)) {
        dmaStatus = dmaError ? 3 : 2;
        stats.busyTicks += curTick() - dmaStart;
        if (dmaError) {
            ++stats.errors;
        }
    }
}

void
Control::finish(uint32_t code)
{
    status = code || dmaStatus == 3 ? 3 : 2;
    completionPending = true;
    auto req = std::make_shared<Request>(msiAddress, 4, Request::UNCACHEABLE,
                                         requestor);
    auto *pkt = new Packet(req, MemCmd::WriteReq);
    pkt->allocate();
    pkt->setLE<uint32_t>(msiData);
    pkt->pushSenderState(new ChunkState(Phase::MSI, 0));
    writes.push_back(pkt);
    kick();
}

DrainState
Control::drain()
{
    return dmaStatus == 1 || completionPending ? DrainState::Draining
                                               : DrainState::Drained;
}

} // namespace rt_accel
} // namespace gem5
