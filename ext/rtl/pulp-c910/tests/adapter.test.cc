/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rtl/checker/memory_backend.hh"
#include "rtl/runtime/image_loader.hh"
#include "rtl/runtime/model_loader.hh"
#include "rtl/runtime/model_validator.hh"
#include "rtl/runtime/protocol/axi.hh"
#include "rtl/runtime/signal_access.hh"

#ifndef RTL_COSIM_PULP_C910_PATH
#error "RTL_COSIM_PULP_C910_PATH must be defined"
#endif
#ifndef RTL_COSIM_PULP_C910_INTERRUPT_WFI_ELF
#error "RTL_COSIM_PULP_C910_INTERRUPT_WFI_ELF must be defined"
#endif
#ifndef RTL_COSIM_PULP_C910_ERROR_TRAP_ELF
#error "RTL_COSIM_PULP_C910_ERROR_TRAP_ELF must be defined"
#endif
#ifndef RTL_COSIM_PULP_C910_C_INTEGRATION_ELF
#error "RTL_COSIM_PULP_C910_C_INTEGRATION_ELF must be defined"
#endif
#ifndef RTL_COSIM_PULP_C910_GEM5_BENCHMARK_ELF
#error "RTL_COSIM_PULP_C910_GEM5_BENCHMARK_ELF must be defined"
#endif
#ifndef RTL_COSIM_PULP_C910_STATE_IMPORT_ELF
#error "RTL_COSIM_PULP_C910_STATE_IMPORT_ELF must be defined"
#endif

