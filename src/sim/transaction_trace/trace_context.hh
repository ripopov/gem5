#ifndef __SIM_TRANSACTION_TRACE_TRACE_CONTEXT_HH__
#define __SIM_TRANSACTION_TRACE_TRACE_CONTEXT_HH__

#include <cstdint>
#include <memory>

#include "base/extensible.hh"
#include "base/types.hh"
#include "mem/request.hh"

namespace gem5
{

using TraceId = uint64_t;

struct TraceContext : public Extension<Request, TraceContext>
{
    TraceId traceId = 0;
    TraceId rootTraceId = 0;
    TraceId parentTraceId = 0;
    Tick originTick = 0;

    std::unique_ptr<ExtensionBase>
    clone() const override
    {
        return std::make_unique<TraceContext>(*this);
    }
};

} // namespace gem5

#endif // __SIM_TRANSACTION_TRACE_TRACE_CONTEXT_HH__
