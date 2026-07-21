/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_COSIM_TESTS_TEST_MODEL_HH__
#define __RTL_COSIM_TESTS_TEST_MODEL_HH__

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gem5/rtl_cosim/api_v1.hh"
#include "rtl/runtime/model_validator.hh"

namespace gem5::rtl_cosim::test
{

class TestSignal : public Signal
{
  public:
    TestSignal(std::string name, std::size_t width, SignalDirection direction)
        : _name(std::move(name)),
          _width(width),
          _direction(direction),
          _value((width + 7) / 8)
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
        ++getCount;
        if (!data || size != _value.size()) {
            return false;
        }
        std::copy(_value.begin(), _value.end(), data);
        return true;
    }
    bool
    setValue(const std::uint8_t *data, std::size_t size) noexcept override
    {
        ++setCount;
        if (_direction != SignalDirection::Input || !data ||
            size != _value.size()) {
            return false;
        }
        std::copy_n(data, size, _value.begin());
        return true;
    }
    void
    setChangeCallback(SignalChangeCallback *callback) noexcept override
    {
        _callback = callback;
    }

    void
    drive(std::uint64_t value)
    {
        std::vector<std::uint8_t> next(_value.size());
        for (std::size_t index = 0; index < next.size(); ++index) {
            next[index] = static_cast<std::uint8_t>(value >> (index * 8));
        }
        drive(next);
    }

    void
    drive(const std::vector<std::uint8_t> &value)
    {
        if (value != _value) {
            _value = value;
            if (_direction == SignalDirection::Output && _callback) {
                _callback->update();
            }
        }
    }

    std::uint64_t
    value() const
    {
        std::uint64_t result = 0;
        for (std::size_t index = 0;
             index < std::min<std::size_t>(_value.size(), 8); ++index) {
            result |= std::uint64_t{_value[index]} << (index * 8);
        }
        return result;
    }

    void
    setWidthForTest(std::size_t width)
    {
        _width = width;
        _value.resize((width + 7) / 8);
    }

    void
    setDirectionForTest(SignalDirection direction)
    {
        _direction = direction;
    }

    mutable std::size_t getCount = 0;
    std::size_t setCount = 0;

  private:
    std::string _name;
    std::size_t _width;
    SignalDirection _direction;
    std::vector<std::uint8_t> _value;
    SignalChangeCallback *_callback = nullptr;
};

class TestBus : public Bus
{
  public:
    TestBus(std::string name, BusProtocol protocol, BusRole role)
        : _name(std::move(name)), _protocol(protocol), _role(role)
    {}

