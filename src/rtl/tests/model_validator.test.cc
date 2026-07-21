/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include "rtl/runtime/model_validator.hh"
#include "rtl/tests/test_model.hh"

namespace gem5::rtl_cosim
{
namespace
{

using namespace test;

TEST(ModelValidator, AcceptsVariableWidthApb)
{
    TestCore core;
    core.buses.push_back(makeApb(BusRole::Initiator, 128));
    ValidationResult result = validateModel(core);
    EXPECT_TRUE(result.ok());
    ASSERT_EQ(result.buses.size(), 1);
    EXPECT_EQ(result.buses[0].require(ApbSignal::PWData).bitWidth(), 128);
}

TEST(ModelValidator, AcceptsAxi4OptionalSignalsAbsent)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Target);
    for (SignalRoleId role :
         {AxiSignal::AwId, AxiSignal::BId, AxiSignal::ArId, AxiSignal::RId,
          AxiSignal::AwLock, AxiSignal::ArLock, AxiSignal::AwCache,
          AxiSignal::ArCache, AxiSignal::AwProt, AxiSignal::ArProt}) {
        bus->remove(role);
    }
    core.buses.push_back(std::move(bus));
    EXPECT_TRUE(validateModel(core).ok());
}

TEST(ModelValidator, AcceptsAxi3AndWideOptionalUserSignals)
{
    TestCore axi3Core;
    axi3Core.buses.push_back(
        makeAxi(BusRole::Target, BusProtocol::Axi3, 1024));
    EXPECT_TRUE(validateModel(axi3Core).ok());

    TestCore axi4Core;
    auto axi4 = makeAxi(BusRole::Initiator);
    axi4->add(
        AxiSignal::AwUser, 257,
        direction(BusProtocol::Axi4, BusRole::Initiator, AxiSignal::AwUser));
    axi4Core.buses.push_back(std::move(axi4));
    EXPECT_TRUE(validateModel(axi4Core).ok());
}

TEST(ModelValidator, RejectsPartialAxiIdPair)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator);
    bus->remove(AxiSignal::BId);
    core.buses.push_back(std::move(bus));
    ValidationResult result = validateModel(core);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.errors.front().find("AWID and BID"), std::string::npos);
}

TEST(ModelValidator, RejectsDuplicateAndWrongWidth)
{
    TestCore core;
    auto bus = makeApb(BusRole::Initiator);
    bus->duplicate(ApbSignal::PAddr);
    bus->get(ApbSignal::PReady).setWidthForTest(2);
    core.buses.push_back(std::move(bus));
    ValidationResult result = validateModel(core);
    EXPECT_FALSE(result.ok());
    EXPECT_GE(result.errors.size(), 2);
}

TEST(ModelValidator, RejectsInvalidEnumsDirectionsAndStandaloneMetadata)
{
    TestCore core;
    auto bus = makeApb(BusRole::Initiator);
    bus->get(ApbSignal::PReady).setDirectionForTest(SignalDirection::Output);
    core.buses.push_back(std::move(bus));
    core.addCoreSignal("bad", static_cast<CoreSignalRole>(99), 0,
                       static_cast<ActiveLevel>(99), 1,
                       SignalDirection::Input);
    ValidationResult result = validateModel(core);
    EXPECT_FALSE(result.ok());
    EXPECT_GE(result.errors.size(), 3);

    TestCore protocolCore;
    protocolCore.buses.push_back(std::make_unique<TestBus>(
        "bad", static_cast<BusProtocol>(99), BusRole::Initiator));
    EXPECT_FALSE(validateModel(protocolCore).ok());
}

TEST(ModelValidator, RejectsDuplicateStandaloneSignalNames)
{
    TestCore core;
    core.addCoreSignal("duplicate", CoreSignalRole::Io, 0, ActiveLevel::High,
                       8, SignalDirection::Input);
    core.addCoreSignal("duplicate", CoreSignalRole::Io, 1, ActiveLevel::High,
                       8, SignalDirection::Input);
    EXPECT_FALSE(validateModel(core).ok());
}

TEST(ModelValidator, ValidatesFullAceSignalListStructurally)
{
    TestCore core;
    core.buses.push_back(
        makeAxi(BusRole::Initiator, BusProtocol::Axi3Ace, 64));
    ValidationResult result = validateModel(core);
    EXPECT_TRUE(result.ok());
    ASSERT_EQ(result.buses.size(), 1);
    EXPECT_NE(result.buses[0].find(AxiSignal::AcValid), nullptr);
    EXPECT_NE(result.buses[0].find(AxiSignal::Wack), nullptr);
}

TEST(ModelValidator, RejectsMissingAceChannel)
{
    TestCore core;
    auto bus = makeAxi(BusRole::Initiator, BusProtocol::Axi3Ace);
    bus->remove(AxiSignal::CrValid);
    core.buses.push_back(std::move(bus));
    EXPECT_FALSE(validateModel(core).ok());
}

} // anonymous namespace
} // namespace gem5::rtl_cosim
