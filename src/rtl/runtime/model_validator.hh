/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_COSIM_RUNTIME_MODEL_VALIDATOR_HH__
#define __RTL_COSIM_RUNTIME_MODEL_VALIDATOR_HH__

#include <string>
#include <unordered_map>
#include <vector>

#include "gem5/rtl_cosim/api_v1.hh"

namespace gem5::rtl_cosim
{

struct ValidatedBus
{
    Bus *bus = nullptr;
    std::unordered_map<SignalRoleId, Signal *> signals;

    Signal *find(SignalRoleId role) const noexcept;
    Signal &require(SignalRoleId role) const;
};

struct ValidationResult
{
    std::vector<ValidatedBus> buses;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    bool
    ok() const noexcept
    {
        return errors.empty();
    }
};

ValidationResult validateModel(RtlCore &core);
const char *protocolName(BusProtocol protocol) noexcept;
const char *busRoleName(BusRole role) noexcept;
std::string signalRoleName(BusProtocol protocol, SignalRoleId role);

} // namespace gem5::rtl_cosim

#endif // __RTL_COSIM_RUNTIME_MODEL_VALIDATOR_HH__
