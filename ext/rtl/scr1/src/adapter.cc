/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "Vscr1_top_axi.h"
#include "Vscr1_top_axi___024root.h"
#include "gem5/rtl_cosim/api_v1.hh"
#include "verilated.h"

namespace
{

constexpr std::uint64_t TcmBase = 0x00480000;
constexpr std::uint64_t TcmSize = 64 * 1024;

class ErrorState
{
  public:
    void
    clear() noexcept
    {
        _message[0] = '\0';
    }

    void
    set(const char *message) noexcept
    {
        std::snprintf(_message.data(), _message.size(), "%s",
                      message ? message : "unknown error");
    }

    void
    setSignal(const char *operation, const char *name) noexcept
    {
        std::snprintf(_message.data(), _message.size(),
                      "%s rejected for signal '%s'", operation, name);
    }

    const char *
    get() const noexcept
    {
        return _message.data();
    }

  private:
    std::array<char, 512> _message{};
};

class PortSignalBase : public Signal
{
  public:
    void
    setChangeCallback(SignalChangeCallback *callback) noexcept override
    {
        _callback = callback;
    }

    virtual void notifyChange() noexcept = 0;

  protected:
    void
    updateCallback() noexcept
    {
        if (_callback) {
            _callback->update();
        }
    }

  private:
    SignalChangeCallback *_callback = nullptr;
};

template <class T> class PortSignal final : public PortSignalBase
{
  public:
    using U = std::make_unsigned_t<T>;

    PortSignal(std::string name, std::size_t width, SignalDirection direction,
               T &port, std::size_t offset, ErrorState &error)
        : _name(std::move(name)),
          _width(width),
          _direction(direction),
          _port(port),
          _offset(offset),
          _error(error),
          _last(value())
    {
        static_assert(std::is_integral_v<T>);
    }

    const char *
    name() const noexcept override
    {
        return _name.c_str();
    }
    std::size_t
    bitWidth() const noexcept override
    {
        return _width;
    }
    SignalDirection
    direction() const noexcept override
    {
        return _direction;
    }

    bool
    getValue(std::uint8_t *data, std::size_t size) const noexcept override
    {
        if (!data || size != byteWidth()) {
            _error.setSignal("read", _name.c_str());
            return false;
        }
        U current = value();
        for (std::size_t index = 0; index < size; ++index) {
            data[index] = static_cast<std::uint8_t>(current >> (index * 8));
        }
        _error.clear();
        return true;
    }

    bool
    setValue(const std::uint8_t *data, std::size_t size) noexcept override
    {
        if (_direction != SignalDirection::Input || !data ||
            size != byteWidth()) {
            _error.setSignal("write", _name.c_str());
            return false;
        }
        if (_width % 8 != 0 && data[size - 1] >> (_width % 8) != 0) {
            _error.setSignal("nonzero unused high bits", _name.c_str());
            return false;
        }
        U incoming = 0;
        for (std::size_t index = 0; index < size; ++index) {
            incoming |= static_cast<U>(data[index]) << (index * 8);
        }
        const U shiftedMask = static_cast<U>(mask() << _offset);
        U current = static_cast<U>(_port);
        current = static_cast<U>((current & ~shiftedMask) |
                                 ((incoming & mask()) << _offset));
        _port = static_cast<T>(current);
        _error.clear();
        return true;
    }

    void
    notifyChange() noexcept override
    {
        const U current = value();
        if (_direction == SignalDirection::Output && current != _last) {
            updateCallback();
        }
        _last = current;
    }

  private:
    std::size_t
    byteWidth() const noexcept
    {
        return (_width + 7) / 8;
    }

    U
    mask() const noexcept
    {
        constexpr std::size_t Digits = std::numeric_limits<U>::digits;
        return _width == Digits ? std::numeric_limits<U>::max()
                                : static_cast<U>((U{1} << _width) - 1);
    }

    U
    value() const noexcept
    {
        return static_cast<U>((static_cast<U>(_port) >> _offset) & mask());
    }

    std::string _name;
    std::size_t _width;
    SignalDirection _direction;
    T &_port;
    std::size_t _offset;
    ErrorState &_error;
    U _last;
};

class Scr1Bus final : public Bus
{
  public:
    explicit Scr1Bus(const char *name) : _name(name) {}

