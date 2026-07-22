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

#include "Vrtl_cosim_c910_top.h"
#include "cpu_state.hh"
#include "gem5/rtl_cosim/api_v1.hh"
#include "verilated.h"

namespace
{

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
    PortSignal(std::string name, std::size_t width, SignalDirection direction,
               T &port, std::size_t offset, ErrorState &error)
        : _name(std::move(name)),
          _width(width),
          _direction(direction),
          _port(port),
          _offset(offset),
          _error(error),
          _last(capture())
    {}

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
        const Bytes current = capture();
        for (std::size_t index = 0; index < size; ++index) {
            data[index] = current[index];
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
        for (std::size_t bit = 0; bit < _width; ++bit) {
            writeBit(_offset + bit, (data[bit / 8] >> (bit % 8)) & 1);
        }
        _error.clear();
        return true;
    }

    void
    notifyChange() noexcept override
    {
        const Bytes current = capture();
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

    bool
    readBit(std::size_t bit) const noexcept
    {
        if constexpr (std::is_integral_v<T>) {
            using U = std::make_unsigned_t<T>;
            return (static_cast<U>(_port) >> bit) & U{1};
        } else {
            return (static_cast<std::uint32_t>(_port[bit / 32]) >>
                    (bit % 32)) & 1U;
        }
    }

    void
    writeBit(std::size_t bit, bool value) noexcept
    {
        if constexpr (std::is_integral_v<T>) {
            using U = std::make_unsigned_t<T>;
            const U mask = U{1} << bit;
            U current = static_cast<U>(_port);
            current = value ? current | mask : current & ~mask;
            _port = static_cast<T>(current);
        } else {
            const std::uint32_t mask = std::uint32_t{1} << (bit % 32);
            std::uint32_t current =
                static_cast<std::uint32_t>(_port[bit / 32]);
            current = value ? current | mask : current & ~mask;
            _port[bit / 32] = current;
        }
    }

    using Bytes = std::array<std::uint8_t, sizeof(T)>;

    Bytes
    capture() const
    {
        Bytes bytes{};
        for (std::size_t bit = 0; bit < _width; ++bit) {
            if (readBit(_offset + bit)) {
                bytes[bit / 8] |= static_cast<std::uint8_t>(1U << (bit % 8));
            }
        }
        return bytes;
    }

    std::string _name;
    std::size_t _width;
    SignalDirection _direction;
    T &_port;
    std::size_t _offset;
    ErrorState &_error;
    Bytes _last;
};

class C910Bus final : public Bus
{
  public:
    const char *
    name() const noexcept override
    {
        return "memory";
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
    std::vector<SignalBinding> _bindings;
};

std::unique_ptr<VerilatedContext>
makeContext()
{
    auto context = std::make_unique<VerilatedContext>();
    context->threads(1);
    return context;
}

class C910Core final : public RtlCore
{
  public:
    explicit C910Core(std::string instanceName)
        : _name(std::move(instanceName)),
          _context(makeContext()),
          _top(std::make_unique<Vrtl_cosim_c910_top>(
              _context.get(), _name.c_str())),
          _bus(std::make_unique<C910Bus>()),
          _cpuState(std::make_unique<C910CpuState>(*_top, *_context))
    {
        initializeInputs();
        _top->eval();
        buildAxiBus();
        buildStandaloneSignals();
        notifyOutputs();
    }

    ~C910Core() override
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
        return 1;
    }

    Bus *
    bus(std::size_t index) noexcept override
    {
        return index == 0 ? _bus.get() : nullptr;
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
        return 0;
    }

    const MemoryRegion *
    memory(std::size_t) const noexcept override
    {
        return nullptr;
    }

    bool
    readMemory(std::size_t, std::uint64_t, std::uint8_t *,
               std::size_t) const noexcept override
    {
        _error.set("PULP C910 exposes no TCM backdoor");
        return false;
    }

