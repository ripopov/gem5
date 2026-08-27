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

#include <string>

#include "base/logging.hh"
#include "cpu/thread_context.hh"
#include "qemu-jit.h"

namespace gem5
{

namespace
{

/** The QEMU/TCG backend, reached through its public C ABI. */
class QemuJitBackend : public RiscvBackend
{
  public:
    QemuJitBackend(const std::string &path, const Gem5QemuJitCallbacks &cb)
        : instanceId(cb.instance_id)
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
        invalidateFn =
            symbol<InvalidateFn>("gem5_qemu_jit_invalidate_translations");

        fatal_if(initFn(&cb) != 0,
                 "JitCPU backend init failed for hart %llu",
                 static_cast<unsigned long long>(cb.hart_id));
    }

    ~QemuJitBackend() override
    {
        /* QEMU owns a live TCG worker thread for the process lifetime. Do
         * not unload its text while that thread exists. */
    }

    RiscvBackendResult
    run(uint64_t instructions) override
    {
        Gem5QemuJitRunResult raw{};
        fatal_if(runFn(instanceId, instructions, &raw) != 0,
                 "JitCPU backend execution failed");

        RiscvBackendResult result{};
        result.instructions = raw.instructions;
        result.m5Function = raw.m5_function;
        result.backendException = raw.qemu_exception;
        switch (raw.reason) {
          case GEM5_QEMU_JIT_EXIT_HALTED:
            result.reason = RiscvBackendExit::Halted;
            break;
          case GEM5_QEMU_JIT_EXIT_INTERRUPT:
            result.reason = RiscvBackendExit::Interrupt;
            break;
          case GEM5_QEMU_JIT_EXIT_EXCEPTION:
            result.reason = RiscvBackendExit::Exception;
            break;
          case GEM5_QEMU_JIT_EXIT_ERROR:
            result.reason = RiscvBackendExit::Error;
            break;
          case GEM5_QEMU_JIT_EXIT_M5OP:
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
                 "Unable to set QEMU GPR %u", index);
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
                 "Unable to set QEMU FPR %u", index);
    }

    uint64_t getPc() const override { return getPcFn(instanceId); }
    void setPc(uint64_t value) override { setPcFn(instanceId, value); }
    unsigned getPriv() const override { return getPrivFn(instanceId); }

    void
    setPriv(unsigned value) override
    {
        fatal_if(setPrivFn(instanceId, value),
                 "Unable to set QEMU privilege %u", value);
    }

    uint64_t
    getCsr(unsigned csr) const override
    {
        uint64_t value = 0;
        fatal_if(getCsrFn(instanceId, csr, &value),
                 "Unable to read QEMU CSR %#x", csr);
        return value;
    }

    void
    setCsr(unsigned csr, uint64_t value) override
    {
        fatal_if(setCsrFn(instanceId, csr, value),
                 "Unable to set QEMU CSR %#x", csr);
    }

    uint64_t getMip() const override { return getMipFn(instanceId); }
    void setMip(uint64_t value) override { setMipFn(instanceId, value); }
    void invalidate() override { invalidateFn(instanceId); }

  private:
    using InitFn = decltype(&gem5_qemu_jit_init);
    using RunFn = decltype(&gem5_qemu_jit_run);
    using GetRegFn = decltype(&gem5_qemu_jit_get_gpr);
    using SetRegFn = decltype(&gem5_qemu_jit_set_gpr);
    using GetPcFn = decltype(&gem5_qemu_jit_get_pc);
    using SetPcFn = decltype(&gem5_qemu_jit_set_pc);
    using GetPrivFn = decltype(&gem5_qemu_jit_get_priv);
    using SetPrivFn = decltype(&gem5_qemu_jit_set_priv);
    using GetCsrFn = decltype(&gem5_qemu_jit_get_csr);
    using SetCsrFn = decltype(&gem5_qemu_jit_set_csr);
    using GetMipFn = decltype(&gem5_qemu_jit_get_mip);
    using SetMipFn = decltype(&gem5_qemu_jit_set_mip);
    using InvalidateFn = decltype(&gem5_qemu_jit_invalidate_translations);

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
RiscvJitCPU::connectBackend()
{
    const Gem5QemuJitCallbacks callbacks = {
        .instance_id = backendInstance,
        .instance_count = backendInstanceCount,
        .hart_id = static_cast<uint64_t>(threadContexts[0]->contextId()),
        .memory_read = memoryReadThunk,
        .memory_write = memoryWriteThunk,
        .memory_map = memoryMapThunk,
        .run_begin = runBeginThunk,
        .run_end = runEndThunk,
        .read_time = readTimeThunk,
        .should_stop = shouldStopThunk,
        .opaque = this,
    };
    return std::make_unique<QemuJitBackend>(backendPath, callbacks);
}

} // namespace gem5
