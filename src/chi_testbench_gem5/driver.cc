/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/driver.hh"

#include <cstring>

#include "base/logging.hh"
#include "base/trace.hh"
#include "chi_testbench_gem5/sequences/base.hh"
#include "chi_testbench_gem5/sync/barrier.hh"
#include "chi_testbench_gem5/sync/latch.hh"
#include "debug/ChiTestbenchGem5.hh"
#include "sim/system.hh"

namespace gem5
{
namespace chi_gem5tb
{

ChiSeqDriver::ChiSeqDriver(const Params &p)
    : ClockedObject(p),
      data_port(name() + ".port", *this),
      seq_thread(*this),
      _p(p),
      requestor_id(p.system->getRequestorId(this)),
      blocking_done(false),
      next_handle(1),
      wait_mode(WaitMode::None),
      wait_handle(0),
      retry_pkt(nullptr),
      kick_event(
          *this, [this] { kickoff(); }, name() + ".kickoff"),
      wake_event(
          *this, [this] { on_wake(); }, name() + ".on_wake"),
      xfer_event(*this, [this] { on_wake(); }, name() + ".xfer_wake")
{}

void
ChiSeqDriver::schedule_xfer_wake()
{
    if (!xfer_event.scheduled()) {
        schedule(xfer_event, curTick());
    }
}

ChiSeqDriver::~ChiSeqDriver() = default;

Port &
ChiSeqDriver::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port") {
        return data_port;
    }
    return ClockedObject::getPort(if_name, idx);
}

void
ChiSeqDriver::startup()
{
    ClockedObject::startup();
    // Fire the fiber on the first simulated tick rather than from the
    // constructor; by then the event queue is alive and sendTimingReq
    // is safe to call.
    schedule(kick_event, curTick());
}

void
ChiSeqDriver::kickoff()
{
    DPRINTF(ChiTestbenchGem5, "%s: kicking off sequence '%s'\n", name(),
            _p.sequence ? _p.sequence->name().c_str() : "<null>");
    seq_thread.run();
}

void
ChiSeqDriver::on_wake()
{
    DPRINTF(ChiTestbenchGem5, "%s: wake event fires at tick %llu\n", name(),
            (unsigned long long)curTick());
    seq_thread.run();
}

// ---------------------------------------------------------------------------
// Packet construction
// ---------------------------------------------------------------------------

PacketPtr
ChiSeqDriver::build_read_pkt(uint64_t addr, uint32_t len, MemCmd cmd,
                             Handle handle, uint8_t *read_dst)
{
    auto req = std::make_shared<Request>(addr, len, /*flags*/ 0, requestor_id);
    auto *pkt = new Packet(req, cmd);
    pkt->dataDynamic(new uint8_t[len]);
    auto *ss = new DriverSenderState{};
    ss->handle = handle;
    ss->read_dst = read_dst;
    ss->len = len;
    pkt->pushSenderState(ss);
    return pkt;
}

PacketPtr
ChiSeqDriver::build_write_pkt(uint64_t addr, const uint8_t *src, uint32_t len,
                              Handle handle)
{
    auto req = std::make_shared<Request>(addr, len, /*flags*/ 0, requestor_id);
    auto *pkt = new Packet(req, MemCmd::WriteReq);
    auto *buf = new uint8_t[len];
    std::memcpy(buf, src, len);
    pkt->dataDynamic(buf);
    auto *ss = new DriverSenderState{};
    ss->handle = handle;
    ss->read_dst = nullptr;
    ss->len = len;
    pkt->pushSenderState(ss);
    return pkt;
}

void
ChiSeqDriver::submit(PacketPtr pkt)
{
    if (!data_port.sendTimingReq(pkt)) {
        // The sequencer rejected the request (BufferFull). Stash and
        // wait for recvReqRetry(). The fiber stays suspended; retry
        // is transparent from its POV.
        if (retry_pkt) {
            panic("%s: retry slot already occupied (issuer fires faster "
                  "than the sequencer accepts retries)",
                  name());
        }
        retry_pkt = pkt;
        DPRINTF(ChiTestbenchGem5, "%s: sendTimingReq stalled, retry queued\n",
                name());
    }
}

void
ChiSeqDriver::handle_retry()
{
    DPRINTF(ChiTestbenchGem5, "%s: recvReqRetry\n", name());
    panic_if(!retry_pkt, "%s: recvReqRetry with no pending packet", name());
    PacketPtr pkt = retry_pkt;
    retry_pkt = nullptr;
    if (!data_port.sendTimingReq(pkt)) {
        // Rejected again: put it back. The sequencer will call us
        // again once it has space.
        retry_pkt = pkt;
    }
}

// ---------------------------------------------------------------------------
// Blocking API
// ---------------------------------------------------------------------------

void
ChiSeqDriver::do_blocking(PacketPtr pkt)
{
    blocking_done = false;
    wait_mode = WaitMode::Blocking;
    submit(pkt);
    // Loop to be robust against spurious wakes — currently the only
    // thing that wakes us during Blocking is our own response, but
    // keeping the loop is cheap and clarifies intent.
    while (!blocking_done) {
        seq_thread.yield_to_primary();
    }
    wait_mode = WaitMode::None;
}

void
ChiSeqDriver::read(uint64_t addr, uint8_t *buf, uint32_t len)
{
    do_blocking(build_read_pkt(addr, len, MemCmd::ReadReq, 0, buf));
}

