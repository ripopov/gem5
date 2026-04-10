# Plan: Deprecate Ownerless Event Wrapper Constructors

## Goal

Guarantee that all `EventFunctionWrapper` and `MemberEventWrapper` events are
automatically traceable to their owning `SimObject` for FST waveform output.
Rather than removing the ownerless constructors (which would touch ~200 call
sites across ~89 files), deprecate them so the codebase migrates incrementally
while FST tracing works correctly today for all already-migrated events.

## Current State

### `Event` base class (`src/sim/eventq.hh`, `src/sim/eventq.cc`)

Two constructors exist:

```cpp
Event(Priority p = Default_Pri, Flags f = 0);                        // ownerless
Event(const SimObject &owner, Priority p = Default_Pri, Flags f = 0); // owner-aware
```

The owner-aware constructor calls `registerStaticOwner(this, owner)` which
inserts into the file-scoped `staticEventOwners` map.
`Event::lookupStaticOwner()` provides read access.
Destructor calls `clearStaticOwner(this)`.

### `MemberEventWrapper` (`src/sim/eventq.hh:1109–1164`)

Four constructors:

| Constructor | Status |
|-------------|--------|
| `(const SimObject &owner, CLASS &object, ...)` | preferred |
| `(const SimObject &owner, CLASS *object, ...)` | pointer convenience, delegates to above |
| `(CLASS *object, ...)` | `[[deprecated]]` — message says "Use reference version" |
| `(CLASS &object, ...)` | **not deprecated — needs deprecation** |

### `EventFunctionWrapper` (`src/sim/eventq.hh:1170–1222`)

Two constructors:

| Constructor | Status |
|-------------|--------|
| `(const SimObject &owner, callback, name, ...)` | preferred |
| `(callback, name, ...)` | **not deprecated — needs deprecation** |

---

## Plan

### Step 1: Deprecate ownerless constructors

Mark the two remaining ownerless constructors with `[[deprecated]]`:

**`EventFunctionWrapper`** — ownerless constructor (line 1194):

```cpp
[[deprecated("Pass owning SimObject as first argument for FST traceability")]]
EventFunctionWrapper(const std::function<void(void)> &callback,
                     const std::string &name,
                     bool del = false,
                     Priority p = Default_Pri)
    : Event(p), callback(callback), _name(name)
{
    if (del)
        setFlags(AutoDelete);
}
```

**`MemberEventWrapper`** — ownerless reference constructor (line 1146):

```cpp
[[deprecated("Pass owning SimObject as first argument for FST traceability")]]
MemberEventWrapper(CLASS &object,
                   bool del = false,
                   Priority p = Default_Pri):
    Event(p),
    Named(object.name() + ".wrapped_event"),
    mObject(&object)
{
    if (del) setFlags(AutoDelete);
    gem5_assert(mObject);
}
```

The pointer overload at line 1122 is already deprecated with a different
message; update its message to match:

```cpp
[[deprecated("Pass owning SimObject as first argument for FST traceability")]]
MemberEventWrapper(CLASS *object, bool del = false, Priority p = Default_Pri)
    : MemberEventWrapper{*object, del, p}
{}
```

### Step 2: Migrate priority subsystems (RISC-V build path)

Migrate the subsystems that actually compile in a RISC-V build and that are
most relevant for FST tracing. These are the high-value targets where FST
event traces matter.

**Priority 1 — memory hierarchy and Ruby (core tracing targets):**

| File | Members | Notes |
|------|---------|-------|
| `src/mem/cache/base.hh` | 2 | SimObject subclass, pass `*this` |
| `src/mem/xbar.hh` | 1 | SimObject subclass |
| `src/mem/simple_mem.hh` | 2 | SimObject subclass |
| `src/mem/mem_ctrl.hh` | 5 | SimObject subclass |
| `src/mem/dram_interface.hh` | 6 | Not SimObject, has `MemCtrl &ctrl` |
| `src/mem/nvm_interface.hh` | 2 | Not SimObject, has `MemCtrl` access |
| `src/mem/hbm_ctrl.hh` | 2 | SimObject subclass |
| `src/mem/bridge.hh` | 2 | Inner classes with `BridgeBase&` (ClockedObject) |
| `src/mem/packet_queue.hh` | 1 | Has `EventManager&`, needs SimObject ref added |
| `src/mem/comm_monitor.hh` | 1 | SimObject subclass |
| `src/mem/ruby/common/Consumer.hh` | 1 | Has `ClockedObject *em` |
| `src/mem/ruby/system/Sequencer.hh` | 1 | SimObject subclass |
| `src/mem/ruby/system/RubySystem.hh` | 1 | `new` call, SimObject context |

**Priority 2 — CPU and simulation core:**

