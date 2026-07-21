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
- Support APB, AXI4, AXI3, and AXI3-ACE bus protocols.
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

From the gem5 model developer's perspective, an RTL model appears as a `ClockedObject`-derived SimObject. The SimObject accepts:

- A path to the vendor shared library.
- A JSON configuration file or JSON configuration object.
- A gem5 clock domain.
- Optional tracing and model-specific parameters.

Python-visible SimObject ports cannot be created after C++ runtime initialization. The RTL SimObject therefore declares a fixed set of vector-port
families, while the number of connected elements is determined by the Python configuration. Standalone vector I/O uses a gem5-internal value type:

```cpp
namespace gem5::rtl_cosim
{

struct SignalValue
{
    std::uint32_t bitWidth;
    std::vector<std::uint8_t> data;

    bool operator==(const SignalValue& other) const noexcept
    {
        return bitWidth == other.bitWidth && data == other.data;
    }
};

} // namespace gem5::rtl_cosim
```

`SignalValue::data` uses the same little-endian byte and bit ordering as the vendor-facing `Signal` API. It exists only inside gem5 and does not
cross the shared-library boundary. The corresponding C++ port elements use `SignalSinkPort<SignalValue>` and `SignalSourcePort<SignalValue>`.

```python
RtlSignalSinkPort = VectorSignalSinkPort("gem5::rtl_cosim::SignalValue")
RtlSignalSourcePort = VectorSignalSourcePort("gem5::rtl_cosim::SignalValue")

class RtlCoreSimObject(ClockedObject):
    initiator_ports = VectorRequestPort("RTL initiator buses")
    target_ports = VectorResponsePort("RTL target buses")

    interrupt_inputs = VectorIntSinkPin("Interrupts driven into RTL")
    interrupt_outputs = VectorIntSourcePin("Interrupts driven by RTL")

    reset_inputs = VectorResetResponsePort("Reset inputs to RTL")
    reset_outputs = VectorResetRequestPort("Reset requests from RTL")

    io_inputs = RtlSignalSinkPort("Standalone scalar and vector inputs to RTL")
    io_outputs = RtlSignalSourcePort("Standalone scalar and vector outputs from RTL")
```

Discovered RTL interfaces map to these port families as follows:

| RTL interface | gem5 SimObject port |
| --- | --- |
| `BusRole::Initiator` | `VectorRequestPort` |
| `BusRole::Target` | `VectorResponsePort` |
| Interrupt input to RTL | `VectorIntSinkPin` |
| Interrupt output from RTL | `VectorIntSourcePin` |
| Reset input to RTL | `VectorResetResponsePort` |
| Reset request from RTL | `VectorResetRequestPort` |
| Standalone scalar or vector input to RTL | `VectorSignalSinkPort("gem5::rtl_cosim::SignalValue")` |
| Standalone scalar or vector output from RTL | `VectorSignalSourcePort("gem5::rtl_cosim::SignalValue")` |

The JSON configuration identifies discovered interfaces by their API names and maps each interface to a vector index. The Python configuration
connects those vector elements before C++ object construction. After loading the vendor library, the framework validates that every configured
interface exists, each discovered interface maps to exactly one compatible vector element, and there are no missing or duplicate bindings.

Runtime initialization proceeds as follows:

1. Resolve the configured shared-library path and load it.
2. Look up the versioned `createRtlCoreManagerV1` and `destroyRtlCoreManagerV1` symbols.
3. Create `RtlCoreManager` and `RtlCore` using the JSON model configuration.
4. Enumerate buses, standalone signals, and backdoor-accessible memories.
5. Validate bus protocol profiles, signal roles, directions, widths, active levels, names, and configured vector indices.
6. Create the C++ transactor behind each connected vector-port element.
7. Register callbacks on subscribed RTL output signals, including interrupt, reset-request, and generic I/O outputs.
8. Sample initial output values and synchronize the corresponding gem5 signal ports.
9. Schedule the first clock event unless the model reports that it is idle.

An RTL initiator transactor presents a gem5 `RequestPort` and converts pin-level requests into gem5 packets. It handles timing requests, responses,
backpressure, and retry callbacks. An RTL target transactor presents a `ResponsePort` and performs the inverse conversion. Protocol-specific state
machines for APB, AXI4, AXI3, and AXI3-ACE remain entirely inside the framework.

The same `RequestPort` connects to either memory-system implementation:

