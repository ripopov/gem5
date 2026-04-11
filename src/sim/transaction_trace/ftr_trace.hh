#ifndef __SIM_TRANSACTION_TRACE_FTR_TRACE_HH__
#define __SIM_TRANSACTION_TRACE_FTR_TRACE_HH__

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "params/FtrTrace.hh"
#include "sim/sim_object.hh"
#include "tx_trace/tx_trace.hh"

namespace gem5
{

class TimingResponseProtocol;
class Packet;
using PacketPtr = Packet *;

using TraceId = uint64_t;

/**
 * FtrTrace is the runtime recorder for FTR transaction tracing.
 *
 * It wraps the tx_trace::TxTrace library, managing streams and
 * generators per originator SimObject, pending-root lifecycle,
 * and the root creation filter policy.
 *
 * Singleton: at most one instance exists.  All instrumentation
 * hooks check FtrTrace::get() first; if null, tracing is disabled.
 */
class FtrTrace : public SimObject
{
  public:
    PARAMS(FtrTrace);
    FtrTrace(const Params &p);
    ~FtrTrace();

    /** Singleton accessor. Returns nullptr when tracing is disabled. */
    static FtrTrace *
    get()
    {
        return instance;
    }

    DrainState drain() override;

    // ---- Filter policy ----

    using RootFilter =
        std::function<bool(PacketPtr, TimingResponseProtocol *)>;

    /** Register the filter that decides which requests get roots. */
    void setRootFilter(RootFilter filter);

    /** Check if this packet should start a new root transaction. */
    bool shouldTrace(PacketPtr pkt, TimingResponseProtocol *peer) const;

    // ---- Root lifecycle ----

    /**
     * Create a pending root (Phase 1).
     * Reserves a TraceId but writes nothing to the trace file.
     * The caller must attach the returned ID to the packet's
     * TraceContext extension.
     *
     * @param sequencer  The originating Sequencer SimObject
     *                   (used for stream lookup; may be nullptr
     *                   if not yet known at port hook time).
     * @param tick       Creation tick.
     * @return Globally unique TraceId for this root.
     */
    TraceId createPendingRoot(SimObject *sequencer, Tick tick);

    /** Is this TraceId in the pending table? */
    bool isPending(TraceId id) const;

    /**
     * Finalize a pending root as live (Phase 2).
     * Calls createRootTransactionWithId on the writer, records
     * static attributes from the Request, and moves the root
     * from pending to live.
     */
    void finalizeRoot(TraceId id, SimObject *sequencer,
                      std::string_view objectName, Tick tick,
                      const tx_trace::AttrList &attrs = {});

    /** Discard a pending root (request was rejected). */
    void discardPendingRoot(TraceId id);

    // ---- Events and retirement ----

    /**
     * Stamp a point-in-time event on a transaction.
     * Looks up the correct .events generator from the live table.
     */
    void stampEvent(TraceId id, std::string_view eventKind,
                    std::string_view objectName, Tick tick,
                    const tx_trace::AttrList &attrs = {});

    /** Retire (end) a transaction. */
    void retireTransaction(TraceId id, std::string_view objectName, Tick tick,
                           const tx_trace::AttrList &attrs = {});

    // ---- Flit children ----

    /**
     * Create a flit child transaction linked to a root.
     * Returns the flit's own TraceId.
     */
    TraceId createFlitChild(TraceId parentRoot, std::string_view objectName,
                            Tick tick, const tx_trace::AttrList &attrs = {});

  private:
    /** Per-stream generator IDs. */
    struct StreamInfo
    {
        uint64_t streamId;
        uint64_t memreqGen;
        uint64_t memreqEvtGen;
        uint64_t flitGen;
        uint64_t flitEvtGen;
    };

    /** Pending root entry (not yet written to trace). */
    struct PendingRoot
    {
        SimObject *sequencer;
        Tick originTick;
    };

    /** Info stored per live transaction. */
    struct TxInfo
    {
        uint64_t evtGenId;     // companion .events generator
        uint64_t flitGenId;    // flit generator (for roots)
        uint64_t flitEvtGenId; // flit.events generator (for roots)
        bool isFlit;
    };

    /** Get or lazily create the stream for a sequencer. */
    const StreamInfo &getOrCreateStream(SimObject *sequencer);

    static FtrTrace *instance;

    std::unique_ptr<tx_trace::TxTrace> trace_;
    RootFilter rootFilter_;

    std::unordered_map<SimObject *, StreamInfo> streams_;
    std::unordered_map<TraceId, PendingRoot> pendingRoots_;
    std::unordered_map<TraceId, TxInfo> liveTxs_;
};

} // namespace gem5

#endif // __SIM_TRANSACTION_TRACE_FTR_TRACE_HH__
