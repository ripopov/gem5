/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gem5/rtl_cosim/api_v1.hh"
#include "rtl/cpu_state.hh"
#include "rtl/tests/test_model.hh"

namespace gem5::rtl_cosim
{
namespace
{

class TestCpuState final : public RtlCpuState
{
  public:
    const char *schema() const noexcept override { return "riscv64/v1"; }
    std::size_t contextCount() const noexcept override { return 1; }

    bool
    importState(std::size_t context, const CpuStateValue *values,
                std::size_t valueCount) noexcept override
    {
        if (context != 0 || !values || valueCount != 2) {
            error = "invalid state bundle";
            return false;
        }

        std::unordered_map<std::string, std::uint64_t> candidate;
        for (std::size_t index = 0; index < valueCount; ++index) {
            const CpuStateValue &value = values[index];
            if (!value.name || value.bitWidth != 64 || value.dataSize != 8 ||
                !value.data || candidate.contains(value.name)) {
                error = "malformed state value";
                return false;
            }
            std::uint64_t decoded = 0;
            for (std::size_t byte = 0; byte < value.dataSize; ++byte) {
                decoded |= static_cast<std::uint64_t>(value.data[byte]) <<
                           (byte * 8);
            }
            candidate.emplace(value.name, decoded);
        }
        if (!candidate.contains("pc") || !candidate.contains("x1")) {
            error = "missing required state value";
            return false;
        }

        imported = std::move(candidate);
        error.clear();
        return true;
    }

    const char *getLastError() const noexcept override
    {
        return error.c_str();
    }

    std::unordered_map<std::string, std::uint64_t> imported;
    std::string error;
};

class CpuTestCore final : public test::TestCore
{
  public:
    RtlCpuState *cpuState() noexcept override { return &state; }

    TestCpuState state;
};

std::array<std::uint8_t, 8>
bytes(std::uint64_t value)
{
    std::array<std::uint8_t, 8> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
    return result;
}

TEST(CpuStateApi, IsOptionalForExistingVendors)
{
    test::TestCore core;
    EXPECT_EQ(core.cpuState(), nullptr);
}

TEST(CpuStateApi, ImportsNamedLittleEndianValuesAtomically)
{
    CpuTestCore core;
    RtlCpuState *state = core.cpuState();
    ASSERT_NE(state, nullptr);
    EXPECT_STREQ(state->schema(), "riscv64/v1");
    EXPECT_EQ(state->contextCount(), 1);

    const auto pc = bytes(UINT64_C(0x1122334455667788));
    const auto x1 = bytes(UINT64_C(0x8877665544332211));
    const std::array values = {
        CpuStateValue{"pc", 64, pc.data(), pc.size()},
        CpuStateValue{"x1", 64, x1.data(), x1.size()},
    };
    ASSERT_TRUE(state->importState(0, values.data(), values.size()))
        << state->getLastError();
    EXPECT_EQ(core.state.imported.at("pc"), UINT64_C(0x1122334455667788));
    EXPECT_EQ(core.state.imported.at("x1"), UINT64_C(0x8877665544332211));

    const auto before = core.state.imported;
    EXPECT_FALSE(state->importState(1, values.data(), values.size()));
    EXPECT_EQ(core.state.imported, before);
    const std::array malformed = {
        CpuStateValue{"pc", 64, pc.data(), pc.size()},
        CpuStateValue{"pc", 64, x1.data(), x1.size()},
    };
    EXPECT_FALSE(state->importState(0, malformed.data(), malformed.size()));
    EXPECT_EQ(core.state.imported, before);
    EXPECT_NE(std::string(state->getLastError()).find("malformed"),
              std::string::npos);
}

TEST(CpuStateApi, RejectsMalformedOwnedBundles)
{
    std::string error;
    std::vector<CpuStateValue> views;

    std::vector<OwnedCpuStateValue> duplicate = {
        {"pc", 64, std::vector<std::uint8_t>(8)},
        {"pc", 64, std::vector<std::uint8_t>(8)},
    };
    EXPECT_FALSE(makeCpuStateViews(duplicate, views, error));
    EXPECT_NE(error.find("malformed or duplicate"), std::string::npos);

    std::vector<OwnedCpuStateValue> wrongSize = {
        {"pc", 64, std::vector<std::uint8_t>(7)},
    };
    EXPECT_FALSE(makeCpuStateViews(wrongSize, views, error));

    std::vector<OwnedCpuStateValue> highBits = {
        {"priv", 2, std::vector<std::uint8_t>{4}},
    };
    EXPECT_FALSE(makeCpuStateViews(highBits, views, error));
    EXPECT_NE(error.find("unused high bits"), std::string::npos);
}

TEST(CpuStateApi, RejectsEmptyAndUnknownSchemas)
{
    std::string error;
    EXPECT_EQ(findCpuStateEncoder(nullptr, error), nullptr);
    EXPECT_NE(error.find("empty schema"), std::string::npos);

    EXPECT_EQ(findCpuStateEncoder("test/unknown/v1", error), nullptr);
    EXPECT_NE(error.find("no CPU-state encoder"), std::string::npos);
}

} // anonymous namespace
} // namespace gem5::rtl_cosim
