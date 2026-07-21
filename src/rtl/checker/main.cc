/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "rtl/checker/checker_config.hh"
#include "rtl/checker/memory_backend.hh"
#include "rtl/runtime/image_loader.hh"
#include "rtl/runtime/model_loader.hh"
#include "rtl/runtime/model_validator.hh"
#include "rtl/runtime/protocol/apb.hh"
#include "rtl/runtime/protocol/axi.hh"
#include "rtl/runtime/signal_access.hh"

namespace gem5::rtl_cosim
{

namespace
{

enum ExitCode
{
    Success = 0,
    ConfigurationError = 1,
    LibraryError = 2,
    ValidationError = 3,
    RuntimeError = 4,
    CycleLimit = 5
};

class CountingCallback : public SignalChangeCallback
{
  public:
    void
    update() noexcept override
    {
        ++updates;
    }
    std::uint64_t updates = 0;
};

struct RegisteredCallback
{
    Signal *signal;
    std::unique_ptr<CountingCallback> callback;
};

struct BusRuntime
{
    std::string name;
    std::unique_ptr<MemoryBackend> backend;
    std::unique_ptr<ScriptedTransactionSource> source;
    std::unique_ptr<BusTransactor> transactor;
};

bool
readText(const std::filesystem::path &path, std::string &text)
{
    std::ifstream stream(path);
    if (!stream) {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    text = buffer.str();
    return stream.good() || stream.eof();
}

Signal *
findStandaloneSignal(RtlCore &core, const std::string &name)
{
    for (std::size_t index = 0; index < core.signalCount(); ++index) {
        Signal *signal = core.signal(index).signal;
        if (signal && signal->name() && name == signal->name()) {
            return signal;
        }
    }
    return nullptr;
}

bool
driveInput(Signal &signal, const json::Value &value, std::string &error)
{
    if (signal.direction() != SignalDirection::Input) {
        error = "standalone signal '" + std::string(signal.name()) +
                "' is an RTL output";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!valueToSignalBytes(value, signal.bitWidth(), bytes, error)) {
        return false;
    }
    return writeSignal(signal, bytes, error);
}

bool
driveBusInputsToZero(const ValidationResult &validation, std::string &error)
{
    for (const ValidatedBus &bus : validation.buses) {
        for (const auto &[role, signal] : bus.signals) {
            if (signal->direction() != SignalDirection::Input) {
                continue;
            }
            std::vector<std::uint8_t> zero(signalBytes(*signal), 0);
            if (!writeSignal(*signal, zero, error)) {
                return false;
            }
        }
    }
    return true;
}

bool
setResets(RtlCore &core, bool asserted, std::string &error)
{
    for (std::size_t index = 0; index < core.signalCount(); ++index) {
        const CoreSignalBinding binding = core.signal(index);
        if (binding.role != CoreSignalRole::Reset || !binding.signal ||
            binding.signal->direction() != SignalDirection::Input) {
            continue;
        }
        const bool high = asserted ? binding.activeLevel == ActiveLevel::High
                                   : binding.activeLevel == ActiveLevel::Low;
        if (!writeSignalU64(*binding.signal, high ? 1 : 0, error)) {
            return false;
        }
    }
    return true;
}

bool
writeSegment(RtlCore &core, SparseMemory &memory, const ImageSegment &segment,
             std::string &error)
{
    if (segment.memorySize < segment.data.size()) {
        error = "image segment memory size is smaller than its file data";
        return false;
    }
    if (segment.memorySize > std::numeric_limits<std::size_t>::max()) {
        error = "image segment is too large for this host";
        return false;
    }
    for (std::size_t index = 0; index < core.memoryCount(); ++index) {
        const MemoryRegion *region = core.memory(index);
        if (!region || segment.address < region->baseAddress ||
            segment.memorySize > region->size ||
            segment.address - region->baseAddress >
                region->size - segment.memorySize) {
            continue;
        }
        const std::uint64_t offset = segment.address - region->baseAddress;
        if (!segment.data.empty() &&
            !core.writeMemory(index, offset, segment.data.data(),
                              segment.data.size())) {
            error = "vendor rejected image data for TCM '" +
                    std::string(region->name) + "'";
            return false;
        }
        std::uint64_t remaining = segment.memorySize - segment.data.size();
        std::uint64_t zeroOffset = offset + segment.data.size();
        const std::vector<std::uint8_t> zero(64 * 1024, 0);
        while (remaining) {
            const std::size_t chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, zero.size()));
            if (!core.writeMemory(index, zeroOffset, zero.data(), chunk)) {
                error = "vendor rejected zero-fill for TCM '" +
                        std::string(region->name) + "'";
                return false;
            }
            remaining -= chunk;
            zeroOffset += chunk;
        }
        return true;
    }
    if (!memory.contains(segment.address,
                         static_cast<std::size_t>(segment.memorySize))) {
        error = "image segment at 0x";
        std::ostringstream address;
        address << std::hex << segment.address;
        error += address.str() + " is outside TCM and external memory";
        return false;
    }
    if (!segment.data.empty() &&
        !memory.write(segment.address, segment.data.data(), nullptr,
                      segment.data.size())) {
        error = "cannot write image segment to external memory";
        return false;
    }
    std::uint64_t remaining = segment.memorySize - segment.data.size();
    std::uint64_t address = segment.address + segment.data.size();
    const std::vector<std::uint8_t> zero(64 * 1024, 0);
    while (remaining) {
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, zero.size()));
        if (!memory.write(address, zero.data(), nullptr, chunk)) {
            error = "cannot zero-fill image segment in external memory";
            return false;
        }
        address += chunk;
        remaining -= chunk;
    }
    return true;
}

