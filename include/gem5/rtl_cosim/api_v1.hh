/*
 * Copyright (c) 2026 The gem5 Authors
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This header is the complete vendor-facing RTL co-simulation V1 ABI.  It is
 * intentionally self-contained and contains no gem5, SystemC, or RTL-tool
 * types.  Vendors may copy this file into their own build trees unchanged.
 */

#ifndef __GEM5_RTL_COSIM_API_V1_HH__
#define __GEM5_RTL_COSIM_API_V1_HH__

#include <cstddef>
#include <cstdint>

// Informational major ABI version. Loaders negotiate V1 by resolving the
// versioned factory and destructor symbols declared at the end of this file.
inline constexpr std::uint32_t RtlCosimApiVersion = 1;

class RtlCore;
class Bus;
class Signal;
class RtlCpuState;

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

enum class MemoryRole : std::uint32_t
{
    CodeTcm,
    DataTcm
};

enum class ClockResult : std::uint32_t
{
    Completed,
    Finished,
    Error
};

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
    // Role of the RTL model, not the external component.
    Initiator,
    Target
};

enum class SignalDirection : std::uint32_t
{
    // Input means driven into RTL; Output means driven by RTL.
    Input,
    Output
};

using SignalRoleId = std::uint32_t;

namespace ApbSignal
{
// Canonical semantic roles. Physical signal names are vendor-defined.
inline constexpr SignalRoleId PAddr = 1;
inline constexpr SignalRoleId PSel = 2;
inline constexpr SignalRoleId PEnable = 3;
inline constexpr SignalRoleId PWrite = 4;
inline constexpr SignalRoleId PWData = 5;
inline constexpr SignalRoleId PStrb = 6;
inline constexpr SignalRoleId PProt = 7;
inline constexpr SignalRoleId PReady = 8;
inline constexpr SignalRoleId PRData = 9;
inline constexpr SignalRoleId PSlvErr = 10;
} // namespace ApbSignal

namespace AxiSignal
{
// Canonical roles common to the protocol profiles where applicable.
inline constexpr SignalRoleId AwId = 1;
inline constexpr SignalRoleId AwAddr = 2;
inline constexpr SignalRoleId AwLen = 3;
inline constexpr SignalRoleId AwSize = 4;
inline constexpr SignalRoleId AwBurst = 5;
inline constexpr SignalRoleId AwLock = 6;
inline constexpr SignalRoleId AwCache = 7;
inline constexpr SignalRoleId AwProt = 8;
inline constexpr SignalRoleId AwValid = 9;
inline constexpr SignalRoleId AwReady = 10;

inline constexpr SignalRoleId WId = 11;
inline constexpr SignalRoleId WData = 12;
inline constexpr SignalRoleId WStrb = 13;
inline constexpr SignalRoleId WLast = 14;
inline constexpr SignalRoleId WValid = 15;
inline constexpr SignalRoleId WReady = 16;

inline constexpr SignalRoleId BId = 17;
inline constexpr SignalRoleId BResp = 18;
inline constexpr SignalRoleId BValid = 19;
inline constexpr SignalRoleId BReady = 20;

inline constexpr SignalRoleId ArId = 21;
inline constexpr SignalRoleId ArAddr = 22;
inline constexpr SignalRoleId ArLen = 23;
inline constexpr SignalRoleId ArSize = 24;
inline constexpr SignalRoleId ArBurst = 25;
inline constexpr SignalRoleId ArLock = 26;
inline constexpr SignalRoleId ArCache = 27;
inline constexpr SignalRoleId ArProt = 28;
inline constexpr SignalRoleId ArValid = 29;
inline constexpr SignalRoleId ArReady = 30;

inline constexpr SignalRoleId RId = 31;
inline constexpr SignalRoleId RData = 32;
inline constexpr SignalRoleId RResp = 33;
inline constexpr SignalRoleId RLast = 34;
inline constexpr SignalRoleId RValid = 35;
inline constexpr SignalRoleId RReady = 36;

inline constexpr SignalRoleId AwRegion = 37;
inline constexpr SignalRoleId AwQos = 38;
inline constexpr SignalRoleId AwUser = 39;
inline constexpr SignalRoleId WUser = 40;
inline constexpr SignalRoleId BUser = 41;
inline constexpr SignalRoleId ArRegion = 42;
inline constexpr SignalRoleId ArQos = 43;
inline constexpr SignalRoleId ArUser = 44;
inline constexpr SignalRoleId RUser = 45;

// AXI3-ACE additions.  V1 validates these roles structurally but does not
// execute coherent transactions.
inline constexpr SignalRoleId AwDomain = 46;
inline constexpr SignalRoleId AwSnoop = 47;
inline constexpr SignalRoleId AwBar = 48;
inline constexpr SignalRoleId ArDomain = 49;
inline constexpr SignalRoleId ArSnoop = 50;
inline constexpr SignalRoleId ArBar = 51;
inline constexpr SignalRoleId AcAddr = 52;
inline constexpr SignalRoleId AcSnoop = 53;
inline constexpr SignalRoleId AcProt = 54;
inline constexpr SignalRoleId AcValid = 55;
inline constexpr SignalRoleId AcReady = 56;
inline constexpr SignalRoleId CrResp = 57;
inline constexpr SignalRoleId CrValid = 58;
inline constexpr SignalRoleId CrReady = 59;
inline constexpr SignalRoleId CdData = 60;
inline constexpr SignalRoleId CdLast = 61;
inline constexpr SignalRoleId CdValid = 62;
inline constexpr SignalRoleId CdReady = 63;
inline constexpr SignalRoleId Rack = 64;
inline constexpr SignalRoleId Wack = 65;
} // namespace AxiSignal

