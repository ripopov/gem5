/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/rtl_core.hh"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/RtlCosim.hh"
#include "rtl/runtime/image_loader.hh"
#include "rtl/runtime/protocol/apb.hh"
#include "rtl/runtime/protocol/axi.hh"
#include "rtl/runtime/signal_access.hh"
#include "sim/system.hh"

namespace gem5::rtl_cosim
{

namespace
{

std::string
readTextFile(const std::string &path)
{
    std::ifstream stream(path);
    fatal_if(!stream, "cannot read RTL model configuration '%s'", path);
    std::ostringstream contents;
    contents << stream.rdbuf();
    fatal_if(!stream.good() && !stream.eof(),
             "cannot read RTL model configuration '%s'", path);
    return contents.str();
}

int
hexDigit(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool
parseSignalInteger(std::string_view text, std::size_t bitWidth,
                   std::vector<std::uint8_t> &bytes, std::string &error)
{
    bytes.assign((bitWidth + 7) / 8, 0);
    if (text.empty()) {
        error = "empty integer";
        return false;
    }
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        if (text.empty()) {
            error = "empty hexadecimal integer";
            return false;
        }
        std::size_t nibble = 0;
        for (auto it = text.rbegin(); it != text.rend(); ++it, ++nibble) {
            const int digit = hexDigit(*it);
            if (digit < 0) {
                error = "invalid hexadecimal integer";
                return false;
            }
            const std::size_t bit = nibble * 4;
            if (bit >= bitWidth) {
                if (digit != 0) {
                    error = "integer does not fit signal width";
                    return false;
                }
                continue;
            }
            bytes[bit / 8] |= static_cast<std::uint8_t>(digit << (bit % 8));
        }
    } else {
        std::uint64_t value = 0;
        const auto result = std::from_chars(
            text.data(), text.data() + text.size(), value, 10);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
            error = "invalid decimal integer";
            return false;
        }
        if (bitWidth < 64 && (value >> bitWidth) != 0) {
            error = "integer does not fit signal width";
            return false;
        }
        for (std::size_t index = 0; index < bytes.size() && index < 8;
             ++index) {
            bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
        }
    }
    const unsigned remainder = bitWidth % 8;
    if (remainder != 0 &&
        (bytes.back() >> remainder) != 0) {
        error = "integer does not fit signal width";
        return false;
    }
    return true;
}

template <class Values>
void
checkUniqueNames(const Values &values, const char *parameter)
{
    std::unordered_set<std::string> names;
    for (const std::string &name : values) {
        fatal_if(name.empty(), "%s contains an empty API name", parameter);
        fatal_if(!names.insert(name).second,
                 "%s contains duplicate API name '%s'", parameter, name);
    }
}

} // anonymous namespace

void
RtlCoreSimObject::OutputCallback::update() noexcept
{
    owner.updateOutput(kind, index);
}

RtlCoreSimObject::RtlCoreSimObject(const Params &params)
    : ClockedObject(params),
      _tickEvent([this] { tick(); }, name() + ".tick"),
      _resetRemaining(params.initial_reset_cycles),
      _maxPending(params.max_pending_transactions),
      _imagePath(params.image), _imageFormat(params.image_format),
      _rawImageAddress(params.raw_image_address),
      _imageBusName(params.image_bus)
{
    fatal_if(_maxPending == 0,
             "%s: max_pending_transactions must be positive", name());
    loadModel(params);
    buildBusMappings(params);
    buildSignalMappings(params);
}

RtlCoreSimObject::~RtlCoreSimObject()
{
    if (_tickEvent.scheduled()) {
        deschedule(_tickEvent);
    }
    unregisterCallbacks();
}

