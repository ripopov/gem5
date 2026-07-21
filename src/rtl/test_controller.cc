/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/test_controller.hh"

#include <iomanip>
#include <sstream>
#include <utility>

#include "base/logging.hh"
#include "rtl/rtl_core.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5::rtl_cosim
{

namespace
{

RtlCosimTestController::Mode
parseMode(const std::string &mode)
{
    if (mode == "idle") {
        return RtlCosimTestController::Mode::Idle;
    }
    if (mode == "interrupt") {
        return RtlCosimTestController::Mode::Interrupt;
    }
    if (mode == "reset") {
        return RtlCosimTestController::Mode::Reset;
    }
    fatal("RtlCosimTestController mode must be idle, interrupt, or reset");
}

} // anonymous namespace

RtlCosimTestController::RtlCosimTestController(const Params &params)
    : ClockedObject(params), _cores(params.cores), _system(params.system),
      _signatureAddress(params.signature_address),
      _expectedSignature(params.expected_signature),
      _tickEvent([this] { tick(); }, name() + ".tick"),
      _mode(parseMode(params.mode)), _pollInterval(params.poll_interval),
      _pulseRemaining(params.pulse_cycles),
      _pulseCycles(params.pulse_cycles),
      _timeoutCycles(params.timeout_cycles)
{
    fatal_if(_cores.empty(), "%s: cores must not be empty", name());
    fatal_if(!_system, "%s: system must not be null", name());
    fatal_if(_pollInterval == Cycles(0),
             "%s: poll_interval must be positive", name());
    fatal_if(_timeoutCycles == Cycles(0),
             "%s: timeout_cycles must be positive", name());
    fatal_if(_mode != Mode::Idle && _pulseCycles == Cycles(0),
             "%s: pulse_cycles must be positive", name());

    for (unsigned index = 0;
         index < params.port_interrupt_outputs_connection_count; ++index) {
        _interruptPorts.push_back(std::make_unique<IntSourcePinBase>(
            name() + ".interrupt_outputs[" + std::to_string(index) + "]",
            static_cast<PortID>(index)));
    }
    for (unsigned index = 0;
         index < params.port_reset_outputs_connection_count; ++index) {
        _resetPorts.push_back(std::make_unique<SignalSourcePort<bool>>(
            name() + ".reset_outputs[" + std::to_string(index) + "]",
            static_cast<PortID>(index)));
    }
}

Port &
RtlCosimTestController::getPort(const std::string &ifName, PortID index)
{
    fatal_if(index < 0, "%s: vector port '%s' requires an index", name(),
             ifName);
    const auto position = static_cast<std::size_t>(index);
    if (ifName == "interrupt_outputs" && position < _interruptPorts.size()) {
        return *_interruptPorts[position];
    }
    if (ifName == "reset_outputs" && position < _resetPorts.size()) {
        return *_resetPorts[position];
    }
    return ClockedObject::getPort(ifName, index);
}

void
RtlCosimTestController::init()
{
    ClockedObject::init();
    fatal_if(_mode == Mode::Interrupt && _interruptPorts.empty(),
             "%s: interrupt mode requires an interrupt output", name());
    fatal_if(_mode == Mode::Reset && _resetPorts.empty(),
             "%s: reset mode requires a reset output", name());
}

void
RtlCosimTestController::startup()
{
    ClockedObject::startup();
    setPulse(false);
    schedule(_tickEvent, clockEdge(_pollInterval));
}

bool
RtlCosimTestController::allCoresIdle() const noexcept
{
    for (const RtlCoreSimObject *core : _cores) {
        if (!core || !core->isQuiescent()) {
            return false;
        }
    }
    return true;
}

bool
RtlCosimTestController::signatureMatches()
{
    if (_expectedSignature.empty()) {
        return true;
    }

    std::vector<std::uint8_t> actual(_expectedSignature.size());
    _system->physProxy.readBlob(
        _signatureAddress, actual.data(), actual.size());
    if (actual == _expectedSignature) {
        return true;
    }

    auto format = [](const std::vector<std::uint8_t> &bytes) {
        std::ostringstream stream;
        stream << std::hex << std::setfill('0');
        for (const std::uint8_t byte : bytes) {
            stream << std::setw(2) << static_cast<unsigned>(byte);
        }
        return stream.str();
    };
    warn("%s: signature mismatch at %#llx: expected %s, observed %s",
         name(), static_cast<unsigned long long>(_signatureAddress),
         format(_expectedSignature), format(actual));
    return false;
}

void
RtlCosimTestController::setPulse(bool asserted)
{
    if (_mode == Mode::Interrupt) {
        for (auto &port : _interruptPorts) {
            port->set(asserted);
        }
    } else if (_mode == Mode::Reset) {
        for (auto &port : _resetPorts) {
            port->set(asserted);
        }
    }
}

void
RtlCosimTestController::finish(const std::string &cause, int status)
{
    _phase = Phase::Complete;
    exitSimLoop(cause, status);
}

void
RtlCosimTestController::tick()
{
    _elapsed += _pollInterval;
    if (_elapsed >= _timeoutCycles) {
        finish("rtl-cosim validation timed out", 1);
        return;
    }

    switch (_phase) {
      case Phase::WaitInitialIdle:
        if (!allCoresIdle()) {
            break;
        }
        if (_mode == Mode::Idle) {
            if (signatureMatches()) {
                finish("rtl-cosim validation passed", 0);
            } else {
                finish("rtl-cosim signature mismatch", 1);
            }
            return;
        }
        setPulse(true);
        _pulseRemaining = _pulseCycles;
        _phase = Phase::PulseAsserted;
        break;

      case Phase::PulseAsserted:
        if (_pulseRemaining > _pollInterval) {
            _pulseRemaining = _pulseRemaining - _pollInterval;
        } else {
            _pulseRemaining = Cycles(0);
            setPulse(false);
            _phase = Phase::WaitFinalIdle;
        }
        break;

      case Phase::WaitFinalIdle:
        if (allCoresIdle()) {
            if (signatureMatches()) {
                finish("rtl-cosim validation passed", 0);
            } else {
                finish("rtl-cosim signature mismatch", 1);
            }
            return;
        }
        break;

      case Phase::Complete:
        return;
    }

    schedule(_tickEvent, clockEdge(_pollInterval));
}

} // namespace gem5::rtl_cosim
