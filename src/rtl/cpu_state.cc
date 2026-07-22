/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/cpu_state.hh"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace gem5::rtl_cosim
{
namespace
{

using EncoderMap = std::unordered_map<std::string, CpuStateEncoder>;

EncoderMap &
encoders()
{
    static EncoderMap map;
    return map;
}

std::mutex &
encoderMutex()
{
    static std::mutex mutex;
    return mutex;
}

} // anonymous namespace

bool
registerCpuStateEncoder(const std::string &schema, CpuStateEncoder encoder)
{
    if (schema.empty() || !encoder) {
        return false;
    }
    const std::lock_guard guard(encoderMutex());
    return encoders().emplace(schema, encoder).second;
}

CpuStateEncoder
findCpuStateEncoder(const char *schema, std::string &error)
{
    if (!schema || !*schema) {
        error = "vendor CPU-state capability returned an empty schema";
        return nullptr;
    }
    const std::lock_guard guard(encoderMutex());
    const auto position = encoders().find(schema);
    if (position == encoders().end()) {
        error = "gem5 has no CPU-state encoder for schema '" +
                std::string(schema) + "'";
        return nullptr;
    }
    error.clear();
    return position->second;
}

bool
encodeCpuState(const char *schema, ThreadContext &context,
               std::vector<OwnedCpuStateValue> &values, std::string &error)
{
    CpuStateEncoder encoder = findCpuStateEncoder(schema, error);
    if (!encoder) {
        return false;
    }

    std::vector<OwnedCpuStateValue> candidate;
    if (!encoder(context, candidate, error)) {
        return false;
    }
    std::vector<CpuStateValue> views;
    if (!makeCpuStateViews(candidate, views, error)) {
        return false;
    }
    values = std::move(candidate);
    error.clear();
    return true;
}

bool
makeCpuStateViews(const std::vector<OwnedCpuStateValue> &owned,
                  std::vector<CpuStateValue> &views, std::string &error)
{
    std::unordered_set<std::string> names;
    std::vector<CpuStateValue> candidate;
    candidate.reserve(owned.size());
    for (const OwnedCpuStateValue &value : owned) {
        const std::size_t byteWidth = (value.bitWidth + 7) / 8;
        if (value.name.empty() || value.bitWidth == 0 ||
            value.data.size() != byteWidth ||
            !names.insert(value.name).second) {
            error = "malformed or duplicate encoded CPU-state value";
            return false;
        }
        if (value.bitWidth % 8 != 0 &&
            value.data.back() >> (value.bitWidth % 8) != 0) {
            error = "encoded CPU-state value has nonzero unused high bits";
            return false;
        }
        candidate.push_back({value.name.c_str(), value.bitWidth,
                             value.data.data(), value.data.size()});
    }
    views = std::move(candidate);
    error.clear();
    return true;
}

} // namespace gem5::rtl_cosim
