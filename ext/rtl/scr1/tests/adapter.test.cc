/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "rtl/checker/memory_backend.hh"
#include "rtl/runtime/image_loader.hh"
#include "rtl/runtime/model_loader.hh"
#include "rtl/runtime/model_validator.hh"
#include "rtl/runtime/protocol/axi.hh"
#include "rtl/runtime/signal_access.hh"

#ifndef RTL_COSIM_SCR1_PATH
#error "RTL_COSIM_SCR1_PATH must be defined"
#endif
#ifndef RTL_COSIM_SCR1_BAREMETAL_ELF
#error "RTL_COSIM_SCR1_BAREMETAL_ELF must be defined"
#endif

namespace gem5::rtl_cosim
{
namespace
{

constexpr std::uint64_t TcmBase = 0x00480000;
constexpr std::uint64_t TcmSize = 64 * 1024;

Signal *
findCoreSignal(RtlCore &core, const std::string &name)
{
    for (std::size_t index = 0; index < core.signalCount(); ++index) {
        Signal *signal = core.signal(index).signal;
        if (signal && signal->name() && signal->name() == name) {
            return signal;
        }
    }
    return nullptr;
}

bool
driveBusInputsToZero(const ValidationResult &validation, std::string &error)
{
    for (const ValidatedBus &bus : validation.buses) {
        for (const auto &[role, signal] : bus.signals) {
            if (signal->direction() != SignalDirection::Input) {
                continue;
            }
            std::vector<std::uint8_t> zero(signalBytes(*signal), 0);
            if (!writeSignal(*signal, zero, error)) {
                error =
                    signalRoleName(bus.bus->protocol(), role) + ": " + error;
                return false;
            }
        }
    }
    return true;
}

bool
setResets(RtlCore &core, bool asserted, std::string &error)
{
    for (std::size_t index = 0; index < core.signalCount(); ++index) {
        const CoreSignalBinding binding = core.signal(index);
        if (binding.role != CoreSignalRole::Reset ||
            binding.signal->direction() != SignalDirection::Input) {
            continue;
        }
        const bool high = asserted ? binding.activeLevel == ActiveLevel::High
                                   : binding.activeLevel == ActiveLevel::Low;
        if (!writeSignalU64(*binding.signal, high ? 1 : 0, error)) {
            return false;
        }
    }
    return true;
}

bool
loadProgram(RtlCore &core, SparseMemory &memory, const char *path,
            std::string &error)
{
    std::vector<ImageSegment> segments;
    if (!loadImage(path, "elf", 0, segments, error)) {
        return false;
    }
    for (const ImageSegment &segment : segments) {
        bool placed = false;
        for (std::size_t index = 0; index < core.memoryCount(); ++index) {
            const MemoryRegion *region = core.memory(index);
            if (!region || segment.address < region->baseAddress ||
                segment.memorySize > region->size ||
                segment.address - region->baseAddress >
                    region->size - segment.memorySize) {
                continue;
            }
            const std::uint64_t offset = segment.address - region->baseAddress;
            if (!segment.data.empty() &&
                !core.writeMemory(index, offset, segment.data.data(),
                                  segment.data.size())) {
                error = core.getLastError();
                return false;
            }
            const std::size_t zeroSize = static_cast<std::size_t>(
                segment.memorySize - segment.data.size());
            if (zeroSize != 0) {
                std::vector<std::uint8_t> zero(zeroSize, 0);
                if (!core.writeMemory(index, offset + segment.data.size(),
                                      zero.data(), zero.size())) {
                    error = core.getLastError();
                    return false;
                }
            }
            placed = true;
            break;
        }
        if (placed) {
            continue;
        }
        if (!memory.write(segment.address, segment.data.data(), nullptr,
                          segment.data.size())) {
            error = "ELF segment does not fit standalone memory";
            return false;
        }
    }
    return true;
}

class CountingCallback final : public SignalChangeCallback
{
  public:
    void
    update() noexcept override
    {
        ++updates;
    }
    std::size_t updates = 0;
};

TEST(Scr1Adapter, ManagerLifetimeAndConfigurationFailures)
{
    ModelLoader first;
    ASSERT_TRUE(first.open(RTL_COSIM_SCR1_PATH)) << first.error();
    ASSERT_TRUE(first.createCore("{\"instance_name\":\"reference-0\"}"))
        << first.error();
    ASSERT_NE(first.core(), nullptr);
    EXPECT_STREQ(first.core()->name(), "reference-0");
    EXPECT_FALSE(first.createCore("{}"));

    ModelLoader invalid;
    ASSERT_TRUE(invalid.open(RTL_COSIM_SCR1_PATH)) << invalid.error();
    EXPECT_FALSE(invalid.createCore("[]"));
    EXPECT_NE(invalid.error().find("SCR1 config"), std::string::npos);

    ModelLoader second;
    ASSERT_TRUE(second.open(RTL_COSIM_SCR1_PATH)) << second.error();
    EXPECT_TRUE(second.createCore("{}")) << second.error();
}

TEST(Scr1Adapter, DiscoversCanonicalInterfacesAndVariableWidths)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_SCR1_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore("{}")) << loader.error();
    RtlCore &core = *loader.core();
    const ValidationResult validation = validateModel(core);
    for (const std::string &message : validation.errors) {
        ADD_FAILURE() << message;
    }
    ASSERT_TRUE(validation.ok());
    ASSERT_EQ(validation.buses.size(), 2);
    EXPECT_STREQ(validation.buses[0].bus->name(), "instruction");
    EXPECT_STREQ(validation.buses[1].bus->name(), "data");
    for (const ValidatedBus &bus : validation.buses) {
        EXPECT_EQ(bus.bus->protocol(), BusProtocol::Axi4);
        EXPECT_EQ(bus.bus->role(), BusRole::Initiator);
        EXPECT_EQ(bus.signals.size(), 44);
        EXPECT_EQ(bus.require(AxiSignal::AwAddr).bitWidth(), 32);
        EXPECT_EQ(bus.require(AxiSignal::WData).bitWidth(), 32);
        EXPECT_EQ(bus.require(AxiSignal::AwUser).bitWidth(), 4);
        EXPECT_EQ(bus.require(AxiSignal::ArLen).bitWidth(), 8);
    }
    EXPECT_EQ(core.signalCount(), 33);
    ASSERT_NE(findCoreSignal(core, "fuse_mhartid"), nullptr);
    EXPECT_EQ(findCoreSignal(core, "fuse_mhartid")->bitWidth(), 32);
    ASSERT_NE(findCoreSignal(core, "irq_lines[15]"), nullptr);
    EXPECT_EQ(findCoreSignal(core, "irq_lines[15]")->bitWidth(), 1);
    EXPECT_EQ(core.memoryCount(), 2);
    EXPECT_EQ(core.memory(0)->baseAddress, TcmBase);
    EXPECT_EQ(core.memory(0)->size, TcmSize);
    EXPECT_EQ(core.memory(0)->role, MemoryRole::CodeTcm);
    EXPECT_EQ(core.memory(1)->role, MemoryRole::DataTcm);
}

