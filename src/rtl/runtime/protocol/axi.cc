/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/runtime/protocol/axi.hh"

#include <algorithm>
#include <bit>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

#include "rtl/runtime/signal_access.hh"

namespace gem5::rtl_cosim
{

namespace
{

constexpr std::uint64_t FourKiB = 4096;

std::ptrdiff_t
offset(std::size_t value)
{
    return static_cast<std::ptrdiff_t>(value);
}

struct AddressBeat
{
    std::uint32_t id = 0;
    std::uint64_t address = 0;
    std::size_t beats = 0;
    std::size_t beatBytes = 0;
    BurstType burst = BurstType::Increment;
};

struct WriteBeat
{
    std::uint32_t id = 0;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> strobe;
    bool last = false;
};

struct BBeat
{
    std::uint32_t id = 0;
    bool error = false;
};

struct RBeat
{
    std::uint32_t id = 0;
    std::vector<std::uint8_t> data;
    bool error = false;
    bool last = false;
};

std::uint64_t
beatAddress(const AddressBeat &address, std::size_t index)
{
    if (address.burst == BurstType::Fixed) {
        return address.address;
    }
    const std::uint64_t offset = index * address.beatBytes;
    if (address.burst == BurstType::Increment) {
        return address.address + offset;
    }
    const std::uint64_t span = address.beats * address.beatBytes;
    const std::uint64_t base = address.address / span * span;
    return base + (address.address - base + offset) % span;
}

AddressBeat
requestAddress(const MemoryRequest &request)
{
    return {request.id, request.address, request.beatCount(),
            request.beatBytes, request.burst};
}

class AxiBase : public BusTransactor
{
  public:
    explicit AxiBase(const ValidatedBus &bus) : _bus(bus) {}
    const char *
    getLastError() const override
    {
        return _error.c_str();
    }

  protected:
    bool
    fail(const std::string &message)
    {
        _error = message;
        return false;
    }

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

    bool
    readIgnored(SignalRoleId role)
    {
        Signal *signal = _bus.find(role);
        if (!signal) {
            return true;
        }
        std::vector<std::uint8_t> ignored;
        return readSignal(*signal, ignored, _error);
    }

    bool
    writeZero(SignalRoleId role)
    {
        Signal *signal = _bus.find(role);
        return !signal ||
               writeSignal(*signal,
                           std::vector<std::uint8_t>(signalBytes(*signal), 0),
                           _error);
    }

    bool
    readStrobe(std::vector<std::uint8_t> &strobe)
    {
        Signal &signal = _bus.require(AxiSignal::WStrb);
        std::vector<std::uint8_t> encoded;
        if (!readSignal(signal, encoded, _error)) {
            return false;
        }
        strobe.resize(dataBytes());
        for (std::size_t lane = 0; lane < strobe.size(); ++lane) {
            strobe[lane] = static_cast<std::uint8_t>(
                (encoded[lane / 8] >> (lane % 8)) & 1);
        }
        return true;
    }

    bool
    writeStrobe(const std::vector<std::uint8_t> &strobe)
    {
        Signal &signal = _bus.require(AxiSignal::WStrb);
        std::vector<std::uint8_t> encoded(signalBytes(signal), 0);
        for (std::size_t lane = 0; lane < strobe.size(); ++lane) {
            if (strobe[lane]) {
                encoded[lane / 8] |=
                    static_cast<std::uint8_t>(std::uint8_t{1} << (lane % 8));
            }
        }
        return writeSignal(signal, encoded, _error);
    }

    std::size_t
    dataBytes() const
    {
        return signalBytes(_bus.require(AxiSignal::WData));
    }

