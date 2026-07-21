# Design Prompt: Generic RTL-to-gem5 Integration Framework

Design a generic framework for integrating RTL models compiled to C++ into gem5. The framework must support multiple RTL-to-C++ toolchains,
including Verilator and commercial simulators.

## Objectives

The framework must:

- Be independent of any specific RTL-to-C++ tool.
- Hide all signal-to-transaction-level modeling (TLM) transactor implementation details from both RTL vendors and gem5 model developers.
- Allow RTL vendors to distribute their compiled models as shared libraries.
- Require no gem5 knowledge or dependency in vendor-provided code.
- Support automatic discovery and connection of buses, interrupts, GPIOs, reset signals, and other interfaces.
- Minimize simulation overhead, particularly when an RTL model is idle.

## RTL Vendor Interface

From the RTL vendor's perspective, the framework must provide a standard, versioned C++ interface. The vendor implements this interface and
packages the compiled RTL model as a shared library.

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

The framework should contain all protocol-specific transactors. Neither the vendor library nor the gem5 model developer should need to implement
or understand pin-level handshaking.

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

`RtlCoreManager` is the root object returned by the shared library. For the proof of concept, one shared library represents one RTL model type. The
manager only needs to report its API version, create and destroy `RtlCore` instances, and report construction errors. Model enumeration, capability
discovery, and structured diagnostics can be added after the SCR1 integration is working.

The shared library must export two C entry points to avoid C++ symbol-name mangling:

```cpp
extern "C" RtlCoreManager*
createRtlCoreManager();

extern "C" void
destroyRtlCoreManager(RtlCoreManager* manager);
```

Core and manager objects must be destroyed by the shared library that created them. For the proof of concept, gem5 and the RTL adapter library may
be required to use compatible C++ compiler ABIs.

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

The `isIdle()` method is an important performance optimization. It should report when the model does not need to be evaluated—for example, while
halted waiting for an interrupt, held in reset, blocked by an external wait condition, or fully clock-gated. The design must explain how the model
is reactivated when an external input changes.

### `Bus`

```cpp
#include <cstddef>
#include <cstdint>

class Signal;

enum class BusProtocol : std::uint32_t
{
    Unknown = 0,
    AhbLite,
    Axi4,
    Apb
};

enum class BusRole : std::uint32_t
{
    Initiator,
    Target
};

using SignalRoleId = std::uint32_t;

namespace AhbLiteSignal
{
constexpr SignalRoleId HAddr  = 1;
constexpr SignalRoleId HBurst = 2;
constexpr SignalRoleId HProt  = 3;
constexpr SignalRoleId HSize  = 4;
constexpr SignalRoleId HTrans = 5;
constexpr SignalRoleId HWData = 6;
constexpr SignalRoleId HWrite = 7;
constexpr SignalRoleId HRData = 8;
constexpr SignalRoleId HReady = 9;
constexpr SignalRoleId HResp  = 10;
}

struct SignalBinding
{
    SignalRoleId role;
    Signal* signal;
};

class Bus
{
  public:
    // Bus instance name, for example "instruction" or "data".
    virtual const char* name() const = 0;

    virtual BusProtocol protocol() const = 0;

    // Role of the RTL module on this bus.
    virtual BusRole role() const = 0;

    virtual std::size_t signalCount() const = 0;

    // Enumerates the semantic role and Signal object for each binding.
    // Returns {0, nullptr} when index is out of range.
    virtual SignalBinding signal(std::size_t index) = 0;

  protected:
    virtual ~Bus() = default;
};
```

`Bus` represents a logical group of signals implementing a protocol such as AXI4, AHB-Lite, or APB. Signal bindings must use canonical semantic role
IDs defined by the framework; the framework must never infer protocol semantics from vendor-specific RTL signal names. `Signal::name()` remains
available for diagnostics and waveform tracing.

The vendor adapter maps its physical RTL signals to these semantic roles. `RtlCore` owns all returned `Bus` and `Signal` objects, and their names and
pointers remain valid for the lifetime of the core.

The framework must define a validation profile for every supported protocol and role. Each profile specifies:

- Required and optional semantic signal roles.
- Expected signal directions from the RTL model's perspective.
- Fixed signal widths.
- Width relationships between signals.

Before constructing a transactor, the framework must enumerate and validate all bindings, reject null or duplicate bindings, check required signals,
and verify their directions and widths. Automatic discovery means consuming this structured metadata rather than guessing interfaces from signal
names. For the initial single-clock proof of concept, clock and reset remain core-level signals.

### `Signal`

```cpp
enum class SignalDirection : std::uint32_t
{
    Input,
    Output
};

class SignalChangeCallback
{
  public:
    virtual ~SignalChangeCallback() = default;

    virtual void update() = 0;
};

class Signal
{
  public:
    virtual const char* name() const = 0;
    virtual std::size_t bitWidth() const = 0;
    virtual SignalDirection direction() const = 0;

    virtual bool getValue(
        std::uint8_t* data,
        std::size_t dataSize) const = 0;

    virtual bool setValue(
        const std::uint8_t* data,
        std::size_t dataSize) = 0;

    // Installs or replaces the callback. Signal does not own the callback.
    // Passing nullptr unregisters the current callback.
    virtual void setChangeCallback(
        SignalChangeCallback* callback) = 0;

  protected:
    virtual ~Signal() = default;
};
```

`Signal` represents a single scalar or vector RTL signal. Directions are defined from the RTL model's perspective. `getValue()` may read any signal,
while `setValue()` must fail for output signals. For the proof of concept, values use two-state logic and little-endian byte order: RTL bit 0 is bit 0
of `data[0]`, and unused high bits in the final byte are zero. The buffer size must be at least `(bitWidth() + 7) / 8` bytes.

The signal-change callback provides event-driven notification for signals such as GPIO and interrupt outputs without polling. Its contract is:

- Each signal has at most one registered callback.
- Registering another callback replaces the previous callback.
- Passing `nullptr` unregisters the callback.
- The framework owns the callback, which must remain alive while registered.
- `update()` takes no arguments because each callback instance is associated with one specific signal.
- `update()` runs synchronously on the RTL evaluation thread after the new value is available.
- A signal generates at most one callback per `RtlCore::clock()` call after its output has stabilized.
- Installing a callback does not generate an initial notification; the framework samples the initial value explicitly.
- Input changes made through `setValue()` do not generate callbacks.
- No callback may occur after it is unregistered or after core destruction begins.
- `update()` must not throw, re-enter the RTL model, or destroy the core. It should schedule deferred gem5 processing.

`RtlCore` owns all `Signal` objects. Signal names and pointers remain valid for the lifetime of the core.

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
