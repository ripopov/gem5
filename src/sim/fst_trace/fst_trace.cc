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
#include <string_view>
#include <utility>

#include "base/logging.hh"
#include "base/output.hh"
#include "debug/FstTrace.hh"
#include "mem/ruby/network/BasicRouter.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/network/simple/SimpleLink.hh"
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

std::string
lastScopeComponent(std::string_view name)
{
    const auto dot = name.find_last_of('.');
    const auto leaf =
        dot == std::string_view::npos ? name : name.substr(dot + 1);
    return sanitizeSignalName(leaf);
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
    : SimObject(p)
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

    collectMessageBuffersAndAliases();
}

void
FstTrace::startup()
{
    fatal_if(activeTrace && activeTrace != this,
             "Only one FstTrace SimObject can be active at a time");
    fatal_if(messageBuffers.empty(),
             "FstTrace %s did not find any Ruby MessageBuffer SimObjects",
             name().c_str());

    resolvedTracePath = simout.resolve(params().trace_file);
    fstCtx = fstWriterCreate(resolvedTracePath.c_str(), 1);
    fatal_if(!fstCtx, "Failed to create FST trace file '%s'",
             resolvedTracePath.c_str());

    fstWriterSetPackType(fstCtx, parseCompression(params().compression));
    fstWriterSetTimescale(fstCtx, params().timescale);
    fstWriterSetVersion(fstCtx, "gem5 Ruby MessageBuffer FST Trace");

    emitHierarchy();
    activeTrace = this;
    emitInitialMessageBufferStates();

    DPRINTF(FstTrace, "Tracing %zu Ruby MessageBuffers into %s\n",
            messageBufferSignalMap.size(), resolvedTracePath.c_str());
}

void
FstTrace::recordMessageBufferPush(const ruby::MessageBuffer *buffer, Tick tick)
{
    if (activeTrace) {
        activeTrace->recordMessageBufferEvent(buffer, tick, true);
    }
}

void
FstTrace::recordMessageBufferPop(const ruby::MessageBuffer *buffer, Tick tick)
{
    if (activeTrace) {
        activeTrace->recordMessageBufferEvent(buffer, tick, false);
    }
}

void
FstTrace::closeTrace()
{
    std::lock_guard<std::mutex> lock(writerMutex);

    if (activeTrace == this) {
        activeTrace = nullptr;
    }

    if (fstCtx) {
        fstWriterClose(fstCtx);
        fstCtx = nullptr;
    }
}

void
FstTrace::collectMessageBuffersAndAliases()
{
    messageBuffers.clear();
    messageBufferAliasesByRouter.clear();

    for (const auto *sim_object : simObjects) {
        if (auto *buffer =
                dynamic_cast<const ruby::MessageBuffer *>(sim_object)) {
            messageBuffers.push_back(buffer);
            continue;
        }

        auto *link = dynamic_cast<const ruby::SimpleIntLink *>(sim_object);
        if (!link) {
            continue;
        }

        const auto &p = link->params();
        const std::string link_name = lastScopeComponent(link->name());
        const std::string src_name = p.src_node->name();
        const std::string dst_name = p.dst_node->name();
        const std::string src_leaf = lastScopeComponent(src_name);
        const std::string dst_leaf = lastScopeComponent(dst_name);
        const std::string src_port = p.src_outport.empty()
                                         ? "unknown"
                                         : sanitizeSignalName(p.src_outport);
        const std::string dst_port = p.dst_inport.empty()
                                         ? "unknown"
                                         : sanitizeSignalName(p.dst_inport);

        for (const auto *buffer : link->m_buffers) {
            const std::string buffer_name = lastScopeComponent(buffer->name());
            messageBufferAliasesByRouter[src_name].push_back({
                "out_" + src_port + "_to_" + dst_leaf + "_" + link_name,
                buffer_name,
                buffer,
            });
            messageBufferAliasesByRouter[dst_name].push_back({
                "in_" + dst_port + "_from_" + src_leaf + "_" + link_name,
                buffer_name,
                buffer,
            });
        }
    }

    for (auto &[router, aliases] : messageBufferAliasesByRouter) {
        std::sort(
            aliases.begin(), aliases.end(),
            [](const MessageBufferAlias &lhs, const MessageBufferAlias &rhs) {
                if (lhs.linkScope != rhs.linkScope) {
                    return lhs.linkScope < rhs.linkScope;
                }
                return lhs.bufferScope < rhs.bufferScope;
            });
    }
}

