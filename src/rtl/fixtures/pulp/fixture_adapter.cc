/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "gem5/rtl_cosim/api_v1.hh"
#include "verilated.h"

#if defined(RTL_COSIM_APB_MASTER_FIXTURE)
#include "Vapb_master_fixture.h"
using FixtureTop = Vapb_master_fixture;
static constexpr BusProtocol FixtureProtocol = BusProtocol::Apb;
static constexpr BusRole FixtureRole = BusRole::Initiator;
static constexpr const char *FixtureName = "pulp-apb-master";
#elif defined(RTL_COSIM_APB_SLAVE_FIXTURE)
#include "Vapb_slave_fixture.h"
using FixtureTop = Vapb_slave_fixture;
static constexpr BusProtocol FixtureProtocol = BusProtocol::Apb;
static constexpr BusRole FixtureRole = BusRole::Target;
static constexpr const char *FixtureName = "pulp-apb-slave";
#elif defined(RTL_COSIM_AXI_MASTER_FIXTURE)
#include "Vaxi_master_fixture.h"
using FixtureTop = Vaxi_master_fixture;
static constexpr BusProtocol FixtureProtocol = BusProtocol::Axi4;
static constexpr BusRole FixtureRole = BusRole::Initiator;
static constexpr const char *FixtureName = "pulp-axi-master";
#elif defined(RTL_COSIM_AXI_SLAVE_FIXTURE)
#include "Vaxi_slave_fixture.h"
using FixtureTop = Vaxi_slave_fixture;
static constexpr BusProtocol FixtureProtocol = BusProtocol::Axi4;
static constexpr BusRole FixtureRole = BusRole::Target;
static constexpr const char *FixtureName = "pulp-axi-slave";
#else
#error "A fixture type must be selected"
#endif

namespace
{

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
               T &port)
        : _name(std::move(name)),
          _width(width),
          _direction(direction),
          _port(port),
          _last(port)
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
        if (!data || size != (_width + 7) / 8) {
            return false;
        }
        using U = std::make_unsigned_t<T>;
        const U value = static_cast<U>(_port);
        for (std::size_t index = 0; index < size; ++index) {
            data[index] = static_cast<std::uint8_t>(value >> (index * 8));
        }
        if (_width % 8) {
            data[size - 1] &= static_cast<std::uint8_t>(
                (std::uint16_t{1} << (_width % 8)) - 1);
        }
        return true;
    }

    bool
    setValue(const std::uint8_t *data, std::size_t size) noexcept override
    {
        if (_direction != SignalDirection::Input || !data ||
            size != (_width + 7) / 8) {
            return false;
        }
        if (_width % 8 && (data[size - 1] >> (_width % 8)) != 0) {
            return false;
        }
        using U = std::make_unsigned_t<T>;
        U value = 0;
        for (std::size_t index = 0; index < size; ++index) {
            value |= static_cast<U>(data[index]) << (index * 8);
        }
        _port = static_cast<T>(value);
        return true;
    }

    void
    notifyChange() noexcept override
    {
        if (_direction == SignalDirection::Output && _port != _last) {
            updateCallback();
        }
        _last = _port;
    }

  private:
    std::string _name;
    std::size_t _width;
    SignalDirection _direction;
    T &_port;
    T _last;
};

