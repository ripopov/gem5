/*
 * Copyright (c) 2026 The gem5 Authors
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

#ifndef __ARCH_RISCV_QEMU_QEMU_SNAPSHOT_HH__
#define __ARCH_RISCV_QEMU_QEMU_SNAPSHOT_HH__

#include <string>
#include <unordered_map>

#include "base/loader/object_file.hh"
#include "base/loader/symtab.hh"
#include "params/RiscvQemuSnapshotWorkload.hh"
#include "sim/workload.hh"

namespace gem5
{

class ThreadContext;

namespace RiscvISA
{

/**
 * QEMU-CPU mode: a full-system workload that restores a machine snapshot
 * captured under QEMU (see util/qemu-cpu/) instead of booting a kernel.
 *
 * At initState() it
 *   1. copies the captured guest RAM image into gem5 physical memory,
 *   2. restores every GPR / PC / CSR of the boot hart from the register
 *      dump produced by the QEMU gdbstub, and
 *   3. seeds the CLINT mtime / mtimecmp so the guest's timer keeps ticking.
 *
 * gem5's O3 or TimingSimpleCPU then continues execution exactly where
 * QEMU left off - which lets a slow Linux boot happen under fast QEMU
 * emulation while the benchmark of interest runs under gem5's detailed
 * timing models.
 */
class QemuSnapshot : public Workload
{
  protected:
    const std::string ramFile;
    const Addr ramAddr;
    const std::string regsFile;
    const Addr clintAddr;
    const uint64_t clintMtime;
    const uint64_t clintMtimecmp;
    const bool verbose;

    /** Parsed register dump (lower-case name -> 64-bit value). */
    std::unordered_map<std::string, uint64_t> regs;
    /** Restored program counter of the boot hart. */
    Addr entryPc = 0;

    loader::SymbolTable kernelSymtab;

    /** Read regs.txt into the `regs` map; called from the constructor. */
    void parseRegs();
    /** Copy the guest RAM image into physical memory. */
    void loadRam();
    /** Restore GPRs / PC / CSRs / privilege onto one hart. */
    void applyRegisters(ThreadContext *tc);
    /** Seed CLINT mtime / mtimecmp through its MMIO interface. */
    void restoreClint();

  public:
    PARAMS(RiscvQemuSnapshotWorkload);
    QemuSnapshot(const Params &p);

    void initState() override;

    loader::Arch getArch() const override { return loader::Riscv64; }
    ByteOrder byteOrder() const override { return ByteOrder::little; }
    Addr getEntry() const override { return entryPc; }

    const loader::SymbolTable &
    symtab(ThreadContext *tc) override
    {
        return kernelSymtab;
    }

    bool
    insertSymbol(const loader::Symbol &symbol) override
    {
        return kernelSymtab.insert(symbol);
    }
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_QEMU_QEMU_SNAPSHOT_HH__
