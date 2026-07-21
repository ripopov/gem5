/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_COSIM_RUNTIME_PROTOCOL_APB_HH__
#define __RTL_COSIM_RUNTIME_PROTOCOL_APB_HH__

#include <cstddef>
#include <cstdint>
#include <memory>

#include "rtl/runtime/model_validator.hh"
#include "rtl/runtime/transaction.hh"

namespace gem5::rtl_cosim
{

struct TransactorLimits
{
    std::size_t maxPending = 64;
    std::size_t maxBurstBeats = 256;
    std::uint64_t tokenBase = 0;
};

std::unique_ptr<BusTransactor>
createApbInitiatorTransactor(const ValidatedBus &bus,
                             TransactionBackend &backend,
                             const TransactorLimits &limits = {});
std::unique_ptr<BusTransactor>
createApbTargetTransactor(const ValidatedBus &bus, TransactionSource &source,
                          const TransactorLimits &limits = {});

} // namespace gem5::rtl_cosim

#endif // __RTL_COSIM_RUNTIME_PROTOCOL_APB_HH__
