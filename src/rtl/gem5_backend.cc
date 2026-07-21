/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/gem5_backend.hh"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "base/logging.hh"
#include "mem/packet.hh"

namespace gem5::rtl_cosim
{

namespace
{

Addr
beatAddress(const MemoryRequest &request, std::size_t beat)
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

Gem5InitiatorBackend::InitiatorPort::InitiatorPort(
    const std::string &name, PortID id, Gem5InitiatorBackend &owner)
    : RequestPort(name, id), owner(owner)
{}

bool
Gem5InitiatorBackend::InitiatorPort::recvTimingResp(PacketPtr packet)
{
    return owner.receiveTimingResponse(packet);
}

void
Gem5InitiatorBackend::InitiatorPort::recvReqRetry()
{
    owner.retryRequest();
}

Gem5InitiatorBackend::Gem5InitiatorBackend(
    const std::string &name, PortID id, RequestorID requestorId,
    std::size_t maxPending, std::function<void()> wakeup)
    : _port(name, id, *this), _requestorId(requestorId),
      _maxPending(maxPending), _wakeup(std::move(wakeup))
{
    fatal_if(maxPending == 0,
             "%s: max pending transactions must be positive", name);
}

bool
Gem5InitiatorBackend::canAccept(const MemoryRequest &request) const
{
    std::string error;
    return request.valid(error) &&
           _transactions.size() < _maxPending &&
           !_transactions.contains(request.token);
}

PacketPtr
Gem5InitiatorBackend::makePacket(const MemoryRequest &request,
                                 std::size_t beat)
{
    const auto size = static_cast<unsigned>(request.beatBytes);
    auto gem5Request = std::make_shared<Request>(
        beatAddress(request, beat), size, Request::Flags(), _requestorId);
    if (request.write) {
        std::vector<bool> enables(request.beatBytes);
        const std::size_t offset = beat * request.beatBytes;
        std::transform(request.byteEnable.begin() + offset,
                       request.byteEnable.begin() + offset + request.beatBytes,
                       enables.begin(),
                       [](std::uint8_t value) { return value != 0; });
        gem5Request->setByteEnable(enables);
    }

    auto *packet = new Packet(
        gem5Request, request.write ? MemCmd::WriteReq : MemCmd::ReadReq);
    packet->allocate();
    std::fill(packet->getPtr<std::uint8_t>(),
              packet->getPtr<std::uint8_t>() + request.beatBytes, 0);
    if (request.write) {
        const std::size_t offset = beat * request.beatBytes;
        packet->writeData(request.data.data() + offset);
    }
    packet->pushSenderState(new PacketState(request.token, beat));
    return packet;
}

bool
Gem5InitiatorBackend::submit(const MemoryRequest &request)
{
    if (!canAccept(request) ||
        request.beatBytes > std::numeric_limits<unsigned>::max()) {
        return false;
    }

    PendingTransaction pending;
    pending.request = request;
    pending.response.token = request.token;
    pending.response.id = request.id;
    if (!request.write) {
        pending.response.data.assign(
            request.beatCount() * request.beatBytes, 0);
    }
    _transactions.emplace(request.token, std::move(pending));
    for (std::size_t beat = 0; beat < request.beatCount(); ++beat) {
        _requests.push_back(makePacket(request, beat));
    }
    return true;
}

bool
Gem5InitiatorBackend::getResponse(MemoryResponse &response)
{
    if (_responses.empty()) {
        return false;
    }
    response = std::move(_responses.front());
    _responses.pop_front();
    return true;
}

void
Gem5InitiatorBackend::pump()
{
    while (!_waitingForRetry && !_requests.empty()) {
        PacketPtr packet = _requests.front();
        if (!_port.sendTimingReq(packet)) {
            _waitingForRetry = true;
            return;
        }
        _requests.pop_front();
    }
}

void
Gem5InitiatorBackend::advance()
{
    pump();
}

void
Gem5InitiatorBackend::retryRequest()
{
    panic_if(!_waitingForRetry, "%s received an unexpected request retry",
             _port.name());
    _waitingForRetry = false;
    pump();
    _wakeup();
}

bool
Gem5InitiatorBackend::receiveTimingResponse(PacketPtr packet)
{
    auto *state = dynamic_cast<PacketState *>(packet->senderState);
    panic_if(!state, "%s received a packet without RTL sender state",
             _port.name());
    state = static_cast<PacketState *>(packet->popSenderState());
    const std::uint64_t token = state->token;
    const std::size_t beat = state->beat;
    delete state;

    auto transaction = _transactions.find(token);
    panic_if(transaction == _transactions.end(),
             "%s received a packet with unknown RTL token %llu",
             _port.name(), static_cast<unsigned long long>(token));
    PendingTransaction &pending = transaction->second;
    panic_if(beat >= pending.request.beatCount(),
             "%s received an invalid RTL beat index", _port.name());

    pending.response.error |= packet->isError();
    if (!pending.request.write && !packet->isError()) {
        const std::size_t offset = beat * pending.request.beatBytes;
        std::copy(packet->getConstPtr<std::uint8_t>(),
                  packet->getConstPtr<std::uint8_t>() +
                      pending.request.beatBytes,
                  pending.response.data.begin() + offset);
    }
    ++pending.completedBeats;
    delete packet;

    if (pending.completedBeats == pending.request.beatCount()) {
        _responses.push_back(std::move(pending.response));
        _transactions.erase(transaction);
    }
    _wakeup();
    return true;
}

bool
Gem5InitiatorBackend::isIdle() const noexcept
{
    return _transactions.empty() && _requests.empty() && _responses.empty() &&
           !_waitingForRetry;
}

void
Gem5InitiatorBackend::sendFunctional(Addr address,
                                     const std::uint8_t *data,
                                     std::size_t size)
{
    constexpr std::size_t MaxChunk = 64 * 1024;
    while (size != 0) {
        const std::size_t chunk = std::min(size, MaxChunk);
        auto request = std::make_shared<Request>(
            address, static_cast<unsigned>(chunk), Request::Flags(),
            _requestorId);
        Packet packet(request, MemCmd::WriteReq);
        packet.dataStaticConst(data);
        _port.sendFunctional(&packet);
        fatal_if(packet.isError(),
                 "%s: functional image write failed at %#llx", _port.name(),
                 static_cast<unsigned long long>(address));
        address += chunk;
        data += chunk;
        size -= chunk;
    }
}

Gem5TargetSource::TargetPort::TargetPort(const std::string &name, PortID id,
                                         Gem5TargetSource &owner)
    : ResponsePort(name, id), owner(owner)
{}

Tick
Gem5TargetSource::TargetPort::recvAtomic(PacketPtr packet)
{
    owner.respondUnsupported(packet);
    return 0;
}

bool
Gem5TargetSource::TargetPort::recvTimingReq(PacketPtr packet)
{
    return owner.receiveTimingRequest(packet);
}

void
Gem5TargetSource::TargetPort::recvRespRetry()
{
    owner.receiveResponseRetry();
}

void
Gem5TargetSource::TargetPort::recvFunctional(PacketPtr packet)
{
    owner.respondUnsupported(packet);
}

AddrRangeList
Gem5TargetSource::TargetPort::getAddrRanges() const
{
    return {owner.range()};
}

Gem5TargetSource::Gem5TargetSource(const std::string &name, PortID id,
                                   AddrRange range, std::size_t maxPending,
                                   std::function<void()> wakeup)
    : _port(name, id, *this), _range(std::move(range)),
      _maxPending(maxPending), _wakeup(std::move(wakeup))
{
    fatal_if(maxPending == 0,
             "%s: max pending transactions must be positive", name);
}

bool
Gem5TargetSource::receiveTimingRequest(PacketPtr packet)
{
    if ((!packet->isRead() && !packet->isWrite()) ||
        _packets.size() >= _maxPending ||
        _nextToken == std::numeric_limits<std::uint64_t>::max()) {
        _retryRequest = true;
        return false;
    }

    MemoryRequest request;
    request.token = ++_nextToken;
    request.address = packet->getAddr();
    request.write = packet->isWrite();
    request.beatBytes = packet->getSize();
    request.byteEnable.assign(request.beatBytes, 1);
    const auto &gem5Enable = packet->req->getByteEnable();
    if (gem5Enable.size() == request.byteEnable.size()) {
        std::transform(gem5Enable.begin(), gem5Enable.end(),
                       request.byteEnable.begin(),
                       [](bool value) { return value ? 1 : 0; });
    }
    if (request.write) {
        request.data.assign(packet->getConstPtr<std::uint8_t>(),
                            packet->getConstPtr<std::uint8_t>() +
                                packet->getSize());
    }
    std::string error;
    if (!request.valid(error)) {
        _retryRequest = true;
        return false;
    }
    _packets.emplace(request.token, packet);
    _requests.push_back(std::move(request));
    _wakeup();
    return true;
}

bool
Gem5TargetSource::getRequest(MemoryRequest &request)
{
    if (_requests.empty()) {
        return false;
    }
    request = std::move(_requests.front());
    _requests.pop_front();
    return true;
}

bool
Gem5TargetSource::canAcceptResponse(const MemoryResponse &response) const
{
    return !_blockedResponse && _packets.contains(response.token);
}

bool
Gem5TargetSource::submitResponse(const MemoryResponse &response)
{
    if (!canAcceptResponse(response)) {
        return false;
    }
    auto position = _packets.find(response.token);
    PacketPtr packet = position->second;
    if (packet->isRead() && response.data.size() != packet->getSize()) {
        return false;
    }
    if (packet->isWrite() && !response.data.empty()) {
        return false;
    }
    if (packet->isRead() && !response.error) {
        packet->writeData(response.data.data());
    }
    packet->makeResponse();
    if (response.error) {
        packet->setBadCommand();
    }
    trySendResponse(packet);
    return true;
}

void
Gem5TargetSource::trySendResponse(PacketPtr packet)
{
    if (_port.sendTimingResp(packet)) {
        for (auto it = _packets.begin(); it != _packets.end(); ++it) {
            if (it->second == packet) {
                _packets.erase(it);
                break;
            }
        }
    } else {
        _blockedResponse = packet;
    }
}

void
Gem5TargetSource::receiveResponseRetry()
{
    panic_if(!_blockedResponse, "%s received an unexpected response retry",
             _port.name());
    PacketPtr packet = std::exchange(_blockedResponse, nullptr);
    trySendResponse(packet);
    _wakeup();
}

void
Gem5TargetSource::advance()
{
    if (_retryRequest && _packets.size() < _maxPending) {
        _retryRequest = false;
        _port.sendRetryReq();
    }
}

void
Gem5TargetSource::respondUnsupported(PacketPtr packet)
{
    if (packet->needsResponse()) {
        packet->makeResponse();
        packet->setBadCommand();
    }
}

bool
Gem5TargetSource::isIdle() const noexcept
{
    return _requests.empty() && _packets.empty() && !_blockedResponse;
}

} // namespace gem5::rtl_cosim
