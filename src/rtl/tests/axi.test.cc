/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <array>
#include <deque>

#include <gtest/gtest.h>

#include "rtl/checker/memory_backend.hh"
#include "rtl/runtime/protocol/axi.hh"
#include "rtl/tests/test_model.hh"

namespace gem5::rtl_cosim
{
namespace
{

using namespace test;

class ControlledBackend final : public TransactionBackend
{
  public:
    bool
    canAccept(const MemoryRequest &) const override
    {
        return true;
    }
    bool
    submit(const MemoryRequest &request) override
    {
        requests.push_back(request);
        return true;
    }
    bool
    getResponse(MemoryResponse &response) override
    {
        if (responses.empty()) {
            return false;
        }
        response = std::move(responses.front());
        responses.pop_front();
        return true;
    }
    void
    advance() override
    {}

    std::vector<MemoryRequest> requests;
    std::deque<MemoryResponse> responses;
};

void
driveWriteAddress(TestBus &bus, std::uint32_t id, std::uint64_t address,
                  std::size_t beats)
{
    bus.get(AxiSignal::AwId).drive(id);
    bus.get(AxiSignal::AwAddr).drive(address);
    bus.get(AxiSignal::AwLen).drive(beats - 1);
    bus.get(AxiSignal::AwSize).drive(2);
    bus.get(AxiSignal::AwBurst).drive(1);
    bus.get(AxiSignal::AwValid).drive(1);
}

void
driveWriteBeat(TestBus &bus, std::uint32_t data, bool last)
{
    bus.get(AxiSignal::WData).drive(data);
    bus.get(AxiSignal::WStrb).drive(0xf);
    bus.get(AxiSignal::WLast).drive(last);
    bus.get(AxiSignal::WValid).drive(1);
}

void
driveReadAddress(TestBus &bus, std::uint32_t id, std::uint64_t address,
                 std::size_t beats)
{
    bus.get(AxiSignal::ArId).drive(id);
    bus.get(AxiSignal::ArAddr).drive(address);
    bus.get(AxiSignal::ArLen).drive(beats - 1);
    bus.get(AxiSignal::ArSize).drive(2);
    bus.get(AxiSignal::ArBurst).drive(1);
    bus.get(AxiSignal::ArValid).drive(1);
}

TEST(AxiInitiator, ExecutesBurstWriteAndResponse)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    auto storage = std::make_shared<SparseMemory>(0, 0x1000);
    MemoryBackend backend(
        storage, {.latencyCycles = 0, .maxPending = 64, .errorRanges = {}});
    auto transactor = createAxiInitiatorTransactor(validated(core), backend);
    raw->get(AxiSignal::BReady).drive(1);

    driveWriteAddress(*raw, 3, 0x40, 2);
    driveWriteBeat(*raw, 0x04030201, false);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    raw->get(AxiSignal::AwValid).drive(0);
    driveWriteBeat(*raw, 0x08070605, true);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    backend.advance();
    raw->get(AxiSignal::WValid).drive(0);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::BValid).value(), 1);
    EXPECT_EQ(raw->get(AxiSignal::BId).value(), 3);
    ASSERT_TRUE(transactor->afterClock());

    std::uint8_t bytes[8] = {};
    ASSERT_TRUE(storage->read(0x40, bytes, 8));
    EXPECT_EQ(std::vector<std::uint8_t>(bytes, bytes + 8),
              (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(AxiInitiator, ExecutesReadAndHoldsResponseUnderBackpressure)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    auto storage = std::make_shared<SparseMemory>(0, 0x1000);
    const std::uint8_t data[] = {0x11, 0x22, 0x33, 0x44,
                                 0x55, 0x66, 0x77, 0x88};
    ASSERT_TRUE(storage->write(0x80, data, nullptr, sizeof(data)));
    MemoryBackend backend(
        storage, {.latencyCycles = 0, .maxPending = 64, .errorRanges = {}});
    auto transactor = createAxiInitiatorTransactor(validated(core), backend);
    driveReadAddress(*raw, 5, 0x80, 2);

    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    backend.advance();
    raw->get(AxiSignal::ArValid).drive(0);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    raw->get(AxiSignal::RReady).drive(0);
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::RValid).value(), 1);
    EXPECT_EQ(raw->get(AxiSignal::RData).value(), 0x44332211);
    EXPECT_EQ(raw->get(AxiSignal::RLast).value(), 0);
    ASSERT_TRUE(transactor->afterClock());
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::RData).value(), 0x44332211);
    raw->get(AxiSignal::RReady).drive(1);
    ASSERT_TRUE(transactor->afterClock());
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::RData).value(), 0x88776655);
    EXPECT_EQ(raw->get(AxiSignal::RLast).value(), 1);
}

