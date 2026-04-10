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
#include <cstring>
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

struct VarInfo
{
    fstHandle handle;
    enum fstVarType type;
    std::string fullName;
};

struct TraceData
{
    std::unordered_set<std::string> scopes;
    std::unordered_map<std::string, fstHandle> eventHandles;
    std::unordered_map<std::string, fstHandle> integerHandles;
    std::unordered_map<std::string, fstHandle> realHandles;
    std::unordered_map<fstHandle, std::vector<uint64_t>> changeTimes;
    std::unordered_map<fstHandle,
                       std::vector<std::pair<uint64_t, std::string>>>
        changes;
    std::vector<std::pair<uint64_t, uint32_t>> dumpActivityChanges;
};

[[noreturn]] void
usage()
{
    std::cerr << "usage: verify_fst_trace "
                 "<events|traffic|stats|dump> <trace.fst>"
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

std::string
formatTimedValues(const std::vector<std::pair<uint64_t, uint64_t>> &values)
{
    std::ostringstream out;
    out << "[";

    for (size_t idx = 0; idx < values.size(); ++idx) {
        if (idx != 0) {
            out << ", ";
        }
        out << "(" << values[idx].first << ", " << values[idx].second << ")";
    }

    out << "]";
    return out.str();
}

uint64_t
parseUnsignedValue(const std::string &value)
{
    require(!value.empty(), "encountered empty FST value");

    bool binary = true;
    for (char ch : value) {
        if (ch != '0' && ch != '1') {
            binary = false;
            break;
        }
    }

    return std::stoull(value, nullptr, binary ? 2 : 10);
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
                    case FST_VT_VCD_REAL:
                    case FST_VT_VCD_REAL_PARAMETER:
                        trace.realHandles.emplace(std::move(full_name),
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

    const uint32_t dump_changes =
        fstReaderGetNumberDumpActivityChanges(reader);
    for (uint32_t idx = 0; idx < dump_changes; ++idx) {
        trace.dumpActivityChanges.emplace_back(
            fstReaderGetDumpActivityChangeTime(reader, idx),
            fstReaderGetDumpActivityChangeValue(reader, idx));
    }

    fstReaderClose(reader);
    return trace;
}

void
requireEventTimes(const TraceData &trace, const std::string &event_name,
                  const std::vector<uint64_t> &expected_times)
{
    auto handle_it = trace.eventHandles.find(event_name);
    require(handle_it != trace.eventHandles.end(),
            "missing event signal: " + event_name);

    auto times_it = trace.changeTimes.find(handle_it->second);
    const auto &times = times_it != trace.changeTimes.end()
                            ? times_it->second
                            : std::vector<uint64_t>{};
    require(times == expected_times, "unexpected timestamps for " +
                                         event_name + ": got " +
                                         formatTimes(times) + ", expected " +
                                         formatTimes(expected_times));
}

void
requireClockValues(
    const TraceData &trace, const std::string &signal_name,
    const std::vector<std::pair<uint64_t, uint64_t>> &expected_values)
{
    auto handle_it = trace.integerHandles.find(signal_name);
    require(handle_it != trace.integerHandles.end(),
            "missing clock signal: " + signal_name);

    auto changes_it = trace.changes.find(handle_it->second);
    const auto &changes =
        changes_it != trace.changes.end()
            ? changes_it->second
            : std::vector<std::pair<uint64_t, std::string>>{};

    std::vector<std::pair<uint64_t, uint64_t>> observed_values;
    observed_values.reserve(changes.size());
    for (const auto &[time, value] : changes) {
        observed_values.emplace_back(time, parseUnsignedValue(value));
    }

    require(observed_values == expected_values,
            "unexpected values for " + signal_name + ": got " +
                formatTimedValues(observed_values) + ", expected " +
                formatTimedValues(expected_values));
}

void
verifyStage1Events(const TraceData &trace)
{
    require(trace.scopes.count("clocks") == 1, "missing clocks scope");
    require(trace.scopes.count("goodbye") == 1, "missing goodbye scope");
    require(trace.scopes.count("hello") == 1, "missing hello scope");
    require(trace.scopes.count("root") == 1, "missing root scope");
    require(trace.scopes.count("trace") == 1, "missing trace scope");

    requireEventTimes(trace, "hello.hello_event_wrapped_function_event", {1});
    requireEventTimes(trace, "goodbye.goodbye_event_wrapped_function_event",
                      {18});

    require(trace.dumpActivityChanges.size() == 2,
            "unexpected dump activity change count");
    require(trace.dumpActivityChanges[0] ==
                std::make_pair<uint64_t, uint32_t>(1, 0),
            "unexpected blackout start transition");
    require(trace.dumpActivityChanges[1] ==
                std::make_pair<uint64_t, uint32_t>(2, 1),
            "unexpected blackout end transition");

    std::cout << "FST trace verified: hello=1 goodbye=18 blackout=1->2"
              << std::endl;
}

void
verifyTraffic(const TraceData &trace)
{
    require(trace.scopes.count("board") == 1, "missing board scope");
    require(trace.scopes.count("board.cache_hierarchy") == 1,
            "missing board.cache_hierarchy scope");
    require(trace.scopes.count("board.memory") == 1,
            "missing board.memory scope");
    require(trace.scopes.count("board.memory.mem_ctrl") == 1,
            "missing board.memory.mem_ctrl scope");
    require(trace.scopes.count("board.memory.mem_ctrl.dram") == 1,
            "missing board.memory.mem_ctrl.dram scope");
    require(trace.scopes.count("board.processor") == 1,
            "missing board.processor scope");
    require(trace.scopes.count("board.processor.cores") == 1,
            "missing board.processor.cores scope");
    require(trace.scopes.count("board.processor.cores.generator") == 1,
            "missing traffic generator scope");
    require(trace.scopes.count("clocks") == 1, "missing clocks scope");
    require(trace.scopes.count("root") == 1, "missing root scope");
    require(trace.scopes.count("trace") == 1, "missing trace scope");

    requireEventTimes(
        trace,
        "board.processor.cores.generator.updateEvent_wrapped_function_event",
        {3725, 7450, 11175, 14900, 1000000});
    requireEventTimes(
        trace, "board.memory.mem_ctrl.nextReqEvent_wrapped_function_event",
        {3725, 3725, 7450, 8332, 11175, 13332, 14900, 18332});
    requireEventTimes(
        trace, "board.memory.mem_ctrl.respondEvent_wrapped_function_event",
        {45812, 50812, 55812, 60812});
    requireEventTimes(trace,
                      "board.processor.cores.generator.noProgressEvent_"
                      "wrapped_function_event",
                      {});
    // After event ownership migration, crossbar layer and packet queue
    // events are also traced, producing additional clock counter entries.
    requireClockValues(
        trace, "clocks.clk_3003_mhz",
        {{3725, 11},   {4329, 13},   {7450, 22},   {7992, 24},
         {8332, 25},   {11175, 33},  {11655, 35},  {13332, 40},
         {14900, 44},  {15318, 46},  {18332, 55},  {45812, 137},
         {50812, 152}, {55812, 167}, {60812, 182}, {68747, 206},
         {69597, 209}, {73685, 221}, {74592, 224}, {78623, 236},
         {79587, 239}, {83561, 250}, {84249, 253}, {1000000, 3003}});

    require(trace.dumpActivityChanges.empty(),
            "traffic trace unexpectedly toggled dump activity");

    std::cout << "FST traffic trace verified: updates=5 nextReq=8 "
                 "respond=4 clk_3003_mhz=3003"
              << std::endl;
}

void
verifyStats(const TraceData &trace)
{
    // Verify the stats scope exists
    require(trace.scopes.count("stats") == 1, "missing stats scope");

    // Verify at least some real-valued stat signals exist
    require(!trace.realHandles.empty(),
            "no real-valued stat signals found in FST trace");

    // Count how many stat signals are under the stats scope
    size_t stat_signal_count = 0;
    for (const auto &[name, handle] : trace.realHandles) {
        if (name.substr(0, 6) == "stats.") {
            ++stat_signal_count;
        }
    }
    require(stat_signal_count > 0,
            "no stat signals found under the 'stats' scope");

    // Verify that stat signals have value changes (sampling happened)
    size_t signals_with_changes = 0;
    for (const auto &[name, handle] : trace.realHandles) {
        if (name.substr(0, 6) != "stats.") {
            continue;
        }
        auto changes_it = trace.changeTimes.find(handle);
        if (changes_it != trace.changeTimes.end() &&
            !changes_it->second.empty()) {
            ++signals_with_changes;
        }
    }
    require(signals_with_changes > 0,
            "no stat signals have any value changes (sampling may not "
            "have fired)");

    // Check that simSeconds or simTicks exists (root stats)
    bool found_sim_stat = false;
    for (const auto &[name, handle] : trace.realHandles) {
        if (name.find("simSeconds") != std::string::npos ||
            name.find("simTicks") != std::string::npos ||
            name.find("simFreq") != std::string::npos) {
            found_sim_stat = true;
            break;
        }
    }
    require(found_sim_stat, "could not find any root sim stats "
                            "(simSeconds/simTicks/simFreq) in FST trace");

    std::cout << "FST stat trace verified: " << stat_signal_count
              << " stat signals, " << signals_with_changes
              << " with value changes" << std::endl;
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

    std::cout << "Reals:\n";
    std::map<std::string, fstHandle> sorted_reals(trace.realHandles.begin(),
                                                  trace.realHandles.end());
    for (const auto &[name, handle] : sorted_reals) {
        auto changes_it = trace.changes.find(handle);
        std::cout << "  " << name;
        if (changes_it != trace.changes.end()) {
            std::cout << " [" << changes_it->second.size() << " changes]";
        } else {
            std::cout << " [no changes]";
        }
        std::cout << "\n";
    }

    std::cout << "Dump activity changes:\n";
    for (const auto &[time, value] : trace.dumpActivityChanges) {
        std::cout << "  " << time << " -> " << value << "\n";
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

    if (mode == "events") {
        verifyStage1Events(trace);
    } else if (mode == "traffic") {
        verifyTraffic(trace);
    } else if (mode == "stats") {
        verifyStats(trace);
    } else if (mode == "dump") {
        dumpTrace(trace);
    } else {
        usage();
    }

    return 0;
}
