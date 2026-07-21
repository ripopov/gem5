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
- Discover and support protocol-defined variable signal widths automatically, without model-specific transactor code or duplicated JSON settings.
- Implement APB, AXI4, and AXI3 protocol transactors.
- Define canonical AXI3-ACE signal discovery and validation, while leaving coherent transaction integration to future work.
- Provide a standalone checker that validates vendor libraries and protocol transactors without gem5.
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
machines for APB, AXI4, and AXI3 live in a pure C++ runtime shared by gem5 and the standalone checker.

The same `RequestPort` connects to either memory-system implementation:

- With the classic memory system, connect it to an XBar response-side port such as `cpu_side_ports`.
- With Ruby, connect it to `RubySequencer.in_ports`, which is a `VectorResponsePort`.

No Ruby-specific vendor interface or RTL SimObject port is required for APB, AXI4, or AXI3. AXI3-ACE support in this design is limited to canonical
signal-role definitions, discovery, and profile validation. `RubySequencer.in_ports` does not forward the required snoops to its requestor, and
classic gem5 snoop packets over a plain `RequestPort` cannot represent the complete ACE snoop-channel semantics. ACE transaction execution,
coherence integration, and behavioral testing are out of scope. A future implementation requires a dedicated gem5 coherence adapter.

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

// Common AXI3 and AXI4 channel roles. Protocol profiles decide which roles
// are required, optional, or prohibited.
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

// AXI4 sideband signals are appended to preserve the common role IDs above.
constexpr SignalRoleId AwRegion = 37;
constexpr SignalRoleId AwQos    = 38;
constexpr SignalRoleId AwUser   = 39;
constexpr SignalRoleId WUser    = 40;
constexpr SignalRoleId BUser    = 41;
constexpr SignalRoleId ArRegion = 42;
constexpr SignalRoleId ArQos    = 43;
constexpr SignalRoleId ArUser   = 44;
constexpr SignalRoleId RUser    = 45;

// AXI3-ACE additions. V1 validates these roles structurally but does not
// execute coherent transactions.
constexpr SignalRoleId AwDomain = 46;
constexpr SignalRoleId AwSnoop  = 47;
constexpr SignalRoleId AwBar    = 48;
constexpr SignalRoleId ArDomain = 49;
constexpr SignalRoleId ArSnoop  = 50;
constexpr SignalRoleId ArBar    = 51;
constexpr SignalRoleId AcAddr   = 52;
constexpr SignalRoleId AcSnoop  = 53;
constexpr SignalRoleId AcProt   = 54;
constexpr SignalRoleId AcValid  = 55;
constexpr SignalRoleId AcReady  = 56;
constexpr SignalRoleId CrResp   = 57;
constexpr SignalRoleId CrValid  = 58;
constexpr SignalRoleId CrReady  = 59;
constexpr SignalRoleId CdData   = 60;
constexpr SignalRoleId CdLast   = 61;
constexpr SignalRoleId CdValid  = 62;
constexpr SignalRoleId CdReady  = 63;
constexpr SignalRoleId Rack     = 64;
constexpr SignalRoleId Wack     = 65;
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

The V1 header defines canonical role-ID sets in `ApbSignal` and `AxiSignal`. AXI4, AXI3, and AXI3-ACE share IDs for common channel signals, while
their validation profiles determine which roles are required, optional, or prohibited. AXI3-specific roles include write-data IDs. AXI4 adds
`AwRegion`, `AwQos`, `AwUser`, `WUser`, `BUser`, `ArRegion`, `ArQos`, `ArUser`, and `RUser`. The structurally validated AXI3-ACE profile adds its
coherent address attributes, snoop channels, response channels, data channels, and acknowledge signals.

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

Signal widths are discovered exclusively through `Signal::bitWidth()` and must not be hard-coded for SCR1 or repeated as required JSON properties.
Transactors derive address, data, strobe, ID, USER, and other variable widths at initialization, validate all protocol-specific relationships, and
allocate byte buffers using `(bitWidth() + 7) / 8`. One transactor implementation must therefore handle every width accepted by its protocol profile.
Configuration may contain optional width assertions for diagnostics, but discovered API metadata remains the source of truth and a mismatch is an
initialization error.

#### AXI4 Validation Profile

The V1 AXI4 profile represents a full read/write AXI4 interface rather than AXI4-Lite. The base channel roles `AwId` through `RReady` are required
except for these rules:

