/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <array>

#include <gtest/gtest.h>

#include "rtl/checker/memory_backend.hh"

namespace gem5::rtl_cosim
{
namespace
{

MemoryRequest
writeRequest(std::uint64_t token, std::uint64_t address,
             std::vector<std::uint8_t> data, std::vector<std::uint8_t> enables)
{
    MemoryRequest request;
    request.token = token;
    request.address = address;
    request.write = true;
    request.beatBytes = 4;
    request.data = std::move(data);
    request.byteEnable = std::move(enables);
    return request;
}

TEST(MemoryBackend, AppliesByteEnablesAndLatency)
{
    auto storage = std::make_shared<SparseMemory>(0x1000, 0x1000);
    MemoryBackend backend(
        storage, {.latencyCycles = 2, .maxPending = 4, .errorRanges = {}});
    MemoryRequest request =
        writeRequest(7, 0x1010, {1, 2, 3, 4}, {1, 0, 1, 0});
    ASSERT_TRUE(backend.submit(request));
    MemoryResponse response;
    backend.advance();
    EXPECT_FALSE(backend.getResponse(response));
    backend.advance();
    ASSERT_TRUE(backend.getResponse(response));
    EXPECT_EQ(response.token, 7);
    std::uint8_t bytes[4] = {};
    ASSERT_TRUE(storage->read(0x1010, bytes, sizeof(bytes)));
    EXPECT_EQ(std::vector<std::uint8_t>(bytes, bytes + 4),
              (std::vector<std::uint8_t>{1, 0, 3, 0}));
}

TEST(MemoryBackend, HandlesIncrementingAndFixedBursts)
{
    auto storage = std::make_shared<SparseMemory>(0, 0x1000);
    MemoryBackend backend(
        storage, {.latencyCycles = 0, .maxPending = 64, .errorRanges = {}});
    MemoryRequest write = writeRequest(1, 0x20, {1, 2, 3, 4, 5, 6, 7, 8},
                                       std::vector<std::uint8_t>(8, 1));
    ASSERT_TRUE(backend.submit(write));
    backend.advance();
    MemoryResponse response;
    ASSERT_TRUE(backend.getResponse(response));

    MemoryRequest read;
    read.token = 2;
    read.address = 0x20;
    read.beatBytes = 4;
    read.byteEnable.assign(8, 1);
    ASSERT_TRUE(backend.submit(read));
    backend.advance();
    ASSERT_TRUE(backend.getResponse(response));
    EXPECT_EQ(response.data,
              (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));

    write.token = 3;
    write.address = 0x40;
    write.burst = BurstType::Fixed;
    ASSERT_TRUE(backend.submit(write));
    backend.advance();
    ASSERT_TRUE(backend.getResponse(response));
    std::uint8_t fixed[4] = {};
    ASSERT_TRUE(storage->read(0x40, fixed, 4));
    EXPECT_EQ(std::vector<std::uint8_t>(fixed, fixed + 4),
              (std::vector<std::uint8_t>{5, 6, 7, 8}));
}

TEST(MemoryBackend, HandlesWrappingBursts)
{
    auto storage = std::make_shared<SparseMemory>(0, 0x1000);
    MemoryBackend backend(
        storage, {.latencyCycles = 0, .maxPending = 4, .errorRanges = {}});
    MemoryRequest write = writeRequest(
        9, 0x2c, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
        std::vector<std::uint8_t>(16, 1));
    write.burst = BurstType::Wrap;
    ASSERT_TRUE(backend.submit(write));
    backend.advance();
    MemoryResponse response;
    ASSERT_TRUE(backend.getResponse(response));
    std::array<std::uint8_t, 16> bytes{};
    ASSERT_TRUE(storage->read(0x20, bytes.data(), bytes.size()));
    EXPECT_EQ(bytes,
              (std::array<std::uint8_t, 16>{5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                                            15, 16, 1, 2, 3, 4}));
}

TEST(MemoryBackend, ReturnsConfiguredAndRangeErrors)
{
    auto storage = std::make_shared<SparseMemory>(0x100, 0x100);
    MemoryBackend backend(
        storage,
        {.latencyCycles = 0, .maxPending = 4, .errorRanges = {{0x140, 4}}});
    MemoryRequest read;
    read.token = 1;
    read.address = 0x140;
    read.beatBytes = 4;
    read.byteEnable.assign(4, 1);
    ASSERT_TRUE(backend.submit(read));
    backend.advance();
    MemoryResponse response;
    ASSERT_TRUE(backend.getResponse(response));
    EXPECT_TRUE(response.error);
    read.token = 2;
    read.address = 0x200;
    ASSERT_TRUE(backend.submit(read));
    backend.advance();
    ASSERT_TRUE(backend.getResponse(response));
    EXPECT_TRUE(response.error);
}

TEST(Transaction, RejectsMalformedShapes)
{
    std::string error;
    MemoryRequest request;
    EXPECT_FALSE(request.valid(error));
    request.write = true;
    request.beatBytes = 4;
    request.data = {1, 2, 3, 4};
    request.byteEnable = {1, 1};
    EXPECT_FALSE(request.valid(error));
    request.byteEnable.assign(4, 1);
    request.burst = static_cast<BurstType>(99);
    EXPECT_FALSE(request.valid(error));
}

TEST(TransactionSource, EnforcesOutstandingLimitAndExpectations)
{
    ScriptedTransactionSource source(1);
    MemoryRequest first;
    first.token = 1;
    first.address = 0;
    first.beatBytes = 4;
    first.byteEnable.assign(4, 1);
    MemoryRequest second = first;
    second.token = 2;
    second.address = 4;
    source.add(first, {.error = false,
                       .data = std::vector<std::uint8_t>{1, 2, 3, 4}});
    source.add(second);

    MemoryRequest request;
    ASSERT_TRUE(source.getRequest(request));
    EXPECT_EQ(request.token, 1);
    EXPECT_FALSE(source.getRequest(request));
    EXPECT_TRUE(source.submitResponse({1, 0, {1, 2, 3, 4}, false}));
    ASSERT_TRUE(source.getRequest(request));
    EXPECT_EQ(request.token, 2);
    EXPECT_FALSE(source.submitResponse({2, 0, {9, 9, 9, 9}, true}));
    EXPECT_NE(source.error().find("error status"), std::string::npos);
}

} // anonymous namespace
} // namespace gem5::rtl_cosim
