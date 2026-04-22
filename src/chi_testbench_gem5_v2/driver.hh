/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_V2_DRIVER_HH__
#define __CHI_TESTBENCH_GEM5_V2_DRIVER_HH__

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "chi_testbench_gem5_v2/seq_thread.hh"
#include "mem/ruby/common/MachineID.hh"
#include "mem/ruby/common/NetDest.hh"
#include "mem/ruby/common/WriteMask.hh"
#include "mem/ruby/protocol/chi/generic/CHIGenericController.hh"
#include "params/ChiDriverNode.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace chi_gem5tb_v2
{

class ChiBarrier;
class ChiEventBus;

/**
 * Synthetic upstream CHI requester.
 *
 * ChiDriverNode is a real Ruby network node (subclass of
 * `gem5::ruby::CHIGenericController`): Garnet wires its eight CHI
 * MessageBuffers through the interconnect the same way any
 * SLICC-generated controller's are. What's different is the behavior
 * layer: instead of a SLICC automaton, a Fiber-hosted sequence body
 * runs inside the driver. The sequence constructs `CHIRequestMsg` /
 * `CHIResponseMsg` / `CHIDataMsg` directly and asks the driver to
 * enqueue them on the outbound buffers. Inbound messages wake the
 * driver's fiber.
 *
 * API surface (blocking helpers — call from inside the fiber):
 *
 *   read_shared(addr, dst, len)  — ReadShared → wait for CompData →
 *                                   send CompAck → return
 *
 * Responses are matched to in-flight transactions by line address
 * (one in-flight per line at a time in step 3; step 6 will add a
 * txnID-based pipeline).
 */
class ChiDriverNode : public gem5::ruby::CHIGenericController
{
  public:
    using Params = ChiDriverNodeParams;

    explicit ChiDriverNode(const Params &p);
    ~ChiDriverNode() override;

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
    const std::string &
    sequence_name() const
    {
        return _p.sequence;
    }

    SeqThread &
    thread()
    {
        return seq_thread;
    }

    void schedule_xfer_wake();

    // --- Cross-tile rendezvous (delegates to event_bus()) ---

    void wait_on(const std::string &name);
    void notify(const std::string &name);

    // --- Time / housekeeping ---

    void wait_ticks(Tick t);
    void
    wait_cycles(Cycles c)
    {
        wait_ticks(clockPeriod() * c);
    }

    // --- Blocking CHI API ---

    /**
     * Issue a CHI ReadShared to `addr`, wait until CompData has
     * been fully assembled and CompAck has been sent back, then
     * copy `len` bytes from line offset (addr & (cacheLineSize-1))
     * into `dst` and return.
     */
    void read_shared(uint64_t addr, uint8_t *dst, uint32_t len);

    /**
     * Issue a CHI WriteUniqueFull for the line containing `addr`:
     * full-line write without prior RN cache state. Pads the line
     * with zeros outside `[offset, offset+len)`. Blocks until
     * CompDBIDResp arrives and NCBWrData has been emitted; for a
     * split DBIDResp + Comp flow, blocks additionally until Comp.
     */
    void write_unique_full(uint64_t addr, const uint8_t *src, uint32_t len);

    /**
     * Issue a CHI WriteNoSnp (no-snoop full-line write). Same wire
     * shape as write_unique_full but uses the WriteNoSnp opcode,
     * natural for `rni`-mode tiles.
     */
    void write_no_snp_full(uint64_t addr, const uint8_t *src, uint32_t len);

    /**
     * Issue a CHI CleanUnique for `addr` — an ownership-upgrade
     * request with no data transfer. The responder sends back a
     * Comp (or CompAck-merged variant); the driver then emits
     * CompAck and returns.
     *
     * This is the "fancy" CHI opcode of the atomic_rmw scenario:
     * emitting a bare CleanUnique from a requestor that does not
     * actually hold the line is not reachable from the v1 CPU/
     * RubySequencer stimulus path, because the sequencer collapses
     * all MemCmd flavors that might lead to CleanUnique.
     */
    void clean_unique(uint64_t addr);

    // --- Non-blocking (async) CHI API ---

    using Handle = uint64_t;

    /** Issue an async ReadShared; returns a handle that resolve()
     *  waits on. Each outstanding async transaction must target a
     *  distinct cache line (one in-flight per line address). */
    Handle async_read_shared(uint64_t addr, uint8_t *dst, uint32_t len);

    /** Issue an async WriteUniqueFull; same handle rules. */
    Handle async_write_unique_full(uint64_t addr, const uint8_t *src,
                                   uint32_t len);

    /** Issue an async WriteNoSnp; same handle rules. */
    Handle async_write_no_snp_full(uint64_t addr, const uint8_t *src,
                                   uint32_t len);

    /** Block the fiber until the transaction identified by `h` has
     *  completed. Each handle may be resolved at most once. */
    void resolve(Handle h);

    /** Block the fiber until all in-flight transactions complete. */
    void resolve_all();

    std::size_t
    outstanding() const
    {
        return pending_reads.size() + pending_writes.size();
    }

  protected:
    // CHIGenericController dispatch (called from wakeup()). Returning
    // `true` dequeues the message; `false` would keep it for a later
    // retry. Everything our driver receives is normal response/data
    // traffic so we always return true.
    bool recvRequestMsg(const CHIRequestMsg *msg) override;
    bool recvSnoopMsg(const CHIRequestMsg *msg) override;
    bool recvResponseMsg(const CHIResponseMsg *msg) override;
    bool recvDataMsg(const CHIDataMsg *msg) override;

  private:
    void kickoff();
    void on_wake();

    // The tile controller (our single downstream). Resolved at
    // startup() once all controllers are constructed.
    gem5::ruby::MachineID tile_machine_id();

    // Construct a line-address mask (addr aligned down to cacheLineSize).
    uint64_t line_addr(uint64_t addr) const;

    // Send a CompAck response for `addr` back to `responder`. Called
    // from recvDataMsg once the full line has arrived.
    void send_comp_ack(uint64_t addr, gem5::ruby::MachineID responder);

    // Issue a write-class request (used by write_unique_full and
    // write_no_snp_full).
    void write_full_common(uint64_t addr, const uint8_t *src, uint32_t len,
                           int chi_req_type);

    // Send the NCBWrData fragments for a completed write.
    void send_ncb_wrdata(uint64_t addr, const uint8_t *line_data,
                         gem5::ruby::MachineID destination, uint64_t txn_id);

    // Per-address pending-read state.
    struct PendingRead
    {
        uint8_t *dst = nullptr;
        uint32_t len = 0;
        uint32_t offset = 0;
        std::vector<uint8_t> line;
        gem5::ruby::WriteMask received;
        gem5::ruby::MachineID responder;
        bool responder_set = false;
        bool complete = false;
        Handle handle = 0; // 0 = blocking, !=0 = async
    };

    // Per-address pending-write state. A write completes when both
    // the data fragments have been emitted AND the completion signal
    // has been received (CompDBIDResp implies both at once).
    struct PendingWrite
    {
        std::vector<uint8_t> line;
        bool dbid_received = false;
        bool comp_received = false;
        bool data_sent = false;
        bool complete = false;
        Handle handle = 0;
        bool expects_data = true; // false for CleanUnique / Evict / atomics
        gem5::ruby::MachineID responder_for_ack;
        bool responder_set = false;
    };

    enum class WaitMode
    {
        None,
        ReadAddr,
        WriteAddr,
        Handle,
        All,
    };

    SeqThread seq_thread;
    const Params &_p;

    std::unordered_map<uint64_t, PendingRead> pending_reads;
    std::unordered_map<uint64_t, PendingWrite> pending_writes;

    // Async-handle accounting. Handle value 0 is reserved for
    // blocking-path transactions (which don't need handle → addr
    // lookup since the blocking caller holds the iterator directly).
    Handle next_handle = 1;
    struct HandleInfo
    {
        uint64_t laddr;
        bool is_write;
    };
    std::unordered_map<Handle, HandleInfo> handle_index;

    WaitMode wait_mode;
    uint64_t wait_addr;
    Handle wait_handle = 0;

    EventFunctionWrapper kick_event;
    EventFunctionWrapper wake_event;
    EventFunctionWrapper xfer_event;
};

} // namespace chi_gem5tb_v2
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_V2_DRIVER_HH__
