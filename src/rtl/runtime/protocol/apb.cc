/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/runtime/protocol/apb.hh"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

#include "rtl/runtime/signal_access.hh"

namespace gem5::rtl_cosim
{

namespace
{

class ApbBase : public BusTransactor
{
  public:
    explicit ApbBase(const ValidatedBus &bus) : _bus(bus) {}
    const char *
    getLastError() const override
    {
        return _error.c_str();
    }

  protected:
    bool
    read(SignalRoleId role, std::uint64_t &value)
    {
        Signal *signal = _bus.find(role);
        if (!signal) {
            value = 0;
            return true;
        }
        return readSignalU64(*signal, value, _error);
    }

    bool
    readBytes(SignalRoleId role, std::vector<std::uint8_t> &value)
    {
        return readSignal(_bus.require(role), value, _error);
    }

    bool
    write(SignalRoleId role, std::uint64_t value)
    {
        Signal *signal = _bus.find(role);
        return !signal || writeSignalU64(*signal, value, _error);
    }

    bool
    writeBytes(SignalRoleId role, const std::vector<std::uint8_t> &value)
    {
        Signal *signal = _bus.find(role);
        return !signal || writeSignal(*signal, value, _error);
    }

    std::size_t
    dataBytes() const
    {
        return signalBytes(_bus.require(ApbSignal::PWData));
    }

    bool
    readByteEnables(std::vector<std::uint8_t> &enables)
    {
        enables.assign(dataBytes(), 1);
        Signal *signal = _bus.find(ApbSignal::PStrb);
        if (!signal) {
            return true;
        }
        std::vector<std::uint8_t> encoded;
        if (!readSignal(*signal, encoded, _error)) {
            return false;
        }
        for (std::size_t lane = 0; lane < enables.size(); ++lane) {
            enables[lane] = static_cast<std::uint8_t>(
                (encoded[lane / 8] >> (lane % 8)) & 1);
        }
        return true;
    }

    bool
    writeByteEnables(const std::vector<std::uint8_t> &enables)
    {
        Signal *signal = _bus.find(ApbSignal::PStrb);
        if (!signal) {
            if (std::any_of(
                    enables.begin(), enables.end(),
                    [](std::uint8_t enabled) { return enabled == 0; })) {
                _error = "partial APB write requires PSTRB";
                return false;
            }
            return true;
        }
        std::vector<std::uint8_t> encoded(signalBytes(*signal), 0);
        for (std::size_t lane = 0; lane < enables.size(); ++lane) {
            if (enables[lane]) {
                encoded[lane / 8] = static_cast<std::uint8_t>(
                    encoded[lane / 8] |
                    static_cast<std::uint8_t>(1U << (lane % 8)));
            }
        }
        return writeSignal(*signal, encoded, _error);
    }

    bool
    writeZeroStrobe()
    {
        Signal *signal = _bus.find(ApbSignal::PStrb);
        return !signal ||
               writeSignal(*signal,
                           std::vector<std::uint8_t>(signalBytes(*signal), 0),
                           _error);
    }

    ValidatedBus _bus;
    std::string _error;
};

class ApbInitiator : public ApbBase
{
  public:
    ApbInitiator(const ValidatedBus &bus, TransactionBackend &backend,
                 const TransactorLimits &limits)
        : ApbBase(bus), _backend(backend), _nextToken(limits.tokenBase)
    {}

    bool
    beforeClock() override
    {
        const bool responseReady = _response.has_value();
        if (!write(ApbSignal::PReady, responseReady ? 1 : 0)) {
            return false;
        }
        if (responseReady) {
            std::vector<std::uint8_t> data(dataBytes(), 0);
            if (!_response->data.empty()) {
                if (_response->data.size() != data.size()) {
                    return fail(
                        "APB backend returned the wrong read-data size");
                }
                data = _response->data;
            }
            if (!writeBytes(ApbSignal::PRData, data) ||
                !write(ApbSignal::PSlvErr, _response->error ? 1 : 0)) {
                return false;
            }
        } else {
            if (!writeBytes(ApbSignal::PRData,
                            std::vector<std::uint8_t>(dataBytes(), 0)) ||
                !write(ApbSignal::PSlvErr, 0)) {
                return false;
            }
        }

        std::uint64_t select = 0;
        std::uint64_t enable = 0;
        if (!read(ApbSignal::PSel, select) ||
            !read(ApbSignal::PEnable, enable)) {
            return false;
        }
        _complete = select && enable && responseReady;
        if (select && enable && !_accessSeen) {
            MemoryRequest request;
            if (_nextToken == std::numeric_limits<std::uint64_t>::max()) {
                return fail("APB transaction token space exhausted");
            }
            request.token = _nextToken + 1;
            request.beatBytes = dataBytes();
            request.byteEnable.assign(request.beatBytes, 1);
            std::uint64_t writeValue = 0;
            std::uint64_t protection = 0;
            if (!read(ApbSignal::PAddr, request.address) ||
                !read(ApbSignal::PWrite, writeValue) ||
                !read(ApbSignal::PProt, protection)) {
                return false;
            }
            request.write = writeValue != 0;
            if (request.write) {
                if (!readBytes(ApbSignal::PWData, request.data) ||
                    !readByteEnables(request.byteEnable)) {
                    return false;
                }
            }
            if (_backend.canAccept(request)) {
                if (!_backend.submit(request)) {
                    return fail(
                        "APB backend rejected a request after accepting it");
                }
                _nextToken = request.token;
                _pendingToken = request.token;
                _accessSeen = true;
            }
        }
        return true;
    }

