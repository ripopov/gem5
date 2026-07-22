/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "cpu_state.hh"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

#include "Vrtl_cosim_c910_top.h"
#include "verilated.h"

namespace
{

constexpr std::uint16_t HadEventOutEnable = 0b00000100010;
constexpr std::uint16_t HadHsr = 0b00011100000;
constexpr std::uint16_t HadWbbr = 0b00100010000;
constexpr std::uint16_t HadPc = 0b00100110000;
constexpr std::uint16_t HadPcGoExit = 0b11100110000;
constexpr std::uint16_t HadInstructionGo = 0b10101000000;
constexpr std::uint16_t HadCsr = 0b00101010000;

constexpr std::uint32_t Mret = 0x30200073;
constexpr std::uint64_t MstatusMie = 1ULL << 3;
constexpr std::uint64_t MstatusMpie = 1ULL << 7;
constexpr std::uint64_t MstatusMppMask = 3ULL << 11;
constexpr std::uint32_t HsrBusDead = 1U << 15;
constexpr std::uint32_t HsrExecutionDead = 1U << 13;

struct ImportedState
{
    std::uint64_t pc = 0;
    std::array<std::uint64_t, 32> x{};
    std::uint8_t privilege = 0;
    std::unordered_map<std::string, std::uint64_t> csr;
    std::array<std::uint64_t, 32> f{};
};

class AxiInputGuard
{
  public:
    explicit AxiInputGuard(Vrtl_cosim_c910_top &top)
        : _top(top),
          _awReady(top.axi_aw_ready_i),
          _wReady(top.axi_w_ready_i),
          _bId(top.axi_b_id_i),
          _bResp(top.axi_b_resp_i),
          _bUser(top.axi_b_user_i),
          _bValid(top.axi_b_valid_i),
          _arReady(top.axi_ar_ready_i),
          _rId(top.axi_r_id_i),
          _rResp(top.axi_r_resp_i),
          _rLast(top.axi_r_last_i),
          _rUser(top.axi_r_user_i),
          _rValid(top.axi_r_valid_i)
    {
        for (unsigned index = 0; index < _rData.size(); ++index) {
            _rData[index] = top.axi_r_data_i[index];
            top.axi_r_data_i[index] = 0;
        }
        top.axi_aw_ready_i = 0;
        top.axi_w_ready_i = 0;
        top.axi_b_id_i = 0;
        top.axi_b_resp_i = 0;
        top.axi_b_user_i = 0;
        top.axi_b_valid_i = 0;
        top.axi_ar_ready_i = 0;
        top.axi_r_id_i = 0;
        top.axi_r_resp_i = 0;
        top.axi_r_last_i = 0;
        top.axi_r_user_i = 0;
        top.axi_r_valid_i = 0;
    }

    ~AxiInputGuard()
    {
        _top.axi_aw_ready_i = _awReady;
        _top.axi_w_ready_i = _wReady;
        _top.axi_b_id_i = _bId;
        _top.axi_b_resp_i = _bResp;
        _top.axi_b_user_i = _bUser;
        _top.axi_b_valid_i = _bValid;
        _top.axi_ar_ready_i = _arReady;
        _top.axi_r_id_i = _rId;
        for (unsigned index = 0; index < _rData.size(); ++index) {
            _top.axi_r_data_i[index] = _rData[index];
        }
        _top.axi_r_resp_i = _rResp;
        _top.axi_r_last_i = _rLast;
        _top.axi_r_user_i = _rUser;
        _top.axi_r_valid_i = _rValid;
    }

  private:
    Vrtl_cosim_c910_top &_top;
    std::uint8_t _awReady;
    std::uint8_t _wReady;
    std::uint8_t _bId;
    std::uint8_t _bResp;
    std::uint8_t _bUser;
    std::uint8_t _bValid;
    std::uint8_t _arReady;
    std::uint8_t _rId;
    std::array<std::uint32_t, 4> _rData{};
    std::uint8_t _rResp;
    std::uint8_t _rLast;
    std::uint8_t _rUser;
    std::uint8_t _rValid;
};

class ImportMutationGuard
{
  public:
    ImportMutationGuard(Vrtl_cosim_c910_top &top, VerilatedContext &context)
        : _top(top), _context(context)
    {}

