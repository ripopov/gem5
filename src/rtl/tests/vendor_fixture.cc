/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gem5/rtl_cosim/api_v1.hh"

namespace
{

class FixtureSignal final : public Signal
{
  public:
    FixtureSignal(std::string name, std::size_t width,
                  SignalDirection direction)
        : _name(std::move(name)),
          _width(width),
          _direction(direction),
          _value((_width + 7) / 8, 0)
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
        if (!data || size != _value.size()) {
            return false;
        }
        std::copy(_value.begin(), _value.end(), data);
        return true;
    }

    bool
    setValue(const std::uint8_t *data, std::size_t size) noexcept override
    {
        if (_direction != SignalDirection::Input || !data ||
            size != _value.size()) {
            return false;
        }
        if (_width % 8 != 0 && (data[size - 1] >> (_width % 8)) != 0) {
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

    bool
    low() const noexcept
    {
        return (_value[0] & 1) == 0;
    }

    void
    drive(bool high) noexcept
    {
        const std::uint8_t value = high ? 1 : 0;
        if (_value[0] == value) {
            return;
        }
        _value[0] = value;
        if (_callback) {
            _callback->update();
        }
    }

  private:
    std::string _name;
    std::size_t _width;
    SignalDirection _direction;
    std::vector<std::uint8_t> _value;
    SignalChangeCallback *_callback = nullptr;
};

class FixtureCore final : public RtlCore
{
  public:
    FixtureCore()
        : _reset("reset_n", 1, SignalDirection::Input),
          _interrupt("interrupt", 1, SignalDirection::Output),
          _vectorInput("vector_input", 72, SignalDirection::Input)
    {}

    const char *
    name() const noexcept override
    {
        return "cpp-vendor-fixture";
    }
    std::size_t
    busCount() const noexcept override
    {
        return 0;
    }
    Bus *
    bus(std::size_t) noexcept override
    {
        return nullptr;
    }
    std::size_t
    signalCount() const noexcept override
    {
        return 3;
    }

    CoreSignalBinding
    signal(std::size_t index) noexcept override
    {
        switch (index) {
            case 0:
                return {CoreSignalRole::Reset, 0, ActiveLevel::Low, &_reset};
            case 1:
                return {CoreSignalRole::Interrupt, 0, ActiveLevel::High,
                        &_interrupt};
            case 2:
                return {CoreSignalRole::Io, 0, ActiveLevel::High,
                        &_vectorInput};
            default:
                return {CoreSignalRole::Io, 0, ActiveLevel::High, nullptr};
        }
    }

    std::size_t
    memoryCount() const noexcept override
    {
        return 1;
    }

    const MemoryRegion *
    memory(std::size_t index) const noexcept override
    {
        return index == 0 ? &_memoryRegion : nullptr;
    }

    bool
    readMemory(std::size_t index, std::uint64_t offset, std::uint8_t *data,
               std::size_t size) const noexcept override
    {
        if (index != 0 || !data || offset > _memory.size() ||
            size > _memory.size() - offset) {
            return false;
        }
        std::copy_n(_memory.begin() + static_cast<std::ptrdiff_t>(offset),
                    size, data);
        return true;
    }

    bool
    writeMemory(std::size_t index, std::uint64_t offset,
                const std::uint8_t *data, std::size_t size) noexcept override
    {
        if (index != 0 || !data || offset > _memory.size() ||
            size > _memory.size() - offset) {
            return false;
        }
        std::copy_n(data, size,
                    _memory.begin() + static_cast<std::ptrdiff_t>(offset));
        return true;
    }

    ClockResult
    clock() noexcept override
    {
        if (_terminal) {
            _error = "clock called after terminal state";
            return ClockResult::Error;
        }
        if (_reset.low()) {
            _interrupt.drive(false);
            _ran = false;
            return ClockResult::Completed;
        }
        if (!_ran) {
            _interrupt.drive(true);
            _ran = true;
        }
        return ClockResult::Completed;
    }

    bool
    isIdle() const noexcept override
    {
        return _ran;
    }
    const char *
    getLastError() const noexcept override
    {
        return _error.c_str();
    }

  private:
    FixtureSignal _reset;
    FixtureSignal _interrupt;
    FixtureSignal _vectorInput;
    std::array<std::uint8_t, 64> _memory{};
    MemoryRegion _memoryRegion{"code_tcm", MemoryRole::CodeTcm, 0x8000, 64};
    std::string _error;
    bool _ran = false;
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
        } catch (...) {
            _error = "fixture allocation failed";
            return nullptr;
        }
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