struct CoreSignalBinding
{
    CoreSignalRole role;
    std::uint32_t index;
    ActiveLevel activeLevel;
    // The signal exposes the actual RTL value; activeLevel is not normalized.
    Signal *signal;
};

struct MemoryRegion
{
    const char *name;
    MemoryRole role;
    std::uint64_t baseAddress;
    std::uint64_t size;
};

struct SignalBinding
{
    SignalRoleId role;
    Signal *signal;
};

struct CpuStateValue
{
    // Canonical schema-defined field name. Names are case-sensitive.
    const char *name;
    std::size_t bitWidth;
    // Values use the same little-endian byte/bit ordering as Signal. The
    // pointed-to bytes need only remain valid for the importState() call.
    const std::uint8_t *data;
    std::size_t dataSize;
};

class RtlCpuState
{
  public:
    // Versioned canonical schema implemented by this core, for example
    // "riscv64/v1". The returned string is valid for the core lifetime.
    virtual const char *schema() const noexcept = 0;
    virtual std::size_t contextCount() const noexcept = 0;

    // Imports one complete architectural context atomically. On failure the
    // RTL architectural state must remain unchanged. Microarchitectural state
    // must be clean and idle before a successful import becomes observable.
    virtual bool importState(std::size_t context,
                             const CpuStateValue *values,
                             std::size_t valueCount) noexcept = 0;
    virtual const char *getLastError() const noexcept = 0;

  protected:
    virtual ~RtlCpuState() noexcept = default;
};

namespace RtlCpuStateSchema
{
inline constexpr char Riscv64V1[] = "riscv64/v1";
} // namespace RtlCpuStateSchema

class SignalChangeCallback
{
  public:
    virtual ~SignalChangeCallback() noexcept = default;
    virtual void update() noexcept = 0;
};

class Signal
{
  public:
    virtual const char *name() const noexcept = 0;
    virtual std::size_t bitWidth() const noexcept = 0;
    virtual SignalDirection direction() const noexcept = 0;

    // Values have exactly ceil(bitWidth / 8) bytes. RTL bit zero is bit zero
    // of byte zero, and unused high bits in the final byte must be zero.
    virtual bool getValue(std::uint8_t *data,
                          std::size_t dataSize) const noexcept = 0;
    virtual bool setValue(const std::uint8_t *data,
                          std::size_t dataSize) noexcept = 0;
    // Installs or replaces the callback; nullptr unregisters it. Signal does
    // not own the callback. Input writes never generate notifications.
    virtual void
    setChangeCallback(SignalChangeCallback *callback) noexcept = 0;

  protected:
    virtual ~Signal() noexcept = default;
};

class Bus
{
  public:
    virtual const char *name() const noexcept = 0;
    virtual BusProtocol protocol() const noexcept = 0;
    virtual BusRole role() const noexcept = 0;
    virtual std::size_t signalCount() const noexcept = 0;
    // Bindings are contiguous and stable for the core lifetime. An
    // out-of-range index returns {0, nullptr}.
    virtual SignalBinding signal(std::size_t index) noexcept = 0;

  protected:
    virtual ~Bus() noexcept = default;
};

class RtlCore
{
  public:
    virtual const char *name() const noexcept = 0;
    virtual std::size_t busCount() const noexcept = 0;
    virtual Bus *bus(std::size_t index) noexcept = 0;
    virtual std::size_t signalCount() const noexcept = 0;
    virtual CoreSignalBinding signal(std::size_t index) noexcept = 0;
    virtual std::size_t memoryCount() const noexcept = 0;
    virtual const MemoryRegion *memory(std::size_t index) const noexcept = 0;
    virtual bool readMemory(std::size_t memoryIndex, std::uint64_t offset,
                            std::uint8_t *data,
                            std::size_t dataSize) const noexcept = 0;
    virtual bool writeMemory(std::size_t memoryIndex, std::uint64_t offset,
                             const std::uint8_t *data,
                             std::size_t dataSize) noexcept = 0;

    // Propagates preceding input writes through combinational logic without
    // advancing the physical clock or modeled time. Implementations that
    // always keep outputs stable may return true without doing any work.
    virtual bool settle() noexcept = 0;

    // Advances one complete clock cycle and returns after outputs stabilize.
    virtual ClockResult clock() noexcept = 0;

    // True only when unchanged inputs guarantee no observable state change.
    virtual bool isIdle() const noexcept = 0;
    virtual const char *getLastError() const noexcept = 0;

    // Optional architectural-state import capability. Existing vendor
    // implementations need no changes when CPU switching is unsupported.
    virtual RtlCpuState *cpuState() noexcept { return nullptr; }

  protected:
    virtual ~RtlCore() noexcept = default;
};

class RtlCoreManager
{
  public:
    // Creates one core from null-terminated JSON; returns nullptr on failure.
    virtual RtlCore *createCore(const char *configJson) noexcept = 0;

    // Core and manager objects must be destroyed by their creating library.
    virtual void destroyCore(RtlCore *core) noexcept = 0;
    virtual const char *getLastError() const noexcept = 0;

  protected:
    virtual ~RtlCoreManager() noexcept = default;
};

// These C-linkage names identify the exploratory V1 interface. Once V1 is
// published as stable, an incompatible API must introduce corresponding V2
// symbols and classes.
extern "C" RtlCoreManager *createRtlCoreManagerV1() noexcept;
extern "C" void destroyRtlCoreManagerV1(RtlCoreManager *manager) noexcept;

#endif // __GEM5_RTL_COSIM_API_V1_HH__