void
RtlCoreSimObject::loadModel(const Params &params)
{
    std::string config = params.model_config;
    fatal_if(!params.model_config_file.empty() && config != "{}",
             "%s: model_config and model_config_file are mutually exclusive",
             name());
    if (!params.model_config_file.empty()) {
        config = readTextFile(params.model_config_file);
    }
    fatal_if(!_loader.open(params.library), "%s: %s", name(),
             _loader.error());
    fatal_if(!_loader.createCore(config), "%s: %s", name(),
             _loader.error());
    _core = _loader.core();
    _validation = validateModel(*_core);
    for (const std::string &warning : _validation.warnings) {
        warn("%s: %s", name(), warning);
    }
    for (const std::string &error : _validation.errors) {
        warn("%s: RTL validation: %s", name(), error);
    }
    fatal_if(!_validation.ok(), "%s: vendor RTL model validation failed",
             name());
}

void
RtlCoreSimObject::buildBusMappings(const Params &params)
{
    checkUniqueNames(params.initiator_bus_names, "initiator_bus_names");
    checkUniqueNames(params.target_bus_names, "target_bus_names");
    fatal_if(params.target_addr_ranges.size() !=
                 params.target_bus_names.size(),
             "%s: target_addr_ranges must match target_bus_names", name());

    std::unordered_map<std::string, const ValidatedBus *> discovered;
    for (const ValidatedBus &bus : _validation.buses) {
        fatal_if(bus.bus->protocol() == BusProtocol::Axi3Ace,
                 "%s: AXI3-ACE bus '%s' is validation-only and cannot be "
                 "executed by gem5", name(), bus.bus->name());
        discovered.emplace(bus.bus->name(), &bus);
    }

    auto findBus = [&](const std::string &apiName, BusRole role) {
        const auto position = discovered.find(apiName);
        fatal_if(position == discovered.end(), "%s: configured bus '%s' "
                 "was not discovered", name(), apiName);
        fatal_if(position->second->bus->role() != role,
                 "%s: bus '%s' has the wrong RTL role", name(), apiName);
        const ValidatedBus *result = position->second;
        discovered.erase(position);
        return result;
    };

    for (std::size_t index = 0; index < params.initiator_bus_names.size();
         ++index) {
        const std::string &apiName = params.initiator_bus_names[index];
        const ValidatedBus *bus = findBus(apiName, BusRole::Initiator);
        BusRuntime runtime;
        runtime.name = apiName;
        runtime.backend = std::make_unique<Gem5InitiatorBackend>(
            name() + ".initiator_ports[" + std::to_string(index) + "]",
            static_cast<PortID>(index),
            params.system->getRequestorId(this, apiName), _maxPending,
            [this] { wake(); });
        const TransactorLimits limits{_maxPending, 256,
                                      (index + 1) << 48};
        if (bus->bus->protocol() == BusProtocol::Apb) {
            runtime.transactor = createApbInitiatorTransactor(
                *bus, *runtime.backend, limits);
        } else {
            runtime.transactor = createAxiInitiatorTransactor(
                *bus, *runtime.backend, limits);
        }
        _initiatorBuses.push_back(std::move(runtime));
    }

    for (std::size_t index = 0; index < params.target_bus_names.size();
         ++index) {
        const std::string &apiName = params.target_bus_names[index];
        const ValidatedBus *bus = findBus(apiName, BusRole::Target);
        BusRuntime runtime;
        runtime.name = apiName;
        runtime.source = std::make_unique<Gem5TargetSource>(
            name() + ".target_ports[" + std::to_string(index) + "]",
            static_cast<PortID>(index), params.target_addr_ranges[index],
            _maxPending, [this] { wake(); });
        const TransactorLimits limits{_maxPending, 256,
                                      (index + 1) << 48};
        if (bus->bus->protocol() == BusProtocol::Apb) {
            runtime.transactor = createApbTargetTransactor(
                *bus, *runtime.source, limits);
        } else {
            runtime.transactor = createAxiTargetTransactor(
                *bus, *runtime.source, limits);
        }
        _targetBuses.push_back(std::move(runtime));
    }
    fatal_if(!discovered.empty(), "%s: discovered bus '%s' has no vector "
             "port mapping", name(), discovered.begin()->first);
}

