/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include "rtl/checker/memory_backend.hh"
#include "rtl/runtime/protocol/apb.hh"
#include "rtl/tests/test_model.hh"

namespace gem5::rtl_cosim
{
namespace
{

using namespace test;

class CombinationalApbTargetCore final : public TestCore
{
  public:
    CombinationalApbTargetCore()
    {
        auto bus = makeApb(BusRole::Target);
        _bus = bus.get();
        buses.push_back(std::move(bus));
    }

    bool
    settle() noexcept override
    {
        const bool access = _bus->get(ApbSignal::PSel).value() != 0 &&
                            _bus->get(ApbSignal::PEnable).value() != 0;
        _bus->get(ApbSignal::PReady).drive(access ? 1 : 0);
        _bus->get(ApbSignal::PSlvErr).drive(0);
        return true;
    }

    ClockResult
    clock() noexcept override
    {
        if (_bus->get(ApbSignal::PSel).value() != 0 &&
            _bus->get(ApbSignal::PEnable).value() != 0 &&
            _bus->get(ApbSignal::PReady).value() != 0) {
            ++handshakes;
        }
        return ClockResult::Completed;
    }

    TestBus &
    apb()
    {
        return *_bus;
    }

    unsigned handshakes = 0;

  private:
    TestBus *_bus = nullptr;
};

void
runCycle(CombinationalApbTargetCore &core, BusTransactor &transactor)
{
    ASSERT_TRUE(transactor.beforeClock()) << transactor.getLastError();
    ASSERT_TRUE(core.settle()) << core.getLastError();
    ASSERT_TRUE(transactor.afterSettle()) << transactor.getLastError();
    ASSERT_EQ(core.clock(), ClockResult::Completed);
    ASSERT_TRUE(transactor.afterClock()) << transactor.getLastError();
}

TEST(ApbInitiator, TransfersWriteWithBackpressure)
{
    TestCore core;
    auto bus = makeApb(BusRole::Initiator);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    ValidatedBus valid = validated(core);
    auto storage = std::make_shared<SparseMemory>(0, 0x1000);
    MemoryBackend backend(
        storage, {.latencyCycles = 1, .maxPending = 64, .errorRanges = {}});
    auto transactor = createApbInitiatorTransactor(valid, backend);

    raw->get(ApbSignal::PSel).drive(1);
    raw->get(ApbSignal::PEnable).drive(1);
    raw->get(ApbSignal::PWrite).drive(1);
    raw->get(ApbSignal::PAddr).drive(0x20);
    raw->get(ApbSignal::PWData).drive(0x44332211);
    raw->get(ApbSignal::PStrb).drive(0b0101);

    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterSettle()) << transactor->getLastError();
    EXPECT_EQ(raw->get(ApbSignal::PReady).value(), 0);
    ASSERT_TRUE(transactor->afterClock());
    backend.advance();
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterSettle()) << transactor->getLastError();
    ASSERT_TRUE(transactor->afterClock());
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterSettle()) << transactor->getLastError();
    EXPECT_EQ(raw->get(ApbSignal::PReady).value(), 1);
    ASSERT_TRUE(transactor->afterClock());

    std::uint8_t bytes[4] = {};
    ASSERT_TRUE(storage->read(0x20, bytes, 4));
    EXPECT_EQ(std::vector<std::uint8_t>(bytes, bytes + 4),
              (std::vector<std::uint8_t>{0x11, 0, 0x33, 0}));
    EXPECT_TRUE(transactor->isIdle());
}

TEST(ApbInitiator, DoesNotSamplePayloadOnIdleBus)
{
    TestCore core;
    auto bus = makeApb(BusRole::Initiator);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    ValidatedBus valid = validated(core);
    auto storage = std::make_shared<SparseMemory>(0, 0x1000);
    MemoryBackend backend(storage);
    auto transactor = createApbInitiatorTransactor(valid, backend);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterSettle()) << transactor->getLastError();
    EXPECT_EQ(raw->get(ApbSignal::PAddr).getCount, 0);
    EXPECT_EQ(raw->get(ApbSignal::PWData).getCount, 0);
    EXPECT_EQ(raw->get(ApbSignal::PProt).getCount, 0);
}

