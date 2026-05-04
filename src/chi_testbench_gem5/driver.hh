/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_DRIVER_HH__
#define __CHI_TESTBENCH_GEM5_DRIVER_HH__

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "chi_testbench_gem5/seq_thread.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "mem/request.hh"
#include "params/ChiSeqDriver.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace chi_gem5tb
{

class ChiBarrier;
class ChiEventBus;
class ChiSequence;

/**
 * Generic per-tile driver. `sequence` (Python param) is a polymorphic
 * `ChiSequence` SimObject; SeqThread invokes `sequence->run(*this)`
 * once per fiber kickoff. Each concrete subclass owns its own typed
 * Params (see ChiSequence.py and the per-sequence headers under
 * sequences/).
 *
 * Architecture:
 *   - ChiSeqDriver is the ClockedObject at system.cpu[i]. Because it
 *     is itself a SimObject, Ruby's per-sequencer stat paths parent
 *     cleanly without any adapter layer.
 *   - getPort("port") returns a RequestPort the Python shell wires to
 *     the tile's sequencer in_ports (`system.ruby._cpu_ports[i]`).
 *   - startup() schedules kick_event at curTick(); when that event
 *     fires, seq_thread.run() enters the fiber for the first time.
 *   - Blocking read/write construct a Packet, call sendTimingReq,
 *     yield the fiber; recvTimingResp copies read data into the
 *     caller's buffer and resumes the fiber.
 *   - Non-blocking async_* allocate a handle, register it in the
 *     inflight table, send the packet, and return; resolve(h) yields
 *     until that specific handle's response arrives.
 *
 * Fiber-side API used by sequences:
 *     void     read(addr, buf, len)
 *     void     read_exclusive(addr, buf, len)
 *     void     write(addr, buf, len)
 *     Handle   async_read / async_read_exclusive / async_write
 *     void     resolve(h) / resolve_all()
 *     void     wait_ticks(t) / wait_cycles(c)
 *     uint32_t tile_id()
 */
class ChiSeqDriver : public ClockedObject
{
  public:
    using Params = ChiSeqDriverParams;
    using Handle = uint64_t;

    explicit ChiSeqDriver(const Params &p);
    ~ChiSeqDriver() override;

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    void startup() override;

    // --- Accessors used by SeqThread::main() and by sequences ---

    const Params &
    params() const
    {
        return _p;
    }
    uint32_t
    tile_id() const
    {
        return _p.tile_id;
    }
    ChiBarrier *
    finish_barrier() const
    {
        return _p.finish_barrier;
    }
    ChiEventBus *
    event_bus() const
    {
        return _p.event_bus;
    }
    ChiSequence *
    sequence() const
    {
        return _p.sequence;
    }

    // --- Blocking API (suspends the fiber until response) ---

    void read(uint64_t addr, uint8_t *buf, uint32_t len);
    /**
     * Issues a Packet tagged with `MemCmd::ReadExReq`, an
     * "exclusive-intent" read hint.
     *
     * At runtime this is indistinguishable from read(): gem5's Ruby
     * CHI sequencer maps both `MemCmd::ReadReq` and `MemCmd::ReadExReq`
     * to `RubyRequestType_LD`, which becomes a CHI `ReadShared` on
     * the wire. The `_exclusive` in the name preserves caller intent
     * in case Ruby CHI starts honoring the hint; today it does not.
     *
     * To acquire exclusive ownership without writing data, issue a
     * write() from a tile that doesn't hold the line — it produces a
     * `CleanUnique` upgrade (or `ReadUnique` on a cold line). The
     * `read_ex_walk` scenario demonstrates this pattern.
     */
    void read_exclusive(uint64_t addr, uint8_t *buf, uint32_t len);
    void write(uint64_t addr, const uint8_t *buf, uint32_t len);

    // --- Non-blocking API ---

    Handle async_read(uint64_t addr, uint8_t *out, uint32_t len);
    Handle async_read_exclusive(uint64_t addr, uint8_t *out, uint32_t len);
    Handle async_write(uint64_t addr, const uint8_t *buf, uint32_t len);
    void resolve(Handle h);
    void resolve_all();
    std::size_t
    outstanding() const
    {
        return inflight.size();
    }

    // --- Time / housekeeping ---

    void wait_ticks(Tick t);
    void
    wait_cycles(Cycles c)
    {
        wait_ticks(clockPeriod() * c);
    }

    // --- Cross-tile rendezvous (delegates to event_bus()) ---

    void wait_on(const std::string &name);
    void notify(const std::string &name);

    // Raw access to this driver's fiber, for sequences that need to
    // hand it to a sync primitive directly.
    SeqThread &
    thread()
    {
        return seq_thread;
    }

    /**
     * Request that this driver's fiber be resumed from the main
     * event loop, not from the caller's fiber context. Used by
     * cross-fiber wakers (Latch::notify, Barrier completion) so the
     * notifying fiber isn't orphaned when control switches to the
     * waiter.
     *
     * Safe to call from within any fiber or from a gem5 event
     * callback; idempotent if a wake is already scheduled.
     */
    void schedule_xfer_wake();

  private:
    enum class WaitMode
    {
        None,           // fiber is currently running or in a non-packet wait
        Blocking,       // fiber is in a blocking read/write/read_exclusive
        SpecificHandle, // fiber is in resolve(h)
        All,            // fiber is in resolve_all()
    };

    class DataPort : public RequestPort
    {
      public:
        DataPort(const std::string &n, ChiSeqDriver &d)
            : RequestPort(n), drv(d)
        {}

      protected:
        bool
        recvTimingResp(PacketPtr pkt) override
        {
            drv.handle_resp(pkt);
            return true;
        }
        void
        recvReqRetry() override
        {
            drv.handle_retry();
        }
        void
        recvTimingSnoopReq(PacketPtr) override
        {}
        void
        recvFunctionalSnoop(PacketPtr) override
        {}
        Tick
        recvAtomicSnoop(PacketPtr) override
        {
            return 0;
        }

      private:
        ChiSeqDriver &drv;
    };

    /**
     * Per-Packet sender state. handle == 0 means "blocking slot",
     * handle > 0 is an async in-flight id (matches an InFlight entry).
     */
    struct DriverSenderState : public Packet::SenderState
    {
        Handle handle;
        uint8_t *read_dst; // nullptr for writes
        uint32_t len;
    };

    struct InFlight
    {
        uint8_t *read_dst; // caller's buffer for reads, nullptr for writes
        uint32_t len;
        bool completed;
    };

    // Packet construction helpers.
    PacketPtr build_read_pkt(uint64_t addr, uint32_t len, MemCmd cmd,
                             Handle handle, uint8_t *read_dst);
    PacketPtr build_write_pkt(uint64_t addr, const uint8_t *src, uint32_t len,
                              Handle handle);

    // Send or queue for retry. Never blocks; always returns.
    void submit(PacketPtr pkt);

    // Port callbacks.
    void handle_resp(PacketPtr pkt);
    void handle_retry();

    // Scheduled-event callbacks.
    void kickoff();
    void on_wake();

    // Common blocking-path bookkeeping.
    void do_blocking(PacketPtr pkt);

    // Common async-path bookkeeping. Allocates a handle, registers
    // the InFlight entry, submits the packet.
    Handle do_async(MemCmd cmd, uint64_t addr, uint8_t *data,
                    const uint8_t *wdata, uint32_t len);

    DataPort data_port;
    SeqThread seq_thread;

    const Params &_p;
    RequestorID requestor_id;

    // Blocking slot.
    bool blocking_done;

    // Async bookkeeping.
    Handle next_handle;
    std::unordered_map<Handle, InFlight> inflight;

    // Current wait state (only meaningful when the fiber is suspended
    // on a packet-response reason — not for timer/latch waits).
    WaitMode wait_mode;
    Handle wait_handle;

    // Retry slot (one-deep is sufficient: we only ever issue one
    // sendTimingReq before the next fiber yield).
    PacketPtr retry_pkt;

    // Scheduled events.
    EventFunctionWrapper kick_event;
    EventFunctionWrapper wake_event;
    EventFunctionWrapper xfer_event;
};

} // namespace chi_gem5tb
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_DRIVER_HH__
