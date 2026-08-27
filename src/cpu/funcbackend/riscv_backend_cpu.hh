/*
 * Copyright (c) 2026 Roman Popov
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

#ifndef __CPU_FUNCBACKEND_RISCV_BACKEND_CPU_HH__
#define __CPU_FUNCBACKEND_RISCV_BACKEND_CPU_HH__

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

#include "cpu/simple/noncaching.hh"
#include "mem/packet.hh"
#include "params/RiscvBackendCPU.hh"
#include "sim/eventq.hh"

namespace gem5
{

/** Why an external backend stopped executing a batch. */
enum class RiscvBackendExit
{
    Budget,
    Halted,
    Interrupt,
    Exception,
    Error,
    M5Op,
};

/** What one batch of external execution did. */
struct RiscvBackendResult
{
    uint64_t instructions;
    RiscvBackendExit reason;
    uint32_t m5Function;
    int backendException;
};

/**
 * One external RV64 functional execution engine, as seen by gem5.
 *
 * The engine owns the architectural state between batches; gem5 imports and
 * exports it around every batch boundary and owns everything else, including
 * physical memory, devices, interrupt controllers and simulated time.
 */
class RiscvBackend
{
  public:
    virtual ~RiscvBackend() = default;

    virtual RiscvBackendResult run(uint64_t instructions) = 0;

    virtual uint64_t getGpr(unsigned index) const = 0;
    virtual void setGpr(unsigned index, uint64_t value) = 0;
    virtual uint64_t getFpr(unsigned index) const = 0;
    virtual void setFpr(unsigned index, uint64_t value) = 0;
    virtual uint64_t getPc() const = 0;
    virtual void setPc(uint64_t value) = 0;
    virtual unsigned getPriv() const = 0;
    virtual void setPriv(unsigned value) = 0;
    virtual uint64_t getCsr(unsigned csr) const = 0;
    virtual void setCsr(unsigned csr, uint64_t value) = 0;
    virtual uint64_t getMip() const = 0;
    virtual void setMip(uint64_t value) = 0;

    /**
     * Whether the backend's CSR accessors enforce the current privilege
     * mode, so a state transfer has to raise the backend to M mode around
     * itself. A backend with a back door into its CSR file says no, which
     * matters when changing its privilege mode is expensive.
     */
    virtual bool csrAccessIsPrivileged() const { return true; }

    /**
     * Drop every piece of non-architectural state: translated or decoded
     * code, software TLBs, the load reservation, and any halted or pending
     * exception condition.
     */
    virtual void invalidate() = 0;
};

/**
 * A fast functional RISC-V CPU whose execution happens outside gem5.
 *
 * The model charges one simulated cycle per retired instruction and performs
 * functional physical-memory accesses, so its IPC is 1 by construction and
 * the host instruction rate is the meaningful performance measure. It exists
 * to skip uninteresting execution, such as a full-system boot, before
 * switching the same harts to a detailed model.
 *
 * Subclasses supply one concrete backend each and nothing else; everything
 * that makes an external engine behave like a gem5 CPU -- architectural
 * state transfer, batch budgeting against the event queue, instruction
 * accounting, physical memory and device access, and m5 pseudo-instructions
 * -- lives here.
 */
class RiscvBackendCPU : public NonCachingSimpleCPU
{
  public:
    using Params = RiscvBackendCPUParams;

    RiscvBackendCPU(const Params &params);
    ~RiscvBackendCPU() override;

    void init() override;
    void startup() override;
    void takeOverFrom(BaseCPU *old_cpu) override;

  protected:
    const std::string backendPath;
    const uint32_t backendInstance;
    const uint32_t backendInstanceCount;

    /**
     * Create the backend for this CPU and bind it to the thunks below.
     *
     * Called once from init(), after the ISA has been validated, so an
     * implementation may consult the ISA description helpers.
     */
    virtual std::unique_ptr<RiscvBackend> connectBackend() = 0;

    void tick() override;

    /** This CPU's ISA as a RISC-V ISA string, for example "rv64imafdc_zba". */
    std::string riscvIsaString() const;
    /** The implemented privilege modes, one of "M", "MU" and "MSU". */
    const char *privilegeModeString() const;
    /** Widest supported virtual address: 0 when bare, else 39, 48 or 57. */
    unsigned maxVirtualAddressBits() const;

    /*
     * Backend callbacks. Every ABI in use declares these with identical
     * signatures, so one set of thunks serves all of them.
     */
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

  private:
    std::unique_ptr<RiscvBackend> backend;
    const uint64_t batchSize;
    bool backendQueueLocked = false;
    bool backendIoAccessed = false;
    /** This thread's event queue, saved across a backend run. */
    EventQueue *savedEventQueue = nullptr;
    /** Interrupt-pending value last exported to the backend. */
    RegVal pushedMip = 0;

    void syncToBackend();
    void syncFromBackend();
    uint64_t executionBudget() const;
    void accountInstructions(uint64_t instructions);

    int physicalAccess(MemCmd::Command command, uint64_t address,
                       uint8_t *data, size_t size);
    int physicalMemoryMap(size_t index, uint64_t *guestAddress,
                          uint64_t *size, uint8_t **hostAddress,
                          int *writable);
    void backendRunBegin();
    void backendRunEnd();
    uint64_t backendReadTime() const;
    bool backendShouldStop() const;

    /**
     * Tick events of every backend CPU in the simulation.
     *
     * A batch must not run past a pending event of any other SimObject, but
     * the tick events of sibling backend CPUs are not a reason to cut it
     * short: they only resume more batched execution. Events carry no owner
     * identity, so tick events are registered here and matched by address.
     */
    static std::unordered_set<const Event *> tickEvents;

    /**
     * Whether an event not owned by a backend CPU is due at or before
     * @p when.
     */
    static bool hasForeignEventAtOrBefore(const EventQueue *event_queue,
                                          Tick when);
};

} // namespace gem5

#endif // __CPU_FUNCBACKEND_RISCV_BACKEND_CPU_HH__
