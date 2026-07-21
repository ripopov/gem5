/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_TEST_CONTROLLER_HH__
#define __RTL_TEST_CONTROLLER_HH__

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dev/intpin.hh"
#include "params/RtlCosimTestController.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/signal.hh"

namespace gem5
{

class System;

namespace rtl_cosim
{

class RtlCoreSimObject;

/** Test-only controller for quiescence, interrupt-wake, and reset tests. */
class RtlCosimTestController final : public ClockedObject
{
  public:
    enum class Mode
    {
        Idle,
        Interrupt,
        Reset
    };

    enum class Phase
    {
        WaitInitialIdle,
        PulseAsserted,
        WaitFinalIdle,
        Complete
    };

    PARAMS(RtlCosimTestController);
    explicit RtlCosimTestController(const Params &params);

    Port &getPort(const std::string &ifName,
                  PortID index = InvalidPortID) override;
    void init() override;
    void startup() override;

  private:
    void tick();
    bool allCoresIdle() const noexcept;
    bool signatureMatches();
    void setPulse(bool asserted);
    void finish(const std::string &cause, int status);

    std::vector<RtlCoreSimObject *> _cores;
    System *_system;
    Addr _signatureAddress;
    std::vector<std::uint8_t> _expectedSignature;
    std::vector<std::unique_ptr<IntSourcePinBase>> _interruptPorts;
    std::vector<std::unique_ptr<SignalSourcePort<bool>>> _resetPorts;
    EventFunctionWrapper _tickEvent;
    Mode _mode;
    Phase _phase = Phase::WaitInitialIdle;
    Cycles _pollInterval;
    Cycles _pulseRemaining;
    Cycles _pulseCycles;
    Cycles _timeoutCycles;
    Cycles _elapsed = Cycles(0);
};

} // namespace rtl_cosim
} // namespace gem5

#endif // __RTL_TEST_CONTROLLER_HH__
