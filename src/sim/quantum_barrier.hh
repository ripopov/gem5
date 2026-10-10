/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SIM_QUANTUM_BARRIER_HH__
#define __SIM_QUANTUM_BARRIER_HH__

#include <atomic>
#include <cassert>
#include <type_traits>
#include <utility>

namespace gem5
{

/**
 * A reusable barrier for short simulation quanta. The last arrival executes
 * completion before publishing the next generation. Participants poll briefly
 * with a processor hint, then block if another queue takes longer to arrive.
 * Unlike a scheduling yield, the hint keeps short waits in user space.
 *
 * Every participant must arrive exactly once per generation. Destruction
 * requires all calls to arriveAndWait() to have returned.
 */
template <class Completion> class QuantumBarrier
{
  private:
    const unsigned participants;
    const unsigned spinCount;
    Completion completion;

    // Arrival writes must not invalidate the cache line being polled.
    alignas(64) std::atomic<unsigned> remaining;
    alignas(64) std::atomic<unsigned> generation{0};

    static void
    relax()
    {
#if defined(__i386__) || defined(__x86_64__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#else
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }

  public:
    QuantumBarrier(unsigned participants, Completion completion,
                   unsigned spin_count)
        : participants(participants),
          spinCount(spin_count),
          completion(std::move(completion)),
          remaining(participants)
    {
        assert(participants > 0);
        static_assert(std::is_nothrow_invocable_v<Completion &>);
    }

    void
    arriveAndWait()
    {
        const unsigned phase = generation.load(std::memory_order_relaxed);
        // The RMW chain acquires every participant's preceding writes before
        // completion runs. Publishing the generation then releases those
        // writes, and completion's writes, to every returning participant.
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            completion();
            remaining.store(participants, std::memory_order_relaxed);
            generation.store(phase + 1, std::memory_order_release);
            generation.notify_all();
            return;
        }

        for (unsigned i = 0; i < spinCount; ++i) {
            if (generation.load(std::memory_order_acquire) != phase) {
                return;
            }
            relax();
        }
        // wait checks the value again, including a release racing with the
        // transition from polling to sleeping. No notification can be lost.
        generation.wait(phase, std::memory_order_acquire);
    }
};

} // namespace gem5

#endif // __SIM_QUANTUM_BARRIER_HH__
