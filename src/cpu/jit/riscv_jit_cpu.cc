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

#include "cpu/jit/riscv_jit_cpu.hh"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <string>

#include "arch/riscv/isa.hh"
#include "arch/riscv/regs/float.hh"
#include "arch/riscv/regs/int.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/reg_abi.hh"
#include "arch/riscv/system.hh"
#include "base/logging.hh"
#include "debug/JitCPU.hh"
#include "cpu/utils.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "sim/eventq.hh"
#include "sim/pseudo_inst.hh"

namespace gem5
{

namespace
{

constexpr uint32_t QemuJitAbiVersion = 3;

using MemoryRead = int (*)(void *, uint64_t, uint8_t *, size_t);
using MemoryWrite = int (*)(void *, uint64_t, const uint8_t *, size_t);
using MemoryMap = int (*)(void *, size_t, uint64_t *, uint64_t *, uint8_t **,
                          int *);
using RunBoundary = void (*)(void *);
using ReadTime = uint64_t (*)(void *);
using ShouldStop = int (*)(void *);

struct QemuJitCallbacks
{
    uint32_t abi_version;
    MemoryRead memory_read;
    MemoryWrite memory_write;
    MemoryMap memory_map;
    RunBoundary run_begin;
    RunBoundary run_end;
    ReadTime read_time;
    ShouldStop should_stop;
    void *opaque;
};

enum class QemuJitExitReason : int
{
    Budget = 0,
    Halted = 1,
    Interrupt = 2,
    Exception = 3,
    Error = 4,
    M5Op = 5,
};

struct QemuJitRunResult
{
    uint64_t instructions;
    int qemu_exception;
    QemuJitExitReason reason;
    uint32_t m5_function;
};

struct CsrMapping
{
    unsigned csr;
    RiscvISA::MiscRegIndex misc;
};

constexpr std::array CsrMappings = {
    CsrMapping{RiscvISA::CSR_MSTATUS, RiscvISA::MISCREG_STATUS},
    CsrMapping{RiscvISA::CSR_MEDELEG, RiscvISA::MISCREG_MEDELEG},
    CsrMapping{RiscvISA::CSR_MIDELEG, RiscvISA::MISCREG_MIDELEG},
    CsrMapping{RiscvISA::CSR_MIE, RiscvISA::MISCREG_IE},
    CsrMapping{RiscvISA::CSR_MTVEC, RiscvISA::MISCREG_MTVEC},
    CsrMapping{RiscvISA::CSR_MCOUNTEREN, RiscvISA::MISCREG_MCOUNTEREN},
    CsrMapping{RiscvISA::CSR_MSCRATCH, RiscvISA::MISCREG_MSCRATCH},
    CsrMapping{RiscvISA::CSR_MEPC, RiscvISA::MISCREG_MEPC},
    CsrMapping{RiscvISA::CSR_MCAUSE, RiscvISA::MISCREG_MCAUSE},
    CsrMapping{RiscvISA::CSR_MTVAL, RiscvISA::MISCREG_MTVAL},
    CsrMapping{RiscvISA::CSR_PMPCFG0, RiscvISA::MISCREG_PMPCFG0},
    CsrMapping{RiscvISA::CSR_PMPCFG2, RiscvISA::MISCREG_PMPCFG2},
    CsrMapping{RiscvISA::CSR_PMPADDR00, RiscvISA::MISCREG_PMPADDR00},
    CsrMapping{RiscvISA::CSR_PMPADDR01, RiscvISA::MISCREG_PMPADDR01},
    CsrMapping{RiscvISA::CSR_PMPADDR02, RiscvISA::MISCREG_PMPADDR02},
    CsrMapping{RiscvISA::CSR_PMPADDR03, RiscvISA::MISCREG_PMPADDR03},
    CsrMapping{RiscvISA::CSR_PMPADDR04, RiscvISA::MISCREG_PMPADDR04},
    CsrMapping{RiscvISA::CSR_PMPADDR05, RiscvISA::MISCREG_PMPADDR05},
    CsrMapping{RiscvISA::CSR_PMPADDR06, RiscvISA::MISCREG_PMPADDR06},
    CsrMapping{RiscvISA::CSR_PMPADDR07, RiscvISA::MISCREG_PMPADDR07},
    CsrMapping{RiscvISA::CSR_PMPADDR08, RiscvISA::MISCREG_PMPADDR08},
    CsrMapping{RiscvISA::CSR_PMPADDR09, RiscvISA::MISCREG_PMPADDR09},
    CsrMapping{RiscvISA::CSR_PMPADDR10, RiscvISA::MISCREG_PMPADDR10},
    CsrMapping{RiscvISA::CSR_PMPADDR11, RiscvISA::MISCREG_PMPADDR11},
    CsrMapping{RiscvISA::CSR_PMPADDR12, RiscvISA::MISCREG_PMPADDR12},
    CsrMapping{RiscvISA::CSR_PMPADDR13, RiscvISA::MISCREG_PMPADDR13},
    CsrMapping{RiscvISA::CSR_PMPADDR14, RiscvISA::MISCREG_PMPADDR14},
    CsrMapping{RiscvISA::CSR_PMPADDR15, RiscvISA::MISCREG_PMPADDR15},
    CsrMapping{RiscvISA::CSR_STVEC, RiscvISA::MISCREG_STVEC},
    CsrMapping{RiscvISA::CSR_SCOUNTEREN, RiscvISA::MISCREG_SCOUNTEREN},
    CsrMapping{RiscvISA::CSR_SSCRATCH, RiscvISA::MISCREG_SSCRATCH},
    CsrMapping{RiscvISA::CSR_SEPC, RiscvISA::MISCREG_SEPC},
    CsrMapping{RiscvISA::CSR_SCAUSE, RiscvISA::MISCREG_SCAUSE},
    CsrMapping{RiscvISA::CSR_STVAL, RiscvISA::MISCREG_STVAL},
    CsrMapping{RiscvISA::CSR_SATP, RiscvISA::MISCREG_SATP},
    CsrMapping{RiscvISA::CSR_SENVCFG, RiscvISA::MISCREG_SENVCFG},
};

} // anonymous namespace

class QemuJitBackend
{
  private:
    void *handle;