    bool
    writeMemory(std::size_t, std::uint64_t, const std::uint8_t *,
                std::size_t) noexcept override
    {
        _error.set("PULP C910 exposes no TCM backdoor");
        return false;
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
            _top->clk_i = 0;
            _top->eval();
            _context->timeInc(1);
            _top->clk_i = 1;
            _top->eval();
            _context->timeInc(1);
            _top->clk_i = 0;
            _top->eval();
            notifyOutputs();
            if (!_top->rst_ni) {
                _sawRunning = false;
            } else if (_top->lpmd_b_o == 3) {
                _sawRunning = true;
            }
            if (_context->gotFinish()) {
                _terminal = true;
                _error.set("PULP C910 Verilated model requested finish");
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
        if (_terminal || !_top->rst_ni || !_sawRunning || rawWakePending()) {
            return false;
        }
        if (_top->axi_aw_valid_o || _top->axi_w_valid_o ||
            _top->axi_ar_valid_o || _top->axi_b_valid_i ||
            _top->axi_r_valid_i) {
            return false;
        }
        return _top->lpmd_b_o != 3;
    }

    const char *
    getLastError() const noexcept override
    {
        return _error.get();
    }

    RtlCpuState *
    cpuState() noexcept override
    {
        return _cpuState.get();
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
    addBusSignal(SignalRoleId role, const char *name, std::size_t width,
                 SignalDirection direction, T &port)
    {
        _bus->add(role, addSignal(name, width, direction, port));
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
    void buildAxiBus();
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
                _error.set("PULP C910 Verilated model requested finish");
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
        Verilated::threadContextp(_context.get());
    }

    bool
    rawWakePending() const noexcept
    {
        return _top->ipi_i || _top->time_irq_i ||
               _top->plic_hartx_mint_req_i ||
               _top->plic_hartx_sint_req_i || _top->ext_int_i ||
               _top->debug_req_i || _top->jtag_tck_i;
    }

    std::string _name;
    std::unique_ptr<VerilatedContext> _context;
    std::unique_ptr<Vrtl_cosim_c910_top> _top;
    std::unique_ptr<C910Bus> _bus;
    std::unique_ptr<C910CpuState> _cpuState;
    std::vector<std::unique_ptr<PortSignalBase>> _signals;
    std::vector<CoreSignalBinding> _coreSignals;
    mutable ErrorState _error;
    bool _terminal = false;
    bool _sawRunning = false;
};

void
C910Core::initializeInputs() noexcept
{
    _top->clk_i = 0;
    _top->rst_ni = 0;
    _top->rtc_i = 0;
    _top->ipi_i = 0;
    _top->time_irq_i = 0;
    _top->plic_hartx_mint_req_i = 0;
    _top->plic_hartx_sint_req_i = 0;
    _top->debug_req_i = 0;
    _top->ext_int_i = 0;
    _top->jtag_tck_i = 0;
    _top->jtag_tdi_i = 0;
    _top->jtag_tms_i = 0;
    _top->jtag_trst_ni = 0;

    _top->axi_aw_ready_i = 0;
    _top->axi_w_ready_i = 0;
    _top->axi_b_id_i = 0;
    _top->axi_b_resp_i = 0;
    _top->axi_b_user_i = 0;
    _top->axi_b_valid_i = 0;
    _top->axi_ar_ready_i = 0;
    _top->axi_r_id_i = 0;
    for (unsigned word = 0; word < 4; ++word) {
        _top->axi_r_data_i[word] = 0;
    }
    _top->axi_r_resp_i = 0;
    _top->axi_r_last_i = 0;
    _top->axi_r_user_i = 0;
    _top->axi_r_valid_i = 0;
}

#define C910_AXI_PORTS(M)                                                     \
    M(AwId, axi_aw_id_o, 8, Output)                                          \
    M(AwAddr, axi_aw_addr_o, 40, Output)                                     \
    M(AwLen, axi_aw_len_o, 8, Output)                                        \
    M(AwSize, axi_aw_size_o, 3, Output)                                      \
    M(AwBurst, axi_aw_burst_o, 2, Output)                                    \
    M(AwLock, axi_aw_lock_o, 1, Output)                                      \
    M(AwCache, axi_aw_cache_o, 4, Output)                                    \
    M(AwProt, axi_aw_prot_o, 3, Output)                                      \
    M(AwRegion, axi_aw_region_o, 4, Output)                                  \
    M(AwQos, axi_aw_qos_o, 4, Output)                                        \
    M(AwUser, axi_aw_user_o, 1, Output)                                      \
    M(AwValid, axi_aw_valid_o, 1, Output)                                    \
    M(AwReady, axi_aw_ready_i, 1, Input)                                     \
    M(WData, axi_w_data_o, 128, Output)                                      \
    M(WStrb, axi_w_strb_o, 16, Output)                                       \
    M(WLast, axi_w_last_o, 1, Output)                                        \
    M(WUser, axi_w_user_o, 1, Output)                                        \
    M(WValid, axi_w_valid_o, 1, Output)                                      \
    M(WReady, axi_w_ready_i, 1, Input)                                       \
    M(BId, axi_b_id_i, 8, Input)                                             \
    M(BResp, axi_b_resp_i, 2, Input)                                         \
    M(BUser, axi_b_user_i, 1, Input)                                         \
    M(BValid, axi_b_valid_i, 1, Input)                                       \
    M(BReady, axi_b_ready_o, 1, Output)                                      \
    M(ArId, axi_ar_id_o, 8, Output)                                          \
    M(ArAddr, axi_ar_addr_o, 40, Output)                                     \
    M(ArLen, axi_ar_len_o, 8, Output)                                        \
    M(ArSize, axi_ar_size_o, 3, Output)                                      \
    M(ArBurst, axi_ar_burst_o, 2, Output)                                    \
    M(ArLock, axi_ar_lock_o, 1, Output)                                      \
    M(ArCache, axi_ar_cache_o, 4, Output)                                    \
    M(ArProt, axi_ar_prot_o, 3, Output)                                      \
    M(ArRegion, axi_ar_region_o, 4, Output)                                  \
    M(ArQos, axi_ar_qos_o, 4, Output)                                        \
    M(ArUser, axi_ar_user_o, 1, Output)                                      \
    M(ArValid, axi_ar_valid_o, 1, Output)                                    \
    M(ArReady, axi_ar_ready_i, 1, Input)                                     \
    M(RId, axi_r_id_i, 8, Input)                                             \
    M(RData, axi_r_data_i, 128, Input)                                       \
    M(RResp, axi_r_resp_i, 2, Input)                                         \
    M(RLast, axi_r_last_i, 1, Input)                                         \
    M(RUser, axi_r_user_i, 1, Input)                                         \
    M(RValid, axi_r_valid_i, 1, Input)                                       \
    M(RReady, axi_r_ready_o, 1, Output)

void
C910Core::buildAxiBus()
{
#define ADD_AXI(role, port, width, direction)                                \
    addBusSignal(AxiSignal::role, #port, width,                              \
                 SignalDirection::direction, _top->port);
    C910_AXI_PORTS(ADD_AXI)
#undef ADD_AXI
}

#undef C910_AXI_PORTS

void
C910Core::buildStandaloneSignals()
{
    addCoreSignal(CoreSignalRole::Reset, 0, ActiveLevel::Low, "rst_ni", 1,
                  SignalDirection::Input, _top->rst_ni);
    addCoreSignal(CoreSignalRole::Reset, 1, ActiveLevel::Low, "jtag_trst_ni",
                  1, SignalDirection::Input, _top->jtag_trst_ni);

    std::uint32_t irq = 0;
    addCoreSignal(CoreSignalRole::Interrupt, irq++, ActiveLevel::High, "ipi_i",
                  1, SignalDirection::Input, _top->ipi_i);
    addCoreSignal(CoreSignalRole::Interrupt, irq++, ActiveLevel::High,
                  "time_irq_i", 1, SignalDirection::Input,
                  _top->time_irq_i);
    for (std::size_t index = 0; index < 2; ++index) {
        addCoreSignal(CoreSignalRole::Interrupt, irq++, ActiveLevel::High,
                      "plic_hartx_mint_req_i[" + std::to_string(index) + "]",
                      1, SignalDirection::Input,
                      _top->plic_hartx_mint_req_i, index);
    }
    for (std::size_t index = 0; index < 2; ++index) {
        addCoreSignal(CoreSignalRole::Interrupt, irq++, ActiveLevel::High,
                      "plic_hartx_sint_req_i[" + std::to_string(index) + "]",
                      1, SignalDirection::Input,
                      _top->plic_hartx_sint_req_i, index);
    }
    for (std::size_t index = 0; index < 40; ++index) {
        addCoreSignal(CoreSignalRole::Interrupt, irq++, ActiveLevel::High,
                      "ext_int_i[" + std::to_string(index) + "]", 1,
                      SignalDirection::Input, _top->ext_int_i, index);
    }

    std::uint32_t io = 0;
    addCoreSignal(CoreSignalRole::Io, io++, ActiveLevel::High, "rtc_i", 1,
                  SignalDirection::Input, _top->rtc_i);
    addCoreSignal(CoreSignalRole::Io, io++, ActiveLevel::High, "debug_req_i",
                  1, SignalDirection::Input, _top->debug_req_i);
    addCoreSignal(CoreSignalRole::Io, io++, ActiveLevel::High, "jtag_tck_i", 1,
                  SignalDirection::Input, _top->jtag_tck_i);
    addCoreSignal(CoreSignalRole::Io, io++, ActiveLevel::High, "jtag_tdi_i", 1,
                  SignalDirection::Input, _top->jtag_tdi_i);
    addCoreSignal(CoreSignalRole::Io, io++, ActiveLevel::High, "jtag_tms_i", 1,
                  SignalDirection::Input, _top->jtag_tms_i);
    addCoreSignal(CoreSignalRole::Io, io++, ActiveLevel::High, "jtag_tdo_o", 1,
                  SignalDirection::Output, _top->jtag_tdo_o);
    addCoreSignal(CoreSignalRole::Io, io++, ActiveLevel::High,
                  "jtag_tdo_en_o", 1, SignalDirection::Output,
                  _top->jtag_tdo_en_o);
    addCoreSignal(CoreSignalRole::Io, io++, ActiveLevel::High, "lpmd_b_o", 2,
                  SignalDirection::Output, _top->lpmd_b_o);
}

bool
parseConfig(const char *json, std::string &name, ErrorState &error)
{
    if (!json) {
        error.set("PULP C910 configuration JSON is null");
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
        name = "pulp-c910";
        return true;
    }

    constexpr std::string_view Prefix = "{\"instance_name\":\"";
    if (!text.starts_with(Prefix) || !text.ends_with("\"}")) {
        error.set("PULP C910 config must be {} or contain only string "
                  "instance_name");
        return false;
    }
    const std::string_view value =
        text.substr(Prefix.size(), text.size() - Prefix.size() - 2);
    if (value.empty() || value.size() > 128 ||
        value.find_first_of("\"\\") != std::string_view::npos) {
        error.set("PULP C910 instance_name must be 1..128 unescaped "
                  "characters");
        return false;
    }
    name.assign(value);
    return true;
}

class C910Manager final : public RtlCoreManager
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
            auto core = std::make_unique<C910Core>(std::move(name));
            _error.clear();
            return core.release();
        } catch (const std::exception &exception) {
            _error.set(exception.what());
        } catch (...) {
            _error.set("unknown PULP C910 construction failure");
        }
        return nullptr;
    }

    void
    destroyCore(RtlCore *core) noexcept override
    {
        delete static_cast<C910Core *>(core);
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
        return new C910Manager;
    } catch (...) {
        return nullptr;
    }
}

extern "C" void
destroyRtlCoreManagerV1(RtlCoreManager *manager) noexcept
{
    delete static_cast<C910Manager *>(manager);
}