    ~ImportMutationGuard()
    {
        if (_committed) {
            return;
        }
        // The only state visible before import is the freshly reset core.
        // Restore that state if any HAD operation fails after validation, so
        // callers never observe a partially imported architectural context.
        try {
            Verilated::threadContextp(&_context);
            _top.debug_req_i = 0;
            _top.rst_ni = 0;
            _top.jtag_trst_ni = 0;
            for (unsigned cycle = 0; cycle < 8; ++cycle) {
                _top.clk_i = 0;
                _top.eval();
                _top.clk_i = 1;
                _top.eval();
                _top.clk_i = 0;
                _top.eval();
                _context.timeInc(1);
            }
            _top.jtag_trst_ni = 1;
            _top.rst_ni = 1;
            _top.eval();
        } catch (...) {
            // The ABI is noexcept. The original import diagnostic remains
            // authoritative if the best-effort reset itself cannot evaluate.
        }
    }

    void commit() noexcept { _committed = true; }

  private:
    Vrtl_cosim_c910_top &_top;
    VerilatedContext &_context;
    bool _committed = false;
};

struct CsrDefinition
{
    const char *name;
    std::uint16_t address;
};

constexpr std::array CsrDefinitions = {
    CsrDefinition{"mstatus", 0x300},
    CsrDefinition{"medeleg", 0x302},
    CsrDefinition{"mideleg", 0x303},
    CsrDefinition{"mie", 0x304},
    CsrDefinition{"mtvec", 0x305},
    CsrDefinition{"mcounteren", 0x306},
    CsrDefinition{"mscratch", 0x340},
    CsrDefinition{"mepc", 0x341},
    CsrDefinition{"mcause", 0x342},
    CsrDefinition{"mtval", 0x343},
    CsrDefinition{"mip", 0x344},
    CsrDefinition{"stvec", 0x105},
    CsrDefinition{"scounteren", 0x106},
    CsrDefinition{"sscratch", 0x140},
    CsrDefinition{"sepc", 0x141},
    CsrDefinition{"scause", 0x142},
    CsrDefinition{"stval", 0x143},
    CsrDefinition{"satp", 0x180},
    CsrDefinition{"pmpcfg0", 0x3a0},
    CsrDefinition{"pmpcfg2", 0x3a2},
};

std::uint64_t
decodeValue(const CpuStateValue &value)
{
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < value.dataSize; ++index) {
        result |= static_cast<std::uint64_t>(value.data[index]) << (8 * index);
    }
    return result;
}

bool
validateValue(const CpuStateValue &value, std::size_t width,
              std::string &error)
{
    if (value.bitWidth != width || value.dataSize != (width + 7) / 8 ||
        !value.data) {
        error = "field '" + std::string(value.name ? value.name : "") +
                "' has an invalid width or data buffer";
        return false;
    }
    if (width % 8 != 0 &&
        value.data[value.dataSize - 1] >> (width % 8) != 0) {
        error = "field '" + std::string(value.name) +
                "' has nonzero unused high bits";
        return false;
    }
    return true;
}

