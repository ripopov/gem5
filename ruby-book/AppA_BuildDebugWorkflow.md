# Appendix A: Build and Debug Workflow

This appendix will provide a concise reference for building gem5 with different protocols enabled, using SCons.
It will cover build variants (`.debug`, `.opt`, `.fast`), protocol-enabled builds via Kconfig, and the minimum debug workflow needed for the book's labs.
GDB attachment, debug trace flags, and common build troubleshooting will be included as quick-reference material.
This serves as the go-to page when a reader hits a build or debug problem during any chapter's exercises.

## Protocol Build Integration

### One-Protocol-Per-Build Constraint

gem5 compiles only one coherence protocol at a time.
Each protocol compiles into the gem5 binary, and switching protocols requires a separate build directory.
For example, to work with both MI_example and MESI_Two_Level, maintain two build trees:

```sh
# MI_example build (default protocol in build_opts/RISCV)
scons build/RISCV/gem5.opt -j$(nproc)

# MESI_Two_Level build — override PROTOCOL on the command line
scons build/RISCV/gem5.opt -j$(nproc) PROTOCOL=MESI_Two_Level
```

### `.slicc` Manifest Files

Every protocol has a master `.slicc` file that lists all `.sm` files in dependency order.
**File ordering is critical**: types and message definitions must appear before the state machines that use them.
A typical manifest looks like:

```
protocol "MSI";
include "RubySlicc_interfaces.slicc";
include "MSI-msg.sm";
include "MSI-cache.sm";
include "MSI-dir.sm";
```

Declaring a type after it is used causes cryptic SLICC compiler errors, often reported on a line *after* the actual problem.

### Kconfig Protocol Registration

New protocols are registered via the Kconfig system.
The protocol's `Kconfig` file declares the protocol option, and the build system uses it to select which `.slicc` manifest to compile.
See `src/mem/ruby/protocol/Kconfig` for the existing protocol registrations.

### SLICC HTML Documentation Generation

The SLICC compiler can generate navigable HTML documentation for the compiled protocol.
Enable it via the `SLICC_HTML` build option:

```sh
# Add SLICC_HTML=True to generate protocol documentation
scons build/RISCV/gem5.opt -j$(nproc) PROTOCOL=MSI SLICC_HTML=True
```

The generated HTML shows all states, events, transitions, and actions per controller.
This is invaluable for understanding unfamiliar protocols and for debugging (see Chapter 17).