    bool
    validateAddress(const AddressBeat &address)
    {
        const std::size_t protocolLimit =
            _bus.bus->protocol() == BusProtocol::Axi4 ? 256 : 16;
        if (address.beats == 0 || address.beats > protocolLimit ||
            address.beats > _limits.maxBurstBeats) {
            return fail("AXI burst exceeds the configured or protocol limit");
        }
        if (!std::has_single_bit(address.beatBytes) ||
            address.beatBytes > dataBytes()) {
            return fail("AXI beat is wider than the data bus");
        }
        if (address.burst != BurstType::Fixed &&
            address.burst != BurstType::Increment &&
            address.burst != BurstType::Wrap) {
            return fail("reserved AXI BURST value");
        }
        if (address.burst == BurstType::Wrap && address.beats != 2 &&
            address.beats != 4 && address.beats != 8 && address.beats != 16) {
            return fail(
                "AXI wrapping burst must contain 2, 4, 8, or 16 beats");
        }
        if (address.address % address.beatBytes != 0) {
            return fail("unaligned AXI transfers are unsupported in V1");
        }

        std::uint64_t highest = address.address;
        if (address.burst == BurstType::Increment) {
            const std::uint64_t offset =
                (address.beats - 1) * address.beatBytes;
            if (address.address >
                std::numeric_limits<std::uint64_t>::max() - offset) {
                return fail("AXI burst address range overflows");
            }
            highest = address.address + offset;
        } else if (address.burst == BurstType::Wrap) {
            const std::uint64_t span = address.beats * address.beatBytes;
            const std::uint64_t base = address.address / span * span;
            if (base >
                std::numeric_limits<std::uint64_t>::max() - (span - 1)) {
                return fail("AXI wrapping burst address range overflows");
            }
            highest = base + span - 1;
        }
        if (address.address / FourKiB != highest / FourKiB) {
            return fail("AXI burst crosses a 4 KiB boundary");
        }

        const std::size_t addressWidth =
            _bus.require(AxiSignal::AwAddr).bitWidth();
        if (addressWidth < 64 && (highest >> addressWidth) != 0) {
            return fail("AXI burst does not fit the discovered address width");
        }
        return true;
    }

    bool
    captureAddress(bool writeAddress, AddressBeat &result)
    {
        const SignalRoleId idRole =
            writeAddress ? AxiSignal::AwId : AxiSignal::ArId;
        const SignalRoleId addressRole =
            writeAddress ? AxiSignal::AwAddr : AxiSignal::ArAddr;
        const SignalRoleId lenRole =
            writeAddress ? AxiSignal::AwLen : AxiSignal::ArLen;
        const SignalRoleId sizeRole =
            writeAddress ? AxiSignal::AwSize : AxiSignal::ArSize;
        const SignalRoleId burstRole =
            writeAddress ? AxiSignal::AwBurst : AxiSignal::ArBurst;
        const SignalRoleId lockRole =
            writeAddress ? AxiSignal::AwLock : AxiSignal::ArLock;
        std::uint64_t id = 0;
        std::uint64_t length = 0;
        std::uint64_t size = 0;
        std::uint64_t burst = 0;
        std::uint64_t lock = 0;
        if (!read(idRole, id) || !read(addressRole, result.address) ||
            !read(lenRole, length) || !read(sizeRole, size) ||
            !read(burstRole, burst) || !read(lockRole, lock)) {
            return false;
        }
        if (lock != 0) {
            return fail(
                "exclusive or locked AXI transactions are unsupported");
        }
        if (size >= std::numeric_limits<std::size_t>::digits) {
            return fail("AXI SIZE is too large");
        }
        result.id = static_cast<std::uint32_t>(id);
        result.beats = static_cast<std::size_t>(length) + 1;
        result.beatBytes = std::size_t{1} << size;
        if (burst > 2) {
            return fail("reserved AXI BURST value");
        }
        result.burst = static_cast<BurstType>(burst);
        std::uint64_t ignored = 0;
        const SignalRoleId cacheRole =
            writeAddress ? AxiSignal::AwCache : AxiSignal::ArCache;
        const SignalRoleId protRole =
            writeAddress ? AxiSignal::AwProt : AxiSignal::ArProt;
        const SignalRoleId regionRole =
            writeAddress ? AxiSignal::AwRegion : AxiSignal::ArRegion;
        const SignalRoleId qosRole =
            writeAddress ? AxiSignal::AwQos : AxiSignal::ArQos;
        const SignalRoleId userRole =
            writeAddress ? AxiSignal::AwUser : AxiSignal::ArUser;
        if (!read(cacheRole, ignored) || !read(protRole, ignored) ||
            !read(regionRole, ignored) || !read(qosRole, ignored) ||
            !readIgnored(userRole)) {
            return false;
        }
        return validateAddress(result);
    }

