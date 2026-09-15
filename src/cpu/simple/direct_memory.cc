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

#include "cpu/simple/direct_memory.hh"

#include <algorithm>
#include <cstring>

#include "arch/generic/decoder.hh"
#include "arch/generic/mmu.hh"
#include "arch/generic/tlb.hh"
#include "cpu/utils.hh"
#include "mem/abstract_mem.hh"
#include "sim/system.hh"

namespace gem5
{

namespace
{

/**
 * memcpy with a size only known at run time is a library call; direct RAM
 * accesses are almost always a naturally sized scalar.
 */
inline void
copyBytes(void *dst, const void *src, size_t size)
{
    switch (size) {
        case 1:
            std::memcpy(dst, src, 1);
            break;
        case 2:
            std::memcpy(dst, src, 2);
            break;
        case 4:
            std::memcpy(dst, src, 4);
            break;
        case 8:
            std::memcpy(dst, src, 8);
            break;
        default:
            std::memcpy(dst, src, size);
            break;
    }
}

} // anonymous namespace

DirectMemorySimpleCPU::DirectMemorySimpleCPU(
    const BaseDirectMemorySimpleCPUParams &p)
    : AtomicSimpleCPU(p)
{
    fatal_if(p.numThreads != 1, "Direct memory requires one thread per CPU");
    fatal_if(!FullSystem || simulate_data_stalls || simulate_inst_stalls,
             "Direct memory requires full-system execution without stalls");
}

bool
DirectMemorySimpleCPU::directAccessActive() const
{
    return !switchedOut() && system->isAtomicMode() && system->bypassCaches();
}

void
DirectMemorySimpleCPU::initDirectAccess()
{
    fetchPage = FetchPage();
    fetchStore = dataStore = nullptr;
    bool found = false;
    for (const auto &store : system->getPhysMem().getBackingStore()) {
        if (!store.isDirectAccessible()) {
            continue;
        }
        for (const auto *mem : store.owners) {
            fatal_if(mem->system() != system ||
                         mem->eventQueue() != eventQueue(),
                     "Direct memory must share the CPU's system and event "
                     "queue");
        }
        found = true;
    }
    fatal_if(!found, "No eligible direct backing store");
}

void
DirectMemorySimpleCPU::startup()
{
    AtomicSimpleCPU::startup();
    initDirectAccess();
}

void
DirectMemorySimpleCPU::switchOut()
{
    AtomicSimpleCPU::switchOut();
    fetchPage = FetchPage();
    fetchStore = dataStore = nullptr;
}

void
DirectMemorySimpleCPU::takeOverFrom(BaseCPU *old_cpu)
{
    AtomicSimpleCPU::takeOverFrom(old_cpu);
    initDirectAccess();
}

void
DirectMemorySimpleCPU::verifyMemoryMode() const
{
    if (!(system->isAtomicMode() && system->bypassCaches())) {
        fatal("The direct CPU requires the memory system to be in the "
              "'atomic_noncaching' mode.\n");
    }
}

uint8_t *
DirectMemorySimpleCPU::hostAddr(const memory::BackingStoreEntry *&store,
                                Addr addr, unsigned size, bool write)
{
    if (!directAccessActive()) {
        return nullptr;
    }
    const auto contains = [addr, size](const auto &entry) {
        return addr >= entry.range.start() && addr < entry.range.end() &&
               size <= entry.range.end() - addr;
    };
    if (!store || !contains(*store)) {
        store = nullptr;
        for (const auto &entry : system->getPhysMem().getBackingStore()) {
            if (contains(entry) && entry.isDirectAccessible()) {
                store = &entry;
                break;
            }
        }
    }
    if (!store ||
        (write && (!storesBypassPort() || !store->canDirectWrite()))) {
        return nullptr;
    }
    return store->pmem + (addr - store->range.start());
}

bool
DirectMemorySimpleCPU::tryDirectAccess(const PacketPtr &pkt)
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
    if (write && !storesBypassPort()) {
        return false;
    }

    const unsigned size = pkt->getSize();
    uint8_t *host = hostAddr(dataStore, pkt->getAddr(), size, write);
    if (!host) {
        return false;
    }

    if (read) {
        copyBytes(pkt->getPtr<uint8_t>(), host, size);
    } else {
        copyBytes(host, pkt->getConstPtr<uint8_t>(), size);
    }
    pkt->makeResponse();
    return true;
}

bool
DirectMemorySimpleCPU::storesBypassPort() const
{
    // Another hart's reservations live in the memory's locked-address
    // list, which only stores on the port maintain.
    return system->threads.size() == 1;
}

bool
DirectMemorySimpleCPU::plainAccess(Addr addr, unsigned size,
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
    if (flags.isSet(special)) {
        return false;
    }
    if (addrBlockOffset(addr, cacheLineSize()) + size > cacheLineSize()) {
        return false;
    }
    return std::find(byte_enable.begin(), byte_enable.end(), false) ==
           byte_enable.end();
}

