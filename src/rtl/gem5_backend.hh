/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_GEM5_BACKEND_HH__
#define __RTL_GEM5_BACKEND_HH__

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/addr_range.hh"
#include "mem/port.hh"
#include "mem/request.hh"
#include "rtl/runtime/transaction.hh"

namespace gem5::rtl_cosim
{

/** Converts requests from an RTL initiator transactor into timing packets. */
class Gem5InitiatorBackend final : public TransactionBackend
{
  private:
    class InitiatorPort final : public RequestPort
    {
      public:
        InitiatorPort(const std::string &name, PortID id,
                      Gem5InitiatorBackend &owner);

      protected:
        bool recvTimingResp(PacketPtr packet) override;
        void recvReqRetry() override;

      private:
        Gem5InitiatorBackend &owner;
    };

    struct PacketState final : public Packet::SenderState
    {
        PacketState(std::uint64_t token, std::size_t beat, bool exclusive,
                    bool write)
            : token(token), beat(beat), exclusive(exclusive), write(write)
        {}

        std::uint64_t token;
        std::size_t beat;
        bool exclusive;
        bool write;
    };

    struct PendingTransaction
    {
        MemoryRequest request;
        MemoryResponse response;
        std::size_t completedBeats = 0;
    };

  public:
    Gem5InitiatorBackend(const std::string &name, PortID id,
                         RequestorID requestorId, std::size_t maxPending,
                         std::size_t cacheLineSize,
                         std::vector<AddrRange> errorRanges,
                         std::function<void()> wakeup);

    bool canAccept(const MemoryRequest &request) const override;
    bool submit(const MemoryRequest &request) override;
    bool getResponse(MemoryResponse &response) override;
    void advance() override;

    bool isIdle() const noexcept;
    RequestPort &port() noexcept { return _port; }
    void sendFunctional(Addr address, const std::uint8_t *data,
                        std::size_t size);
    void setRequestContextId(ContextID contextId) noexcept;

  private:
    bool receiveTimingResponse(PacketPtr packet);
    void completeBeat(std::uint64_t token, std::size_t beat, bool error,
                      bool exclusiveOkay, const std::uint8_t *data);
    void retryRequest();
    void pump();
    PacketPtr makePacket(const MemoryRequest &request, std::size_t beat);

    InitiatorPort _port;
    RequestorID _requestorId;
    std::size_t _maxPending;
    std::size_t _cacheLineSize;
    std::function<void()> _wakeup;
    std::unordered_map<std::uint64_t, PendingTransaction> _transactions;
    std::deque<PacketPtr> _requests;
    std::deque<std::pair<std::uint64_t, std::size_t>> _completedWriteBeats;
    std::deque<std::pair<std::uint64_t, std::size_t>> _errorBeats;
    std::deque<MemoryResponse> _responses;
    std::vector<AddrRange> _errorRanges;
    bool _waitingForRetry = false;
    ContextID _contextId = InvalidContextID;
};

/** Converts timing packets into requests driven into an RTL target bus. */
class Gem5TargetSource final : public TransactionSource
{
  private:
    class TargetPort final : public ResponsePort
    {
      public:
        TargetPort(const std::string &name, PortID id,
                   Gem5TargetSource &owner);

      protected:
        Tick recvAtomic(PacketPtr packet) override;
        bool recvTimingReq(PacketPtr packet) override;
        void recvRespRetry() override;
        void recvFunctional(PacketPtr packet) override;
        AddrRangeList getAddrRanges() const override;

      private:
        Gem5TargetSource &owner;
    };

  public:
    Gem5TargetSource(const std::string &name, PortID id, AddrRange range,
                     std::size_t maxPending,
                     std::function<void()> wakeup);

    bool getRequest(MemoryRequest &request) override;
    bool canAcceptResponse(const MemoryResponse &response) const override;
    bool submitResponse(const MemoryResponse &response) override;
    void advance() override;

    bool isIdle() const noexcept;
    ResponsePort &port() noexcept { return _port; }
    const AddrRange &range() const noexcept { return _range; }

  private:
    bool receiveTimingRequest(PacketPtr packet);
    void receiveResponseRetry();
    void respondUnsupported(PacketPtr packet);
    void trySendResponse(PacketPtr packet);

    TargetPort _port;
    AddrRange _range;
    std::size_t _maxPending;
    std::function<void()> _wakeup;
    std::uint64_t _nextToken = 0;
    std::deque<MemoryRequest> _requests;
    std::unordered_map<std::uint64_t, PacketPtr> _packets;
    PacketPtr _blockedResponse = nullptr;
    bool _retryRequest = false;
};

} // namespace gem5::rtl_cosim

#endif // __RTL_GEM5_BACKEND_HH__
