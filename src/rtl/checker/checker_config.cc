/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/checker/checker_config.hh"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

namespace gem5::rtl_cosim
{

namespace
{

bool
fail(std::string &error, const std::string &path, const std::string &message)
{
    error = path + ": " + message;
    return false;
}

bool
allowedMembers(const json::Value &object,
               std::initializer_list<std::string_view> allowed,
               const std::string &path, std::string &error)
{
    if (!object.isObject()) {
        return fail(error, path, "expected an object");
    }
    const std::set<std::string_view> names(allowed);
    for (const auto &[name, value] : object.asObject()) {
        if (names.count(name) == 0) {
            return fail(error, path + "." + name, "unknown member");
        }
    }
    return true;
}

bool
getUnsigned(const json::Value &object, std::string_view name,
            std::uint64_t &value, const std::string &path, std::string &error,
            bool required = false)
{
    const json::Value *member = object.find(name);
    if (!member) {
        if (required) {
            return fail(error, path + "." + std::string(name), "is required");
        }
        return true;
    }
    std::string reason;
    if (!json::unsignedInteger(*member, value, reason)) {
        return fail(error, path + "." + std::string(name), reason);
    }
    return true;
}

bool
getBool(const json::Value &object, std::string_view name, bool &value,
        const std::string &path, std::string &error)
{
    const json::Value *member = object.find(name);
    if (!member) {
        return true;
    }
    if (!member->isBool()) {
        return fail(error, path + "." + std::string(name),
                    "expected a boolean");
    }
    value = member->asBool();
    return true;
}

bool
getString(const json::Value &object, std::string_view name, std::string &value,
          const std::string &path, std::string &error, bool required = false)
{
    const json::Value *member = object.find(name);
    if (!member) {
        if (required) {
            return fail(error, path + "." + std::string(name), "is required");
        }
        return true;
    }
    if (!member->isString()) {
        return fail(error, path + "." + std::string(name),
                    "expected a string");
    }
    value = member->asString();
    return true;
}

int
hexDigit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

bool
parseHexBytes(std::string_view text, bool numericOrder,
              std::vector<std::uint8_t> &bytes, std::string &error)
{
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        numericOrder = true;
    }
    if (text.empty()) {
        return fail(error, "hex value", "has no digits");
    }
    std::string digits(text);
    if (digits.size() % 2) {
        digits.insert(digits.begin(), '0');
    }
    bytes.clear();
    bytes.reserve(digits.size() / 2);
    for (std::size_t index = 0; index < digits.size(); index += 2) {
        const int high = hexDigit(digits[index]);
        const int low = hexDigit(digits[index + 1]);
        if (high < 0 || low < 0) {
            return fail(error, "hex value", "contains a non-hex digit");
        }
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    if (numericOrder) {
        std::reverse(bytes.begin(), bytes.end());
    }
    return true;
}

bool
valueToByteVector(const json::Value &value, bool numericString,
                  std::vector<std::uint8_t> &bytes, const std::string &path,
                  std::string &error)
{
    if (value.isString()) {
        if (!parseHexBytes(value.asString(), numericString, bytes, error)) {
            error = path + ": " + error;
            return false;
        }
        return true;
    }
    if (!value.isArray()) {
        return fail(error, path, "expected a hex string or byte array");
    }
    bytes.clear();
    for (std::size_t index = 0; index < value.asArray().size(); ++index) {
        std::uint64_t byte = 0;
        std::string reason;
        if (!json::unsignedInteger(value.asArray()[index], byte, reason) ||
            byte > 255) {
            return fail(error, path + "[" + std::to_string(index) + "]",
                        "invalid byte");
        }
        bytes.push_back(static_cast<std::uint8_t>(byte));
    }
    return true;
}

bool
parseTransaction(const json::Value &value,
                 CheckerConfig::Transaction &transaction,
                 std::uint64_t defaultToken, const std::string &path,
                 std::string &error)
{
    if (!allowedMembers(value,
                        {"token", "id", "address", "write", "beat_bytes",
                         "beats", "burst", "data", "byte_enable",
                         "expect_error", "expect_data"},
                        path, error)) {
        return false;
    }
    MemoryRequest &request = transaction.request;
    request.token = defaultToken;
    std::uint64_t number = request.token;
    if (!getUnsigned(value, "token", number, path, error)) {
        return false;
    }
    request.token = number;
    number = 0;
    if (!getUnsigned(value, "id", number, path, error) ||
        number > std::numeric_limits<std::uint32_t>::max()) {
        return fail(error, path + ".id", "ID is out of range");
    }
    request.id = static_cast<std::uint32_t>(number);
    if (!getUnsigned(value, "address", request.address, path, error, true)) {
        return false;
    }
    if (!getBool(value, "write", request.write, path, error)) {
        return false;
    }
    number = 0;
    if (!getUnsigned(value, "beat_bytes", number, path, error, true) ||
        number == 0 || number > std::numeric_limits<std::size_t>::max()) {
        return fail(error, path + ".beat_bytes", "invalid beat size");
    }
    request.beatBytes = static_cast<std::size_t>(number);
    std::uint64_t beats = 1;
    if (!getUnsigned(value, "beats", beats, path, error) || beats == 0 ||
        beats > std::numeric_limits<std::size_t>::max()) {
        return fail(error, path + ".beats", "invalid beat count");
    }
    if (beats > std::numeric_limits<std::size_t>::max() / request.beatBytes) {
        return fail(error, path + ".beats", "transaction size is too large");
    }
    const std::size_t transactionBytes =
        static_cast<std::size_t>(beats) * request.beatBytes;
    std::string burst = "increment";
    if (!getString(value, "burst", burst, path, error)) {
        return false;
    }
    if (burst == "fixed") {
        request.burst = BurstType::Fixed;
    } else if (burst == "increment") {
        request.burst = BurstType::Increment;
    } else if (burst == "wrap") {
        request.burst = BurstType::Wrap;
    } else {
        return fail(error, path + ".burst",
                    "expected fixed, increment, or wrap");
    }

    const json::Value *data = value.find("data");
    if (request.write) {
        if (!data) {
            return fail(error, path + ".data", "write data is required");
        }
        if (!valueToByteVector(*data, false, request.data, path + ".data",
                               error)) {
            return false;
        }
        if (request.data.size() != transactionBytes) {
            return fail(error, path + ".data", "length does not match beats");
        }
    } else if (data) {
        return fail(error, path + ".data",
                    "read request must not contain data");
    }
    const json::Value *enable = value.find("byte_enable");
    if (enable) {
        if (!valueToByteVector(*enable, false, request.byteEnable,
                               path + ".byte_enable", error)) {
            return false;
        }
        for (std::uint8_t &byte : request.byteEnable) {
            byte = byte != 0;
        }
    } else {
        request.byteEnable.assign(transactionBytes, 1);
    }
    if (request.byteEnable.size() != transactionBytes) {
        return fail(error, path + ".byte_enable",
                    "length does not match beats");
    }
    std::string reason;
    if (!request.valid(reason)) {
        return fail(error, path, reason);
    }
    if (!getBool(value, "expect_error", transaction.expectation.error, path,
                 error)) {
        return false;
    }
    if (const json::Value *expected = value.find("expect_data")) {
        std::vector<std::uint8_t> data;
        if (!valueToByteVector(*expected, false, data, path + ".expect_data",
                               error)) {
            return false;
        }
        if (request.write) {
            return fail(error, path + ".expect_data",
                        "write response cannot contain data");
        }
        if (data.size() != transactionBytes) {
            return fail(error, path + ".expect_data",
                        "length does not match beats");
        }
        transaction.expectation.data = std::move(data);
    }
    return true;
}

} // anonymous namespace

bool
valueToSignalBytes(const json::Value &value, std::size_t bitWidth,
                   std::vector<std::uint8_t> &bytes, std::string &error)
{
    if (bitWidth == 0) {
        return fail(error, "signal value", "signal width is zero");
    }
    const std::size_t size = (bitWidth + 7) / 8;
    if (value.isNumber()) {
        std::uint64_t number = 0;
        std::string reason;
        if (bitWidth > 64 || !json::unsignedInteger(value, number, reason)) {
            return fail(error, "signal value", "number does not fit signal");
        }
        if (bitWidth < 64 && (number >> bitWidth) != 0) {
            return fail(error, "signal value", "number does not fit signal");
        }
        bytes.assign(size, 0);
        for (std::size_t index = 0; index < size; ++index) {
            bytes[index] = static_cast<std::uint8_t>(number >> (index * 8));
        }
    } else if (value.isString()) {
        if (!parseHexBytes(value.asString(), true, bytes, error)) {
            return false;
        }
        if (bytes.size() > size) {
            return fail(error, "signal value", "does not fit signal width");
        }
        bytes.resize(size, 0);
    } else {
        return fail(error, "signal value",
                    "expected an integer or hex string");
    }
    const unsigned remainder = bitWidth % 8;
    if (remainder && (bytes.back() >> remainder) != 0) {
        return fail(error, "signal value", "sets bits above signal width");
    }
    return true;
}

bool
parseCheckerConfig(std::string_view text, CheckerConfig &config,
                   std::string &error)
{
    config = CheckerConfig{};
    json::Value root;
    if (!json::parse(text, root, error)) {
        return false;
    }
    if (!allowedMembers(
            root, {"core", "reset", "inputs", "memory", "transactions", "run"},
            "$", error)) {
        return false;
    }

    if (const json::Value *core = root.find("core")) {
        if (!allowedMembers(*core, {"config"}, "$.core", error)) {
            return false;
        }
        if (const json::Value *vendor = core->find("config")) {
            if (!vendor->isObject()) {
                return fail(error, "$.core.config", "expected an object");
            }
            config.coreConfigJson = vendor->serialize();
        }
    }
    if (const json::Value *reset = root.find("reset")) {
        if (!allowedMembers(*reset, {"assert_cycles"}, "$.reset", error)) {
            return false;
        }
        std::uint64_t cycles = config.resetAssertCycles;
        if (!getUnsigned(*reset, "assert_cycles", cycles, "$.reset", error) ||
            cycles > std::numeric_limits<std::size_t>::max()) {
            return fail(error, "$.reset.assert_cycles", "value is too large");
        }
        config.resetAssertCycles = static_cast<std::size_t>(cycles);
    }
    if (const json::Value *inputs = root.find("inputs")) {
        if (!inputs->isObject()) {
            return fail(error, "$.inputs", "expected an object");
        }
        for (const auto &[name, value] : inputs->asObject()) {
            config.inputs.push_back({name, value});
        }
    }
    if (const json::Value *memory = root.find("memory")) {
        if (!allowedMembers(*memory,
                            {"base", "size", "latency_cycles", "max_pending",
                             "error_ranges", "images"},
                            "$.memory", error)) {
            return false;
        }
        if (!getUnsigned(*memory, "base", config.memoryBase, "$.memory",
                         error) ||
            !getUnsigned(*memory, "size", config.memorySize, "$.memory",
                         error)) {
            return false;
        }
        std::uint64_t number = config.memory.latencyCycles;
        if (!getUnsigned(*memory, "latency_cycles", number, "$.memory",
                         error) ||
            number > std::numeric_limits<std::size_t>::max()) {
            return fail(error, "$.memory.latency_cycles",
                        "value is too large");
        }
        config.memory.latencyCycles = static_cast<std::size_t>(number);
        number = config.memory.maxPending;
        if (!getUnsigned(*memory, "max_pending", number, "$.memory", error) ||
            number == 0 || number > std::numeric_limits<std::size_t>::max()) {
            return fail(error, "$.memory.max_pending", "invalid value");
        }
        config.memory.maxPending = static_cast<std::size_t>(number);
        if (const json::Value *ranges = memory->find("error_ranges")) {
            if (!ranges->isArray()) {
                return fail(error, "$.memory.error_ranges",
                            "expected an array");
            }
            for (std::size_t index = 0; index < ranges->asArray().size();
                 ++index) {
                const auto &range = ranges->asArray()[index];
                const std::string path =
                    "$.memory.error_ranges[" + std::to_string(index) + "]";
                if (!allowedMembers(range, {"base", "size"}, path, error)) {
                    return false;
                }
                std::uint64_t base = 0;
                std::uint64_t size = 0;
                if (!getUnsigned(range, "base", base, path, error, true) ||
                    !getUnsigned(range, "size", size, path, error, true)) {
                    return false;
                }
                config.memory.errorRanges.emplace_back(base, size);
            }
        }
        if (const json::Value *images = memory->find("images")) {
            if (!images->isArray()) {
                return fail(error, "$.memory.images", "expected an array");
            }
            for (std::size_t index = 0; index < images->asArray().size();
                 ++index) {
                const auto &image = images->asArray()[index];
                const std::string path =
                    "$.memory.images[" + std::to_string(index) + "]";
                if (!allowedMembers(image, {"path", "format", "address"}, path,
                                    error)) {
                    return false;
                }
                ImageConfig parsed;
                if (!getString(image, "path", parsed.path, path, error,
                               true) ||
                    !getString(image, "format", parsed.format, path, error) ||
                    !getUnsigned(image, "address", parsed.address, path,
                                 error)) {
                    return false;
                }
                config.images.push_back(std::move(parsed));
            }
        }
    }
    if (config.memorySize == 0 ||
        config.memorySize >
            std::numeric_limits<std::uint64_t>::max() - config.memoryBase) {
        return fail(error, "$.memory", "invalid address range");
    }

    if (const json::Value *transactions = root.find("transactions")) {
        if (!transactions->isObject()) {
            return fail(error, "$.transactions", "expected an object");
        }
        std::uint64_t token = 1;
        for (const auto &[bus, list] : transactions->asObject()) {
            if (!list.isArray()) {
                return fail(error, "$.transactions." + bus,
                            "expected an array");
            }
            auto &requests = config.transactions[bus];
            std::set<std::uint64_t> busTokens;
            for (std::size_t index = 0; index < list.asArray().size();
                 ++index) {
                CheckerConfig::Transaction transaction;
                const std::string path = "$.transactions." + bus + "[" +
                                         std::to_string(index) + "]";
                if (!parseTransaction(list.asArray()[index], transaction,
                                      token++, path, error)) {
                    return false;
                }
                if (!busTokens.insert(transaction.request.token).second) {
                    return fail(error, path + ".token", "duplicate token");
                }
                requests.push_back(std::move(transaction));
            }
        }
    }
    if (const json::Value *run = root.find("run")) {
        if (!allowedMembers(*run,
                            {"max_cycles", "stop_on_idle", "stop_on_finish"},
                            "$.run", error)) {
            return false;
        }
        if (!getUnsigned(*run, "max_cycles", config.maxCycles, "$.run",
                         error) ||
            config.maxCycles == 0) {
            return fail(error, "$.run.max_cycles", "must be positive");
        }
        if (!getBool(*run, "stop_on_idle", config.stopOnIdle, "$.run",
                     error) ||
            !getBool(*run, "stop_on_finish", config.stopOnFinish, "$.run",
                     error)) {
            return false;
        }
    }
    return true;
}

} // namespace gem5::rtl_cosim
