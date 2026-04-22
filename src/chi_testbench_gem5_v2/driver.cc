/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5_v2/driver.hh"

#include <cstring>

#include "base/logging.hh"
#include "base/trace.hh"
#include "chi_testbench_gem5_v2/sequence_context.hh"
#include "chi_testbench_gem5_v2/sequences/registry.hh"
#include "chi_testbench_gem5_v2/sync/barrier.hh"
#include "chi_testbench_gem5_v2/sync/latch.hh"
#include "debug/ChiTestbenchGem5V2.hh"
#include "mem/ruby/protocol/CHI/CHIDataType.hh"
#include "mem/ruby/protocol/CHI/CHIRequestType.hh"
#include "mem/ruby/protocol/CHI/CHIResponseType.hh"

namespace gem5
{
namespace chi_gem5tb_v2
{

using namespace gem5::ruby::CHI;

ChiDriverNode::ChiDriverNode(const Params &p)
    : CHIGenericController(p),
      seq_thread(*this),
      _p(p),
      wait_mode(WaitMode::None),
      wait_addr(0),
      kick_event(
          *this, [this] { kickoff(); }, name() + ".kickoff"),
      wake_event(
          *this, [this] { on_wake(); }, name() + ".on_wake"),
      xfer_event(*this, [this] { on_wake(); }, name() + ".xfer_wake")
{}

ChiDriverNode::~ChiDriverNode() = default;

void
ChiDriverNode::startup()
{
    CHIGenericController::startup();
    schedule(kick_event, curTick());
}

void
ChiDriverNode::kickoff()
{
    DPRINTF(ChiTestbenchGem5V2, "%s: kicking off sequence '%s'\n", name(),
            _p.sequence.c_str());
    seq_thread.run();
}

void
ChiDriverNode::on_wake()
{
    DPRINTF(ChiTestbenchGem5V2, "%s: wake event fires at tick %llu\n", name(),
            (unsigned long long)curTick());
    seq_thread.run();
}

void
ChiDriverNode::schedule_xfer_wake()
{
    if (!xfer_event.scheduled()) {
        schedule(xfer_event, curTick());
    }
}

void
ChiDriverNode::wait_ticks(Tick t)
{
    if (t == 0) {
        return;
    }
    schedule(wake_event, curTick() + t);
    seq_thread.yield_to_primary();
}

void
ChiDriverNode::wait_on(const std::string &name)
{
    ChiEventBus *bus = event_bus();
    panic_if(!bus, "%s: wait_on('%s') requires event_bus param", this->name(),
             name.c_str());
    bus->wait_on(name, seq_thread);
}

void
ChiDriverNode::notify(const std::string &name)
{
    ChiEventBus *bus = event_bus();
    panic_if(!bus, "%s: notify('%s') requires event_bus param", this->name(),
             name.c_str());
    bus->notify(name);
}

// ---------------------------------------------------------------------------
// Routing / helpers
// ---------------------------------------------------------------------------

gem5::ruby::MachineID
ChiDriverNode::tile_machine_id()
{
    const auto &dests = allDownstreamDest();
    panic_if(dests.count() == 0,
             "%s: driver has no downstream_destinations; should point at its "
             "colocated tile cache controller",
             name());
    // CHI_Tile_v2 registers exactly one downstream (the tile). If
    // something else gets wired in, smallestElement() still returns a
    // valid MachineID — the caller is responsible for the routing
    // semantics.
    return dests.smallestElement();
}

uint64_t
ChiDriverNode::line_addr(uint64_t addr) const
{
    return addr & ~(uint64_t)(cacheLineSize - 1);
}

// ---------------------------------------------------------------------------
// Blocking CHI API
// ---------------------------------------------------------------------------

void
ChiDriverNode::read_shared(uint64_t addr, uint8_t *dst, uint32_t len)
{
    const uint64_t laddr = line_addr(addr);
    const uint32_t offset = (uint32_t)(addr - laddr);
    panic_if(offset + len > (uint32_t)cacheLineSize,
             "%s: read_shared addr=%#llx len=%u crosses cache-line boundary",
             name(), (unsigned long long)addr, len);

    auto [it, inserted] = pending_reads.try_emplace(laddr);
    panic_if(!inserted, "%s: read_shared addr=%#llx already in flight", name(),
             (unsigned long long)addr);
    auto &pr = it->second;
    pr.dst = dst;
    pr.len = len;
    pr.offset = offset;
    pr.line.assign(cacheLineSize, 0);
    pr.received = gem5::ruby::WriteMask(cacheLineSize);
    pr.handle = 0;

    // Build the request.
    auto req = std::make_shared<CHIRequestMsg>(curTick(), cacheLineSize,
                                               m_ruby_system);
    req->m_addr = laddr;
    req->m_accAddr = addr;
    req->m_accSize = len;
    req->m_type = CHIRequestType_ReadShared;
    req->m_requestor = getMachineID();
    req->m_Destination.clear();
    req->m_Destination.add(tile_machine_id());
    req->m_usesTxnId = false;
    req->m_allowRetry = true;
    req->m_retToSrc = true;
    req->m_dataToFwdRequestor = false;

    DPRINTF(ChiTestbenchGem5V2, "%s: send ReadShared addr=%#llx dst=%s\n",
            name(), (unsigned long long)laddr,
            tile_machine_id().getType() == gem5::ruby::MachineType_Cache
                ? "tile"
                : "?");

    const bool ok = sendRequestMsg(req);
    panic_if(!ok, "%s: reqOut buffer full on first use — unexpected in step 3",
             name());

    // Suspend fiber until the full line has arrived and CompAck has
    // been sent.
    wait_mode = WaitMode::ReadAddr;
    wait_addr = laddr;
    while (!pr.complete) {
        seq_thread.yield_to_primary();
    }
    wait_mode = WaitMode::None;
    wait_addr = 0;

    if (len > 0 && dst != nullptr) {
        std::memcpy(dst, pr.line.data() + offset, len);
    }
    pending_reads.erase(it);
}

// ---------------------------------------------------------------------------
// CompAck
// ---------------------------------------------------------------------------

void
ChiDriverNode::send_comp_ack(uint64_t addr, gem5::ruby::MachineID responder)
{
    auto rsp = std::make_shared<CHIResponseMsg>(curTick(), cacheLineSize,
                                                m_ruby_system);
    rsp->m_addr = addr;
    rsp->m_type = CHIResponseType_CompAck;
    rsp->m_responder = getMachineID();
    rsp->m_Destination.clear();
    rsp->m_Destination.add(responder);
    rsp->m_usesTxnId = false;

    DPRINTF(ChiTestbenchGem5V2, "%s: send CompAck addr=%#llx\n", name(),
            (unsigned long long)addr);
    const bool ok = sendResponseMsg(rsp);
    panic_if(!ok, "%s: rspOut buffer full when sending CompAck", name());
}

// ---------------------------------------------------------------------------
// WriteUniqueFull / WriteNoSnpFull
// ---------------------------------------------------------------------------

void
ChiDriverNode::write_unique_full(uint64_t addr, const uint8_t *src,
                                 uint32_t len)
{
    write_full_common(addr, src, len, (int)CHIRequestType_WriteUniqueFull);
}

void
ChiDriverNode::write_no_snp_full(uint64_t addr, const uint8_t *src,
                                 uint32_t len)
{
    write_full_common(addr, src, len, (int)CHIRequestType_WriteNoSnp);
}

void
ChiDriverNode::clean_unique(uint64_t addr)
{
    const uint64_t laddr = line_addr(addr);

    auto [it, inserted] = pending_writes.try_emplace(laddr);
    panic_if(!inserted, "%s: clean_unique addr=%#llx already in flight",
             name(), (unsigned long long)addr);
    auto &pw = it->second;
    pw.line.clear();         // no data payload
    pw.expects_data = false; // CleanUnique has no NCBWrData phase
    pw.handle = 0;
    // Pre-set data_sent so the completion predicate only needs the
    // response bit.
    pw.data_sent = true;

    auto req = std::make_shared<CHIRequestMsg>(curTick(), cacheLineSize,
                                               m_ruby_system);
    req->m_addr = laddr;
    req->m_accAddr = laddr;
    req->m_accSize = cacheLineSize;
    req->m_type = CHIRequestType_CleanUnique;
    req->m_requestor = getMachineID();
    req->m_Destination.clear();
    req->m_Destination.add(tile_machine_id());
    req->m_usesTxnId = false;
    req->m_allowRetry = true;
    req->m_retToSrc = false;
    req->m_dataToFwdRequestor = false;

    DPRINTF(ChiTestbenchGem5V2, "%s: send CleanUnique addr=%#llx\n", name(),
            (unsigned long long)laddr);

    const bool ok = sendRequestMsg(req);
    panic_if(!ok, "%s: reqOut buffer full when sending CleanUnique", name());

    wait_mode = WaitMode::WriteAddr;
    wait_addr = laddr;
    while (!pw.complete) {
        seq_thread.yield_to_primary();
    }
    wait_mode = WaitMode::None;
    wait_addr = 0;

    // CleanUnique completion is announced by Comp (not CompDBIDResp);
    // we still need to send CompAck back to the responder so the HN
    // can retire its TBE.
    send_comp_ack(laddr, pw.responder_for_ack);

    pending_writes.erase(it);
}

void
ChiDriverNode::write_full_common(uint64_t addr, const uint8_t *src,
                                 uint32_t len, int chi_req_type)
{
    const uint64_t laddr = line_addr(addr);
    const uint32_t offset = (uint32_t)(addr - laddr);
    panic_if(offset + len > (uint32_t)cacheLineSize,
             "%s: write addr=%#llx len=%u crosses cache-line boundary", name(),
             (unsigned long long)addr, len);

    auto [it, inserted] = pending_writes.try_emplace(laddr);
    panic_if(!inserted, "%s: write addr=%#llx already in flight", name(),
             (unsigned long long)addr);
    auto &pw = it->second;
    pw.line.assign(cacheLineSize, 0);
    if (src && len) {
        std::memcpy(pw.line.data() + offset, src, len);
    }
    pw.handle = 0;

    auto req = std::make_shared<CHIRequestMsg>(curTick(), cacheLineSize,
                                               m_ruby_system);
    req->m_addr = laddr;
    // Full-line write: accAddr points at the line base, accSize is the
    // whole line. Any partial payload was zero-padded when we copied
    // it into `pw.line`. If accSize < cacheLineSize the tile reclassifies
    // the request to WriteUniquePtl and expects only a single
    // bitmask-scoped NCBWrData fragment, which is incompatible with our
    // two-fragment (dataMsgsPerLine=2) send.
    req->m_accAddr = laddr;
    req->m_accSize = cacheLineSize;
    req->m_type = (CHIRequestType)chi_req_type;
    req->m_requestor = getMachineID();
    req->m_Destination.clear();
    req->m_Destination.add(tile_machine_id());
    req->m_usesTxnId = false;
    req->m_allowRetry = true;
    req->m_retToSrc = false;
    req->m_dataToFwdRequestor = false;

    DPRINTF(ChiTestbenchGem5V2, "%s: send %s addr=%#llx\n", name(),
            chi_req_type == (int)CHIRequestType_WriteUniqueFull
                ? "WriteUniqueFull"
                : "WriteNoSnp",
            (unsigned long long)laddr);

    const bool ok = sendRequestMsg(req);
    panic_if(!ok, "%s: reqOut buffer full when sending write request", name());

    wait_mode = WaitMode::WriteAddr;
    wait_addr = laddr;
    while (!pw.complete) {
        seq_thread.yield_to_primary();
    }
    wait_mode = WaitMode::None;
    wait_addr = 0;
    pending_writes.erase(it);
}

// ---------------------------------------------------------------------------
// Non-blocking (async) API
// ---------------------------------------------------------------------------

ChiDriverNode::Handle
ChiDriverNode::async_read_shared(uint64_t addr, uint8_t *dst, uint32_t len)
{
    const uint64_t laddr = line_addr(addr);
    const uint32_t offset = (uint32_t)(addr - laddr);
    panic_if(offset + len > (uint32_t)cacheLineSize,
             "%s: async_read_shared addr=%#llx len=%u crosses line boundary",
             name(), (unsigned long long)addr, len);

    auto [it, inserted] = pending_reads.try_emplace(laddr);
    panic_if(!inserted, "%s: async_read_shared addr=%#llx already in flight",
             name(), (unsigned long long)addr);
    auto &pr = it->second;
    pr.dst = dst;
    pr.len = len;
    pr.offset = offset;
    pr.line.assign(cacheLineSize, 0);
    pr.received = gem5::ruby::WriteMask(cacheLineSize);
    const Handle h = next_handle++;
    pr.handle = h;
    handle_index[h] = HandleInfo{laddr, /*is_write=*/false};

    auto req = std::make_shared<CHIRequestMsg>(curTick(), cacheLineSize,
                                               m_ruby_system);
    req->m_addr = laddr;
    req->m_accAddr = addr;
    req->m_accSize = len;
    req->m_type = CHIRequestType_ReadShared;
    req->m_requestor = getMachineID();
    req->m_Destination.clear();
    req->m_Destination.add(tile_machine_id());
    req->m_usesTxnId = false;
    req->m_allowRetry = true;
    req->m_retToSrc = true;
    req->m_dataToFwdRequestor = false;

    const bool ok = sendRequestMsg(req);
    panic_if(!ok, "%s: reqOut buffer full on async_read_shared", name());
    DPRINTF(ChiTestbenchGem5V2,
            "%s: async ReadShared addr=%#llx handle=%llu\n", name(),
            (unsigned long long)laddr, (unsigned long long)h);
    return h;
}

ChiDriverNode::Handle
ChiDriverNode::async_write_unique_full(uint64_t addr, const uint8_t *src,
                                       uint32_t len)
{
    const uint64_t laddr = line_addr(addr);
    const uint32_t offset = (uint32_t)(addr - laddr);
    panic_if(offset + len > (uint32_t)cacheLineSize,
             "%s: async_write addr=%#llx len=%u crosses line boundary", name(),
             (unsigned long long)addr, len);

    auto [it, inserted] = pending_writes.try_emplace(laddr);
    panic_if(!inserted, "%s: async_write addr=%#llx already in flight", name(),
             (unsigned long long)addr);
    auto &pw = it->second;
    pw.line.assign(cacheLineSize, 0);
    if (src && len) {
        std::memcpy(pw.line.data() + offset, src, len);
    }
    const Handle h = next_handle++;
    pw.handle = h;
    handle_index[h] = HandleInfo{laddr, /*is_write=*/true};

    auto req = std::make_shared<CHIRequestMsg>(curTick(), cacheLineSize,
                                               m_ruby_system);
    req->m_addr = laddr;
    req->m_accAddr = laddr;
    req->m_accSize = cacheLineSize;
    req->m_type = CHIRequestType_WriteUniqueFull;
    req->m_requestor = getMachineID();
    req->m_Destination.clear();
    req->m_Destination.add(tile_machine_id());
    req->m_usesTxnId = false;
    req->m_allowRetry = true;
    req->m_retToSrc = false;
    req->m_dataToFwdRequestor = false;

    const bool ok = sendRequestMsg(req);
    panic_if(!ok, "%s: reqOut buffer full on async_write_unique_full", name());
    DPRINTF(ChiTestbenchGem5V2,
            "%s: async WriteUniqueFull addr=%#llx handle=%llu\n", name(),
            (unsigned long long)laddr, (unsigned long long)h);
    return h;
}

ChiDriverNode::Handle
ChiDriverNode::async_write_no_snp_full(uint64_t addr, const uint8_t *src,
                                       uint32_t len)
{
    // Same wire shape, WriteNoSnp opcode. Kept separate for clarity in
    // scenarios that deliberately suppress snooping.
    const uint64_t laddr = line_addr(addr);
    const uint32_t offset = (uint32_t)(addr - laddr);
    panic_if(offset + len > (uint32_t)cacheLineSize,
             "%s: async_write addr=%#llx len=%u crosses line boundary", name(),
             (unsigned long long)addr, len);

    auto [it, inserted] = pending_writes.try_emplace(laddr);
    panic_if(!inserted, "%s: async_write addr=%#llx already in flight", name(),
             (unsigned long long)addr);
    auto &pw = it->second;
    pw.line.assign(cacheLineSize, 0);
    if (src && len) {
        std::memcpy(pw.line.data() + offset, src, len);
    }
    const Handle h = next_handle++;
    pw.handle = h;
    handle_index[h] = HandleInfo{laddr, /*is_write=*/true};

    auto req = std::make_shared<CHIRequestMsg>(curTick(), cacheLineSize,
                                               m_ruby_system);
    req->m_addr = laddr;
    req->m_accAddr = laddr;
    req->m_accSize = cacheLineSize;
    req->m_type = CHIRequestType_WriteNoSnp;
    req->m_requestor = getMachineID();
    req->m_Destination.clear();
    req->m_Destination.add(tile_machine_id());
    req->m_usesTxnId = false;
    req->m_allowRetry = true;
    req->m_retToSrc = false;
    req->m_dataToFwdRequestor = false;

    const bool ok = sendRequestMsg(req);
    panic_if(!ok, "%s: reqOut buffer full on async_write_no_snp_full", name());
    return h;
}

void
ChiDriverNode::resolve(Handle h)
{
    auto hit = handle_index.find(h);
    panic_if(hit == handle_index.end(), "%s: resolve(%llu) on unknown handle",
             name(), (unsigned long long)h);
    const uint64_t laddr = hit->second.laddr;
    const bool is_write = hit->second.is_write;

    auto completed = [&]() -> bool {
        if (is_write) {
            auto it = pending_writes.find(laddr);
            return it != pending_writes.end() && it->second.complete;
        }
        auto it = pending_reads.find(laddr);
        return it != pending_reads.end() && it->second.complete;
    };

    if (!completed()) {
        wait_mode = WaitMode::Handle;
        wait_handle = h;
        while (!completed()) {
            seq_thread.yield_to_primary();
        }
        wait_mode = WaitMode::None;
        wait_handle = 0;
    }

    if (is_write) {
        pending_writes.erase(laddr);
    } else {
        auto it = pending_reads.find(laddr);
        auto &pr = it->second;
        if (pr.len > 0 && pr.dst != nullptr) {
            std::memcpy(pr.dst, pr.line.data() + pr.offset, pr.len);
        }
        pending_reads.erase(it);
    }
    handle_index.erase(hit);
}

void
ChiDriverNode::resolve_all()
{
    // Wait until every in-flight transaction has completed. Each entry
    // stays in pending_reads/pending_writes until its complete bit is
    // set; we wake and drain below.
    auto all_done = [&]() {
        for (auto &kv : pending_reads) {
            if (!kv.second.complete) {
                return false;
            }
        }
        for (auto &kv : pending_writes) {
            if (!kv.second.complete) {
                return false;
            }
        }
        return true;
    };
    if (!all_done()) {
        wait_mode = WaitMode::All;
        while (!all_done()) {
            seq_thread.yield_to_primary();
        }
        wait_mode = WaitMode::None;
    }
    // Copy read data into caller buffers and erase every pending entry.
    for (auto &kv : pending_reads) {
        auto &pr = kv.second;
        if (pr.len > 0 && pr.dst != nullptr) {
            std::memcpy(pr.dst, pr.line.data() + pr.offset, pr.len);
        }
        if (pr.handle) {
            handle_index.erase(pr.handle);
        }
    }
    pending_reads.clear();
    for (auto &kv : pending_writes) {
        if (kv.second.handle) {
            handle_index.erase(kv.second.handle);
        }
    }
    pending_writes.clear();
}

void
ChiDriverNode::send_ncb_wrdata(uint64_t addr, const uint8_t *line_data,
                               gem5::ruby::MachineID destination,
                               uint64_t txn_id)
{
    // Emit dataMsgsPerLine fragments, each carrying dataChannelSize
    // bytes' worth of bitMask. Every fragment carries the same
    // DataBlock for simplicity (gem5's network layer re-reads bytes
    // by bitMask regardless, so there is no correctness issue).
    for (int frag = 0; frag < dataMsgsPerLine; ++frag) {
        auto d = std::make_shared<CHIDataMsg>(curTick(), cacheLineSize,
                                              m_ruby_system);
        d->m_addr = addr;
        d->m_type = CHIDataType_NCBWrData;
        d->m_responder = getMachineID();
        d->m_Destination.clear();
        d->m_Destination.add(destination);
        // datInPort dispatches by address; txnId on data messages is
        // rejected by the CHI cache controller (see CHI-cache-ports.sm:108).
        d->m_usesTxnId = false;
        (void)txn_id;

        const int start = frag * dataChannelSize;
        for (int i = 0; i < dataChannelSize; ++i) {
            d->m_dataBlk.setByte(start + i, line_data[start + i]);
        }
        d->m_bitMask = gem5::ruby::WriteMask(cacheLineSize);
        d->m_bitMask.setMask(start, dataChannelSize, true);

        const bool ok = sendDataMsg(d);
        panic_if(!ok, "%s: datOut buffer full when sending NCBWrData", name());
    }
}

// ---------------------------------------------------------------------------
// CHIGenericController dispatch overrides.
// ---------------------------------------------------------------------------

bool
ChiDriverNode::recvRequestMsg(const CHIRequestMsg *msg)
{
    // Driver does not act as a Home Node in step 3, so reqIn should be
    // idle. Drop + warn if something unexpectedly lands here.
    DPRINTF(ChiTestbenchGem5V2,
            "%s: unexpected CHIRequestMsg on reqIn (addr=%#llx type=%d)\n",
            name(), (unsigned long long)msg->m_addr, (int)msg->m_type);
    return true;
}

bool
ChiDriverNode::recvSnoopMsg(const CHIRequestMsg *msg)
{
    // The driver is a cache-less node — it never holds a line in a
    // cached state, so every snoop is answered with SnpResp_I ("I
    // don't have it"). That's the same response a real RN would emit
    // from state I, and it's the simplest correct behavior even for
    // snoops that set retToSrc (we legitimately have nothing to
    // return).
    auto rsp = std::make_shared<CHIResponseMsg>(curTick(), cacheLineSize,
                                                m_ruby_system);
    rsp->m_addr = msg->m_addr;
    rsp->m_type = CHIResponseType_SnpResp_I;
    rsp->m_responder = getMachineID();
    rsp->m_Destination.clear();
    rsp->m_Destination.add(msg->m_requestor);
    rsp->m_usesTxnId = msg->m_usesTxnId;
    rsp->m_txnId = msg->m_txnId;

    DPRINTF(ChiTestbenchGem5V2, "%s: snoop addr=%#llx type=%d → SnpResp_I\n",
            name(), (unsigned long long)msg->m_addr, (int)msg->m_type);

    const bool ok = sendResponseMsg(rsp);
    panic_if(!ok, "%s: rspOut buffer full when sending SnpResp_I", name());
    return true;
}

bool
ChiDriverNode::recvResponseMsg(const CHIResponseMsg *msg)
{
    const uint64_t laddr = msg->m_addr;
    const auto type = msg->m_type;

    // Write flows: CompDBIDResp (combined), DBIDResp + Comp (split),
    // or plain Comp (after DBIDResp).
    auto wit = pending_writes.find(laddr);
    if (wit != pending_writes.end()) {
        auto &pw = wit->second;
        DPRINTF(ChiTestbenchGem5V2,
                "%s: rsp addr=%#llx type=%d (write flow)\n", name(),
                (unsigned long long)laddr, (int)type);

        auto on_dbid = [&](gem5::ruby::MachineID responder, uint64_t dbid) {
            if (!pw.data_sent) {
                send_ncb_wrdata(laddr, pw.line.data(), responder, dbid);
                pw.data_sent = true;
            }
            pw.dbid_received = true;
        };

        const bool is_comp_like = type == CHIResponseType_Comp ||
                                  type == CHIResponseType_Comp_I ||
                                  type == CHIResponseType_Comp_UC ||
                                  type == CHIResponseType_Comp_SC ||
                                  type == CHIResponseType_Comp_UD_PD;

        if (type == CHIResponseType_CompDBIDResp) {
            on_dbid(msg->m_responder, msg->m_dbid);
            pw.comp_received = true;
            pw.responder_for_ack = msg->m_responder;
            pw.responder_set = true;
        } else if (type == CHIResponseType_DBIDResp) {
            on_dbid(msg->m_responder, msg->m_dbid);
        } else if (is_comp_like) {
            pw.comp_received = true;
            pw.responder_for_ack = msg->m_responder;
            pw.responder_set = true;
        } else {
            DPRINTF(ChiTestbenchGem5V2,
                    "%s: unexpected write-side rsp type=%d — dropping\n",
                    name(), (int)type);
        }

        const bool data_phase_done =
            !pw.expects_data || (pw.dbid_received && pw.data_sent);
        if (data_phase_done && pw.comp_received) {
            pw.complete = true;

            bool should_wake = false;
            switch (wait_mode) {
                case WaitMode::WriteAddr:
                    should_wake = (wait_addr == laddr);
                    break;
                case WaitMode::Handle:
                    should_wake = (wait_handle == pw.handle);
                    break;
                case WaitMode::All:
                    should_wake = true;
                    break;
                default:
                    break;
            }
            if (should_wake) {
                seq_thread.run();
            }
        }
        return true;
    }

    DPRINTF(ChiTestbenchGem5V2,
            "%s: CHIResponseMsg on rspIn (addr=%#llx type=%d) — no pending "
            "transaction, dropping\n",
            name(), (unsigned long long)laddr, (int)type);
    return true;
}

bool
ChiDriverNode::recvDataMsg(const CHIDataMsg *msg)
{
    const uint64_t laddr = msg->m_addr;
    auto it = pending_reads.find(laddr);
    if (it == pending_reads.end()) {
        DPRINTF(ChiTestbenchGem5V2,
                "%s: CompData for %#llx with no pending read — dropping\n",
                name(), (unsigned long long)laddr);
        return true;
    }
    auto &pr = it->second;

    // Merge this fragment into the accumulated data / mask. The
    // incoming DataBlock already holds bytes indexed by position
    // within the cache line; copy only those the bitMask marks valid.
    for (int i = 0; i < cacheLineSize; ++i) {
        if (msg->m_bitMask.test(i)) {
            pr.line[i] = msg->m_dataBlk.getByte(i);
        }
    }
    pr.received.orMask(msg->m_bitMask);
    if (!pr.responder_set) {
        pr.responder = msg->m_responder;
        pr.responder_set = true;
    }

    DPRINTF(ChiTestbenchGem5V2,
            "%s: CompData addr=%#llx type=%d fragment (mask full? %d)\n",
            name(), (unsigned long long)laddr, (int)msg->m_type,
            (int)pr.received.isFull());

    if (pr.received.isFull()) {
        // Full line received — acknowledge and wake the fiber if it's
        // waiting on something this completion satisfies.
        send_comp_ack(laddr, pr.responder);
        pr.complete = true;

        bool should_wake = false;
        switch (wait_mode) {
            case WaitMode::ReadAddr:
                should_wake = (wait_addr == laddr);
                break;
            case WaitMode::Handle:
                should_wake = (wait_handle == pr.handle);
                break;
            case WaitMode::All:
                should_wake = true; // resolve_all() re-checks under the loop
                break;
            default:
                break;
        }
        if (should_wake) {
            seq_thread.run();
        }
    }
    return true;
}

} // namespace chi_gem5tb_v2
} // namespace gem5

// ---------------------------------------------------------------------------
// SeqThread::main — needs full ChiDriverNode definition.
// ---------------------------------------------------------------------------

namespace gem5
{
namespace chi_gem5tb_v2
{

void
SeqThread::main()
{
    ChiDriverNode &d = drv();
    const auto *fn = SequenceRegistry::instance().find(d.sequence_name());
    if (!fn) {
        panic("ChiDriverNode %s: no sequence registered for '%s'",
              d.name().c_str(), d.sequence_name().c_str());
    }
    SequenceContext ctx{d};
    (*fn)(ctx);

    if (ChiBarrier *b = d.finish_barrier()) {
        b->signal_finish();
    }
}

} // namespace chi_gem5tb_v2
} // namespace gem5