void
printDiscovery(RtlCore &core, const ValidationResult &validation)
{
    std::cout << "core: " << core.name() << '\n';
    for (const ValidatedBus &bus : validation.buses) {
        std::cout << "bus: " << bus.bus->name()
                  << " protocol=" << protocolName(bus.bus->protocol())
                  << " role=" << busRoleName(bus.bus->role())
                  << " signals=" << bus.signals.size() << '\n';
        std::vector<std::pair<SignalRoleId, Signal *>> signals(
            bus.signals.begin(), bus.signals.end());
        std::sort(signals.begin(), signals.end(),
                  [](const auto &lhs, const auto &rhs) {
                      return lhs.first < rhs.first;
                  });
        for (const auto &[role, signal] : signals) {
            std::cout << "  " << signalRoleName(bus.bus->protocol(), role)
                      << " -> " << signal->name() << " [" << signal->bitWidth()
                      << "] "
                      << (signal->direction() == SignalDirection::Input
                              ? "input"
                              : "output")
                      << '\n';
        }
    }
    for (std::size_t index = 0; index < core.signalCount(); ++index) {
        const CoreSignalBinding binding = core.signal(index);
        std::cout << "signal: " << binding.signal->name() << " ["
                  << binding.signal->bitWidth() << "]\n";
    }
    for (std::size_t index = 0; index < core.memoryCount(); ++index) {
        const MemoryRegion *memory = core.memory(index);
        std::cout << "memory: " << memory->name << " base=0x" << std::hex
                  << memory->baseAddress << " size=0x" << memory->size
                  << std::dec << '\n';
    }
}

