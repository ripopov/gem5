# Design Prompt: Generic RTL-to-gem5 Integration Framework

Design a generic framework for integrating RTL models compiled to C++ into gem5. The framework must support multiple RTL-to-C++ toolchains, including Verilator and commercial simulators.

## Objectives

The framework must:

- Be independent of any specific RTL-to-C++ tool.
- Hide all signal-to-transaction-level modeling (TLM) transactor implementation details from both RTL vendors and gem5 model developers.
- Allow RTL vendors to distribute their compiled models as shared libraries.
- Require no gem5 knowledge or dependency in vendor-provided code.
- Support automatic discovery and connection of buses, interrupts, GPIOs, reset signals, and other interfaces.
- Minimize simulation overhead, particularly when an RTL model is idle.

## RTL Vendor Interface

From the RTL vendor's perspective, the framework must provide a standard, versioned C++ interface. The vendor implements this interface and packages the compiled RTL model as a shared library.

The vendor-facing API must:

- Use standard C++ and have no gem5 dependencies.
- Be independent of the RTL compilation tool.
- Define clear ownership and lifetime rules.
- Include error handling, API version negotiation, and capability discovery.
- Address ABI compatibility across compilers, standard library versions, and operating systems.

The shared library must expose a well-defined entry point that returns the root `RtlCoreManager` object.

## gem5 Integration

From the gem5 model developer's perspective, an RTL model must appear as a conventional `SimObject`.

The `SimObject` should accept:

- A path to the RTL shared library.
- A JSON configuration file or JSON configuration object.
- Optional clock, reset, tracing, and model-specific parameters.

At runtime, the framework must:

1. Load the shared library.
2. Validate API and ABI compatibility.
3. Create the requested RTL model instance.
4. Discover all exported signals and interfaces.
5. Classify signal groups as known protocols such as AXI, ACE, APB, interrupts, GPIO, clock, or reset.
6. Instantiate the appropriate signal-to-TLM transactors.
7. Expose corresponding gem5 interfaces, such as:
   - `RequestPort`
   - `ResponsePort`
   - `IntSourcePin`
   - `IntSinkPin`
   - Reset-related ports
   - GPIO or other protocol-specific ports
8. Connect model events to gem5's event-driven simulation infrastructure.

The framework should contain all protocol-specific transactors. Neither the vendor library nor the gem5 model developer should need to implement or understand pin-level handshaking.

## Proposed Vendor-Facing API

The API should use abstract interfaces based on virtual methods or propose a safer ABI-compatible alternative where appropriate.

### `RtlCoreManager`

```cpp
#include <cstdint>

class RtlCore;

class RtlCoreManager
{
  public:
    virtual std::uint32_t apiVersion() const = 0;

    // Creates one RTL instance from a null-terminated JSON configuration.
    // Returns nullptr on failure.
    virtual RtlCore* createCore(const char* configJson) = 0;

    // Destroys a core created by this manager.
    virtual void destroyCore(RtlCore* core) = 0;

    // Returns a description of the most recent error. The returned string is
    // owned by the implementation.
    virtual const char* getLastError() const = 0;

  protected:
    virtual ~RtlCoreManager() = default;
};
```

`RtlCoreManager` is the root object returned by the shared library. For the proof of concept, one shared library represents one RTL model type. The manager only needs to report its API version, create and destroy `RtlCore` instances, and report construction errors. Model enumeration, capability discovery, and structured diagnostics can be added after the SCR1 integration is working.

The shared library must export two C entry points to avoid C++ symbol-name mangling:

```cpp
extern "C" RtlCoreManager*
createRtlCoreManager();

extern "C" void
destroyRtlCoreManager(RtlCoreManager* manager);
```

Core and manager objects must be destroyed by the shared library that created them. For the proof of concept, gem5 and the RTL adapter library may be required to use compatible C++ compiler ABIs.

### `RtlCore`

```cpp
class RtlCore
{
  public:
    virtual ~RtlCore() = default;

    // Interface discovery, execution, and backdoor access.
};
```

`RtlCore` represents one instantiated RTL core or subsystem. It must provide APIs to:

1. Discover all exported buses and individual signals.
2. Advance the model by one clock cycle or clock phase.
3. Determine whether the model is idle.
4. Read and write tightly coupled memories through optional backdoor access.
5. Initialize code or data memories from ELF, COFF, or raw binary images.
6. Read and write architectural or implementation-specific registers through optional backdoor access.
7. Control reset and other lifecycle operations.
8. Enable optional tracing and diagnostic facilities.
9. Report supported capabilities so optional APIs can be used safely.

The `isIdle()` method is an important performance optimization. It should report when the model does not need to be evaluated—for example, while halted waiting for an interrupt, held in reset, blocked by an external wait condition, or fully clock-gated. The design must explain how the model is reactivated when an external input changes.

### `Bus`

```cpp
class Bus
{
  public:
    virtual ~Bus() = default;

    // Bus metadata and constituent signal discovery.
};
```

`Bus` represents a logical group of signals implementing a protocol such as:

- AXI3 or AXI4
- ACE
- AHB or AHB-Lite
- APB
- A custom vendor-defined protocol

It must expose:

- Bus name and instance identifier.
- Protocol type and version.
- Initiator or target role.
- Protocol parameters such as address, data, and ID widths.
- Constituent signals.
- Optional protocol-specific metadata.
- Clock and reset domain associations.

### `Signal`

```cpp
class Signal
{
  public:
    virtual ~Signal() = default;

    virtual const char* name() const = 0;
    virtual std::size_t bitWidth() const = 0;
    virtual SignalDirection direction() const = 0;

    virtual void getValue(
        unsigned char* data,
        std::size_t dataSize) const = 0;

    virtual void setValue(
        const unsigned char* data,
        std::size_t dataSize) = 0;

    virtual void registerSignalChangeCallback(
        SignalChangeCallback* callback) = 0;
};
```

`Signal` represents a single scalar or vector RTL signal. Its specification must define:

- Direction semantics.
- Byte order and bit ordering.
- Representation of values wider than 64 bits.
- Handling of two-state versus four-state logic.
- Input validation and error reporting.
- Callback ownership, registration, and removal.
- Callback ordering and thread-safety requirements.
- Signal lifetime guarantees.

## Required Design Output

Provide:

1. A high-level architecture and component diagram.
2. A recommended source directory structure within gem5.
3. The complete vendor-facing API proposal.
4. Shared-library entry-point and ABI-versioning conventions.
5. JSON configuration schema and representative examples.
6. Interface-discovery and protocol-classification mechanisms.
7. gem5 `SimObject`, port, event, clock, and reset integration.
8. Signal-to-TLM transactor architecture.
9. Idle detection and wake-up behavior.
10. Support for multiple clocks and reset domains.
11. Checkpointing, serialization, and restore behavior.
12. Debugging, tracing, and waveform-generation support.
13. Error handling and diagnostics.
14. Thread-safety and reentrancy requirements.
15. A phased implementation plan beginning with a Verilator-based proof of concept.
16. Example integration using the SCR1 RISC-V core with both gem5's classic memory system and Ruby.
