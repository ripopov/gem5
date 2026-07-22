/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/checker/memory_backend.hh"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gem5::rtl_cosim
{

namespace
{

std::string
hexBytes(const std::vector<std::uint8_t> &bytes)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const std::uint8_t byte : bytes) {
        stream << std::setw(2) << static_cast<unsigned>(byte);
    }
    return stream.str();
}

} // anonymous namespace

SparseMemory::SparseMemory(std::uint64_t base, std::uint64_t size)
    : _base(base), _size(size)
{
    if (size == 0 || size > std::numeric_limits<std::uint64_t>::max() - base) {
        throw std::invalid_argument("invalid sparse-memory address range");
    }
}

bool
SparseMemory::contains(std::uint64_t address, std::size_t size) const noexcept
{
    if (address < _base || size > _size) {
        return false;
    }
    return address - _base <= _size - size;
}

bool
SparseMemory::read(std::uint64_t address, std::uint8_t *data,
                   std::size_t size) const noexcept
{
    if (!data || !contains(address, size)) {
        return false;
    }
    for (std::size_t index = 0; index < size; ++index) {
        const auto it = _bytes.find(address + index);
        data[index] = it == _bytes.end() ? 0 : it->second;
    }
    return true;
}

bool
SparseMemory::write(std::uint64_t address, const std::uint8_t *data,
                    const std::uint8_t *enable, std::size_t size) noexcept
{
    if (!data || !contains(address, size)) {
        return false;
    }
    for (std::size_t index = 0; index < size; ++index) {
        if (!enable || enable[index]) {
            _bytes[address + index] = data[index];
        }
    }
    return true;
}

MemoryBackend::MemoryBackend(std::shared_ptr<SparseMemory> memory,
                             MemoryBackendConfig config)
    : _memory(std::move(memory)), _config(std::move(config))
{
    if (!_memory) {
        throw std::invalid_argument("memory backend requires storage");
    }
    if (_config.maxPending == 0) {
        throw std::invalid_argument(
            "memory backend maxPending must be positive");
    }
}

bool
MemoryBackend::canAccept(const MemoryRequest &request) const
{
    std::string error;
    return _pending.size() < _config.maxPending && request.valid(error);
}

bool
MemoryBackend::submit(const MemoryRequest &request)
{
    if (!canAccept(request)) {
        return false;
    }
    _pending.push_back({_cycle + _config.latencyCycles, request});
    ++_requests;
    if (request.write) {
        ++_writes;
    } else {
        ++_reads;
    }
    return true;
}

bool
MemoryBackend::getResponse(MemoryResponse &response)
{
    if (_responses.empty()) {
        return false;
    }
    response = std::move(_responses.front());
    _responses.pop_front();
    return true;
}

namespace
{

std::uint64_t
transactionBeatAddress(const MemoryRequest &request, std::size_t beat)
{
    if (request.burst == BurstType::Fixed) {
        return request.address;
    }
    const std::uint64_t offset = beat * request.beatBytes;
    if (request.burst == BurstType::Increment) {
        return request.address + offset;
    }
    const std::uint64_t span = request.beatCount() * request.beatBytes;
    const std::uint64_t base = request.address / span * span;
    return base + (request.address - base + offset) % span;
}

} // anonymous namespace

MemoryResponse
MemoryBackend::execute(const MemoryRequest &request)
{
    MemoryResponse response;
    response.token = request.token;
    response.id = request.id;
    if (!request.write) {
        response.data.resize(request.beatCount() * request.beatBytes);
    }
    const bool exclusiveWriteOkay = request.exclusive && request.write &&
        _exclusiveAddress && *_exclusiveAddress == request.address;
    for (std::size_t beat = 0; beat < request.beatCount(); ++beat) {
        const std::uint64_t address = transactionBeatAddress(request, beat);
        if (!_memory->contains(address, request.beatBytes) ||
            errorsAt(address, request.beatBytes)) {
            response.error = true;
            continue;
        }
        const std::size_t offset = beat * request.beatBytes;
        if (request.write) {
            if (!request.exclusive || exclusiveWriteOkay) {
                _memory->write(address, request.data.data() + offset,
                               request.byteEnable.data() + offset,
                               request.beatBytes);
            }
        } else {
            _memory->read(address, response.data.data() + offset,
                          request.beatBytes);
        }
    }
    if (request.exclusive && !response.error) {
        response.exclusiveOkay = !request.write || exclusiveWriteOkay;
    }
    if (request.exclusive && !request.write && !response.error) {
        _exclusiveAddress = request.address;
    } else if (request.write) {
        _exclusiveAddress.reset();
    }
    return response;
}

