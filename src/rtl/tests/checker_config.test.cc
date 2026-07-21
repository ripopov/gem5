/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <limits>

#include "rtl/checker/checker_config.hh"

namespace gem5::rtl_cosim
{
namespace
{

TEST(CheckerConfig, ParsesBidirectionalConfiguration)
{
    constexpr std::string_view text = R"json(
      {
        "core": {"config": {"kind": "fixture", "width": 64}},
        "reset": {"assert_cycles": 3},
        "inputs": {"reset_vector": "0x1234"},
        "memory": {
          "base": 4096,
          "size": 8192,
          "latency_cycles": 2,
          "max_pending": 8,
          "error_ranges": [{"base": 6144, "size": 16}]
        },
        "transactions": {
          "target": [
            {"address": 4096, "write": true, "beat_bytes": 4,
             "data": "01020304"},
            {"address": 4096, "beat_bytes": 4, "beats": 2, "id": 3,
             "expect_data": "0102030405060708"}
          ]
        },
        "run": {"max_cycles": 100, "stop_on_idle": true}
      }
    )json";
    CheckerConfig config;
    std::string error;
    ASSERT_TRUE(parseCheckerConfig(text, config, error)) << error;
    EXPECT_EQ(config.resetAssertCycles, 3);
    EXPECT_EQ(config.memoryBase, 4096);
    EXPECT_EQ(config.memory.latencyCycles, 2);
    ASSERT_EQ(config.transactions.at("target").size(), 2);
    EXPECT_EQ(config.transactions.at("target")[0].request.data,
              (std::vector<std::uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(config.transactions.at("target")[1].request.id, 3);
    EXPECT_EQ(config.transactions.at("target")[1].request.beatCount(), 2);
    ASSERT_TRUE(config.transactions.at("target")[1].expectation.data);
    EXPECT_EQ(*config.transactions.at("target")[1].expectation.data,
              (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(CheckerConfig, RejectsUnknownAndMalformedFields)
{
    CheckerConfig config;
    std::string error;
    EXPECT_FALSE(parseCheckerConfig(R"({"mystery": 1})", config, error));
    EXPECT_NE(error.find("unknown"), std::string::npos);
    error.clear();
    EXPECT_FALSE(
        parseCheckerConfig(R"({"run":{"max_cycles":0}})", config, error));
    EXPECT_NE(error.find("positive"), std::string::npos);
}

TEST(CheckerConfig, ConvertsArbitraryWidthSignalValues)
{
    json::Value value;
    std::string error;
    ASSERT_TRUE(json::parse(R"("0x123456789abcdef001")", value, error));
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(valueToSignalBytes(value, 72, bytes, error)) << error;
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{1, 0xf0, 0xde, 0xbc, 0x9a,
                                                0x78, 0x56, 0x34, 0x12}));

    ASSERT_TRUE(json::parse("256", value, error));
    EXPECT_FALSE(valueToSignalBytes(value, 8, bytes, error));
}

TEST(CheckerConfig, RejectsOversizedTransactionsAndDuplicateTokens)
{
    CheckerConfig config;
    std::string error;
    EXPECT_FALSE(parseCheckerConfig(R"json(
      {"transactions":{"target":[
        {"address":0,"beat_bytes":18446744073709551615,"beats":2}
      ]}}
    )json",
                                    config, error));
    EXPECT_FALSE(error.empty());

    EXPECT_FALSE(parseCheckerConfig(R"json(
      {"transactions":{"target":[
        {"token":7,"address":0,"beat_bytes":4},
        {"token":7,"address":4,"beat_bytes":4}
      ]}}
    )json",
                                    config, error));
    EXPECT_NE(error.find("duplicate token"), std::string::npos);
}

TEST(Json, RejectsDuplicateMembersAndPreservesIntegers)
{
    json::Value value;
    std::string error;
    EXPECT_FALSE(json::parse(R"({"x":1,"x":2})", value, error));
    ASSERT_TRUE(json::parse(R"({"x":18446744073709551615})", value, error));
    std::uint64_t number = 0;
    ASSERT_TRUE(json::unsignedInteger(*value.find("x"), number, error));
    EXPECT_EQ(number, std::numeric_limits<std::uint64_t>::max());
}

} // anonymous namespace
} // namespace gem5::rtl_cosim
