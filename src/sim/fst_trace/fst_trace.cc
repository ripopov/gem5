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

#include "sim/fst_trace/fst_trace.hh"

#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>
#include <utility>

#include "base/logging.hh"
#include "base/output.hh"
#include "base/stats/group.hh"
#include "base/stats/info.hh"
#include "debug/FstTrace.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/root.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

namespace
{

FstTrace *activeTrace = nullptr;

std::vector<std::string>
splitScopePath(std::string_view name)
{
    std::vector<std::string> parts;
    std::string current;

    for (char ch : name) {
        if (ch == '.') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }

    parts.push_back(current);
    return parts;
}

std::string
sanitizeSignalName(std::string_view raw_name)
{
    std::string sanitized;
    sanitized.reserve(raw_name.size());

    for (unsigned char ch : raw_name) {
        sanitized.push_back(std::isalnum(ch) || ch == '_' ? ch : '_');
    }

    return sanitized;
}

uint64_t
clockPeriodToMHz(Tick period)
{
    fatal_if(period == 0, "FST trace encountered a zero-length clock period");

    return (sim_clock::Frequency / period) / 1000000;
}

enum fstWriterPackType
parseCompression(const std::string &compression)
{
    if (compression == "zlib") {
        return FST_WR_PT_ZLIB;
    }
    if (compression == "fastlz") {
        return FST_WR_PT_FASTLZ;
    }
    if (compression == "lz4") {
        return FST_WR_PT_LZ4;
    }

    fatal("FstTrace %s has unsupported compression '%s'",
          activeTrace ? activeTrace->name().c_str() : "<unbound>",
          compression.c_str());
}

} // anonymous namespace

FstTrace::FstTrace(const Params &p)
    : SimObject(p),
      dumpActive(p.start_active),
      sampleStatsEvent(
          *this, [this] { sampleStats(); }, name()),
      statSamplePeriod(p.stat_sample_period)
{
    registerExitCallback([this]() { closeTrace(); });
}

FstTrace::~FstTrace()
{
    closeTrace();
}

void
FstTrace::init()
{
    const auto &all_objects = SimObject::getSimObjectList();
    simObjects.assign(all_objects.begin(), all_objects.end());
    std::sort(simObjects.begin(), simObjects.end(),
              [](const SimObject *lhs, const SimObject *rhs) {
                  return lhs->name() < rhs->name();
              });
}

void
FstTrace::startup()
{
    fatal_if(activeTrace && activeTrace != this,
             "Only one FstTrace SimObject can be active at a time");

    collectClockSignals();
    collectOwnedEvents();

    resolvedTracePath = simout.resolve(params().trace_file);
    fstCtx = fstWriterCreate(resolvedTracePath.c_str(), 1);
    fatal_if(!fstCtx, "Failed to create FST trace file '%s'",
             resolvedTracePath.c_str());

    fstWriterSetPackType(fstCtx, parseCompression(params().compression));
    fstWriterSetTimescale(fstCtx, params().timescale);
    fstWriterSetVersion(fstCtx, "gem5 FstTrace");

    emitHierarchy();

    if (statSamplePeriod > 0) {
        createStatHierarchy();
        lastValues.resize(statEntries.size(), 0.0);
        hasEmitted.resize(statEntries.size(), false);
    }

    installHooks();

    if (!dumpActive) {
        std::lock_guard<std::mutex> lock(writerMutex);
        emitTimeChangeLocked(curTick());
        fstWriterEmitDumpActive(fstCtx, 0);
    }

    if (statSamplePeriod > 0) {
        schedule(sampleStatsEvent, curTick() + statSamplePeriod);
    }

    DPRINTF(FstTrace,
            "Tracing %zu static events and %zu stat signals "
            "into %s\n",
            eventHandleMap.size(), statEntries.size(),
            resolvedTracePath.c_str());
}

void
FstTrace::setDumpActive(bool enable)
{
    std::lock_guard<std::mutex> lock(writerMutex);

    if (enable == dumpActive) {
        return;
    }

    dumpActive = enable;
    if (!fstCtx) {
        return;
    }

    emitTimeChangeLocked(curTick());
    fstWriterEmitDumpActive(fstCtx, enable ? 1 : 0);

    // On re-enable, clear hasEmitted so the next stat sample emits all
    // values unconditionally. Waveform viewers treat blackout exit as
    // unknown state, so we must re-establish all signal values.
    if (enable) {
        std::fill(hasEmitted.begin(), hasEmitted.end(), false);
    }
}

void
FstTrace::dispatchTrampoline(const Event *event, void *arg)
{
    auto *self = static_cast<FstTrace *>(arg);
    self->recordDispatch(event, curTick());
}