    using InitFn = int (*)(const QemuJitCallbacks *);
    using RunFn = int (*)(uint64_t, QemuJitRunResult *);
    using GetRegFn = uint64_t (*)(unsigned);
    using SetRegFn = int (*)(unsigned, uint64_t);
    using GetPcFn = uint64_t (*)();
    using SetPcFn = void (*)(uint64_t);
    using GetPrivFn = unsigned (*)();
    using SetPrivFn = int (*)(unsigned);
    using GetCsrFn = int (*)(unsigned, uint64_t *);
    using SetCsrFn = int (*)(unsigned, uint64_t);
    using GetMipFn = uint64_t (*)();
    using SetMipFn = void (*)(uint64_t);
    using InvalidateFn = void (*)();

    InitFn initFn;
    RunFn runFn;
    GetRegFn getGprFn;
    SetRegFn setGprFn;
    GetRegFn getFprFn;
    SetRegFn setFprFn;
    GetPcFn getPcFn;
    SetPcFn setPcFn;
    GetPrivFn getPrivFn;
    SetPrivFn setPrivFn;
    GetCsrFn getCsrFn;
    SetCsrFn setCsrFn;
    GetMipFn getMipFn;
    SetMipFn setMipFn;
    InvalidateFn invalidateFn;

    template <typename T>
    T
    symbol(const char *name)
    {
        dlerror();
        void *address = dlsym(handle, name);
        const char *error = dlerror();
        fatal_if(error || !address, "JitCPU backend symbol %s: %s", name,
                 error ? error : "not found");
        return reinterpret_cast<T>(address);
    }

  public:
    explicit QemuJitBackend(const std::string &path)
    {
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        fatal_if(!handle, "Unable to load JitCPU backend '%s': %s", path,
                 dlerror());

        initFn = symbol<InitFn>("gem5_qemu_jit_init");
        runFn = symbol<RunFn>("gem5_qemu_jit_run");
        getGprFn = symbol<GetRegFn>("gem5_qemu_jit_get_gpr");
        setGprFn = symbol<SetRegFn>("gem5_qemu_jit_set_gpr");
        getFprFn = symbol<GetRegFn>("gem5_qemu_jit_get_fpr");
        setFprFn = symbol<SetRegFn>("gem5_qemu_jit_set_fpr");
        getPcFn = symbol<GetPcFn>("gem5_qemu_jit_get_pc");
        setPcFn = symbol<SetPcFn>("gem5_qemu_jit_set_pc");
        getPrivFn = symbol<GetPrivFn>("gem5_qemu_jit_get_priv");
        setPrivFn = symbol<SetPrivFn>("gem5_qemu_jit_set_priv");
        getCsrFn = symbol<GetCsrFn>("gem5_qemu_jit_get_csr");
        setCsrFn = symbol<SetCsrFn>("gem5_qemu_jit_set_csr");
        getMipFn = symbol<GetMipFn>("gem5_qemu_jit_get_mip");
        setMipFn = symbol<SetMipFn>("gem5_qemu_jit_set_mip");
        invalidateFn = symbol<InvalidateFn>(
            "gem5_qemu_jit_invalidate_translations");
    }

