/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "sim/quantum_barrier.hh"

namespace gem5
{
namespace
{

void
checkPublication(unsigned participants, unsigned spins)
{
    constexpr unsigned Rounds = 2000;
    // Each slot has one writer. Completion must acquire all input writes;
    // each returning worker must acquire completion's output write.
    std::vector<unsigned> inputs(Rounds * participants);
    std::vector<unsigned> outputs(Rounds);
    unsigned completed = 0;
    auto completion = [&]() noexcept {
        const unsigned phase = completed++;
        for (unsigned id = 0; id < participants; ++id) {
            outputs[phase] += inputs[phase * participants + id];
        }
    };
    QuantumBarrier barrier(participants, completion, spins);
    std::atomic<unsigned> errors{0};
    std::vector<std::thread> workers;
    for (unsigned id = 0; id < participants; ++id) {
        workers.emplace_back([&, id] {
            for (unsigned phase = 0; phase < Rounds; ++phase) {
                if (phase % 113 == 0 && id == phase % participants) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                inputs[phase * participants + id] = phase + id;
                barrier.arriveAndWait();
                const unsigned expected =
                    participants * phase +
                    participants * (participants - 1) / 2;
                if (outputs[phase] != expected) {
                    ++errors;
                }
            }
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }
    EXPECT_EQ(completed, Rounds);
    EXPECT_EQ(errors.load(), 0);
}

TEST(QuantumBarrier, OneParticipant)
{
    checkPublication(1, 4096);
}

TEST(QuantumBarrier, BlockingPublication)
{
    checkPublication(17, 0);
}

TEST(QuantumBarrier, SpinningPublication)
{
    checkPublication(17, 32768);
}

TEST(QuantumBarrier, TwoParticipants)
{
    checkPublication(2, 4096);
}

TEST(QuantumBarrier, ManyParticipantsWithShortPolling)
{
    checkPublication(32, 32);
}

TEST(QuantumBarrier, CompletionBeforeReturn)
{
    constexpr unsigned Participants = 4;
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    std::atomic<unsigned> premature{0};
    auto completion = [&]() noexcept {
        entered.store(true);
        entered.notify_one();
        release.wait(false);
        finished.store(true, std::memory_order_relaxed);
    };
    QuantumBarrier barrier(Participants, completion, 4096);
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < Participants; ++i) {
        workers.emplace_back([&] {
            barrier.arriveAndWait();
            if (!finished.load(std::memory_order_relaxed)) {
                ++premature;
            }
        });
    }
    entered.wait(false);
    // Force the waiters past their polling budget while completion is held.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    release.store(true);
    release.notify_one();
    for (auto &worker : workers) {
        worker.join();
    }
    EXPECT_EQ(premature.load(), 0);
}

} // anonymous namespace
} // namespace gem5
