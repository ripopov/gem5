#ifndef TX_TRACE_TX_FTR_WRITER_HH
#define TX_TRACE_TX_FTR_WRITER_HH

#include <cstdint>
#include <string>
#include <unordered_map>

#include "ftr/ftr_writer.h"
#include "tx_trace/tx_trace.hh"

namespace tx_trace
{

class TxFtrWriter : public TxWriter
{
  public:
    explicit TxFtrWriter(const std::string &path);
    ~TxFtrWriter() override = default;

    void writeStream(uint64_t id, std::string_view name,
                     std::string_view kind) override;
    void writeGenerator(uint64_t id, std::string_view name,
                        uint64_t stream_id) override;
    void startTransaction(uint64_t tx_id, uint64_t gen_id, Tick tick) override;
    void endTransaction(uint64_t tx_id, uint64_t gen_id, Tick tick) override;
    void writeAttribute(uint64_t tx_id, std::string_view name,
                        const AttrValue &value, AttrPhase phase) override;
    void writeRelation(std::string_view name, uint64_t src_tx_id,
                       uint64_t sink_tx_id, uint64_t src_stream_id,
                       uint64_t sink_stream_id) override;
    void flush() override;

  private:
    static ftr::event_type toFtrEventType(AttrPhase phase);
    static ftr::data_type toFtrDataType(const AttrValue &value);

    ftr::ftr_writer<true> writer_;
    std::unordered_map<uint64_t, uint64_t> generatorToStream_;
};

} // namespace tx_trace

#endif // TX_TRACE_TX_FTR_WRITER_HH
