/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/cpu_state.hh"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "arch/riscv/isa.hh"
#include "arch/riscv/regs/float.hh"
#include "arch/riscv/regs/int.hh"
#include "arch/riscv/regs/misc.hh"
#include "cpu/thread_context.hh"

namespace gem5::rtl_cosim
{
namespace
{

void
addValue(std::vector<OwnedCpuStateValue> &values, std::string name,
         std::size_t width, std::uint64_t value)
{
    OwnedCpuStateValue result{std::move(name), width,
                              std::vector<std::uint8_t>((width + 7) / 8)};
    for (std::size_t index = 0; index < result.data.size(); ++index) {
        result.data[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
    if (width % 8 != 0) {
        result.data.back() &= static_cast<std::uint8_t>((1U << (width % 8)) - 1);
    }
    values.push_back(std::move(result));
}

bool
encodeRiscv64(ThreadContext &context,
              std::vector<OwnedCpuStateValue> &values, std::string &error)
{
    auto *isa = dynamic_cast<RiscvISA::ISA *>(context.getIsaPtr());
    if (!isa) {
        error = "riscv64/v1 requires a RISC-V ThreadContext";
        return false;
    }
    if (isa->getEnableRvv()) {
        error = "riscv64/v1 does not transfer vector architectural state; "
                "disable RVV for this handover";
        return false;
    }

    addValue(values, "pc", 64, context.pcState().instAddr());
    for (std::size_t index = 0; index < 32; ++index) {
        addValue(values, "x" + std::to_string(index), 64,
                 context.getReg(RiscvISA::intRegClass[index]));
    }
    addValue(values, "priv", 2,
             context.readMiscRegNoEffect(RiscvISA::MISCREG_PRV));

    using Csr = std::pair<const char *, RiscvISA::MiscRegIndex>;
    static constexpr std::array csrs = {
        Csr{"csr.mstatus", RiscvISA::MISCREG_STATUS},
        Csr{"csr.medeleg", RiscvISA::MISCREG_MEDELEG},
        Csr{"csr.mideleg", RiscvISA::MISCREG_MIDELEG},
        Csr{"csr.mie", RiscvISA::MISCREG_IE},
        Csr{"csr.mtvec", RiscvISA::MISCREG_MTVEC},
        Csr{"csr.mcounteren", RiscvISA::MISCREG_MCOUNTEREN},
        Csr{"csr.mscratch", RiscvISA::MISCREG_MSCRATCH},
        Csr{"csr.mepc", RiscvISA::MISCREG_MEPC},
        Csr{"csr.mcause", RiscvISA::MISCREG_MCAUSE},
        Csr{"csr.mtval", RiscvISA::MISCREG_MTVAL},
        Csr{"csr.mip", RiscvISA::MISCREG_IP},
        Csr{"csr.stvec", RiscvISA::MISCREG_STVEC},
        Csr{"csr.scounteren", RiscvISA::MISCREG_SCOUNTEREN},
        Csr{"csr.sscratch", RiscvISA::MISCREG_SSCRATCH},
        Csr{"csr.sepc", RiscvISA::MISCREG_SEPC},
        Csr{"csr.scause", RiscvISA::MISCREG_SCAUSE},
        Csr{"csr.stval", RiscvISA::MISCREG_STVAL},
        Csr{"csr.satp", RiscvISA::MISCREG_SATP},
        Csr{"csr.pmpcfg0", RiscvISA::MISCREG_PMPCFG0},
        Csr{"csr.pmpcfg2", RiscvISA::MISCREG_PMPCFG2},
    };
    for (const auto &[name, index] : csrs) {
        // MIE and MIP are owned by gem5's RISC-V interrupt controller, not
        // the ISA misc-register array. Use architectural reads for every CSR
        // so state stored outside that array is included in the handover.
        addValue(values, name, 64, context.readMiscReg(index));
    }
    for (std::size_t index = 0; index < 16; ++index) {
        const auto reg = static_cast<RiscvISA::MiscRegIndex>(
            RiscvISA::MISCREG_PMPADDR00 + index);
        addValue(values, "csr.pmpaddr" + std::to_string(index), 64,
                 context.readMiscRegNoEffect(reg));
    }

    for (std::size_t index = 0; index < 32; ++index) {
        addValue(values, "f" + std::to_string(index), 64,
                 context.getReg(RiscvISA::floatRegClass[index]));
    }
    const std::uint64_t fcsr =
        (context.readMiscRegNoEffect(RiscvISA::MISCREG_FFLAGS) & 0x1f) |
        ((context.readMiscRegNoEffect(RiscvISA::MISCREG_FRM) & 0x7) << 5);
    addValue(values, "csr.fcsr", 8, fcsr);

    error.clear();
    return true;
}

[[maybe_unused]] const bool registered = registerCpuStateEncoder(
    RtlCpuStateSchema::Riscv64V1, encodeRiscv64);

} // anonymous namespace
} // namespace gem5::rtl_cosim
