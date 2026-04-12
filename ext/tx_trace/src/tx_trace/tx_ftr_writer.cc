#include "tx_trace/tx_ftr_writer.hh"

#include <cassert>
#include <limits>
#include <type_traits>

namespace tx_trace
{

TxFtrWriter::TxFtrWriter(const std::string &path) : writer_(path)
{
    writer_.writeInfo(-12);
}

void
TxFtrWriter::writeStream(uint64_t id, std::string_view name,
                         std::string_view kind)
{
    writer_.writeStream(id, std::string(name), std::string(kind));
}

void
TxFtrWriter::writeGenerator(uint64_t id, std::string_view name,
                            uint64_t stream_id)
{
    generatorToStream_[id] = stream_id;
    writer_.writeGenerator(id, std::string(name), stream_id);
}

void
TxFtrWriter::startTransaction(uint64_t tx_id, uint64_t gen_id, Tick tick)
{
    auto it = generatorToStream_.find(gen_id);
    assert(it != generatorToStream_.end());
    writer_.startTransaction(tx_id, gen_id, it->second, tick);
}

void
TxFtrWriter::endTransaction(uint64_t tx_id, uint64_t gen_id, Tick tick)
{
    (void)gen_id;
    writer_.endTransaction(tx_id, tick);
}

void
TxFtrWriter::writeAttribute(uint64_t tx_id, std::string_view name,
                            const AttrValue &value, AttrPhase phase)
{
    const auto event_type = toFtrEventType(phase);
    const auto data_type = toFtrDataType(value);
    const std::string attr_name(name);

    std::visit(
        [&](const auto &v) {
            writer_.writeAttribute(tx_id, event_type, attr_name, data_type, v);
        },
        value);
}

void
TxFtrWriter::writeRelation(std::string_view name, uint64_t src_tx_id,
                           uint64_t sink_tx_id, uint64_t src_stream_id,
                           uint64_t sink_stream_id)
{
    writer_.writeRelation(std::string(name), sink_stream_id, sink_tx_id,
                          src_stream_id, src_tx_id);
}

void
TxFtrWriter::flush()
{
    writer_.dict.flush(writer_.cw);
    writer_.dir.flush(writer_.cw);
    for (auto &block : writer_.fiber_blocks) {
        if (block) {
            block->flush(writer_.cw);
            block->start_time = std::numeric_limits<uint64_t>::max();
            block->end_time = 0;
        }
    }
    writer_.rel.flush(writer_.cw);
    writer_.cw.enc.ofs.flush();
}

ftr::event_type
TxFtrWriter::toFtrEventType(AttrPhase phase)
{
    switch (phase) {
        case AttrPhase::Begin:
            return ftr::event_type::BEGIN;
        case AttrPhase::Record:
            return ftr::event_type::RECORD;
        case AttrPhase::End:
            return ftr::event_type::END;
    }

    assert(false);
    return ftr::event_type::RECORD;
}

ftr::data_type
TxFtrWriter::toFtrDataType(const AttrValue &value)
{
    return std::visit(
        [](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return ftr::data_type::BOOLEAN;
            } else if constexpr (std::is_same_v<T, int64_t>) {
                return ftr::data_type::INTEGER;
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                return ftr::data_type::UNSIGNED;
            } else if constexpr (std::is_same_v<T, double>) {
                return ftr::data_type::FLOATING_POINT_NUMBER;
            } else if constexpr (std::is_same_v<T, std::string>) {
                return ftr::data_type::STRING;
            }
        },
        value);
}

} // namespace tx_trace