    const char *
    name() const noexcept override
    {
        return _name;
    }
    BusProtocol
    protocol() const noexcept override
    {
        return BusProtocol::Axi4;
    }
    BusRole
    role() const noexcept override
    {
        return BusRole::Initiator;
    }
    std::size_t
    signalCount() const noexcept override
    {
        return _bindings.size();
    }
    SignalBinding
    signal(std::size_t index) noexcept override
    {
        return index < _bindings.size() ? _bindings[index]
                                        : SignalBinding{0, nullptr};
    }
    void
    add(SignalRoleId role, Signal *signal)
    {
        _bindings.push_back({role, signal});
    }

  private:
    const char *_name;
    std::vector<SignalBinding> _bindings;
};

std::unique_ptr<VerilatedContext>
makeContext()
{
    auto context = std::make_unique<VerilatedContext>();
    // A V1 core is clocked serially. Explicitly avoid Verilator's process-wide
    // default worker pool, which is unnecessary here and impedes DLL teardown
    // when several independent contexts coexist.
    context->threads(1);
    return context;
}

class Scr1Core final : public RtlCore
{
  public:
    explicit Scr1Core(std::string instanceName)
        : _name(std::move(instanceName)),
          _context(makeContext()),
          _top(std::make_unique<Vscr1_top_axi>(_context.get(), _name.c_str())),
          _buses{std::make_unique<Scr1Bus>("instruction"),
                 std::make_unique<Scr1Bus>("data")}
    {
        initializeInputs();
        _top->eval();
        buildAxiBuses();
        buildStandaloneSignals();
        for (auto &signal : _signals) {
            signal->notifyChange();
        }
    }

    ~Scr1Core() override
    {
        activateContext();
        try {
            _top->final();
        } catch (...) {}
    }

    const char *
    name() const noexcept override
    {
        return _name.c_str();
    }
    std::size_t
    busCount() const noexcept override
    {
        return _buses.size();
    }
    Bus *
    bus(std::size_t index) noexcept override
    {
        return index < _buses.size() ? _buses[index].get() : nullptr;
    }
    std::size_t
    signalCount() const noexcept override
    {
        return _coreSignals.size();
    }
    CoreSignalBinding
    signal(std::size_t index) noexcept override
    {
        return index < _coreSignals.size()
                   ? _coreSignals[index]
                   : CoreSignalBinding{CoreSignalRole::Io, 0,
                                       ActiveLevel::High, nullptr};
    }
    std::size_t
    memoryCount() const noexcept override
    {
        return _memories.size();
    }
    const MemoryRegion *
    memory(std::size_t index) const noexcept override
    {
        return index < _memories.size() ? &_memories[index] : nullptr;
    }

    bool
    readMemory(std::size_t memoryIndex, std::uint64_t offset,
               std::uint8_t *data, std::size_t size) const noexcept override
    {
        if (_terminal || memoryIndex >= _memories.size() || !data ||
            offset > TcmSize || size > TcmSize - offset) {
            _error.set("invalid or terminal SCR1 TCM read");
            return false;
        }
        const auto &words = tcmWords();
        for (std::size_t index = 0; index < size; ++index) {
            const std::uint64_t byte = offset + index;
            data[index] =
                static_cast<std::uint8_t>(words[byte / 4] >> ((byte % 4) * 8));
        }
        _error.clear();
        return true;
    }

