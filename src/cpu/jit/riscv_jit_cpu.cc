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

#include "cpu/jit/riscv_jit_cpu.hh"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <string>

#include "arch/riscv/isa.hh"
#include "arch/riscv/reg_abi.hh"
#include "arch/riscv/regs/float.hh"
#include "arch/riscv/regs/int.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/system.hh"
#include "base/logging.hh"
#include "cpu/utils.hh"
#include "debug/JitCPU.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "params/RiscvISA.hh"
#include "sim/pseudo_inst.hh"

namespace gem5
{

namespace
{

using MemoryRead = int (*)(void *, uint64_t, uint8_t *, size_t);
using MemoryWrite = int (*)(void *, uint64_t, const uint8_t *, size_t);
using MemoryMap = int (*)(void *, size_t, uint64_t *, uint64_t *, uint8_t **,
                          int *);
using RunBoundary = void (*)(void *);
using ReadTime = uint64_t (*)(void *);
using ShouldStop = int (*)(void *);

struct QemuJitCallbacks
{
    uint32_t instance_id;
    uint32_t instance_count;
    uint64_t hart_id;
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
    uint32_t instanceId = 0;

    using InitFn = int (*)(const QemuJitCallbacks *);
    using RunFn = int (*)(uint32_t, uint64_t, QemuJitRunResult *);
    using GetRegFn = uint64_t (*)(uint32_t, unsigned);
    using SetRegFn = int (*)(uint32_t, unsigned, uint64_t);
    using GetPcFn = uint64_t (*)(uint32_t);
    using SetPcFn = void (*)(uint32_t, uint64_t);
    using GetPrivFn = unsigned (*)(uint32_t);
    using SetPrivFn = int (*)(uint32_t, unsigned);
    using GetCsrFn = int (*)(uint32_t, unsigned, uint64_t *);
    using SetCsrFn = int (*)(uint32_t, unsigned, uint64_t);
    using GetMipFn = uint64_t (*)(uint32_t);
    using SetMipFn = void (*)(uint32_t, uint64_t);
    using InvalidateFn = void (*)(uint32_t);

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
        instanceId = callbacks.instance_id;
        fatal_if(initFn(&callbacks) != 0,
                 "JitCPU backend init failed for hart %llu",
                 static_cast<unsigned long long>(callbacks.hart_id));
    }

    QemuJitRunResult
    run(uint64_t instructions)
    {
        QemuJitRunResult result{};
        fatal_if(runFn(instanceId, instructions, &result) != 0,
                 "JitCPU backend execution failed");
        return result;
    }

    uint64_t getGpr(unsigned index) const
    {
        return getGprFn(instanceId, index);
    }
    uint64_t getFpr(unsigned index) const
    {
        return getFprFn(instanceId, index);
    }
    uint64_t getPc() const { return getPcFn(instanceId); }
    unsigned getPriv() const { return getPrivFn(instanceId); }
    uint64_t getMip() const { return getMipFn(instanceId); }

    void
    setGpr(unsigned index, uint64_t value)
    {
        fatal_if(setGprFn(instanceId, index, value),
                 "Unable to set QEMU GPR %u", index);
    }

    void
    setFpr(unsigned index, uint64_t value)
    {
        fatal_if(setFprFn(instanceId, index, value),
                 "Unable to set QEMU FPR %u", index);
    }

    void setPc(uint64_t value) { setPcFn(instanceId, value); }

    void
    setPriv(unsigned value)
    {
        fatal_if(setPrivFn(instanceId, value),
                 "Unable to set QEMU privilege %u", value);
    }

    uint64_t
    getCsr(unsigned csr) const
    {
        uint64_t value = 0;
        fatal_if(getCsrFn(instanceId, csr, &value),
                 "Unable to read QEMU CSR %#x", csr);
        return value;
    }

    void
    setCsr(unsigned csr, uint64_t value)
    {
        fatal_if(setCsrFn(instanceId, csr, value),
                 "Unable to set QEMU CSR %#x", csr);
    }