TEST(ApbInitiator, Supports1024BitDataAndStrobes)
{
    TestCore core;
    auto bus = makeApb(BusRole::Initiator, 1024);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    auto storage = std::make_shared<SparseMemory>(0, 0x1000);
    MemoryBackend backend(
        storage, {.latencyCycles = 0, .maxPending = 4, .errorRanges = {}});
    auto transactor = createApbInitiatorTransactor(validated(core), backend);

    std::vector<std::uint8_t> data(128);
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<std::uint8_t>(index);
    }
    std::vector<std::uint8_t> strobe(16, 0);
    strobe[0] = 0x01;
    strobe[12] = 0x10;
    raw->get(ApbSignal::PSel).drive(1);
    raw->get(ApbSignal::PEnable).drive(1);
    raw->get(ApbSignal::PWrite).drive(1);
    raw->get(ApbSignal::PAddr).drive(0x100);
    raw->get(ApbSignal::PWData).drive(data);
    raw->get(ApbSignal::PStrb).drive(strobe);

    ASSERT_TRUE(transactor->beforeClock()) << transactor->getLastError();
    ASSERT_TRUE(transactor->afterSettle()) << transactor->getLastError();
    ASSERT_TRUE(transactor->afterClock());
    backend.advance();
    std::vector<std::uint8_t> stored(128, 0);
    ASSERT_TRUE(storage->read(0x100, stored.data(), stored.size()));
    EXPECT_EQ(stored[0], 0);
    EXPECT_EQ(stored[100], 100);
    EXPECT_EQ(std::count_if(stored.begin(), stored.end(),
                            [](std::uint8_t byte) { return byte != 0; }),
              1);
}

TEST(ApbTarget, DrivesSetupAndCompletesRead)
{
    TestCore core;
    auto bus = makeApb(BusRole::Target);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    ValidatedBus valid = validated(core);
    ScriptedTransactionSource source;
    MemoryRequest request;
    request.token = 9;
    request.address = 0x80;
    request.beatBytes = 4;
    request.byteEnable.assign(4, 1);
    source.add(request);
    auto transactor = createApbTargetTransactor(valid, source);

    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterSettle()) << transactor->getLastError();
    EXPECT_EQ(raw->get(ApbSignal::PSel).value(), 1);
    EXPECT_EQ(raw->get(ApbSignal::PEnable).value(), 0);
    ASSERT_TRUE(transactor->afterClock());
    raw->get(ApbSignal::PReady).drive(1);
    raw->get(ApbSignal::PRData).drive(0x78563412);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterSettle()) << transactor->getLastError();
    EXPECT_EQ(raw->get(ApbSignal::PEnable).value(), 1);
    ASSERT_TRUE(transactor->afterClock());

    MemoryResponse response;
    ASSERT_TRUE(source.getCompleted(response));
    EXPECT_EQ(response.token, 9);
    EXPECT_EQ(response.data,
              (std::vector<std::uint8_t>{0x12, 0x34, 0x56, 0x78}));
    EXPECT_TRUE(transactor->isIdle());
}

TEST(ApbTarget, SettlesCombinationalReadyBeforeActiveEdge)
{
    CombinationalApbTargetCore core;
    ScriptedTransactionSource source;
    MemoryRequest request;
    request.token = 10;
    request.address = 0x20;
    request.write = true;
    request.beatBytes = 4;
    request.data = {1, 2, 3, 4};
    request.byteEnable.assign(4, 1);
    source.add(request);
    auto transactor = createApbTargetTransactor(validated(core), source);

    runCycle(core, *transactor);
    EXPECT_EQ(core.apb().get(ApbSignal::PEnable).value(), 0);
    EXPECT_EQ(core.handshakes, 0);

    runCycle(core, *transactor);
    EXPECT_EQ(core.handshakes, 1);
    EXPECT_TRUE(source.isIdle());
    EXPECT_TRUE(transactor->isIdle());
}

TEST(ApbTarget, PropagatesSlaveError)
{
    TestCore core;
    auto bus = makeApb(BusRole::Target);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    ScriptedTransactionSource source;
    MemoryRequest request;
    request.token = 2;
    request.address = 4;
    request.write = true;
    request.beatBytes = 4;
    request.data = {1, 2, 3, 4};
    request.byteEnable.assign(4, 1);
    source.add(request, {.error = true, .data = std::nullopt});
    auto transactor = createApbTargetTransactor(validated(core), source);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterSettle()) << transactor->getLastError();
    ASSERT_TRUE(transactor->afterClock());
    raw->get(ApbSignal::PReady).drive(1);
    raw->get(ApbSignal::PSlvErr).drive(1);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterSettle()) << transactor->getLastError();
    ASSERT_TRUE(transactor->afterClock());
    MemoryResponse response;
    ASSERT_TRUE(source.getCompleted(response));
    EXPECT_TRUE(response.error);
}

TEST(ApbTarget, RejectsPartialWriteWithoutPStrb)
{
    TestCore core;
    auto bus = makeApb(BusRole::Target);
    bus->remove(ApbSignal::PStrb);
    core.buses.push_back(std::move(bus));
    ScriptedTransactionSource source;
    MemoryRequest request;
    request.token = 8;
    request.address = 0x10;
    request.write = true;
    request.beatBytes = 4;
    request.data = {1, 2, 3, 4};
    request.byteEnable = {1, 0, 1, 0};
    source.add(request);
    auto transactor = createApbTargetTransactor(validated(core), source);
    EXPECT_FALSE(transactor->beforeClock());
    EXPECT_NE(std::string(transactor->getLastError()).find("PSTRB"),
              std::string::npos);
}

} // anonymous namespace
} // namespace gem5::rtl_cosim