    bool
    writeMemory(std::size_t memoryIndex, std::uint64_t offset,
                const std::uint8_t *data, std::size_t size) noexcept override
    {
        if (_terminal || memoryIndex >= _memories.size() || !data ||
            offset > TcmSize || size > TcmSize - offset) {
            _error.set("invalid or terminal SCR1 TCM write");
            return false;
        }
        auto &words = tcmWords();
        for (std::size_t index = 0; index < size; ++index) {
            const std::uint64_t byte = offset + index;
            const unsigned shift = static_cast<unsigned>((byte % 4) * 8);
            const std::uint32_t mask = std::uint32_t{0xff} << shift;
            words[byte / 4] = (words[byte / 4] & ~mask) |
                              (std::uint32_t{data[index]} << shift);
        }
        _error.clear();
        return true;
    }

    bool
    settle() noexcept override
    {
        return evaluate();
    }

    ClockResult
    clock() noexcept override
    {
        if (_terminal) {
            _error.set("clock called after terminal state");
            return ClockResult::Error;
        }
        try {
            activateContext();
            _top->clk = 0;
            _top->eval();
            _context->timeInc(1);
            _top->clk = 1;
            _top->eval();
            _context->timeInc(1);
            _top->clk = 0;
            _top->eval();
            notifyOutputs();
            if (_context->gotFinish()) {
                _terminal = true;
                _error.set("SCR1 Verilated model requested finish");
                return ClockResult::Finished;
            }
            _error.clear();
            return ClockResult::Completed;
        } catch (const std::exception &exception) {
            _error.set(exception.what());
        } catch (...) {
            _error.set("unknown Verilator clock failure");
        }
        _terminal = true;
        return ClockResult::Error;
    }

    bool
    isIdle() const noexcept override
    {
        if (_terminal || !_top->pwrup_rst_n || !_top->rst_n ||
            !_top->cpu_rst_n || !_top->test_rst_n || _top->test_mode ||
            _top->irq_lines || _top->soft_irq || _top->io_axi_imem_bvalid ||
            _top->io_axi_imem_rvalid || _top->io_axi_dmem_bvalid ||
            _top->io_axi_dmem_rvalid) {
            return false;
        }
        const auto &root = *_top->rootp;
        return root.scr1_top_axi__DOT__i_core_top__DOT__i_pipe_top__DOT__i_pipe_exu__DOT__wfi_halted_ff &&
               !root.scr1_top_axi__DOT__i_core_top__DOT__i_pipe_top__DOT__pipe2clkctl_wake_req_o &&
               !root.scr1_top_axi__DOT__i_timer__DOT__timer_en &&
               root.scr1_top_axi__DOT__axi_imem_idle &&
               root.scr1_top_axi__DOT__axi_dmem_idle &&
               !_top->io_axi_imem_awvalid && !_top->io_axi_imem_wvalid &&
               !_top->io_axi_imem_arvalid && !_top->io_axi_dmem_awvalid &&
               !_top->io_axi_dmem_wvalid && !_top->io_axi_dmem_arvalid;
    }

    const char *
    getLastError() const noexcept override
    {
        return _error.get();
    }

  private:
    template <class T>
    PortSignalBase *
    addSignal(std::string name, std::size_t width, SignalDirection direction,
              T &port, std::size_t offset = 0)
    {
        auto signal = std::make_unique<PortSignal<T>>(
            std::move(name), width, direction, port, offset, _error);
        PortSignalBase *result = signal.get();
        _signals.push_back(std::move(signal));
        return result;
    }

    template <class T>
    void
    addBusSignal(Scr1Bus &bus, SignalRoleId role, const char *name,
                 std::size_t width, SignalDirection direction, T &port)
    {
        bus.add(role, addSignal(name, width, direction, port));
    }

    template <class T>
    void
    addCoreSignal(CoreSignalRole role, std::uint32_t index,
                  ActiveLevel activeLevel, std::string name, std::size_t width,
                  SignalDirection direction, T &port, std::size_t offset = 0)
    {
        Signal *signal =
            addSignal(std::move(name), width, direction, port, offset);
        _coreSignals.push_back({role, index, activeLevel, signal});
    }