bool
parseState(const CpuStateValue *values, std::size_t valueCount,
           ImportedState &state, std::string &error)
{
    if (!values && valueCount != 0) {
        error = "state array is null";
        return false;
    }
    std::unordered_map<std::string_view, const CpuStateValue *> fields;
    for (std::size_t index = 0; index < valueCount; ++index) {
        const CpuStateValue &value = values[index];
        if (!value.name || !*value.name) {
            error = "state contains an empty field name";
            return false;
        }
        if (!fields.emplace(value.name, &value).second) {
            error = "state contains duplicate field '" +
                    std::string(value.name) + "'";
            return false;
        }
    }

    auto take = [&](const std::string &name, std::size_t width,
                    std::uint64_t &destination) {
        const auto found = fields.find(name);
        if (found == fields.end()) {
            error = "state is missing field '" + name + "'";
            return false;
        }
        if (!validateValue(*found->second, width, error)) {
            return false;
        }
        destination = decodeValue(*found->second);
        fields.erase(found);
        return true;
    };

    if (!take("pc", 64, state.pc)) {
        return false;
    }
    for (std::size_t index = 0; index < state.x.size(); ++index) {
        if (!take("x" + std::to_string(index), 64, state.x[index])) {
            return false;
        }
    }
    std::uint64_t privilege = 0;
    if (!take("priv", 2, privilege) || privilege > 3 || privilege == 2) {
        if (error.empty()) {
            error = "field 'priv' is not a supported RISC-V privilege mode";
        }
        return false;
    }
    state.privilege = static_cast<std::uint8_t>(privilege);

    for (const CsrDefinition &definition : CsrDefinitions) {
        std::uint64_t value = 0;
        if (!take("csr." + std::string(definition.name), 64, value)) {
            return false;
        }
        state.csr.emplace(definition.name, value);
    }
    for (std::size_t index = 0; index < 16; ++index) {
        std::uint64_t value = 0;
        const std::string name = "pmpaddr" + std::to_string(index);
        if (!take("csr." + name, 64, value)) {
            return false;
        }
        state.csr.emplace(name, value);
    }
    for (std::size_t index = 0; index < state.f.size(); ++index) {
        if (!take("f" + std::to_string(index), 64, state.f[index])) {
            return false;
        }
    }
    std::uint64_t fcsr = 0;
    if (!take("csr.fcsr", 8, fcsr)) {
        return false;
    }
    state.csr.emplace("fcsr", fcsr);

    if (!fields.empty()) {
        error = "state contains unknown field '" +
                std::string(fields.begin()->first) + "'";
        return false;
    }
    if (state.x[0] != 0) {
        error = "field 'x0' must be zero";
        return false;
    }
    if ((state.pc & 1) != 0) {
        error = "field 'pc' must be two-byte aligned";
        return false;
    }
    const std::uint64_t mstatus = state.csr.at("mstatus");
    if (state.privilege != 3 &&
        ((mstatus & MstatusMppMask) != 0 || !(mstatus & MstatusMpie))) {
        error = "riscv64/v1 mstatus is not a post-MRET state for privilege "
                "mode " + std::to_string(state.privilege);
        return false;
    }
    return true;
}

class HadJtag
{
  public:
    HadJtag(Vrtl_cosim_c910_top &top, VerilatedContext &context)
        : _top(top), _context(context)
    {}

    void
    reset()
    {
        _top.jtag_trst_ni = 0;
        for (unsigned cycle = 0; cycle < 16; ++cycle) {
            jtagCycle(false, false);
        }
        _top.jtag_trst_ni = 1;
        jtagCycle(false, false);
        jtagCycle(false, false);
    }

    void
    writeRegister(std::uint16_t selection, std::uint64_t value,
                  unsigned width)
    {
        writeInstruction(static_cast<std::uint16_t>(selection << 4));
        writeData(value, width);
    }

    std::uint64_t
    readRegister(std::uint16_t selection, unsigned width)
    {
        writeInstruction(static_cast<std::uint16_t>(0x8000 | selection << 4));
        return readData(width);
    }

    bool
    enterDebug()
    {
        // Assert before the first post-reset CPU edge so the IFU cannot issue
        // a fetch which an out-of-band importer would then have to service.
        _top.debug_req_i = 1;
        reset();
        for (unsigned attempt = 0; attempt < 8192; ++attempt) {
            mainClock();
            if (_top.cosim_debug_mode_o) {
                _top.debug_req_i = 0;
                _serveBootstrapReads = false;
                _top.axi_ar_ready_i = 0;
                _top.axi_r_valid_i = 0;
                writeRegister(HadEventOutEnable, 0, 32);
                return true;
            }
        }
        _top.debug_req_i = 0;
        return false;
    }