TEST(AxiInitiator, SamplesPayloadOnlyOnHandshake)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    auto storage = std::make_shared<SparseMemory>(0, 0x1000);
    MemoryBackend backend(storage);
    auto transactor = createAxiInitiatorTransactor(validated(core), backend,
                                                   {.maxPending = 1});
    driveWriteAddress(*raw, 1, 0x10, 1);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    const std::size_t reads = raw->get(AxiSignal::AwAddr).getCount;
    raw->get(AxiSignal::AwAddr).drive(0x20);
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::AwReady).value(), 0);
    EXPECT_EQ(raw->get(AxiSignal::AwAddr).getCount, reads);
    EXPECT_EQ(raw->get(AxiSignal::WData).getCount, 0);
}

TEST(AxiInitiator, SamplesWideUserSignalsOnlyOnHandshake)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator);
    TestSignal &awUser = bus->add(
        AxiSignal::AwUser, 129,
        direction(BusProtocol::Axi4, BusRole::Initiator, AxiSignal::AwUser));
    TestSignal &wUser = bus->add(
        AxiSignal::WUser, 129,
        direction(BusProtocol::Axi4, BusRole::Initiator, AxiSignal::WUser));
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    ControlledBackend backend;
    auto transactor = createAxiInitiatorTransactor(validated(core), backend);
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(awUser.getCount, 0);
    EXPECT_EQ(wUser.getCount, 0);

    driveWriteAddress(*raw, 1, 0x20, 1);
    driveWriteBeat(*raw, 0x04030201, true);
    ASSERT_TRUE(transactor->beforeClock()) << transactor->getLastError();
    EXPECT_EQ(awUser.getCount, 1);
    EXPECT_EQ(wUser.getCount, 1);
}

TEST(AxiInitiator, RejectsAssertedLock)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    auto storage = std::make_shared<SparseMemory>(0, 0x1000);
    MemoryBackend backend(storage);
    auto transactor = createAxiInitiatorTransactor(validated(core), backend);
    driveReadAddress(*raw, 0, 0, 1);
    raw->get(AxiSignal::ArLock).drive(1);
    EXPECT_FALSE(transactor->beforeClock());
    EXPECT_NE(std::string(transactor->getLastError()).find("locked"),
              std::string::npos);
}

TEST(AxiInitiator, Supports1024BitDataAndStrobes)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator, BusProtocol::Axi4, 1024);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    auto storage = std::make_shared<SparseMemory>(0, 0x2000);
    MemoryBackend backend(
        storage, {.latencyCycles = 0, .maxPending = 4, .errorRanges = {}});
    auto transactor = createAxiInitiatorTransactor(validated(core), backend);

    std::vector<std::uint8_t> data(128);
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<std::uint8_t>(index + 1);
    }
    driveWriteAddress(*raw, 1, 0x200, 1);
    raw->get(AxiSignal::AwSize).drive(7);
    raw->get(AxiSignal::WData).drive(data);
    raw->get(AxiSignal::WStrb).drive(std::vector<std::uint8_t>(16, 0xff));
    raw->get(AxiSignal::WLast).drive(1);
    raw->get(AxiSignal::WValid).drive(1);

    ASSERT_TRUE(transactor->beforeClock()) << transactor->getLastError();
    ASSERT_TRUE(transactor->afterClock()) << transactor->getLastError();
    backend.advance();
    std::vector<std::uint8_t> stored(128);
    ASSERT_TRUE(storage->read(0x200, stored.data(), stored.size()));
    EXPECT_EQ(stored, data);
}

