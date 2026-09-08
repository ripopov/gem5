/*
 * Copyright (c) 2012, 2018 ARM Limited
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/simple/noncaching.hh"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "arch/generic/decoder.hh"
#include "cpu/utils.hh"
#include "sim/system.hh"

namespace gem5
{

namespace
{

/**
 * memcpy with a size only known at run time is a library call; backdoor
 * accesses are almost always a naturally sized scalar.
 */
inline void
copyBytes(void *dst, const void *src, size_t size)
{
    switch (size) {
      case 1: std::memcpy(dst, src, 1); break;
      case 2: std::memcpy(dst, src, 2); break;
      case 4: std::memcpy(dst, src, 4); break;
      case 8: std::memcpy(dst, src, 8); break;
      default: std::memcpy(dst, src, size); break;
    }
}

} // anonymous namespace

NonCachingSimpleCPU::NonCachingSimpleCPU(
        const BaseNonCachingSimpleCPUParams &p)
    : AtomicSimpleCPU(p)
{
    assert(p.numThreads == 1);
    fatal_if(!FullSystem && p.workload.size() != 1,
             "only one workload allowed");
}

void
NonCachingSimpleCPU::verifyMemoryMode() const
{
    if (!(system->isAtomicMode() && system->bypassCaches())) {
        fatal("The direct CPU requires the memory system to be in the "
              "'atomic_noncaching' mode.\n");
    }
}

uint8_t *
NonCachingSimpleCPU::hostAddr(BackdoorWindow &window, Addr addr,
                              unsigned size, bool write)
{
    if (addr < window.start || addr + size > window.end) {
        auto bd_it = memBackdoors.contains(RangeSize(addr, size));
        if (bd_it == memBackdoors.end())
            return nullptr;
        MemBackdoorPtr bd = bd_it->second;
        if (bd->range().interleaved()) {
            // Offsets are not linear in the address; resolve this one
            // access through the range and leave the window alone.
            if (write ? !bd->writeable() : !bd->readable())
                return nullptr;
            return bd->ptr() + bd->range().getOffset(addr);
        }
        window.set(bd);
    }
    if (write ? !window.writeable : !window.readable)
        return nullptr;
    return window.base + (addr - window.start);
}

bool
NonCachingSimpleCPU::tryBackdoorAccess(const PacketPtr &pkt)
{
    const RequestPtr &req = pkt->req;
    const bool read = pkt->cmd == MemCmd::ReadReq;
    const bool write = pkt->cmd == MemCmd::WriteReq;

    // Only plain loads and stores. LR/SC, atomics, swaps and masked
    // writes rely on the memory's own bookkeeping, and stores are kept
    // on the port when another hart could observe them there: its
    // reservations live in the memory's locked-address list.
    if (!(read || write) || req->isLLSC() || req->isAtomic() ||
        req->isSwap() || pkt->isMaskedWrite()) {
        return false;
    }
    if (write && !storesBypassPort())
        return false;

    const unsigned size = pkt->getSize();
    uint8_t *host = hostAddr(dataWindow, pkt->getAddr(), size, write);
    if (!host)
        return false;

    if (read) {
        copyBytes(pkt->getPtr<uint8_t>(), host, size);
    } else {
        copyBytes(host, pkt->getConstPtr<uint8_t>(), size);
    }
    pkt->makeResponse();
    return true;
}

bool
NonCachingSimpleCPU::storesBypassPort() const
{
    // Another hart's reservations live in the memory's locked-address
    // list, which only stores on the port maintain.
    return system->threads.size() == 1;
}

bool
NonCachingSimpleCPU::plainAccess(Addr addr, unsigned size,
                                 Request::Flags flags,
                                 const std::vector<bool> &byte_enable) const
{
    static constexpr Request::FlagsType special =
        Request::LLSC | Request::LOCKED_RMW | Request::MEM_SWAP |
        Request::MEM_SWAP_COND | Request::PREFETCH | Request::PF_EXCLUSIVE |
        Request::ATOMIC_RETURN_OP | Request::ATOMIC_NO_RETURN_OP |
        Request::STORE_NO_DATA | Request::CACHE_BLOCK_ZERO |
        Request::NO_ACCESS | Request::CLEAN | Request::INVALIDATE |
        Request::HTM_CMD;
    if (flags.isSet(special))
        return false;
    if (addrBlockOffset(addr, cacheLineSize()) + size > cacheLineSize())
        return false;
    return std::find(byte_enable.begin(), byte_enable.end(), false) ==
        byte_enable.end();
}