    bool
    driveAddress(bool writeAddress, const AddressBeat *address)
    {
        const SignalRoleId idRole =
            writeAddress ? AxiSignal::AwId : AxiSignal::ArId;
        const SignalRoleId addressRole =
            writeAddress ? AxiSignal::AwAddr : AxiSignal::ArAddr;
        const SignalRoleId lenRole =
            writeAddress ? AxiSignal::AwLen : AxiSignal::ArLen;
        const SignalRoleId sizeRole =
            writeAddress ? AxiSignal::AwSize : AxiSignal::ArSize;
        const SignalRoleId burstRole =
            writeAddress ? AxiSignal::AwBurst : AxiSignal::ArBurst;
        const SignalRoleId lockRole =
            writeAddress ? AxiSignal::AwLock : AxiSignal::ArLock;
        const SignalRoleId cacheRole =
            writeAddress ? AxiSignal::AwCache : AxiSignal::ArCache;
        const SignalRoleId protRole =
            writeAddress ? AxiSignal::AwProt : AxiSignal::ArProt;
        const SignalRoleId regionRole =
            writeAddress ? AxiSignal::AwRegion : AxiSignal::ArRegion;
        const SignalRoleId qosRole =
            writeAddress ? AxiSignal::AwQos : AxiSignal::ArQos;
        const SignalRoleId userRole =
            writeAddress ? AxiSignal::AwUser : AxiSignal::ArUser;
        const SignalRoleId validRole =
            writeAddress ? AxiSignal::AwValid : AxiSignal::ArValid;
        const std::uint64_t size =
            address ? static_cast<std::uint64_t>(
                          std::countr_zero(address->beatBytes))
                    : 0;
        return write(idRole, address ? address->id : 0) &&
               write(addressRole, address ? address->address : 0) &&
               write(lenRole, address ? address->beats - 1 : 0) &&
               write(sizeRole, size) &&
               write(burstRole, address
                                    ? static_cast<std::uint8_t>(address->burst)
                                    : 0) &&
               write(lockRole, 0) && write(cacheRole, 0) &&
               write(protRole, 0) && write(regionRole, 0) &&
               write(qosRole, 0) && writeZero(userRole) &&
               write(validRole, address ? 1 : 0);
    }

    ValidatedBus _bus;
    TransactorLimits _limits;
    std::string _error;
};

class AxiInitiator : public AxiBase
{
  public:
    AxiInitiator(const ValidatedBus &bus, TransactionBackend &backend,
                 const TransactorLimits &limits)
        : AxiBase(bus), _backend(backend), _nextToken(limits.tokenBase)
    {
        _limits = limits;
        if (_limits.maxPending == 0 || _limits.maxBurstBeats == 0) {
            fail("AXI transactor limits must be positive");
        }
    }

    bool
    beforeClock() override
    {
        if (!_error.empty()) {
            return false;
        }
        _capture = {};
        const bool acceptAw = _aw.size() < _limits.maxPending;
        const auto maxSize = std::numeric_limits<std::size_t>::max();
        const auto maxWriteBeats =
            _limits.maxPending > maxSize / _limits.maxBurstBeats
                ? maxSize
                : _limits.maxPending * _limits.maxBurstBeats;
        const bool acceptW = _w.size() < maxWriteBeats;
        const bool acceptAr = _ar.size() < _limits.maxPending;
        if (!write(AxiSignal::AwReady, acceptAw) ||
            !write(AxiSignal::WReady, acceptW) ||
            !write(AxiSignal::ArReady, acceptAr) || !driveB() || !driveR()) {
            return false;
        }

        std::uint64_t valid = 0;
        if (!read(AxiSignal::AwValid, valid)) {
            return false;
        }
        if (valid && acceptAw) {
            AddressBeat address;
            if (!captureAddress(true, address)) {
                return false;
            }
            _capture.aw = std::move(address);
        }
        if (!read(AxiSignal::WValid, valid)) {
            return false;
        }
        if (valid && acceptW) {
            WriteBeat beat;
            std::uint64_t id = 0;
            std::uint64_t last = 0;
            if (!read(AxiSignal::WId, id) ||
                !readBytes(AxiSignal::WData, beat.data) ||
                !readStrobe(beat.strobe) || !read(AxiSignal::WLast, last) ||
                !readIgnored(AxiSignal::WUser)) {
                return false;
            }
            beat.id = static_cast<std::uint32_t>(id);
            beat.last = last != 0;
            _capture.w = std::move(beat);
        }
        if (!read(AxiSignal::ArValid, valid)) {
            return false;
        }
        if (valid && acceptAr) {
            AddressBeat address;
            if (!captureAddress(false, address)) {
                return false;
            }
            _capture.ar = std::move(address);
        }
        std::uint64_t ready = 0;
        if (!read(AxiSignal::BReady, ready)) {
            return false;
        }
        _capture.b = ready && !_b.empty();
        if (!read(AxiSignal::RReady, ready)) {
            return false;
        }
        _capture.r = ready && !_r.empty();
        return true;
    }

