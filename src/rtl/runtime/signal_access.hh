/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_COSIM_RUNTIME_SIGNAL_ACCESS_HH__
#define __RTL_COSIM_RUNTIME_SIGNAL_ACCESS_HH__

#include <cstdint>
#include <string>
#include <vector>

#include "gem5/rtl_cosim/api_v1.hh"

namespace gem5::rtl_cosim
{

std::size_t signalBytes(const Signal &signal) noexcept;
bool readSignal(const Signal &signal, std::vector<std::uint8_t> &value,
                std::string &error);
bool writeSignal(Signal &signal, const std::vector<std::uint8_t> &value,
                 std::string &error);
bool readSignalU64(const Signal &signal, std::uint64_t &value,
                   std::string &error);
bool writeSignalU64(Signal &signal, std::uint64_t value, std::string &error);

} // namespace gem5::rtl_cosim

#endif // __RTL_COSIM_RUNTIME_SIGNAL_ACCESS_HH__
