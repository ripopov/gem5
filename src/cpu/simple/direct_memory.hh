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

#ifndef __CPU_SIMPLE_DIRECT_MEMORY_HH__
#define __CPU_SIMPLE_DIRECT_MEMORY_HH__

#include "cpu/simple/atomic.hh"
#include "mem/physical.hh"
#include "params/BaseDirectMemorySimpleCPU.hh"

namespace gem5
{

/**
 * An atomic CPU using eligible system RAM backing stores in
 * 'atomic_noncaching' mode. Special accesses retain atomic port handling.
 */
class DirectMemorySimpleCPU : public AtomicSimpleCPU
{
  public:
    DirectMemorySimpleCPU(const BaseDirectMemorySimpleCPUParams &p);

    void verifyMemoryMode() const override;
    void startup() override;
    void takeOverFrom(BaseCPU *old_cpu) override;

  protected:
    void initDirectAccess();
    bool directAccessActive() const;

    /** Last entries; PhysicalMemory keeps their addresses stable. */
    const memory::BackingStoreEntry *fetchStore = nullptr;
    const memory::BackingStoreEntry *dataStore = nullptr;

    /**
     * The instruction page being executed from, as a host pointer.
     * While the fetch PC stays inside it and the TLB's translation epoch
     * is unchanged, a fetch is a copy from this pointer with no request,
     * translation or mapping lookup. The bytes are read afresh on every
     * fetch, so writes to the page need no special handling; only the
     * translation is cached, and only for pages the TLB promises are
     * uniformly translated (BaseTLB::stableFetchPage()).
     */
    struct FetchPage
    {
        Addr vpage = 0;
        Addr size = 0; // zero: nothing cached
        uint64_t epoch = 0;
        const uint8_t *host = nullptr;
    } fetchPage;

    Fault fetchInstruction(Tick &latency) override;

    /**
     * Host address of [addr, addr + size) through an eligible direct mapping,
     * or nullptr. Refreshes the cached pointer when
     * the access is outside its mapping.
     */
    uint8_t *hostAddr(const memory::BackingStoreEntry *&store, Addr addr,
                      unsigned size, bool write);

    /**
     * Perform a plain load or store through a direct mapping instead
     * of the port, if the packet and the target allow it.
     *
     * @return true if the packet was completed here.
     */
    bool tryDirectAccess(const PacketPtr &pkt);

    /** May plain stores skip the port? See tryDirectAccess(). */
    bool storesBypassPort() const;

    /**
     * Is this a plain load or store the fast path in readMem()/writeMem()
     * may handle: one cache-line fragment, every byte enabled, no
     * reservation, atomic, prefetch or cache-maintenance semantics?
     */
    bool plainAccess(Addr addr, unsigned size, Request::Flags flags,
                     const std::vector<bool> &byte_enable) const;

    Tick sendPacket(RequestPort &port, const PacketPtr &pkt) override;
    Tick fetchInstMem() override;

    Fault readMem(Addr addr, uint8_t *data, unsigned size,
                  Request::Flags flags,
                  const std::vector<bool> &byte_enable) override;
    Fault writeMem(uint8_t *data, unsigned size, Addr addr,
                   Request::Flags flags, uint64_t *res,
                   const std::vector<bool> &byte_enable) override;
};

} // namespace gem5

#endif // __CPU_SIMPLE_DIRECT_MEMORY_HH__