    bool
    afterClock() override
    {
        if (_capture.b) {
            _b.pop_front();
        }
        if (_capture.r) {
            _r.pop_front();
        }
        if (_capture.aw) {
            _aw.emplace_back(std::move(*_capture.aw));
        }
        if (_capture.w) {
            _w.emplace_back(std::move(*_capture.w));
        }
        if (_capture.ar) {
            _ar.emplace_back(std::move(*_capture.ar));
        }
        _capture = {};
        if (!submitWrites() || !submitReads() || !receiveResponses()) {
            return false;
        }
        return true;
    }

    bool
    isIdle() const override
    {
        return _aw.empty() && _w.empty() && _ar.empty() && _b.empty() &&
               _r.empty() && _pending.empty() && _received.empty();
    }

  private:
    struct Capture
    {
        std::optional<AddressBeat> aw;
        std::optional<WriteBeat> w;
        std::optional<AddressBeat> ar;
        bool b = false;
        bool r = false;
    };

    struct Pending
    {
        AddressBeat address;
        bool write = false;
    };

    bool
    driveB()
    {
        const BBeat *beat = _b.empty() ? nullptr : &_b.front();
        return write(AxiSignal::BId, beat ? beat->id : 0) &&
               write(AxiSignal::BResp, beat && beat->error ? 2 : 0) &&
               writeZero(AxiSignal::BUser) &&
               write(AxiSignal::BValid, beat ? 1 : 0);
    }

    bool
    driveR()
    {
        const RBeat *beat = _r.empty() ? nullptr : &_r.front();
        std::vector<std::uint8_t> data(dataBytes(), 0);
        if (beat) {
            data = beat->data;
        }
        return write(AxiSignal::RId, beat ? beat->id : 0) &&
               writeBytes(AxiSignal::RData, data) &&
               write(AxiSignal::RResp, beat && beat->error ? 2 : 0) &&
               write(AxiSignal::RLast, beat && beat->last ? 1 : 0) &&
               writeZero(AxiSignal::RUser) &&
               write(AxiSignal::RValid, beat ? 1 : 0);
    }

    std::optional<std::vector<WriteBeat>>
    takeWriteBeats(const AddressBeat &address)
    {
        std::vector<std::size_t> positions;
        for (std::size_t index = 0;
             index < _w.size() && positions.size() < address.beats; ++index) {
            if (_bus.bus->protocol() == BusProtocol::Axi4 ||
                _w[index].id == address.id) {
                positions.push_back(index);
            }
        }
        if (positions.size() != address.beats) {
            return std::nullopt;
        }
        std::vector<WriteBeat> result;
        result.reserve(address.beats);
        for (std::size_t pos : positions) {
            result.push_back(_w[pos]);
        }
        for (std::size_t index = positions.size(); index > 0; --index) {
            _w.erase(_w.begin() + offset(positions[index - 1]));
        }
        return result;
    }