    void setMip(uint64_t value) { setMipFn(instanceId, value); }
    void invalidate() { invalidateFn(instanceId); }
};

std::unordered_set<const Event *> RiscvJitCPU::tickEvents;

bool
RiscvJitCPU::hasForeignEventAtOrBefore(const EventQueue *event_queue,
                                       Tick when)
{
    return event_queue->anyEventAtOrBefore(when, [](const Event *event) {
        return tickEvents.find(event) == tickEvents.end();
    });
}

RiscvJitCPU::RiscvJitCPU(const Params &p)
    : NonCachingSimpleCPU(p), batchSize(p.batch_size),
      backendInstance(p.backend_instance),
      backendInstanceCount(p.backend_instance_count)
{
    fatal_if(batchSize == 0, "JitCPU batch_size must be non-zero");
    fatal_if(backendInstanceCount == 0,
             "JitCPU backend_instance_count must be non-zero");
    fatal_if(backendInstance >= backendInstanceCount,
             "JitCPU backend_instance must be smaller than "
             "backend_instance_count");
    tickEvents.insert(&tickEvent);
    backend = std::make_unique<QemuJitBackend>(p.backend_path);
}

RiscvJitCPU::~RiscvJitCPU()
{
    tickEvents.erase(&tickEvent);
}

void
RiscvJitCPU::init()
{
    NonCachingSimpleCPU::init();

    ThreadContext *tc = threadContexts[0];
    auto *isa = dynamic_cast<RiscvISA::ISA *>(tc->getIsaPtr());
    fatal_if(!isa, "JitCPU requires a RISC-V ISA object");
    const RiscvISA::MISA misa(
        tc->readMiscRegNoEffect(RiscvISA::MISCREG_ISA));
    fatal_if(misa.rv64_mxl != 2, "JitCPU supports RV64 only");
    fatal_if(isa->reportsExtension("V"),
             "JitCPU does not support the RISC-V vector extension");
    fatal_if(isa->getPrivilegeModeSet() == enums::MHSU,
             "JitCPU does not support the RISC-V hypervisor extension");
    fatal_if(isa->reportsExtension("Zicbom") ||
                 isa->reportsExtension("Zicboz"),
             "JitCPU does not support Zicbom or Zicboz");
    fatal_if(isa->reportsExtension("Smrnmi"),
             "JitCPU does not support Smrnmi");

    const QemuJitCallbacks callbacks = {
        .instance_id = backendInstance,
        .instance_count = backendInstanceCount,
        .hart_id = static_cast<uint64_t>(tc->contextId()),
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
RiscvJitCPU::takeOverFrom(BaseCPU *old_cpu)
{
    NonCachingSimpleCPU::takeOverFrom(old_cpu);

    // The other CPU may have changed page tables without changing SATP, and
    // QEMU may still carry a halted state, a load reservation, an exception,
    // translated code, or software-TLB entries from the previous JIT phase.
    // Reset that non-architectural state before importing gem5's new state.
    backend->invalidate();
    syncToBackend();
}

void
RiscvJitCPU::syncToBackend()
{
    ThreadContext *tc = threadContexts[0];
    const unsigned privilege =
        tc->readMiscRegNoEffect(RiscvISA::MISCREG_PRV);

    // The backend reaches CSRs through QEMU's architectural accessors, which
    // enforce the current privilege mode. Raise the backend to M mode for the
    // duration of the transfer and restore the guest's mode at the end.
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
    const RegVal fflags =
        tc->readMiscRegNoEffect(RiscvISA::MISCREG_FFLAGS);
    const RegVal frm = tc->readMiscRegNoEffect(RiscvISA::MISCREG_FRM);
    if (backend->getCsr(RiscvISA::CSR_FFLAGS) != fflags) {
        backend->setCsr(RiscvISA::CSR_FFLAGS, fflags);
    }
    if (backend->getCsr(RiscvISA::CSR_FRM) != frm) {
        backend->setCsr(RiscvISA::CSR_FRM, frm);
    }
    pushedMip = tc->readMiscReg(RiscvISA::MISCREG_IP);
    backend->setMip(pushedMip);
    backend->setPriv(privilege);
}

void
RiscvJitCPU::syncFromBackend()
{
    ThreadContext *tc = threadContexts[0];
    const unsigned privilege = backend->getPriv();

    // Both sides check the current privilege mode on CSR access, so raise
    // both to M mode for the transfer and restore the guest's mode at the
    // end. See the matching comment in syncToBackend().
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
    const RegVal fflags = backend->getCsr(RiscvISA::CSR_FFLAGS);
    const RegVal frm = backend->getCsr(RiscvISA::CSR_FRM);
    if (tc->readMiscRegNoEffect(RiscvISA::MISCREG_FFLAGS) != fflags) {
        tc->setMiscReg(RiscvISA::MISCREG_FFLAGS, fflags);
    }
    if (tc->readMiscRegNoEffect(RiscvISA::MISCREG_FRM) != frm) {
        tc->setMiscReg(RiscvISA::MISCREG_FRM, frm);
    }

    // Interrupt-controller state is canonical in gem5.  In particular,
    // copying QEMU's sampled MSIP/MTIP/MEIP levels back after a device MMIO
    // clear would immediately re-post the stale interrupt.  Supervisor
    // pending bits may originate from guest CSR execution in QEMU; legacy
    // SBI firmware, for example, converts MTIP into STIP for Linux.
    //
    // Both sides can move a supervisor bit during one batch: a translated
    // MMIO access runs gem5 device code inline, and the PLIC recomputes its
    // supervisor external line synchronously from that access.  Importing the
    // whole mask would then overwrite the device level with the sample taken
    // before the batch.  Import only the supervisor bits QEMU actually
    // changed, which keeps every batch boundary consistent with what the
    // detailed CPU would have observed at the same point.
    constexpr RegVal guest_pending_mask =
        RiscvISA::SSI_MASK | RiscvISA::STI_MASK | RiscvISA::SEI_MASK;
    const RegVal backend_mip = backend->getMip();
    const RegVal gem5_mip = tc->readMiscReg(RiscvISA::MISCREG_IP);
    const RegVal import_mask = guest_pending_mask & ~(gem5_mip ^ pushedMip);
    const RegVal merged_mip =
        (gem5_mip & ~import_mask) | (backend_mip & import_mask);
    if (gem5_mip != merged_mip) {
        tc->setMiscReg(RiscvISA::MISCREG_IP, merged_mip);
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
        } else if (hasForeignEventAtOrBefore(event_queue, curTick())) {
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

    fatal_if(result.reason == QemuJitExitReason::Exception ||
                 result.reason == QemuJitExitReason::Error,
             "QEMU JitCPU execution stopped unexpectedly: exception=%d, "
             "pc=%#x",
             result.qemu_exception, info.thread->pcState().instAddr());

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
        uint64_t poll_instructions =
            std::max<uint64_t>(1, result.instructions);
        if (!event_queue->empty() && event_queue->nextTick() <= curTick() &&
            !hasForeignEventAtOrBefore(event_queue, curTick())) {
            // A halted peer at the same tick is not a reason to poll WFI
            // every cycle.  Bound the wakeup delay by the normal JIT
            // batch, matching the granularity already used while
            // executing instructions.
            poll_instructions = std::max(poll_instructions, batchSize);
        }
        Tick wakeup = clockEdge(Cycles(poll_instructions));
        if (!event_queue->empty()) {
            wakeup = std::max(wakeup, event_queue->nextTick());
        }
        // A suspended hart must always keep a tick scheduled: nothing else
        // reactivates it, because the gem5 context stays runnable while only
        // QEMU is halted.
        reschedule(tickEvent, wakeup, true);
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
RiscvJitCPU::physicalAccess(MemCmd::Command command, uint64_t address,
                            uint8_t *data, size_t size)
{
    while (size) {
        // An access outside backing memory reaches a gem5 device model, which
        // may schedule events. Remember it so the batch ends at the next
        // instruction boundary instead of running past those events.
        backendIoAccessed = backendIoAccessed || !system->isMemAddr(address);
        const size_t fragment = std::min<size_t>(
            size, cacheLineSize() - addrBlockOffset(address, cacheLineSize()));
        RequestPtr request = std::make_shared<Request>(
            address, fragment, Request::PHYSICAL, dataRequestorId());
        request->setContext(threadContexts[0]->contextId());
        Packet packet(request, command);
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
RiscvJitCPU::physicalMemoryMap(size_t index, uint64_t *guestAddress,
                               uint64_t *size, uint8_t **hostAddress,
                               int *writable)
{
    const auto stores = system->getPhysMem().getBackingStore();
    size_t eligible = 0;

    for (const auto &store : stores) {
        DPRINTF(JitCPU,
                "Backend store [%#x:%#x] kvmMap=%d pmem=%p "
                "interleaved=%d\n",
                store.range.start(), store.range.end(), store.kvmMap,
                store.pmem, store.range.interleaved());
        // PhysicalMemory merges interleaved ranges into the contiguous range
        // they tile and refuses to back an interleaved range at all, so a
        // backing store can never be interleaved. Direct mapping depends on
        // that: the host image must be linear in guest physical address.
        panic_if(store.range.interleaved(),
                 "JitCPU backing store %s is interleaved",
                 store.range.to_string());
        if (!store.kvmMap || !store.pmem) {
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
    backendIoAccessed = false;
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
    EventQueue *event_queue = eventQueue();
    return backendIoAccessed ||
           (!event_queue->empty() && event_queue->nextTick() <= curTick() &&
            hasForeignEventAtOrBefore(event_queue, curTick()));
}

int
RiscvJitCPU::memoryReadThunk(void *opaque, uint64_t address,
                             uint8_t *data, size_t size)
{
    return static_cast<RiscvJitCPU *>(opaque)->physicalAccess(
        MemCmd::ReadReq, address, data, size);
}

int
RiscvJitCPU::memoryWriteThunk(void *opaque, uint64_t address,
                              const uint8_t *data, size_t size)
{
    return static_cast<RiscvJitCPU *>(opaque)->physicalAccess(
        MemCmd::WriteReq, address, const_cast<uint8_t *>(data), size);
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