namespace gem5::rtl_cosim
{
namespace
{

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
loadProgram(SparseMemory &memory, const char *path, std::string &error)
{
    std::vector<ImageSegment> segments;
    if (!loadImage(path, "elf", 0, segments, error)) {
        return false;
    }
    for (const ImageSegment &segment : segments) {
        if (!segment.data.empty() &&
            !memory.write(segment.address, segment.data.data(), nullptr,
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

class CoreRunner
{
  public:
    CoreRunner(RtlCore &core, const ValidatedBus &bus,
               std::shared_ptr<SparseMemory> memory,
               MemoryBackendConfig config = {2, 64, {}})
        : _core(core),
          _backend(std::move(memory), std::move(config)),
          _transactor(createAxiInitiatorTransactor(bus, _backend))
    {}

    bool
    step(std::string &error)
    {
        if (!_transactor->beforeClock()) {
            error = _transactor->getLastError();
            return false;
        }
        if (!_core.settle()) {
            error = _core.getLastError();
            return false;
        }
        if (!_transactor->afterSettle()) {
            error = _transactor->getLastError();
            return false;
        }
        if (_core.clock() != ClockResult::Completed) {
            error = _core.getLastError();
            return false;
        }
        if (!_transactor->afterClock()) {
            error = _transactor->getLastError();
            return false;
        }
        _backend.advance();
        return true;
    }

    bool
    idle() const noexcept
    {
        return _core.isIdle() && _transactor->isIdle() && _backend.isIdle();
    }

    bool
    runUntilIdle(std::size_t maxCycles, std::string &error)
    {
        for (std::size_t cycle = 0; cycle < maxCycles; ++cycle) {
            if (!step(error)) {
                return false;
            }
            if (idle()) {
                return true;
            }
        }
        error = "C910 did not become idle within cycle limit";
        return false;
    }

    MemoryBackend &backend() noexcept { return _backend; }
    bool transactorIdle() const noexcept { return _transactor->isIdle(); }

  private:
    RtlCore &_core;
    MemoryBackend _backend;
    std::unique_ptr<BusTransactor> _transactor;
};

struct OwnedStateValue
{
    std::string name;
    std::size_t width;
    std::vector<std::uint8_t> data;
};

class Riscv64State
{
  public:
    Riscv64State(std::uint64_t pc, std::uint64_t dumpAddress,
                 std::uint8_t privilege = 3,
                 std::uint64_t mstatus =
                     UINT64_C(0x8000000a00000000) | (3ULL << 13))
    {
        _owned.reserve(104);
        add("pc", 64, pc);
        add("x0", 64, 0);
        for (unsigned index = 1; index < 31; ++index) {
            add("x" + std::to_string(index), 64,
                UINT64_C(0x1111000000000000) + index);
        }
        add("x31", 64, dumpAddress);
        add("priv", 2, privilege);

        constexpr std::array<const char *, 20> Csrs = {
            "mstatus", "medeleg", "mideleg", "mie", "mtvec",
            "mcounteren", "mscratch", "mepc", "mcause", "mtval",
            "mip", "stvec", "scounteren", "sscratch", "sepc",
            "scause", "stval", "satp", "pmpcfg0", "pmpcfg2",
        };
        for (const char *name : Csrs) {
            const std::uint64_t value =
                std::string_view(name) == "mstatus" ? mstatus : 0;
            add("csr." + std::string(name), 64, value);
        }
        for (unsigned index = 0; index < 16; ++index) {
            add("csr.pmpaddr" + std::to_string(index), 64, 0);
        }
        for (unsigned index = 0; index < 32; ++index) {
            add("f" + std::to_string(index), 64,
                UINT64_C(0x3ff0000000000000) + index);
        }
        add("csr.fcsr", 8, 0x21);

        _views.reserve(_owned.size());
        for (const OwnedStateValue &value : _owned) {
            _views.push_back({value.name.c_str(), value.width,
                              value.data.data(), value.data.size()});
        }
    }

    const std::vector<CpuStateValue> &values() const { return _views; }

  private:
    void
    add(std::string name, std::size_t width, std::uint64_t value)
    {
        OwnedStateValue field{std::move(name), width,
                              std::vector<std::uint8_t>((width + 7) / 8)};
        for (std::size_t index = 0; index < field.data.size(); ++index) {
            field.data[index] = static_cast<std::uint8_t>(value >> (index * 8));
        }
        _owned.push_back(std::move(field));
    }

    std::vector<OwnedStateValue> _owned;
    std::vector<CpuStateValue> _views;
};

TEST(PulpC910Adapter, ManagerLifetimeAndConfigurationFailures)
{
    ModelLoader first;
    ASSERT_TRUE(first.open(RTL_COSIM_PULP_C910_PATH)) << first.error();
    ASSERT_TRUE(first.createCore("{\"instance_name\":\"c910-0\"}"))
        << first.error();
    ASSERT_NE(first.core(), nullptr);
    EXPECT_STREQ(first.core()->name(), "c910-0");
    EXPECT_FALSE(first.createCore("{}"));

    ModelLoader invalid;
    ASSERT_TRUE(invalid.open(RTL_COSIM_PULP_C910_PATH)) << invalid.error();
    EXPECT_FALSE(invalid.createCore("[]"));
    EXPECT_NE(invalid.error().find("C910 config"), std::string::npos);

    ModelLoader second;
    ASSERT_TRUE(second.open(RTL_COSIM_PULP_C910_PATH)) << second.error();
    EXPECT_TRUE(second.createCore("{}")) << second.error();
}

TEST(PulpC910Adapter, DiscoversCanonicalAxiAndStandaloneSignals)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_PULP_C910_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore("{}")) << loader.error();
    RtlCore &core = *loader.core();
    const ValidationResult validation = validateModel(core);
    for (const std::string &message : validation.errors) {
        ADD_FAILURE() << message;
    }
    ASSERT_TRUE(validation.ok());
    ASSERT_EQ(validation.buses.size(), 1);
    const ValidatedBus &bus = validation.buses[0];
    EXPECT_STREQ(bus.bus->name(), "memory");
    EXPECT_EQ(bus.bus->protocol(), BusProtocol::Axi4);
    EXPECT_EQ(bus.bus->role(), BusRole::Initiator);
    EXPECT_EQ(bus.signals.size(), 44);
    EXPECT_EQ(bus.require(AxiSignal::AwAddr).bitWidth(), 40);
    EXPECT_EQ(bus.require(AxiSignal::WData).bitWidth(), 128);
    EXPECT_EQ(bus.require(AxiSignal::WStrb).bitWidth(), 16);
    EXPECT_EQ(bus.require(AxiSignal::AwId).bitWidth(), 8);
    EXPECT_EQ(bus.require(AxiSignal::AwUser).bitWidth(), 1);
    EXPECT_EQ(bus.require(AxiSignal::ArLen).bitWidth(), 8);

    EXPECT_EQ(core.signalCount(), 56);
    ASSERT_NE(findCoreSignal(core, "ext_int_i[39]"), nullptr);
    ASSERT_NE(findCoreSignal(core, "lpmd_b_o"), nullptr);
    EXPECT_EQ(findCoreSignal(core, "lpmd_b_o")->bitWidth(), 2);
    ASSERT_NE(findCoreSignal(core, "rtc_i"), nullptr);
    ASSERT_NE(findCoreSignal(core, "debug_req_i"), nullptr);
    EXPECT_EQ(core.memoryCount(), 0);
    ASSERT_NE(core.cpuState(), nullptr);
    EXPECT_STREQ(core.cpuState()->schema(), RtlCpuStateSchema::Riscv64V1);
    EXPECT_EQ(core.cpuState()->contextCount(), 1);
}

TEST(PulpC910Adapter, RejectsMalformedArchitecturalStateBeforeMutation)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_PULP_C910_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore("{}")) << loader.error();
    RtlCpuState *state = loader.core()->cpuState();
    ASSERT_NE(state, nullptr);

    EXPECT_FALSE(state->importState(1, nullptr, 0));
    EXPECT_NE(std::string(state->getLastError()).find("context 1"),
              std::string::npos);
    EXPECT_FALSE(state->importState(0, nullptr, 0));
    EXPECT_NE(std::string(state->getLastError()).find("missing field 'pc'"),
              std::string::npos);

    std::uint8_t data = 0;
    const CpuStateValue duplicate[] = {
        {"pc", 8, &data, 1}, {"pc", 8, &data, 1}};
    EXPECT_FALSE(state->importState(0, duplicate, 2));
    EXPECT_NE(std::string(state->getLastError()).find("duplicate field 'pc'"),
              std::string::npos);

    const Riscv64State valid(0x02001000, 0x01810000);
    auto values = valid.values();
    values.front().bitWidth = 63;
    EXPECT_FALSE(state->importState(0, values.data(), values.size()));
    EXPECT_NE(std::string(state->getLastError()).find("invalid width"),
              std::string::npos);

    values = valid.values();
    values.push_back({"vendor.private", 1, &data, 1});
    EXPECT_FALSE(state->importState(0, values.data(), values.size()));
    EXPECT_NE(std::string(state->getLastError()).find("unknown field"),
              std::string::npos);
}

TEST(PulpC910Adapter, ImportsPcIntegerAndFloatingPointStateAndResumes)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_PULP_C910_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore("{\"instance_name\":\"state-import\"}"))
        << loader.error();
    RtlCore &core = *loader.core();
    const ValidationResult validation = validateModel(core);
    ASSERT_TRUE(validation.ok());