    const char *
    name() const noexcept override
    {
        return _name.c_str();
    }
    BusProtocol
    protocol() const noexcept override
    {
        return _protocol;
    }
    BusRole
    role() const noexcept override
    {
        return _role;
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

    TestSignal &
    add(SignalRoleId role, std::size_t width, SignalDirection direction)
    {
        auto signal = std::make_unique<TestSignal>("s" + std::to_string(role),
                                                   width, direction);
        TestSignal &result = *signal;
        _signals.emplace_back(std::move(signal));
        _bindings.push_back({role, &result});
        return result;
    }

    TestSignal &
    get(SignalRoleId role)
    {
        auto it = std::find_if(_bindings.begin(), _bindings.end(),
                               [=](const SignalBinding &binding) {
                                   return binding.role == role;
                               });
        return *static_cast<TestSignal *>(it->signal);
    }

    void
    duplicate(SignalRoleId role)
    {
        _bindings.push_back({role, _bindings.front().signal});
    }

    void
    remove(SignalRoleId role)
    {
        _bindings.erase(std::remove_if(_bindings.begin(), _bindings.end(),
                                       [=](const SignalBinding &binding) {
                                           return binding.role == role;
                                       }),
                        _bindings.end());
    }

  private:
    std::string _name;
    BusProtocol _protocol;
    BusRole _role;
    std::vector<std::unique_ptr<TestSignal>> _signals;
    std::vector<SignalBinding> _bindings;
};

inline bool
isRequest(BusProtocol protocol, SignalRoleId role)
{
    if (protocol == BusProtocol::Apb) {
        return role <= ApbSignal::PProt;
    }
    if ((role >= AxiSignal::AwId && role <= AxiSignal::AwValid) ||
        (role >= AxiSignal::WId && role <= AxiSignal::WValid) ||
        role == AxiSignal::BReady ||
        (role >= AxiSignal::ArId && role <= AxiSignal::ArValid) ||
        role == AxiSignal::RReady ||
        (role >= AxiSignal::AwRegion && role <= AxiSignal::WUser) ||
        (role >= AxiSignal::ArRegion && role <= AxiSignal::ArUser) ||
        (role >= AxiSignal::AwDomain && role <= AxiSignal::ArBar) ||
        role == AxiSignal::AcReady ||
        (role >= AxiSignal::CrResp && role <= AxiSignal::CrValid) ||
        (role >= AxiSignal::CdData && role <= AxiSignal::CdValid) ||
        role == AxiSignal::Rack || role == AxiSignal::Wack) {
        return true;
    }
    return false;
}

inline SignalDirection
direction(BusProtocol protocol, BusRole busRole, SignalRoleId role)
{
    const bool output = busRole == BusRole::Initiator
                            ? isRequest(protocol, role)
                            : !isRequest(protocol, role);
    return output ? SignalDirection::Output : SignalDirection::Input;
}

inline std::size_t
axiWidth(BusProtocol protocol, SignalRoleId role, std::size_t dataWidth = 32)
{
    if (role == AxiSignal::AwAddr || role == AxiSignal::ArAddr ||
        role == AxiSignal::AcAddr) {
        return 32;
    }
    if (role == AxiSignal::WData || role == AxiSignal::RData ||
        role == AxiSignal::CdData) {
        return dataWidth;
    }
    if (role == AxiSignal::WStrb) {
        return dataWidth / 8;
    }
    if (role == AxiSignal::AwLen || role == AxiSignal::ArLen) {
        return protocol == BusProtocol::Axi4 ? 8 : 4;
    }
    if (role == AxiSignal::AwSize || role == AxiSignal::ArSize ||
        role == AxiSignal::AwProt || role == AxiSignal::ArProt ||
        role == AxiSignal::AwSnoop || role == AxiSignal::AcProt) {
        return 3;
    }
    if (role == AxiSignal::AwBurst || role == AxiSignal::ArBurst ||
        role == AxiSignal::BResp || role == AxiSignal::RResp ||
        role == AxiSignal::AwDomain || role == AxiSignal::AwBar ||
        role == AxiSignal::ArDomain || role == AxiSignal::ArBar) {
        return 2;
    }
    if (role == AxiSignal::AwCache || role == AxiSignal::ArCache ||
        role == AxiSignal::AwRegion || role == AxiSignal::AwQos ||
        role == AxiSignal::ArRegion || role == AxiSignal::ArQos ||
        role == AxiSignal::ArSnoop || role == AxiSignal::AcSnoop) {
        return 4;
    }
    if (role == AxiSignal::CrResp) {
        return 5;
    }
    if (role == AxiSignal::AwId || role == AxiSignal::WId ||
        role == AxiSignal::BId || role == AxiSignal::ArId ||
        role == AxiSignal::RId) {
        return 4;
    }
    if (role == AxiSignal::AwUser || role == AxiSignal::WUser ||
        role == AxiSignal::BUser || role == AxiSignal::ArUser ||
        role == AxiSignal::RUser) {
        return 2;
    }
    if (role == AxiSignal::AwLock || role == AxiSignal::ArLock) {
        return protocol == BusProtocol::Axi4 ? 1 : 2;
    }
    return 1;
}

inline std::unique_ptr<TestBus>
makeApb(BusRole role, std::size_t dataWidth = 32)
{
    auto bus = std::make_unique<TestBus>("apb", BusProtocol::Apb, role);
    for (SignalRoleId signal = ApbSignal::PAddr; signal <= ApbSignal::PSlvErr;
         ++signal) {
        std::size_t width = 1;
        if (signal == ApbSignal::PAddr) {
            width = 32;
        }
        if (signal == ApbSignal::PWData || signal == ApbSignal::PRData) {
            width = dataWidth;
        }
        if (signal == ApbSignal::PStrb) {
            width = dataWidth / 8;
        }
        if (signal == ApbSignal::PProt) {
            width = 3;
        }
        bus->add(signal, width, direction(BusProtocol::Apb, role, signal));
    }
    return bus;
}

inline std::unique_ptr<TestBus>
makeAxi(BusRole role, BusProtocol protocol = BusProtocol::Axi4,
        std::size_t dataWidth = 32)
{
    auto bus = std::make_unique<TestBus>("axi", protocol, role);
    for (SignalRoleId signal = AxiSignal::AwId; signal <= AxiSignal::RReady;
         ++signal) {
        if (protocol == BusProtocol::Axi4 && signal == AxiSignal::WId) {
            continue;
        }
        bus->add(signal, axiWidth(protocol, signal, dataWidth),
                 direction(protocol, role, signal));
    }
    if (protocol == BusProtocol::Axi3Ace) {
        for (SignalRoleId signal = AxiSignal::AwDomain;
             signal <= AxiSignal::Wack; ++signal) {
            bus->add(signal, axiWidth(protocol, signal, dataWidth),
                     direction(protocol, role, signal));
        }
    }
    return bus;
}

class TestCore : public RtlCore
{
  public:
    const char *
    name() const noexcept override
    {
        return "test-core";
    }
    std::size_t
    busCount() const noexcept override
    {
        return buses.size();
    }
    Bus *
    bus(std::size_t index) noexcept override
    {
        return index < buses.size() ? buses[index].get() : nullptr;
    }
    std::size_t
    signalCount() const noexcept override
    {
        return coreBindings.size();
    }
    CoreSignalBinding
    signal(std::size_t index) noexcept override
    {
        return index < coreBindings.size()
                   ? coreBindings[index]
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
        return true;
    }
    ClockResult
    clock() noexcept override
    {
        return ClockResult::Completed;
    }
    bool
    isIdle() const noexcept override
    {
        return true;
    }
    const char *
    getLastError() const noexcept override
    {
        return "";
    }

    TestSignal &
    addCoreSignal(std::string name, CoreSignalRole role, std::uint32_t index,
                  ActiveLevel activeLevel, std::size_t width,
                  SignalDirection direction)
    {
        auto signal =
            std::make_unique<TestSignal>(std::move(name), width, direction);
        TestSignal &result = *signal;
        coreSignals.push_back(std::move(signal));
        coreBindings.push_back({role, index, activeLevel, &result});
        return result;
    }

    std::vector<std::unique_ptr<TestBus>> buses;
    std::vector<std::unique_ptr<TestSignal>> coreSignals;
    std::vector<CoreSignalBinding> coreBindings;
};

inline ValidatedBus
validated(TestCore &core)
{
    ValidationResult result = validateModel(core);
    if (!result.ok()) {
        throw std::runtime_error(result.errors.front());
    }
    return result.buses.front();
}

} // namespace gem5::rtl_cosim::test

#endif // __RTL_COSIM_TESTS_TEST_MODEL_HH__