void
RtlCoreSimObject::buildSignalMappings(const Params &params)
{
    checkUniqueNames(params.interrupt_input_names, "interrupt_input_names");
    checkUniqueNames(params.interrupt_output_names, "interrupt_output_names");
    checkUniqueNames(params.reset_input_names, "reset_input_names");
    checkUniqueNames(params.reset_output_names, "reset_output_names");
    checkUniqueNames(params.io_input_names, "io_input_names");
    checkUniqueNames(params.io_output_names, "io_output_names");
    fatal_if(!params.io_input_values.empty() &&
                 params.io_input_values.size() != params.io_input_names.size(),
             "%s: io_input_values must be empty or match io_input_names",
             name());

    std::unordered_map<std::string, CoreSignalBinding> discovered;
    for (std::size_t index = 0; index < _core->signalCount(); ++index) {
        const CoreSignalBinding binding = _core->signal(index);
        discovered.emplace(binding.signal->name(), binding);
    }

    auto findSignal = [&](const std::string &apiName, CoreSignalRole role,
                          SignalDirection direction) {
        const auto position = discovered.find(apiName);
        fatal_if(position == discovered.end(), "%s: configured signal '%s' "
                 "was not discovered", name(), apiName);
        const CoreSignalBinding binding = position->second;
        fatal_if(binding.role != role ||
                     binding.signal->direction() != direction,
                 "%s: signal '%s' has an incompatible role or direction",
                 name(), apiName);
        discovered.erase(position);
        return binding;
    };

    for (std::size_t index = 0; index < params.interrupt_input_names.size();
         ++index) {
        StandaloneBinding binding{findSignal(
            params.interrupt_input_names[index], CoreSignalRole::Interrupt,
            SignalDirection::Input)};
        _interruptInputs.push_back(std::move(binding));
        auto port = std::make_unique<SignalSinkPort<bool>>(
            name() + ".interrupt_inputs[" + std::to_string(index) + "]",
            static_cast<PortID>(index));
        port->onChange([this, index](const bool &value) {
            interruptInputChanged(index, value);
        });
        _interruptInputPorts.push_back(std::move(port));
    }
    for (std::size_t index = 0; index < params.interrupt_output_names.size();
         ++index) {
        _interruptOutputs.push_back({findSignal(
            params.interrupt_output_names[index], CoreSignalRole::Interrupt,
            SignalDirection::Output)});
        _interruptOutputPorts.push_back(std::make_unique<IntSourcePinBase>(
            name() + ".interrupt_outputs[" + std::to_string(index) + "]",
            static_cast<PortID>(index)));
    }
    for (std::size_t index = 0; index < params.reset_input_names.size();
         ++index) {
        _resetInputs.push_back({findSignal(
            params.reset_input_names[index], CoreSignalRole::Reset,
            SignalDirection::Input)});
        auto port = std::make_unique<SignalSinkPort<bool>>(
            name() + ".reset_inputs[" + std::to_string(index) + "]",
            static_cast<PortID>(index));
        port->onChange([this, index](const bool &value) {
            resetInputChanged(index, value);
        });
        _resetInputPorts.push_back(std::move(port));
    }
    for (std::size_t index = 0; index < params.reset_output_names.size();
         ++index) {
        _resetOutputs.push_back({findSignal(
            params.reset_output_names[index], CoreSignalRole::Reset,
            SignalDirection::Output)});
        _resetOutputPorts.push_back(
            std::make_unique<SignalSourcePort<bool>>(
                name() + ".reset_outputs[" + std::to_string(index) + "]",
                static_cast<PortID>(index)));
    }
    for (std::size_t index = 0; index < params.io_input_names.size();
         ++index) {
        StandaloneBinding binding{findSignal(
            params.io_input_names[index], CoreSignalRole::Io,
            SignalDirection::Input)};
        const std::string value = params.io_input_values.empty()
                                      ? "0"
                                      : params.io_input_values[index];
        std::string error;
        fatal_if(!parseSignalInteger(value, binding.api.signal->bitWidth(),
                                     binding.initialValue, error),
                 "%s: invalid initial value for io input '%s': %s", name(),
                 params.io_input_names[index], error);
        _ioInputs.push_back(std::move(binding));
        auto port = std::make_unique<SignalSinkPort<SignalValue>>(
            name() + ".io_inputs[" + std::to_string(index) + "]",
            static_cast<PortID>(index));
        port->onChange([this, index](const SignalValue &value) {
            ioInputChanged(index, value);
        });
        _ioInputPorts.push_back(std::move(port));
    }
    for (std::size_t index = 0; index < params.io_output_names.size();
         ++index) {
        _ioOutputs.push_back({findSignal(
            params.io_output_names[index], CoreSignalRole::Io,
            SignalDirection::Output)});
        _ioOutputPorts.push_back(
            std::make_unique<SignalSourcePort<SignalValue>>(
                name() + ".io_outputs[" + std::to_string(index) + "]",
                static_cast<PortID>(index)));
    }
    fatal_if(!discovered.empty(), "%s: discovered standalone signal '%s' "
             "has no vector port mapping", name(), discovered.begin()->first);
}