TEST(AxiInitiator, SupportsAxi3WriteDataIds)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator, BusProtocol::Axi3);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    auto storage = std::make_shared<SparseMemory>(0, 0x1000);
    MemoryBackend backend(
        storage, {.latencyCycles = 0, .maxPending = 4, .errorRanges = {}});
    auto transactor = createAxiInitiatorTransactor(validated(core), backend);

    driveWriteAddress(*raw, 5, 0x80, 1);
    driveWriteBeat(*raw, 0x44332211, true);
    raw->get(AxiSignal::WId).drive(5);
    ASSERT_TRUE(transactor->beforeClock()) << transactor->getLastError();
    ASSERT_TRUE(transactor->afterClock()) << transactor->getLastError();
    backend.advance();
    std::array<std::uint8_t, 4> data{};
    ASSERT_TRUE(storage->read(0x80, data.data(), data.size()));
    EXPECT_EQ(data, (std::array<std::uint8_t, 4>{0x11, 0x22, 0x33, 0x44}));
}

TEST(AxiInitiator, PreservesPerIdOrderForBackendResponses)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    ControlledBackend backend;
    auto transactor = createAxiInitiatorTransactor(validated(core), backend);

    driveReadAddress(*raw, 7, 0x100, 1);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    driveReadAddress(*raw, 7, 0x104, 1);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    raw->get(AxiSignal::ArValid).drive(0);
    ASSERT_EQ(backend.requests.size(), 2);

    backend.responses.push_back(
        {backend.requests[1].token, 7, {5, 6, 7, 8}, false});
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::RValid).value(), 0);
    ASSERT_TRUE(transactor->afterClock());

    backend.responses.push_back(
        {backend.requests[0].token, 7, {1, 2, 3, 4}, false});
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    raw->get(AxiSignal::RReady).drive(1);
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::RData).value(), 0x04030201);
    ASSERT_TRUE(transactor->afterClock());
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::RData).value(), 0x08070605);
}

TEST(AxiInitiator, AllowsResponsesToReorderAcrossIds)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    ControlledBackend backend;
    auto transactor = createAxiInitiatorTransactor(validated(core), backend);

    driveReadAddress(*raw, 1, 0x100, 1);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    driveReadAddress(*raw, 2, 0x104, 1);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    raw->get(AxiSignal::ArValid).drive(0);
    ASSERT_EQ(backend.requests.size(), 2);
    backend.responses.push_back(
        {backend.requests[1].token, 2, {5, 6, 7, 8}, false});
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    raw->get(AxiSignal::RReady).drive(1);
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::RId).value(), 2);
    EXPECT_EQ(raw->get(AxiSignal::RData).value(), 0x08070605);
}

TEST(AxiInitiator, AppliesNarrowWriteLaneAndByteStrobes)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator, BusProtocol::Axi4, 64);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    auto storage = std::make_shared<SparseMemory>(0, 0x1000);
    MemoryBackend backend(
        storage, {.latencyCycles = 0, .maxPending = 4, .errorRanges = {}});
    auto transactor = createAxiInitiatorTransactor(validated(core), backend);
    driveWriteAddress(*raw, 1, 4, 1);
    raw->get(AxiSignal::WData)
        .drive(std::vector<std::uint8_t>{0, 0, 0, 0, 1, 2, 3, 4});
    raw->get(AxiSignal::WStrb).drive(0xd0);
    raw->get(AxiSignal::WLast).drive(1);
    raw->get(AxiSignal::WValid).drive(1);
    ASSERT_TRUE(transactor->beforeClock()) << transactor->getLastError();
    ASSERT_TRUE(transactor->afterClock());
    backend.advance();
    std::array<std::uint8_t, 4> stored{};
    ASSERT_TRUE(storage->read(4, stored.data(), stored.size()));
    EXPECT_EQ(stored, (std::array<std::uint8_t, 4>{1, 0, 3, 4}));
}

