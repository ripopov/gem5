/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared microbenchmark for out-of-order (OoO) ready-message selection and
 * popping in ruby::MessageBuffer. The SAME source is compiled against three
 * interchangeable MessageBuffer implementations (pruned-heap traversal, flat
 * O(n) scan, two-container heap + matured set) so their selection cost and
 * memory footprint can be compared apples-to-apples.
 *
 * It sweeps occupancy N in {1,2,...,1024} and matured fraction in
 * {30%, 70%, 100%}, reporting (to stdout, "MBBENCH," prefixed CSV):
 *   - ns per selectEligible() call on a static full buffer (selection cost)
 *   - ns per matured pop during a select+popAt drain (pop cost)
 *   - selectorMemoryBytes() at that occupancy (memory cost)
 *
 * Only the public MessageBuffer API plus selectorMemoryBytes() is used, so the
 * file is byte-for-byte identical across the three worktrees.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "base/gtest/logging.hh"
#include "enums/MessageRandomization.hh"
#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/slicc_interface/Message.hh"
#include "params/ClockDomain.hh"
#include "params/ClockedObject.hh"
#include "params/MessageBuffer.hh"
#include "sim/clock_domain.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/fst_trace/fst_trace.hh"
#include "sim/power/power_model.hh"
#include "sim/root.hh"
#include "sim/transaction_trace/ftr_trace.hh"
#include "sim/voltage_domain.hh"

namespace gem5
{

// Satisfy the same narrow set of link-time references the unit test stubs out;
// this standalone binary never exercises tracing/root/voltage/power paths.
void
FstTrace::recordMessageBufferPush(const ruby::MessageBuffer *buffer, Tick tick)
{
}

void
FstTrace::recordMessageBufferPop(const ruby::MessageBuffer *buffer, Tick tick)
{
}

FtrTrace *FtrTrace::instance = nullptr;

Root *Root::_root = nullptr;

bool
VoltageDomain::sanitiseVoltages()
{
    return false;
}

void
PowerModel::setClockedObject(ClockedObject *object)
{
}

void
FtrTrace::stampEvent(TraceId id, std::string_view event_kind,
                     std::string_view object_name, Tick tick,
                     const tx_trace::AttrList &attrs)
{
}

namespace ruby
{
namespace
{

constexpr Tick ClockPeriod = 1;

// cur_time used for every selection: matured messages have arrival <= this,
// immature messages have arrival far above it.
constexpr Tick SelectTime = Tick(1) << 20;

using MessagePredicate = std::function<bool(const Message&)>;

class BenchMessage : public Message
{
  public:
    explicit BenchMessage(Tick cur_time)
        : Message(cur_time, 64, nullptr)
    {
    }

    MsgPtr
    clone() const override
    {
        return std::make_shared<BenchMessage>(*this);
    }

    void
    print(std::ostream &out) const override
    {
        out << "BenchMessage";
    }
};

class TestClockDomain : public ClockDomain
{
  public:
    TestClockDomain(const ClockDomainParams &params, Tick period)
        : ClockDomain(params, nullptr)
    {
        _clockPeriod = period;
    }
};

// MessageBuffer::enqueue signals a consumer; a do-nothing one suffices here.
class BenchConsumer : public Consumer
{
  public:
    explicit BenchConsumer(ClockedObject *object) : Consumer(object) {}
    void wakeup() override {}
    void print(std::ostream &out) const override { out << "BenchConsumer"; }
};

// Build a MessageBuffer harness once; refill it cheaply between drains.
class Harness
{
  public:
    Harness()
    {
        queue = getEventQueue(0);
        curEventQueue(queue);
        queue->setCurTick(SelectTime);

        clockDomainParams.name = "bench.clock_domain";
        clockDomainParams.eventq_index = 0;
        clockDomain = std::make_unique<TestClockDomain>(
            clockDomainParams, ClockPeriod);

        clockedObjectParams.name = "bench.clocked_object";
        clockedObjectParams.eventq_index = 0;
        clockedObjectParams.clk_domain = clockDomain.get();
        clockedObjectParams.power_state = nullptr;
        clockedObject = std::make_unique<ClockedObject>(clockedObjectParams);
        consumer = std::make_unique<BenchConsumer>(clockedObject.get());
    }

    // Construct a fresh, uncredited, infinite-capacity OoO buffer.
    MessageBuffer &
    makeBuffer()
    {
        buffer.reset();
        bufferParams = {};
        bufferParams.name = "bench.buffer";
        bufferParams.eventq_index = 0;
        bufferParams.allow_zero_latency = false;
        bufferParams.buffer_size = 0;     // infinite
        bufferParams.max_dequeue_rate = 0;
        bufferParams.ordered = false;     // free arrival order
        bufferParams.randomization = MessageRandomization::disabled;
        bufferParams.routing_priority = 0;
        bufferParams.port_out_port_connection_count = 0;
        bufferParams.port_in_port_connection_count = 0;
        bufferParams.credit_return_latency = Cycles(0);
        bufferParams.credits = 0;         // uncredited -> infinite credit
        bufferParams.enable_ooo_pop = true;

        buffer = std::make_unique<MessageBuffer>(bufferParams);
        buffer->setConsumer(consumer.get());
        return *buffer;
    }

