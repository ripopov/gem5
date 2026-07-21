/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/runtime/signal_access.hh"

#include <algorithm>

namespace gem5::rtl_cosim
{

std::size_t
signalBytes(const Signal &signal) noexcept
{
    return (signal.bitWidth() + 7) / 8;
}

namespace
{

std::string
signalError(const Signal &signal, const char *operation)
{
    const char *name = signal.name();
    return std::string(operation) + " signal '" + (name ? name : "<unnamed>") +
           "' failed";
}

} // anonymous namespace

bool
readSignal(const Signal &signal, std::vector<std::uint8_t> &value,
           std::string &error)
{
    value.assign(signalBytes(signal), 0);
    if (value.empty() || !signal.getValue(value.data(), value.size())) {
        error = signalError(signal, "reading");
        return false;
    }
    const unsigned remainder = signal.bitWidth() % 8;
    if (remainder != 0) {
        const std::uint8_t highMask =
            static_cast<std::uint8_t>(~((std::uint16_t{1} << remainder) - 1));
        if ((value.back() & highMask) != 0) {
            const char *name = signal.name();
            error = "signal '" + std::string(name ? name : "<unnamed>") +
                    "' returned nonzero unused high bits";
            return false;
        }
    }
    return true;
}

bool
writeSignal(Signal &signal, const std::vector<std::uint8_t> &value,
            std::string &error)
{
    if (value.size() != signalBytes(signal)) {
        error = "wrong byte count when writing signal '" +
                std::string(signal.name() ? signal.name() : "<unnamed>") + "'";
        return false;
    }
    if (!signal.setValue(value.data(), value.size())) {
        error = signalError(signal, "writing");
        return false;
    }
    return true;
}

bool
readSignalU64(const Signal &signal, std::uint64_t &value, std::string &error)
{
    if (signal.bitWidth() > 64) {
        const char *name = signal.name();
        error = "signal '" + std::string(name ? name : "<unnamed>") +
                "' is wider than 64 bits";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!readSignal(signal, bytes, error)) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= std::uint64_t{bytes[i]} << (i * 8);
    }
    return true;
}

bool
writeSignalU64(Signal &signal, std::uint64_t value, std::string &error)
{
    if (signal.bitWidth() > 64) {
        const char *name = signal.name();
        error = "signal '" + std::string(name ? name : "<unnamed>") +
                "' is wider than 64 bits";
        return false;
    }
    if (signal.bitWidth() < 64 && (value >> signal.bitWidth()) != 0) {
        const char *name = signal.name();
        error = "value does not fit signal '" +
                std::string(name ? name : "<unnamed>") + "'";
        return false;
    }
    std::vector<std::uint8_t> bytes(signalBytes(signal));
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
    return writeSignal(signal, bytes, error);
}

} // namespace gem5::rtl_cosim