    bool
    execute(std::uint32_t instruction, bool allowNoWriteback = false,
            bool allowPostExecutionBusActivity = false)
    {
        writeRegister(HadInstructionGo, instruction, 32);
        // HSR.PS is one only after the injected instruction has retired and
        // the pipeline, LSU, and IFU are quiescent again. Polling it avoids a
        // core-specific fixed delay and catches instructions which trap or
        // otherwise fail to complete under HAD control.
        for (unsigned attempt = 0; attempt < 256; ++attempt) {
            const std::uint32_t status = readRegister(HadHsr, 16);
            _lastStatus = status;
            if (status & (1U << 12)) {
                const bool executionDead = status & (1U << 13);
                const bool didNotWriteBack = status & (1U << 17);
                return !executionDead &&
                       (allowNoWriteback || !didNotWriteBack);
            }
        }
        if (allowPostExecutionBusActivity &&
            (_lastStatus & HsrBusDead) &&
            !(_lastStatus & HsrExecutionDead)) {
            return true;
        }
        return false;
    }

    std::uint32_t
    lastStatus() const noexcept
    {
        return _lastStatus;
    }

    bool
    writeGpr(unsigned index, std::uint64_t value)
    {
        if (index == 0) {
            return value == 0;
        }
        writeRegister(HadCsr, 0x100, 16);
        writeRegister(HadWbbr, value, 64);
        const std::uint32_t move = 0x8002U | (index << 7) | (index << 2);
        const bool completed = execute(move);
        writeRegister(HadCsr, 0, 16);
        return completed;
    }

    bool
    writeCsr(std::uint16_t address, std::uint64_t value,
             bool allowPostExecutionBusActivity = false)
    {
        // HAD's CSR source bypass maps architectural x0 to WBBR. This is the
        // sequence used by the C910 reference debug driver and avoids both a
        // scratch GPR dependency and an architectural register side effect.
        writeRegister(HadCsr, 0x100, 16);
        writeRegister(HadWbbr, value, 64);
        const std::uint32_t instruction =
            (static_cast<std::uint32_t>(address) << 20) |
            (1U << 12) | 0x73U;
        // CSRRW with rd=x0 intentionally has no architectural writeback.
        // HSR reports that condition separately from execution failure.
        const bool completed = execute(
            instruction, true, allowPostExecutionBusActivity);
        writeRegister(HadCsr, 0, 16);
        return completed;
    }

    bool
    readCsr(std::uint16_t address, std::uint64_t &value)
    {
        writeRegister(HadCsr, 0, 16);
        const std::uint32_t instruction =
            (static_cast<std::uint32_t>(address) << 20) |
            (2U << 12) | 0x73U;
        if (!execute(instruction)) {
            return false;
        }
        value = readRegister(HadWbbr, 64);
        return true;
    }

    bool
    writeFpr(unsigned index, std::uint64_t value)
    {
        if (!writeGpr(31, value)) {
            return false;
        }
        const std::uint32_t instruction =
            0xf2000053U | (31U << 15) | (index << 7);
        return execute(instruction);
    }

    bool
    resume(std::uint64_t pc)
    {
        writeRegister(HadPc, pc, 64);
        const std::uint64_t readBack = readRegister(HadPc, 64);
        if (readBack != pc) {
            return false;
        }
        writeRegister(HadPcGoExit, pc, 64);
        for (unsigned cycle = 0; cycle < 8; ++cycle) {
            mainClock();
        }
        return !_top.cosim_debug_mode_o;
    }

  private:
    bool
    jtagCycle(bool tms, bool tdi)
    {
        _top.jtag_tms_i = tms;
        _top.jtag_tdi_i = tdi;
        _top.jtag_tck_i = 0;
        mainClock();
        _top.jtag_tck_i = 1;
        eval();
        const bool tdo = _top.jtag_tdo_o;
        _top.jtag_tck_i = 0;
        eval();
        return tdo;
    }