| File | Members | Notes |
|------|---------|-------|
| `src/cpu/base.hh` | 1 | SimObject subclass |
| `src/cpu/simple/timing.hh` | 2 | SimObject subclass |
| `src/cpu/simple/atomic.hh` | 1 | SimObject subclass |
| `src/cpu/minor/cpu.hh` | 1 | SimObject subclass + `new` call |
| `src/cpu/o3/cpu.hh` | 2 | SimObject subclass |
| `src/cpu/o3/lsq.hh` | 1 | Not SimObject, needs CPU ref |
| `src/cpu/trace/trace_cpu.hh` | 2 | SimObject subclass |
| `src/sim/root.hh` | 1 | SimObject subclass |
| `src/sim/ticked_object.hh` | 1 | Has `ClockedObject &owner` |

**Priority 3 — RISC-V devices:**

| File | Members | Notes |
|------|---------|-------|
| `src/dev/riscv/plic.hh` | 1 | SimObject subclass |
| `src/dev/lupio/lupio_blk.hh` | 1 | SimObject subclass |
| `src/dev/lupio/lupio_tmr.cc` | 1 | `new` call, SimObject context |
| `src/dev/serial/uart8250.hh` | 2 | SimObject subclass |
| `src/dev/dma_device.hh` | 1+1 | DmaPort has `ClockedObject*`; DmaCallback `new` |
| `src/dev/pci/copy_engine.hh` | 5 | SimObject subclass |
| `src/dev/storage/ide_disk.hh` | 6 | SimObject subclass |

### Step 3: Leave non-RISC-V subsystems for later

These files use the deprecated constructors but are not part of the RISC-V
build or are low-priority for tracing. They will produce deprecation warnings
only when someone builds their respective ISA targets:

- `src/dev/arm/*` — ARM-specific devices (~25 members across ~15 files)
- `src/dev/amdgpu/*` — AMD GPU (~2 files)
- `src/gpu-compute/*` — GPU compute (~3 files)
- `src/arch/arm/*` — ARM table walker, fastmodel (~7 files)
- `src/arch/amdgpu/*` — AMD GPU TLBs (~4 files)
- `src/arch/x86/*` — x86 interrupts, page walker (~2 files)
- `src/arch/mips/*` — MIPS ISA (~1 file)
- `src/dev/net/*` — network devices (~6 files)
- `src/systemc/*` — SystemC bridges (~1 file)
- `src/learning_gem5/*` — tutorials (~2 files)

### Step 4: Suppress deprecation warnings during build

While migration is in progress, the ownerless constructors will emit
compiler warnings. To keep the build clean for CI while allowing gradual
migration, there are two options:

**Option A (preferred):** Suppress with a pragma in the ownerless constructors:

```cpp
// Temporary: suppress deprecation in the definition itself so that
// only NEW call sites trigger warnings, not the existing ones.
// Remove once migration is complete.
```

This is not needed — `[[deprecated]]` warnings fire at the *call site*, not
the definition. Existing code will warn; new code will also warn. The warnings
serve as migration pressure without breaking the build (warnings are not
errors by default in gem5's SCons config for `.opt` builds).

**Option B:** If warnings-as-errors is enabled, use targeted
`GCC diagnostic ignored` pragmas at stubborn call sites until they are
migrated.

### Step 5: FST trace handles missing owners gracefully

In `FstTrace::startup()`, when building the event-to-FST-handle map:

```cpp
for (auto *event : Event::getAllEvents()) {
    const SimObject *owner = Event::lookupStaticOwner(event);
    if (!owner) continue;  // skip unowned events — not traceable
    // ... create FST signal under owner's scope ...
}
```

Events without owners simply don't appear in the FST trace. This is safe
and correct: the trace shows all migrated events, and the deprecation
warnings guide developers to migrate the rest over time.

---

## Non-SimObject Classes Requiring Special Attention

These 7 classes own `EventFunctionWrapper` members but are not SimObject
subclasses. Each needs a SimObject reference threaded through:

| Class | File | Access to SimObject |
|-------|------|---------------------|
| `Consumer` | `src/mem/ruby/common/Consumer.hh` | `ClockedObject *em` member — use directly |
| `DmaPort` | `src/dev/dma_device.hh` | `ClockedObject *device` member — use directly |
| `PacketQueue` | `src/mem/packet_queue.hh` | `EventManager &em` — add `const SimObject&` param |
| `BridgeResponsePort` | `src/mem/bridge.hh` | `BridgeBase &bridge` (ClockedObject) — use directly |
| `BridgeRequestPort` | `src/mem/bridge.hh` | `BridgeBase &bridge` (ClockedObject) — use directly |
| `EtherLink::Link` | `src/dev/net/etherlink.hh` | `EtherLink *parent` (SimObject) — use directly |
| `o3::LSQ` | `src/cpu/o3/lsq.hh` | No direct ref — add `CPU&` or `SimObject&` param |

---

## Execution Order

1. Add `[[deprecated]]` to the two ownerless constructors and update the
   existing deprecation message on `MemberEventWrapper(CLASS*, ...)`
2. Migrate Priority 1 files (memory hierarchy + Ruby) — this is the FST
   tracing sweet spot
3. Migrate Priority 2 files (CPU + sim core)
4. Migrate Priority 3 files (RISC-V devices)
5. Leave remaining files to produce deprecation warnings until migrated

Each priority group is independently committable and testable with
`scons build/RISCV/gem5.opt`.
