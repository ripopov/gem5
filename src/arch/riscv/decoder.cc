/*
 * Copyright (c) 2012 Google
 * Copyright (c) The University of Virginia
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

#include "arch/riscv/decoder.hh"
#include "arch/riscv/insts/zcmt.hh"
#include "arch/riscv/isa.hh"
#include "arch/riscv/types.hh"
#include "base/bitfield.hh"
#include "debug/Decode.hh"

namespace gem5
{

namespace RiscvISA
{

Decoder::Decoder(const RiscvDecoderParams &p) : InstDecoder(p, &machInst)
{
    ISA *isa = dynamic_cast<ISA*>(p.isa);
    vlen = isa->getVecLenInBits();
    elen = isa->getVecElemLenInBits();
    _hasZcd = isa->reportsExtension("Zcd");
    decodedInsts.resize(NumDecodedInsts);
    reset();
}

void Decoder::reset()
{
    InstDecoder::reset();
    aligned = true;
    mid = false;
    machInst = 0;
    emi = 0;
    jvtEntry = 0;
    squashed = true;
}

void
Decoder::moreBytes(const PCStateBase &pc, Addr fetchPC)
{
    // The MSB of the upper and lower halves of a machine instruction.
    constexpr size_t max_bit = sizeof(machInst) * 8 - 1;
    constexpr size_t mid_bit = sizeof(machInst) * 4 - 1;

    auto inst = letoh(machInst);
    DPRINTF(Decode, "Requesting bytes 0x%08x from address %#x\n", inst,
            fetchPC);

    const PCState &pc_state = pc.as<PCState>();

    if (GEM5_UNLIKELY(pc_state.zcmtSecondFetch())) {
        if (mid) {
            replaceBits(jvtEntry, sizeof(jvtEntry) * 8 - 1, max_bit + 1, inst);
            mid = false;
            instDone = true;
            outOfBytes = true;
        } else {
            replaceBits(jvtEntry, max_bit, 0, inst);
            mid = (pc_state.rvType() != RV32);
            instDone = (pc_state.rvType() == RV32);
            outOfBytes = true;
        }

        if (instDone && pc_state.rvType() == RV32) {
            jvtEntry = sext<32>(jvtEntry);
        }
        return;
    }

    // Assemble the instruction bits in a local and store the whole union
    // once: a 32-bit field write into emi followed by decode()'s 64-bit
    // read of the union stalls on store forwarding.
    ExtMachInst next_emi = emi;
    uint32_t inst_bits = next_emi.instBits;
    bool aligned = pc.instAddr() % sizeof(machInst) == 0;
    if (aligned) {
        inst_bits = inst;
        if (compressed(inst))
            inst_bits = bits(inst, mid_bit, 0);
        outOfBytes = !compressed(inst);
        instDone = true;
    } else {
        if (mid) {
            assert(bits(inst_bits, max_bit, mid_bit + 1) == 0);
            replaceBits(inst_bits, max_bit, mid_bit + 1, inst);
            mid = false;
            outOfBytes = false;
            instDone = true;
        } else {
            inst_bits = bits(inst, max_bit, mid_bit + 1);
            const bool is_compressed = compressed(inst_bits);
            mid = !is_compressed;
            outOfBytes = true;
            instDone = is_compressed;
        }
    }
    next_emi.instBits = inst_bits;
    emi = next_emi;
}

StaticInstPtr
Decoder::decode(ExtMachInst mach_inst, Addr addr)
{
    DPRINTF(Decode, "Decoding instruction 0x%08x at address %#x\n",
            mach_inst.instBits, addr);

    // Instructions are at least halfword aligned.
    DecodedInst &entry = decodedInsts[(addr >> 1) % NumDecodedInsts];
    if (entry.inst && entry.addr == addr && entry.machInst == mach_inst)
        return entry.inst;

    StaticInstPtr &si = instMap[mach_inst];
    if (!si)
        si = decodeInst(mach_inst);

    si->size(compressed(mach_inst) ? 2 : 4);

    entry.addr = addr;
    entry.machInst = mach_inst;
    entry.inst = si;

    DPRINTF(Decode, "Decode: Decoded %s instruction: %#x\n",
            si->getName(), mach_inst);
    return si;
}

StaticInstPtr
Decoder::decode(PCStateBase &_next_pc)
{
    if (!instDone)
        return nullptr;
    instDone = false;

    auto &next_pc = _next_pc.as<PCState>();

    if (GEM5_UNLIKELY(next_pc.zcmtSecondFetch())) {
        return new ZcmtSecondFetchInst(emi, jvtEntry);
    }

    if (compressed(emi)) {
        next_pc.npc(next_pc.instAddr() + sizeof(machInst) / 2);
        next_pc.compressed(true);
    } else {
        next_pc.npc(next_pc.instAddr() + sizeof(machInst));
        next_pc.compressed(false);
    }

    if (GEM5_UNLIKELY(squashed || next_pc.new_vconf())) {
        squashed = false;
        next_pc.new_vconf(false);
        vl = next_pc.vl();
        vtype = next_pc.vtype();
    } else {
        next_pc.vl(vl);
        next_pc.vtype(vtype);
    }

    // Fill the context fields in a register copy; successive bitfield
    // writes to the member would each read and write the whole union
    // through memory.
    ExtMachInst inst = emi;
    inst.vl      = vl;
    inst.vtype8  = vtype & 0xff;
    inst.vill    = vtype.vill;
    inst.rv_type = static_cast<int>(next_pc.rvType());
    inst.has_zcd = _hasZcd;
    emi = inst;

    return decode(inst, next_pc.instAddr());
}

} // namespace RiscvISA
} // namespace gem5