class FixtureBus final : public Bus
{
  public:
    const char *
    name() const noexcept override
    {
        return "amba";
    }
    BusProtocol
    protocol() const noexcept override
    {
        return FixtureProtocol;
    }
    BusRole
    role() const noexcept override
    {
        return FixtureRole;
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

class FixtureCore final : public RtlCore
{
  public:
    FixtureCore()
        : _context(std::make_unique<VerilatedContext>()),
          _top(std::make_unique<FixtureTop>(_context.get(), FixtureName)),
          _bus(std::make_unique<FixtureBus>())
    {
        _top->clk_i = 0;
        _top->rst_ni = 1;
        _top->eval();
        addSignal("reset_n", 1, SignalDirection::Input, _top->rst_ni,
                  std::nullopt);
        _resetSignal = _signals.back().get();
        if constexpr (FixtureProtocol == BusProtocol::Apb) {
            buildApb();
        } else {
            buildAxi();
        }
        for (auto &signal : _signals) {
            signal->notifyChange();
        }
    }

    ~FixtureCore() override { _top->final(); }

    const char *
    name() const noexcept override
    {
        return FixtureName;
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
        return 1;
    }
    CoreSignalBinding
    signal(std::size_t index) noexcept override
    {
        if (index != 0) {
            return {CoreSignalRole::Io, 0, ActiveLevel::High, nullptr};
        }
        return {CoreSignalRole::Reset, 0, ActiveLevel::Low, _resetSignal};
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
        return false;
    }
    bool
    writeMemory(std::size_t, std::uint64_t, const std::uint8_t *,
                std::size_t) noexcept override
    {
        return false;
    }

    bool
    settle() noexcept override
    {
        if (_terminal) {
            _error = "settle called after terminal state";
            return false;
        }
        try {
            _top->eval();
            for (auto &signal : _signals) {
                signal->notifyChange();
            }
            return true;
        } catch (const std::exception &exception) {
            _error = exception.what();
        } catch (...) {
            _error = "unknown Verilator settle failure";
        }
        _terminal = true;
        return false;
    }

    ClockResult
    clock() noexcept override
    {
        if (_terminal) {
            return ClockResult::Error;
        }
        try {
            _top->clk_i = 0;
            _top->eval();
            _context->timeInc(1);
            _top->clk_i = 1;
            _top->eval();
            _context->timeInc(1);
            _top->clk_i = 0;
            _top->eval();
            for (auto &signal : _signals) {
                signal->notifyChange();
            }
            if (_context->gotFinish()) {
                _terminal = true;
                return ClockResult::Finished;
            }
            return ClockResult::Completed;
        } catch (const std::exception &exception) {
            _error = exception.what();
        } catch (...) {
            _error = "unknown Verilator evaluation failure";
        }
        _terminal = true;
        return ClockResult::Error;
    }

    bool
    isIdle() const noexcept override
    {
        return _top->idle_o != 0;
    }
    const char *
    getLastError() const noexcept override
    {
        return _error.c_str();
    }

  private:
    template <class T>
    void
    addSignal(const char *name, std::size_t width, SignalDirection direction,
              T &port, std::optional<SignalRoleId> role)
    {
        auto signal =
            std::make_unique<PortSignal<T>>(name, width, direction, port);
        if (role) {
            _bus->add(*role, signal.get());
        }
        _signals.push_back(std::move(signal));
    }

    template <class T>
    void
    addBusSignal(SignalRoleId role, const char *name, std::size_t width,
                 SignalDirection direction, T &port)
    {
        addSignal(name, width, direction, port, role);
    }

    void
    buildApb()
    {
#if defined(RTL_COSIM_APB_MASTER_FIXTURE)
        addBusSignal(ApbSignal::PAddr, "paddr", 32, SignalDirection::Output,
                     _top->paddr_o);
        addBusSignal(ApbSignal::PSel, "psel", 1, SignalDirection::Output,
                     _top->psel_o);
        addBusSignal(ApbSignal::PEnable, "penable", 1, SignalDirection::Output,
                     _top->penable_o);
        addBusSignal(ApbSignal::PWrite, "pwrite", 1, SignalDirection::Output,
                     _top->pwrite_o);
        addBusSignal(ApbSignal::PWData, "pwdata", 32, SignalDirection::Output,
                     _top->pwdata_o);
        addBusSignal(ApbSignal::PStrb, "pstrb", 4, SignalDirection::Output,
                     _top->pstrb_o);
        addBusSignal(ApbSignal::PProt, "pprot", 3, SignalDirection::Output,
                     _top->pprot_o);
        addBusSignal(ApbSignal::PReady, "pready", 1, SignalDirection::Input,
                     _top->pready_i);
        addBusSignal(ApbSignal::PRData, "prdata", 32, SignalDirection::Input,
                     _top->prdata_i);
        addBusSignal(ApbSignal::PSlvErr, "pslverr", 1, SignalDirection::Input,
                     _top->pslverr_i);
#elif defined(RTL_COSIM_APB_SLAVE_FIXTURE)
        addBusSignal(ApbSignal::PAddr, "paddr", 32, SignalDirection::Input,
                     _top->paddr_i);
        addBusSignal(ApbSignal::PSel, "psel", 1, SignalDirection::Input,
                     _top->psel_i);
        addBusSignal(ApbSignal::PEnable, "penable", 1, SignalDirection::Input,
                     _top->penable_i);
        addBusSignal(ApbSignal::PWrite, "pwrite", 1, SignalDirection::Input,
                     _top->pwrite_i);
        addBusSignal(ApbSignal::PWData, "pwdata", 32, SignalDirection::Input,
                     _top->pwdata_i);
        addBusSignal(ApbSignal::PStrb, "pstrb", 4, SignalDirection::Input,
                     _top->pstrb_i);
        addBusSignal(ApbSignal::PProt, "pprot", 3, SignalDirection::Input,
                     _top->pprot_i);
        addBusSignal(ApbSignal::PReady, "pready", 1, SignalDirection::Output,
                     _top->pready_o);
        addBusSignal(ApbSignal::PRData, "prdata", 32, SignalDirection::Output,
                     _top->prdata_o);
        addBusSignal(ApbSignal::PSlvErr, "pslverr", 1, SignalDirection::Output,
                     _top->pslverr_o);
#endif
    }

    void
    buildAxi()
    {
#if defined(RTL_COSIM_AXI_MASTER_FIXTURE)
#define ADD_AXI(role, name, width, channel, direction)                        \
    addBusSignal(AxiSignal::role, name, width, SignalDirection::direction,    \
                 _top->channel)
        ADD_AXI(AwId, "awid", 4, awid_o, Output);
        ADD_AXI(AwAddr, "awaddr", 32, awaddr_o, Output);
        ADD_AXI(AwLen, "awlen", 8, awlen_o, Output);
        ADD_AXI(AwSize, "awsize", 3, awsize_o, Output);
        ADD_AXI(AwBurst, "awburst", 2, awburst_o, Output);
        ADD_AXI(AwLock, "awlock", 1, awlock_o, Output);
        ADD_AXI(AwCache, "awcache", 4, awcache_o, Output);
        ADD_AXI(AwProt, "awprot", 3, awprot_o, Output);
        ADD_AXI(AwRegion, "awregion", 4, awregion_o, Output);
        ADD_AXI(AwQos, "awqos", 4, awqos_o, Output);
        ADD_AXI(AwUser, "awuser", 2, awuser_o, Output);
        ADD_AXI(AwValid, "awvalid", 1, awvalid_o, Output);
        ADD_AXI(AwReady, "awready", 1, awready_i, Input);
        ADD_AXI(WData, "wdata", 32, wdata_o, Output);
        ADD_AXI(WStrb, "wstrb", 4, wstrb_o, Output);
        ADD_AXI(WLast, "wlast", 1, wlast_o, Output);
        ADD_AXI(WUser, "wuser", 2, wuser_o, Output);
        ADD_AXI(WValid, "wvalid", 1, wvalid_o, Output);
        ADD_AXI(WReady, "wready", 1, wready_i, Input);
        ADD_AXI(BId, "bid", 4, bid_i, Input);
        ADD_AXI(BResp, "bresp", 2, bresp_i, Input);
        ADD_AXI(BUser, "buser", 2, buser_i, Input);
        ADD_AXI(BValid, "bvalid", 1, bvalid_i, Input);
        ADD_AXI(BReady, "bready", 1, bready_o, Output);
        ADD_AXI(ArId, "arid", 4, arid_o, Output);
        ADD_AXI(ArAddr, "araddr", 32, araddr_o, Output);
        ADD_AXI(ArLen, "arlen", 8, arlen_o, Output);
        ADD_AXI(ArSize, "arsize", 3, arsize_o, Output);
        ADD_AXI(ArBurst, "arburst", 2, arburst_o, Output);
        ADD_AXI(ArLock, "arlock", 1, arlock_o, Output);
        ADD_AXI(ArCache, "arcache", 4, arcache_o, Output);
        ADD_AXI(ArProt, "arprot", 3, arprot_o, Output);
        ADD_AXI(ArRegion, "arregion", 4, arregion_o, Output);
        ADD_AXI(ArQos, "arqos", 4, arqos_o, Output);
        ADD_AXI(ArUser, "aruser", 2, aruser_o, Output);
        ADD_AXI(ArValid, "arvalid", 1, arvalid_o, Output);
        ADD_AXI(ArReady, "arready", 1, arready_i, Input);
        ADD_AXI(RId, "rid", 4, rid_i, Input);
        ADD_AXI(RData, "rdata", 32, rdata_i, Input);
        ADD_AXI(RResp, "rresp", 2, rresp_i, Input);
        ADD_AXI(RLast, "rlast", 1, rlast_i, Input);
        ADD_AXI(RUser, "ruser", 2, ruser_i, Input);
        ADD_AXI(RValid, "rvalid", 1, rvalid_i, Input);
        ADD_AXI(RReady, "rready", 1, rready_o, Output);
#undef ADD_AXI
#elif defined(RTL_COSIM_AXI_SLAVE_FIXTURE)
#define ADD_AXI(role, name, width, channel, direction)                        \
    addBusSignal(AxiSignal::role, name, width, SignalDirection::direction,    \
                 _top->channel)
        ADD_AXI(AwId, "awid", 4, awid_i, Input);
        ADD_AXI(AwAddr, "awaddr", 32, awaddr_i, Input);
        ADD_AXI(AwLen, "awlen", 8, awlen_i, Input);
        ADD_AXI(AwSize, "awsize", 3, awsize_i, Input);
        ADD_AXI(AwBurst, "awburst", 2, awburst_i, Input);
        ADD_AXI(AwLock, "awlock", 1, awlock_i, Input);
        ADD_AXI(AwCache, "awcache", 4, awcache_i, Input);
        ADD_AXI(AwProt, "awprot", 3, awprot_i, Input);
        ADD_AXI(AwRegion, "awregion", 4, awregion_i, Input);
        ADD_AXI(AwQos, "awqos", 4, awqos_i, Input);
        ADD_AXI(AwUser, "awuser", 2, awuser_i, Input);
        ADD_AXI(AwValid, "awvalid", 1, awvalid_i, Input);
        ADD_AXI(AwReady, "awready", 1, awready_o, Output);
        ADD_AXI(WData, "wdata", 32, wdata_i, Input);
        ADD_AXI(WStrb, "wstrb", 4, wstrb_i, Input);
        ADD_AXI(WLast, "wlast", 1, wlast_i, Input);
        ADD_AXI(WUser, "wuser", 2, wuser_i, Input);
        ADD_AXI(WValid, "wvalid", 1, wvalid_i, Input);
        ADD_AXI(WReady, "wready", 1, wready_o, Output);
        ADD_AXI(BId, "bid", 4, bid_o, Output);
        ADD_AXI(BResp, "bresp", 2, bresp_o, Output);
        ADD_AXI(BUser, "buser", 2, buser_o, Output);
        ADD_AXI(BValid, "bvalid", 1, bvalid_o, Output);
        ADD_AXI(BReady, "bready", 1, bready_i, Input);
        ADD_AXI(ArId, "arid", 4, arid_i, Input);
        ADD_AXI(ArAddr, "araddr", 32, araddr_i, Input);
        ADD_AXI(ArLen, "arlen", 8, arlen_i, Input);
        ADD_AXI(ArSize, "arsize", 3, arsize_i, Input);
        ADD_AXI(ArBurst, "arburst", 2, arburst_i, Input);
        ADD_AXI(ArLock, "arlock", 1, arlock_i, Input);
        ADD_AXI(ArCache, "arcache", 4, arcache_i, Input);
        ADD_AXI(ArProt, "arprot", 3, arprot_i, Input);
        ADD_AXI(ArRegion, "arregion", 4, arregion_i, Input);
        ADD_AXI(ArQos, "arqos", 4, arqos_i, Input);
        ADD_AXI(ArUser, "aruser", 2, aruser_i, Input);
        ADD_AXI(ArValid, "arvalid", 1, arvalid_i, Input);
        ADD_AXI(ArReady, "arready", 1, arready_o, Output);
        ADD_AXI(RId, "rid", 4, rid_o, Output);
        ADD_AXI(RData, "rdata", 32, rdata_o, Output);
        ADD_AXI(RResp, "rresp", 2, rresp_o, Output);
        ADD_AXI(RLast, "rlast", 1, rlast_o, Output);
        ADD_AXI(RUser, "ruser", 2, ruser_o, Output);
        ADD_AXI(RValid, "rvalid", 1, rvalid_o, Output);
        ADD_AXI(RReady, "rready", 1, rready_i, Input);
#undef ADD_AXI
#endif
    }

    std::unique_ptr<VerilatedContext> _context;
    std::unique_ptr<FixtureTop> _top;
    std::unique_ptr<FixtureBus> _bus;
    std::vector<std::unique_ptr<PortSignalBase>> _signals;
    Signal *_resetSignal = nullptr;
    std::string _error;
    bool _terminal = false;
};

class FixtureManager final : public RtlCoreManager
{
  public:
    RtlCore *
    createCore(const char *configJson) noexcept override
    {
        if (!configJson) {
            _error = "configuration JSON is null";
            return nullptr;
        }
        try {
            _error.clear();
            return new FixtureCore();
        } catch (const std::exception &exception) {
            _error = exception.what();
        } catch (...) {
            _error = "unknown fixture construction failure";
        }
        return nullptr;
    }
    void
    destroyCore(RtlCore *core) noexcept override
    {
        delete static_cast<FixtureCore *>(core);
    }
    const char *
    getLastError() const noexcept override
    {
        return _error.c_str();
    }

  private:
    std::string _error;
};

} // anonymous namespace

extern "C" RtlCoreManager *
createRtlCoreManagerV1() noexcept
{
    try {
        return new FixtureManager();
    } catch (...) {
        return nullptr;
    }
}

extern "C" void
destroyRtlCoreManagerV1(RtlCoreManager *manager) noexcept
{
    delete static_cast<FixtureManager *>(manager);
}
