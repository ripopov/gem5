/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __RT_ACCEL_ACCELERATOR_HH__
#define __RT_ACCEL_ACCELERATOR_HH__

#include <cstddef>
#include <memory>

#include "base/addr_range.hh"
#include "rt_accel/api.hh"

namespace gem5
{
namespace rt_accel
{

struct Config
{
    AddrRange mmioRange;
    Tick clockPeriod;
    std::size_t queueDepth = 16;
    RequestorID requestorId = Request::invldRequestorId;
};

class Accelerator : public PortAPI
{
  public:
    virtual void connectNonCoherentBus(PortAPI &responder) = 0;
    virtual void useExternalTransport(LinkTiming mmio, LinkTiming master) = 0;
    virtual void runCycle(Tick edge) = 0;
    // Called only by the owner; used to drain the surrounding SimObject.
    virtual bool idle() const = 0;
};

// Creates an independent instance. The initial implementation is an MMIO
// storage mock; it does not execute RISC-V instructions or initiate DMA.
std::unique_ptr<Accelerator> createAccelerator(const Config &config);

} // namespace rt_accel
} // namespace gem5

#endif // __RT_ACCEL_ACCELERATOR_HH__
