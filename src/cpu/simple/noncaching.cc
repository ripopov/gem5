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
#include "arch/generic/mmu.hh"
#include "arch/generic/tlb.hh"
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
    : AtomicSimpleCPU(p), directMemory(p.direct_memory)
{
    assert(p.numThreads == 1);
    fatal_if(!FullSystem && p.workload.size() != 1,
             "only one workload allowed");
    fatal_if(!directMemory.empty() &&
                 (!FullSystem || simulate_data_stalls || simulate_inst_stalls),
             "Direct memory requires full-system execution without stalls");
}

bool
NonCachingSimpleCPU::directAccessActive() const
{
    return !switchedOut() && system->isAtomicMode() && system->bypassCaches();
}

void
NonCachingSimpleCPU::rebuildDirectMappings()
{
    fetchPage = FetchPage();
    fetchWindow.forget();
    dataWindow.forget();
    directMappings.clear();
    if (directMemory.empty()) {
        return;
    }
    for (auto *mem : directMemory) {
        fatal_if(!mem, "Direct memory owners must not be null");
        fatal_if(std::count(directMemory.begin(), directMemory.end(), mem) !=
                     1,
                 "Duplicate direct memory owner: %s", mem->name());
        fatal_if(mem->system() != system || mem->eventQueue() != eventQueue(),
                 "Direct memory must share the CPU's system and event queue");
    }

    // Use the same allocations as KVM, but require explicit authorization
    // of every RAM owner. Never map a separate Ruby reference image.
    for (const auto &store : system->getPhysMem().getBackingStore()) {
        if (!store.inAddrMap || !store.kvmMap || !store.pmem ||
            store.range.interleaved() || store.range.isSparse()) {
            continue;
        }
        DirectMapping mapping{
            store.range.start(), store.range.end(), store.pmem, {}, true};
        Addr covered = 0;
        for (auto *mem : directMemory) {
            const auto range = mem->getAddrRange();
            if (!mem->isInAddrMap() || !mem->isKvmMap() || mem->isNull() ||
                range.isSparse() || range.start() < mapping.start ||
                range.end() > mapping.end) {
                continue;
            }
            mapping.owners.push_back(mem);
            mapping.writeable &= mem->params().writeable;
            covered += range.size();
        }
        if (covered == store.range.size()) {
            directMappings.push_back(std::move(mapping));
        }
    }
    fatal_if(directMappings.empty(), "No eligible direct backing store");
}

void
NonCachingSimpleCPU::startup()
{
    AtomicSimpleCPU::startup();
    rebuildDirectMappings();
}

void
NonCachingSimpleCPU::switchOut()
{
    AtomicSimpleCPU::switchOut();
    fetchPage = FetchPage();
    fetchWindow.forget();
    dataWindow.forget();
    directMappings.clear();
}

void
NonCachingSimpleCPU::takeOverFrom(BaseCPU *old_cpu)
{
    AtomicSimpleCPU::takeOverFrom(old_cpu);
    rebuildDirectMappings();
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
    if (!directMemory.empty()) {
        if (!directAccessActive()) {
            return nullptr;
        }
        if (!window.direct || addr < window.start || addr >= window.end ||
            size > window.end - addr) {
            window.forget();
            for (const auto &mapping : directMappings) {
                if (addr >= mapping.start && addr < mapping.end &&
                    size <= mapping.end - addr) {
                    window.direct = &mapping;
                    window.start = mapping.start;
                    window.end = mapping.end;
                    window.base = mapping.base;
                    break;
                }
            }
        }
        if (!window.direct) {
            return nullptr;
        }
        if (write) {
            if (!storesBypassPort() || !window.direct->writeable) {
                return nullptr;
            }
            // A constant-time empty check per owner includes restored locks
            // and every interleaved channel, without caching stale
            // eligibility.
            for (auto *mem : window.direct->owners) {
                if (!mem->getLockedAddrList().empty()) {
                    return nullptr;
                }
            }
        }
        return window.base + (addr - window.start);
    }
    if (addr < window.start || addr >= window.end ||
        size > window.end - addr) {
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
        req->isSwap() || req->isUncacheable() || req->isStrictlyOrdered() ||
        pkt->isMaskedWrite()) {
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

    const uint8_t *host =
        (req->isLocalAccess() || req->isUncacheable() ||
         req->isStrictlyOrdered())
            ? nullptr
            : hostAddr(dataWindow, req->getPaddr(), size, false);
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

    uint8_t *host = (req->isLocalAccess() || req->isUncacheable() ||
                     req->isStrictlyOrdered() || !storesBypassPort())
                        ? nullptr
                        : hostAddr(dataWindow, req->getPaddr(), size, true);
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
    if (!directMemory.empty()) {
        return port.sendAtomic(pkt);
    }

    MemBackdoorPtr bd = nullptr;
    Tick latency = port.sendAtomicBackdoor(pkt, bd);

    // If the target gave us a backdoor for next time and we didn't
    // already have it, record it.
    if (bd && memBackdoors.insert(bd->range(), bd) != memBackdoors.end()) {
        // Install a callback to erase this backdoor if it goes away.
        auto callback = [this](const MemBackdoor &backdoor) {
                if (fetchWindow.backdoor == &backdoor)
                    fetchWindow.forget();
                if (fetchPage.backdoor == &backdoor)
                    fetchPage = FetchPage();
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

Fault
NonCachingSimpleCPU::fetchInstruction(Tick &latency)
{
    SimpleExecContext &t_info = *threadInfo[curThread];
    SimpleThread *thread = t_info.thread;
    auto &decoder = thread->decoder;
    const Addr fetch_pc =
        (thread->pcState().instAddr() & decoder->pcMask()) +
        t_info.fetchOffset;
    const unsigned size = decoder->moreBytesSize();
    BaseTLB *itb = thread->mmu->itb;

    if ((directMemory.empty() || directAccessActive()) &&
        fetch_pc >= fetchPage.vpage &&
        fetch_pc - fetchPage.vpage < fetchPage.size &&
        size <= fetchPage.size - (fetch_pc - fetchPage.vpage) &&
        itb->translationEpoch(thread->getTC()) == fetchPage.epoch) {
        copyBytes(decoder->moreBytesPtr(),
                  fetchPage.host + (fetch_pc - fetchPage.vpage), size);
        latency = 0;
        return NoFault;
    }

    Fault fault = AtomicSimpleCPU::fetchInstruction(latency);
    if (fault != NoFault)
        return fault;

    // Cache the page if translation is uniform and one mapping covers it.
    Addr vpage, ppage, page_size;
    if (!ifetch_req->isUncacheable() && !ifetch_req->isStrictlyOrdered() &&
        !ifetch_req->isLocalAccess() &&
        itb->stableFetchPage(thread->getTC(), fetch_pc, vpage, ppage,
                             page_size)) {
        const uint8_t *host = hostAddr(fetchWindow, ppage, page_size, false);
        if (host) {
            fetchPage.vpage = vpage;
            fetchPage.size = page_size;
            fetchPage.epoch = itb->translationEpoch(thread->getTC());
            fetchPage.host = host;
            fetchPage.backdoor = fetchWindow.backdoor;
        }
    }
    return NoFault;
}

Tick
NonCachingSimpleCPU::fetchInstMem()
{
    if (ifetch_req->isUncacheable() || ifetch_req->isStrictlyOrdered() ||
        ifetch_req->isLocalAccess()) {
        return AtomicSimpleCPU::fetchInstMem();
    }
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