TEST(AxiInitiator, ConvertsBackendReadErrorToRresp)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    auto storage = std::make_shared<SparseMemory>(0, 0x1000);
    MemoryBackend backend(
        storage,
        {.latencyCycles = 0, .maxPending = 4, .errorRanges = {{0x100, 4}}});
    auto transactor = createAxiInitiatorTransactor(validated(core), backend);
    driveReadAddress(*raw, 3, 0x100, 1);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    backend.advance();
    raw->get(AxiSignal::ArValid).drive(0);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::RValid).value(), 1);
    EXPECT_EQ(raw->get(AxiSignal::RResp).value(), 2);
}

TEST(AxiInitiator, HandlesWrappingBurstAtFourKiBBoundary)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    auto storage = std::make_shared<SparseMemory>(0, 0x2000);
    const std::array<std::uint8_t, 16> bytes = {
        0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22,
        0x33, 0x33, 0x33, 0x33, 0x44, 0x44, 0x44, 0x44};
    ASSERT_TRUE(storage->write(0xff0, bytes.data(), nullptr, bytes.size()));
    MemoryBackend backend(
        storage, {.latencyCycles = 0, .maxPending = 4, .errorRanges = {}});
    auto transactor = createAxiInitiatorTransactor(validated(core), backend);
    driveReadAddress(*raw, 1, 0xffc, 4);
    raw->get(AxiSignal::ArBurst).drive(2);
    ASSERT_TRUE(transactor->beforeClock()) << transactor->getLastError();
    ASSERT_TRUE(transactor->afterClock());
    backend.advance();
    raw->get(AxiSignal::ArValid).drive(0);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    raw->get(AxiSignal::RReady).drive(1);
    const std::array<std::uint32_t, 4> expected = {0x44444444, 0x11111111,
                                                   0x22222222, 0x33333333};
    for (std::uint32_t word : expected) {
        ASSERT_TRUE(transactor->beforeClock()) << transactor->getLastError();
        EXPECT_EQ(raw->get(AxiSignal::RData).value(), word);
        ASSERT_TRUE(transactor->afterClock());
    }
}

TEST(AxiInitiator, RejectsMalformedWriteLast)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    ControlledBackend backend;
    auto transactor = createAxiInitiatorTransactor(validated(core), backend);
    driveWriteAddress(*raw, 1, 0x40, 2);
    driveWriteBeat(*raw, 0x04030201, true);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    raw->get(AxiSignal::AwValid).drive(0);
    driveWriteBeat(*raw, 0x08070605, true);
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_FALSE(transactor->afterClock());
    EXPECT_NE(std::string(transactor->getLastError()).find("WLAST"),
              std::string::npos);
}

TEST(AxiTarget, DrivesBurstWriteAndCollectsB)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Target);
    bus->add(AxiSignal::AwUser, 129,
             direction(BusProtocol::Axi4, BusRole::Target, AxiSignal::AwUser));
    bus->add(AxiSignal::WUser, 129,
             direction(BusProtocol::Axi4, BusRole::Target, AxiSignal::WUser));
    bus->add(AxiSignal::BUser, 129,
             direction(BusProtocol::Axi4, BusRole::Target, AxiSignal::BUser));
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    ScriptedTransactionSource source;
    MemoryRequest request;
    request.token = 42;
    request.id = 2;
    request.address = 0x100;
    request.write = true;
    request.beatBytes = 4;
    request.data = {1, 2, 3, 4, 5, 6, 7, 8};
    request.byteEnable.assign(8, 1);
    source.add(request);
    auto transactor = createAxiTargetTransactor(validated(core), source);
    raw->get(AxiSignal::AwReady).drive(1);
    raw->get(AxiSignal::WReady).drive(1);

    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::AwValid).value(), 1);
    EXPECT_EQ(raw->get(AxiSignal::WData).value(), 0x04030201);
    EXPECT_EQ(raw->get(AxiSignal::WLast).value(), 0);
    ASSERT_TRUE(transactor->afterClock());
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::AwValid).value(), 0);
    EXPECT_EQ(raw->get(AxiSignal::WData).value(), 0x08070605);
    EXPECT_EQ(raw->get(AxiSignal::WLast).value(), 1);
    ASSERT_TRUE(transactor->afterClock());

    raw->get(AxiSignal::BId).drive(2);
    raw->get(AxiSignal::BResp).drive(0);
    raw->get(AxiSignal::BValid).drive(1);
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::BReady).value(), 1);
    ASSERT_TRUE(transactor->afterClock());
    MemoryResponse response;
    ASSERT_TRUE(source.getCompleted(response));
    EXPECT_EQ(response.token, 42);
    EXPECT_FALSE(response.error);
}

