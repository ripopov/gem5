/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_SIGNAL_VALUE_HH__
#define __RTL_SIGNAL_VALUE_HH__

#include <cstdint>
#include <vector>

namespace gem5::rtl_cosim
{

/** A standalone RTL signal value in V1 little-endian byte/bit order. */
struct SignalValue
{
    std::uint32_t bitWidth = 0;
    std::vector<std::uint8_t> data;

    bool
    operator==(const SignalValue &other) const noexcept
    {
        return bitWidth == other.bitWidth && data == other.data;
    }
};

} // namespace gem5::rtl_cosim

#endif // __RTL_SIGNAL_VALUE_HH__
