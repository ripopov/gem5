#include "tx_trace/tx_trace.hh"

#include <cassert>

namespace tx_trace
{

TxTrace::TxTrace(std::unique_ptr<TxWriter> writer) : writer_(std::move(writer))
{
    assert(writer_);
}

TxTrace::~TxTrace()
{
    flush();
}

uint64_t
TxTrace::addStream(std::string_view name, std::string_view kind)
{
    uint64_t stream_id = nextStreamId_++;
    writer_->writeStream(stream_id, name, kind);
    return stream_id;
}

uint64_t
TxTrace::addGeneratorPair(uint64_t stream_id, std::string_view name)
{
    uint64_t gen_id = nextGenId_++;
    uint64_t evt_gen_id = nextGenId_++;
    writer_->writeGenerator(gen_id, name, stream_id);
    std::string evt_name = std::string(name) + ".events";
    writer_->writeGenerator(evt_gen_id, evt_name, stream_id);
    return gen_id;
}

TraceId
TxTrace::allocateId()
{
    return nextId_.fetch_add(1, std::memory_order_relaxed);
}

void
TxTrace::writeAttrs(uint64_t tx_id, const AttrList &attrs)
{
    for (const auto &attr : attrs) {
        writer_->writeAttribute(tx_id, attr.name, attr.value);
    }
}

TraceId
TxTrace::createRootTransaction(uint64_t gen_id, Tick tick,
                               const AttrList &attrs)
{
    TraceId id = allocateId();
    writer_->startTransaction(id, gen_id, tick);
    writeAttrs(id, attrs);
    liveTxs_[id] = {gen_id, 0};
    return id;
}

TraceId
TxTrace::createChildTransaction(uint64_t gen_id, TraceId parent, Tick tick,
                                const AttrList &attrs)
{
    TraceId id = allocateId();
    writer_->startTransaction(id, gen_id, tick);
    writeAttrs(id, attrs);
    writer_->writeRelation("parent_of", id, parent);
    liveTxs_[id] = {gen_id, parent};
    return id;
}

void
TxTrace::stampEvent(TraceId parent, uint64_t evt_gen_id,
                    std::string_view event_kind, std::string_view object_name,
                    Tick tick, const AttrList &attrs)
{
    TraceId evt_id = allocateId();
    writer_->startTransaction(evt_id, evt_gen_id, tick);
    writer_->writeAttribute(evt_id, "event_kind",
                            AttrValue{std::string(event_kind)});
    writer_->writeAttribute(evt_id, "object_name",
                            AttrValue{std::string(object_name)});
    writeAttrs(evt_id, attrs);
    writer_->writeRelation("parent_of", evt_id, parent);
    writer_->endTransaction(evt_id, evt_gen_id, tick);
}

void
TxTrace::retireTransaction(TraceId id, Tick tick, const AttrList &attrs)
{
    auto it = liveTxs_.find(id);
    assert(it != liveTxs_.end());
    writeAttrs(id, attrs);
    writer_->endTransaction(id, it->second.gen_id, tick);
    liveTxs_.erase(it);
}

void
TxTrace::flush()
{
    writer_->flush();
}

} // namespace tx_trace
