#ifndef TX_TRACE_TX_TRACE_HH
#define TX_TRACE_TX_TRACE_HH

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace tx_trace
{

using TraceId = uint64_t;
using Tick = uint64_t;

/** Attribute value: one of the types FTR supports. */
using AttrValue = std::variant<bool, int64_t, uint64_t, double, std::string>;

/** A single named attribute. */
struct Attr
{
    std::string name;
    AttrValue value;
};

using AttrList = std::vector<Attr>;

/**
 * Abstract writer interface.  The recorder calls these methods;
 * concrete backends (text, FTR binary) implement them.
 */
class TxWriter
{
  public:
    virtual ~TxWriter() = default;

    virtual void writeStream(uint64_t id, std::string_view name,
                             std::string_view kind) = 0;
    virtual void writeGenerator(uint64_t id, std::string_view name,
                                uint64_t stream_id) = 0;
    virtual void startTransaction(uint64_t tx_id, uint64_t gen_id,
                                  Tick tick) = 0;
    virtual void endTransaction(uint64_t tx_id, uint64_t gen_id,
                                Tick tick) = 0;
    virtual void writeAttribute(uint64_t tx_id, std::string_view name,
                                const AttrValue &value) = 0;
    virtual void writeRelation(std::string_view name, uint64_t src_tx_id,
                               uint64_t sink_tx_id) = 0;
    virtual void flush() = 0;
};

/**
 * Transaction trace recorder.
 *
 * Manages streams, generators, ID allocation, and the mapping from
 * high-level operations (createRootTransaction, stampEvent, etc.)
 * to low-level writer calls.
 */
class TxTrace
{
  public:
    explicit TxTrace(std::unique_ptr<TxWriter> writer);
    ~TxTrace();

    TxTrace(const TxTrace &) = delete;
    TxTrace &operator=(const TxTrace &) = delete;

    /**
     * Define a stream (transaction originator).
     * Returns the stream ID.
     */
    uint64_t addStream(std::string_view name, std::string_view kind);

    /**
     * Add a generator pair (transaction + .events companion) to an
     * existing stream.  Returns the transaction generator ID;
     * the events generator ID is always (returned_id + 1).
     */
    uint64_t addGeneratorPair(uint64_t stream_id, std::string_view name);

    /**
     * Create a root transaction on the given generator.
     * Returns a globally unique TraceId.
     */
    TraceId createRootTransaction(uint64_t gen_id, Tick tick,
                                  const AttrList &attrs = {});

    /**
     * Create a child transaction on the given generator, linked
     * to parent via a "parent_of" relation.
     */
    TraceId createChildTransaction(uint64_t gen_id, TraceId parent, Tick tick,
                                   const AttrList &attrs = {});

    /**
     * Stamp a point-in-time event on a transaction.
     * Internally creates a zero-duration child transaction on
     * the .events companion generator, with event_kind and
     * object_name as explicit attributes.
     */
    void stampEvent(TraceId parent, uint64_t evt_gen_id,
                    std::string_view event_kind, std::string_view object_name,
                    Tick tick, const AttrList &attrs = {});

    /** End a transaction and record its retirement. */
    void retireTransaction(TraceId id, Tick tick, const AttrList &attrs = {});

    /** Flush any buffered output. */
    void flush();

  private:
    struct LiveTx
    {
        uint64_t gen_id;
        TraceId parent_id;
    };

    TraceId allocateId();
    void writeAttrs(uint64_t tx_id, const AttrList &attrs);

    std::unique_ptr<TxWriter> writer_;
    std::atomic<uint64_t> nextId_{1};
    uint64_t nextStreamId_{0};
    uint64_t nextGenId_{0};
    std::unordered_map<TraceId, LiveTx> liveTxs_;
};

} // namespace tx_trace

#endif // TX_TRACE_TX_TRACE_HH
