/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/gem5_backend.hh"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/RtlCosim.hh"
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
    std::size_t maxPending, std::size_t cacheLineSize,
    std::vector<AddrRange> errorRanges, std::function<void()> wakeup)
    : _port(name, id, *this), _requestorId(requestorId),
      _maxPending(maxPending), _cacheLineSize(cacheLineSize),
      _wakeup(std::move(wakeup)),
      _errorRanges(std::move(errorRanges))
{
    fatal_if(maxPending == 0,
             "%s: max pending transactions must be positive", name);
    fatal_if(cacheLineSize == 0,
             "%s: cache line size must be positive", name);
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
    if (request.exclusive) {
        fatal_if(_contextId == InvalidContextID,
                 "%s received an exclusive request without a CPU context",
                 _port.name());
        gem5Request->setFlags(Request::LLSC);
        gem5Request->setContext(_contextId);
    }
    if (request.write) {
        std::vector<bool> enables(request.beatBytes);
        const std::size_t offset = beat * request.beatBytes;
        std::transform(request.byteEnable.begin() + offset,
                       request.byteEnable.begin() + offset + request.beatBytes,
                       enables.begin(),
                       [](std::uint8_t value) { return value != 0; });
        gem5Request->setByteEnable(enables);
    }

    const MemCmd command = request.exclusive
        ? (request.write ? MemCmd::StoreCondReq : MemCmd::LoadLockedReq)
        : (request.write ? MemCmd::WriteReq : MemCmd::ReadReq);
    auto *packet = new Packet(gem5Request, command);
    // getPtr() and setData() intentionally reject masked writes. Populate an
    // owned buffer before attaching it so arbitrary AMBA byte strobes remain
    // valid gem5 WriteReq packets.
    auto *payload = new std::uint8_t[request.beatBytes]{};
    if (request.write) {
        const std::size_t offset = beat * request.beatBytes;
        std::copy(request.data.begin() + offset,
                  request.data.begin() + offset + request.beatBytes,
                  payload);
    }
    packet->dataDynamic(payload);
    packet->pushSenderState(new PacketState(
        request.token, beat, request.exclusive, request.write));
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
    DPRINTF(RtlCosim,
            "%s: submit AXI %s at %#llx (%llu beats, %llu bytes/beat)\n",
            _port.name(), request.write ? "write" : "read",
            static_cast<unsigned long long>(request.address),
            static_cast<unsigned long long>(request.beatCount()),
            static_cast<unsigned long long>(request.beatBytes));
    if (request.exclusive) {
        DPRINTF(RtlCosim,
                "%s: submit AXI exclusive %s at %#llx (%llu bytes)\n",
                _port.name(), request.write ? "write" : "read",
                static_cast<unsigned long long>(request.address),
                static_cast<unsigned long long>(request.beatBytes));
    }
    pending.request = request;
    pending.response.token = request.token;
    pending.response.id = request.id;
    if (!request.write) {
        pending.response.data.assign(
            request.beatCount() * request.beatBytes, 0);
    }
    _transactions.emplace(request.token, std::move(pending));
    for (std::size_t beat = 0; beat < request.beatCount(); ++beat) {
        const Addr address = beatAddress(request, beat);
        panic_if(request.beatBytes - 1 > MaxAddr - address,
                 "%s received an overflowing beat address", _port.name());
        const Addr last = address + request.beatBytes - 1;
        const bool injectError = std::any_of(
            _errorRanges.begin(), _errorRanges.end(),
            [address, last](const AddrRange &range) {
                return range.contains(address) && range.contains(last);
            });
        if (injectError) {
            _errorBeats.emplace_back(request.token, beat);
        } else {
            _requests.push_back(makePacket(request, beat));
        }
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
    while (!_errorBeats.empty()) {
        const auto [token, beat] = _errorBeats.front();
        _errorBeats.pop_front();
        completeBeat(token, beat, true, false, nullptr);
    }
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
    const bool exclusive = state->exclusive;
    const bool write = state->write;
    delete state;

    const bool error = packet->isError();
    // LoadLockedReq deliberately becomes a plain ReadResp in gem5, so the
    // response command alone cannot identify an exclusive read. Preserve the
    // original request attributes in PacketState for AXI RRESP=EXOKAY.
    const bool exclusiveOkay = exclusive && !error &&
        (!write || packet->req->getExtraData() != 0);
    if (exclusive) {
        DPRINTF(RtlCosim,
                "%s: complete gem5 exclusive %s (error=%d, success=%d)\n",
                _port.name(), write ? "write" : "read", error,
                exclusiveOkay);
    }
    const std::uint8_t *data = error
                                   ? nullptr
                                   : packet->getConstPtr<std::uint8_t>();
    completeBeat(token, beat, error, exclusiveOkay, data);
    delete packet;
    return true;
}

void
Gem5InitiatorBackend::completeBeat(std::uint64_t token, std::size_t beat,
                                   bool error, bool exclusiveOkay,
                                   const std::uint8_t *data)
{
    auto transaction = _transactions.find(token);
    panic_if(transaction == _transactions.end(),
             "%s received a packet with unknown RTL token %llu",
             _port.name(), static_cast<unsigned long long>(token));
    PendingTransaction &pending = transaction->second;
    panic_if(beat >= pending.request.beatCount(),
             "%s received an invalid RTL beat index", _port.name());

    pending.response.error |= error;
    pending.response.exclusiveOkay |= exclusiveOkay;
    if (!pending.request.write && !error) {
        panic_if(!data, "%s completed a read beat without data", _port.name());
        const std::size_t offset = beat * pending.request.beatBytes;
        std::copy(data, data + pending.request.beatBytes,
                  pending.response.data.begin() + offset);
    }
    ++pending.completedBeats;

    if (pending.completedBeats == pending.request.beatCount()) {
        _responses.push_back(std::move(pending.response));
        _transactions.erase(transaction);
    }
    _wakeup();
}

void
Gem5InitiatorBackend::setRequestContextId(ContextID contextId) noexcept
{
    _contextId = contextId;
}

bool
Gem5InitiatorBackend::isIdle() const noexcept
{
    return _transactions.empty() && _requests.empty() &&
           _errorBeats.empty() && _responses.empty() && !_waitingForRetry;
}

void
Gem5InitiatorBackend::sendFunctional(Addr address,
                                     const std::uint8_t *data,
                                     std::size_t size)
{
    while (size != 0) {
        const std::size_t lineOffset = address % _cacheLineSize;
        const std::size_t chunk = std::min(
            size, _cacheLineSize - lineOffset);
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
    request.exclusive = packet->isLLSC();
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
        packet->setData(response.data.data());
    }
    if (packet->isLLSC() && packet->isWrite()) {
        packet->req->setExtraData(response.exclusiveOkay ? 1 : 0);
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
