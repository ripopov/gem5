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

#include "arch/riscv/qemu_snap/qemu_snapshot.hh"

#include <cstring>
#include <fstream>
#include <set>

#include "arch/riscv/isa.hh"
#include "arch/riscv/regs/float.hh"
#include "arch/riscv/regs/int.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/logging.hh"
#include "cpu/thread_context.hh"
#include "mem/port_proxy.hh"
#include "sim/system.hh"

namespace gem5
{

namespace RiscvISA
{

// CLINT MMIO register offsets (SiFive / QEMU ACLINT layout).
static constexpr Addr CLINT_MSIP0 = 0x0000;
static constexpr Addr CLINT_MTIMECMP0 = 0x4000;
static constexpr Addr CLINT_MTIME = 0xBFF8;

// PLIC MMIO register offsets (standard SiFive PLIC layout).
static constexpr Addr PLIC_PRIORITY = 0x000000;
static constexpr Addr PLIC_ENABLE = 0x002000;
static constexpr Addr PLIC_ENABLE_STRIDE = 0x80;
static constexpr Addr PLIC_THRESHOLD = 0x200000;
static constexpr Addr PLIC_THRESHOLD_STRIDE = 0x1000;

// 8250 UART register offsets (reg-shift 0).
static constexpr Addr UART_IER = 1;
static constexpr Addr UART_LCR = 3;
static constexpr Addr UART_MCR = 4;

QemuSnapshot::QemuSnapshot(const Params &p)
    : Workload(p),
      ramFile(p.ram_file),
      ramAddr(p.ram_addr),
      clintAddr(p.clint_addr),
      clintFile(p.clint_file),
      clintTimerGap(p.clint_timer_gap),
      plicAddr(p.plic_addr),
      plicFile(p.plic_file),
      plicNumSrc(p.plic_num_src),
      plicNumContexts(p.plic_num_contexts),
      uartAddr(p.uart_addr),
      uartFile(p.uart_file),
      verbose(p.verbose)
{
    fatal_if(p.regs_files.empty(),
             "QemuSnapshot: no per-hart register dumps given");
    for (const auto &f : p.regs_files)
        hartRegs.push_back(parseRegs(f));

    auto pc = hartRegs[0].find("pc");
    fatal_if(pc == hartRegs[0].end(),
             "QemuSnapshot: hart 0 register dump has no 'pc'");
    entryPc = pc->second;
    inform("QemuSnapshot: %d hart(s), hart0 pc=%#x", hartRegs.size(), entryPc);
}

std::vector<uint8_t>
QemuSnapshot::readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    fatal_if(!f.is_open(), "QemuSnapshot: cannot open '%s'", path);
    std::streamsize n = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(n > 0 ? n : 0);
    if (n > 0)
        f.read(reinterpret_cast<char *>(buf.data()), n);
    return buf;
}

std::unordered_map<std::string, uint64_t>
QemuSnapshot::parseRegs(const std::string &path)
{
    std::ifstream f(path);
    fatal_if(!f.is_open(),
             "QemuSnapshot: cannot open register dump '%s'", path);
    std::unordered_map<std::string, uint64_t> regs;
    std::string name, val;
    while (f >> name >> val) {
        try {
            regs[name] = std::stoull(val, nullptr, 0);
        } catch (const std::exception &) {
            warn("QemuSnapshot: ignoring malformed line '%s %s'", name, val);
        }
    }
    return regs;
}

void
QemuSnapshot::loadRam()
{
    std::ifstream f(ramFile, std::ios::binary);
    fatal_if(!f.is_open(), "QemuSnapshot: cannot open RAM image '%s'",
             ramFile);
    const size_t chunk = 1 << 20; // 1 MiB
    std::vector<uint8_t> buf(chunk);
    Addr off = 0;
    while (f) {
        f.read(reinterpret_cast<char *>(buf.data()), chunk);
        std::streamsize got = f.gcount();
        if (got <= 0)
            break;
        system->physProxy.writeBlob(ramAddr + off, buf.data(), got);
        off += got;
    }
    inform("QemuSnapshot: loaded %#x bytes of guest RAM at %#x",
           (uint64_t)off, ramAddr);
}

void
QemuSnapshot::restoreClint()
{
    auto d = readFile(clintFile);
    auto rd64 = [&](Addr o) {
        uint64_t v = 0;
        if (o + 8 <= d.size())
            std::memcpy(&v, &d[o], 8);
        return v;
    };
    auto rd32 = [&](Addr o) {
        uint32_t v = 0;
        if (o + 4 <= d.size())
            std::memcpy(&v, &d[o], 4);
        return v;
    };

    uint64_t mtime = rd64(CLINT_MTIME);
    system->physProxy.writeBlob(clintAddr + CLINT_MTIME, &mtime, 8);

    for (unsigned h = 0; h < hartRegs.size(); h++) {
        uint32_t msip = rd32(CLINT_MSIP0 + 4 * h);
        uint64_t mtimecmp = rd64(CLINT_MTIMECMP0 + 8 * h);
        // Optionally pull a far-future timer interrupt closer so gem5 need
        // not fast-forward through millions of idle RTC ticks.
        if (clintTimerGap && mtimecmp > mtime + clintTimerGap)
            mtimecmp = mtime + clintTimerGap;
        system->physProxy.writeBlob(clintAddr + CLINT_MSIP0 + 4 * h,
                                    &msip, 4);
        system->physProxy.writeBlob(clintAddr + CLINT_MTIMECMP0 + 8 * h,
                                    &mtimecmp, 8);
    }
    inform("QemuSnapshot: CLINT @%#x mtime=%#x restored for %d hart(s)",
           clintAddr, mtime, hartRegs.size());
}

void
QemuSnapshot::restorePlic()
{
    auto d = readFile(plicFile);
    auto wr = [&](Addr off) {
        if (off + 4 <= d.size())
            system->physProxy.writeBlob(plicAddr + off, &d[off], 4);
    };

    // Per-source interrupt priority.
    for (unsigned s = 0; s < plicNumSrc; s++)
        wr(PLIC_PRIORITY + 4 * s);

    // Per-context interrupt-enable bitmaps.
    unsigned nSrc32 = (plicNumSrc + 31) / 32;
    for (unsigned c = 0; c < plicNumContexts; c++)
        for (unsigned w = 0; w < nSrc32; w++)
            wr(PLIC_ENABLE + c * PLIC_ENABLE_STRIDE + 4 * w);

    // Per-context priority threshold (the +0 word; +4 is claim - skip it).
    for (unsigned c = 0; c < plicNumContexts; c++)
        wr(PLIC_THRESHOLD + c * PLIC_THRESHOLD_STRIDE);

    inform("QemuSnapshot: PLIC @%#x restored (%d sources, %d contexts)",
           plicAddr, plicNumSrc, plicNumContexts);
}

void
QemuSnapshot::restoreUart()
{
    auto d = readFile(uartFile);
    if (d.size() < 8) {
        warn("QemuSnapshot: UART dump too small, skipping");
        return;
    }
    // Restore with DLAB clear so offset 1 addresses IER (not the divisor
    // latch).  Force the receive-data interrupt enable on: the console tty
    // is open for input across the snapshot, so a restored guest must get
    // an interrupt when a character arrives (interactive input).
    uint8_t lcr = d[UART_LCR] & 0x7f;
    uint8_t ier = d[UART_IER] | 0x01;
    uint8_t mcr = d[UART_MCR];
    system->physProxy.writeBlob(uartAddr + UART_LCR, &lcr, 1);
    system->physProxy.writeBlob(uartAddr + UART_IER, &ier, 1);
    system->physProxy.writeBlob(uartAddr + UART_MCR, &mcr, 1);
    inform("QemuSnapshot: UART @%#x restored (IER=%#x LCR=%#x MCR=%#x)",
           uartAddr, ier, lcr, mcr);
}

void
QemuSnapshot::applyRegisters(ThreadContext *tc,
        const std::unordered_map<std::string, uint64_t> &regs)
{
    // Map every CSR name gem5 knows to its internal MiscReg index, straight
    // out of gem5's own CSR table - so the QEMU CSR names line up.
    std::unordered_map<std::string, int> csrByName;
    for (const auto &kv : CSRData)
        csrByName[kv.second.name] = kv.second.physIndex;

    // sstatus/sie/sip are restricted views of mstatus/mie/mip (restored in
    // full) - skip the aliases so they do not clobber them.
    static const std::set<std::string> aliases = {"sstatus", "sie", "sip"};
    // These CSRs must be written *with side effects* so the value also
    // propagates into gem5's interrupt controller (the cached ie/ip
    // bitsets and the delegation state) - otherwise a restored guest never
    // takes interrupts.
    static const std::set<std::string> withEffect = {
        "mie", "mip", "mideleg", "medeleg"};
    auto isPmp = [](const std::string &n) { return n.rfind("pmp", 0) == 0; };
    auto get = [&](const std::string &n) -> const uint64_t * {
        auto it = regs.find(n);
        return it == regs.end() ? nullptr : &it->second;
    };

    // resetThread() left the hart in M-mode; PMP CSR writes are only legal
    // from M-mode and must take effect (rebuilding the MMU's PMP table), so
    // do them first, before privilege is dropped.
    for (const auto &kv : regs) {
        if (!isPmp(kv.first))
            continue;
        auto it = csrByName.find(kv.first);
        if (it != csrByName.end())
            tc->setMiscReg(it->second, kv.second);
    }

    // Integer registers x1..x31 (x0 is hard-wired zero).  gem5 calls x8
    // "s0"; QEMU's gdbstub dumps it under its other ABI name, "fp".
    for (size_t i = 1; i < int_reg::NumArchRegs &&
                       i < int_reg::RegNames.size(); i++) {
        const std::string &name = int_reg::RegNames[i];
        const uint64_t *v = get(name);
        if (!v && name == "s0")
            v = get("fp");
        if (v)
            tc->setReg(intRegClass[i], (RegVal)*v);
    }

    // Floating-point registers f0..f31 (gdb's ABI names match gem5's).
    int fpRestored = 0;
    for (size_t i = 0; i < float_reg::NumRegs &&
                       i < float_reg::RegNames.size(); i++) {
        const uint64_t *v = get(float_reg::RegNames[i]);
        if (v) {
            tc->setReg(floatRegClass[i], (RegVal)*v);
            fpRestored++;
        }
    }

    // Program counter.
    tc->pcState(get("pc") ? *get("pc") : entryPc);

    // Remaining CSRs - restored verbatim, no side effects.
    int csrRestored = 0;
    for (const auto &kv : regs) {
        const std::string &name = kv.first;
        if (name == "pc" || name == "priv" || aliases.count(name) ||
            isPmp(name))
            continue;
        auto it = csrByName.find(name);
        if (it == csrByName.end())
            continue; // GPR / FP reg / a CSR gem5 does not model
        if (withEffect.count(name))
            tc->setMiscReg(it->second, kv.second);
        else
            tc->setMiscRegNoEffect(it->second, kv.second);
        csrRestored++;
    }

    // Current privilege mode - set last, after the M-mode-only PMP writes.
    if (const uint64_t *prv = get("priv"))
        tc->setMiscRegNoEffect(MISCREG_PRV, *prv);

    inform("QemuSnapshot: hart %d restored - pc=%#x, 31 GPRs, %d FP regs, "
           "%d CSRs", tc->contextId(),
           get("pc") ? *get("pc") : entryPc, fpRestored, csrRestored);
}

void
QemuSnapshot::initState()
{
    Workload::initState();

    fatal_if(system->threads.size() < hartRegs.size(),
             "QemuSnapshot: snapshot has %d harts but the system has only "
             "%d thread context(s)", hartRegs.size(),
             system->threads.size());

    loadRam();
    restoreClint();
    restorePlic();
    restoreUart();

    for (unsigned h = 0; h < hartRegs.size(); h++) {
        ThreadContext *tc = system->threads[h];
        tc->getIsaPtr()->resetThread();
        applyRegisters(tc, hartRegs[h]);
        tc->activate();
    }
    inform("QemuSnapshot: snapshot restored - handing control to gem5");
}

} // namespace RiscvISA
} // namespace gem5
