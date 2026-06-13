/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_RUBY_NETWORK_SIMPLE_XP_CREDITEDLINKBUFFER_HH__
#define __MEM_RUBY_NETWORK_SIMPLE_XP_CREDITEDLINKBUFFER_HH__

#include <functional>
#include <map>

#include "mem/ruby/network/MessageBuffer.hh"
#include "params/CreditedLinkBuffer.hh"
#include "sim/eventq.hh"

namespace gem5
{

namespace ruby
{

class CreditedLinkBuffer : public MessageBuffer
{
  public:
    PARAMS(CreditedLinkBuffer);

    struct Handle
    {
        size_t index = invalidMessageIndex;

        bool valid() const { return index != invalidMessageIndex; }
    };

    CreditedLinkBuffer(const Params &p);
    void preDumpStats() override;

    bool isCredited() const { return m_maxCredits != 0; }
    bool hasCredit(unsigned slots = 1) const;
    unsigned availableCredits() const { return m_credits; }
    unsigned maxCredits() const { return m_maxCredits; }
    Cycles creditReturnLatency() const { return m_creditReturnLatency; }
    bool enableOooPop() const { return m_enableOooPop; }

    void registerCreditCallback(std::function<void()> callback);
    void unregisterCreditCallback();

    void enqueue(MsgPtr message, Tick cur_time, Tick forward_latency,
                 bool ruby_is_random, bool ruby_warmup,
                 bool bypass_strict_fifo = false) override;

    Handle selectEligible(const MessagePredicate &predicate,
                          Tick cur_time) const;
    Handle selectHead(const MessagePredicate &predicate,
                      Tick cur_time) const;
    const MsgPtr& peekAt(Handle handle) const;
    Tick popAt(Handle handle, Tick cur_time, Tick credit_return_delay,
               unsigned slots = 1);
    Tick dequeue(Tick current_time,
                 bool decrement_messages = true) override;

    unsigned pendingCreditReturns() const { return m_pendingCreditReturns; }

  private:
    void scheduleCreditReturn(Tick cur_time, Tick credit_return_delay,
                              unsigned slots);
    void processCreditReturn();
    void scheduleNextCreditReturn();
    void updateCreditStats();

    const unsigned m_maxCredits;
    unsigned m_credits;
    const Cycles m_creditReturnLatency;
    const bool m_enableOooPop;

    std::function<void()> m_creditCallback;
    std::map<Tick, unsigned> m_creditReturnEvents;
    unsigned m_pendingCreditReturns = 0;
    EventFunctionWrapper m_creditReturnEvent;

    statistics::Scalar m_creditStalls;
    statistics::Scalar m_creditReturns;
    statistics::Scalar m_creditReturnEventCount;
    statistics::Scalar m_availableCredits;
    statistics::Scalar m_pendingReturnsStat;
    statistics::Formula m_creditOccupancy;
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_SIMPLE_XP_CREDITEDLINKBUFFER_HH__
