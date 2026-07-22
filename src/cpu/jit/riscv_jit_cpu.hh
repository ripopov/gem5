/*
 * Copyright (c) 2026
 * All rights reserved.
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

#ifndef __CPU_JIT_RISCV_JIT_CPU_HH__
#define __CPU_JIT_RISCV_JIT_CPU_HH__

#include <cstddef>
#include <cstdint>
#include <memory>

#include "cpu/simple/noncaching.hh"
#include "params/RiscvJitCPU.hh"

namespace gem5
{

class QemuJitBackend;

class RiscvJitCPU : public NonCachingSimpleCPU
{
  public:
    using Params = RiscvJitCPUParams;

    RiscvJitCPU(const Params &params);
    ~RiscvJitCPU() override;

    void init() override;
    void initState() override;
    void startup() override;
    void takeOverFrom(BaseCPU *old_cpu) override;

  protected:
    void tick() override;

  private:
    std::unique_ptr<QemuJitBackend> backend;
    const uint64_t batchSize;
    const uint32_t backendInstance;
    const uint32_t backendInstanceCount;
    bool backendInitialized = false;
    bool backendQueueLocked = false;
    bool backendIoAccessed = false;

    void syncToBackend();
    void syncFromBackend();
    uint64_t executionBudget() const;
    void accountInstructions(uint64_t instructions);

    int physicalRead(uint64_t address, uint8_t *data, size_t size);
    int physicalWrite(uint64_t address, const uint8_t *data, size_t size);
    int physicalMemoryMap(size_t index, uint64_t *guestAddress,
                          uint64_t *size, uint8_t **hostAddress,
                          int *writable);
    void backendRunBegin();
    void backendRunEnd();
    uint64_t backendReadTime() const;
    bool backendShouldStop() const;

    static int memoryReadThunk(void *opaque, uint64_t address,
                               uint8_t *data, size_t size);
    static int memoryWriteThunk(void *opaque, uint64_t address,
                                const uint8_t *data, size_t size);
    static int memoryMapThunk(void *opaque, size_t index,
                              uint64_t *guestAddress, uint64_t *size,
                              uint8_t **hostAddress, int *writable);
    static void runBeginThunk(void *opaque);
    static void runEndThunk(void *opaque);
    static uint64_t readTimeThunk(void *opaque);
    static int shouldStopThunk(void *opaque);
};

} // namespace gem5

#endif // __CPU_JIT_RISCV_JIT_CPU_HH__
