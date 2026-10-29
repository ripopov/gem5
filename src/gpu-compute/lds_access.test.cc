/*
 * Copyright (c) 2014-2015 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <gtest/gtest.h>

#include "gpu-compute/lds_access.hh"

namespace gem5
{
namespace
{

std::vector<Addr>
addresses(unsigned stride, unsigned lanes = 64)
{
    std::vector<Addr> result(lanes);
    for (unsigned lane = 0; lane < lanes; ++lane) {
        result[lane] = lane * stride;
    }
    return result;
}

TEST(LdsAccess, WordBanksAndBaselinePhases)
{
    for (unsigned stride : {4, 8, 16, 128}) {
        auto result =
            countLdsBankAccesses(addresses(stride), ~0ULL, {}, 32, true);
        EXPECT_EQ(result.accesses, 64);
        EXPECT_EQ(result.phases, 2);
        EXPECT_EQ(result.conflicts, 2 * (stride / 4 - 1));
    }
}

TEST(LdsAccess, MasksAndBroadcasts)
{
    auto addr = addresses(0);
    auto read = countLdsBankAccesses(addr, ~0ULL, {}, 32, true);
    EXPECT_EQ(read.accesses, 2);
    EXPECT_EQ(read.conflicts, 0);
    // Stores and modifying atomics cannot use the read broadcast path.
    auto write = countLdsBankAccesses(addr, ~0ULL, {}, 32, false);
    EXPECT_EQ(write.accesses, 64);
    EXPECT_EQ(write.conflicts, 62);
    auto empty = countLdsBankAccesses(addr, 0, {}, 32, true);
    EXPECT_EQ(empty.accesses, 0);
    EXPECT_EQ(empty.phases, 0);
    EXPECT_EQ(empty.conflicts, 0);
    auto masked =
        countLdsBankAccesses(addresses(128), 0x80000001, {}, 32, false);
    EXPECT_EQ(masked.accesses, 2);
    EXPECT_EQ(masked.phases, 1);
    EXPECT_EQ(masked.conflicts, 1);
}

TEST(LdsAccess, SubwordReadsShareAWord)
{
    auto read = countLdsBankAccesses(addresses(1, 32), ~0ULL,
                                     {1, 1, {0, 0}, false}, 32, true);
    EXPECT_EQ(read.accesses, 8);
    EXPECT_EQ(read.conflicts, 0);
    auto write = countLdsBankAccesses(addresses(1, 32), ~0ULL,
                                      {1, 1, {0, 0}, false}, 32, false);
    EXPECT_EQ(write.accesses, 32);
    EXPECT_EQ(write.conflicts, 3);
}

TEST(LdsAccess, WidthsAndDualOffsets)
{
    for (unsigned bytes : {4, 8, 16}) {
        LdsAccess access{bytes, 1, {12, 0}, false};
        auto result =
            countLdsBankAccesses(addresses(bytes), ~0ULL, access, 32, false);
        EXPECT_EQ(result.accesses, 64 * bytes / 4);
        EXPECT_EQ(result.phases, 2 * bytes / 4);
        EXPECT_EQ(result.conflicts, 0);
    }
    // The second immediate describes a separate access, not a contiguous
    // extension of the first; identical addresses in separate phases remain.
    LdsAccess dual{8, 2, {8, 64 * 8}, false};
    auto result = countLdsBankAccesses(addresses(8), ~0ULL, dual, 32, true);
    EXPECT_EQ(result.accesses, 256);
    EXPECT_EQ(result.phases, 8);
    EXPECT_EQ(result.conflicts, 0);
    // An immediate can move a subword access across a word boundary.
    result = countLdsBankAccesses({0}, 1, {2, 1, {3, 0}, false}, 32, true);
    EXPECT_EQ(result.accesses, 2);
}

TEST(LdsAccess, B128ReadPhases)
{
    const unsigned groups[8][8] = {
        {0, 1, 2, 3, 20, 21, 22, 23},     {4, 5, 6, 7, 16, 17, 18, 19},
        {8, 9, 10, 11, 28, 29, 30, 31},   {12, 13, 14, 15, 24, 25, 26, 27},
        {32, 33, 34, 35, 52, 53, 54, 55}, {36, 37, 38, 39, 48, 49, 50, 51},
        {40, 41, 42, 43, 60, 61, 62, 63}, {44, 45, 46, 47, 56, 57, 58, 59}};
    LdsAccess access{16, 1, {0, 0}, true};
    for (const auto &group : groups) {
        auto addr = addresses(128);
        uint64_t mask = 0;
        for (unsigned i = 0; i < 8; ++i) {
            addr[group[i]] += i * 16;
            mask |= uint64_t(1) << group[i];
        }
        auto result = countLdsBankAccesses(addr, mask, access, 32, true);
        EXPECT_EQ(result.phases, 1);
        EXPECT_EQ(result.accesses, 32);
        EXPECT_EQ(result.conflicts, 0);
        // Four distinct words per bank require four service rounds.
        for (unsigned i = 0; i < 8; ++i) {
            addr[group[i]] = group[i] * 128 + (i % 2) * 16;
        }
        result = countLdsBankAccesses(addr, mask, access, 32, true);
        EXPECT_EQ(result.conflicts, 3);
    }
}

TEST(LdsAccess, PaddingAndXor)
{
    // Eight rows of eight 16-byte packs, read down columns by a B128 phase.
    // Padding by one pack or XORing the pack column with the row removes
    // bank aliasing while preserving each pack's four adjacent words.
    const unsigned lanes[] = {0, 1, 2, 3, 20, 21, 22, 23};
    for (unsigned layout = 0; layout < 3; ++layout) {
        auto addr = addresses(0);
        uint64_t mask = 0;
        for (unsigned row = 0; row < 8; ++row) {
            const unsigned stride = layout == 1 ? 144 : 128;
            const unsigned col = layout == 2 ? row ^ 0 : 0;
            addr[lanes[row]] = row * stride + col * 16;
            mask |= uint64_t(1) << lanes[row];
        }
        auto result =
            countLdsBankAccesses(addr, mask, {16, 1, {0, 0}, true}, 32, true);
        EXPECT_EQ(result.conflicts, layout == 0 ? 7 : 0);
        EXPECT_EQ(result.accesses, 32);
    }
}

} // anonymous namespace
} // namespace gem5