TEST(Scr1Adapter, SequencesAllResetsAndReportsOutputCallbacks)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_SCR1_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore("{}")) << loader.error();
    RtlCore &core = *loader.core();
    const ValidationResult validation = validateModel(core);
    ASSERT_TRUE(validation.ok());
    std::string error;
    ASSERT_TRUE(driveBusInputsToZero(validation, error)) << error;

    Signal *systemReset = findCoreSignal(core, "sys_rst_n_o");
    ASSERT_NE(systemReset, nullptr);
    CountingCallback callback;
    systemReset->setChangeCallback(&callback);

    ASSERT_TRUE(setResets(core, true, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    for (unsigned cycle = 0; cycle < 5; ++cycle) {
        ASSERT_EQ(core.clock(), ClockResult::Completed) << core.getLastError();
    }
    ASSERT_TRUE(setResets(core, false, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    for (unsigned cycle = 0; cycle < 5; ++cycle) {
        ASSERT_EQ(core.clock(), ClockResult::Completed) << core.getLastError();
    }
    std::uint64_t resetValue = 0;
    ASSERT_TRUE(readSignalU64(*systemReset, resetValue, error)) << error;
    EXPECT_EQ(resetValue, 1);
    EXPECT_GT(callback.updates, 0);
    systemReset->setChangeCallback(nullptr);
}

TEST(Scr1Adapter, ProvidesAliasedUnalignedTcmBackdoorsAndFailures)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_SCR1_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore("{}")) << loader.error();
    RtlCore &core = *loader.core();
    const std::vector<std::uint8_t> written = {0x11, 0x22, 0x33, 0x44,
                                               0x55, 0x66, 0x77};
    ASSERT_TRUE(core.writeMemory(0, 3, written.data(), written.size()))
        << core.getLastError();
    std::vector<std::uint8_t> read(written.size());
    ASSERT_TRUE(core.readMemory(1, 3, read.data(), read.size()))
        << core.getLastError();
    EXPECT_EQ(read, written);

    std::uint8_t byte = 0;
    EXPECT_FALSE(core.readMemory(2, 0, &byte, 1));
    EXPECT_FALSE(core.writeMemory(0, TcmSize, &byte, 1));
    EXPECT_FALSE(core.readMemory(0, TcmSize - 1, nullptr, 1));

    Signal &output = validateModel(core).buses[0].require(AxiSignal::ArValid);
    EXPECT_FALSE(output.setValue(&byte, 1));
    Signal &oneBitInput =
        validateModel(core).buses[0].require(AxiSignal::ArReady);
    byte = 2;
    EXPECT_FALSE(oneBitInput.setValue(&byte, 1));
    EXPECT_NE(std::string(core.getLastError()).find("high bits"),
              std::string::npos);
}

TEST(Scr1Adapter, LoadsTcmProgramRunsAxiAndWakesFromInterrupt)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_SCR1_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore("{\"instance_name\":\"wake-test\"}"))
        << loader.error();
    RtlCore &core = *loader.core();
    const ValidationResult validation = validateModel(core);
    ASSERT_TRUE(validation.ok());

    auto memory = std::make_shared<SparseMemory>(0, 16 * 1024 * 1024);
    std::string error;
    ASSERT_TRUE(
        loadProgram(core, *memory, RTL_COSIM_SCR1_BAREMETAL_ELF, error))
        << error;
    ASSERT_TRUE(driveBusInputsToZero(validation, error)) << error;

    ASSERT_TRUE(setResets(core, true, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    for (unsigned cycle = 0; cycle < 5; ++cycle) {
        ASSERT_EQ(core.clock(), ClockResult::Completed) << core.getLastError();
    }
    ASSERT_TRUE(setResets(core, false, error)) << error;

    std::vector<std::unique_ptr<MemoryBackend>> backends;
    std::vector<std::unique_ptr<BusTransactor>> transactors;
    for (const ValidatedBus &bus : validation.buses) {
        backends.push_back(std::make_unique<MemoryBackend>(
            memory, MemoryBackendConfig{2, 16, {}}));
        transactors.push_back(
            createAxiInitiatorTransactor(bus, *backends.back()));
    }

    Signal *arValid = validation.buses[0].find(AxiSignal::ArValid);
    ASSERT_NE(arValid, nullptr);
    CountingCallback callback;
    arValid->setChangeCallback(&callback);

    constexpr std::size_t MaxCycles = 3000;
    std::size_t cycles = 0;
    for (; cycles < MaxCycles; ++cycles) {
        for (auto &transactor : transactors) {
            ASSERT_TRUE(transactor->beforeClock())
                << transactor->getLastError();
        }
        ASSERT_TRUE(core.settle()) << core.getLastError();
        for (auto &transactor : transactors) {
            ASSERT_TRUE(transactor->afterSettle())
                << transactor->getLastError();
        }
        ASSERT_EQ(core.clock(), ClockResult::Completed) << core.getLastError();
        for (auto &transactor : transactors) {
            ASSERT_TRUE(transactor->afterClock())
                << transactor->getLastError();
        }
        for (auto &backend : backends) {
            backend->advance();
        }
        const bool idle =
            core.isIdle() &&
            std::all_of(
                transactors.begin(), transactors.end(),
                [](const auto &transactor) { return transactor->isIdle(); }) &&
            std::all_of(backends.begin(), backends.end(),
                        [](const auto &backend) { return backend->isIdle(); });
        if (idle) {
            ++cycles;
            break;
        }
    }
    EXPECT_LT(cycles, MaxCycles);
    EXPECT_TRUE(core.isIdle());
    EXPECT_GT(backends[0]->reads(), 0);
    EXPECT_GT(backends[1]->reads(), 0);
    EXPECT_GT(backends[1]->writes(), 0);
    EXPECT_GT(callback.updates, 0);

    Signal *softIrq = findCoreSignal(core, "soft_irq");
    ASSERT_NE(softIrq, nullptr);
    ASSERT_TRUE(writeSignalU64(*softIrq, 1, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    EXPECT_FALSE(core.isIdle());
    arValid->setChangeCallback(nullptr);
}

} // anonymous namespace
} // namespace gem5::rtl_cosim