Port &
RtlCoreSimObject::getPort(const std::string &ifName, PortID index)
{
    fatal_if(index < 0, "%s: vector port '%s' requires an index", name(),
             ifName);
    const std::size_t position = static_cast<std::size_t>(index);
    if (ifName == "initiator_ports" && position < _initiatorBuses.size()) {
        return _initiatorBuses[position].backend->port();
    }
    if (ifName == "target_ports" && position < _targetBuses.size()) {
        return _targetBuses[position].source->port();
    }
    if (ifName == "interrupt_inputs" &&
        position < _interruptInputPorts.size()) {
        return *_interruptInputPorts[position];
    }
    if (ifName == "interrupt_outputs" &&
        position < _interruptOutputPorts.size()) {
        return *_interruptOutputPorts[position];
    }
    if (ifName == "reset_inputs" && position < _resetInputPorts.size()) {
        return *_resetInputPorts[position];
    }
    if (ifName == "reset_outputs" && position < _resetOutputPorts.size()) {
        return *_resetOutputPorts[position];
    }
    if (ifName == "io_inputs" && position < _ioInputPorts.size()) {
        return *_ioInputPorts[position];
    }
    if (ifName == "io_outputs" && position < _ioOutputPorts.size()) {
        return *_ioOutputPorts[position];
    }
    return ClockedObject::getPort(ifName, index);
}

void
RtlCoreSimObject::init()
{
    ClockedObject::init();
    for (const BusRuntime &bus : _initiatorBuses) {
        fatal_if(!bus.backend->port().isConnected(),
                 "%s: initiator bus '%s' is not connected", name(),
                 bus.name);
    }
    for (const BusRuntime &bus : _targetBuses) {
        fatal_if(!bus.source->port().isConnected(),
                 "%s: target bus '%s' is not connected", name(), bus.name);
    }
}

void
RtlCoreSimObject::startup()
{
    ClockedObject::startup();
    _started = true;
    synchronizeInputs();
    registerCallbacks();
    synchronizeOutputs();
    loadConfiguredImage();
    setInitialReset(_resetRemaining > Cycles(0));
    schedule(_tickEvent, clockEdge(Cycles(0)));
}