void
FstTrace::closeTrace()
{
    std::lock_guard<std::mutex> lock(writerMutex);

    if (hooksInstalled) {
        for (auto *eventq : hookedQueues) {
            if (eventq->dispatchHook == &FstTrace::dispatchTrampoline &&
                eventq->dispatchHookArg == this) {
                eventq->dispatchHook = nullptr;
                eventq->dispatchHookArg = nullptr;
            }
        }
        hookedQueues.clear();
        hooksInstalled = false;
    }

    if (activeTrace == this) {
        activeTrace = nullptr;
    }

    if (fstCtx) {
        fstWriterClose(fstCtx);
        fstCtx = nullptr;
    }
}

void
FstTrace::collectClockSignals()
{
    clockSignals.clear();

    std::map<uint64_t, Tick> period_by_mhz;
    for (const auto *sim_object : simObjects) {
        auto *clocked = dynamic_cast<const ClockedObject *>(sim_object);
        if (!clocked) {
            continue;
        }

        const Tick period = clocked->clockPeriod();
        const uint64_t mhz = clockPeriodToMHz(period);
        period_by_mhz.emplace(mhz, period);
    }

    for (const auto &[mhz, period] : period_by_mhz) {
        ClockSignal clock_signal;
        clock_signal.period = period;
        clock_signal.mhz = mhz;
        clock_signal.signalName = "clk_" + std::to_string(mhz) + "_mhz";
        clockSignals.push_back(std::move(clock_signal));
    }
}

void
FstTrace::collectOwnedEvents()
{
    ownerEventMap.clear();
    eventHandleMap.clear();

    for (const auto *event : Event::getAllEvents()) {
        const auto *owner = Event::lookupStaticOwner(event);
        if (!owner) {
            continue;
        }

        ownerEventMap[owner].push_back(event);
    }

    for (auto &[owner, events] : ownerEventMap) {
        std::sort(events.begin(), events.end(),
                  [](const Event *lhs, const Event *rhs) {
                      return lhs->name() < rhs->name();
                  });
    }
}

void
FstTrace::emitHierarchy()
{
    std::vector<std::string> current_scope;

    for (const auto *sim_object : simObjects) {
        const auto scope = splitScopePath(sim_object->name());
        size_t common_depth = 0;

        while (common_depth < current_scope.size() &&
               common_depth < scope.size() &&
               current_scope[common_depth] == scope[common_depth]) {
            ++common_depth;
        }

        while (current_scope.size() > common_depth) {
            fstWriterSetUpscope(fstCtx);
            current_scope.pop_back();
        }

        for (size_t idx = common_depth; idx < scope.size(); ++idx) {
            fstWriterSetScope(fstCtx, FST_ST_VCD_MODULE, scope[idx].c_str(),
                              nullptr);
            current_scope.push_back(scope[idx]);
        }

        auto owner_it = ownerEventMap.find(sim_object);
        if (owner_it == ownerEventMap.end()) {
            continue;
        }

        for (const auto *event : owner_it->second) {
            auto signal_name = sanitizeSignalName(event->name());
            fstHandle handle =
                fstWriterCreateVar(fstCtx, FST_VT_VCD_EVENT, FST_VD_IMPLICIT,
                                   1, signal_name.c_str(), 0);
            fatal_if(handle == 0, "Failed to create FST variable for %s",
                     event->name().c_str());
            eventHandleMap.emplace(event, handle);
        }
    }

    while (!current_scope.empty()) {
        fstWriterSetUpscope(fstCtx);
        current_scope.pop_back();
    }

    fstWriterSetScope(fstCtx, FST_ST_VCD_MODULE, "clocks", nullptr);
    for (auto &clock_signal : clockSignals) {
        clock_signal.handle =
            fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64,
                               clock_signal.signalName.c_str(), 0);
        fatal_if(clock_signal.handle == 0,
                 "Failed to create FST clock variable for %s",
                 clock_signal.signalName.c_str());
    }
    fstWriterSetUpscope(fstCtx);
}

void
FstTrace::installHooks()
{
    fatal_if(mainEventQueue.empty(),
             "FstTrace %s did not find any main event queues", name().c_str());

    for (auto *eventq : mainEventQueue) {
        if (!eventq) {
            continue;
        }

        fatal_if(eventq->dispatchHook &&
                     eventq->dispatchHook != &FstTrace::dispatchTrampoline,
                 "Event queue %s already has an incompatible dispatch hook",
                 eventq->name().c_str());

        eventq->dispatchHook = &FstTrace::dispatchTrampoline;
        eventq->dispatchHookArg = this;
        hookedQueues.push_back(eventq);
    }

    hooksInstalled = true;
    activeTrace = this;
}