    bool
    submitWrites()
    {
        while (!_aw.empty()) {
            const AddressBeat address = _aw.front();
            auto beats = takeWriteBeats(address);
            if (!beats) {
                break;
            }
            for (std::size_t index = 0; index < beats->size(); ++index) {
                if ((*beats)[index].last != (index + 1 == beats->size())) {
                    return fail("AXI WLAST does not match AWLEN");
                }
            }
            MemoryRequest request;
            if (_nextToken == std::numeric_limits<std::uint64_t>::max()) {
                return fail("AXI transaction token space exhausted");
            }
            request.token = _nextToken + 1;
            request.id = address.id;
            request.address = address.address;
            request.write = true;
            request.beatBytes = address.beatBytes;
            request.burst = address.burst;
            request.data.reserve(address.beats * address.beatBytes);
            request.byteEnable.reserve(request.data.capacity());
            for (std::size_t index = 0; index < address.beats; ++index) {
                const std::size_t lane =
                    beatAddress(address, index) % dataBytes();
                if (lane + address.beatBytes > dataBytes()) {
                    return fail(
                        "AXI narrow beat crosses the data-bus boundary");
                }
                request.data.insert(request.data.end(),
                                    (*beats)[index].data.begin() +
                                        offset(lane),
                                    (*beats)[index].data.begin() +
                                        offset(lane + address.beatBytes));
                request.byteEnable.insert(
                    request.byteEnable.end(),
                    (*beats)[index].strobe.begin() + offset(lane),
                    (*beats)[index].strobe.begin() +
                        offset(lane + address.beatBytes));
            }
            if (!_backend.canAccept(request)) {
                // Restore beats in their same-ID order and retry later.
                for (auto it = beats->rbegin(); it != beats->rend(); ++it) {
                    _w.push_front(std::move(*it));
                }
                break;
            }
            if (!_backend.submit(request)) {
                return fail(
                    "AXI backend rejected a request after accepting it");
            }
            _nextToken = request.token;
            addPending(request.token, address, true);
            _aw.pop_front();
        }
        return true;
    }

    bool
    submitReads()
    {
        while (!_ar.empty()) {
            const AddressBeat address = _ar.front();
            MemoryRequest request;
            if (_nextToken == std::numeric_limits<std::uint64_t>::max()) {
                return fail("AXI transaction token space exhausted");
            }
            request.token = _nextToken + 1;
            request.id = address.id;
            request.address = address.address;
            request.beatBytes = address.beatBytes;
            request.burst = address.burst;
            request.byteEnable.assign(address.beats * address.beatBytes, 1);
            if (!_backend.canAccept(request)) {
                break;
            }
            if (!_backend.submit(request)) {
                return fail(
                    "AXI backend rejected a request after accepting it");
            }
            _nextToken = request.token;
            addPending(request.token, address, false);
            _ar.pop_front();
        }
        return true;
    }

    void
    addPending(std::uint64_t token, const AddressBeat &address, bool write)
    {
        _pending.emplace(token, Pending{address, write});
        auto &order = write ? _writeOrder[address.id] : _readOrder[address.id];
        order.push_back(token);
    }

    bool
    receiveResponses()
    {
        MemoryResponse response;
        while (_backend.getResponse(response)) {
            if (!_pending.contains(response.token)) {
                return fail("AXI backend returned an unknown token");
            }
            if (!_received.emplace(response.token, std::move(response))
                     .second) {
                return fail("AXI backend returned a duplicate response");
            }
        }
        for (auto &[id, order] : _writeOrder) {
            while (!order.empty() && _received.contains(order.front())) {
                const std::uint64_t token = order.front();
                if (!completeResponse(token)) {
                    return false;
                }
                order.pop_front();
            }
        }
        for (auto &[id, order] : _readOrder) {
            while (!order.empty() && _received.contains(order.front())) {
                const std::uint64_t token = order.front();
                if (!completeResponse(token)) {
                    return false;
                }
                order.pop_front();
            }
        }
        return true;
    }