    void initializeInputs() noexcept;
    void buildAxiBuses();
    void buildStandaloneSignals();

    bool
    evaluate() noexcept
    {
        if (_terminal) {
            _error.set("settle called after terminal state");
            return false;
        }
        try {
            activateContext();
            _top->eval();
            notifyOutputs();
            if (_context->gotFinish()) {
                _terminal = true;
                _error.set("SCR1 Verilated model requested finish");
                return false;
            }
            _error.clear();
            return true;
        } catch (const std::exception &exception) {
            _error.set(exception.what());
        } catch (...) {
            _error.set("unknown Verilator settle failure");
        }
        _terminal = true;
        return false;
    }

    void
    notifyOutputs() noexcept
    {
        for (auto &signal : _signals) {
            signal->notifyChange();
        }
    }

    void
    activateContext() const noexcept
    {
        // Verilator's scope bookkeeping uses its thread-local active context,
        // including during model destruction. Rebind it on every entry so
        // multiple independently loaded cores remain safe on one host thread.
        Verilated::threadContextp(_context.get());
    }

    VlUnpacked<IData, 16384> &
    tcmWords() noexcept
    {
        return _top->rootp
            ->scr1_top_axi__DOT__i_tcm__DOT__i_dp_memory__DOT__ram_block;
    }

    const VlUnpacked<IData, 16384> &
    tcmWords() const noexcept
    {
        return _top->rootp
            ->scr1_top_axi__DOT__i_tcm__DOT__i_dp_memory__DOT__ram_block;
    }

