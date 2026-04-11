#ifndef TX_TRACE_TX_TEXT_WRITER_HH
#define TX_TRACE_TX_TEXT_WRITER_HH

#include <memory>
#include <ostream>

#include "tx_trace/tx_trace.hh"

namespace tx_trace
{

/**
 * Text-format transaction writer following the LWTR4SC text
 * backend conventions.  Produces human-readable, grep-friendly
 * output suitable for bringup and debugging.
 *
 * Output format (one line per record):
 *
 *   scv_tr_stream (ID <id>, name "<name>", kind "<kind>")
 *   scv_tr_generator (ID <id>, name "<name>", scv_tr_stream <sid>)
 *   tx_begin <tx_id> <gen_id> <tick>
 *   tx_end <tx_id> <gen_id> <tick>
 *   tx_record_attribute <tx_id> "<name>" <TYPE> = <value>
 *   tx_relation "<name>" <src_tx_id> <sink_tx_id>
 */
class TxTextWriter : public TxWriter
{
  public:
    explicit TxTextWriter(std::unique_ptr<std::ostream> os);
    explicit TxTextWriter(const std::string &path);
    ~TxTextWriter() override = default;

    void writeStream(uint64_t id, std::string_view name,
                     std::string_view kind) override;
    void writeGenerator(uint64_t id, std::string_view name,
                        uint64_t stream_id) override;
    void startTransaction(uint64_t tx_id, uint64_t gen_id, Tick tick) override;
    void endTransaction(uint64_t tx_id, uint64_t gen_id, Tick tick) override;
    void writeAttribute(uint64_t tx_id, std::string_view name,
                        const AttrValue &value) override;
    void writeRelation(std::string_view name, uint64_t src_tx_id,
                       uint64_t sink_tx_id) override;
    void flush() override;

  private:
    std::unique_ptr<std::ostream> os_;
};

} // namespace tx_trace

#endif // TX_TRACE_TX_TEXT_WRITER_HH