    bool
    afterClock() override
    {
        if (_complete) {
            _response.reset();
            _pendingToken = 0;
            _accessSeen = false;
            _complete = false;
        }
        MemoryResponse response;
        while (_backend.getResponse(response)) {
            if (!_accessSeen || response.token != _pendingToken) {
                return fail("APB backend returned an unexpected token");
            }
            if (_response) {
                return fail("APB backend returned more than one response");
            }
            _response = std::move(response);
        }
        return true;
    }

    bool
    isIdle() const override
    {
        return !_accessSeen && !_response;
    }

  private:
    bool
    fail(const char *message)
    {
        _error = message;
        return false;
    }

    TransactionBackend &_backend;
    std::uint64_t _nextToken;
    std::uint64_t _pendingToken = 0;
    bool _accessSeen = false;
    bool _complete = false;
    std::optional<MemoryResponse> _response;
};

class ApbTarget : public ApbBase
{
  public:
    ApbTarget(const ValidatedBus &bus, TransactionSource &source,
              const TransactorLimits &limits)
        : ApbBase(bus), _source(source), _limits(limits)
    {}

    bool
    beforeClock() override
    {
        if (!_request && !_response) {
            MemoryRequest candidate;
            if (_source.getRequest(candidate)) {
                std::string reason;
                if (!candidate.valid(reason)) {
                    return fail("invalid APB source request: " + reason);
                }
                if (candidate.beatCount() != 1 ||
                    candidate.beatBytes != dataBytes()) {
                    return fail(
                        "APB requires one full-width beat per request");
                }
                _request = std::move(candidate);
                _phase = Phase::Setup;
            }
        }

        if (!_request) {
            if (!driveIdle()) {
                return false;
            }
        } else {
            if (!driveRequest()) {
                return false;
            }
        }

        _complete = false;
        if (_request && _phase == Phase::Access) {
            std::uint64_t ready = 0;
            if (!read(ApbSignal::PReady, ready)) {
                return false;
            }
            if (ready) {
                MemoryResponse response;
                response.token = _request->token;
                response.id = _request->id;
                std::uint64_t error = 0;
                if (!read(ApbSignal::PSlvErr, error)) {
                    return false;
                }
                response.error = error != 0;
                if (!_request->write &&
                    !readBytes(ApbSignal::PRData, response.data)) {
                    return false;
                }
                _response = std::move(response);
                _complete = true;
            }
        }
        return true;
    }

    bool
    afterClock() override
    {
        if (_request && _phase == Phase::Setup) {
            _phase = Phase::Access;
        }
        if (_complete) {
            _request.reset();
            _phase = Phase::Idle;
            _complete = false;
        }
        if (_response && _source.canAcceptResponse(*_response)) {
            if (!_source.submitResponse(*_response)) {
                return fail(
                    "APB source rejected a response after accepting it");
            }
            _response.reset();
        }
        return true;
    }

    bool
    isIdle() const override
    {
        return !_request && !_response;
    }

  private:
    enum class Phase
    {
        Idle,
        Setup,
        Access
    };

    bool
    fail(const std::string &message)
    {
        _error = message;
        return false;
    }

    bool
    driveIdle()
    {
        return write(ApbSignal::PSel, 0) && write(ApbSignal::PEnable, 0) &&
               write(ApbSignal::PWrite, 0) && write(ApbSignal::PAddr, 0) &&
               writeBytes(ApbSignal::PWData,
                          std::vector<std::uint8_t>(dataBytes(), 0)) &&
               writeZeroStrobe() && write(ApbSignal::PProt, 0);
    }

    bool
    driveRequest()
    {
        const MemoryRequest &request = *_request;
        if (!write(ApbSignal::PSel, 1) ||
            !write(ApbSignal::PEnable, _phase == Phase::Access) ||
            !write(ApbSignal::PWrite, request.write ? 1 : 0) ||
            !write(ApbSignal::PAddr, request.address) ||
            !write(ApbSignal::PProt, 0)) {
            return false;
        }
        std::vector<std::uint8_t> data(dataBytes(), 0);
        if (request.write) {
            data = request.data;
        }
        if (!writeBytes(ApbSignal::PWData, data)) {
            return false;
        }
        std::vector<std::uint8_t> enables(dataBytes(), 0);
        if (request.write) {
            enables = request.byteEnable;
        }
        return writeByteEnables(enables);
    }

    TransactionSource &_source;
    TransactorLimits _limits;
    Phase _phase = Phase::Idle;
    bool _complete = false;
    std::optional<MemoryRequest> _request;
    std::optional<MemoryResponse> _response;
};

} // anonymous namespace

std::unique_ptr<BusTransactor>
createApbInitiatorTransactor(const ValidatedBus &bus,
                             TransactionBackend &backend,
                             const TransactorLimits &limits)
{
    return std::make_unique<ApbInitiator>(bus, backend, limits);
}

std::unique_ptr<BusTransactor>
createApbTargetTransactor(const ValidatedBus &bus, TransactionSource &source,
                          const TransactorLimits &limits)
{
    return std::make_unique<ApbTarget>(bus, source, limits);
}

} // namespace gem5::rtl_cosim
