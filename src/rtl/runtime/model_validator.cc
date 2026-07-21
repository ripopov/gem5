/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/runtime/model_validator.hh"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace gem5::rtl_cosim
{

Signal *
ValidatedBus::find(SignalRoleId role) const noexcept
{
    const auto it = signals.find(role);
    return it == signals.end() ? nullptr : it->second;
}

Signal &
ValidatedBus::require(SignalRoleId role) const
{
    Signal *signal = find(role);
    if (!signal) {
        throw std::logic_error("validated bus is missing a required signal");
    }
    return *signal;
}

const char *
protocolName(BusProtocol protocol) noexcept
{
    switch (protocol) {
        case BusProtocol::Apb:
            return "APB";
        case BusProtocol::Axi3:
            return "AXI3";
        case BusProtocol::Axi3Ace:
            return "AXI3-ACE";
        case BusProtocol::Axi4:
            return "AXI4";
        default:
            return "Unknown";
    }
}

const char *
busRoleName(BusRole role) noexcept
{
    switch (role) {
        case BusRole::Initiator:
            return "initiator";
        case BusRole::Target:
            return "target";
    }
    return "invalid";
}

std::string
signalRoleName(BusProtocol protocol, SignalRoleId role)
{
    static const std::array<const char *, 11> apb = {
        "invalid", "PADDR", "PSEL",   "PENABLE", "PWRITE", "PWDATA",
        "PSTRB",   "PPROT", "PREADY", "PRDATA",  "PSLVERR"};
    static const std::array<const char *, 66> axi = {
        "invalid",  "AWID",     "AWADDR",  "AWLEN",   "AWSIZE",   "AWBURST",
        "AWLOCK",   "AWCACHE",  "AWPROT",  "AWVALID", "AWREADY",  "WID",
        "WDATA",    "WSTRB",    "WLAST",   "WVALID",  "WREADY",   "BID",
        "BRESP",    "BVALID",   "BREADY",  "ARID",    "ARADDR",   "ARLEN",
        "ARSIZE",   "ARBURST",  "ARLOCK",  "ARCACHE", "ARPROT",   "ARVALID",
        "ARREADY",  "RID",      "RDATA",   "RRESP",   "RLAST",    "RVALID",
        "RREADY",   "AWREGION", "AWQOS",   "AWUSER",  "WUSER",    "BUSER",
        "ARREGION", "ARQOS",    "ARUSER",  "RUSER",   "AWDOMAIN", "AWSNOOP",
        "AWBAR",    "ARDOMAIN", "ARSNOOP", "ARBAR",   "ACADDR",   "ACSNOOP",
        "ACPROT",   "ACVALID",  "ACREADY", "CRRESP",  "CRVALID",  "CRREADY",
        "CDDATA",   "CDLAST",   "CDVALID", "CDREADY", "RACK",     "WACK"};
    if (protocol == BusProtocol::Apb && role < apb.size()) {
        return apb[role];
    }
    if (protocol != BusProtocol::Apb && role < axi.size()) {
        return axi[role];
    }
    return "role-" + std::to_string(role);
}

namespace
{

struct Profile
{
    std::set<SignalRoleId> required;
    std::set<SignalRoleId> optional;
    std::set<SignalRoleId> prohibited;
};

Profile
apbProfile()
{
    return {{ApbSignal::PAddr, ApbSignal::PSel, ApbSignal::PEnable,
             ApbSignal::PWrite, ApbSignal::PWData, ApbSignal::PReady,
             ApbSignal::PRData},
            {ApbSignal::PStrb, ApbSignal::PProt, ApbSignal::PSlvErr},
            {}};
}

Profile
axiProfile(BusProtocol protocol)
{
    Profile result;
    for (SignalRoleId role = AxiSignal::AwId; role <= AxiSignal::RReady;
         ++role) {
        result.required.insert(role);
    }
    if (protocol == BusProtocol::Axi4) {
        result.required.erase(AxiSignal::WId);
        result.prohibited.insert(AxiSignal::WId);
        for (SignalRoleId role :
             {AxiSignal::AwId, AxiSignal::BId, AxiSignal::ArId, AxiSignal::RId,
              AxiSignal::AwLock, AxiSignal::ArLock, AxiSignal::AwCache,
              AxiSignal::ArCache, AxiSignal::AwProt, AxiSignal::ArProt}) {
            result.required.erase(role);
            result.optional.insert(role);
        }
        for (SignalRoleId role = AxiSignal::AwRegion; role <= AxiSignal::RUser;
             ++role) {
            result.optional.insert(role);
        }
    } else if (protocol == BusProtocol::Axi3Ace) {
        for (SignalRoleId role = AxiSignal::AwDomain; role <= AxiSignal::Wack;
             ++role) {
            result.required.insert(role);
        }
    }
    return result;
}

bool
isRequestRole(BusProtocol protocol, SignalRoleId role)
{
    if (protocol == BusProtocol::Apb) {
        return role >= ApbSignal::PAddr && role <= ApbSignal::PProt;
    }
    if (role >= AxiSignal::AwId && role <= AxiSignal::AwValid) {
        return true;
    }
    if (role >= AxiSignal::WId && role <= AxiSignal::WValid) {
        return true;
    }
    if (role == AxiSignal::BReady) {
        return true;
    }
    if (role >= AxiSignal::ArId && role <= AxiSignal::ArValid) {
        return true;
    }
    if (role == AxiSignal::RReady) {
        return true;
    }
    if (role >= AxiSignal::AwRegion && role <= AxiSignal::WUser) {
        return true;
    }
    if (role >= AxiSignal::ArRegion && role <= AxiSignal::ArUser) {
        return true;
    }
    if (role >= AxiSignal::AwDomain && role <= AxiSignal::ArBar) {
        return true;
    }
    if (role == AxiSignal::AcReady) {
        return true;
    }
    if (role >= AxiSignal::CrResp && role <= AxiSignal::CrValid) {
        return true;
    }
    if (role >= AxiSignal::CdData && role <= AxiSignal::CdValid) {
        return true;
    }
    return role == AxiSignal::Rack || role == AxiSignal::Wack;
}

SignalDirection
expectedDirection(const Bus &bus, SignalRoleId role)
{
    const bool request = isRequestRole(bus.protocol(), role);
    const bool output = bus.role() == BusRole::Initiator ? request : !request;
    return output ? SignalDirection::Output : SignalDirection::Input;
}

void
addError(ValidationResult &result, const Bus &bus, const std::string &message)
{
    result.errors.emplace_back(
        "bus '" + std::string(bus.name() ? bus.name() : "<unnamed>") + "' (" +
        protocolName(bus.protocol()) + "): " + message);
}

bool
powerOfTwo(std::size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

void
fixedWidth(ValidationResult &result, const ValidatedBus &bus,
           SignalRoleId role, std::size_t width)
{
    Signal *signal = bus.find(role);
    if (signal && signal->bitWidth() != width) {
        addError(result, *bus.bus,
                 signalRoleName(bus.bus->protocol(), role) + " must be " +
                     std::to_string(width) + " bits, got " +
                     std::to_string(signal->bitWidth()));
    }
}

void
sameWidth(ValidationResult &result, const ValidatedBus &bus, SignalRoleId lhs,
          SignalRoleId rhs)
{
    Signal *a = bus.find(lhs);
    Signal *b = bus.find(rhs);
    if (a && b && a->bitWidth() != b->bitWidth()) {
        addError(result, *bus.bus,
                 signalRoleName(bus.bus->protocol(), lhs) + " and " +
                     signalRoleName(bus.bus->protocol(), rhs) +
                     " widths must match");
    }
}

void
validateApbWidths(ValidationResult &result, const ValidatedBus &bus)
{
    for (SignalRoleId role :
         {ApbSignal::PSel, ApbSignal::PEnable, ApbSignal::PWrite,
          ApbSignal::PReady, ApbSignal::PSlvErr}) {
        fixedWidth(result, bus, role, 1);
    }
    fixedWidth(result, bus, ApbSignal::PProt, 3);
    Signal *address = bus.find(ApbSignal::PAddr);
    if (address && (address->bitWidth() == 0 || address->bitWidth() > 64)) {
        addError(result, *bus.bus, "PADDR must be between 1 and 64 bits");
    }
    sameWidth(result, bus, ApbSignal::PWData, ApbSignal::PRData);
    Signal *data = bus.find(ApbSignal::PWData);
    if (data && (!powerOfTwo(data->bitWidth()) || data->bitWidth() < 8 ||
                 data->bitWidth() > 1024)) {
        addError(
            result, *bus.bus,
            "PWDATA width must be a power of two between 8 and 1024 bits");
    }
    Signal *strobe = bus.find(ApbSignal::PStrb);
    if (strobe && data && strobe->bitWidth() * 8 != data->bitWidth()) {
        addError(result, *bus.bus, "PSTRB must have one bit per data byte");
    }
}

void
validateAxiWidths(ValidationResult &result, const ValidatedBus &bus)
{
    const BusProtocol protocol = bus.bus->protocol();
    for (SignalRoleId role :
         {AxiSignal::AwValid, AxiSignal::AwReady, AxiSignal::WLast,
          AxiSignal::WValid, AxiSignal::WReady, AxiSignal::BValid,
          AxiSignal::BReady, AxiSignal::ArValid, AxiSignal::ArReady,
          AxiSignal::RLast, AxiSignal::RValid, AxiSignal::RReady}) {
        fixedWidth(result, bus, role, 1);
    }
    const std::size_t lenWidth = protocol == BusProtocol::Axi4 ? 8 : 4;
    fixedWidth(result, bus, AxiSignal::AwLen, lenWidth);
    fixedWidth(result, bus, AxiSignal::ArLen, lenWidth);
    fixedWidth(result, bus, AxiSignal::AwSize, 3);
    fixedWidth(result, bus, AxiSignal::ArSize, 3);
    fixedWidth(result, bus, AxiSignal::AwBurst, 2);
    fixedWidth(result, bus, AxiSignal::ArBurst, 2);
    fixedWidth(result, bus, AxiSignal::AwLock,
               protocol == BusProtocol::Axi4 ? 1 : 2);
    fixedWidth(result, bus, AxiSignal::ArLock,
               protocol == BusProtocol::Axi4 ? 1 : 2);
    fixedWidth(result, bus, AxiSignal::AwCache, 4);
    fixedWidth(result, bus, AxiSignal::ArCache, 4);
    fixedWidth(result, bus, AxiSignal::AwProt, 3);
    fixedWidth(result, bus, AxiSignal::ArProt, 3);
    fixedWidth(result, bus, AxiSignal::BResp, 2);
    fixedWidth(result, bus, AxiSignal::RResp, 2);
    for (SignalRoleId role : {AxiSignal::AwRegion, AxiSignal::AwQos,
                              AxiSignal::ArRegion, AxiSignal::ArQos}) {
        fixedWidth(result, bus, role, 4);
    }
    sameWidth(result, bus, AxiSignal::AwAddr, AxiSignal::ArAddr);
    sameWidth(result, bus, AxiSignal::WData, AxiSignal::RData);
    sameWidth(result, bus, AxiSignal::AwId, AxiSignal::BId);
    sameWidth(result, bus, AxiSignal::ArId, AxiSignal::RId);
    Signal *address = bus.find(AxiSignal::AwAddr);
    if (address && (address->bitWidth() == 0 || address->bitWidth() > 64)) {
        addError(result, *bus.bus,
                 "AXI address width must be between 1 and 64 bits");
    }
    Signal *data = bus.find(AxiSignal::WData);
    if (data && (!powerOfTwo(data->bitWidth()) || data->bitWidth() < 8 ||
                 data->bitWidth() > 1024)) {
        addError(
            result, *bus.bus,
            "AXI data width must be a power of two between 8 and 1024 bits");
    }
    Signal *strobe = bus.find(AxiSignal::WStrb);
    if (strobe && data && strobe->bitWidth() * 8 != data->bitWidth()) {
        addError(result, *bus.bus, "WSTRB must have one bit per data byte");
    }
    for (SignalRoleId role : {AxiSignal::AwId, AxiSignal::BId, AxiSignal::WId,
                              AxiSignal::ArId, AxiSignal::RId}) {
        Signal *id = bus.find(role);
        if (id && (id->bitWidth() == 0 || id->bitWidth() > 32)) {
            addError(result, *bus.bus,
                     signalRoleName(protocol, role) +
                         " width must be between 1 and 32 bits");
        }
    }
    if (protocol == BusProtocol::Axi4) {
        const bool awId = bus.find(AxiSignal::AwId);
        const bool bId = bus.find(AxiSignal::BId);
        const bool arId = bus.find(AxiSignal::ArId);
        const bool rId = bus.find(AxiSignal::RId);
        if (awId != bId) {
            addError(result, *bus.bus,
                     "AWID and BID must both be present or absent");
        }
        if (arId != rId) {
            addError(result, *bus.bus,
                     "ARID and RID must both be present or absent");
        }
    }
    if (protocol == BusProtocol::Axi3Ace) {
        fixedWidth(result, bus, AxiSignal::AwDomain, 2);
        fixedWidth(result, bus, AxiSignal::AwSnoop, 3);
        fixedWidth(result, bus, AxiSignal::AwBar, 2);
        fixedWidth(result, bus, AxiSignal::ArDomain, 2);
        fixedWidth(result, bus, AxiSignal::ArSnoop, 4);
        fixedWidth(result, bus, AxiSignal::ArBar, 2);
        fixedWidth(result, bus, AxiSignal::AcSnoop, 4);
        fixedWidth(result, bus, AxiSignal::AcProt, 3);
        fixedWidth(result, bus, AxiSignal::CrResp, 5);
        for (SignalRoleId role :
             {AxiSignal::AcValid, AxiSignal::AcReady, AxiSignal::CrValid,
              AxiSignal::CrReady, AxiSignal::CdLast, AxiSignal::CdValid,
              AxiSignal::CdReady, AxiSignal::Rack, AxiSignal::Wack}) {
            fixedWidth(result, bus, role, 1);
        }
        sameWidth(result, bus, AxiSignal::AcAddr, AxiSignal::AwAddr);
        sameWidth(result, bus, AxiSignal::CdData, AxiSignal::WData);
    }
}

ValidatedBus
validateBus(ValidationResult &result, Bus &bus)
{
    ValidatedBus validated{&bus, {}};
    const Profile profile = bus.protocol() == BusProtocol::Apb
                                ? apbProfile()
                                : axiProfile(bus.protocol());
    if (!bus.name() || !*bus.name()) {
        addError(result, bus, "name is null or empty");
    }
    if (bus.role() != BusRole::Initiator && bus.role() != BusRole::Target) {
        addError(result, bus, "invalid bus role");
    }

    std::unordered_set<Signal *> signalPointers;
    for (std::size_t index = 0; index < bus.signalCount(); ++index) {
        const SignalBinding binding = bus.signal(index);
        if (!binding.signal) {
            addError(result, bus,
                     "binding " + std::to_string(index) +
                         " has a null signal");
            continue;
        }
        if (!signalPointers.insert(binding.signal).second) {
            addError(result, bus,
                     "the same Signal object is bound to multiple roles");
        }
        if (!validated.signals.emplace(binding.role, binding.signal).second) {
            addError(result, bus,
                     "duplicate binding for " +
                         signalRoleName(bus.protocol(), binding.role));
            continue;
        }
        if (profile.required.count(binding.role) == 0 &&
            profile.optional.count(binding.role) == 0) {
            addError(result, bus,
                     "unknown or prohibited role " +
                         signalRoleName(bus.protocol(), binding.role));
        }
        Signal &signal = *binding.signal;
        if (!signal.name() || !*signal.name()) {
            addError(result, bus, "a bound signal has a null or empty name");
        }
        if (signal.bitWidth() == 0) {
            addError(result, bus,
                     signalRoleName(bus.protocol(), binding.role) +
                         " has zero width");
        }
        if (signal.direction() != SignalDirection::Input &&
            signal.direction() != SignalDirection::Output) {
            addError(result, bus,
                     signalRoleName(bus.protocol(), binding.role) +
                         " has an invalid direction");
        } else if (signal.direction() !=
                   expectedDirection(bus, binding.role)) {
            addError(result, bus,
                     signalRoleName(bus.protocol(), binding.role) +
                         " has the wrong direction for an RTL " +
                         busRoleName(bus.role()));
        }
    }
    for (SignalRoleId role : profile.required) {
        if (!validated.find(role)) {
            addError(result, bus,
                     "missing required " +
                         signalRoleName(bus.protocol(), role));
        }
    }
    if (bus.protocol() == BusProtocol::Apb) {
        validateApbWidths(result, validated);
    } else {
        validateAxiWidths(result, validated);
    }
    return validated;
}

} // anonymous namespace

ValidationResult
validateModel(RtlCore &core)
{
    ValidationResult result;
    if (!core.name() || !*core.name()) {
        result.errors.emplace_back("core name is null or empty");
    }

    std::unordered_set<std::string> busNames;
    for (std::size_t index = 0; index < core.busCount(); ++index) {
        Bus *bus = core.bus(index);
        if (!bus) {
            result.errors.emplace_back("core bus " + std::to_string(index) +
                                       " is null");
            continue;
        }
        if (bus->protocol() != BusProtocol::Apb &&
            bus->protocol() != BusProtocol::Axi3 &&
            bus->protocol() != BusProtocol::Axi3Ace &&
            bus->protocol() != BusProtocol::Axi4) {
            result.errors.emplace_back(
                "bus '" +
                std::string(bus->name() ? bus->name() : "<unnamed>") +
                "' has an unknown or invalid protocol");
            continue;
        }
        const std::string name = bus->name() ? bus->name() : "";
        if (!name.empty() && !busNames.insert(name).second) {
            result.errors.emplace_back("duplicate bus name '" + name + "'");
        }
        result.buses.emplace_back(validateBus(result, *bus));
    }

    std::set<std::pair<CoreSignalRole, std::uint32_t>> coreRoles;
    std::unordered_set<std::string> coreSignalNames;
    std::unordered_set<Signal *> coreSignalPointers;
    for (std::size_t index = 0; index < core.signalCount(); ++index) {
        CoreSignalBinding binding = core.signal(index);
        if (!binding.signal) {
            result.errors.emplace_back("core signal " + std::to_string(index) +
                                       " is null");
            continue;
        }
        if (binding.role != CoreSignalRole::Reset &&
            binding.role != CoreSignalRole::Interrupt &&
            binding.role != CoreSignalRole::Io) {
            result.errors.emplace_back(
                "standalone signal has an invalid semantic role");
        }
        if (binding.activeLevel != ActiveLevel::Low &&
            binding.activeLevel != ActiveLevel::High) {
            result.errors.emplace_back(
                "standalone signal has an invalid active level");
        }
        if (!coreRoles.emplace(binding.role, binding.index).second) {
            result.errors.emplace_back(
                "duplicate standalone signal role/index at " +
                std::to_string(index));
        }
        if (!coreSignalPointers.insert(binding.signal).second) {
            result.errors.emplace_back(
                "the same standalone Signal object is enumerated twice");
        }
        if (!binding.signal->name() || !*binding.signal->name()) {
            result.errors.emplace_back("standalone signal has no name");
        } else if (!coreSignalNames.insert(binding.signal->name()).second) {
            result.errors.emplace_back("duplicate standalone signal name '" +
                                       std::string(binding.signal->name()) +
                                       "'");
        }
        if (binding.signal->bitWidth() == 0) {
            result.errors.emplace_back("standalone signal has zero width");
        }
        if ((binding.role == CoreSignalRole::Reset ||
             binding.role == CoreSignalRole::Interrupt) &&
            binding.signal->bitWidth() != 1) {
            result.errors.emplace_back(
                "reset and interrupt signals must be one bit");
        }
    }

    std::unordered_set<std::string> memoryNames;
    for (std::size_t index = 0; index < core.memoryCount(); ++index) {
        const MemoryRegion *memory = core.memory(index);
        if (!memory) {
            result.errors.emplace_back("memory region " +
                                       std::to_string(index) + " is null");
            continue;
        }
        if (!memory->name || !*memory->name) {
            result.errors.emplace_back("memory region has no name");
        } else if (!memoryNames.insert(memory->name).second) {
            result.errors.emplace_back("duplicate memory name '" +
                                       std::string(memory->name) + "'");
        }
        if (memory->size == 0) {
            result.errors.emplace_back("memory region has zero size");
        }
        if (memory->role != MemoryRole::CodeTcm &&
            memory->role != MemoryRole::DataTcm) {
            result.errors.emplace_back("memory region has an invalid role");
        }
        if (memory->size >
            std::numeric_limits<std::uint64_t>::max() - memory->baseAddress) {
            result.errors.emplace_back(
                "memory region address range overflows");
        }
    }
    return result;
}

} // namespace gem5::rtl_cosim
