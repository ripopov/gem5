/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_COSIM_RUNTIME_IMAGE_LOADER_HH__
#define __RTL_COSIM_RUNTIME_IMAGE_LOADER_HH__

#include <cstdint>
#include <string>
#include <vector>

namespace gem5::rtl_cosim
{

struct ImageSegment
{
    std::uint64_t address = 0;
    std::vector<std::uint8_t> data;
    std::uint64_t memorySize = 0;
};

bool loadImage(const std::string &path, const std::string &format,
               std::uint64_t rawAddress, std::vector<ImageSegment> &segments,
               std::string &error);

} // namespace gem5::rtl_cosim

#endif // __RTL_COSIM_RUNTIME_IMAGE_LOADER_HH__