    constexpr std::uint64_t DumpAddress = 0x01810000;
    auto memory = std::make_shared<SparseMemory>(0, 64 * 1024 * 1024);
    std::string error;
    ASSERT_TRUE(loadProgram(*memory, RTL_COSIM_PULP_C910_STATE_IMPORT_ELF,
                            error))
        << error;
    ASSERT_TRUE(driveBusInputsToZero(validation, error)) << error;
    ASSERT_TRUE(setResets(core, true, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        ASSERT_EQ(core.clock(), ClockResult::Completed) << core.getLastError();
    }
    ASSERT_TRUE(setResets(core, false, error)) << error;

    RtlCpuState *cpuState = core.cpuState();
    ASSERT_NE(cpuState, nullptr);
    const Riscv64State invalidTransition(
        0x02001000, DumpAddress, 1, 3ULL << 13);
    EXPECT_FALSE(cpuState->importState(
        0, invalidTransition.values().data(),
        invalidTransition.values().size()));
    EXPECT_NE(std::string(cpuState->getLastError()).find("post-MRET state"),
              std::string::npos);

    // A rejected semantic bundle must not consume the one successful import.
    const Riscv64State state(0x02001000, DumpAddress);
    ASSERT_TRUE(cpuState->importState(0, state.values().data(),
                                      state.values().size()))
        << cpuState->getLastError();
    EXPECT_FALSE(cpuState->importState(0, state.values().data(),
                                       state.values().size()));

    CoreRunner runner(core, validation.buses[0], memory);
    ASSERT_TRUE(runner.runUntilIdle(50000, error)) << error;
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 32; ++index) {
        ASSERT_TRUE(memory->read(DumpAddress + index * 8,
                                 reinterpret_cast<std::uint8_t *>(&value), 8));
        const std::uint64_t expected = index == 0 ? 0 :
            index == 31 ? DumpAddress :
            UINT64_C(0x1111000000000000) + index;
        EXPECT_EQ(value, expected) << "x" << index;
    }
    for (unsigned index = 0; index < 32; ++index) {
        ASSERT_TRUE(memory->read(DumpAddress + 256 + index * 8,
                                 reinterpret_cast<std::uint8_t *>(&value), 8));
        EXPECT_EQ(value, UINT64_C(0x3ff0000000000000) + index)
            << "f" << index;
    }
    ASSERT_TRUE(memory->read(DumpAddress + 512,
                             reinterpret_cast<std::uint8_t *>(&value), 8));
    EXPECT_EQ(value, UINT64_C(0x600dc9105a7e0001));
}