- `WId` is prohibited because AXI4 removed write-data interleaving.
- `AwId` and `BId` are an optional matched pair; `ArId` and `RId` are another optional matched pair. An absent pair uses implicit ID zero, while a
  partially present pair is a validation error.
- `AwLock`, `ArLock`, `AwCache`, `ArCache`, `AwProt`, and `ArProt` are optional and default to zero when absent.

The AXI4 sideband roles `AwRegion`, `AwQos`, `AwUser`, `WUser`, `BUser`, `ArRegion`, `ArQos`, `ArUser`, and `RUser` are optional and default to zero
when absent. An absent optional role has no `SignalBinding`; the vendor must enumerate only real RTL pins and must not synthesize constant signals.

All non-fixed AXI4 widths are discovered independently for each bus instance. For example, the two SCR1 buses are handled as discovered 32-bit
address and data interfaces; supporting a model with another valid address, data, ID, or USER width requires no source-code change.

The profile enforces these widths and relationships for required signals and for optional signals when present:

- VALID, READY, LAST, and LOCK signals are one bit.
- `AwLen` and `ArLen` are 8 bits; `AwSize` and `ArSize` are 3 bits; `AwBurst` and `ArBurst` are 2 bits.
- `AwCache` and `ArCache` are 4 bits, while `AwProt` and `ArProt` are 3 bits.
- `AwRegion`, `ArRegion`, `AwQos`, and `ArQos`, when present, are 4 bits.
- `BResp` and `RResp` are 2 bits.
- `AwAddr` and `ArAddr` have the same nonzero width, which must not exceed 64 bits in V1.
- `WData` and `RData` have the same power-of-two width between 8 and 1024 bits. `WStrb` has one bit per data byte.
- Each present ID pair has equal widths between 1 and 32 bits. Write and read ID pairs may have different widths.
- USER signal widths are implementation-defined positive values and have no required relationship across channels.

For an RTL initiator, AW, W, and AR payload and VALID signals, plus `BReady` and `RReady`, are outputs; the corresponding READY, B, and R response
signals are inputs. An RTL target uses the inverse directions. Optional sideband signals follow the direction of their channel.

The initial neutral transaction backend does not carry CACHE, PROT, REGION, QOS, or USER metadata. A transactor samples and ignores such RTL-driven
sidebands only when their channel handshakes, and drives zero on sidebands directed into the RTL. A present, asserted LOCK is an unsupported exclusive
access and must produce an explicit runtime error. Optional signals must never cause eager payload sampling on an idle or stalled channel. This
behavior preserves functional SCR1 execution while keeping the canonical AXI4 signal mapping complete.

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

## Standalone API Checker and Pure C++ Runtime

The framework provides a standalone `rtl-cosim-check` executable that loads and exercises a vendor shared library without gem5. It is both a
vendor-library conformance checker and an end-to-end integration test for the shared protocol transactors.

```text
Vendor shared library
        │
        ▼
Vendor-Facing PoC API
        │
        ▼
Pure C++ RTL runtime
├── API and protocol-profile validation
├── signal bindings and callbacks
├── APB, AXI4, and AXI3 protocol engines
├── clock and reset sequencing
├── transaction queues and ordering
└── neutral transaction interfaces
        │
        ├── TransactionBackend ◄── RTL initiator
        └── TransactionSource  ──► RTL target
```

### Pure C++ Runtime

The reusable `rtl_cosim_runtime` library implements:

- Platform-independent shared-library loading.
- V1 entry-point resolution and object lifetime management.
- Bus, signal, and memory discovery.
- Protocol-profile, direction, width, role, and binding validation.
- Runtime discovery of address, data, strobe, ID, USER, and other variable signal widths, with dynamically sized signal buffers.
- Reset sequencing and initial signal sampling.
- Signal callback registration and teardown.
- APB, AXI4, and AXI3 protocol state machines.
- AXI3-ACE canonical signal-role and profile validation without transaction execution.
- Burst assembly, byte enables, response generation, AXI ID tracking, ordering, and backpressure.
- Transaction request and response queues.
- ELF, COFF, and raw-image loading into TCM or external memory.
- Clock-loop coordination and idle detection.

The runtime depends only on `include/gem5/rtl_cosim/api_v1.hh`, the C++ standard library, a small JSON parser, and platform shared-library APIs. It
must not depend on gem5, SystemC, Verilator, or any commercial RTL tool. SystemC TLM types are not used at this boundary; the runtime defines a small
tool-independent transaction interface.