- With the classic memory system, connect it to an XBar response-side port such as `cpu_side_ports`.
- With Ruby, connect it to `RubySequencer.in_ports`, which is a `VectorResponsePort`.

No Ruby-specific vendor interface or RTL SimObject port is required. AXI3-ACE requires snoop handling in the gem5 transactor and support from the
selected memory system, but it still uses gem5 `RequestPort` and `ResponsePort` types.

Interrupt and reset gem5 ports carry logical assertion state. The framework uses `CoreSignalBinding::activeLevel` to convert between that logical
state and the actual, non-normalized RTL signal value. Generic I/O values are transferred without polarity conversion. Each I/O vector-port element
represents one standalone RTL `Signal` and may carry any positive bit width supported by the vendor API. Initialization fails if connected endpoints
or the configured metadata disagree with `Signal::bitWidth()`.

Reset-vector addresses, hart IDs, fuse values, clock enables, debug controls, and GPIO are generic I/O rather than reset ports. For example, a 32-bit
reset-vector input is exposed through one `io_inputs` element carrying a 32-bit `SignalValue`; `reset_inputs` remains reserved for reset assertion.

Clocking, TCM backdoor access, and idle state do not require SimObject ports. The gem5 clock domain schedules `RtlCore::clock()`, object-file loading
uses the memory backdoor API directly, and `RtlCore::isIdle()` controls whether future clock events are scheduled.

The SCR1 PoC uses `scr1_top_axi` and requires two `VectorRequestPort` elements for its instruction and data AXI4 initiator buses, interrupt sink
elements, and reset response elements. A reset-request element is required only when the optional SCR1 system-reset output is exposed. Target-bus,
interrupt-source, and generic I/O port families remain part of the framework contract but are not required for the first SCR1 execution milestone.

## Vendor-Facing PoC API

The PoC API uses abstract interfaces based on virtual methods.

No exception may cross the shared-library boundary. Every vendor-facing virtual method and exported entry point must be `noexcept`; failures are
reported through return values and `getLastError()`.

For the proof of concept, all manager, core, bus, signal, memory, and callback operations occur on the gem5 simulation thread. The vendor
implementation is not required to be thread-safe.

The complete vendor-facing PoC API is defined in the standalone header `include/gem5/rtl_cosim/api_v1.hh`. This file is the single source of truth
for the V1 interface and contains no gem5-specific types or dependencies, allowing RTL vendors to copy it directly into their own codebases.

### `RtlCoreManager`

```cpp
#include <cstdint>

class RtlCore;

class RtlCoreManager
{
  public:
    // Creates one RTL instance from a null-terminated JSON configuration.
    // Returns nullptr on failure.
    virtual RtlCore* createCore(const char* configJson) noexcept = 0;

    // Destroys a core created by this manager.
    virtual void destroyCore(RtlCore* core) noexcept = 0;

    // Returns a description of the most recent error. The returned string is
    // owned by the implementation.
    virtual const char* getLastError() const noexcept = 0;

  protected:
    virtual ~RtlCoreManager() noexcept = default;
};
```

`RtlCoreManager` is the root object returned by the shared library. For the proof of concept, one shared library represents one RTL model type. The
manager only needs to create and destroy `RtlCore` instances and report construction errors. Model enumeration, capability discovery, and structured
diagnostics can be added after the SCR1 integration is working.

`RtlCoreManager::getLastError()` returns the error from the most recent failed manager operation. The returned string is owned by the implementation
and remains valid until the next manager API call. It returns `nullptr` or an empty string when no error is available.

The shared library must export two C entry points to avoid C++ symbol-name mangling:

```cpp
inline constexpr std::uint32_t RtlCosimApiVersion = 1;

extern "C" RtlCoreManager*
createRtlCoreManagerV1() noexcept;

extern "C" void
destroyRtlCoreManagerV1(RtlCoreManager* manager) noexcept;
```

gem5 negotiates the API version by looking up the versioned factory symbol before creating or calling a C++ object. Successfully locating
`createRtlCoreManagerV1` identifies the V1 interface and vtable layout; no manager-level version method is needed.

The suffix is the major API and ABI version. Every V1 virtual interface is immutable: methods must not be added, removed, reordered, or have their
signatures changed. Any such change introduces V2 factory and destructor symbols with corresponding V2 interfaces. A library may export multiple
major versions during migration, while compatible implementation changes continue to use V1.

