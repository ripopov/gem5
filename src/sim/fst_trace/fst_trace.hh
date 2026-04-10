/*
 * Copyright (c) 2026 The Regents of The University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __SIM_FST_TRACE_FST_TRACE_HH__
#define __SIM_FST_TRACE_FST_TRACE_HH__

#include <fstapi.h>

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "params/FstTrace.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class Event;

namespace statistics
{
class Group;
class Info;
} // namespace statistics

class FstTrace : public SimObject
{
  public:
    PARAMS(FstTrace);

    explicit FstTrace(const Params &p);
    ~FstTrace() override;

    void init() override;
    void startup() override;

    void setDumpActive(bool enable);

    static void dispatchTrampoline(const Event *event, void *arg);

  private:
    fstWriterContext *fstCtx = nullptr;

    std::vector<const SimObject *> simObjects;
    std::vector<EventQueue *> hookedQueues;
    struct ClockSignal
    {
        Tick period = 0;
        uint64_t mhz = 0;
        fstHandle handle = 0;
        std::string signalName;
    };

    std::vector<ClockSignal> clockSignals;
    std::unordered_map<const SimObject *, std::vector<const Event *>>
        ownerEventMap;
    std::unordered_map<const Event *, fstHandle> eventHandleMap;

    std::mutex writerMutex;

    std::string resolvedTracePath;
    Tick lastWrittenTick = 0;
    bool hasWrittenTime = false;
    bool dumpActive = true;
    bool hooksInstalled = false;

    void closeTrace();
    void collectClockSignals();
    void collectOwnedEvents();
    void emitHierarchy();
    void installHooks();
    void emitTimeChangeLocked(Tick tick);
    void recordDispatch(const Event *event, Tick tick);

    // --- Stage 2: Periodic stat sampling ---

    enum class StatKind
    {
        Scalar,
        VectorElem,
        VectorTotal,
        DistMean,
        DistSamples,
        SparseHistSamples,
    };

    struct StatEntry
    {
        fstHandle handle = 0;
        const statistics::Info *info = nullptr;
        StatKind kind = StatKind::Scalar;
        size_t index = 0;
    };

    // Intermediate tree node built during Pass 1. Dot-separated stat
    // names are split into a trie of ScopeNode children so that each
    // component becomes an FST scope and only the leaf carries signals.
    struct PendingSignal
    {
        const statistics::Info *info;
        StatKind kind;
        size_t index;
        std::string signalName;
    };

    struct ScopeNode
    {
        std::map<std::string, ScopeNode> children;
        std::vector<PendingSignal> signals;
    };

    EventFunctionWrapper sampleStatsEvent;
    Tick statSamplePeriod = 0;

    std::vector<StatEntry> statEntries;
    std::vector<double> lastValues;
    std::vector<bool> hasEmitted;

    void createStatHierarchy();
    void collectStatGroup(const statistics::Group *group,
                          const std::string &scopePrefix, ScopeNode &node);
    void collectStatSignals(const statistics::Info *info,
                            const std::string &scopePrefix, ScopeNode &node);
    void emitScopeNode(ScopeNode &node);
    void sampleStats();
    void prepareStatsRecursive(statistics::Group *group);
    double readStatValue(const StatEntry &entry) const;
};

} // namespace gem5

#endif // __SIM_FST_TRACE_FST_TRACE_HH__
