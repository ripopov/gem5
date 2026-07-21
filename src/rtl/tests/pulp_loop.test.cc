/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "rtl/runtime/model_loader.hh"
#include "rtl/runtime/model_validator.hh"
#include "rtl/runtime/protocol/apb.hh"
#include "rtl/runtime/protocol/axi.hh"
#include "rtl/runtime/signal_access.hh"

#if !defined(RTL_COSIM_PULP_APB_MASTER_PATH) ||                               \
    !defined(RTL_COSIM_PULP_APB_SLAVE_PATH) ||                                \
    !defined(RTL_COSIM_PULP_AXI_MASTER_PATH) ||                               \
    !defined(RTL_COSIM_PULP_AXI_SLAVE_PATH)
#error "PULP fixture library paths must be defined"
#endif

namespace gem5::rtl_cosim
{
namespace
{

class TransactionBridge final : public TransactionBackend,
                                public TransactionSource
{
  public:
    explicit TransactionBridge(std::size_t capacity = 4) : _capacity(capacity)
    {}

    bool
    canAccept(const MemoryRequest &request) const override
    {
        std::string error;
        return request.valid(error) &&
               _requests.size() + _inFlight < _capacity;
    }

    bool
    submit(const MemoryRequest &request) override
    {
        if (!canAccept(request)) {
            return false;
        }
        _requests.push_back(request);
        observedRequests.push_back(request);
        return true;
    }

    bool
    getResponse(MemoryResponse &response) override
    {
        if (_responses.empty()) {
            return false;
        }
        response = std::move(_responses.front());
        _responses.pop_front();
        return true;
    }

    void
    advance() override
    {}

    bool
    getRequest(MemoryRequest &request) override
    {
        if (_requests.empty()) {
            return false;
        }
        request = std::move(_requests.front());
        _requests.pop_front();
        _pending.emplace(request.token, request);
        ++_inFlight;
        return true;
    }

    bool
    canAcceptResponse(const MemoryResponse &) const override
    {
        return _responses.size() < _capacity;
    }

    bool
    submitResponse(const MemoryResponse &response) override
    {
        const auto pending = _pending.find(response.token);
        if (!canAcceptResponse(response) || pending == _pending.end() ||
            response.id != pending->second.id) {
            return false;
        }
        _pending.erase(pending);
        --_inFlight;
        _responses.push_back(response);
        observedResponses.push_back(response);
        return true;
    }

    bool
    idle() const noexcept
    {
        return _requests.empty() && _responses.empty() && _pending.empty() &&
               _inFlight == 0;
    }

    std::vector<MemoryRequest> observedRequests;
    std::vector<MemoryResponse> observedResponses;

  private:
    std::size_t _capacity;
    std::size_t _inFlight = 0;
    std::deque<MemoryRequest> _requests;
    std::deque<MemoryResponse> _responses;
    std::map<std::uint64_t, MemoryRequest> _pending;
};

void
driveBusInputsToZero(const ValidatedBus &bus)
{
    std::string error;
    for (const auto &[role, signal] : bus.signals) {
        if (signal->direction() != SignalDirection::Input) {
            continue;
        }
        ASSERT_TRUE(writeSignal(
            *signal, std::vector<std::uint8_t>(signalBytes(*signal), 0),
            error))
            << signalRoleName(bus.bus->protocol(), role) << ": " << error;
    }
}

void
setReset(RtlCore &core, bool asserted)
{
    std::string error;
    for (std::size_t index = 0; index < core.signalCount(); ++index) {
        const CoreSignalBinding binding = core.signal(index);
        if (binding.role != CoreSignalRole::Reset) {
            continue;
        }
        const bool high = asserted ? binding.activeLevel == ActiveLevel::High
                                   : binding.activeLevel == ActiveLevel::Low;
        ASSERT_TRUE(writeSignalU64(*binding.signal, high ? 1 : 0, error))
            << error;
    }
}

void
resetPair(RtlCore &master, RtlCore &slave)
{
    setReset(master, true);
    setReset(slave, true);
    ASSERT_TRUE(master.settle()) << master.getLastError();
    ASSERT_TRUE(slave.settle()) << slave.getLastError();
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        ASSERT_EQ(master.clock(), ClockResult::Completed);
        ASSERT_EQ(slave.clock(), ClockResult::Completed);
    }
    setReset(master, false);
    setReset(slave, false);
    ASSERT_TRUE(master.settle()) << master.getLastError();
    ASSERT_TRUE(slave.settle()) << slave.getLastError();
}

struct LoopResult
{
    std::size_t cycles;
    TransactionBridge bridge;
};

LoopResult
runLoop(const char *masterPath, const char *slavePath, BusProtocol protocol)
{
    ModelLoader masterLoader;
    ModelLoader slaveLoader;
    EXPECT_TRUE(masterLoader.open(masterPath)) << masterLoader.error();
    EXPECT_TRUE(slaveLoader.open(slavePath)) << slaveLoader.error();
    EXPECT_TRUE(masterLoader.createCore("{}")) << masterLoader.error();
    EXPECT_TRUE(slaveLoader.createCore("{}")) << slaveLoader.error();
    if (!masterLoader.core() || !slaveLoader.core()) {
        return {0, TransactionBridge{}};
    }

    RtlCore &master = *masterLoader.core();
    RtlCore &slave = *slaveLoader.core();
    ValidationResult masterValidation = validateModel(master);
    ValidationResult slaveValidation = validateModel(slave);
    EXPECT_TRUE(masterValidation.ok());
    EXPECT_TRUE(slaveValidation.ok());
    if (!masterValidation.ok() || !slaveValidation.ok()) {
        return {0, TransactionBridge{}};
    }
    EXPECT_EQ(masterValidation.buses.size(), 1);
    EXPECT_EQ(slaveValidation.buses.size(), 1);
    if (masterValidation.buses.empty() || slaveValidation.buses.empty()) {
        return {0, TransactionBridge{}};
    }

    ValidatedBus masterBus = masterValidation.buses.front();
    ValidatedBus slaveBus = slaveValidation.buses.front();
    EXPECT_EQ(masterBus.bus->protocol(), protocol);
    EXPECT_EQ(slaveBus.bus->protocol(), protocol);
    driveBusInputsToZero(masterBus);
    driveBusInputsToZero(slaveBus);
    resetPair(master, slave);

    TransactionBridge bridge;
    std::unique_ptr<BusTransactor> masterTransactor;
    std::unique_ptr<BusTransactor> slaveTransactor;
    if (protocol == BusProtocol::Apb) {
        masterTransactor = createApbInitiatorTransactor(masterBus, bridge);
        slaveTransactor = createApbTargetTransactor(slaveBus, bridge);
    } else {
        masterTransactor = createAxiInitiatorTransactor(masterBus, bridge);
        slaveTransactor = createAxiTargetTransactor(slaveBus, bridge);
    }

    constexpr std::size_t MaxCycles = 1000;
    std::size_t cycles = 0;
    for (; cycles < MaxCycles; ++cycles) {
        const bool masterBefore = masterTransactor->beforeClock();
        const bool slaveBefore = slaveTransactor->beforeClock();
        EXPECT_TRUE(masterBefore) << masterTransactor->getLastError();
        EXPECT_TRUE(slaveBefore) << slaveTransactor->getLastError();
        if (!masterBefore || !slaveBefore) {
            break;
        }
        const bool masterSettled = master.settle();
        const bool slaveSettled = slave.settle();
        EXPECT_TRUE(masterSettled) << master.getLastError();
        EXPECT_TRUE(slaveSettled) << slave.getLastError();
        if (!masterSettled || !slaveSettled) {
            break;
        }
        const bool masterCaptured = masterTransactor->afterSettle();
        const bool slaveCaptured = slaveTransactor->afterSettle();
        EXPECT_TRUE(masterCaptured) << masterTransactor->getLastError();
        EXPECT_TRUE(slaveCaptured) << slaveTransactor->getLastError();
        if (!masterCaptured || !slaveCaptured) {
            break;
        }
        EXPECT_EQ(master.clock(), ClockResult::Completed);
        EXPECT_EQ(slave.clock(), ClockResult::Completed);
        const bool masterAfter = masterTransactor->afterClock();
        const bool slaveAfter = slaveTransactor->afterClock();
        EXPECT_TRUE(masterAfter) << masterTransactor->getLastError();
        EXPECT_TRUE(slaveAfter) << slaveTransactor->getLastError();
        if (!masterAfter || !slaveAfter) {
            break;
        }
        bridge.advance();
        if (master.isIdle() && slave.isIdle() && masterTransactor->isIdle() &&
            slaveTransactor->isIdle() && bridge.idle()) {
            ++cycles;
            break;
        }
    }
    EXPECT_LT(cycles, MaxCycles);
    return {cycles, std::move(bridge)};
}

TEST(PulpRuntimeLoop, ConnectsApbMasterAndSlaveThroughNeutralTransactions)
{
    LoopResult result =
        runLoop(RTL_COSIM_PULP_APB_MASTER_PATH, RTL_COSIM_PULP_APB_SLAVE_PATH,
                BusProtocol::Apb);
    EXPECT_GT(result.cycles, 0);
    ASSERT_EQ(result.bridge.observedRequests.size(), 2);
    ASSERT_EQ(result.bridge.observedResponses.size(), 2);
    EXPECT_TRUE(result.bridge.observedRequests[0].write);
    EXPECT_FALSE(result.bridge.observedRequests[1].write);
    EXPECT_EQ(result.bridge.observedResponses[1].data,
              (std::vector<std::uint8_t>{0x11, 0, 0x33, 0}));
}

TEST(PulpRuntimeLoop, ConnectsAxiMasterAndSlaveThroughNeutralTransactions)
{
    LoopResult result =
        runLoop(RTL_COSIM_PULP_AXI_MASTER_PATH, RTL_COSIM_PULP_AXI_SLAVE_PATH,
                BusProtocol::Axi4);
    EXPECT_GT(result.cycles, 0);
    ASSERT_EQ(result.bridge.observedRequests.size(), 2);
    ASSERT_EQ(result.bridge.observedResponses.size(), 2);
    EXPECT_EQ(result.bridge.observedRequests[0].id, 3);
    EXPECT_EQ(result.bridge.observedResponses[1].data,
              (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));
}

} // anonymous namespace
} // namespace gem5::rtl_cosim
