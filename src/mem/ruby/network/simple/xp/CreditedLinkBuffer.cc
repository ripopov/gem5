/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mem/ruby/network/simple/xp/CreditedLinkBuffer.hh"

#include "base/logging.hh"

namespace gem5
{

namespace ruby
{

CreditedLinkBuffer::CreditedLinkBuffer(const Params &p)
    : MessageBuffer(p),
      m_maxCredits(p.credits),
      m_credits(p.credits),
      m_creditReturnLatency(p.credit_return_latency),
      m_enableOooPop(p.enable_ooo_pop),
      m_creditReturnEvent(
          *this, [this] { processCreditReturn(); }, name() + ".creditReturn",
          false, Event::Default_Pri - 1),
      ADD_STAT(m_creditStalls, statistics::units::Count::get(),
               "Number of failed credited-send attempts"),
      ADD_STAT(m_creditReturns, statistics::units::Count::get(),
               "Number of credits returned to the producer"),
      ADD_STAT(m_creditReturnEventCount, statistics::units::Count::get(),
               "Number of delayed credit-return events processed"),
      ADD_STAT(m_availableCredits, statistics::units::Count::get(),
               "Current available producer-visible credits"),
      ADD_STAT(m_pendingReturnsStat, statistics::units::Count::get(),
               "Current credits scheduled to return later"),
      ADD_STAT(m_creditOccupancy, statistics::units::Count::get(),
               "Current occupied credit slots")
{
    fatal_if(isCredited() && params().buffer_size != 0 &&
                 params().buffer_size < m_maxCredits,
             "%s: buffer_size (%u) must be >= credits (%u)",
             name(), params().buffer_size, m_maxCredits);
    fatal_if(isCredited() && m_creditReturnLatency == Cycles(0),
             "%s: credited links require non-zero credit_return_latency",
             name());

    m_creditStalls.flags(statistics::nozero);
    m_creditReturns.flags(statistics::nozero);
    m_creditReturnEventCount.flags(statistics::nozero);
    m_availableCredits.flags(statistics::nozero);
    m_pendingReturnsStat.flags(statistics::nozero);
    m_creditOccupancy.flags(statistics::nozero);

    m_creditOccupancy =
        statistics::constant(m_maxCredits) - m_availableCredits;
    updateCreditStats();
}

void
CreditedLinkBuffer::preDumpStats()
{
    updateCreditStats();
    MessageBuffer::preDumpStats();
}

bool
CreditedLinkBuffer::hasCredit(unsigned slots) const
{
    return !isCredited() || m_credits >= slots;
}

void
CreditedLinkBuffer::registerCreditCallback(std::function<void()> callback)
{
    m_creditCallback = std::move(callback);
}

void
CreditedLinkBuffer::unregisterCreditCallback()
{
    m_creditCallback = nullptr;
}

void
CreditedLinkBuffer::enqueue(MsgPtr message, Tick cur_time,
                            Tick forward_latency, bool ruby_is_random,
                            bool ruby_warmup, bool bypass_strict_fifo)
{
    if (isCredited()) {
        fatal_if(forward_latency == 0,
                 "%s: credited enqueue requires non-zero forward latency",
                 name());
        if (!hasCredit()) {
            m_creditStalls++;
            updateCreditStats();
            panic("%s: credited enqueue without an available credit", name());
        }
        --m_credits;
        updateCreditStats();
    }

    MessageBuffer::enqueue(message, cur_time, forward_latency, ruby_is_random,
                           ruby_warmup, bypass_strict_fifo);
}

CreditedLinkBuffer::Handle
CreditedLinkBuffer::selectEligible(const MessagePredicate &predicate,
                                   Tick cur_time) const
{
    if (!m_enableOooPop) {
        return selectHead(predicate, cur_time);
    }
    return Handle{findReady(predicate, cur_time)};
}

CreditedLinkBuffer::Handle
CreditedLinkBuffer::selectHead(const MessagePredicate &predicate,
                               Tick cur_time) const
{
    if (!canDequeue(cur_time) || isEmpty() || readyTime() > cur_time) {
        return Handle{};
    }

    const MsgPtr &msg = peekMsgPtr();
    if (predicate && !predicate(*msg)) {
        return Handle{};
    }
    return Handle{0};
}

const MsgPtr&
CreditedLinkBuffer::peekAt(Handle handle) const
{
    assert(handle.valid());
    return peekMsgPtrAt(handle.index);
}

Tick
CreditedLinkBuffer::popAt(Handle handle, Tick cur_time,
                          Tick credit_return_delay, unsigned slots)
{
    assert(handle.valid());
    Tick delay = dequeueAt(handle.index, cur_time);
    scheduleCreditReturn(cur_time, credit_return_delay, slots);
    return delay;
}

Tick
CreditedLinkBuffer::dequeue(Tick current_time, bool decrement_messages)
{
    Tick delay = dequeueAt(0, current_time, decrement_messages);
    scheduleCreditReturn(current_time, 0, 1);
    return delay;
}

void
CreditedLinkBuffer::scheduleCreditReturn(Tick cur_time,
                                         Tick credit_return_delay,
                                         unsigned slots)
{
    if (!isCredited() || slots == 0) {
        return;
    }

    const Tick when = cur_time + credit_return_delay;
    m_creditReturnEvents[when] += slots;
    m_pendingCreditReturns += slots;
    updateCreditStats();
    scheduleNextCreditReturn();
}

void
CreditedLinkBuffer::processCreditReturn()
{
    const Tick now = curTick();
    unsigned returned = 0;

    auto it = m_creditReturnEvents.begin();
    while (it != m_creditReturnEvents.end() && it->first <= now) {
        returned += it->second;
        it = m_creditReturnEvents.erase(it);
    }

    if (returned != 0) {
        gem5_assert(m_pendingCreditReturns >= returned,
                    "CreditedLinkBuffer::processCreditReturn");
        m_pendingCreditReturns -= returned;
        m_credits += returned;
        gem5_assert(m_credits <= m_maxCredits,
                    "CreditedLinkBuffer::processCreditReturn");
        m_creditReturns += returned;
        m_creditReturnEventCount++;
        updateCreditStats();

        if (m_creditCallback) {
            m_creditCallback();
        }
    }

    scheduleNextCreditReturn();
}

void
CreditedLinkBuffer::scheduleNextCreditReturn()
{
    if (m_creditReturnEvents.empty()) {
        return;
    }

    const Tick when = m_creditReturnEvents.begin()->first;
    if (m_creditReturnEvent.scheduled()) {
        if (when < m_creditReturnEvent.when()) {
            reschedule(m_creditReturnEvent, when, true);
        }
    } else {
        schedule(m_creditReturnEvent, when);
    }
}

void
CreditedLinkBuffer::updateCreditStats()
{
    m_availableCredits = m_credits;
    m_pendingReturnsStat = m_pendingCreditReturns;
}

} // namespace ruby
} // namespace gem5