void
ChiSeqDriver::read_exclusive(uint64_t addr, uint8_t *buf, uint32_t len)
{
    // MemCmd::ReadExReq signals exclusive-intent load, which the
    // Ruby sequencer maps to a CHI ReadUnique on the wire — the same
    // opcode a core would emit for a load that plans to modify.
    do_blocking(build_read_pkt(addr, len, MemCmd::ReadExReq, 0, buf));
}

void
ChiSeqDriver::write(uint64_t addr, const uint8_t *buf, uint32_t len)
{
    do_blocking(build_write_pkt(addr, buf, len, 0));
}

// ---------------------------------------------------------------------------
// Non-blocking API
// ---------------------------------------------------------------------------

ChiSeqDriver::Handle
ChiSeqDriver::do_async(MemCmd cmd, uint64_t addr, uint8_t *read_dst,
                       const uint8_t *wdata, uint32_t len)
{
    const Handle h = next_handle++;
    PacketPtr pkt = (cmd == MemCmd::WriteReq)
                        ? build_write_pkt(addr, wdata, len, h)
                        : build_read_pkt(addr, len, cmd, h, read_dst);

    inflight.emplace(h, InFlight{read_dst, len, false});
    submit(pkt);
    return h;
}

ChiSeqDriver::Handle
ChiSeqDriver::async_read(uint64_t addr, uint8_t *out, uint32_t len)
{
    return do_async(MemCmd::ReadReq, addr, out, nullptr, len);
}

ChiSeqDriver::Handle
ChiSeqDriver::async_read_exclusive(uint64_t addr, uint8_t *out, uint32_t len)
{
    return do_async(MemCmd::ReadExReq, addr, out, nullptr, len);
}

ChiSeqDriver::Handle
ChiSeqDriver::async_write(uint64_t addr, const uint8_t *buf, uint32_t len)
{
    return do_async(MemCmd::WriteReq, addr, nullptr, buf, len);
}

void
ChiSeqDriver::resolve(Handle h)
{
    auto it = inflight.find(h);
    if (it == inflight.end()) {
        panic("%s: resolve() on unknown handle %llu", name(),
              (unsigned long long)h);
    }
    if (!it->second.completed) {
        wait_mode = WaitMode::SpecificHandle;
        wait_handle = h;
        seq_thread.yield_to_primary();
        wait_mode = WaitMode::None;
        wait_handle = 0;
    }
    inflight.erase(h);
}

void
ChiSeqDriver::resolve_all()
{
    bool any_pending = false;
    for (auto &kv : inflight) {
        if (!kv.second.completed) {
            any_pending = true;
            break;
        }
    }
    if (any_pending) {
        wait_mode = WaitMode::All;
        seq_thread.yield_to_primary();
        wait_mode = WaitMode::None;
    }
    inflight.clear();
}

// ---------------------------------------------------------------------------
// Timer / response plumbing
// ---------------------------------------------------------------------------

void
ChiSeqDriver::wait_ticks(Tick t)
{
    if (t == 0) {
        return;
    }
    schedule(wake_event, curTick() + t);
    seq_thread.yield_to_primary();
}

void
ChiSeqDriver::wait_on(const std::string &name)
{
    ChiEventBus *bus = event_bus();
    panic_if(!bus, "%s: wait_on('%s') requires event_bus param", this->name(),
             name.c_str());
    bus->wait_on(name, seq_thread);
}

void
ChiSeqDriver::notify(const std::string &name)
{
    ChiEventBus *bus = event_bus();
    panic_if(!bus, "%s: notify('%s') requires event_bus param", this->name(),
             name.c_str());
    bus->notify(name);
}

void
ChiSeqDriver::handle_resp(PacketPtr pkt)
{
    auto *ss = dynamic_cast<DriverSenderState *>(pkt->popSenderState());
    panic_if(!ss, "%s: response missing DriverSenderState", name());

    const Handle h = ss->handle;
    uint8_t *dst = ss->read_dst;
    const uint32_t len = ss->len;
    const bool is_read = pkt->isRead();
    delete ss;

    if (pkt->isError()) {
        panic("%s: error response for addr %#llx", name(),
              (unsigned long long)pkt->getAddr());
    }
    if (is_read && dst) {
        std::memcpy(dst, pkt->getPtr<uint8_t>(), len);
    }
    delete pkt;

    bool should_wake = false;
    if (h == 0) {
        blocking_done = true;
        if (wait_mode == WaitMode::Blocking) {
            should_wake = true;
        }
    } else {
        auto it = inflight.find(h);
        panic_if(it == inflight.end(), "%s: response for unknown handle %llu",
                 name(), (unsigned long long)h);
        it->second.completed = true;
        if (wait_mode == WaitMode::SpecificHandle && wait_handle == h) {
            should_wake = true;
        } else if (wait_mode == WaitMode::All) {
            bool any_pending = false;
            for (auto &kv : inflight) {
                if (!kv.second.completed) {
                    any_pending = true;
                    break;
                }
            }
            if (!any_pending) {
                should_wake = true;
            }
        }
    }

    if (should_wake) {
        seq_thread.run();
    }
}

} // namespace chi_gem5tb
} // namespace gem5

// ---------------------------------------------------------------------------
// SeqThread::main — needs full ChiSeqDriver definition.
// ---------------------------------------------------------------------------

namespace gem5
{
namespace chi_gem5tb
{

void
SeqThread::main()
{
    ChiSeqDriver &d = drv();
    ChiSequence *seq = d.sequence();
    panic_if(!seq, "ChiSeqDriver %s: sequence param is null", d.name());
    seq->run(d);

    if (ChiBarrier *b = d.finish_barrier()) {
        b->signal_finish();
    }
    // main() returns → fiber auto-finishes and yields to primary link.
}

} // namespace chi_gem5tb
} // namespace gem5
