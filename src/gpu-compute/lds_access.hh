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

#ifndef __GPU_COMPUTE_LDS_ACCESS_HH__
#define __GPU_COMPUTE_LDS_ACCESS_HH__

#include <cstdint>
#include <vector>

#include "base/types.hh"

namespace gem5
{

/**
 * Per-lane LDS footprint, shared by address generation and bank timing.
 * Dual-address instructions service each component separately. B128 DS reads
 * use the 32-bank CDNA lane groups documented in AMD's CK Tile LDS guide:
 * https://rocm.docs.amd.com/projects/composable_kernel/en/docs-7.1.1/
 * conceptual/ck_tile/hardware/lds_bank_conflicts.html
 * Other widths use contiguous lane groups sized to the bank word capacity.
 * This is a bank serialization model, not a detailed model of bank ports or
 * architecture-specific transpose instructions. Instruction latency and
 * register dependencies remain the responsibility of the existing pipeline.
 */
struct LdsAccess
{
    unsigned bytes = 4;
    unsigned count = 1;
    Addr offsets[2] = {0, 0};
    bool readB128 = false;
};

struct LdsBankAccesses
{
    unsigned accesses = 0;
    unsigned phases = 0;
    unsigned conflicts = 0;
};

LdsBankAccesses countLdsBankAccesses(const std::vector<Addr> &addresses,
                                     uint64_t mask, const LdsAccess &access,
                                     unsigned banks, bool broadcast);

} // namespace gem5

#endif // __GPU_COMPUTE_LDS_ACCESS_HH__