    // Fill `buf` with `n` messages of which `matured` have arrival <= SelectTime
    // and the rest are far in the future. Enqueue order is shuffled (LCG) so
    // the resulting heap is a realistic multi-level tree, not a sorted array.
    void
    fill(MessageBuffer &buf, size_t n, size_t matured)
    {
        buf.clear();
        // Deterministic permutation of [0, n) via an LCG-driven Fisher-Yates.
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; ++i) {
            order[i] = i;
        }
        uint64_t rng = 0x9e3779b97f4a7c15ULL ^ (uint64_t(n) << 1);
        for (size_t i = n; i > 1; --i) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            size_t j = size_t(rng >> 33) % i;
            std::swap(order[i - 1], order[j]);
        }

        for (size_t k = 0; k < n; ++k) {
            size_t rank = order[k];
            Tick arrival;
            if (rank < matured) {
                // Spread matured arrivals over [1, matured] (<= SelectTime).
                arrival = Tick(1 + rank);
            } else {
                // Immature: well past SelectTime so they prune cleanly.
                arrival = (Tick(1) << 21) + Tick(rank);
            }
            // enqueue(message, current_time=0, delta=arrival) sets the
            // message's lastEnqueueTime to `arrival`.
            buf.enqueue(std::make_shared<BenchMessage>(0), 0, arrival,
                        false, false);
        }
    }

    EventQueue *queue = nullptr;
    ClockDomainParams clockDomainParams{};
    ClockedObjectParams clockedObjectParams{};
    MessageBufferParams bufferParams{};
    std::unique_ptr<TestClockDomain> clockDomain;
    std::unique_ptr<ClockedObject> clockedObject;
    std::unique_ptr<BenchConsumer> consumer;
    std::unique_ptr<MessageBuffer> buffer;
};

using Clock = std::chrono::steady_clock;

double
toNs(Clock::duration d)
{
    return std::chrono::duration<double, std::nano>(d).count();
}

// Always-true predicate forces selectEligible down the O(n_ready) findReady
// path (a null predicate would short-circuit to the O(1) head) and makes every
// call traverse the full ready set, isolating selection cost.
const MessagePredicate AcceptAll = [](const Message &) { return true; };

// Measure ns per selectEligible() on a static, full buffer.
double
measureSelectNs(MessageBuffer &buf, volatile size_t &sink)
{
    constexpr double TargetNs = 1.0e8;  // ~100 ms
    uint64_t calls = 0;
    auto start = Clock::now();
    do {
        for (int rep = 0; rep < 1024; ++rep) {
            auto h = buf.selectEligible(AcceptAll, SelectTime);
            sink += h.index;
        }
        calls += 1024;
    } while (toNs(Clock::now() - start) < TargetNs);
    return toNs(Clock::now() - start) / double(calls);
}

// Measure ns per matured pop during a select+popAt drain. The fill is untimed;
// only the drain loop is measured.
double
measurePopNs(Harness &h, size_t n, size_t matured, volatile size_t &sink)
{
    if (matured == 0) {
        return 0.0;
    }
    constexpr double TargetNs = 1.0e8;  // ~100 ms
    double total_ns = 0.0;
    uint64_t total_pops = 0;
    while (total_ns < TargetNs && total_pops < 5'000'000) {
        MessageBuffer &buf = *h.buffer;
        h.fill(buf, n, matured);
        auto start = Clock::now();
        for (;;) {
            auto handle = buf.selectEligible(AcceptAll, SelectTime);
            if (!handle.valid()) {
                break;
            }
            sink += handle.index;
            buf.popAt(handle, SelectTime, 0);
        }
        total_ns += toNs(Clock::now() - start);
        total_pops += matured;
    }
    return total_ns / double(total_pops);
}

TEST(MessageBufferOooBench, Sweep)
{
    Harness h;
    volatile size_t sink = 0;

    const size_t sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    const int matured_pct[] = {30, 70, 100};

    std::printf("MBBENCH,N,matured_pct,ready,ns_per_select,"
                "ns_per_pop,selector_bytes\n");
    std::fflush(stdout);

    for (size_t n : sizes) {
        for (int pct : matured_pct) {
            size_t matured = (n * size_t(pct)) / 100;
            if (pct == 100) {
                matured = n;
            }

            // Static full buffer for selection + memory measurement.
            MessageBuffer &buf = h.makeBuffer();
            h.fill(buf, n, matured);
            double sel_ns = measureSelectNs(buf, sink);
            // Read memory AFTER warming selection so variants that lazily build
            // an auxiliary matured-set container report their active footprint
            // rather than the pre-selection (empty-cache) state.
            size_t bytes = buf.selectorMemoryBytes();

            // Drain measurement (refills internally).
            double pop_ns = measurePopNs(h, n, matured, sink);

            std::printf("MBBENCH,%zu,%d,%zu,%.3f,%.3f,%zu\n",
                        n, pct, matured, sel_ns, pop_ns, bytes);
            std::fflush(stdout);
        }
    }

    // Keep the optimizer honest.
    EXPECT_NE(sink, 0xdeadbeefu);
}

} // anonymous namespace
} // namespace ruby
} // namespace gem5