bool
MemoryBackend::errorsAt(std::uint64_t address, std::size_t size) const noexcept
{
    for (const auto &[base, rangeSize] : _config.errorRanges) {
        if (rangeSize == 0) {
            continue;
        }
        const std::uint64_t end =
            address > std::numeric_limits<std::uint64_t>::max() - size
                ? std::numeric_limits<std::uint64_t>::max()
                : address + size;
        const std::uint64_t rangeEnd =
            base > std::numeric_limits<std::uint64_t>::max() - rangeSize
                ? std::numeric_limits<std::uint64_t>::max()
                : base + rangeSize;
        if (address < rangeEnd && base < end) {
            return true;
        }
    }
    return false;
}

void
MemoryBackend::advance()
{
    ++_cycle;
    while (!_pending.empty() && _pending.front().readyCycle <= _cycle) {
        _responses.push_back(execute(_pending.front().request));
        _pending.pop_front();
    }
}

bool
MemoryBackend::isIdle() const noexcept
{
    return _pending.empty() && _responses.empty();
}

ScriptedTransactionSource::ScriptedTransactionSource(
    std::size_t maxOutstanding)
    : _maxOutstanding(maxOutstanding)
{
    if (maxOutstanding == 0) {
        throw std::invalid_argument("source maxOutstanding must be positive");
    }
}

void
ScriptedTransactionSource::add(MemoryRequest request)
{
    add(std::move(request), Expectation{});
}

void
ScriptedTransactionSource::add(MemoryRequest request, Expectation expectation)
{
    _requests.push_back({std::move(request), std::move(expectation)});
}

bool
ScriptedTransactionSource::getRequest(MemoryRequest &request)
{
    if (_requests.empty() || _outstanding.size() >= _maxOutstanding ||
        !_error.empty()) {
        return false;
    }
    Queued queued = std::move(_requests.front());
    _requests.pop_front();
    request = std::move(queued.request);
    const auto [position, inserted] = _outstanding.try_emplace(request.token);
    if (!inserted) {
        _error =
            "duplicate transaction token " + std::to_string(request.token);
        return false;
    }
    position->second.id = request.id;
    position->second.write = request.write;
    position->second.responseBytes =
        request.write ? 0 : request.beatCount() * request.beatBytes;
    position->second.expectation = std::move(queued.expectation);
    ++_submitted;
    if (request.write) {
        ++_writes;
    } else {
        ++_reads;
    }
    return true;
}

bool
ScriptedTransactionSource::canAcceptResponse(const MemoryResponse &) const
{
    return _error.empty();
}

bool
ScriptedTransactionSource::submitResponse(const MemoryResponse &response)
{
    if (!canAcceptResponse(response)) {
        return false;
    }
    const auto it = _outstanding.find(response.token);
    if (it == _outstanding.end()) {
        _error = "response has unknown or duplicate token " +
                 std::to_string(response.token);
        return false;
    }
    if (response.id != it->second.id) {
        _error = "response ID does not match request for token " +
                 std::to_string(response.token);
        return false;
    }
    if (response.data.size() != it->second.responseBytes) {
        _error = "response data length does not match request for token " +
                 std::to_string(response.token);
        return false;
    }
    if (response.error != it->second.expectation.error) {
        _error =
            "response error status does not match expectation for token " +
            std::to_string(response.token);
        return false;
    }
    if (response.exclusiveOkay !=
        it->second.expectation.exclusiveOkay) {
        _error = "response exclusive status does not match expectation for "
                 "token " + std::to_string(response.token);
        return false;
    }
    if (it->second.expectation.data &&
        response.data != *it->second.expectation.data) {
        _error = "response data does not match expectation for token " +
                 std::to_string(response.token) + ": expected " +
                 hexBytes(*it->second.expectation.data) + ", received " +
                 hexBytes(response.data);
        return false;
    }
    _outstanding.erase(it);
    ++_completed;
    _responses.push_back(response);
    return true;
}

void
ScriptedTransactionSource::advance()
{}

bool
ScriptedTransactionSource::getCompleted(MemoryResponse &response)
{
    if (_responses.empty()) {
        return false;
    }
    response = std::move(_responses.front());
    _responses.pop_front();
    return true;
}

bool
ScriptedTransactionSource::isIdle() const noexcept
{
    return _requests.empty() && _outstanding.empty();
}

} // namespace gem5::rtl_cosim