    bool
    completeResponse(std::uint64_t token)
    {
        Pending pending = _pending.at(token);
        MemoryResponse response = std::move(_received.at(token));
        _pending.erase(token);
        _received.erase(token);
        if (response.id != pending.address.id) {
            return fail("AXI backend response ID does not match its request");
        }
        if (pending.write) {
            if (!response.data.empty()) {
                return fail("AXI write response unexpectedly contains data");
            }
            _b.push_back({pending.address.id, response.error});
            return true;
        }
        const std::size_t expected =
            pending.address.beats * pending.address.beatBytes;
        if (response.data.size() != expected) {
            return fail("AXI read response has the wrong data length");
        }
        for (std::size_t index = 0; index < pending.address.beats; ++index) {
            RBeat beat;
            beat.id = pending.address.id;
            beat.data.assign(dataBytes(), 0);
            const std::size_t lane =
                beatAddress(pending.address, index) % dataBytes();
            if (lane + pending.address.beatBytes > dataBytes()) {
                return fail(
                    "AXI narrow read beat crosses the data-bus boundary");
            }
            std::copy_n(response.data.begin() +
                            offset(index * pending.address.beatBytes),
                        pending.address.beatBytes,
                        beat.data.begin() + offset(lane));
            beat.error = response.error;
            beat.last = index + 1 == pending.address.beats;
            _r.push_back(std::move(beat));
        }
        return true;
    }

    TransactionBackend &_backend;
    std::uint64_t _nextToken;
    Capture _capture;
    std::deque<AddressBeat> _aw;
    std::deque<WriteBeat> _w;
    std::deque<AddressBeat> _ar;
    std::deque<BBeat> _b;
    std::deque<RBeat> _r;
    std::unordered_map<std::uint64_t, Pending> _pending;
    std::unordered_map<std::uint64_t, MemoryResponse> _received;
    std::map<std::uint32_t, std::deque<std::uint64_t>> _writeOrder;
    std::map<std::uint32_t, std::deque<std::uint64_t>> _readOrder;
};

class AxiTarget : public AxiBase
{
  public:
    AxiTarget(const ValidatedBus &bus, TransactionSource &source,
              const TransactorLimits &limits)
        : AxiBase(bus), _source(source)
    {
        _limits = limits;
        if (_limits.maxPending == 0 || _limits.maxBurstBeats == 0) {
            fail("AXI transactor limits must be positive");
        }
    }

    bool
    beforeClock() override
    {
        if (!_error.empty()) {
            return false;
        }
        fillRequests();
        if (!_error.empty()) {
            return false;
        }
        _capture = {};
        if (!driveWrite() || !driveRead()) {
            return false;
        }
        const bool acceptB = _responses.size() < _limits.maxPending;
        const bool acceptR = _responses.size() < _limits.maxPending;
        if (!write(AxiSignal::BReady, acceptB) ||
            !write(AxiSignal::RReady, acceptR)) {
            return false;
        }

        std::uint64_t ready = 0;
        if (_write) {
            if (!read(AxiSignal::AwReady, ready)) {
                return false;
            }
            _capture.aw = !_write->awSent && ready;
            if (!read(AxiSignal::WReady, ready)) {
                return false;
            }
            _capture.w = ready && _write->beat < _write->address.beats;
        }
        if (_read) {
            if (!read(AxiSignal::ArReady, ready)) {
                return false;
            }
            _capture.ar = ready;
        }

        std::uint64_t valid = 0;
        if (!read(AxiSignal::BValid, valid)) {
            return false;
        }
        if (valid && acceptB) {
            BBeat beat;
            std::uint64_t id = 0;
            std::uint64_t response = 0;
            if (!read(AxiSignal::BId, id) ||
                !read(AxiSignal::BResp, response) ||
                !readIgnored(AxiSignal::BUser)) {
                return false;
            }
            beat.id = static_cast<std::uint32_t>(id);
            beat.error = response != 0;
            _capture.b = beat;
        }
        if (!read(AxiSignal::RValid, valid)) {
            return false;
        }
        if (valid && acceptR) {
            RBeat beat;
            std::uint64_t id = 0;
            std::uint64_t response = 0;
            std::uint64_t last = 0;
            if (!read(AxiSignal::RId, id) ||
                !readBytes(AxiSignal::RData, beat.data) ||
                !read(AxiSignal::RResp, response) ||
                !read(AxiSignal::RLast, last) ||
                !readIgnored(AxiSignal::RUser)) {
                return false;
            }
            beat.id = static_cast<std::uint32_t>(id);
            beat.error = response != 0;
            beat.last = last != 0;
            _capture.r = std::move(beat);
        }
        return true;
    }

