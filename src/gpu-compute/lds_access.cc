/*
 * Copyright (c) 2014-2015 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "gpu-compute/lds_access.hh"

#include <algorithm>
#include <cassert>

namespace gem5
{

LdsBankAccesses
countLdsBankAccesses(const std::vector<Addr> &addresses, uint64_t mask,
                     const LdsAccess &access, unsigned banks, bool broadcast)
{
    assert(banks > 0 && addresses.size() <= 64);
    assert(access.bytes > 0 && access.bytes <= 16);
    assert(access.count > 0 && access.count <= 2);
    LdsBankAccesses result;
    // A bank serves a 32-bit word. Reserve four word slots for B96/B128.
    const unsigned words = access.bytes <= 4 ? 1 : access.bytes <= 8 ? 2 : 4;
    const unsigned lanesPerPhase = std::max(1U, banks / words);
    const unsigned phaseCount =
        (addresses.size() + lanesPerPhase - 1) / lanesPerPhase;
    std::vector<unsigned> requests(banks);
    std::vector<Addr> reads;
    reads.reserve(banks);
    for (unsigned part = 0; part < access.count; ++part) {
        for (unsigned phase = 0; phase < phaseCount; ++phase) {
            std::fill(requests.begin(), requests.end(), 0);
            reads.clear();
            unsigned maximum = 0;
            for (unsigned lane = 0; lane < addresses.size(); ++lane) {
                unsigned lanePhase = lane / lanesPerPhase;
                if (access.readB128 && banks == 32) {
                    // CDNA B128 reads pair four lanes from each half-wave:
                    // 0:3+20:23, 4:7+16:19, 8:11+28:31, 12:15+24:27.
                    lanePhase = (lane / 32) * 4 +
                                ((lane % 16) / 4 ^ ((lane % 32) / 16));
                }
                if (lanePhase != phase || !(mask & (uint64_t(1) << lane))) {
                    continue;
                }
                const Addr first = addresses[lane] + access.offsets[part];
                const Addr last = (first + access.bytes - 1) / 4;
                for (Addr word = first / 4; word <= last; ++word) {
                    if (broadcast) {
                        if (std::find(reads.begin(), reads.end(), word) !=
                            reads.end()) {
                            continue;
                        }
                        reads.push_back(word);
                    }
                    ++result.accesses;
                    maximum = std::max(maximum, ++requests[word % banks]);
                }
            }
            if (maximum) {
                ++result.phases;
                result.conflicts += maximum - 1;
            }
        }
    }
    return result;
}

} // namespace gem5