    void
    mainClock()
    {
        if (_serveBootstrapReads) {
            driveBootstrapRead();
        }
        _top.clk_i = 0;
        eval();
        const bool acceptAddress =
            _top.axi_ar_valid_o && _top.axi_ar_ready_i;
        const bool acceptData = _top.axi_r_valid_i && _top.axi_r_ready_o;
        const std::uint8_t addressId = _top.axi_ar_id_o;
        const unsigned addressBeats =
            static_cast<unsigned>(_top.axi_ar_len_o) + 1;
        _top.clk_i = 1;
        eval();
        _top.clk_i = 0;
        eval();
        if (_serveBootstrapReads) {
            if (acceptData && _readBeats != 0) {
                --_readBeats;
                if (_readBeats == 0) {
                    _readActive = false;
                }
            }
            if (acceptAddress && !_readActive) {
                _readActive = true;
                _readId = addressId;
                _readBeats = addressBeats;
            }
        }
    }

    void
    driveBootstrapRead()
    {
        _top.axi_ar_ready_i = !_readActive;
        _top.axi_r_valid_i = _readActive;
        _top.axi_r_id_i = _readId;
        _top.axi_r_resp_i = 0;
        _top.axi_r_last_i = _readActive && _readBeats == 1;
        for (unsigned word = 0; word < 4; ++word) {
            _top.axi_r_data_i[word] = 0x00000013U;
        }
    }

    void
    eval()
    {
        _top.eval();
        _context.timeInc(1);
    }

    void
    writeInstruction(std::uint16_t value)
    {
        jtagCycle(true, false);
        jtagCycle(true, false);
        jtagCycle(false, false);
        jtagCycle(false, false);
        for (unsigned bit = 0; bit < 16; ++bit) {
            jtagCycle(bit == 15, (value >> bit) & 1U);
        }
        jtagCycle(true, false);
        synchronizeUpdate();
        jtagCycle(false, false);
        synchronizeUpdate();
    }

    void
    writeData(std::uint64_t value, unsigned width)
    {
        jtagCycle(true, false);
        jtagCycle(false, false);
        jtagCycle(false, false);
        for (unsigned bit = 0; bit < width; ++bit) {
            jtagCycle(bit + 1 == width, (value >> bit) & 1U);
        }
        jtagCycle(true, false);
        synchronizeUpdate();
        jtagCycle(false, false);
        synchronizeUpdate();
    }

    std::uint64_t
    readData(unsigned width)
    {
        std::uint64_t value = 0;
        jtagCycle(true, false);
        jtagCycle(false, false);
        jtagCycle(false, false);
        for (unsigned bit = 0; bit < width; ++bit) {
            if (jtagCycle(bit + 1 == width, false)) {
                value |= std::uint64_t{1} << bit;
            }
        }
        jtagCycle(true, false);
        jtagCycle(false, false);
        return value;
    }

    void
    synchronizeUpdate()
    {
        // T-Head's HAD crosses UPDATE_IR/UPDATE_DR through level-to-pulse
        // synchronizers and specifies TCK as the slow clock. This dwell is
        // used on both sides of the TAP update-to-idle edge: private HAD logic
        // samples the update level, while the common router first registers
        // it on that TCK edge. Keep the serial shifter stable until both have
        // consumed the write.
        for (unsigned cycle = 0; cycle < 8; ++cycle) {
            mainClock();
        }
    }

    Vrtl_cosim_c910_top &_top;
    VerilatedContext &_context;
    bool _serveBootstrapReads = true;
    bool _readActive = false;
    std::uint8_t _readId = 0;
    unsigned _readBeats = 0;
    std::uint32_t _lastStatus = 0;
};

} // anonymous namespace

C910CpuState::C910CpuState(Vrtl_cosim_c910_top &top,
                           VerilatedContext &context)
    : _top(top), _context(context)
{}

const char *
C910CpuState::schema() const noexcept
{
    return RtlCpuStateSchema::Riscv64V1;
}

std::size_t
C910CpuState::contextCount() const noexcept
{
    return 1;
}