Core and manager objects must be destroyed by the shared library that created them. For the proof of concept, gem5 and the RTL adapter library may
be required to use compatible C++ compiler ABIs.

### `RtlCore`

```cpp
class Bus;
class Signal;

enum class CoreSignalRole : std::uint32_t
{
    Reset,
    Interrupt,
    Io
};

enum class ActiveLevel : std::uint32_t
{
    Low,
    High
};

struct CoreSignalBinding
{
    CoreSignalRole role;

    // Index within a signal group, such as interrupt 0 or I/O 3.
    std::uint32_t index;

    // Assertion level for reset and interrupt signals. Ignored for I/O.
    ActiveLevel activeLevel;

    // Exposes the actual, non-normalized RTL signal value.
    Signal* signal;
};

enum class MemoryRole : std::uint32_t
{
    CodeTcm,
    DataTcm
};

struct MemoryRegion
{
    const char* name;
    MemoryRole role;

    // Architectural base address and size in bytes.
    std::uint64_t baseAddress;
    std::uint64_t size;
};

enum class ClockResult : std::uint32_t
{
    Completed,
    Finished,
    Error
};

class RtlCore
{
  public:
    // Instance name used for diagnostics and waveform hierarchy.
    virtual const char* name() const noexcept = 0;

    virtual std::size_t busCount() const noexcept = 0;

    // Returns nullptr if index is out of range.
    virtual Bus* bus(std::size_t index) noexcept = 0;

    // Enumerates standalone reset, interrupt, and generic I/O signals.
    virtual std::size_t signalCount() const noexcept = 0;

    // Returns a binding with signal == nullptr if index is out of range.
    virtual CoreSignalBinding signal(std::size_t index) noexcept = 0;

    // Enumerates memories that support backdoor access.
    virtual std::size_t memoryCount() const noexcept = 0;

    // Returns nullptr if index is out of range.
    virtual const MemoryRegion* memory(
        std::size_t index) const noexcept = 0;

    // Accesses a memory using a byte offset relative to its base address.
    virtual bool readMemory(
        std::size_t memoryIndex,
        std::uint64_t offset,
        std::uint8_t* data,
        std::size_t dataSize) const noexcept = 0;

    virtual bool writeMemory(
        std::size_t memoryIndex,
        std::uint64_t offset,
        const std::uint8_t* data,
        std::size_t dataSize) noexcept = 0;

    // Advances the RTL model by one complete clock cycle.
    virtual ClockResult clock() noexcept = 0;

    // True when gem5 may stop clock events until an input changes.
    virtual bool isIdle() const noexcept = 0;

    // Describes the most recent failed operation or ClockResult::Error.
    virtual const char* getLastError() const noexcept = 0;

  protected:
    virtual ~RtlCore() noexcept = default;
};
```

`RtlCore` represents one instantiated RTL core or subsystem. Enumeration indices are contiguous from zero through `count - 1`, and discovery results
must remain stable for the core's lifetime. The core owns all returned `Bus`, `Signal`, and `MemoryRegion` objects. Their pointers and names remain
valid until `RtlCoreManager::destroyCore()` is called, and the framework must unregister all signal callbacks before destroying the core.

`RtlCore::getLastError()` reports the most recent failed core, memory, or child `Signal` operation. Its returned string is owned by the implementation
and remains valid until the next API call on the core or any of its child objects. It returns `nullptr` or an empty string when no error is available.

`CoreSignalBinding` supplies the semantic role, group index, and assertion level for each standalone signal. `Signal` always exposes the actual RTL
value without polarity normalization. For example, setting an active-low reset signal to zero asserts reset. `Signal::direction()` determines
whether an interrupt, reset, or generic I/O signal is driven by gem5 or by the RTL model.

`clock()` must advance one complete RTL clock cycle and return only after outputs have stabilized. It evaluates the inactive phase, active clock
edge, and final inactive phase, generating callbacks for subscribed outputs after their final values are available. No gem5 simulation time passes
inside `clock()`; the gem5 adapter controls clock-event scheduling. `ClockResult::Finished` reports an RTL termination request, while
`ClockResult::Error` indicates an evaluation failure or fatal RTL condition.

`ClockResult::Finished` and `ClockResult::Error` are terminal states. After either result, the framework may only query `getLastError()`, unregister
signal callbacks by passing `nullptr`, and destroy the core. It must not clock the model, change signal values, or access backdoor memories.