    ~QemuJitBackend()
    {
        /* QEMU owns a live TCG worker thread for the process lifetime. Do not
         * unload its text while that thread exists. */
    }

    void
    init(const QemuJitCallbacks &callbacks)
    {
        fatal_if(initFn(&callbacks) != 0, "JitCPU backend init failed");
    }

    QemuJitRunResult
    run(uint64_t instructions)
    {
        QemuJitRunResult result{};
        fatal_if(runFn(instructions, &result) != 0,
                 "JitCPU backend execution failed");
        return result;
    }

    uint64_t getGpr(unsigned index) const { return getGprFn(index); }
    uint64_t getFpr(unsigned index) const { return getFprFn(index); }
    uint64_t getPc() const { return getPcFn(); }
    unsigned getPriv() const { return getPrivFn(); }
    uint64_t getMip() const { return getMipFn(); }

    void
    setGpr(unsigned index, uint64_t value)
    {
        fatal_if(setGprFn(index, value), "Unable to set QEMU GPR %u", index);
    }

    void
    setFpr(unsigned index, uint64_t value)
    {
        fatal_if(setFprFn(index, value), "Unable to set QEMU FPR %u", index);
    }

    void setPc(uint64_t value) { setPcFn(value); }

    void
    setPriv(unsigned value)
    {
        fatal_if(setPrivFn(value), "Unable to set QEMU privilege %u", value);
    }

    uint64_t
    getCsr(unsigned csr) const
    {
        uint64_t value = 0;
        fatal_if(getCsrFn(csr, &value), "Unable to read QEMU CSR %#x", csr);
        return value;
    }

    void
    setCsr(unsigned csr, uint64_t value)
    {
        fatal_if(setCsrFn(csr, value), "Unable to set QEMU CSR %#x", csr);
    }

