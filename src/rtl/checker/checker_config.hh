/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_COSIM_CHECKER_CONFIG_HH__
#define __RTL_COSIM_CHECKER_CONFIG_HH__

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "rtl/checker/memory_backend.hh"
#include "rtl/runtime/json.hh"

namespace gem5::rtl_cosim
{

struct InputValue
{
    std::string signalName;
    json::Value value;
};

struct ImageConfig
{
    std::string path;
    std::string format = "auto";
    std::uint64_t address = 0;
};

struct CheckerConfig
{
    std::string coreConfigJson = "{}";
    std::size_t resetAssertCycles = 5;
    std::vector<InputValue> inputs;
    std::uint64_t memoryBase = 0;
    std::uint64_t memorySize = 64 * 1024 * 1024;
    MemoryBackendConfig memory;
    std::vector<ImageConfig> images;
    struct Transaction
    {
        MemoryRequest request;
        ScriptedTransactionSource::Expectation expectation;
    };
    std::map<std::string, std::vector<Transaction>, std::less<>> transactions;
    std::uint64_t maxCycles = 1000000;
    bool stopOnIdle = true;
    bool stopOnFinish = true;
};

bool parseCheckerConfig(std::string_view text, CheckerConfig &config,
                        std::string &error);
bool valueToSignalBytes(const json::Value &value, std::size_t bitWidth,
                        std::vector<std::uint8_t> &bytes, std::string &error);

} // namespace gem5::rtl_cosim

#endif // __RTL_COSIM_CHECKER_CONFIG_HH__
