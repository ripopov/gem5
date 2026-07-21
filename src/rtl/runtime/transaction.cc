/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/runtime/transaction.hh"

namespace gem5::rtl_cosim
{

std::size_t
MemoryRequest::beatCount() const noexcept
{
    if (beatBytes == 0) {
        return 0;
    }
    const std::size_t bytes = write ? data.size() : byteEnable.size();
    return bytes / beatBytes;
}

bool
MemoryRequest::valid(std::string &error) const
{
    if (burst != BurstType::Fixed && burst != BurstType::Increment &&
        burst != BurstType::Wrap) {
        error = "transaction has an invalid burst type";
        return false;
    }
    if (beatBytes == 0) {
        error = "transaction beat size is zero";
        return false;
    }
    if (write && data.empty()) {
        error = "write transaction has no data";
        return false;
    }
    const std::size_t bytes = write ? data.size() : byteEnable.size();
    if (bytes == 0 || bytes % beatBytes != 0) {
        error = "transaction length is not a positive number of beats";
        return false;
    }
    if (write && byteEnable.size() != data.size()) {
        error = "write byte-enable length does not match data length";
        return false;
    }
    if (!write && !data.empty()) {
        error = "read request must not contain data";
        return false;
    }
    return true;
}

} // namespace gem5::rtl_cosim