void
FstTrace::emitTimeChangeLocked(Tick tick)
{
    if (!hasWrittenTime || tick != lastWrittenTick) {
        fstWriterEmitTimeChange(fstCtx, tick);
        for (const auto &clock_signal : clockSignals) {
            fstWriterEmitValueChange64(fstCtx, clock_signal.handle, 64,
                                       tick / clock_signal.period);
        }
        lastWrittenTick = tick;
        hasWrittenTime = true;
    }
}

void
FstTrace::recordDispatch(const Event *event, Tick tick)
{
    if (!dumpActive || !Event::lookupStaticOwner(event)) {
        return;
    }

    std::lock_guard<std::mutex> lock(writerMutex);
    if (!fstCtx || !dumpActive) {
        return;
    }

    auto handle_it = eventHandleMap.find(event);
    if (handle_it == eventHandleMap.end()) {
        return;
    }

    emitTimeChangeLocked(tick);
    fstWriterEmitValueChange(fstCtx, handle_it->second, "1");
}

// --- Stage 2: Periodic stat sampling ---
//
// Two-pass approach to build the FST stat hierarchy:
//
// Pass 1 (collectStatGroup / collectStatSignals): Walk the Group tree
// and build an in-memory trie (ScopeNode). Each dot-separated component
// in a stat's relative name becomes a trie edge, so stats from merged
// groups (e.g. "Cache_Controller.BUSY_BLKD.Load") produce intermediate
// scope nodes rather than flat signal names with underscores.
//
// Pass 2 (emitScopeNode): Walk the trie depth-first, emitting FST
// scopes for interior nodes and FST_VT_VCD_REAL variables for leaves.

void
FstTrace::createStatHierarchy()
{
    ScopeNode root;
    collectStatGroup(Root::root(), "", root);

    fstWriterSetScope(fstCtx, FST_ST_VCD_MODULE, "stats", nullptr);
    emitScopeNode(root);
    fstWriterSetUpscope(fstCtx);

    DPRINTF(FstTrace, "Created %zu stat signals in FST hierarchy\n",
            statEntries.size());
}

void
FstTrace::collectStatGroup(const statistics::Group *group,
                           const std::string &scopePrefix, ScopeNode &node)
{
    for (auto *info : group->getStats()) {
        collectStatSignals(info, scopePrefix, node);
    }

    for (const auto &[child_name, child] : group->getStatGroups()) {
        std::string childPrefix = scopePrefix.empty()
                                      ? child_name + "."
                                      : scopePrefix + child_name + ".";
        collectStatGroup(child, childPrefix, node.children[child_name]);
    }
}

void
FstTrace::collectStatSignals(const statistics::Info *info,
                             const std::string &scopePrefix, ScopeNode &node)
{
    // Compute the stat name relative to the current group scope.
    // New-style stats have info->name = "system.ruby.foo" matching
    // the group prefix "system.ruby." — strip the prefix.
    // Old-style stats (e.g. SLICC profiler) have info->name =
    // "Cache_Controller.ActionStalledOnHazard" with NO prefix at all —
    // use the entire name as-is so dots become scope nodes.
    std::string relName;
    if (!scopePrefix.empty() && info->name.size() > scopePrefix.size() &&
        info->name.compare(0, scopePrefix.size(), scopePrefix) == 0) {
        relName = info->name.substr(scopePrefix.size());
    } else {
        relName = info->name;
    }

    // Split relName by dots to find the target ScopeNode. All
    // components except the last become intermediate scope nodes;
    // the last becomes the signal leaf name.
    ScopeNode *target = &node;
    std::string remaining = relName;
    while (true) {
        auto dot = remaining.find('.');
        if (dot == std::string::npos) {
            break;
        }
        target = &target->children[remaining.substr(0, dot)];
        remaining = remaining.substr(dot + 1);
    }
    std::string leaf = sanitizeSignalName(remaining);

    // Dispatch on Info subclass to create PendingSignal entries
    if (auto *si = dynamic_cast<const statistics::ScalarInfo *>(info)) {
        (void)si;
        target->signals.push_back({info, StatKind::Scalar, 0, leaf});
    }
    // FormulaInfo before VectorInfo (FormulaInfo inherits VectorInfo)
    else if (auto *fi = dynamic_cast<const statistics::FormulaInfo *>(info)) {
        for (size_t i = 0; i < fi->size(); ++i) {
            std::string ename =
                fi->subnames.size() > i && !fi->subnames[i].empty()
                    ? sanitizeSignalName(fi->subnames[i])
                    : leaf + "_" + std::to_string(i);
            target->signals.push_back({info, StatKind::VectorElem, i, ename});
        }
        target->signals.push_back(
            {info, StatKind::VectorTotal, 0, leaf + "_total"});
    } else if (auto *vi = dynamic_cast<const statistics::VectorInfo *>(info)) {
        for (size_t i = 0; i < vi->size(); ++i) {
            std::string ename =
                vi->subnames.size() > i && !vi->subnames[i].empty()
                    ? sanitizeSignalName(vi->subnames[i])
                    : leaf + "_" + std::to_string(i);
            target->signals.push_back({info, StatKind::VectorElem, i, ename});
        }
        target->signals.push_back(
            {info, StatKind::VectorTotal, 0, leaf + "_total"});
    } else if (auto *di = dynamic_cast<const statistics::DistInfo *>(info)) {
        (void)di;
        target->signals.push_back(
            {info, StatKind::DistMean, 0, leaf + "_mean"});
        target->signals.push_back(
            {info, StatKind::DistSamples, 0, leaf + "_samples"});
    } else if (auto *shi =
                   dynamic_cast<const statistics::SparseHistInfo *>(info)) {
        (void)shi;
        target->signals.push_back(
            {info, StatKind::SparseHistSamples, 0, leaf + "_samples"});
    }
}