    std::string _name;
    std::unique_ptr<VerilatedContext> _context;
    std::unique_ptr<Vscr1_top_axi> _top;
    std::array<std::unique_ptr<Scr1Bus>, 2> _buses;
    std::vector<std::unique_ptr<PortSignalBase>> _signals;
    std::vector<CoreSignalBinding> _coreSignals;
    const std::array<MemoryRegion, 2> _memories{{
        {"unified_tcm_code", MemoryRole::CodeTcm, TcmBase, TcmSize},
        {"unified_tcm_data", MemoryRole::DataTcm, TcmBase, TcmSize},
    }};
    mutable ErrorState _error;
    bool _terminal = false;
};

void
Scr1Core::initializeInputs() noexcept
{
    _top->clk = 0;
    _top->rtc_clk = 0;
    _top->pwrup_rst_n = 0;
    _top->rst_n = 0;
    _top->cpu_rst_n = 0;
    _top->test_rst_n = 0;
    _top->test_mode = 0;
    _top->fuse_mhartid = 0;
    _top->fuse_idcode = 0;
    _top->irq_lines = 0;
    _top->soft_irq = 0;
    _top->trst_n = 0;
    _top->tck = 0;
    _top->tms = 0;
    _top->tdi = 0;

#define ZERO_AXI_INPUT(prefix)                                                \
    _top->prefix##_awready = 0;                                               \
    _top->prefix##_wready = 0;                                                \
    _top->prefix##_bid = 0;                                                   \
    _top->prefix##_bresp = 0;                                                 \
    _top->prefix##_bvalid = 0;                                                \
    _top->prefix##_buser = 0;                                                 \
    _top->prefix##_arready = 0;                                               \
    _top->prefix##_rid = 0;                                                   \
    _top->prefix##_rdata = 0;                                                 \
    _top->prefix##_rresp = 0;                                                 \
    _top->prefix##_rlast = 0;                                                 \
    _top->prefix##_ruser = 0;                                                 \
    _top->prefix##_rvalid = 0
    ZERO_AXI_INPUT(io_axi_imem);
    ZERO_AXI_INPUT(io_axi_dmem);
#undef ZERO_AXI_INPUT
}

#define SCR1_AXI_PORTS(M, prefix, bus)                                        \
    M(prefix, bus, AwId, awid, 4, Output)                                     \
    M(prefix, bus, AwAddr, awaddr, 32, Output)                                \
    M(prefix, bus, AwLen, awlen, 8, Output)                                   \
    M(prefix, bus, AwSize, awsize, 3, Output)                                 \
    M(prefix, bus, AwBurst, awburst, 2, Output)                               \
    M(prefix, bus, AwLock, awlock, 1, Output)                                 \
    M(prefix, bus, AwCache, awcache, 4, Output)                               \
    M(prefix, bus, AwProt, awprot, 3, Output)                                 \
    M(prefix, bus, AwRegion, awregion, 4, Output)                             \
    M(prefix, bus, AwUser, awuser, 4, Output)                                 \
    M(prefix, bus, AwQos, awqos, 4, Output)                                   \
    M(prefix, bus, AwValid, awvalid, 1, Output)                               \
    M(prefix, bus, AwReady, awready, 1, Input)                                \
    M(prefix, bus, WData, wdata, 32, Output)                                  \
    M(prefix, bus, WStrb, wstrb, 4, Output)                                   \
    M(prefix, bus, WLast, wlast, 1, Output)                                   \
    M(prefix, bus, WUser, wuser, 4, Output)                                   \
    M(prefix, bus, WValid, wvalid, 1, Output)                                 \
    M(prefix, bus, WReady, wready, 1, Input)                                  \
    M(prefix, bus, BId, bid, 4, Input)                                        \
    M(prefix, bus, BResp, bresp, 2, Input)                                    \
    M(prefix, bus, BUser, buser, 4, Input)                                    \
    M(prefix, bus, BValid, bvalid, 1, Input)                                  \
    M(prefix, bus, BReady, bready, 1, Output)                                 \
    M(prefix, bus, ArId, arid, 4, Output)                                     \
    M(prefix, bus, ArAddr, araddr, 32, Output)                                \
    M(prefix, bus, ArLen, arlen, 8, Output)                                   \
    M(prefix, bus, ArSize, arsize, 3, Output)                                 \
    M(prefix, bus, ArBurst, arburst, 2, Output)                               \
    M(prefix, bus, ArLock, arlock, 1, Output)                                 \
    M(prefix, bus, ArCache, arcache, 4, Output)                               \
    M(prefix, bus, ArProt, arprot, 3, Output)                                 \
    M(prefix, bus, ArRegion, arregion, 4, Output)                             \
    M(prefix, bus, ArUser, aruser, 4, Output)                                 \
    M(prefix, bus, ArQos, arqos, 4, Output)                                   \
    M(prefix, bus, ArValid, arvalid, 1, Output)                               \
    M(prefix, bus, ArReady, arready, 1, Input)                                \
    M(prefix, bus, RId, rid, 4, Input)                                        \
    M(prefix, bus, RData, rdata, 32, Input)                                   \
    M(prefix, bus, RResp, rresp, 2, Input)                                    \
    M(prefix, bus, RLast, rlast, 1, Input)                                    \
    M(prefix, bus, RUser, ruser, 4, Input)                                    \
    M(prefix, bus, RValid, rvalid, 1, Input)                                  \
    M(prefix, bus, RReady, rready, 1, Output)

void
Scr1Core::buildAxiBuses()
{
#define ADD_AXI(prefix, bus, role, suffix, width, direction)                  \
    addBusSignal(*_buses[bus], AxiSignal::role, #prefix "_" #suffix, width,   \
                 SignalDirection::direction, _top->prefix##_##suffix);
    SCR1_AXI_PORTS(ADD_AXI, io_axi_imem, 0)
    SCR1_AXI_PORTS(ADD_AXI, io_axi_dmem, 1)
#undef ADD_AXI
}

#undef SCR1_AXI_PORTS

void
Scr1Core::buildStandaloneSignals()
{
#define ADD_CORE(role, index, level, name, width, direction, port)            \
    addCoreSignal(CoreSignalRole::role, index, ActiveLevel::level, name,      \
                  width, SignalDirection::direction, _top->port)
    ADD_CORE(Reset, 0, Low, "pwrup_rst_n", 1, Input, pwrup_rst_n);
    ADD_CORE(Reset, 1, Low, "rst_n", 1, Input, rst_n);
    ADD_CORE(Reset, 2, Low, "cpu_rst_n", 1, Input, cpu_rst_n);
    ADD_CORE(Reset, 3, Low, "test_rst_n", 1, Input, test_rst_n);
    ADD_CORE(Reset, 4, Low, "trst_n", 1, Input, trst_n);
    ADD_CORE(Reset, 5, Low, "sys_rst_n_o", 1, Output, sys_rst_n_o);

    for (std::uint32_t index = 0; index < 16; ++index) {
        addCoreSignal(CoreSignalRole::Interrupt, index, ActiveLevel::High,
                      "irq_lines[" + std::to_string(index) + "]", 1,
                      SignalDirection::Input, _top->irq_lines, index);
    }
    ADD_CORE(Interrupt, 16, High, "soft_irq", 1, Input, soft_irq);

    ADD_CORE(Io, 0, High, "fuse_mhartid", 32, Input, fuse_mhartid);
    ADD_CORE(Io, 1, High, "fuse_idcode", 32, Input, fuse_idcode);
    ADD_CORE(Io, 2, High, "test_mode", 1, Input, test_mode);
    ADD_CORE(Io, 3, High, "rtc_clk", 1, Input, rtc_clk);
    ADD_CORE(Io, 4, High, "tck", 1, Input, tck);
    ADD_CORE(Io, 5, High, "tms", 1, Input, tms);
    ADD_CORE(Io, 6, High, "tdi", 1, Input, tdi);
    ADD_CORE(Io, 7, High, "sys_rdc_qlfy_o", 1, Output, sys_rdc_qlfy_o);
    ADD_CORE(Io, 8, High, "tdo", 1, Output, tdo);
    ADD_CORE(Io, 9, High, "tdo_en", 1, Output, tdo_en);
#undef ADD_CORE
}

bool
parseConfig(const char *json, std::string &name, ErrorState &error)
{
    if (!json) {
        error.set("SCR1 configuration JSON is null");
        return false;
    }
    std::string_view text(json);
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    if (text == "{}") {
        name = "scr1";
        return true;
    }

    constexpr std::string_view Prefix = "{\"instance_name\":\"";
    if (!text.starts_with(Prefix) || !text.ends_with("\"}")) {
        error.set(
            "SCR1 config must be {} or contain only string instance_name");
        return false;
    }
    const std::string_view value =
        text.substr(Prefix.size(), text.size() - Prefix.size() - 2);
    if (value.empty() || value.size() > 128 ||
        value.find_first_of("\"\\") != std::string_view::npos) {
        error.set("SCR1 instance_name must be 1..128 unescaped characters");
        return false;
    }
    name.assign(value);
    return true;
}

class Scr1Manager final : public RtlCoreManager
{
  public:
    RtlCore *
    createCore(const char *configJson) noexcept override
    {
        try {
            std::string name;
            if (!parseConfig(configJson, name, _error)) {
                return nullptr;
            }
            auto core = std::make_unique<Scr1Core>(std::move(name));
            _error.clear();
            return core.release();
        } catch (const std::exception &exception) {
            _error.set(exception.what());
        } catch (...) {
            _error.set("unknown SCR1 construction failure");
        }
        return nullptr;
    }

    void
    destroyCore(RtlCore *core) noexcept override
    {
        delete static_cast<Scr1Core *>(core);
    }
    const char *
    getLastError() const noexcept override
    {
        return _error.get();
    }

  private:
    ErrorState _error;
};

} // anonymous namespace

extern "C" RtlCoreManager *
createRtlCoreManagerV1() noexcept
{
    try {
        return new Scr1Manager;
    } catch (...) {
        return nullptr;
    }
}

extern "C" void
destroyRtlCoreManagerV1(RtlCoreManager *manager) noexcept
{
    delete static_cast<Scr1Manager *>(manager);
}
