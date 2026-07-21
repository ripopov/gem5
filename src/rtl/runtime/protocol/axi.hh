/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_COSIM_RUNTIME_PROTOCOL_AXI_HH__
#define __RTL_COSIM_RUNTIME_PROTOCOL_AXI_HH__

#include <memory>

#include "rtl/runtime/protocol/apb.hh"

namespace gem5::rtl_cosim
{

std::unique_ptr<BusTransactor>
createAxiInitiatorTransactor(const ValidatedBus &bus,
                             TransactionBackend &backend,
                             const TransactorLimits &limits = {});
std::unique_ptr<BusTransactor>
createAxiTargetTransactor(const ValidatedBus &bus, TransactionSource &source,
                          const TransactorLimits &limits = {});

} // namespace gem5::rtl_cosim

#endif // __RTL_COSIM_RUNTIME_PROTOCOL_AXI_HH__
