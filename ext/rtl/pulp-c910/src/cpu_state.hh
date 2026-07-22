/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __EXT_RTL_PULP_C910_CPU_STATE_HH__
#define __EXT_RTL_PULP_C910_CPU_STATE_HH__

#include <array>
#include <cstddef>
#include <cstdint>

#include "gem5/rtl_cosim/api_v1.hh"

class VerilatedContext;
class Vrtl_cosim_c910_top;

/** PULP C910 architectural-state import through the vendor HAD/JTAG unit. */
class C910CpuState final : public RtlCpuState
{
  public:
    C910CpuState(Vrtl_cosim_c910_top &top, VerilatedContext &context);

    const char *schema() const noexcept override;
    std::size_t contextCount() const noexcept override;
    bool importState(std::size_t context, const CpuStateValue *values,
                     std::size_t valueCount) noexcept override;
    const char *getLastError() const noexcept override;

  private:
    void setError(const char *format, ...) noexcept;

    Vrtl_cosim_c910_top &_top;
    VerilatedContext &_context;
    std::array<char, 512> _error{};
    bool _imported = false;
};

#endif // __EXT_RTL_PULP_C910_CPU_STATE_HH__
