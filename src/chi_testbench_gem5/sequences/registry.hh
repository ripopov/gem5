/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_SEQUENCES_REGISTRY_HH__
#define __CHI_TESTBENCH_GEM5_SEQUENCES_REGISTRY_HH__

#include <string>
#include <unordered_map>

#include "chi_testbench_gem5/sequence_context.hh"

namespace gem5
{
namespace chi_gem5tb
{

/**
 * Name → SequenceFn lookup. Each sequence file registers itself at
 * static-init time via a file-local `Registrar` so adding a scenario
 * means dropping one .cc under sequences/ plus (optionally) one
 * Python wrapper under ruby-book/final/chi_testbench_gem5/scenarios/.
 */
class SequenceRegistry
{
  public:
    static SequenceRegistry &instance();

    void add(const std::string &name, SequenceFn fn);
    const SequenceFn *find(const std::string &name) const;

  private:
    std::unordered_map<std::string, SequenceFn> table;
};

struct Registrar
{
    Registrar(const char *name, SequenceFn fn)
    {
        SequenceRegistry::instance().add(name, std::move(fn));
    }
};

} // namespace chi_gem5tb
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_SEQUENCES_REGISTRY_HH__