    bool
    afterClock() override
    {
        if (_write) {
            if (_capture.aw) {
                _write->awSent = true;
            }
            if (_capture.w) {
                ++_write->beat;
            }
            if (_write->awSent && _write->beat == _write->address.beats) {
                _pendingWrites[_write->address.id].push_back(
                    std::move(_write->request));
                _write.reset();
            }
        }
        if (_read && _capture.ar) {
            ReadPending pending;
            pending.request = std::move(_read->request);
            pending.address = _read->address;
            _pendingReads[pending.address.id].push_back(std::move(pending));
            _read.reset();
        }
        if (_capture.b && !captureB(*_capture.b)) {
            return false;
        }
        if (_capture.r && !captureR(*_capture.r)) {
            return false;
        }
        _capture = {};
        while (!_responses.empty() &&
               _source.canAcceptResponse(_responses.front())) {
            if (!_source.submitResponse(_responses.front())) {
                return fail(
                    "AXI source rejected a response after accepting it");
            }
            _responses.pop_front();
        }
        return true;
    }

    bool
    isIdle() const override
    {
        if (_write || _read || !_queuedWrites.empty() ||
            !_queuedReads.empty() || !_responses.empty()) {
            return false;
        }
        for (const auto &[id, queue] : _pendingWrites) {
            if (!queue.empty()) {
                return false;
            }
        }
        for (const auto &[id, queue] : _pendingReads) {
            if (!queue.empty()) {
                return false;
            }
        }
        return true;
    }

  private:
    struct ActiveWrite
    {
        MemoryRequest request;
        AddressBeat address;
        bool awSent = false;
        std::size_t beat = 0;
    };
    struct ActiveRead
    {
        MemoryRequest request;
        AddressBeat address;
    };
    struct ReadPending
    {
        MemoryRequest request;
        AddressBeat address;
        std::size_t beatsReceived = 0;
        bool error = false;
        std::vector<std::uint8_t> data;
    };
    struct Capture
    {
        bool aw = false;
        bool w = false;
        bool ar = false;
        std::optional<BBeat> b;
        std::optional<RBeat> r;
    };

    void
    fillRequests()
    {
        while (outstandingCount() < _limits.maxPending) {
            MemoryRequest request;
            if (!_source.getRequest(request)) {
                break;
            }
            std::string reason;
            if (!request.valid(reason)) {
                fail("invalid AXI source request: " + reason);
                break;
            }
            const AddressBeat address = requestAddress(request);
            if (!validateAddress(address)) {
                break;
            }
            const SignalRoleId idRole =
                request.write ? AxiSignal::AwId : AxiSignal::ArId;
            Signal *idSignal = _bus.find(idRole);
            if ((!idSignal && request.id != 0) ||
                (idSignal && idSignal->bitWidth() < 32 &&
                 (request.id >> idSignal->bitWidth()) != 0)) {
                fail("AXI source ID does not fit the discovered ID width");
                break;
            }
            if (request.write) {
                _queuedWrites.push_back(std::move(request));
            } else {
                _queuedReads.push_back(std::move(request));
            }
        }
        if (!_write && !_queuedWrites.empty()) {
            MemoryRequest request = std::move(_queuedWrites.front());
            _queuedWrites.pop_front();
            _write = ActiveWrite{std::move(request), {}, false, 0};
            _write->address = requestAddress(_write->request);
        }
        if (!_read && !_queuedReads.empty()) {
            MemoryRequest request = std::move(_queuedReads.front());
            _queuedReads.pop_front();
            _read = ActiveRead{std::move(request), {}};
            _read->address = requestAddress(_read->request);
        }
    }

    std::size_t
    outstandingCount() const
    {
        std::size_t count = _queuedWrites.size() + _queuedReads.size() +
                            static_cast<std::size_t>(_write.has_value()) +
                            static_cast<std::size_t>(_read.has_value()) +
                            _responses.size();
        for (const auto &[id, queue] : _pendingWrites) {
            count += queue.size();
        }
        for (const auto &[id, queue] : _pendingReads) {
            count += queue.size();
        }
        return count;
    }

