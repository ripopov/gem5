#include "sim/transaction_trace/ftr_trace.hh"

#include <fstream>
#include <memory>

#include "base/output.hh"
#include "base/trace.hh"
#include "debug/TxTrace.hh"
#include "tx_trace/tx_text_writer.hh"

namespace gem5
{

FtrTrace *FtrTrace::instance = nullptr;

FtrTrace::FtrTrace(const Params &p) : SimObject(p)
{
    fatal_if(instance != nullptr, "Only one FtrTrace instance is allowed.");

    const std::string &format = p.output_format;
    const std::string &base = p.output_file;

    if (format == "text") {
        std::string path = simout.resolve(base + ".txlog");
        auto writer = std::make_unique<tx_trace::TxTextWriter>(path);
        trace_ = std::make_unique<tx_trace::TxTrace>(std::move(writer));
    } else {
        fatal("FtrTrace: unsupported output_format '%s'. "
              "Use 'text'.",
              format);
    }

    instance = this;
    DPRINTF(TxTrace, "FtrTrace initialized, output: %s.txlog\n", base);
}

FtrTrace::~FtrTrace()
{
    if (trace_) {
        trace_->flush();
    }
    instance = nullptr;
}

DrainState
FtrTrace::drain()
{
    if (trace_) {
        trace_->flush();
    }
    return DrainState::Drained;
}

// ---- Filter policy ----

void
FtrTrace::setRootFilter(RootFilter filter)
{
    rootFilter_ = std::move(filter);
}

bool
FtrTrace::shouldTrace(PacketPtr pkt, TimingResponseProtocol *peer) const
{
    if (rootFilter_) {
        return rootFilter_(pkt, peer);
    }
    return false;
}

// ---- Root lifecycle ----

TraceId
FtrTrace::createPendingRoot(SimObject *sequencer, Tick tick)
{
    TraceId id = trace_->reserveId();
    pendingRoots_[id] = {sequencer, tick};
    DPRINTF(TxTrace, "Pending root %lu created at tick %lu\n", id, tick);
    return id;
}

bool
FtrTrace::isPending(TraceId id) const
{
    return pendingRoots_.count(id) > 0;
}

void
FtrTrace::finalizeRoot(TraceId id, SimObject *sequencer,
                       std::string_view objectName, Tick tick,
                       const tx_trace::AttrList &attrs)
{
    auto it = pendingRoots_.find(id);
    assert(it != pendingRoots_.end());

    // Use the sequencer from finalization (more reliable than port-time).
    SimObject *seq = sequencer ? sequencer : it->second.sequencer;
    Tick origin = it->second.originTick;
    pendingRoots_.erase(it);

    const StreamInfo &si = getOrCreateStream(seq);

    trace_->createRootTransactionWithId(id, si.memreqGen, origin, attrs);
    liveTxs_[id] = {si.memreqEvtGen, si.flitGen, si.flitEvtGen, false};

    DPRINTF(TxTrace, "Root %lu finalized at %s tick %lu\n", id, objectName,
            tick);
}

void
FtrTrace::discardPendingRoot(TraceId id)
{
    auto it = pendingRoots_.find(id);
    if (it != pendingRoots_.end()) {
        pendingRoots_.erase(it);
        DPRINTF(TxTrace, "Pending root %lu discarded\n", id);
    }
}

// ---- Events and retirement ----

void
FtrTrace::stampEvent(TraceId id, std::string_view eventKind,
                     std::string_view objectName, Tick tick,
                     const tx_trace::AttrList &attrs)
{
    const TxInfo *info = findTxInfo(id);
    if (info == nullptr) {
        DPRINTF(TxTrace,
                "stampEvent: TraceId %lu not in live table, "
                "skipping '%s'\n",
                id, eventKind);
        return;
    }

    trace_->stampEvent(id, info->evtGenId, eventKind, objectName, tick, attrs);
}

void
FtrTrace::retireTransaction(TraceId id, std::string_view objectName, Tick tick,
                            const tx_trace::AttrList &attrs)
{
    auto it = liveTxs_.find(id);
    if (it == liveTxs_.end()) {
        DPRINTF(TxTrace,
                "retireTransaction: TraceId %lu not in live "
                "table, skipping\n",
                id);
        return;
    }

    trace_->retireTransaction(id, tick, attrs);
    if (it->second.isFlit) {
        liveTxs_.erase(it);
    } else {
        retiredRoots_[id] = it->second;
        liveTxs_.erase(it);
    }

    DPRINTF(TxTrace, "Transaction %lu retired at %s tick %lu\n", id,
            objectName, tick);
}

// ---- Flit children ----

TraceId
FtrTrace::createFlitChild(TraceId parentRoot, std::string_view objectName,
                          Tick tick, const tx_trace::AttrList &attrs)
{
    const TxInfo *info = findTxInfo(parentRoot);
    if (info == nullptr) {
        DPRINTF(TxTrace,
                "createFlitChild: parent TraceId %lu not available, "
                "skipping child creation at %s\n",
                parentRoot, objectName);
        return 0;
    }

    TraceId flitId = trace_->reserveId();
    trace_->createChildTransactionWithId(flitId, info->flitGenId, parentRoot,
                                         tick, attrs);
    liveTxs_[flitId] = {info->flitEvtGenId, 0, 0, true};

    DPRINTF(TxTrace, "Flit %lu (parent %lu) created at %s tick %lu\n", flitId,
            parentRoot, objectName, tick);
    return flitId;
}

// ---- Stream management ----

const FtrTrace::TxInfo *
FtrTrace::findTxInfo(TraceId id) const
{
    auto live = liveTxs_.find(id);
    if (live != liveTxs_.end()) {
        return &live->second;
    }

    auto retired = retiredRoots_.find(id);
    if (retired != retiredRoots_.end()) {
        return &retired->second;
    }

    return nullptr;
}

const FtrTrace::StreamInfo &
FtrTrace::getOrCreateStream(SimObject *sequencer)
{
    auto it = streams_.find(sequencer);
    if (it != streams_.end()) {
        return it->second;
    }

    std::string streamName = sequencer->name();
    uint64_t sid = trace_->addStream(streamName, "Sequencer");
    uint64_t memreqGen = trace_->addGeneratorPair(sid, "memreq");
    uint64_t flitGen = trace_->addGeneratorPair(sid, "flit");

    StreamInfo si{sid, memreqGen, memreqGen + 1, flitGen, flitGen + 1};
    auto [ins, ok] = streams_.emplace(sequencer, si);
    assert(ok);

    DPRINTF(TxTrace, "Stream created: %s (id %lu)\n", streamName, sid);
    return ins->second;
}

} // namespace gem5