void
RtlCoreSimObject::registerCallbacks()
{
    auto add = [this](StandaloneBinding &binding, OutputKind kind,
                      std::size_t index) {
        auto callback = std::make_unique<OutputCallback>(*this, kind, index);
        binding.api.signal->setChangeCallback(callback.get());
        _callbacks.push_back(std::move(callback));
    };
    for (std::size_t i = 0; i < _interruptOutputs.size(); ++i) {
        add(_interruptOutputs[i], OutputKind::Interrupt, i);
    }
    for (std::size_t i = 0; i < _resetOutputs.size(); ++i) {
        add(_resetOutputs[i], OutputKind::Reset, i);
    }
    for (std::size_t i = 0; i < _ioOutputs.size(); ++i) {
        add(_ioOutputs[i], OutputKind::Io, i);
    }
}

void
RtlCoreSimObject::unregisterCallbacks() noexcept
{
    for (StandaloneBinding &binding : _interruptOutputs) {
        binding.api.signal->setChangeCallback(nullptr);
    }
    for (StandaloneBinding &binding : _resetOutputs) {
        binding.api.signal->setChangeCallback(nullptr);
    }
    for (StandaloneBinding &binding : _ioOutputs) {
        binding.api.signal->setChangeCallback(nullptr);
    }
    _callbacks.clear();
}

void
RtlCoreSimObject::driveBoolean(const StandaloneBinding &binding, bool logical)
{
    const bool actual = binding.api.activeLevel == ActiveLevel::High
                            ? logical
                            : !logical;
    std::string error;
    if (!writeSignalU64(*binding.api.signal, actual ? 1 : 0, error)) {
        runtimeFailure("driving standalone input", error.c_str());
    }
}

void
RtlCoreSimObject::driveIo(const StandaloneBinding &binding,
                          const SignalValue &value)
{
    if (value.bitWidth != binding.api.signal->bitWidth() ||
        value.data.size() != signalBytes(*binding.api.signal)) {
        runtimeFailure("generic input width or byte count mismatch",
                       binding.api.signal->name());
    }
    std::string error;
    if (!writeSignal(*binding.api.signal, value.data, error)) {
        runtimeFailure("driving generic input", error.c_str());
    }
}

void
RtlCoreSimObject::interruptInputChanged(std::size_t index, bool asserted)
{
    driveBoolean(_interruptInputs.at(index), asserted);
    wake();
}

void
RtlCoreSimObject::resetInputChanged(std::size_t index, bool asserted)
{
    if (_resetRemaining == Cycles(0)) {
        driveBoolean(_resetInputs.at(index), asserted);
    }
    wake();
}

void
RtlCoreSimObject::ioInputChanged(std::size_t index, const SignalValue &value)
{
    driveIo(_ioInputs.at(index), value);
    wake();
}

void
RtlCoreSimObject::synchronizeInputs()
{
    for (std::size_t i = 0; i < _interruptInputs.size(); ++i) {
        driveBoolean(_interruptInputs[i], _interruptInputPorts[i]->state());
    }
    for (std::size_t i = 0; i < _resetInputs.size(); ++i) {
        driveBoolean(_resetInputs[i], _resetInputPorts[i]->state());
    }
    for (std::size_t i = 0; i < _ioInputs.size(); ++i) {
        if (_ioInputPorts[i]->isConnected()) {
            driveIo(_ioInputs[i], _ioInputPorts[i]->state());
        } else {
            driveIo(_ioInputs[i],
                    {static_cast<std::uint32_t>(
                         _ioInputs[i].api.signal->bitWidth()),
                     _ioInputs[i].initialValue});
        }
    }
}