```cpp
struct MemoryRequest
{
    // Runtime-generated token used to match an asynchronous response.
    std::uint64_t token;

    // Protocol transaction ID, such as an AXI ID. Zero when unused.
    std::uint32_t id;

    std::uint64_t address;
    bool write;

    // Bytes transferred per beat and the protocol burst shape.
    std::size_t beatBytes;
    BurstType burst;

    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> byteEnable;
};

struct MemoryResponse
{
    std::uint64_t token;
    std::uint32_t id;
    std::vector<std::uint8_t> data;
    bool error;
};

class TransactionBackend
{
  public:
    virtual bool canAccept(const MemoryRequest& request) const = 0;
    virtual bool submit(const MemoryRequest& request) = 0;
    virtual bool getResponse(MemoryResponse& response) = 0;
    virtual void advance() = 0;

    virtual ~TransactionBackend() = default;
};

class TransactionSource
{
  public:
    virtual bool getRequest(MemoryRequest& request) = 0;
    virtual bool canAcceptResponse(
        const MemoryResponse& response) const = 0;
    virtual bool submitResponse(const MemoryResponse& response) = 0;
    virtual void advance() = 0;

    virtual ~TransactionSource() = default;
};
```

`TransactionBackend` consumes requests produced by an RTL initiator. `TransactionSource` produces requests for an RTL target and consumes its
responses. Both use compact beat data in increasing-address order; transactors expand or extract bus lanes using the discovered data width.

The standalone backend implements sparse byte-addressable memory. The gem5 adapters convert neutral transactions into gem5 packets, queue
asynchronous responses, and translate gem5 retry notifications into backend or source availability.

### Protocol Transactor Cycle Interface

Protocol transactors are independent of both backends and use a cycle-oriented interface:

```cpp
class BusTransactor
{
  public:
    // Drive RTL inputs and capture any payload accepted at the upcoming edge.
    virtual bool beforeClock() = 0;

    // Commit captured handshakes and sample controls for the next active edge.
    virtual bool afterClock() = 0;

    virtual bool isIdle() const = 0;
    virtual const char* getLastError() const = 0;

    virtual ~BusTransactor() = default;
};
```

Each transactor retains the handshake state prepared for the upcoming active edge. `beforeClock()` drives READY, response, and data inputs,
determines which channels will handshake, and captures payload outputs only for those active handshakes. After `RtlCore::clock()`
completes, `afterClock()` commits the captured handshakes, then samples the stabilized VALID, READY, and other control signals needed for the next
edge. This prevents a transfer from being missed when RTL deasserts VALID or changes its payload during the active edge.

Signal sampling must be handshake-gated. For each channel, inspect VALID and READY first and call `Signal::getValue()` for address, data, ID,
attributes, response, or other payload signals only when that channel's handshake is active. Do not eagerly sample payload for an idle or stalled
channel. Payload driven by RTL must be captured before the active edge rather than reconstructed from post-edge values. This rule avoids most virtual
signal reads on idle buses without changing the vendor API or protocol behavior.

### Checker Command and Execution

The checker is built with CMake and accepts the vendor library path and checker JSON as positional arguments:

```bash
rtl-cosim-check ./libscr1.so scr1-check.json
```

It performs the following sequence:

1. Parse and validate the checker JSON.
2. Load the vendor library and resolve `createRtlCoreManagerV1` and `destroyRtlCoreManagerV1`.
3. Create one manager and one core.
4. Enumerate and validate all buses, standalone signals, and backdoor-accessible memories.
5. Create the appropriate pure C++ transactor for every APB, AXI4, or AXI3 bus; AXI3-ACE buses receive structural validation only.
6. Attach every transacted RTL initiator bus to a shared memory backend unless JSON assigns it to a separate address space.
7. Load configured ELF, COFF, or raw images into a matching TCM region or the standalone memory backend.
8. Drive configured generic inputs such as reset vector, hart ID, and fuse values.
9. Apply the configured reset sequence.
10. Clock until the core and all transactors are idle, the model finishes, an error occurs, or the cycle limit is reached.
11. Print discovered interfaces, validation results, traffic statistics, stop reason, and errors.
12. Destroy the core and manager, unload the vendor library, and return a meaningful process status.

If an AXI3-ACE bus is discovered, the checker completes structural profile validation and stops before reset or clock execution. It must not clock a
model while an ACE interface is undriven.

Instruction and data initiator buses share one memory address space by default. This allows instruction fetches and data accesses to observe the
same loaded program. JSON may explicitly assign an interface to a different memory backend when the RTL memory map requires it.

