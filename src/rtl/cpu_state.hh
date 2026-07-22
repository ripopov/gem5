/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_CPU_STATE_HH__
#define __RTL_CPU_STATE_HH__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gem5/rtl_cosim/api_v1.hh"

namespace gem5
{

class ThreadContext;

namespace rtl_cosim
{

struct OwnedCpuStateValue
{
    std::string name;
    std::size_t bitWidth = 0;
    std::vector<std::uint8_t> data;
};

using CpuStateEncoder = bool (*)(ThreadContext &context,
                                 std::vector<OwnedCpuStateValue> &values,
                                 std::string &error);

bool registerCpuStateEncoder(const std::string &schema,
                             CpuStateEncoder encoder);

CpuStateEncoder findCpuStateEncoder(const char *schema, std::string &error);

bool encodeCpuState(const char *schema, ThreadContext &context,
                    std::vector<OwnedCpuStateValue> &values,
                    std::string &error);

bool makeCpuStateViews(const std::vector<OwnedCpuStateValue> &owned,
                       std::vector<CpuStateValue> &views,
                       std::string &error);

} // namespace rtl_cosim
} // namespace gem5

#endif // __RTL_CPU_STATE_HH__