TEST(PulpC910Adapter, SequencesResetAndSupportsWideSignalAccess)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_PULP_C910_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore("{}")) << loader.error();
    RtlCore &core = *loader.core();
    const ValidationResult validation = validateModel(core);
    ASSERT_TRUE(validation.ok());
    std::string error;
    ASSERT_TRUE(driveBusInputsToZero(validation, error)) << error;

    ASSERT_TRUE(setResets(core, true, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        ASSERT_EQ(core.clock(), ClockResult::Completed) << core.getLastError();
    }
    ASSERT_TRUE(setResets(core, false, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();

    Signal &rdata = validation.buses[0].require(AxiSignal::RData);
    std::vector<std::uint8_t> written(signalBytes(rdata));
    for (std::size_t index = 0; index < written.size(); ++index) {
        written[index] = static_cast<std::uint8_t>(index * 17);
    }
    ASSERT_TRUE(writeSignal(rdata, written, error)) << error;
    std::vector<std::uint8_t> read;
    ASSERT_TRUE(readSignal(rdata, read, error)) << error;
    EXPECT_EQ(read, written);

    Signal &ready = validation.buses[0].require(AxiSignal::ArReady);
    const std::vector<std::uint8_t> invalid = {2};
    EXPECT_FALSE(writeSignal(ready, invalid, error));
    EXPECT_NE(std::string(core.getLastError()).find("high bits"),
              std::string::npos);
    std::uint8_t byte = 0;
    EXPECT_FALSE(core.readMemory(0, 0, &byte, 1));
    EXPECT_FALSE(core.writeMemory(0, 0, &byte, 1));

    Signal *rtc = findCoreSignal(core, "rtc_i");
    ASSERT_NE(rtc, nullptr);
    ASSERT_TRUE(writeSignalU64(*rtc, 1, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    ASSERT_TRUE(writeSignalU64(*rtc, 0, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
}

TEST(PulpC910Adapter, BootsSleepsSuppressesClockAndWakesOnTimerInterrupt)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_PULP_C910_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore("{\"instance_name\":\"wake-test\"}"))
        << loader.error();
    RtlCore &core = *loader.core();
    const ValidationResult validation = validateModel(core);
    ASSERT_TRUE(validation.ok());

    auto memory = std::make_shared<SparseMemory>(0, 64 * 1024 * 1024);
    std::string error;
    ASSERT_TRUE(loadProgram(*memory, RTL_COSIM_PULP_C910_INTERRUPT_WFI_ELF,
                            error))
        << error;
    ASSERT_TRUE(driveBusInputsToZero(validation, error)) << error;
    ASSERT_TRUE(setResets(core, true, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        ASSERT_EQ(core.clock(), ClockResult::Completed) << core.getLastError();
    }
    ASSERT_TRUE(setResets(core, false, error)) << error;

    Signal &arValid = validation.buses[0].require(AxiSignal::ArValid);
    CountingCallback callback;
    arValid.setChangeCallback(&callback);

    CoreRunner runner(core, validation.buses[0], memory);
    const bool reachedFirstIdle = runner.runUntilIdle(500000, error);
    std::uint64_t firstLpmd = 0;
    std::string diagnosticError;
    Signal *lpmd = findCoreSignal(core, "lpmd_b_o");
    ASSERT_NE(lpmd, nullptr);
    ASSERT_TRUE(readSignalU64(*lpmd, firstLpmd, diagnosticError))
        << diagnosticError;
    std::uint64_t marker = 0;
    ASSERT_TRUE(memory->read(0x01800018,
                             reinterpret_cast<std::uint8_t *>(&marker),
                             sizeof(marker)));
    auto readBus = [&](SignalRoleId role) {
        std::uint64_t value = 0;
        EXPECT_TRUE(readSignalU64(validation.buses[0].require(role), value,
                                  diagnosticError))
            << diagnosticError;
        return value;
    };
    ASSERT_TRUE(reachedFirstIdle)
        << error << "; lpmd=" << firstLpmd << ", marker=" << marker
        << ", reads=" << runner.backend().reads()
        << ", writes=" << runner.backend().writes()
        << ", core_idle=" << core.isIdle()
        << ", transactor_idle=" << runner.transactorIdle()
        << ", backend_idle=" << runner.backend().isIdle()
        << ", awvalid=" << readBus(AxiSignal::AwValid)
        << ", wvalid=" << readBus(AxiSignal::WValid)
        << ", arvalid=" << readBus(AxiSignal::ArValid)
        << ", bvalid=" << readBus(AxiSignal::BValid)
        << ", rvalid=" << readBus(AxiSignal::RValid);
    EXPECT_TRUE(core.isIdle());
    EXPECT_GT(runner.backend().reads(), 0);
    EXPECT_GT(runner.backend().writes(), 0);
    EXPECT_GT(callback.updates, 0);

    EXPECT_EQ(marker, 1);

    // Pulse the independent RTC through settle() while deliberately issuing
    // no main-core clock cycles. The drained core remains safely asleep.
    Signal *rtc = findCoreSignal(core, "rtc_i");
    ASSERT_NE(rtc, nullptr);
    for (unsigned pulse = 0; pulse < 4; ++pulse) {
        ASSERT_TRUE(writeSignalU64(*rtc, 1, error)) << error;
        ASSERT_TRUE(core.settle()) << core.getLastError();
        ASSERT_TRUE(writeSignalU64(*rtc, 0, error)) << error;
        ASSERT_TRUE(core.settle()) << core.getLastError();
    }
    EXPECT_TRUE(runner.idle());
    marker = 0;
    ASSERT_TRUE(memory->read(0x01800018,
                             reinterpret_cast<std::uint8_t *>(&marker),
                             sizeof(marker)));
    EXPECT_EQ(marker, 1);

    Signal *timerInterrupt = findCoreSignal(core, "time_irq_i");
    ASSERT_NE(timerInterrupt, nullptr);
    ASSERT_TRUE(writeSignalU64(*timerInterrupt, 1, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    EXPECT_FALSE(core.isIdle());
    for (unsigned cycle = 0; cycle < 64; ++cycle) {
        ASSERT_TRUE(runner.step(error)) << error;
    }
    ASSERT_TRUE(writeSignalU64(*timerInterrupt, 0, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    ASSERT_TRUE(runner.runUntilIdle(500000, error)) << error;
    EXPECT_TRUE(core.isIdle());
    marker = 0;
    ASSERT_TRUE(memory->read(0x01800018,
                             reinterpret_cast<std::uint8_t *>(&marker),
                             sizeof(marker)));
    EXPECT_EQ(marker, 2);
    arValid.setChangeCallback(nullptr);
}

TEST(PulpC910Adapter, PropagatesAxiReadErrorToTheCore)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_PULP_C910_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore("{}")) << loader.error();
    RtlCore &core = *loader.core();
    const ValidationResult validation = validateModel(core);
    ASSERT_TRUE(validation.ok());

    auto memory = std::make_shared<SparseMemory>(0, 64 * 1024 * 1024);
    std::string error;
    ASSERT_TRUE(loadProgram(*memory, RTL_COSIM_PULP_C910_ERROR_TRAP_ELF,
                            error))
        << error;
    ASSERT_TRUE(driveBusInputsToZero(validation, error)) << error;
    ASSERT_TRUE(setResets(core, true, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        ASSERT_EQ(core.clock(), ClockResult::Completed) << core.getLastError();
    }
    ASSERT_TRUE(setResets(core, false, error)) << error;

    CoreRunner runner(core, validation.buses[0], memory,
                      {2, 64, {{0x01801000, 4096}}});
    ASSERT_TRUE(runner.runUntilIdle(500000, error)) << error;
    std::uint64_t marker = 0;
    ASSERT_TRUE(memory->read(0x01800010,
                             reinterpret_cast<std::uint8_t *>(&marker),
                             sizeof(marker)));
    EXPECT_EQ(marker, 0xe220);
}

TEST(PulpC910Adapter, RunsFreestandingCAndInitializesBss)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_PULP_C910_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore("{}")) << loader.error();
    RtlCore &core = *loader.core();
    const ValidationResult validation = validateModel(core);
    ASSERT_TRUE(validation.ok());

    auto memory = std::make_shared<SparseMemory>(0, 64 * 1024 * 1024);
    std::string error;
    ASSERT_TRUE(loadProgram(*memory, RTL_COSIM_PULP_C910_C_INTEGRATION_ELF,
                            error))
        << error;
    ASSERT_TRUE(driveBusInputsToZero(validation, error)) << error;
    ASSERT_TRUE(setResets(core, true, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        ASSERT_EQ(core.clock(), ClockResult::Completed) << core.getLastError();
    }
    ASSERT_TRUE(setResets(core, false, error)) << error;

    CoreRunner runner(core, validation.buses[0], memory);
    ASSERT_TRUE(runner.runUntilIdle(500000, error)) << error;
    std::uint64_t marker = 0;
    ASSERT_TRUE(memory->read(0x01800020,
                             reinterpret_cast<std::uint8_t *>(&marker),
                             sizeof(marker)));
    EXPECT_EQ(marker, UINT64_C(0x1828384858687888));
}

TEST(PulpC910Adapter, RunsGem5ComputeAndMemoryBenchmark)
{
    ModelLoader loader;
    ASSERT_TRUE(loader.open(RTL_COSIM_PULP_C910_PATH)) << loader.error();
    ASSERT_TRUE(loader.createCore("{}")) << loader.error();
    RtlCore &core = *loader.core();
    const ValidationResult validation = validateModel(core);
    ASSERT_TRUE(validation.ok());

    auto memory = std::make_shared<SparseMemory>(0, 64 * 1024 * 1024);
    std::string error;
    ASSERT_TRUE(loadProgram(
        *memory, RTL_COSIM_PULP_C910_GEM5_BENCHMARK_ELF, error))
        << error;
    ASSERT_TRUE(driveBusInputsToZero(validation, error)) << error;
    ASSERT_TRUE(setResets(core, true, error)) << error;
    ASSERT_TRUE(core.settle()) << core.getLastError();
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        ASSERT_EQ(core.clock(), ClockResult::Completed)
            << core.getLastError();
    }
    ASSERT_TRUE(setResets(core, false, error)) << error;

    CoreRunner runner(core, validation.buses[0], memory);
    ASSERT_TRUE(runner.runUntilIdle(500000, error)) << error;
    std::uint64_t signature = 0;
    ASSERT_TRUE(memory->read(0x01800020,
                             reinterpret_cast<std::uint8_t *>(&signature),
                             sizeof(signature)));
    EXPECT_EQ(signature, UINT64_C(0xdc2efb8acc3994ff));
}

} // anonymous namespace
} // namespace gem5::rtl_cosim