void
FstTrace::emitScopeNode(ScopeNode &node)
{
    // Emit signals at this level
    for (auto &sig : node.signals) {
        fstHandle h =
            fstWriterCreateVar(fstCtx, FST_VT_VCD_REAL, FST_VD_OUTPUT, 64,
                               sig.signalName.c_str(), 0);
        fatal_if(h == 0, "Failed to create FST stat signal '%s'",
                 sig.signalName.c_str());
        statEntries.push_back({h, sig.info, sig.kind, sig.index});
    }

    // Recurse into child scopes (std::map keeps them sorted)
    for (auto &[child_name, child] : node.children) {
        fstWriterSetScope(fstCtx, FST_ST_VCD_MODULE, child_name.c_str(),
                          nullptr);
        emitScopeNode(child);
        fstWriterSetUpscope(fstCtx);
    }
}

void
FstTrace::sampleStats()
{
    {
        std::lock_guard<std::mutex> lock(writerMutex);

        // Skip sampling during blackout regions. Stat value changes
        // inside FST blackout would be silently discarded by viewers.
        if (!dumpActive || !fstCtx) {
            schedule(sampleStatsEvent, curTick() + statSamplePeriod);
            return;
        }

        // Recursively prepare all stats (formulas, averages recompute).
        // preDumpStats() walks child groups but does NOT call prepare()
        // on individual Info objects — we must do that ourselves.
        Root::root()->preDumpStats();
        prepareStatsRecursive(Root::root());

        emitTimeChangeLocked(curTick());

        for (size_t i = 0; i < statEntries.size(); ++i) {
            double val = readStatValue(statEntries[i]);

            // Use hasEmitted flag instead of NaN sentinel. NaN != NaN
            // is always true in IEEE 754, which would defeat delta
            // compression if we used NaN as the initial "no value" marker.
            if (!hasEmitted[i] || val != lastValues[i]) {
                // Emit as raw double bytes for FST_VT_VCD_REAL signals
                fstWriterEmitValueChange(fstCtx, statEntries[i].handle, &val);
                lastValues[i] = val;
                hasEmitted[i] = true;
            }
        }
    }

    schedule(sampleStatsEvent, curTick() + statSamplePeriod);
}

void
FstTrace::prepareStatsRecursive(statistics::Group *group)
{
    for (auto *info : group->getStats()) {
        info->prepare();
    }

    for (auto &[child_name, child] : group->getStatGroups()) {
        prepareStatsRecursive(child);
    }
}

double
FstTrace::readStatValue(const StatEntry &entry) const
{
    switch (entry.kind) {
        case StatKind::Scalar: {
            auto *si = static_cast<const statistics::ScalarInfo *>(entry.info);
            return si->result();
        }
        case StatKind::VectorElem: {
            auto *vi = static_cast<const statistics::VectorInfo *>(entry.info);
            const auto &res = vi->result();
            return entry.index < res.size() ? res[entry.index] : 0.0;
        }
        case StatKind::VectorTotal: {
            auto *vi = static_cast<const statistics::VectorInfo *>(entry.info);
            return vi->total();
        }
        case StatKind::DistMean: {
            auto *di = static_cast<const statistics::DistInfo *>(entry.info);
            return di->data.samples > 0 ? di->data.sum / di->data.samples
                                        : 0.0;
        }
        case StatKind::DistSamples: {
            auto *di = static_cast<const statistics::DistInfo *>(entry.info);
            return static_cast<double>(di->data.samples);
        }
        case StatKind::SparseHistSamples: {
            auto *shi =
                static_cast<const statistics::SparseHistInfo *>(entry.info);
            return static_cast<double>(shi->data.samples);
        }
        default:
            return 0.0;
    }
}

} // namespace gem5
