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

#include <fstapi.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

std::string
joinScope(const std::vector<std::string> &scope)
{
    std::string joined;

    for (const auto &component : scope) {
        if (!joined.empty()) {
            joined += '.';
        }
        joined += component;
    }

    return joined;
}

void
require(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

struct CallbackState
{
    std::unordered_map<fstHandle,
                       std::vector<std::pair<uint64_t, std::string>>>
        changes;
};

struct TraceData
{
    std::unordered_set<std::string> scopes;
    std::unordered_map<std::string, fstHandle> eventHandles;
    std::unordered_map<std::string, fstHandle> integerHandles;
    std::unordered_map<fstHandle, std::vector<uint64_t>> changeTimes;
    std::unordered_map<fstHandle,
                       std::vector<std::pair<uint64_t, std::string>>>
        changes;
};

[[noreturn]] void
usage()
{
    std::cerr << "usage: verify_fst_trace "
                 "<message-buffers|dump> <trace.fst>"
              << std::endl;
    std::exit(1);
}

std::string
formatTimes(const std::vector<uint64_t> &times)
{
    std::ostringstream out;
    out << "[";

    for (size_t idx = 0; idx < times.size(); ++idx) {
        if (idx != 0) {
            out << ", ";
        }
        out << times[idx];
    }

    out << "]";
    return out.str();
}

TraceData
loadTrace(const char *trace_path)
{
    fstReaderContext *reader = fstReaderOpen(trace_path);
    require(reader != nullptr, "failed to open FST trace");

    TraceData trace;
    std::vector<std::string> scope_stack;

    while (auto *hier = fstReaderIterateHier(reader)) {
        switch (hier->htyp) {
            case FST_HT_SCOPE:
                scope_stack.emplace_back(hier->u.scope.name);
                trace.scopes.emplace(joinScope(scope_stack));
                break;

            case FST_HT_UPSCOPE:
                require(!scope_stack.empty(),
                        "invalid hierarchy: unmatched upscope");
                scope_stack.pop_back();
                break;

            case FST_HT_VAR: {
                auto full_name = joinScope(scope_stack);
                if (!full_name.empty()) {
                    full_name += '.';
                }
                full_name += hier->u.var.name;

                switch (hier->u.var.typ) {
                    case FST_VT_VCD_EVENT:
                        trace.eventHandles.emplace(std::move(full_name),
                                                   hier->u.var.handle);
                        break;
                    case FST_VT_VCD_INTEGER:
                        trace.integerHandles.emplace(std::move(full_name),
                                                     hier->u.var.handle);
                        break;
                    default:
                        break;
                }
                break;
            }

            default:
                break;
        }
    }

    fstReaderSetFacProcessMaskAll(reader);

    CallbackState callback_state;
    fstReaderIterBlocks(
        reader,
        [](void *data, uint64_t time, fstHandle handle,
           const unsigned char *value) {
            auto *state = static_cast<CallbackState *>(data);
            state->changes[handle].emplace_back(
                time, std::string(reinterpret_cast<const char *>(value)));
        },
        &callback_state, nullptr);

    trace.changes = std::move(callback_state.changes);
    for (const auto &[handle, changes] : trace.changes) {
        auto &times = trace.changeTimes[handle];
        for (const auto &[time, _] : changes) {
            times.push_back(time);
        }
    }

    fstReaderClose(reader);
    return trace;
}

bool
endsWith(const std::string &str, const std::string &suffix)
{
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void
requireEventSignal(const TraceData &trace, const std::string &signal_name)
{
    require(trace.eventHandles.count(signal_name) == 1,
            "missing event signal: " + signal_name);
}

void
requireIntegerSignal(const TraceData &trace, const std::string &signal_name)
{
    require(trace.integerHandles.count(signal_name) == 1,
            "missing integer signal: " + signal_name);
}

void
verifyMessageBuffers(const TraceData &trace)
{
    const std::string controller = "system.ruby.hnf00.cntrl.reqIn";
    const std::string int_link = "system.ruby.network.int_links000.buffers0";
    const std::string src_alias =
        "system.ruby.network.routers00."
        "out_East_to_routers01_int_links000.buffers0";
    const std::string dst_alias =
        "system.ruby.network.routers01."
        "in_West_from_routers00_int_links000.buffers0";

    requireEventSignal(trace, controller + ".push");
    requireEventSignal(trace, controller + ".pop");
    requireIntegerSignal(trace, controller + ".current_size");
    requireIntegerSignal(trace, controller + ".capacity");
    requireIntegerSignal(trace, controller + ".total_enqueued");
    requireIntegerSignal(trace, controller + ".total_dequeued");
    requireIntegerSignal(trace, controller + ".not_available_count");
    requireIntegerSignal(trace, controller + ".buffered_messages_stat");

    requireEventSignal(trace, int_link + ".push");
    requireEventSignal(trace, int_link + ".pop");
    requireIntegerSignal(trace, int_link + ".current_size");
    requireIntegerSignal(trace, int_link + ".occupied_slots");
    requireIntegerSignal(trace, int_link + ".capacity");
    requireIntegerSignal(trace, int_link + ".max_dequeue_rate");

    requireEventSignal(trace, src_alias + ".push");
    requireEventSignal(trace, src_alias + ".pop");
    requireIntegerSignal(trace, src_alias + ".current_size");
    requireEventSignal(trace, dst_alias + ".push");
    requireEventSignal(trace, dst_alias + ".pop");
    requireIntegerSignal(trace, dst_alias + ".current_size");
    requireIntegerSignal(trace, dst_alias + ".capacity");

    require(trace.eventHandles.at(src_alias + ".push") ==
                trace.eventHandles.at(int_link + ".push"),
            "source-router int-link push alias does not share the base "
            "handle");
    require(trace.eventHandles.at(dst_alias + ".pop") ==
                trace.eventHandles.at(int_link + ".pop"),
            "destination-router int-link pop alias does not share the base "
            "handle");
    require(trace.integerHandles.at(src_alias + ".current_size") ==
                trace.integerHandles.at(int_link + ".current_size"),
            "source-router int-link current_size alias does not share the "
            "base handle");
    require(trace.integerHandles.at(dst_alias + ".capacity") ==
                trace.integerHandles.at(int_link + ".capacity"),
            "destination-router int-link capacity alias does not share the "
            "base handle");

    size_t message_buffer_push_signals = 0;
    size_t message_buffer_integer_signals = 0;
    size_t push_signals_with_changes = 0;
    size_t pop_signals_with_changes = 0;

    for (const auto &[name, handle] : trace.eventHandles) {
        if (endsWith(name, ".push")) {
            ++message_buffer_push_signals;
            auto changes_it = trace.changeTimes.find(handle);
            if (changes_it != trace.changeTimes.end() &&
                !changes_it->second.empty()) {
                ++push_signals_with_changes;
            }
        } else if (endsWith(name, ".pop")) {
            auto changes_it = trace.changeTimes.find(handle);
            if (changes_it != trace.changeTimes.end() &&
                !changes_it->second.empty()) {
                ++pop_signals_with_changes;
            }
        }
    }

    for (const auto &entry : trace.integerHandles) {
        if (entry.first.find(".current_size") != std::string::npos ||
            entry.first.find(".capacity") != std::string::npos ||
            entry.first.find(".total_enqueued") != std::string::npos) {
            ++message_buffer_integer_signals;
        }
    }

    require(message_buffer_push_signals > 0,
            "no MessageBuffer push signals found");
    require(message_buffer_integer_signals > 0,
            "no MessageBuffer integer state signals found");
    require(push_signals_with_changes > 0,
            "no MessageBuffer push signal has value changes");
    require(pop_signals_with_changes > 0,
            "no MessageBuffer pop signal has value changes");

    std::cout << "FST MessageBuffer trace verified: "
              << message_buffer_push_signals << " push signals, "
              << message_buffer_integer_signals
              << " integer state signals, aliases under routers00/routers01"
              << std::endl;
}

void
dumpTrace(const TraceData &trace)
{
    std::map<std::string, std::vector<uint64_t>> events_by_name;
    std::map<std::string, std::vector<std::pair<uint64_t, std::string>>>
        integers_by_name;

    for (const auto &[name, handle] : trace.eventHandles) {
        auto times_it = trace.changeTimes.find(handle);
        if (times_it != trace.changeTimes.end()) {
            events_by_name.emplace(name, times_it->second);
        } else {
            events_by_name.emplace(name, std::vector<uint64_t>{});
        }
    }

    for (const auto &[name, handle] : trace.integerHandles) {
        auto changes_it = trace.changes.find(handle);
        if (changes_it != trace.changes.end()) {
            integers_by_name.emplace(name, changes_it->second);
        } else {
            integers_by_name.emplace(
                name, std::vector<std::pair<uint64_t, std::string>>{});
        }
    }

    std::map<std::string, bool> sorted_scopes;
    for (const auto &scope : trace.scopes) {
        sorted_scopes.emplace(scope, true);
    }

    std::cout << "Scopes:\n";
    for (const auto &[scope, _] : sorted_scopes) {
        std::cout << "  " << scope << "\n";
    }

    std::cout << "Events:\n";
    for (const auto &[name, times] : events_by_name) {
        std::cout << "  " << name << " " << formatTimes(times) << "\n";
    }

    std::cout << "Integers:\n";
    for (const auto &[name, changes] : integers_by_name) {
        std::cout << "  " << name << " [";
        for (size_t idx = 0; idx < changes.size(); ++idx) {
            if (idx != 0) {
                std::cout << ", ";
            }
            std::cout << "(" << changes[idx].first << ", "
                      << changes[idx].second << ")";
        }
        std::cout << "]\n";
    }
}

} // anonymous namespace

int
main(int argc, char **argv)
{
    if (argc != 3) {
        usage();
    }

    const std::string mode = argv[1];
    const TraceData trace = loadTrace(argv[2]);

    if (mode == "message-buffers") {
        verifyMessageBuffers(trace);
    } else if (mode == "dump") {
        dumpTrace(trace);
    } else {
        usage();
    }

    return 0;
}