TEST(AxiTarget, DrivesAxi3WriteDataId)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Target, BusProtocol::Axi3);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    ScriptedTransactionSource source;
    MemoryRequest request;
    request.token = 55;
    request.id = 6;
    request.address = 0x100;
    request.write = true;
    request.beatBytes = 4;
    request.data = {1, 2, 3, 4};
    request.byteEnable.assign(4, 1);
    source.add(request);
    auto transactor = createAxiTargetTransactor(validated(core), source);
    raw->get(AxiSignal::AwReady).drive(1);
    raw->get(AxiSignal::WReady).drive(1);
    ASSERT_TRUE(transactor->beforeClock()) << transactor->getLastError();
    EXPECT_EQ(raw->get(AxiSignal::WId).value(), 6);
    ASSERT_TRUE(transactor->afterClock());
    raw->get(AxiSignal::BId).drive(6);
    raw->get(AxiSignal::BValid).drive(1);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    EXPECT_TRUE(source.isIdle());
}

TEST(AxiTarget, PropagatesWriteResponseError)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Target);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    ScriptedTransactionSource source;
    MemoryRequest request;
    request.token = 77;
    request.id = 2;
    request.address = 0x100;
    request.write = true;
    request.beatBytes = 4;
    request.data = {1, 2, 3, 4};
    request.byteEnable.assign(4, 1);
    source.add(request, {.error = true, .data = std::nullopt});
    auto transactor = createAxiTargetTransactor(validated(core), source);
    raw->get(AxiSignal::AwReady).drive(1);
    raw->get(AxiSignal::WReady).drive(1);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    raw->get(AxiSignal::BId).drive(2);
    raw->get(AxiSignal::BResp).drive(2);
    raw->get(AxiSignal::BValid).drive(1);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock()) << transactor->getLastError();
    MemoryResponse response;
    ASSERT_TRUE(source.getCompleted(response));
    EXPECT_TRUE(response.error);
}

TEST(AxiTarget, CollectsNarrowReadBeats)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Target, BusProtocol::Axi4, 64);
    TestBus *raw = bus.get();
    core.buses.push_back(std::move(bus));
    ScriptedTransactionSource source;
    MemoryRequest request;
    request.token = 4;
    request.id = 1;
    request.address = 4;
    request.beatBytes = 4;
    request.byteEnable.assign(8, 1);
    source.add(request);
    auto transactor = createAxiTargetTransactor(validated(core), source);
    raw->get(AxiSignal::ArReady).drive(1);
    ASSERT_TRUE(transactor->beforeClock());
    EXPECT_EQ(raw->get(AxiSignal::ArSize).value(), 2);
    ASSERT_TRUE(transactor->afterClock());

    raw->get(AxiSignal::RId).drive(1);
    raw->get(AxiSignal::RData)
        .drive(std::vector<std::uint8_t>{0, 0, 0, 0, 1, 2, 3, 4});
    raw->get(AxiSignal::RValid).drive(1);
    raw->get(AxiSignal::RLast).drive(0);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    raw->get(AxiSignal::RData)
        .drive(std::vector<std::uint8_t>{5, 6, 7, 8, 0, 0, 0, 0});
    raw->get(AxiSignal::RLast).drive(1);
    ASSERT_TRUE(transactor->beforeClock());
    ASSERT_TRUE(transactor->afterClock());
    MemoryResponse response;
    ASSERT_TRUE(source.getCompleted(response));
    EXPECT_EQ(response.data,
              (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));
}

} // anonymous namespace
} // namespace gem5::rtl_cosim