int
run(const std::filesystem::path &libraryPath,
    const std::filesystem::path &configPath)
{
    std::string configText;
    if (!readText(configPath, configText)) {
        std::cerr << "configuration error: cannot read '" << configPath
                  << "'\n";
        return ConfigurationError;
    }
    CheckerConfig config;
    std::string error;
    if (!parseCheckerConfig(configText, config, error)) {
        std::cerr << "configuration error: " << error << '\n';
        return ConfigurationError;
    }

    ModelLoader loader;
    if (!loader.open(libraryPath.string()) ||
        !loader.createCore(config.coreConfigJson)) {
        std::cerr << "library error: " << loader.error() << '\n';
        return LibraryError;
    }
    RtlCore &core = *loader.core();
    ValidationResult validation = validateModel(core);
    for (const std::string &warning : validation.warnings) {
        std::cerr << "warning: " << warning << '\n';
    }
    if (!validation.ok()) {
        for (const std::string &diagnostic : validation.errors) {
            std::cerr << "validation error: " << diagnostic << '\n';
        }
        return ValidationError;
    }
    printDiscovery(core, validation);
    if (std::any_of(validation.buses.begin(), validation.buses.end(),
                    [](const ValidatedBus &bus) {
                        return bus.bus->protocol() == BusProtocol::Axi3Ace;
                    })) {
        std::cout << "stop: AXI3-ACE structural validation completed; "
                     "transaction execution is out of scope\n";
        return Success;
    }

    auto memory =
        std::make_shared<SparseMemory>(config.memoryBase, config.memorySize);
    for (ImageConfig image : config.images) {
        std::filesystem::path path(image.path);
        if (path.is_relative()) {
            path = configPath.parent_path() / path;
        }
        std::vector<ImageSegment> segments;
        if (!loadImage(path.string(), image.format, image.address, segments,
                       error)) {
            std::cerr << "configuration error: " << error << '\n';
            return ConfigurationError;
        }
        for (const ImageSegment &segment : segments) {
            if (!writeSegment(core, *memory, segment, error)) {
                std::cerr << "runtime error: " << error << '\n';
                return RuntimeError;
            }
        }
    }

    if (!driveBusInputsToZero(validation, error)) {
        std::cerr << "runtime error: " << error << '\n';
        return RuntimeError;
    }
    for (const InputValue &input : config.inputs) {
        Signal *signal = findStandaloneSignal(core, input.signalName);
        if (!signal) {
            std::cerr << "configuration error: input signal '"
                      << input.signalName << "' was not discovered\n";
            return ConfigurationError;
        }
        if (!driveInput(*signal, input.value, error)) {
            std::cerr << "configuration error: input '" << input.signalName
                      << "': " << error << '\n';
            return ConfigurationError;
        }
    }
    if (!setResets(core, true, error)) {
        std::cerr << "runtime error: " << error << '\n';
        return RuntimeError;
    }
    if (!core.settle()) {
        std::cerr << "runtime error: core settle failed";
        const char *message = core.getLastError();
        if (message && *message) {
            std::cerr << ": " << message;
        }
        std::cerr << '\n';
        return RuntimeError;
    }
    for (std::size_t cycle = 0; cycle < config.resetAssertCycles; ++cycle) {
        const ClockResult result = core.clock();
        if (result != ClockResult::Completed) {
            std::cerr << "runtime error: core terminated during reset";
            const char *message = core.getLastError();
            if (message && *message) {
                std::cerr << ": " << message;
            }
            std::cerr << '\n';
            return RuntimeError;
        }
    }
    if (!setResets(core, false, error)) {
        std::cerr << "runtime error: " << error << '\n';
        return RuntimeError;
    }
    if (!core.settle()) {
        std::cerr << "runtime error: core settle failed after reset";
        const char *message = core.getLastError();
        if (message && *message) {
            std::cerr << ": " << message;
        }
        std::cerr << '\n';
        return RuntimeError;
    }

    std::vector<RegisteredCallback> callbacks;
    for (std::size_t index = 0; index < core.signalCount(); ++index) {
        Signal *signal = core.signal(index).signal;
        std::vector<std::uint8_t> initial;
        if (!readSignal(*signal, initial, error)) {
            std::cerr << "runtime error: initial signal sample: " << error
                      << '\n';
            return RuntimeError;
        }
        if (signal->direction() == SignalDirection::Output) {
            auto callback = std::make_unique<CountingCallback>();
            signal->setChangeCallback(callback.get());
            callbacks.push_back({signal, std::move(callback)});
        }
    }

    std::vector<BusRuntime> buses;
    for (std::size_t index = 0; index < validation.buses.size(); ++index) {
        const ValidatedBus &bus = validation.buses[index];
        BusRuntime runtime;
        runtime.name = bus.bus->name();
        TransactorLimits limits;
        limits.maxPending = config.memory.maxPending;
        limits.tokenBase = (static_cast<std::uint64_t>(index) + 1) << 48;
        if (bus.bus->role() == BusRole::Initiator) {
            runtime.backend =
                std::make_unique<MemoryBackend>(memory, config.memory);
            if (bus.bus->protocol() == BusProtocol::Apb) {
                runtime.transactor = createApbInitiatorTransactor(
                    bus, *runtime.backend, limits);
            } else {
                runtime.transactor = createAxiInitiatorTransactor(
                    bus, *runtime.backend, limits);
            }
        } else {
            runtime.source = std::make_unique<ScriptedTransactionSource>(
                config.memory.maxPending);
            if (auto it = config.transactions.find(runtime.name);
                it != config.transactions.end()) {
                for (CheckerConfig::Transaction transaction : it->second) {
                    runtime.source->add(std::move(transaction.request),
                                        std::move(transaction.expectation));
                }
            }
            if (bus.bus->protocol() == BusProtocol::Apb) {
                runtime.transactor =
                    createApbTargetTransactor(bus, *runtime.source, limits);
            } else {
                runtime.transactor =
                    createAxiTargetTransactor(bus, *runtime.source, limits);
            }
        }
        buses.push_back(std::move(runtime));
    }
    for (const auto &[name, requests] : config.transactions) {
        const bool exists = std::any_of(
            buses.begin(), buses.end(), [&](const BusRuntime &bus) {
                return bus.name == name && bus.source;
            });
        if (!exists) {
            std::cerr << "configuration error: transactions name unknown "
                         "or non-target bus '"
                      << name << "'\n";
            for (RegisteredCallback &callback : callbacks) {
                callback.signal->setChangeCallback(nullptr);
            }
            return ConfigurationError;
        }
    }

    std::uint64_t cycles = 0;
    std::string stopReason;
    for (; cycles < config.maxCycles; ++cycles) {
        for (BusRuntime &bus : buses) {
            if (!bus.transactor->beforeClock()) {
                error = "bus '" + bus.name +
                        "': " + bus.transactor->getLastError();
                stopReason = "error";
                break;
            }
        }
        if (!error.empty()) {
            break;
        }
        if (!core.settle()) {
            const char *message = core.getLastError();
            error = "core settle failed";
            if (message && *message) {
                error += ": " + std::string(message);
            }
            stopReason = "error";
            break;
        }
        for (BusRuntime &bus : buses) {
            if (!bus.transactor->afterSettle()) {
                error = "bus '" + bus.name +
                        "': " + bus.transactor->getLastError();
                stopReason = "error";
                break;
            }
        }
        if (!error.empty()) {
            break;
        }
        const ClockResult result = core.clock();
        if (result == ClockResult::Error) {
            const char *message = core.getLastError();
            error = "core clock failed";
            if (message && *message) {
                error += ": " + std::string(message);
            }
            stopReason = "error";
            break;
        }
        if (result == ClockResult::Finished) {
            stopReason = "model finished";
            if (!config.stopOnFinish) {
                error = "model finished while stop_on_finish is false";
            }
            ++cycles;
            break;
        }
        for (BusRuntime &bus : buses) {
            if (!bus.transactor->afterClock()) {
                error = "bus '" + bus.name +
                        "': " + bus.transactor->getLastError();
                stopReason = "error";
                break;
            }
        }
        if (!error.empty()) {
            break;
        }
        for (BusRuntime &bus : buses) {
            if (bus.backend) {
                bus.backend->advance();
            }
            if (bus.source) {
                bus.source->advance();
            }
        }
        const bool idle =
            core.isIdle() &&
            std::all_of(buses.begin(), buses.end(), [](const BusRuntime &bus) {
                return bus.transactor->isIdle() &&
                       (!bus.backend || bus.backend->isIdle()) &&
                       (!bus.source || bus.source->isIdle());
            });
        if (config.stopOnIdle && idle) {
            ++cycles;
            stopReason = "idle";
            break;
        }
    }
    for (RegisteredCallback &callback : callbacks) {
        callback.signal->setChangeCallback(nullptr);
    }

    for (const BusRuntime &bus : buses) {
        if (!bus.source || bus.source->error().empty()) {
            continue;
        }
        const std::string sourceError =
            "bus '" + bus.name + "' source: " + bus.source->error();
        if (error.empty()) {
            error = sourceError;
        } else {
            error += "; " + sourceError;
        }
    }

    std::uint64_t requests = 0;
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    for (const BusRuntime &bus : buses) {
        if (bus.backend) {
            requests += bus.backend->requests();
            reads += bus.backend->reads();
            writes += bus.backend->writes();
        } else if (bus.source) {
            requests += bus.source->requestCount();
            reads += bus.source->readCount();
            writes += bus.source->writeCount();
        }
    }
    std::uint64_t callbackUpdates = 0;
    for (const RegisteredCallback &callback : callbacks) {
        callbackUpdates += callback.callback->updates;
    }
    std::cout << "traffic: requests=" << requests << " reads=" << reads
              << " writes=" << writes << '\n';
    std::cout << "callbacks: updates=" << callbackUpdates << '\n';
    if (!error.empty()) {
        std::cerr << "runtime error after " << cycles << " cycles: " << error
                  << '\n';
        return RuntimeError;
    }
    if (stopReason.empty()) {
        std::cerr << "timeout: cycle limit " << config.maxCycles
                  << " reached\n";
        return CycleLimit;
    }
    std::cout << "stop: " << stopReason << " after " << cycles << " cycles\n";
    return Success;
}

} // anonymous namespace

} // namespace gem5::rtl_cosim

int
main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "usage: rtl-cosim-check VENDOR_LIBRARY CHECKER_JSON\n";
        return gem5::rtl_cosim::ConfigurationError;
    }
    return gem5::rtl_cosim::run(argv[1], argv[2]);
}