The clock loop is logically equivalent to:

```cpp
while (cycles < maxCycles) {
    for (auto& transactor : transactors) {
        if (!transactor->beforeClock())
            fail(transactor->getLastError());
    }

    const ClockResult result = core->clock();
    if (result == ClockResult::Error)
        fail(core->getLastError());
    if (result == ClockResult::Finished)
        break;

    for (auto& transactor : transactors) {
        if (!transactor->afterClock())
            fail(transactor->getLastError());
    }

    memory.advance();
    ++cycles;

    if (core->isIdle() && allTransactorsIdle())
        break;
}
```

A maximum cycle count is mandatory. A CPU may never become idle when memory is empty, the loaded program traps repeatedly, or the program never
executes a wait-for-interrupt instruction. Tests that expect an idle stop must load a program that reaches a defined idle state.

### Checker JSON

A representative configuration is:

```json
{
  "core": {
    "config": {
      "implementation": "scr1-axi"
    }
  },
  "reset": {
    "assert_cycles": 10
  },
  "inputs": {
    "fuse_mhartid": "0x0",
    "reset_vector": "0x0"
  },
  "memory": {
    "base": 0,
    "size": 67108864,
    "images": [
      {"path": "test.elf", "format": "auto"}
    ],
    "latency_cycles": 1
  },
  "run": {
    "max_cycles": 1000000,
    "stop_on_idle": true,
    "stop_on_finish": true
  }
}
```

The `core.config` object is serialized and passed to `RtlCoreManager::createCore()`. The checker-specific fields configure reset, generic inputs,
memory, image loading, traffic behavior, and termination conditions.

### Memory Test Modes

The initial SCR1 test uses shared fixed-latency memory and a small program that reaches an idle state. The checker architecture also supports:

- Zero or fixed memory latency.
- Capacity-driven READY backpressure with deterministic pending limits.
- Burst and narrow transfers.
- Address-range error responses.
- Multiple outstanding AXI IDs.
- Out-of-order responses across AXI IDs while preserving order within each ID.

The checker attaches shared memory to RTL initiator buses and uses configured transaction scripts to drive RTL target buses. Response expectations
may check data and error status. For AXI3-ACE, the checker validates only the canonical signal list, directions, and widths; it does not instantiate
an ACE transactor or drive the interface. ACE transaction behavior and coherence testing require a future dedicated backend and gem5 adapter.

### Checker Scope and Diagnostics

The checker validates:

- Shared-library and V1 entry-point compatibility.
- Vendor object construction and destruction.
- Interface discovery and protocol-profile compliance.
- Signal roles, directions, widths, active levels, values, and callbacks.
- APB, AXI3, and AXI4 channel handshakes, ordering, bursts, backpressure, and errors.
- AXI3-ACE canonical signal roles, directions, and widths, without transaction execution.
- Reset sequencing, TCM programming, external-memory traffic, and idle reporting.

It does not validate gem5 event scheduling, packet semantics, Classic XBar integration, Ruby integration, or any AXI3-ACE transaction behavior.
ACE coherence is future work rather than a gem5 integration test in the current design. The checker should use distinct nonzero exit codes for
command-line or JSON errors, library or API errors, validation errors, runtime or protocol errors, and cycle-limit timeouts.

### Source and Build Layout

```text
src/rtl/
├── CMakeLists.txt
├── README.md
├── runtime/
│   ├── model_loader.{hh,cc}
│   ├── model_validator.{hh,cc}
│   ├── transaction.{hh,cc}
│   ├── image_loader.{hh,cc}
│   └── protocol/
│       ├── apb.{hh,cc}
│       └── axi.{hh,cc}
├── checker/
│   ├── main.cc
│   ├── memory_backend.{hh,cc}
│   └── checker_config.{hh,cc}
├── fixtures/pulp/
│   ├── fixture_adapter.cc
│   ├── rtl/
│   └── config/
└── tests/
```

CMake builds `rtl_cosim_runtime`, `rtl_cosim_checker_support`, and `rtl-cosim-check` without gem5. Stage 3 will compile the same runtime and protocol
sources with the gem5 adapter. Protocol behavior must not be duplicated in checker and gem5 layers.

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
packet backends remain outside `ext/rtl/scr1`; the pure C++ protocol transactors are supplied by `rtl_cosim_runtime`.

## Implementation Stages

