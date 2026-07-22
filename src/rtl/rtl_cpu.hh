/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_RTL_CPU_HH__
#define __RTL_RTL_CPU_HH__

#include <memory>
#include <vector>

#include "cpu/base.hh"
#include "cpu/simple_thread.hh"
#include "params/RtlCpuSimObject.hh"

namespace gem5::rtl_cosim
{

class RtlCoreSimObject;

class RtlCpuSimObject final : public BaseCPU
{
  public:
    PARAMS(RtlCpuSimObject);
    explicit RtlCpuSimObject(const Params &params);
    ~RtlCpuSimObject() override = default;

    Port &getDataPort() override;
    Port &getInstPort() override;
    void wakeup(ThreadID thread) override;
    void takeOverFrom(BaseCPU *oldCpu) override;
    void verifyMemoryMode() const override;

    Counter totalInsts() const override { return 0; }
    Counter totalOps() const override { return 0; }
    void serializeThread(CheckpointOut &cp, ThreadID thread) const override;
    void unserializeThread(CheckpointIn &cp, ThreadID thread) override;

  private:
    RtlCoreSimObject *_rtlCore;
    std::vector<std::unique_ptr<SimpleThread>> _threads;
};

} // namespace gem5::rtl_cosim

#endif // __RTL_RTL_CPU_HH__