Fault
NonCachingSimpleCPU::readMem(Addr addr, uint8_t *data, unsigned size,
                             Request::Flags flags,
                             const std::vector<bool> &byte_enable)
{
    if (!plainAccess(addr, size, flags, byte_enable))
        return AtomicSimpleCPU::readMem(addr, data, size, flags, byte_enable);

    // Same steps as AtomicSimpleCPU::readMem() for its single-fragment,
    // unmasked, plain-load case, minus the fragment loop and the packet:
    // the value is copied straight out of the backing store.
    SimpleThread *thread = threadInfo[curThread]->thread;
    const RequestPtr &req = data_read_req;

    if (traceData)
        traceData->setMem(addr, size, flags);

    dcache_latency = 0;
    req->taskId(taskId());
    req->setVirt(addr, size, flags, dataRequestorId(),
                 thread->pcState().instAddr());
    req->setAllBytesEnabled();

    Fault fault = thread->mmu->translateAtomic(req, thread->getTC(),
                                               BaseMMU::Read);
    if (fault != NoFault)
        return fault;
    if (req->getFlags().isSet(Request::NO_ACCESS))
        return NoFault;

    const uint8_t *host = req->isLocalAccess() ? nullptr :
        hostAddr(dataWindow, req->getPaddr(), size, false);
    if (host) {
        copyBytes(data, host, size);
    } else {
        Packet pkt(req, MemCmd::ReadReq);
        pkt.dataStatic(data);
        if (req->isLocalAccess()) {
            dcache_latency += req->localAccessor(thread->getTC(), &pkt);
        } else {
            dcache_latency += sendPacket(dcachePort, &pkt);
        }
        panic_if(pkt.isError(), "Data fetch (%s) failed: %s",
                 pkt.getAddrRange().to_string(), pkt.print());
    }
    dcache_access = true;
    return NoFault;
}

Fault
NonCachingSimpleCPU::writeMem(uint8_t *data, unsigned size, Addr addr,
                              Request::Flags flags, uint64_t *res,
                              const std::vector<bool> &byte_enable)
{
    if (res || !data || !plainAccess(addr, size, flags, byte_enable)) {
        return AtomicSimpleCPU::writeMem(data, size, addr, flags, res,
                                         byte_enable);
    }

    // The plain-store counterpart of readMem() above.
    SimpleThread *thread = threadInfo[curThread]->thread;
    const RequestPtr &req = data_write_req;

    if (traceData)
        traceData->setMem(addr, size, flags);

    dcache_latency = 0;
    req->taskId(taskId());
    req->setVirt(addr, size, flags, dataRequestorId(),
                 thread->pcState().instAddr());
    req->setAllBytesEnabled();

    Fault fault = thread->mmu->translateAtomic(req, thread->getTC(),
                                               BaseMMU::Write);
    if (fault != NoFault)
        return fault;
    if (req->getFlags().isSet(Request::NO_ACCESS))
        return NoFault;

    uint8_t *host = (req->isLocalAccess() || !storesBypassPort()) ? nullptr :
        hostAddr(dataWindow, req->getPaddr(), size, true);
    if (host) {
        copyBytes(host, data, size);
    } else {
        Packet pkt(req, MemCmd::WriteReq);
        pkt.dataStatic(data);
        if (req->isLocalAccess()) {
            dcache_latency += req->localAccessor(thread->getTC(), &pkt);
        } else {
            dcache_latency += sendPacket(dcachePort, &pkt);
            // Notify other threads on this CPU of write
            threadSnoop(&pkt, curThread);
        }
        panic_if(pkt.isError(), "Data write (%s) failed: %s",
                 pkt.getAddrRange().to_string(), pkt.print());
    }
    dcache_access = true;
    return NoFault;
}

Tick
NonCachingSimpleCPU::sendPacket(RequestPort &port, const PacketPtr &pkt)
{
    if (&port == &dcachePort && tryBackdoorAccess(pkt))
        return 0;

    MemBackdoorPtr bd = nullptr;
    Tick latency = port.sendAtomicBackdoor(pkt, bd);

    // If the target gave us a backdoor for next time and we didn't
    // already have it, record it.
    if (bd && memBackdoors.insert(bd->range(), bd) != memBackdoors.end()) {
        // Install a callback to erase this backdoor if it goes away.
        auto callback = [this](const MemBackdoor &backdoor) {
                if (fetchWindow.backdoor == &backdoor)
                    fetchWindow.forget();
                if (dataWindow.backdoor == &backdoor)
                    dataWindow.forget();
                for (auto it = memBackdoors.begin();
                        it != memBackdoors.end(); it++) {
                    if (it->second == &backdoor) {
                        memBackdoors.erase(it);
                        return;
                    }
                }
                panic("Got invalidation for unknown memory backdoor.");
            };
        bd->addInvalidationCallback(callback);
    }
    return latency;
}

Tick
NonCachingSimpleCPU::fetchInstMem()
{
    const unsigned size = ifetch_req->getSize();
    const uint8_t *host =
        hostAddr(fetchWindow, ifetch_req->getPaddr(), size, false);
    if (!host)
        return AtomicSimpleCPU::fetchInstMem();

    auto &decoder = threadInfo[curThread]->thread->decoder;
    copyBytes(decoder->moreBytesPtr(), host, size);
    return 0;
}

} // namespace gem5