    void setMip(uint64_t value) { setMipFn(value); }
    void invalidate() { invalidateFn(); }
};

RiscvJitCPU::RiscvJitCPU(const Params &p)
    : NonCachingSimpleCPU(p), batchSize(p.batch_size)
{
    fatal_if(batchSize == 0, "JitCPU batch_size must be non-zero");
    backend = std::make_unique<QemuJitBackend>(p.backend_path);
}

RiscvJitCPU::~RiscvJitCPU() = default;

void
RiscvJitCPU::init()
{
    NonCachingSimpleCPU::init();

    const QemuJitCallbacks callbacks = {
        .abi_version = QemuJitAbiVersion,
        .memory_read = memoryReadThunk,
        .memory_write = memoryWriteThunk,
        .memory_map = memoryMapThunk,
        .run_begin = runBeginThunk,
        .run_end = runEndThunk,
        .read_time = readTimeThunk,
        .should_stop = shouldStopThunk,
        .opaque = this,
    };
    backend->init(callbacks);
    backendInitialized = true;
}

void
RiscvJitCPU::initState()
{
    NonCachingSimpleCPU::initState();
}

void
RiscvJitCPU::startup()
{
    NonCachingSimpleCPU::startup();

    // Workloads establish the reset PC and initial architectural state from
    // their own initState() hook. startup() runs after every SimObject has
    // completed initState() (or checkpoint restoration).
    syncToBackend();
}

void
RiscvJitCPU::syncToBackend()
{
    if (!backendInitialized) {
        return;
    }

    ThreadContext *tc = threadContexts[0];
    const unsigned privilege =
        tc->readMiscRegNoEffect(RiscvISA::MISCREG_PRV);

    backend->setPriv(RiscvISA::PRV_M);

    for (unsigned index = 0; index < RiscvISA::int_reg::NumArchRegs;
         ++index) {
        const RegVal value = tc->getReg(RiscvISA::intRegClass[index]);
        if (backend->getGpr(index) != value) {
            backend->setGpr(index, value);
        }
    }
    for (unsigned index = 0; index < RiscvISA::float_reg::NumRegs; ++index) {
        const RegVal value = tc->getReg(RiscvISA::floatRegClass[index]);
        if (backend->getFpr(index) != value) {
            backend->setFpr(index, value);
        }
    }

    const Addr pc = tc->pcState().instAddr();
    if (backend->getPc() != pc) {
        backend->setPc(pc);
    }

    for (const auto &mapping : CsrMappings) {
        const RegVal value = mapping.misc == RiscvISA::MISCREG_IE ?
            tc->readMiscReg(mapping.misc) :
            tc->readMiscRegNoEffect(mapping.misc);
        if (backend->getCsr(mapping.csr) != value) {
            backend->setCsr(mapping.csr, value);
        }
    }
    const RiscvISA::STATUS status(
        tc->readMiscRegNoEffect(RiscvISA::MISCREG_STATUS));
    if (status.fs != RiscvISA::FPUStatus::OFF) {
        const RegVal fflags =
            tc->readMiscRegNoEffect(RiscvISA::MISCREG_FFLAGS);
        const RegVal frm = tc->readMiscRegNoEffect(RiscvISA::MISCREG_FRM);
        if (backend->getCsr(RiscvISA::CSR_FFLAGS) != fflags) {
            backend->setCsr(RiscvISA::CSR_FFLAGS, fflags);
        }
        if (backend->getCsr(RiscvISA::CSR_FRM) != frm) {
            backend->setCsr(RiscvISA::CSR_FRM, frm);
        }
    }
    backend->setMip(tc->readMiscReg(RiscvISA::MISCREG_IP));
    backend->setPriv(privilege);
}

void
RiscvJitCPU::syncFromBackend()
{
    ThreadContext *tc = threadContexts[0];
    const unsigned privilege = backend->getPriv();

    backend->setPriv(RiscvISA::PRV_M);
    tc->setMiscRegNoEffect(RiscvISA::MISCREG_PRV, RiscvISA::PRV_M);

    for (unsigned index = 1; index < RiscvISA::int_reg::NumArchRegs;
         ++index) {
        const RegVal value = backend->getGpr(index);
        if (tc->getReg(RiscvISA::intRegClass[index]) != value) {
            tc->setReg(RiscvISA::intRegClass[index], value);
        }
    }
    for (unsigned index = 0; index < RiscvISA::float_reg::NumRegs; ++index) {
        const RegVal value = backend->getFpr(index);
        if (tc->getReg(RiscvISA::floatRegClass[index]) != value) {
            tc->setReg(RiscvISA::floatRegClass[index], value);
        }
    }

    const Addr pc = backend->getPc();
    if (tc->pcState().instAddr() != pc) {
        tc->pcState(pc);
    }

    for (const auto &mapping : CsrMappings) {
        const RegVal value = backend->getCsr(mapping.csr);
        const RegVal old_value = mapping.misc == RiscvISA::MISCREG_IE ?
            tc->readMiscReg(mapping.misc) :
            tc->readMiscRegNoEffect(mapping.misc);
        if (old_value != value) {
            tc->setMiscReg(mapping.misc, value);
        }
    }
    const RiscvISA::STATUS status(
        tc->readMiscRegNoEffect(RiscvISA::MISCREG_STATUS));
    if (status.fs != RiscvISA::FPUStatus::OFF) {
        const RegVal fflags = backend->getCsr(RiscvISA::CSR_FFLAGS);
        const RegVal frm = backend->getCsr(RiscvISA::CSR_FRM);
        if (tc->readMiscRegNoEffect(RiscvISA::MISCREG_FFLAGS) != fflags) {
            tc->setMiscReg(RiscvISA::MISCREG_FFLAGS, fflags);
        }
        if (tc->readMiscRegNoEffect(RiscvISA::MISCREG_FRM) != frm) {
            tc->setMiscReg(RiscvISA::MISCREG_FRM, frm);
        }
    }

    const RegVal mip = backend->getMip();
    if (tc->readMiscReg(RiscvISA::MISCREG_IP) != mip) {
        tc->setMiscReg(RiscvISA::MISCREG_IP, mip);
    }

    tc->setMiscRegNoEffect(RiscvISA::MISCREG_PRV, privilege);
    backend->setPriv(privilege);
}

uint64_t
RiscvJitCPU::executionBudget() const
{
    uint64_t budget = batchSize;
    const auto *thread = threadInfo[0]->thread;
    const auto &inst_queue = thread->comInstEventQueue;

    if (!inst_queue.empty()) {
        const Counter current = threadInfo[0]->numInst;
        const Tick next = inst_queue.nextTick();
        if (next > current) {
            budget = std::min<uint64_t>(budget, next - current);
        } else {
            budget = 1;
        }
    }

    EventQueue *event_queue = eventQueue();
    if (!event_queue->empty()) {
        const Tick next = event_queue->nextTick();
        if (next > curTick()) {
            const Cycles cycles = ticksToCycles(next - curTick());
            budget = std::min<uint64_t>(budget,
                std::max<uint64_t>(1, cycles));
        } else {
            budget = 1;
        }
    }

    return std::max<uint64_t>(1, budget);
}

void
RiscvJitCPU::accountInstructions(uint64_t instructions)
{
    if (!instructions) {
        return;
    }

    SimpleExecContext &info = *threadInfo[0];
    const bool user_mode = info.thread->getIsaPtr()->inUserMode();

    info.numInst += instructions;
    info.numOp += instructions;
    info.thread->threadStats.numInsts += instructions;
    info.thread->threadStats.numOps += instructions;

    instCnt += instructions;
    baseStats.numCycles += instructions;
    baseStats.numInsts += instructions;
    baseStats.numOps += instructions;
    fetchStats[0]->numInsts += instructions;
    fetchStats[0]->numOps += instructions;
    executeStats[0]->numInsts += instructions;
    commitStats[0]->numInsts += instructions;
    commitStats[0]->numOps += instructions;
    commitStats[0]->numInstsNotNOP += instructions;
    commitStats[0]->numOpsNotNOP += instructions;
    if (user_mode) {
        commitStats[0]->numUserInsts += instructions;
        commitStats[0]->numUserOps += instructions;
    }
}

void
RiscvJitCPU::tick()
{
    SimpleExecContext &info = *threadInfo[0];

    if (_status == Idle) {
        tryCompleteDrain();
        return;
    }

    checkForInterrupts();
    checkPcEventQueue();
    if (_status == Idle) {
        tryCompleteDrain();
        return;
    }

    serviceInstCountEvents();
    syncToBackend();

    const uint64_t budget = executionBudget();
    QemuJitRunResult result;
    {
        EventQueue::ScopedRelease release(eventQueue());
        result = backend->run(budget);
    }

    syncFromBackend();
    accountInstructions(result.instructions);
    serviceInstCountEvents();

    DPRINTF(JitCPU,
            "Executed %llu instructions, pc=%#x, exit=%d, qemu=%d\n",
            result.instructions, info.thread->pcState().instAddr(),
            static_cast<int>(result.reason), result.qemu_exception);

    if (result.reason == QemuJitExitReason::M5Op) {
        uint64_t value = 0;
        ThreadContext *tc = threadContexts[0];
        fatal_if(!pseudo_inst::pseudoInst<RiscvISA::RegABI64>(
                     tc, result.m5_function, value),
                 "Unhandled JitCPU m5 pseudo instruction %#x",
                 result.m5_function);
        tc->setReg(RiscvISA::int_reg::A0, value);
        tc->pcState(tc->pcState().instAddr() + 4);

        if (!tryCompleteDrain() && _status != Idle) {
            const Cycles elapsed(
                std::max<uint64_t>(1, result.instructions));
            reschedule(tickEvent, clockEdge(elapsed), true);
        }
        return;
    }

    if (result.reason == QemuJitExitReason::Halted) {
        if (tryCompleteDrain()) {
            return;
        }

        // Keep the gem5 context runnable while QEMU is in WFI. QEMU wakes
        // from its halted state after syncToBackend() injects a newly pending
        // interrupt. Poll only at the next gem5 event, avoiding a busy loop
        // while preserving device/timer event ordering.
        EventQueue *event_queue = eventQueue();
        if (!event_queue->empty()) {
            const Tick after_instructions = clockEdge(
                Cycles(std::max<uint64_t>(1, result.instructions)));
            schedule(tickEvent,
                     std::max(after_instructions, event_queue->nextTick()));
        }
        return;
    }

    if (tryCompleteDrain()) {
        return;
    }

    if (_status != Idle) {
        const Cycles elapsed(std::max<uint64_t>(1, result.instructions));
        reschedule(tickEvent, clockEdge(elapsed), true);
    }
}

int
RiscvJitCPU::physicalRead(uint64_t address, uint8_t *data, size_t size)
{
    while (size) {
        const size_t fragment = std::min<size_t>(
            size, cacheLineSize() - addrBlockOffset(address, cacheLineSize()));
        RequestPtr request = std::make_shared<Request>(
            address, fragment, Request::PHYSICAL, dataRequestorId());
        request->setContext(threadContexts[0]->contextId());
        Packet packet(request, MemCmd::ReadReq);
        packet.dataStatic(data);
        sendPacket(dcachePort, &packet);
        if (packet.isError()) {
            return -1;
        }
        address += fragment;
        data += fragment;
        size -= fragment;
    }
    return 0;
}

int
RiscvJitCPU::physicalWrite(uint64_t address, const uint8_t *data, size_t size)
{
    while (size) {
        const size_t fragment = std::min<size_t>(
            size, cacheLineSize() - addrBlockOffset(address, cacheLineSize()));
        RequestPtr request = std::make_shared<Request>(
            address, fragment, Request::PHYSICAL, dataRequestorId());
        request->setContext(threadContexts[0]->contextId());
        Packet packet(request, MemCmd::WriteReq);
        packet.dataStatic(const_cast<uint8_t *>(data));
        sendPacket(dcachePort, &packet);
        if (packet.isError()) {
            return -1;
        }
        address += fragment;
        data += fragment;
        size -= fragment;
    }
    return 0;
}

int
RiscvJitCPU::physicalMemoryMap(size_t index, uint64_t *guestAddress,
                               uint64_t *size, uint8_t **hostAddress,
                               int *writable)
{
    const auto stores = system->getPhysMem().getBackingStore();
    size_t eligible = 0;

    for (const auto &store : stores) {
        if (!store.kvmMap || !store.pmem || store.range.interleaved()) {
            continue;
        }
        if (eligible++ != index) {
            continue;
        }

        *guestAddress = store.range.start();
        *size = store.range.size();
        *hostAddress = store.pmem;
        *writable = 1;
        return 0;
    }
    return -1;
}

void
RiscvJitCPU::backendRunBegin()
{
    fatal_if(backendQueueLocked, "Nested JitCPU backend execution");
    eventQueue()->lock();
    curEventQueue(eventQueue());
    backendQueueLocked = true;
}

void
RiscvJitCPU::backendRunEnd()
{
    fatal_if(!backendQueueLocked, "Unbalanced JitCPU backend execution");
    backendQueueLocked = false;
    curEventQueue(nullptr);
    eventQueue()->unlock();
}

uint64_t
RiscvJitCPU::backendReadTime() const
{
    auto *riscv_system = dynamic_cast<RiscvSystem *>(system);
    fatal_if(!riscv_system, "JitCPU requires a RiscvSystem");
    return riscv_system->tryReadMtime();
}

bool
RiscvJitCPU::backendShouldStop() const
{
    return !eventQueue()->empty() && eventQueue()->nextTick() <= curTick();
}

int
RiscvJitCPU::memoryReadThunk(void *opaque, uint64_t address,
                             uint8_t *data, size_t size)
{
    return static_cast<RiscvJitCPU *>(opaque)->physicalRead(
        address, data, size);
}

int
RiscvJitCPU::memoryWriteThunk(void *opaque, uint64_t address,
                              const uint8_t *data, size_t size)
{
    return static_cast<RiscvJitCPU *>(opaque)->physicalWrite(
        address, data, size);
}

int
RiscvJitCPU::memoryMapThunk(void *opaque, size_t index,
                            uint64_t *guestAddress, uint64_t *size,
                            uint8_t **hostAddress, int *writable)
{
    return static_cast<RiscvJitCPU *>(opaque)->physicalMemoryMap(
        index, guestAddress, size, hostAddress, writable);
}

void
RiscvJitCPU::runBeginThunk(void *opaque)
{
    static_cast<RiscvJitCPU *>(opaque)->backendRunBegin();
}

void
RiscvJitCPU::runEndThunk(void *opaque)
{
    static_cast<RiscvJitCPU *>(opaque)->backendRunEnd();
}

uint64_t
RiscvJitCPU::readTimeThunk(void *opaque)
{
    return static_cast<RiscvJitCPU *>(opaque)->backendReadTime();
}

int
RiscvJitCPU::shouldStopThunk(void *opaque)
{
    return static_cast<RiscvJitCPU *>(opaque)->backendShouldStop();
}

} // namespace gem5