bool
C910CpuState::importState(std::size_t context, const CpuStateValue *values,
                          std::size_t valueCount) noexcept
{
    _error[0] = '\0';
    if (context != 0) {
        setError("PULP C910 has no architectural context %zu", context);
        return false;
    }
    if (_imported) {
        setError("PULP C910 architectural state was already imported");
        return false;
    }

    ImportedState state;
    std::string validationError;
    if (!parseState(values, valueCount, state, validationError)) {
        setError("invalid riscv64/v1 state: %s", validationError.c_str());
        return false;
    }

    try {
        Verilated::threadContextp(&_context);
        // JTAG operation advances the CPU clock. Prevent an AXI handshake
        // from occurring behind the framework transactor's back.
        AxiInputGuard isolatedBus(_top);
        ImportMutationGuard mutation(_top, _context);
        HadJtag had(_top, _context);
        if (!had.enterDebug()) {
            setError("PULP C910 HAD did not enter debug mode");
            return false;
        }

        // Address registers precede configuration because locked PMP entries
        // cannot be changed after their L bit is imported.
        for (unsigned index = 0; index < 16; ++index) {
            const std::string name = "pmpaddr" + std::to_string(index);
            if (!had.writeCsr(static_cast<std::uint16_t>(0x3b0 + index),
                              state.csr.at(name))) {
                setError("PULP C910 HAD timed out writing CSR %s",
                         name.c_str());
                return false;
            }
        }
        for (const CsrDefinition &definition : CsrDefinitions) {
            if (std::strcmp(definition.name, "mstatus") == 0 ||
                std::strcmp(definition.name, "pmpcfg0") == 0 ||
                std::strcmp(definition.name, "pmpcfg2") == 0 ||
                std::strcmp(definition.name, "satp") == 0 ||
                std::strcmp(definition.name, "mie") == 0) {
                continue;
            }
            if (!had.writeCsr(definition.address,
                              state.csr.at(definition.name))) {
                setError("PULP C910 HAD timed out writing CSR %s",
                         definition.name);
                return false;
            }
        }
        if (!had.writeCsr(0x3a0, state.csr.at("pmpcfg0")) ||
            !had.writeCsr(0x3a2, state.csr.at("pmpcfg2"))) {
            setError("PULP C910 HAD timed out writing PMP configuration");
            return false;
        }

        // Enable the F/D context while loading exact IEEE-754 register bits.
        const std::uint64_t desiredMstatus = state.csr.at("mstatus");
        if (!had.writeCsr(0x300, desiredMstatus | (3ULL << 13))) {
            setError("PULP C910 HAD timed out enabling floating-point state");
            return false;
        }
        for (unsigned index = 0; index < state.f.size(); ++index) {
            if (!had.writeFpr(index, state.f[index])) {
                setError("PULP C910 HAD timed out writing f%u", index);
                return false;
            }
        }
        if (!had.writeCsr(0x003, state.csr.at("fcsr"))) {
            setError("PULP C910 HAD timed out writing fcsr");
            return false;
        }

        // Stage a non-M-mode transition while address translation is still
        // disabled. This keeps MRET quiescent so HAD can restore the mepc it
        // consumes and only then install the imported satp value.
        if (state.privilege != 3) {
            std::uint64_t transition =
                (desiredMstatus &
                 ~(MstatusMppMask | MstatusMie | MstatusMpie)) |
                (static_cast<std::uint64_t>(state.privilege) << 11);
            if (desiredMstatus & MstatusMie) {
                transition |= MstatusMpie;
            }
            if (!had.writeCsr(0x341, state.pc)) {
                setError("PULP C910 HAD could not stage mepc for privilege "
                         "mode %u", state.privilege);
                return false;
            }
            if (!had.writeCsr(0x300, transition)) {
                setError("PULP C910 HAD could not stage mstatus for "
                         "privilege mode %u", state.privilege);
                return false;
            }
        } else if (!had.writeCsr(0x300, desiredMstatus)) {
            setError("PULP C910 HAD timed out restoring mstatus");
            return false;
        }
        if (state.privilege == 3 &&
            !had.writeCsr(0x180, state.csr.at("satp"))) {
            setError("PULP C910 HAD timed out restoring satp");
            return false;
        }
        if (!had.writeCsr(0x304, state.csr.at("mie"))) {
            setError("PULP C910 HAD timed out restoring mie");
            return false;
        }
        std::uint64_t importedMie = 0;
        if (!had.readCsr(0x304, importedMie) ||
            importedMie != state.csr.at("mie")) {
            setError("PULP C910 HAD read back mie=%#llx, expected %#llx",
                     static_cast<unsigned long long>(importedMie),
                     static_cast<unsigned long long>(state.csr.at("mie")));
            return false;
        }

        for (unsigned index = 1; index < state.x.size(); ++index) {
            if (!had.writeGpr(index, state.x[index])) {
                setError("PULP C910 HAD timed out writing x%u", index);
                return false;
            }
        }
        if (state.privilege == 3) {
            importedMie = 0;
            if (!had.readCsr(0x304, importedMie) ||
                importedMie != state.csr.at("mie")) {
                setError("PULP C910 HAD lost mie before resume: %#llx",
                         static_cast<unsigned long long>(importedMie));
                return false;
            }
            std::uint64_t importedMstatus = 0;
            if (!had.readCsr(0x300, importedMstatus) ||
                importedMstatus != desiredMstatus) {
                setError("PULP C910 HAD read back mstatus=%#llx, expected "
                         "%#llx before resume",
                         static_cast<unsigned long long>(importedMstatus),
                         static_cast<unsigned long long>(desiredMstatus));
                return false;
            }
        }
        if (state.privilege != 3) {
            if (!had.execute(Mret, true, true)) {
                setError("PULP C910 HAD could not enter privilege mode %u "
                         "(HSR=%#x)", state.privilege, had.lastStatus());
                return false;
            }
            // MRET updates the interrupt-stack and privilege fields. Rewrite
            // the canonical post-MRET value while HAD's debug privilege
            // bypass is active, including the imported FS/VS state.
            if (!had.writeCsr(0x300, desiredMstatus)) {
                setError("PULP C910 HAD could not restore mstatus after "
                         "entering privilege mode %u", state.privilege);
                return false;
            }
            // MRET necessarily consumes and overwrites mepc while changing
            // privilege. HAD remains active after the injected instruction,
            // so restore the architectural value before leaving debug mode.
            if (!had.writeCsr(0x341, state.csr.at("mepc"))) {
                setError("PULP C910 HAD could not restore mepc after entering "
                         "privilege mode %u", state.privilege);
                return false;
            }
            std::uint64_t restoredMepc = 0;
            if (!had.readCsr(0x341, restoredMepc) ||
                restoredMepc != state.csr.at("mepc")) {
                setError("PULP C910 HAD read back mepc=%#llx after entering "
                         "privilege mode %u",
                         static_cast<unsigned long long>(restoredMepc),
                         state.privilege);
                return false;
            }
            std::uint64_t restoredMstatus = 0;
            if (!had.readCsr(0x300, restoredMstatus) ||
                restoredMstatus != desiredMstatus) {
                setError("PULP C910 HAD read back mstatus=%#llx, expected "
                         "%#llx after entering privilege mode %u",
                         static_cast<unsigned long long>(restoredMstatus),
                         static_cast<unsigned long long>(desiredMstatus),
                         state.privilege);
                return false;
            }
            if (!had.writeCsr(0x180, state.csr.at("satp"), true)) {
                setError("PULP C910 HAD could not restore satp after entering "
                         "privilege mode %u", state.privilege);
                return false;
            }
        }
        if (!had.resume(state.pc)) {
            setError("PULP C910 HAD did not leave debug mode");
            return false;
        }
        _imported = true;
        mutation.commit();
        return true;
    } catch (const std::exception &exception) {
        setError("PULP C910 state import failed: %s", exception.what());
    } catch (...) {
        setError("PULP C910 state import failed with an unknown exception");
    }
    return false;
}

const char *
C910CpuState::getLastError() const noexcept
{
    return _error.data();
}

void
C910CpuState::setError(const char *format, ...) noexcept
{
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(_error.data(), _error.size(), format, arguments);
    va_end(arguments);
}
