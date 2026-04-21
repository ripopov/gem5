/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/sequences/registry.hh"

#include "base/logging.hh"

namespace gem5
{
namespace chi_gem5tb
{

SequenceRegistry &
SequenceRegistry::instance()
{
    static SequenceRegistry inst;
    return inst;
}

void
SequenceRegistry::add(const std::string &name, SequenceFn fn)
{
    auto [it, inserted] = table.emplace(name, std::move(fn));
    if (!inserted) {
        panic("SequenceRegistry: duplicate sequence '%s'", name.c_str());
    }
}

const SequenceFn *
SequenceRegistry::find(const std::string &name) const
{
    auto it = table.find(name);
    return it == table.end() ? nullptr : &it->second;
}

} // namespace chi_gem5tb
} // namespace gem5
