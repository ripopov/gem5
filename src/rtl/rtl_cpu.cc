/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/rtl_cpu.hh"

#include <string>
#include <vector>

#include "base/logging.hh"
#include "rtl/cpu_state.hh"
#include "rtl/rtl_core.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"

namespace gem5::rtl_cosim
{

RtlCpuSimObject::RtlCpuSimObject(const Params &params)
    : BaseCPU(params), _rtlCore(params.rtl_core)
{
    fatal_if(!_rtlCore, "%s: rtl_core must be configured", name());
    fatal_if(!_rtlCore->deferredForCpuSwitch(),
             "%s: rtl_core.defer_startup must be true", name());
    RtlCpuState *state = _rtlCore->cpuStateCapability();
    fatal_if(!state, "%s: vendor RTL core has no CPU-state capability",
             name());
    fatal_if(state->contextCount() != numThreads,
             "%s: vendor exposes %zu contexts but numThreads is %u", name(),
             state->contextCount(), numThreads);

    _threads.reserve(numThreads);
    for (ThreadID thread = 0; thread < numThreads; ++thread) {
        std::unique_ptr<SimpleThread> stateThread;
        if (FullSystem) {
            stateThread = std::make_unique<SimpleThread>(
                this, thread, params.system, params.mmu, params.isa[thread],
                params.decoder[thread]);
        } else {
            stateThread = std::make_unique<SimpleThread>(
                this, thread, params.system, params.workload[thread],
                params.mmu, params.isa[thread], params.decoder[thread]);
        }
        threadContexts.push_back(stateThread->getTC());
        _threads.push_back(std::move(stateThread));
    }
}

Port &
RtlCpuSimObject::getDataPort()
{
    return _rtlCore->defaultInitiatorPort();
}

Port &
RtlCpuSimObject::getInstPort()
{
    return _rtlCore->defaultInitiatorPort();
}

void
RtlCpuSimObject::wakeup(ThreadID thread)
{
    fatal_if(thread >= numThreads, "%s: invalid wakeup thread %u", name(),
             thread);
    _rtlCore->wake();
}

void
RtlCpuSimObject::takeOverFrom(BaseCPU *oldCpu)
{
    RtlCpuState *state = _rtlCore->cpuStateCapability();
    fatal_if(!state, "%s: vendor RTL core has no CPU-state capability",
             name());
    fatal_if(!state->schema() || !*state->schema(),
             "%s: vendor RTL core returned an empty CPU-state schema",
             name());
    fatal_if(state->contextCount() != numThreads,
             "%s: CPU-state context count changed before handover", name());

    std::string error;
    fatal_if(!findCpuStateEncoder(state->schema(), error),
             "%s: incompatible vendor CPU-state capability: %s", name(),
             error);

    // The RTL core starts with clean translation caches and keeps its own
    // preconnected memory topology. Architectural translation state (for
    // example, RISC-V satp and PMP CSRs) is encoded below with the rest of
    // the ThreadContext, so gem5 MMU/TLB state and walker ports must stay
    // behind.
    takeOverStateFrom(oldCpu, false);
    if (numThreads == 1) {
        _rtlCore->setRequestContextId(threadContexts[0]->contextId());
    }

    std::vector<std::vector<OwnedCpuStateValue>> bundles(numThreads);
    std::vector<std::vector<CpuStateValue>> views(numThreads);
    for (ThreadID thread = 0; thread < numThreads; ++thread) {
        fatal_if(!encodeCpuState(state->schema(), *threadContexts[thread],
                                 bundles[thread], error),
                 "%s: cannot encode thread %u as '%s': %s", name(), thread,
                 state->schema(), error);
        fatal_if(!makeCpuStateViews(bundles[thread], views[thread], error),
                 "%s: cannot prepare thread %u CPU state: %s", name(),
                 thread, error);
    }

    fatal_if(!_rtlCore->prepareCpuStateImport(error),
             "%s: cannot prepare RTL CPU for handover: %s", name(), error);
    for (ThreadID thread = 0; thread < numThreads; ++thread) {
        fatal_if(!_rtlCore->importCpuState(thread, views[thread], error),
                 "%s: vendor rejected thread %u CPU state: %s", name(),
                 thread, error);
    }
    _rtlCore->activateAfterCpuStateImport();
}

void
RtlCpuSimObject::verifyMemoryMode() const
{
    fatal_if(!system->isTimingMode(),
             "%s: RTL CPUs require the timing memory mode", name());
}

void
RtlCpuSimObject::serializeThread(CheckpointOut &, ThreadID) const
{
    fatal("%s: checkpointing an active RTL CPU requires state export, which "
          "is not supported by the PoC", name());
}

void
RtlCpuSimObject::unserializeThread(CheckpointIn &, ThreadID)
{
    fatal("%s: restoring an RTL CPU checkpoint is not supported by the PoC",
          name());
}

} // namespace gem5::rtl_cosim