Fault
DirectMemorySimpleCPU::readMem(Addr addr, uint8_t *data, unsigned size,
                               Request::Flags flags,
                               const std::vector<bool> &byte_enable)
{
    if (!plainAccess(addr, size, flags, byte_enable)) {
        return AtomicSimpleCPU::readMem(addr, data, size, flags, byte_enable);
    }

    // Same steps as AtomicSimpleCPU::readMem() for its single-fragment,
    // unmasked, plain-load case, minus the fragment loop and the packet:
    // the value is copied straight out of the backing store.
    SimpleThread *thread = threadInfo[curThread]->thread;
    const RequestPtr &req = data_read_req;

    if (traceData) {
        traceData->setMem(addr, size, flags);
    }

    dcache_latency = 0;
    req->taskId(taskId());
    req->setVirt(addr, size, flags, dataRequestorId(),
                 thread->pcState().instAddr());
    req->setAllBytesEnabled();

    Fault fault =
        thread->mmu->translateAtomic(req, thread->getTC(), BaseMMU::Read);
    if (fault != NoFault) {
        return fault;
    }
    if (req->getFlags().isSet(Request::NO_ACCESS)) {
        return NoFault;
    }

    const uint8_t *host =
        (req->isLocalAccess() || req->isUncacheable() ||
         req->isStrictlyOrdered())
            ? nullptr
            : hostAddr(dataStore, req->getPaddr(), size, false);
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
DirectMemorySimpleCPU::writeMem(uint8_t *data, unsigned size, Addr addr,
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

    if (traceData) {
        traceData->setMem(addr, size, flags);
    }

    dcache_latency = 0;
    req->taskId(taskId());
    req->setVirt(addr, size, flags, dataRequestorId(),
                 thread->pcState().instAddr());
    req->setAllBytesEnabled();

    Fault fault =
        thread->mmu->translateAtomic(req, thread->getTC(), BaseMMU::Write);
    if (fault != NoFault) {
        return fault;
    }
    if (req->getFlags().isSet(Request::NO_ACCESS)) {
        return NoFault;
    }

    uint8_t *host = (req->isLocalAccess() || req->isUncacheable() ||
                     req->isStrictlyOrdered() || !storesBypassPort())
                        ? nullptr
                        : hostAddr(dataStore, req->getPaddr(), size, true);
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
DirectMemorySimpleCPU::sendPacket(RequestPort &port, const PacketPtr &pkt)
{
    if (&port == &dcachePort && tryDirectAccess(pkt)) {
        return 0;
    }
    return port.sendAtomic(pkt);
}

Fault
DirectMemorySimpleCPU::fetchInstruction(Tick &latency)
{
    SimpleExecContext &t_info = *threadInfo[curThread];
    SimpleThread *thread = t_info.thread;
    auto &decoder = thread->decoder;
    const Addr fetch_pc = (thread->pcState().instAddr() & decoder->pcMask()) +
                          t_info.fetchOffset;
    const unsigned size = decoder->moreBytesSize();
    BaseTLB *itb = thread->mmu->itb;

    if (directAccessActive() && fetch_pc >= fetchPage.vpage &&
        fetch_pc - fetchPage.vpage < fetchPage.size &&
        size <= fetchPage.size - (fetch_pc - fetchPage.vpage) &&
        itb->translationEpoch(thread->getTC()) == fetchPage.epoch) {
        copyBytes(decoder->moreBytesPtr(),
                  fetchPage.host + (fetch_pc - fetchPage.vpage), size);
        latency = 0;
        return NoFault;
    }

    Fault fault = AtomicSimpleCPU::fetchInstruction(latency);
    if (fault != NoFault) {
        return fault;
    }

    // Cache the page if translation is uniform and one mapping covers it.
    Addr vpage, ppage, page_size;
    if (!ifetch_req->isUncacheable() && !ifetch_req->isStrictlyOrdered() &&
        !ifetch_req->isLocalAccess() &&
        itb->stableFetchPage(thread->getTC(), fetch_pc, vpage, ppage,
                             page_size)) {
        const uint8_t *host = hostAddr(fetchStore, ppage, page_size, false);
        if (host) {
            fetchPage.vpage = vpage;
            fetchPage.size = page_size;
            fetchPage.epoch = itb->translationEpoch(thread->getTC());
            fetchPage.host = host;
        }
    }
    return NoFault;
}

Tick
DirectMemorySimpleCPU::fetchInstMem()
{
    if (ifetch_req->isUncacheable() || ifetch_req->isStrictlyOrdered() ||
        ifetch_req->isLocalAccess()) {
        return AtomicSimpleCPU::fetchInstMem();
    }
    const unsigned size = ifetch_req->getSize();
    const uint8_t *host =
        hostAddr(fetchStore, ifetch_req->getPaddr(), size, false);
    if (!host) {
        return AtomicSimpleCPU::fetchInstMem();
    }

    auto &decoder = threadInfo[curThread]->thread->decoder;
    copyBytes(decoder->moreBytesPtr(), host, size);
    return 0;
}

} // namespace gem5