void
FstTrace::emitHierarchy()
{
    struct ScopeEntry
    {
        std::string scope;
        const ruby::MessageBuffer *buffer = nullptr;
    };

    std::vector<ScopeEntry> entries;
    entries.reserve(
        messageBuffers.size() + messageBufferAliasesByRouter.size());
    for (const auto *buffer : messageBuffers) {
        entries.push_back({buffer->name(), buffer});
    }
    for (const auto &entry : messageBufferAliasesByRouter) {
        entries.push_back({entry.first, nullptr});
    }

    std::sort(entries.begin(), entries.end(),
              [](const ScopeEntry &lhs, const ScopeEntry &rhs) {
                  return lhs.scope < rhs.scope;
              });

    std::vector<std::string> current_scope;

    for (const auto &entry : entries) {
        const auto scope = splitScopePath(entry.scope);
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

        if (entry.buffer) {
            messageBufferSignalMap.emplace(
                entry.buffer, createMessageBufferSignals(entry.buffer));
        }

        emitMessageBufferAliasesForScope(entry.scope);
    }

    while (!current_scope.empty()) {
        fstWriterSetUpscope(fstCtx);
        current_scope.pop_back();
    }
}

FstTrace::MessageBufferSignals
FstTrace::createMessageBufferSignals(const ruby::MessageBuffer *buffer)
{
    MessageBufferSignals signals;

    signals.push = fstWriterCreateVar(fstCtx, FST_VT_VCD_EVENT,
                                      FST_VD_IMPLICIT, 1, "push", 0);
    signals.pop = fstWriterCreateVar(fstCtx, FST_VT_VCD_EVENT, FST_VD_IMPLICIT,
                                     1, "pop", 0);
    signals.currentSize = fstWriterCreateVar(
        fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32, "current_size", 0);
    signals.occupiedSlots = fstWriterCreateVar(
        fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32, "occupied_slots", 0);
    signals.stalledMessages =
        fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                           "stalled_messages", 0);
    signals.deferredMessages =
        fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                           "deferred_messages", 0);
    signals.capacity = fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER,
                                          FST_VD_IMPLICIT, 32, "capacity", 0);
    signals.unbounded = fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER,
                                           FST_VD_IMPLICIT, 1, "unbounded", 0);
    signals.maxDequeueRate =
        fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                           "max_dequeue_rate", 0);
    signals.totalEnqueued = fstWriterCreateVar(
        fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64, "total_enqueued", 0);
    signals.totalDequeued = fstWriterCreateVar(
        fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64, "total_dequeued", 0);
    signals.notAvailableCount =
        fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64,
                           "not_available_count", 0);
    signals.stallCount = fstWriterCreateVar(
        fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64, "stall_count", 0);
    signals.stallTicks = fstWriterCreateVar(
        fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64, "stall_ticks", 0);
    signals.bufferedMessagesStat =
        fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                           "buffered_messages_stat", 0);
    signals.dequeuesThisCycle =
        fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                           "dequeues_this_cycle", 0);
    signals.vnet = fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER,
                                      FST_VD_IMPLICIT, 32, "vnet", 0);
    signals.incomingLink = fstWriterCreateVar(
        fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32, "incoming_link", 0);
    signals.routingPriority =
        fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                           "routing_priority", 0);
    signals.strictFifo = fstWriterCreateVar(
        fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 1, "strict_fifo", 0);
    signals.allowZeroLatency =
        fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 1,
                           "allow_zero_latency", 0);
    signals.randomization = fstWriterCreateVar(
        fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32, "randomization", 0);
    signals.headReadyTick = fstWriterCreateVar(
        fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64, "head_ready_tick", 0);

    fatal_if(signals.push == 0 || signals.pop == 0 ||
                 signals.currentSize == 0 || signals.capacity == 0 ||
                 signals.totalEnqueued == 0 || signals.totalDequeued == 0,
             "Failed to create MessageBuffer trace variables for %s",
             buffer->name().c_str());
    return signals;
}

void
FstTrace::createMessageBufferAliasSignals(const MessageBufferSignals &target)
{
    fstWriterCreateVar(fstCtx, FST_VT_VCD_EVENT, FST_VD_IMPLICIT, 1, "push",
                       target.push);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_EVENT, FST_VD_IMPLICIT, 1, "pop",
                       target.pop);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                       "current_size", target.currentSize);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                       "occupied_slots", target.occupiedSlots);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                       "stalled_messages", target.stalledMessages);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                       "deferred_messages", target.deferredMessages);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                       "capacity", target.capacity);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 1,
                       "unbounded", target.unbounded);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                       "max_dequeue_rate", target.maxDequeueRate);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64,
                       "total_enqueued", target.totalEnqueued);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64,
                       "total_dequeued", target.totalDequeued);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64,
                       "not_available_count", target.notAvailableCount);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64,
                       "stall_count", target.stallCount);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64,
                       "stall_ticks", target.stallTicks);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                       "buffered_messages_stat", target.bufferedMessagesStat);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                       "dequeues_this_cycle", target.dequeuesThisCycle);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32, "vnet",
                       target.vnet);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                       "incoming_link", target.incomingLink);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                       "routing_priority", target.routingPriority);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 1,
                       "strict_fifo", target.strictFifo);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 1,
                       "allow_zero_latency", target.allowZeroLatency);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                       "randomization", target.randomization);
    fstWriterCreateVar(fstCtx, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 64,
                       "head_ready_tick", target.headReadyTick);
}