    bool
    driveWrite()
    {
        if (!driveAddress(true, _write && !_write->awSent ? &_write->address
                                                          : nullptr)) {
            return false;
        }
        if (!_write || _write->beat >= _write->address.beats) {
            return write(AxiSignal::WId, 0) &&
                   writeBytes(AxiSignal::WData,
                              std::vector<std::uint8_t>(dataBytes(), 0)) &&
                   writeStrobe(std::vector<std::uint8_t>(dataBytes(), 0)) &&
                   write(AxiSignal::WLast, 0) && writeZero(AxiSignal::WUser) &&
                   write(AxiSignal::WValid, 0);
        }
        const std::size_t index = _write->beat;
        const std::size_t lane =
            beatAddress(_write->address, index) % dataBytes();
        if (lane + _write->address.beatBytes > dataBytes()) {
            return fail("AXI narrow write beat crosses the data-bus boundary");
        }
        std::vector<std::uint8_t> data(dataBytes(), 0);
        std::copy_n(_write->request.data.begin() +
                        offset(index * _write->address.beatBytes),
                    _write->address.beatBytes, data.begin() + offset(lane));
        std::vector<std::uint8_t> strobe(dataBytes(), 0);
        for (std::size_t byte = 0; byte < _write->address.beatBytes; ++byte) {
            if (_write->request
                    .byteEnable[index * _write->address.beatBytes + byte]) {
                strobe[lane + byte] = 1;
            }
        }
        return write(AxiSignal::WId, _write->address.id) &&
               writeBytes(AxiSignal::WData, data) && writeStrobe(strobe) &&
               write(AxiSignal::WLast,
                     index + 1 == _write->address.beats ? 1 : 0) &&
               writeZero(AxiSignal::WUser) && write(AxiSignal::WValid, 1);
    }

    bool
    driveRead()
    {
        return driveAddress(false, _read ? &_read->address : nullptr);
    }

    bool
    captureB(const BBeat &beat)
    {
        auto &queue = _pendingWrites[beat.id];
        if (queue.empty()) {
            return fail("AXI target returned B for an unknown ID");
        }
        MemoryRequest request = std::move(queue.front());
        queue.pop_front();
        _responses.push_back(
            MemoryResponse{request.token, request.id, {}, beat.error});
        return true;
    }

    bool
    captureR(const RBeat &beat)
    {
        auto &queue = _pendingReads[beat.id];
        if (queue.empty()) {
            return fail("AXI target returned R for an unknown ID");
        }
        ReadPending &pending = queue.front();
        const std::size_t index = pending.beatsReceived;
        if (index >= pending.address.beats) {
            return fail("AXI target returned too many R beats");
        }
        const std::size_t lane =
            beatAddress(pending.address, index) % dataBytes();
        if (lane + pending.address.beatBytes > dataBytes()) {
            return fail("AXI narrow read beat crosses the data-bus boundary");
        }
        pending.data.insert(
            pending.data.end(), beat.data.begin() + offset(lane),
            beat.data.begin() + offset(lane + pending.address.beatBytes));
        pending.error = pending.error || beat.error;
        ++pending.beatsReceived;
        const bool expectedLast =
            pending.beatsReceived == pending.address.beats;
        if (beat.last != expectedLast) {
            return fail("AXI RLAST does not match ARLEN");
        }
        if (expectedLast) {
            _responses.push_back(
                MemoryResponse{pending.request.token, pending.request.id,
                               std::move(pending.data), pending.error});
            queue.pop_front();
        }
        return true;
    }

    TransactionSource &_source;
    Capture _capture;
    std::deque<MemoryRequest> _queuedWrites;
    std::deque<MemoryRequest> _queuedReads;
    std::optional<ActiveWrite> _write;
    std::optional<ActiveRead> _read;
    std::map<std::uint32_t, std::deque<MemoryRequest>> _pendingWrites;
    std::map<std::uint32_t, std::deque<ReadPending>> _pendingReads;
    std::deque<MemoryResponse> _responses;
};

} // anonymous namespace

std::unique_ptr<BusTransactor>
createAxiInitiatorTransactor(const ValidatedBus &bus,
                             TransactionBackend &backend,
                             const TransactorLimits &limits)
{
    return std::make_unique<AxiInitiator>(bus, backend, limits);
}

std::unique_ptr<BusTransactor>
createAxiTargetTransactor(const ValidatedBus &bus, TransactionSource &source,
                          const TransactorLimits &limits)
{
    return std::make_unique<AxiTarget>(bus, source, limits);
}

} // namespace gem5::rtl_cosim
