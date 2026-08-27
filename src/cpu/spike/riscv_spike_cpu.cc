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

#include "cpu/spike/riscv_spike_cpu.hh"

#include <dlfcn.h>

#include <string>

#include "base/logging.hh"
#include "cpu/thread_context.hh"
#include "gem5-spike.h"

namespace gem5
{

namespace
{

/** The Spike backend, reached through its public C ABI. */
class SpikeBackend : public RiscvBackend
{
  public:
    SpikeBackend(const std::string &path, const Gem5SpikeCallbacks &cb)
        : instanceId(cb.instance_id)
    {
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        fatal_if(!handle, "Unable to load SpikeCPU backend '%s': %s", path,
                 dlerror());

        initFn = symbol<InitFn>("gem5_spike_init");
        runFn = symbol<RunFn>("gem5_spike_run");
        getGprFn = symbol<GetRegFn>("gem5_spike_get_gpr");
        setGprFn = symbol<SetRegFn>("gem5_spike_set_gpr");
        getFprFn = symbol<GetRegFn>("gem5_spike_get_fpr");
        setFprFn = symbol<SetRegFn>("gem5_spike_set_fpr");
        getPcFn = symbol<GetPcFn>("gem5_spike_get_pc");
        setPcFn = symbol<SetPcFn>("gem5_spike_set_pc");
        getPrivFn = symbol<GetPrivFn>("gem5_spike_get_priv");
        setPrivFn = symbol<SetPrivFn>("gem5_spike_set_priv");
        getCsrFn = symbol<GetCsrFn>("gem5_spike_get_csr");
        setCsrFn = symbol<SetCsrFn>("gem5_spike_set_csr");
        getMipFn = symbol<GetMipFn>("gem5_spike_get_mip");
        setMipFn = symbol<SetMipFn>("gem5_spike_set_mip");
        invalidateFn =
            symbol<InvalidateFn>("gem5_spike_invalidate_translations");

        fatal_if(initFn(&cb) != 0,
                 "SpikeCPU backend init failed for hart %llu",
                 static_cast<unsigned long long>(cb.hart_id));
    }

    ~SpikeBackend() override
    {
        /* The harts outlive this object only until the process exits, and
         * unloading the image would invalidate their vtables. */
    }

    RiscvBackendResult
    run(uint64_t instructions) override
    {
        Gem5SpikeRunResult raw{};
        fatal_if(runFn(instanceId, instructions, &raw) != 0,
                 "SpikeCPU backend execution failed");

        RiscvBackendResult result{};
        result.instructions = raw.instructions;
        result.m5Function = raw.m5_function;
        result.backendException = raw.spike_exception;
        switch (raw.reason) {
          case GEM5_SPIKE_EXIT_HALTED:
            result.reason = RiscvBackendExit::Halted;
            break;
          case GEM5_SPIKE_EXIT_INTERRUPT:
            result.reason = RiscvBackendExit::Interrupt;
            break;
          case GEM5_SPIKE_EXIT_EXCEPTION:
            result.reason = RiscvBackendExit::Exception;
            break;
          case GEM5_SPIKE_EXIT_ERROR:
            result.reason = RiscvBackendExit::Error;
            break;
          case GEM5_SPIKE_EXIT_M5OP:
            result.reason = RiscvBackendExit::M5Op;
            break;
          default:
            result.reason = RiscvBackendExit::Budget;
            break;
        }
        return result;
    }

    uint64_t
    getGpr(unsigned index) const override
    {
        return getGprFn(instanceId, index);
    }

    void
    setGpr(unsigned index, uint64_t value) override
    {
        fatal_if(setGprFn(instanceId, index, value),
                 "Unable to set Spike GPR %u", index);
    }

    uint64_t
    getFpr(unsigned index) const override
    {
        return getFprFn(instanceId, index);
    }

    void
    setFpr(unsigned index, uint64_t value) override
    {
        fatal_if(setFprFn(instanceId, index, value),
                 "Unable to set Spike FPR %u", index);
    }