The meaning of `isIdle()` is strict: it returns true only when further clock cycles with unchanged inputs cannot change externally observable RTL
state. gem5 may then stop scheduling clock events until it changes an input, such as an interrupt, reset, generic I/O, memory response, or bus wait
signal.
The vendor implementation must return false whenever it cannot safely prove that the model is idle.

The memory backdoor enumerates code and data TCM regions using their configured architectural base addresses. `readMemory()` and `writeMemory()`
transfer bytes in increasing address order and must reject an operation whose complete range does not fit within the selected region. Access is
untimed, bypasses the modeled bus, and is only permitted when `clock()` is not executing. A library that has no backdoor-accessible memory returns
zero from `memoryCount()`.

ELF and COFF parsing belongs to the framework rather than the vendor library. To initialize a code TCM, the framework loads each applicable image
section, locates the `MemoryRegion` containing its architectural address, converts that address to a region-relative offset, and calls
`writeMemory()`. It also zero-fills image sections such as BSS through the same API. This keeps object-file support out of vendor adapters.

For the single-clock proof of concept, the physical clock is owned and toggled by the vendor adapter inside `clock()` rather than exposed as a
`Signal`. Register access, checkpointing, tracing controls, and multiple clock domains should be separate optional interfaces added after the SCR1
execution path is working.

### `Bus`

```cpp
#include <cstddef>
#include <cstdint>

class Signal;

enum class BusProtocol : std::uint32_t
{
    Unknown = 0,
    Apb,
    Axi3,
    Axi3Ace,
    Axi4
};

enum class BusRole : std::uint32_t
{
    Initiator,
    Target
};

using SignalRoleId = std::uint32_t;

namespace ApbSignal
{
constexpr SignalRoleId PAddr   = 1;
constexpr SignalRoleId PSel    = 2;
constexpr SignalRoleId PEnable = 3;
constexpr SignalRoleId PWrite  = 4;
constexpr SignalRoleId PWData  = 5;
constexpr SignalRoleId PStrb   = 6;
constexpr SignalRoleId PProt   = 7;
constexpr SignalRoleId PReady  = 8;
constexpr SignalRoleId PRData  = 9;
constexpr SignalRoleId PSlvErr = 10;
}

// Base AXI3 channel roles. AXI4 and AXI3-ACE reuse applicable IDs and add
// protocol-specific roles in the same namespace.
namespace AxiSignal
{
constexpr SignalRoleId AwId    = 1;
constexpr SignalRoleId AwAddr  = 2;
constexpr SignalRoleId AwLen   = 3;
constexpr SignalRoleId AwSize  = 4;
constexpr SignalRoleId AwBurst = 5;
constexpr SignalRoleId AwLock  = 6;
constexpr SignalRoleId AwCache = 7;
constexpr SignalRoleId AwProt  = 8;
constexpr SignalRoleId AwValid = 9;
constexpr SignalRoleId AwReady = 10;

constexpr SignalRoleId WId     = 11;
constexpr SignalRoleId WData   = 12;
constexpr SignalRoleId WStrb   = 13;
constexpr SignalRoleId WLast   = 14;
constexpr SignalRoleId WValid  = 15;
constexpr SignalRoleId WReady  = 16;

constexpr SignalRoleId BId     = 17;
constexpr SignalRoleId BResp   = 18;
constexpr SignalRoleId BValid  = 19;
constexpr SignalRoleId BReady  = 20;

constexpr SignalRoleId ArId    = 21;
constexpr SignalRoleId ArAddr  = 22;
constexpr SignalRoleId ArLen   = 23;
constexpr SignalRoleId ArSize  = 24;
constexpr SignalRoleId ArBurst = 25;
constexpr SignalRoleId ArLock  = 26;
constexpr SignalRoleId ArCache = 27;
constexpr SignalRoleId ArProt  = 28;
constexpr SignalRoleId ArValid = 29;
constexpr SignalRoleId ArReady = 30;

constexpr SignalRoleId RId     = 31;
constexpr SignalRoleId RData   = 32;
constexpr SignalRoleId RResp   = 33;
constexpr SignalRoleId RLast   = 34;
constexpr SignalRoleId RValid  = 35;
constexpr SignalRoleId RReady  = 36;
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
    virtual const char* name() const noexcept = 0;

    virtual BusProtocol protocol() const noexcept = 0;

    // Role of the RTL module on this bus.
    virtual BusRole role() const noexcept = 0;

    virtual std::size_t signalCount() const noexcept = 0;

    // Enumerates the semantic role and Signal object for each binding.
    // Returns {0, nullptr} when index is out of range.
    virtual SignalBinding signal(std::size_t index) noexcept = 0;

  protected:
    virtual ~Bus() noexcept = default;
};
```