### Stage 1: Standalone AMBA Transactor Validation

- Use the PULP Platform SystemVerilog implementations for the independent Verilog/Verilator endpoints. Keep AXI at
  `ext/rtl/pulp/axi`, pinned to `v0.39.10`, and APB at `ext/rtl/pulp/apb`. The submodule gitlinks provide the authoritative
  exact revisions. Both projects use the Solderpad Hardware License 0.51, whose license text permits treating the work as Apache License 2.0.
- Keep PULP entirely on the reference RTL endpoint side. Thin Stage 1 wrappers flatten PULP interfaces and structs into canonical V1 signal bindings,
  tie unsupported AXI atomic-operation inputs to zero, and package the endpoints as Verilated vendor libraries. `rtl_cosim_runtime` and
  `rtl-cosim-check` must not include PULP headers, use PULP types, or depend on PULP build infrastructure.
- Provide independent CMake targets that run Verilator on the pinned PULP sources and build the AXI and APB master and slave fixtures as loadable
  vendor DLLs/shared libraries implementing the candidate V1 API. Building these libraries must not require gem5 or SCR1.
- Qualify every selected PULP master, slave, memory, delay, and error-response component with the pinned Verilator version before relying on it in an
  end-to-end fixture. Simulation-only PULP drivers require compile-and-execute coverage; successful lint of the synthesizable RTL alone is not
  sufficient.
- Implement the candidate V1 vendor API and both neutral transaction directions: RTL initiator to backend and transaction source to RTL target.
- Implement `rtl_cosim_runtime`, APB and AXI3/AXI4 initiator and target transactors, memory and transaction-driver backends, and `rtl-cosim-check`
  without SCR1 or gem5 dependencies.
- Use CMake and organize GoogleTest suites around deterministic C++ protocol fixtures, malformed behavior, reset, idle, errors, bursts, IDs, ordering,
  variable widths, backpressure, and handshake-gated sampling.
- Add end-to-end tests using independent Verilated master and slave RTL endpoints, including master-to-slave loop tests through the runtime.
- Limit AXI3-ACE work to canonical signal-profile validation; do not implement or behaviorally test ACE transactions or coherence.
- Stage 1 is complete when both protocol directions are documented, reproducible, sanitizer-clean, and independently validated; then freeze API V1.

### Stage 2: SCR1 Reference Vendor Integration

- Build the Verilated `scr1_top_axi` reference DLL under `ext/rtl/scr1` using only the frozen vendor API and the selected RTL-to-C++ tool runtime.
- Map both SCR1 AXI4 initiator buses and its reset, interrupt, fuse, and other standalone signals using automatic discovery and width validation.
- Implement SCR1 clocking, callbacks, idle detection, error reporting, and backdoor access to its unified instruction/data TCM.
- Reuse the Stage 1 runtime and AXI4 initiator transactor unchanged; SCR1-specific code must remain inside the reference vendor adapter.
- Add CMake and GoogleTest coverage for adapter lifetime, discovery, reset sequencing, TCM image loading, interrupts, idle wake-up, and failure paths.
- Run documented `rtl-cosim-check` scenarios with RISC-V unit and bare-metal programs, deterministic timeouts, and known completion or idle states.
- Stage 2 is complete when the SCR1 DLL passes all standalone tests without gem5 and serves as a reproducible reference implementation for vendors.

### Stage 3: gem5 Integration and Two-Core System Validation

- Implement `RtlCoreSimObject`, the gem5 packet backend, vector-port mapping, callbacks, clock scheduling, reset handling, and configuration validation.
- Reuse the Stage 1 runtime and APB and AXI3/AXI4 transactors unchanged; gem5-specific code is limited to SimObject, event, signal-port, and packet
  integration. AXI3-ACE remains structural-validation-only and outside Stage 3 execution and testing.
- Add a configuration system that creates two SCR1 `RtlCoreSimObject` instances and connects each core's instruction and data ports by API name.
- Provide one configurable two-core system that selects either a classic coherent cache hierarchy or Ruby `MESI_Two_Level` with `SimpleNetwork`.
- Run bare-metal RISC-V unit tests for boot, memory traffic, interrupts, reset, errors, idle wake-up, and deterministic multicore execution.
- Run dining philosophers using Peterson's algorithm with the required compiler barriers and RISC-V fences; SCR1 has no LR/SC or AMOs.
- Stage 3 is complete when the same configurable system passes both memory-system modes through gem5's test infrastructure, with documented commands
  and expected results.