void
RtlCoreSimObject::updateOutput(OutputKind kind, std::size_t index) noexcept
{
    try {
        StandaloneBinding *binding = nullptr;
        if (kind == OutputKind::Interrupt) {
            binding = &_interruptOutputs.at(index);
        } else if (kind == OutputKind::Reset) {
            binding = &_resetOutputs.at(index);
        } else {
            binding = &_ioOutputs.at(index);
        }
        std::vector<std::uint8_t> bytes;
        std::string error;
        if (!readSignal(*binding->api.signal, bytes, error)) {
            runtimeFailure("sampling standalone output", error.c_str());
        }
        if (kind == OutputKind::Io) {
            if (_ioOutputPorts.at(index)->isConnected()) {
                _ioOutputPorts[index]->set(
                    {static_cast<std::uint32_t>(
                         binding->api.signal->bitWidth()),
                     std::move(bytes)});
            }
            return;
        }
        const bool actual = (bytes.front() & 1) != 0;
        const bool logical = binding->api.activeLevel == ActiveLevel::High
                                 ? actual
                                 : !actual;
        if (kind == OutputKind::Interrupt) {
            if (_interruptOutputPorts.at(index)->isConnected()) {
                _interruptOutputPorts[index]->set(logical);
            }
        } else if (_resetOutputPorts.at(index)->isConnected()) {
            _resetOutputPorts[index]->set(logical);
        }
    } catch (...) {
        panic("%s: exception while updating an RTL output callback", name());
    }
}

void
RtlCoreSimObject::synchronizeOutputs()
{
    for (std::size_t i = 0; i < _interruptOutputs.size(); ++i) {
        updateOutput(OutputKind::Interrupt, i);
    }
    for (std::size_t i = 0; i < _resetOutputs.size(); ++i) {
        updateOutput(OutputKind::Reset, i);
    }
    for (std::size_t i = 0; i < _ioOutputs.size(); ++i) {
        updateOutput(OutputKind::Io, i);
    }
}

void
RtlCoreSimObject::setInitialReset(bool asserted)
{
    for (const StandaloneBinding &binding : _resetInputs) {
        driveBoolean(binding, asserted);
    }
}

void
RtlCoreSimObject::runtimeFailure(const std::string &context,
                                 const char *detail) const
{
    panic("%s: %s%s%s", name(), context, detail && *detail ? ": " : "",
          detail && *detail ? detail : "");
}

void
RtlCoreSimObject::tick()
{
    DPRINTF(RtlCosim, "clocking RTL core\n");
    auto runPhase = [this](auto operation, const char *phase) {
        for (BusRuntime &bus : _initiatorBuses) {
            if (!(bus.transactor.get()->*operation)()) {
                runtimeFailure(phase, bus.transactor->getLastError());
            }
        }
        for (BusRuntime &bus : _targetBuses) {
            if (!(bus.transactor.get()->*operation)()) {
                runtimeFailure(phase, bus.transactor->getLastError());
            }
        }
    };
    runPhase(&BusTransactor::beforeClock, "driving RTL bus inputs");
    if (!_core->settle()) {
        runtimeFailure("settling RTL core", _core->getLastError());
    }
    runPhase(&BusTransactor::afterSettle, "capturing RTL bus handshakes");

    const ClockResult result = _core->clock();
    if (result == ClockResult::Error) {
        runtimeFailure("clocking RTL core", _core->getLastError());
    }
    runPhase(&BusTransactor::afterClock, "committing RTL bus handshakes");
    for (BusRuntime &bus : _initiatorBuses) {
        bus.backend->advance();
    }
    for (BusRuntime &bus : _targetBuses) {
        bus.source->advance();
    }

    if (_resetRemaining > Cycles(0)) {
        --_resetRemaining;
        if (_resetRemaining == Cycles(0)) {
            for (std::size_t i = 0; i < _resetInputs.size(); ++i) {
                driveBoolean(_resetInputs[i], _resetInputPorts[i]->state());
            }
        }
    }
    if (result == ClockResult::Finished) {
        _finished = true;
        DPRINTF(RtlCosim, "vendor RTL model reported finished\n");
        return;
    }
    if (!isQuiescent()) {
        scheduleNextCycle();
    } else {
        DPRINTF(RtlCosim, "RTL core is idle; suppressing clock events\n");
    }
}