void
FstTrace::emitMessageBufferAliasesForScope(const std::string &scope)
{
    auto alias_it = messageBufferAliasesByRouter.find(scope);
    if (alias_it == messageBufferAliasesByRouter.end()) {
        return;
    }

    for (const auto &alias : alias_it->second) {
        auto signal_it = messageBufferSignalMap.find(alias.buffer);
        fatal_if(signal_it == messageBufferSignalMap.end(),
                 "Missing MessageBuffer trace handles for alias %s.%s",
                 alias.linkScope.c_str(), alias.bufferScope.c_str());

        fstWriterSetScope(fstCtx, FST_ST_VCD_MODULE, alias.linkScope.c_str(),
                          nullptr);
        fstWriterSetScope(fstCtx, FST_ST_VCD_MODULE, alias.bufferScope.c_str(),
                          nullptr);
        createMessageBufferAliasSignals(signal_it->second);
        fstWriterSetUpscope(fstCtx);
        fstWriterSetUpscope(fstCtx);
    }
}

void
FstTrace::emitInitialMessageBufferStates()
{
    emitTimeChangeLocked(curTick());
    for (const auto *buffer : messageBuffers) {
        auto handle_it = messageBufferSignalMap.find(buffer);
        fatal_if(handle_it == messageBufferSignalMap.end(),
                 "Missing MessageBuffer trace handles for %s",
                 buffer->name().c_str());
        emitMessageBufferStateLocked(buffer, handle_it->second);
    }
}

void
FstTrace::emitMessageBufferStateLocked(const ruby::MessageBuffer *buffer,
                                       const MessageBufferSignals &signals)
{
    const auto state = buffer->traceState();

    fstWriterEmitValueChange64(fstCtx, signals.currentSize, 32,
                               state.currentSize);
    fstWriterEmitValueChange64(fstCtx, signals.occupiedSlots, 32,
                               state.occupiedSlots);
    fstWriterEmitValueChange64(fstCtx, signals.stalledMessages, 32,
                               state.stalledMessages);
    fstWriterEmitValueChange64(fstCtx, signals.deferredMessages, 32,
                               state.deferredMessages);
    fstWriterEmitValueChange64(fstCtx, signals.capacity, 32, state.capacity);
    fstWriterEmitValueChange64(fstCtx, signals.unbounded, 1, state.unbounded);
    fstWriterEmitValueChange64(fstCtx, signals.maxDequeueRate, 32,
                               state.maxDequeueRate);
    fstWriterEmitValueChange64(fstCtx, signals.totalEnqueued, 64,
                               state.totalEnqueued);
    fstWriterEmitValueChange64(fstCtx, signals.totalDequeued, 64,
                               state.totalDequeued);
    fstWriterEmitValueChange64(fstCtx, signals.notAvailableCount, 64,
                               state.notAvailableCount);
    fstWriterEmitValueChange64(fstCtx, signals.stallCount, 64,
                               state.stallCount);
    fstWriterEmitValueChange64(fstCtx, signals.stallTicks, 64,
                               state.stallTicks);
    fstWriterEmitValueChange64(fstCtx, signals.bufferedMessagesStat, 32,
                               state.bufferedMessagesStat);
    fstWriterEmitValueChange64(fstCtx, signals.dequeuesThisCycle, 32,
                               state.dequeuesThisCycle);
    fstWriterEmitValueChange64(fstCtx, signals.vnet, 32, state.vnet);
    fstWriterEmitValueChange64(fstCtx, signals.incomingLink, 32,
                               state.incomingLink);
    fstWriterEmitValueChange64(fstCtx, signals.routingPriority, 32,
                               state.routingPriority);
    fstWriterEmitValueChange64(fstCtx, signals.strictFifo, 1,
                               state.strictFifo);
    fstWriterEmitValueChange64(fstCtx, signals.allowZeroLatency, 1,
                               state.allowZeroLatency);
    fstWriterEmitValueChange64(fstCtx, signals.randomization, 32,
                               state.randomization);
    fstWriterEmitValueChange64(fstCtx, signals.headReadyTick, 64,
                               state.headReadyTick);
}

void
FstTrace::recordMessageBufferEvent(const ruby::MessageBuffer *buffer,
                                   Tick tick, bool is_push)
{
    std::lock_guard<std::mutex> lock(writerMutex);
    if (!fstCtx) {
        return;
    }

    auto handle_it = messageBufferSignalMap.find(buffer);
    if (handle_it == messageBufferSignalMap.end()) {
        return;
    }

    emitTimeChangeLocked(tick);
    fstWriterEmitValueChange(
        fstCtx, is_push ? handle_it->second.push : handle_it->second.pop, "1");
    emitMessageBufferStateLocked(buffer, handle_it->second);
}

void
FstTrace::emitTimeChangeLocked(Tick tick)
{
    if (!hasWrittenTime || tick != lastWrittenTick) {
        fstWriterEmitTimeChange(fstCtx, tick);
        lastWrittenTick = tick;
        hasWrittenTime = true;
    }
}

} // namespace gem5
