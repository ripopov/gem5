/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_RTL_CORE_HH__
#define __RTL_RTL_CORE_HH__

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dev/intpin.hh"
#include "params/RtlCoreSimObject.hh"
#include "rtl/gem5_backend.hh"
#include "rtl/runtime/model_loader.hh"
#include "rtl/runtime/model_validator.hh"
#include "rtl/runtime/transaction.hh"
#include "rtl/signal_value.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/signal.hh"

namespace gem5::rtl_cosim
{

class RtlCoreSimObject final : public ClockedObject
{
  private:
    enum class OutputKind
    {
        Interrupt,
        Reset,
        Io
    };

    class OutputCallback final : public SignalChangeCallback
    {
      public:
        OutputCallback(RtlCoreSimObject &owner, OutputKind kind,
                       std::size_t index)
            : owner(owner), kind(kind), index(index)
        {}

        void update() noexcept override;

      private:
        RtlCoreSimObject &owner;
        OutputKind kind;
        std::size_t index;
    };

    struct StandaloneBinding
    {
        CoreSignalBinding api{};
        std::vector<std::uint8_t> initialValue;
    };

    struct BusRuntime
    {
        std::string name;
        std::unique_ptr<Gem5InitiatorBackend> backend;
        std::unique_ptr<Gem5TargetSource> source;
        std::unique_ptr<BusTransactor> transactor;
    };

  public:
    PARAMS(RtlCoreSimObject);
    explicit RtlCoreSimObject(const Params &params);
    ~RtlCoreSimObject() override;

    Port &getPort(const std::string &ifName,
                  PortID index = InvalidPortID) override;
    void init() override;
    void startup() override;

    /** Schedule a cycle after asynchronous packet or pin activity. */
    void wake();
    bool isQuiescent() const noexcept;

    /** CPU-switch support used by the composing RtlCpuSimObject. */
    Port &defaultInitiatorPort();
    RtlCpuState *cpuStateCapability() const noexcept;
    bool deferredForCpuSwitch() const noexcept { return _deferStartup; }
    bool prepareCpuStateImport(std::string &error);
    bool importCpuState(std::size_t context,
                        const std::vector<CpuStateValue> &values,
                        std::string &error);
    void setRequestContextId(ContextID contextId);
    void activateAfterCpuStateImport();
    bool hasInterruptInput(const std::string &signalName) const noexcept;
    void driveCpuInterrupt(const std::string &signalName, bool asserted);

  private:
    void loadModel(const Params &params);
    void buildBusMappings(const Params &params);
    void buildSignalMappings(const Params &params);
    void registerCallbacks();
    void unregisterCallbacks() noexcept;

    void tick();
    void scheduleNextCycle();
    bool allTransactorsIdle() const noexcept;
    void runtimeFailure(const std::string &context,
                        const char *detail = nullptr) const;

    void driveBoolean(const StandaloneBinding &binding, bool logical);
    void driveIo(const StandaloneBinding &binding,
                 const SignalValue &value);
    void interruptInputChanged(std::size_t index, bool asserted);
    void resetInputChanged(std::size_t index, bool asserted);
    void ioInputChanged(std::size_t index, const SignalValue &value);
    void updateOutput(OutputKind kind, std::size_t index) noexcept;
    void synchronizeInputs();
    void synchronizeOutputs();
    void setInitialReset(bool asserted);

    void loadConfiguredImage();
    bool writeTcmSegment(std::uint64_t address,
                         const std::vector<std::uint8_t> &data,
                         std::uint64_t memorySize);
    Gem5InitiatorBackend &imageBackend();

    ModelLoader _loader;
    ValidationResult _validation;
    RtlCore *_core = nullptr;

    std::vector<BusRuntime> _initiatorBuses;
    std::vector<BusRuntime> _targetBuses;
    std::vector<StandaloneBinding> _interruptInputs;
    std::vector<StandaloneBinding> _interruptOutputs;
    std::vector<StandaloneBinding> _resetInputs;
    std::vector<StandaloneBinding> _resetOutputs;
    std::vector<StandaloneBinding> _ioInputs;
    std::vector<StandaloneBinding> _ioOutputs;

    std::vector<std::unique_ptr<SignalSinkPort<bool>>> _interruptInputPorts;
    std::vector<std::unique_ptr<IntSourcePinBase>> _interruptOutputPorts;
    std::vector<std::unique_ptr<SignalSinkPort<bool>>> _resetInputPorts;
    std::vector<std::unique_ptr<SignalSourcePort<bool>>> _resetOutputPorts;
    std::vector<std::unique_ptr<SignalSinkPort<SignalValue>>> _ioInputPorts;
    std::vector<std::unique_ptr<SignalSourcePort<SignalValue>>> _ioOutputPorts;
    std::vector<std::unique_ptr<OutputCallback>> _callbacks;

    EventFunctionWrapper _tickEvent;
    Cycles _resetRemaining;
    std::size_t _maxPending;
    std::string _imagePath;
    std::string _imageFormat;
    Addr _rawImageAddress;
    std::string _imageBusName;
    bool _started = false;
    bool _finished = false;
    bool _deferStartup = false;
    bool _cpuImportPrepared = false;
    std::vector<bool> _cpuContextsImported;
    bool _cpuActivated = false;
};

} // namespace gem5::rtl_cosim

#endif // __RTL_RTL_CORE_HH__
