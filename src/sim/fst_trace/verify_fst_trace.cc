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
    std::unordered_map<fstHandle, std::vector<uint64_t>> changeTimes;
};

} // anonymous namespace

int
main(int argc, char **argv)
{
    require(argc == 2, "usage: verify_fst_trace <trace.fst>");

    fstReaderContext *reader = fstReaderOpen(argv[1]);
    require(reader != nullptr, "failed to open FST trace");

    std::vector<std::string> scope_stack;
    std::unordered_set<std::string> scopes;
    std::unordered_map<std::string, fstHandle> event_handles;

    while (auto *hier = fstReaderIterateHier(reader)) {
        switch (hier->htyp) {
            case FST_HT_SCOPE:
                scope_stack.emplace_back(hier->u.scope.name);
                scopes.emplace(joinScope(scope_stack));
                break;

            case FST_HT_UPSCOPE:
                require(!scope_stack.empty(),
                        "invalid hierarchy: unmatched upscope");
                scope_stack.pop_back();
                break;

            case FST_HT_VAR:
                if (hier->u.var.typ == FST_VT_VCD_EVENT) {
                    auto full_name = joinScope(scope_stack);
                    if (!full_name.empty()) {
                        full_name += '.';
                    }
                    full_name += hier->u.var.name;
                    event_handles.emplace(std::move(full_name),
                                          hier->u.var.handle);
                }
                break;

            default:
                break;
        }
    }

    require(scopes.count("goodbye") == 1, "missing goodbye scope");
    require(scopes.count("hello") == 1, "missing hello scope");
    require(scopes.count("root") == 1, "missing root scope");
    require(scopes.count("trace") == 1, "missing trace scope");

    const std::string hello_event = "hello.hello_event_wrapped_function_event";
    const std::string goodbye_event =
        "goodbye.goodbye_event_wrapped_function_event";

    auto hello_it = event_handles.find(hello_event);
    auto goodbye_it = event_handles.find(goodbye_event);

    require(hello_it != event_handles.end(), "missing hello event signal");
    require(goodbye_it != event_handles.end(), "missing goodbye event signal");

    fstReaderSetFacProcessMask(reader, hello_it->second);
    fstReaderSetFacProcessMask(reader, goodbye_it->second);

    CallbackState state;
    fstReaderIterBlocks(
        reader,
        [](void *data, uint64_t time, fstHandle handle,
           const unsigned char *) {
            auto *callback_state = static_cast<CallbackState *>(data);
            callback_state->changeTimes[handle].push_back(time);
        },
        &state, nullptr);

    const auto &hello_times = state.changeTimes[hello_it->second];
    const auto &goodbye_times = state.changeTimes[goodbye_it->second];

    require(hello_times == std::vector<uint64_t>{1},
            "unexpected hello event timestamps");
    require(goodbye_times == std::vector<uint64_t>{18},
            "unexpected goodbye event timestamps");

    require(fstReaderGetNumberDumpActivityChanges(reader) == 2,
            "unexpected dump activity change count");
    require(fstReaderGetDumpActivityChangeTime(reader, 0) == 1,
            "unexpected blackout start timestamp");
    require(fstReaderGetDumpActivityChangeValue(reader, 0) == 0,
            "unexpected blackout start value");
    require(fstReaderGetDumpActivityChangeTime(reader, 1) == 2,
            "unexpected blackout end timestamp");
    require(fstReaderGetDumpActivityChangeValue(reader, 1) == 1,
            "unexpected blackout end value");

    fstReaderClose(reader);

    std::cout << "FST trace verified: hello=1 goodbye=18 blackout=1->2"
              << std::endl;

    return 0;
}
