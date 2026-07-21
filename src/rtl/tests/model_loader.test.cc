/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <array>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "rtl/runtime/model_loader.hh"
#include "rtl/runtime/model_validator.hh"
#include "rtl/runtime/signal_access.hh"

#ifndef RTL_COSIM_TEST_VENDOR_PATH
#error "RTL_COSIM_TEST_VENDOR_PATH must name the test vendor shared library"
#endif

namespace gem5::rtl_cosim
{
namespace
{

class CountingCallback final : public SignalChangeCallback
{
  public:
    void
    update() noexcept override
    {
        ++updates;
    }
    unsigned updates = 0;
};

TEST(ModelLoader, LoadsV1ModelAndHonorsLifetimeContracts)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_TEST_VENDOR_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore(R"({"test":true})")) << loader.error();
    EXPECT_FALSE(loader.createCore("{}"));
    ASSERT_NE(loader.core(), nullptr);

    RtlCore &core = *loader.core();
    EXPECT_STREQ(core.name(), "cpp-vendor-fixture");
    EXPECT_TRUE(validateModel(core).ok());
    ASSERT_EQ(core.signalCount(), 3);

    Signal *reset = core.signal(0).signal;
    Signal *interrupt = core.signal(1).signal;
    Signal *vectorInput = core.signal(2).signal;
    ASSERT_NE(reset, nullptr);
    ASSERT_NE(interrupt, nullptr);
    ASSERT_NE(vectorInput, nullptr);

    std::string error;
    ASSERT_TRUE(writeSignalU64(*reset, 0, error)) << error;
    EXPECT_EQ(core.clock(), ClockResult::Completed);
    CountingCallback callback;
    interrupt->setChangeCallback(&callback);
    ASSERT_TRUE(writeSignalU64(*reset, 1, error)) << error;
    const std::vector<std::uint8_t> vectorValue = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    ASSERT_TRUE(writeSignal(*vectorInput, vectorValue, error)) << error;
    EXPECT_EQ(core.clock(), ClockResult::Completed);
    EXPECT_EQ(callback.updates, 1);
    EXPECT_TRUE(core.isIdle());

    std::uint64_t interruptValue = 0;
    ASSERT_TRUE(readSignalU64(*interrupt, interruptValue, error)) << error;
    EXPECT_EQ(interruptValue, 1);

    CountingCallback replacement;
    interrupt->setChangeCallback(&replacement);
    EXPECT_EQ(replacement.updates, 0);
    ASSERT_TRUE(writeSignalU64(*reset, 0, error)) << error;
    EXPECT_EQ(core.clock(), ClockResult::Completed);
    EXPECT_EQ(callback.updates, 1);
    EXPECT_EQ(replacement.updates, 1);
    interrupt->setChangeCallback(nullptr);
    ASSERT_TRUE(writeSignalU64(*reset, 1, error)) << error;
    EXPECT_EQ(core.clock(), ClockResult::Completed);
    EXPECT_EQ(replacement.updates, 1);
    EXPECT_TRUE(core.isIdle());

    constexpr std::array<std::uint8_t, 4> written = {0xde, 0xad, 0xbe, 0xef};
    std::array<std::uint8_t, 4> read{};
    ASSERT_TRUE(core.writeMemory(0, 4, written.data(), written.size()));
    ASSERT_TRUE(core.readMemory(0, 4, read.data(), read.size()));
    EXPECT_EQ(read, written);
    EXPECT_FALSE(core.writeMemory(0, 63, written.data(), written.size()));

    loader.close();
    EXPECT_EQ(loader.core(), nullptr);
    EXPECT_EQ(loader.manager(), nullptr);
}

TEST(ModelLoader, ReportsDynamicLoaderFailures)
{
    ModelLoader loader;
    EXPECT_FALSE(loader.open("rtl-cosim-library-that-does-not-exist"));
    EXPECT_NE(loader.error().find("cannot load"), std::string::npos);
    EXPECT_FALSE(loader.createCore("{}"));
    EXPECT_NE(loader.error().find("no vendor library"), std::string::npos);
}

} // anonymous namespace
} // namespace gem5::rtl_cosim
