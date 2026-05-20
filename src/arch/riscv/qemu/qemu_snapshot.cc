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

#include "arch/riscv/qemu/qemu_snapshot.hh"

#include <fstream>
#include <set>
#include <vector>

#include "arch/riscv/isa.hh"
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
static constexpr Addr CLINT_MTIMECMP0 = 0x4000;
static constexpr Addr CLINT_MTIME = 0xBFF8;

QemuSnapshot::QemuSnapshot(const Params &p)
    : Workload(p),
      ramFile(p.ram_file),
      ramAddr(p.ram_addr),
      regsFile(p.regs_file),
      clintAddr(p.clint_addr),
      clintMtime(p.clint_mtime),
      clintMtimecmp(p.clint_mtimecmp),
      verbose(p.verbose)
{
    parseRegs();
}

void
QemuSnapshot::parseRegs()
{
    std::ifstream f(regsFile);
    fatal_if(!f.is_open(),
             "QemuSnapshot: cannot open register dump '%s'", regsFile);

    std::string name, val;
    while (f >> name >> val) {
        try {
            regs[name] = std::stoull(val, nullptr, 0);
        } catch (const std::exception &) {
            warn("QemuSnapshot: ignoring malformed register line '%s %s'",
                 name, val);
        }
    }
    auto it = regs.find("pc");
    fatal_if(it == regs.end(),
             "QemuSnapshot: register dump '%s' has no 'pc'", regsFile);
    entryPc = it->second;
    inform("QemuSnapshot: parsed %d registers from %s (pc=%#x)",
           regs.size(), regsFile, entryPc);
}

void
QemuSnapshot::loadRam()
{
    std::ifstream f(ramFile, std::ios::binary);
    fatal_if(!f.is_open(),
             "QemuSnapshot: cannot open RAM image '%s'", ramFile);

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
    uint64_t mtime = clintMtime;
    uint64_t mtimecmp = clintMtimecmp;
    system->physProxy.writeBlob(clintAddr + CLINT_MTIME, &mtime, 8);
    system->physProxy.writeBlob(clintAddr + CLINT_MTIMECMP0, &mtimecmp, 8);
    inform("QemuSnapshot: CLINT @%#x mtime=%#x mtimecmp=%#x",
           clintAddr, mtime, mtimecmp);
}

void
QemuSnapshot::applyRegisters(ThreadContext *tc)
{
    // Map every CSR name gem5 knows about to its internal MiscReg index,
    // straight out of gem5's own CSR table - so the QEMU CSR names line up.
    std::unordered_map<std::string, int> csrByName;
    for (const auto &kv : CSRData)
        csrByName[kv.second.name] = kv.second.physIndex;

    // sstatus/sie/sip are restricted views of mstatus/mie/mip, which we
    // restore in full - skip the aliases so they do not clobber them.
    static const std::set<std::string> aliases = {"sstatus", "sie", "sip"};

    auto isPmp = [](const std::string &n) { return n.rfind("pmp", 0) == 0; };

    // resetThread() left the hart in M-mode.  PMP config/addr writes are
    // only legal from M-mode and must take effect (so the MMU's PMP table
    // is rebuilt), so do them first, before we drop the privilege level.
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
        auto it = regs.find(name);
        if (it == regs.end() && name == "s0")
            it = regs.find("fp");
        if (it != regs.end())
            tc->setReg(intRegClass[i], (RegVal)it->second);
        else
            warn("QemuSnapshot: register '%s' (x%d) not in dump", name, i);
    }

    // Program counter.
    tc->pcState(entryPc);

    // Remaining CSRs - restored verbatim, no side effects.
    int restored = 0;
    for (const auto &kv : regs) {
        const std::string &name = kv.first;
        if (name == "pc" || name == "priv" || aliases.count(name) ||
            isPmp(name))
            continue;
        auto it = csrByName.find(name);
        if (it == csrByName.end())
            continue; // GPR or a CSR gem5 does not model - skip
        tc->setMiscRegNoEffect(it->second, kv.second);
        restored++;
    }

    // Current privilege mode (0=U, 1=S, 3=M) - set last, after the
    // M-mode-only PMP writes above.
    auto prv = regs.find("priv");
    if (prv != regs.end())
        tc->setMiscRegNoEffect(MISCREG_PRV, prv->second);

    inform("QemuSnapshot: restored 31 GPRs, pc=%#x, %d CSRs onto hart %d",
           entryPc, restored, tc->contextId());
}

void
QemuSnapshot::initState()
{
    Workload::initState();

    fatal_if(system->threads.empty(),
             "QemuSnapshot: system has no thread contexts");

    loadRam();
    restoreClint();

    for (auto *tc : system->threads) {
        tc->getIsaPtr()->resetThread();
        applyRegisters(tc);
        tc->activate();
    }
    inform("QemuSnapshot: snapshot restored - handing control to gem5");
}

} // namespace RiscvISA
} // namespace gem5