bool
RtlCoreSimObject::allTransactorsIdle() const noexcept
{
    auto idle = [](const BusRuntime &bus) {
        const bool adapterIdle = bus.backend ? bus.backend->isIdle()
                                             : bus.source->isIdle();
        return adapterIdle && bus.transactor->isIdle();
    };
    return std::all_of(_initiatorBuses.begin(), _initiatorBuses.end(), idle) &&
           std::all_of(_targetBuses.begin(), _targetBuses.end(), idle);
}

bool
RtlCoreSimObject::isQuiescent() const noexcept
{
    return _started && !_finished && _resetRemaining == Cycles(0) &&
           _core->isIdle() && allTransactorsIdle();
}

void
RtlCoreSimObject::scheduleNextCycle()
{
    if (!_finished && !_tickEvent.scheduled()) {
        schedule(_tickEvent, clockEdge(Cycles(1)));
    }
}

void
RtlCoreSimObject::wake()
{
    if (_started) {
        scheduleNextCycle();
    }
}

bool
RtlCoreSimObject::writeTcmSegment(
    std::uint64_t address, const std::vector<std::uint8_t> &data,
    std::uint64_t memorySize)
{
    for (std::size_t index = 0; index < _core->memoryCount(); ++index) {
        const MemoryRegion *region = _core->memory(index);
        if (!region || address < region->baseAddress ||
            memorySize > region->size ||
            address - region->baseAddress > region->size - memorySize) {
            continue;
        }
        std::uint64_t offset = address - region->baseAddress;
        if (!data.empty() && !_core->writeMemory(
                                 index, offset, data.data(), data.size())) {
            runtimeFailure("writing image to RTL TCM", region->name);
        }
        offset += data.size();
        std::uint64_t remaining = memorySize - data.size();
        const std::vector<std::uint8_t> zeros(64 * 1024, 0);
        while (remaining != 0) {
            const std::size_t chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, zeros.size()));
            if (!_core->writeMemory(index, offset, zeros.data(), chunk)) {
                runtimeFailure("zero-filling image in RTL TCM", region->name);
            }
            offset += chunk;
            remaining -= chunk;
        }
        return true;
    }
    return false;
}

Gem5InitiatorBackend &
RtlCoreSimObject::imageBackend()
{
    fatal_if(_initiatorBuses.empty(),
             "%s: external image requires an RTL initiator bus", name());
    if (_imageBusName.empty()) {
        return *_initiatorBuses.front().backend;
    }
    const auto position = std::find_if(
        _initiatorBuses.begin(), _initiatorBuses.end(),
        [this](const BusRuntime &bus) { return bus.name == _imageBusName; });
    fatal_if(position == _initiatorBuses.end(),
             "%s: image_bus '%s' is not an RTL initiator bus", name(),
             _imageBusName);
    return *position->backend;
}

void
RtlCoreSimObject::loadConfiguredImage()
{
    if (_imagePath.empty()) {
        return;
    }
    std::vector<ImageSegment> segments;
    std::string error;
    fatal_if(!loadImage(_imagePath, _imageFormat, _rawImageAddress, segments,
                        error),
             "%s: cannot load image: %s", name(), error);
    for (const ImageSegment &segment : segments) {
        if (writeTcmSegment(segment.address, segment.data,
                            segment.memorySize)) {
            continue;
        }
        Gem5InitiatorBackend &backend = imageBackend();
        if (!segment.data.empty()) {
            backend.sendFunctional(segment.address, segment.data.data(),
                                   segment.data.size());
        }
        std::uint64_t remaining = segment.memorySize - segment.data.size();
        Addr address = segment.address + segment.data.size();
        const std::vector<std::uint8_t> zeros(64 * 1024, 0);
        while (remaining != 0) {
            const std::size_t chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, zeros.size()));
            backend.sendFunctional(address, zeros.data(), chunk);
            address += chunk;
            remaining -= chunk;
        }
    }
    DPRINTF(RtlCosim, "loaded image %s\n", _imagePath);
}

} // namespace gem5::rtl_cosim
