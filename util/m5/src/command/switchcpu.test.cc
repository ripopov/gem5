/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include "args.hh"
#include "command.hh"
#include "dispatch_table.hh"

namespace
{

bool switched;

void
test_m5_switch_cpu()
{
    switched = true;
}

DispatchTable dispatch = {.m5_switch_cpu = &test_m5_switch_cpu};

bool
run(std::initializer_list<std::string> arguments)
{
    Args args(arguments);
    return Command::run(dispatch, args);
}

TEST(Switchcpu, Arguments)
{
    switched = false;
    EXPECT_TRUE(run({"switchcpu"}));
    EXPECT_TRUE(switched);

    switched = false;
    EXPECT_FALSE(run({"switchcpu", "unexpected"}));
    EXPECT_FALSE(switched);
}

} // anonymous namespace