`Bus` represents a logical group of signals implementing APB, AXI4, AXI3, or AXI3-ACE. Signal bindings must use canonical semantic role IDs defined
by the framework; the framework must never infer protocol semantics from vendor-specific RTL signal names. `Signal::name()` remains available for
diagnostics and waveform tracing.

The V1 header defines complete canonical role-ID sets in `ApbSignal` and `AxiSignal`. AXI4, AXI3, and AXI3-ACE share IDs for common channel signals,
while their validation profiles determine which roles are required, optional, or prohibited. AXI3-specific roles include write-data IDs, and the
AXI3-ACE profile adds coherent address attributes, snoop channels, response channels, data channels, and acknowledge signals.

Role IDs are interpreted together with `Bus::protocol()`. APB and AXI may therefore use overlapping numeric ID values without ambiguity. The actual
RTL signal names remain vendor-defined and do not affect role matching.

The vendor adapter maps its physical RTL signals to these semantic roles. `RtlCore` owns all returned `Bus` and `Signal` objects, and their names and
pointers remain valid for the lifetime of the core.

For each bus, signal-binding indices are contiguous from zero through `signalCount() - 1`. The set of bindings and every returned `Signal` pointer
must remain stable for the lifetime of the core.

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
    virtual ~SignalChangeCallback() noexcept = default;

    virtual void update() noexcept = 0;
};

class Signal
{
  public:
    virtual const char* name() const noexcept = 0;
    virtual std::size_t bitWidth() const noexcept = 0;
    virtual SignalDirection direction() const noexcept = 0;

    virtual bool getValue(
        std::uint8_t* data,
        std::size_t dataSize) const noexcept = 0;

    virtual bool setValue(
        const std::uint8_t* data,
        std::size_t dataSize) noexcept = 0;

    // Installs or replaces the callback. Signal does not own the callback.
    // Passing nullptr unregisters the current callback.
    virtual void setChangeCallback(
        SignalChangeCallback* callback) noexcept = 0;

  protected:
    virtual ~Signal() noexcept = default;
};
```

`Signal` represents a single scalar or vector RTL signal. Directions are defined from the RTL model's perspective. `getValue()` may read any signal,
while `setValue()` must fail for output signals. For the proof of concept, values use two-state logic and little-endian byte order: RTL bit 0 is bit 0
of `data[0]`, and unused high bits in the final byte are zero. `dataSize` must equal `(bitWidth() + 7) / 8`; otherwise, the operation returns false.

The signal-change callback provides event-driven notification for signals such as generic I/O and interrupt outputs without polling. Its contract
is:

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

## SCR1 Reference Vendor Implementation

The SCR1 integration under `ext/rtl/scr1` serves as the reference vendor implementation of the PoC API. The pristine upstream SCR1 repository is
kept as a submodule at `ext/rtl/scr1/repo`, while the parent directory contains the adapter source, documentation, and independent shared-library
build used to produce the vendor DLL.

The reference model compiles only `scr1_top_axi` and maps its separate instruction and data AXI4 initiator interfaces to the framework.

```text
ext/rtl/scr1/
├── repo/              # Pristine upstream SCR1 submodule
├── src/               # Vendor-facing PoC API implementation
├── CMakeLists.txt      # Independent shared-library build
└── README.md           # Build, configuration, and integration guide
```

The adapter must depend only on `include/gem5/rtl_cosim/api_v1.hh`, SCR1, and the selected RTL-to-C++ tool runtime; it must not include gem5-internal
headers or link against gem5. It demonstrates how vendors map physical RTL signals to semantic bus and core bindings, implement clocking and signal
callbacks, expose optional TCM backdoors, and export the versioned manager factory and destructor symbols.

This integration is the reference vendors can copy and adapt for Verilator or commercial RTL-to-C++ models. gem5-specific loading, ports, and
signal-to-TLM transactors remain outside `ext/rtl/scr1`.

## Required Design Output

Provide:

1. A high-level architecture and component diagram.
2. A recommended source directory structure within gem5.
3. The complete vendor-facing PoC API specification.
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