    uint64_t getPc() const override { return getPcFn(instanceId); }
    void setPc(uint64_t value) override { setPcFn(instanceId, value); }
    unsigned getPriv() const override { return getPrivFn(instanceId); }

    void
    setPriv(unsigned value) override
    {
        fatal_if(setPrivFn(instanceId, value),
                 "Unable to set Spike privilege %u", value);
    }

    /*
     * The adapter reads and writes Spike's CSR objects directly, so a
     * transfer needs no privilege change. That matters: Spike flushes its
     * software TLB and decode cache whenever the privilege mode moves, and
     * raising it to M twice per batch would throw away the caches that make
     * the interpreter fast.
     */
    bool csrAccessIsPrivileged() const override { return false; }

    uint64_t
    getCsr(unsigned csr) const override
    {
        uint64_t value = 0;
        fatal_if(getCsrFn(instanceId, csr, &value),
                 "Unable to read Spike CSR %#x", csr);
        return value;
    }

    void
    setCsr(unsigned csr, uint64_t value) override
    {
        fatal_if(setCsrFn(instanceId, csr, value),
                 "Unable to set Spike CSR %#x", csr);
    }

    uint64_t getMip() const override { return getMipFn(instanceId); }
    void setMip(uint64_t value) override { setMipFn(instanceId, value); }
    void invalidate() override { invalidateFn(instanceId); }

  private:
    using InitFn = decltype(&gem5_spike_init);
    using RunFn = decltype(&gem5_spike_run);
    using GetRegFn = decltype(&gem5_spike_get_gpr);
    using SetRegFn = decltype(&gem5_spike_set_gpr);
    using GetPcFn = decltype(&gem5_spike_get_pc);
    using SetPcFn = decltype(&gem5_spike_set_pc);
    using GetPrivFn = decltype(&gem5_spike_get_priv);
    using SetPrivFn = decltype(&gem5_spike_set_priv);
    using GetCsrFn = decltype(&gem5_spike_get_csr);
    using SetCsrFn = decltype(&gem5_spike_set_csr);
    using GetMipFn = decltype(&gem5_spike_get_mip);
    using SetMipFn = decltype(&gem5_spike_set_mip);
    using InvalidateFn = decltype(&gem5_spike_invalidate_translations);

    template <typename T>
    T
    symbol(const char *name)
    {
        dlerror();
        void *address = dlsym(handle, name);
        const char *error = dlerror();
        fatal_if(error || !address, "SpikeCPU backend symbol %s: %s", name,
                 error ? error : "not found");
        return reinterpret_cast<T>(address);
    }

    void *handle = nullptr;
    const uint32_t instanceId;

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
};

} // anonymous namespace

std::unique_ptr<RiscvBackend>
RiscvSpikeCPU::connectBackend()
{
    // Spike builds its hart from an ISA string and learns its translation
    // modes from a device tree. gem5 already knows both, so they are
    // reported from this CPU's own ISA object and nothing is configured
    // twice.
    //
    // Misaligned accesses are deliberately left trapping, which is what
    // Spike does without Zicclsm and what gem5's own CPU models do unless
    // PMAChecker.misaligned names a range. Matching gem5 matters more here
    // than matching the other functional backend.
    const std::string isa = riscvIsaString();
    const Gem5SpikeCallbacks callbacks = {
        .instance_id = backendInstance,
        .instance_count = backendInstanceCount,
        .hart_id = static_cast<uint64_t>(threadContexts[0]->contextId()),
        .isa_string = isa.c_str(),
        .privilege_string = privilegeModeString(),
        .max_vaddr_bits = maxVirtualAddressBits(),
        .memory_read = memoryReadThunk,
        .memory_write = memoryWriteThunk,
        .memory_map = memoryMapThunk,
        .run_begin = runBeginThunk,
        .run_end = runEndThunk,
        .read_time = readTimeThunk,
        .should_stop = shouldStopThunk,
        .opaque = this,
    };
    return std::make_unique<SpikeBackend>(backendPath, callbacks);
}

} // namespace gem5
