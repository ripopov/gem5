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

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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
 *   1. copies the captured guest RAM image into gem5 physical memory;
 *   2. restores device state through MMIO -- CLINT (mtime/mtimecmp/msip),
 *      PLIC (priority/enable/threshold) and the 8250 UART (IER/LCR/MCR) --
 *      so timers and interrupt-driven I/O keep working after restore;
 *   3. restores every GPR, FP register and CSR of every hart from the
 *      per-hart gdbstub register dumps.
 *
 * gem5's O3 / Timing / Atomic CPUs then continue execution exactly where
 * QEMU left off.
 */
class QemuSnapshot : public Workload
{
  protected:
    const std::string ramFile;
    const Addr ramAddr;

    const Addr clintAddr;
    const std::string clintFile;
    const uint64_t clintTimerGap;

    const Addr plicAddr;
    const std::string plicFile;
    const unsigned plicNumSrc;
    const unsigned plicNumContexts;

    const Addr uartAddr;
    const std::string uartFile;

    const bool verbose;

    /** Per-hart parsed register dumps (lower-case name -> 64-bit value). */
    std::vector<std::unordered_map<std::string, uint64_t>> hartRegs;
    /** Restored program counter of hart 0. */
    Addr entryPc = 0;

    loader::SymbolTable kernelSymtab;

    /** Read a whole binary file into a byte vector. */
    static std::vector<uint8_t> readFile(const std::string &path);
    /** Parse a `name 0xvalue` register dump. */
    static std::unordered_map<std::string, uint64_t>
        parseRegs(const std::string &path);

    void loadRam();
    void restoreClint();
    void restorePlic();
    void restoreUart();
    void applyRegisters(ThreadContext *tc,
            const std::unordered_map<std::string, uint64_t> &regs);

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
