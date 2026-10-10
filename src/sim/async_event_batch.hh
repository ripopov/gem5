/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SIM_ASYNC_EVENT_BATCH_HH__
#define __SIM_ASYNC_EVENT_BATCH_HH__

#include <atomic>
#include <mutex>
#include <vector>

namespace gem5
{

class Event;

/**
 * A growable, single-producer/single-consumer event channel. Appending needs
 * no synchronization. Publication and collection transfer whole batches;
 * neither holds the channel lock while processing individual events.
 *
 * A producer can publish again before collection (e.g., at consecutive
 * global barriers). Pending batches retain producer order. Capacity is
 * recycled, and a full buffer never blocks progress towards a barrier.
 */
class AsyncEventBatch
{
  private:
    alignas(64) std::vector<Event *> staging;

    alignas(64) std::mutex mutex;
    std::vector<Event *> pending;
    std::atomic<bool> available{false};

    alignas(64) std::vector<Event *> incoming;

  public:
    // Producer only. True marks the transition to an active channel.
    bool
    append(Event *event)
    {
        const bool first = staging.empty();
        staging.push_back(event);
        return first;
    }

    // Producer only, before arriving at a global barrier.
    void
    publish()
    {
        if (staging.empty()) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (pending.empty()) {
            pending.swap(staging);
        } else {
            pending.insert(pending.end(), staging.begin(), staging.end());
            staging.clear();
        }
        available.store(true, std::memory_order_release);
    }

    // Consumer only. Publication may overlap collection and processing.
    template <class Consume>
    void
    drain(Consume consume)
    {
        if (!available.load(std::memory_order_acquire)) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            incoming.swap(pending);
            available.store(false, std::memory_order_relaxed);
        }
        for (Event *event : incoming) {
            consume(event);
        }
        incoming.clear();
    }
};

} // namespace gem5

#endif // __SIM_ASYNC_EVENT_BATCH_HH__
