/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "args.hh"
#include "command.hh"
#include "dispatch_table.hh"

namespace
{

bool
do_switch_cpu(const DispatchTable &dt, Args &)
{
    (*dt.m5_switch_cpu)();
    return true;
}

Command switch_cpu = {
    "switchcpu", 0, 0, do_switch_cpu, "\n"
        "        Request a CPU-model switch"};

} // anonymous namespace
