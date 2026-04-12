#include "tx_trace/tx_text_writer.hh"

#include <fstream>

namespace tx_trace
{

TxTextWriter::TxTextWriter(std::unique_ptr<std::ostream> os)
    : os_(std::move(os))
{}

TxTextWriter::TxTextWriter(const std::string &path)
    : os_(std::make_unique<std::ofstream>(path))
{}

void
TxTextWriter::writeStream(uint64_t id, std::string_view name,
                          std::string_view kind)
{
    *os_ << "scv_tr_stream (ID " << id << ", name \"" << name << "\", kind \""
         << (kind.empty() ? "<no_stream_kind>" : kind) << "\")\n";
}

void
TxTextWriter::writeGenerator(uint64_t id, std::string_view name,
                             uint64_t stream_id)
{
    *os_ << "scv_tr_generator (ID " << id << ", name \"" << name
         << "\", scv_tr_stream " << stream_id << ")\n";
}

void
TxTextWriter::startTransaction(uint64_t tx_id, uint64_t gen_id, Tick tick)
{
    *os_ << "tx_begin " << tx_id << " " << gen_id << " " << tick << "\n";
}

void
TxTextWriter::endTransaction(uint64_t tx_id, uint64_t gen_id, Tick tick)
{
    *os_ << "tx_end " << tx_id << " " << gen_id << " " << tick << "\n";
}

void
TxTextWriter::writeAttribute(uint64_t tx_id, std::string_view name,
                             const AttrValue &value, AttrPhase phase)
{
    (void)phase;
    *os_ << "tx_record_attribute " << tx_id << " \"" << name << "\" ";

    std::visit(
        [this](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                *os_ << "BOOLEAN = " << (v ? "true" : "false");
            } else if constexpr (std::is_same_v<T, int64_t>) {
                *os_ << "INTEGER = " << v;
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                *os_ << "UNSIGNED = " << v;
            } else if constexpr (std::is_same_v<T, double>) {
                *os_ << "FLOATING_POINT_NUMBER = " << v;
            } else if constexpr (std::is_same_v<T, std::string>) {
                *os_ << "STRING = \"" << v << "\"";
            }
        },
        value);

    *os_ << "\n";
}

void
TxTextWriter::writeRelation(std::string_view name, uint64_t src_tx_id,
                            uint64_t sink_tx_id, uint64_t src_stream_id,
                            uint64_t sink_stream_id)
{
    (void)src_stream_id;
    (void)sink_stream_id;
    *os_ << "tx_relation \"" << name << "\" " << src_tx_id << " " << sink_tx_id
         << "\n";
}

void
TxTextWriter::flush()
{
    os_->flush();
}

} // namespace tx_trace
