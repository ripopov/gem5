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
#include <functional>
#include <string_view>
#include <utility>

#include "base/logging.hh"
#include "base/output.hh"
#include "debug/FstTrace.hh"
#include "sim/eventq.hh"
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

FstTrace::FstTrace(const Params &p) : SimObject(p), dumpActive(p.start_active)
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

    collectOwnedEvents();

    resolvedTracePath = simout.resolve(params().trace_file);
    fstCtx = fstWriterCreate(resolvedTracePath.c_str(), 1);
    fatal_if(!fstCtx, "Failed to create FST trace file '%s'",
             resolvedTracePath.c_str());

    fstWriterSetPackType(fstCtx, parseCompression(params().compression));
    fstWriterSetTimescale(fstCtx, params().timescale);
    fstWriterSetVersion(fstCtx, "gem5 FstTrace");

    emitHierarchy();
    installHooks();

    if (!dumpActive) {
        std::lock_guard<std::mutex> lock(writerMutex);
        emitTimeChangeLocked(curTick());
        fstWriterEmitDumpActive(fstCtx, 0);
    }

    DPRINTF(FstTrace, "Tracing %zu static events into %s\n",
            eventHandleMap.size(), resolvedTracePath.c_str());
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

} // namespace gem5
