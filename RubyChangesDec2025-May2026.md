# Changes to `src/mem/ruby` — December 2025 to May 2026

This document summarizes every patch that touched `src/mem/ruby` between
December 2025 and 20 May 2026, in chronological order. It is based on an
analysis of 47 commits in the gem5 repository, grouped into 33 logical
changes (some pull requests landed as several commits or as a feature
commit plus a merge/squash commit).

Dates shown are commit (landing) dates. Several patches carry old author
dates because they were long-standing contributions rebased onto the
current tree before merging.

---

## December 2025

### 1. Add `uint8_t` and `uint16_t` data types to SLICC
*2025-12-22 · `78bfe9bb9f` · Giacomo Travaglini*

SLICC is the domain-specific language gem5 uses to describe Ruby cache
coherence protocols. Until now protocol authors could only declare
unsigned integer fields of 32 or 64 bits. This patch registers `uint8_t`
and `uint16_t` as primitive external SLICC types, letting protocol message
structures use narrower, more appropriately sized integer fields. It has no
behavioral effect on its own; it is an enabling change and a direct
prerequisite for the CHI `lpid` work that follows.

### 2. Add a CHI `lpid` field to `CHIRequest`
*2025-12-22 · `ca22e394e3` · Giacomo Travaglini*

This adds a logical-processor-ID (`lpid`) field, declared as the newly
available `uint8_t` type, to the CHI protocol's request and response
messages. The motivation is that several logical processors can share a
single requestor node, and the requestor field alone cannot tell them
apart. The new field is most important in the CHI retry/credit flow: when a
request cannot be tracked for lack of buffer resources, the cache enqueues a
retry, and the `lpid` is carried through the retry-queue entry, the
`RetryAck`, and the `PCrdGrant` credit grant so the credit reaches the
correct logical processor. The retry queue is refactored from an ad-hoc
helper into a structured push of a retry-queue entry, and the TLM CHI
controller is updated to propagate `lpid` between payloads and messages.

### 3. Add a backpressure mechanism to `TlmController`
*2025-12-22 · `97b5ab7bc3` · Giacomo Travaglini · PR #2851*

This implements CHI protocol-credit (p-credit) handling in the
`TlmGenerator`, the TLM-based CHI test/traffic generator. Previously the
generator assumed every transaction would always be accepted. Now, when a
downstream node such as a Home Node cannot accept a transaction, it returns
a `RetryAck`; the generator removes the transaction from its pending set and
either re-issues it immediately if a credit is available, or parks it in a
"waiting for credit" queue until a `PCrdGrant` arrives. To support
re-injection at the head of the queue, the enqueue method is split into
front and back variants. This is a verification/modeling improvement and
does not affect production simulation paths.

---

## January 2026

### 4. Remove a shadow declaration of `m_ruby_system`
*2026-01-20 · `8a070a7c64` · Mahyar Samani · PR #2893*

The Ruby `Sequencer` declared and initialized its own `m_ruby_system`
member even though its base class `RubyPort` already provides exactly the
same member. This shadowing is error-prone, because base-class and
derived-class code could end up referring to different variables. The patch
deletes the duplicate member and its constructor initialization so the
`Sequencer` consistently uses the single inherited field. It is a
code-hygiene cleanup with no intended behavioral change, and it also
prepares the ground for the later runtime protocol-check work, which relies
on accessing `m_ruby_system` through the base class.

### 5. Pre-commit autoupdate (Black formatter)
*2026-01-23 / 2026-01-26 · `e81893fb70`, `ee36068db5` · pre-commit-ci / Erin (Jianghua) Le · PR #2899*

This repository-wide maintenance change bumps the Black Python formatter
pre-commit hook from 25.12.0 to 26.1.0 and applies the resulting style
changes. Only one Ruby file is affected: in `network/simple/SimpleLink.py`
a multi-line `fatal()` message argument is re-wrapped onto fewer lines. The
reformatting commits are also added to the blame-ignore list. There is no
functional or behavioral change to Ruby; this is purely a code-style and
tooling update.

---

## February 2026

### 6. Fix packet conversion to atomic type with the `ATOMIC` flag
*2026-02-02 · `3dd002b78e` · xin*

The Ruby `Sequencer` code that maps an incoming atomic packet to the Ruby
`ATOMIC_RETURN` / `ATOMIC_NO_RETURN` request types was guarded by
`#if defined(PROTOCOL_CHI)`, but that macro name does not exist — the build
system actually defines `RUBY_PROTOCOL_CHI`. As a result the entire atomic
branch was always compiled out, so atomic packets carrying the `ATOMIC`
flag were never converted to proper CHI atomic requests even on CHI builds.
This patch corrects the macro name and adds the missing config-header
include, so atomic operations are now correctly issued under CHI.

### 7. Use runtime protocol information instead of a build macro
*2026-02-02 · `bfe145803b` · xin*

A follow-up to the previous fix that removes compile-time protocol gating
of atomic handling altogether. Instead of a preprocessor `#if` on the CHI
macro, the atomic branch is now guarded by a runtime check that asks the
configured Ruby system for its protocol name. This is cleaner and more
robust: the binary no longer needs to be compiled specifically for CHI for
the branch to exist, and the decision is made dynamically from the actual
running configuration. Behavior is identical to the previous fix, but
expressed through gem5's runtime protocol-info mechanism.

### 8. Make `_transactions` a per-instance variable in `TlmGenerator`
*2026-02-02 · `413ce67fe6` · xin*

In the CHI TLM `TlmGenerator` Python SimObject, the `_transactions` list
holding pending transactions was declared at class level, so every
generator instance shared one and the same list. With multiple generators,
transactions enqueued on one would leak into all others. The fix moves the
list into the constructor so each generator gets its own independent list.
It is a correctness fix for multi-generator TLM CHI test configurations and
has no effect with a single generator.

### 9. Improve the performance of `NetDest::resize`
*2026-02-07 · `68688c817f`, `005e24b60f` · Jason Lowe-Power · PR #2921*

`NetDest::resize()` is one of the hottest functions in Ruby because it sits
on the critical path of many operations, rebuilding a per-machine vector of
bitmask sets every time a `NetDest` is constructed. Since almost every
`NetDest` is sized for all machines in the same Ruby system, the resulting
vector is nearly always identical. This patch adds a static cache, keyed by
the Ruby system pointer, that stores one already-built vector; subsequent
resizes simply copy it instead of recomputing. Keying on the Ruby system
keeps it correct for multi-system setups such as GPU configurations. The
measured effect is large — in a CHI traffic-generator test, time spent in
`resize` dropped from 28 seconds to under 1 second, out of a roughly
188-second baseline run.

### 10. Add missing transaction IDs in the CHI protocol
*2026-02-11 · `ecf856468b` · Jason Lowe-Power · PR #2935*

Several CHI cache-controller SLICC actions emitted response messages
without setting the `txnId` (and in some cases `dbid`) field. In CHI the
transaction ID ties a response back to the request it satisfies, so leaving
it unset could cause responses to be mismatched or mishandled by the
receiving node. The patch copies the transaction ID from the
transaction-buffer entry into the affected responses — including forwarded
snoop responses, separate-data responses, data-buffer-ID responses, and
write-unique completions — improving CHI protocol correctness.

### 11. Allow non-power-of-two cache capacity
*2026-02-17 · `c916ccf9be` · Jason Lowe-Power · PR #2922*

Ruby's `CacheMemory` previously required the number of cache sets to be a
power of two, because it computed the set index purely by bit-selection on
the address. This patch records at initialization whether the set count is
a power of two: if so, the fast bit-select path is kept; otherwise the set
index falls back to a modulo operation. A clearer fatal error is also added
when the configured cache size is not an exact multiple of associativity
times block size. Users can now configure non-power-of-two cache capacities
without slowing down the common power-of-two case.

### 12. Add names to `TBEStorage` objects
*2026-02-27 · `08efef22dc` · Jason Lowe-Power · PR #2882*

This improves statistics readability for the CHI protocol's
transaction-buffer storage. The `TBEStorage` and `MN_TBEStorage`
constructors now accept a name string, which is forwarded to the underlying
statistics group, and the CHI state machines plus the SLICC code generator
are updated to create each storage instance with a meaningful name.
Previously multiple TBE-storage objects in one controller emitted
identically named statistics tagged "(Unspecified)", making them
indistinguishable in the output. Now each one carries a distinct subname.
The impact is purely observability — no functional or timing change.

---

## March 2026

### 13. Exit simulation when the `TlmGenerator` is done
*2026-03-05 · `a93d44c32c` · Giacomo Travaglini · PR #2977*

The CHI TLM `TlmGenerator` never told the simulator to stop once it had
injected and processed all of its packets. With simple memory this was
harmless, but with more complex systems other components kept scheduling
events, producing very long, wasteful runs. The patch adds an `isActive()`
method reporting whether the generator still has pending, unscheduled, or
credit-waiting transactions; when a reply arrives and the generator is no
longer active, it now ends the simulation loop. `isActive()` is also
exported to Python so multi-generator scripts can wait for all of them to
drain. Diagnostics are improved too: warnings and panics are tagged with the
CHI transaction ID and the still-pending transactions are printed on suite
failure.

### 14. Fix a compiler warning in the CHI TLM `CacheController`
*2026-03-05 · `888785b637` · Giacomo Travaglini / Andreas Sandberg · PR #2979*

The nested `Transaction` class inside the CHI TLM `CacheController` has
virtual methods but its destructor was not declared virtual. This both
triggered a compiler warning and was a latent bug: deleting a derived
`Transaction` through a base-class pointer would not run the correct
destructor, risking incomplete cleanup. Marking the destructor virtual
ensures the proper teardown chain runs and silences the warning. There is no
behavioral change beyond correct object destruction.

### 15. Optionally ignore debug accesses in `TlmController`
*2026-03-05 · `94f5f4448b` · Giacomo Travaglini / Andreas Sandberg · PR #2978*

The CHI `TlmController` did not implement functional (debug) read and write
accesses, so any such access through it would panic. This patch adds those
override methods together with a new `ignore_functional` parameter
(defaulting to off). When the parameter is enabled, functional accesses
become harmless no-ops that return success without touching memory, on the
assumption that no caches sit behind the controller. When left at its
default, the original panic behavior is preserved. This helps full-system or
mixed setups where functional accesses must be allowed to pass through.

### 16. Handle separate `Comp`/`DBIDResp` responses in the CHI Controller
*2026-03-09 · `7e100cc304` · Giacomo Travaglini · PR #2987*

The CHI TLM controller's response-translation utilities only recognized the
combined `CompDBIDResp` write acknowledgement. This patch extends them so
the controller also handles writes whose completion and data-buffer-ID
responses arrive as two separate messages, mapping the `Comp` and
`DBIDResp` CHI response types to their respective opcodes and response
states. The result is broader CHI protocol compatibility, allowing the TLM
controller to interoperate with components that split the write
acknowledgement into distinct responses.

### 17. Add a missing `Param` import in `TlmController.py`
*2026-03-31 · `7cd09beb8d` · Giacomo Travaglini · PR #3047*

A build fix. The earlier change that added the `ignore_functional`
parameter to `TlmController.py` forgot to import `Param` from `m5.params`,
which broke all CHI-TLM builds. This one-line patch adds the missing import
and restores the ability to compile CHI-TLM configurations. There is no
functional change.

---

## April 2026

### 18. HeteroGarnet per-vnet dedicated links for XY routing
*2026-04-07 · `94d5aa38b4` · Pol Petrakis · PR #3060*

This completes support in the Garnet network model for assigning dedicated
physical links to individual virtual networks when using XY routing on a
mesh topology, and adds a `--per-vnet-links` command-line flag. The aim is
to model heterogeneous interconnects such as ARM's CMN, where different
traffic classes use separate channels to improve bandwidth isolation and
reduce head-of-line blocking. The routing unit's direction-to-port map
becomes a per-vnet structure so the XY route computation can pick the port
belonging to a packet's virtual network, and network links gain a per-vnet
flit-count statistic for measuring utilization. The change is fully
backward compatible — without the flag, behavior is identical to before — and
a regression test is added.

### 19. Prefetch wrapper fixes read-hit and fill notifications
*2026-04-14 · `3ecc1d47ef`, `8a4728e52d` · Tiago Mück · PR #3079*

In `RubyPrefetcherProxy`, the wrapper that notifies prefetchers of cache
events, the synthetic packets created for read-hit and cache-fill
notifications were built as request commands. Because they were not
response packets, they did not advertise that they carry data, so
prefetchers could not inspect the block data. This patch builds those
notification packets as the appropriate response commands so they correctly
carry data and prefetchers can read it.

### 20. Fix missing retries on prefetch completion
*2026-04-14 · `2a65c21321` · Tiago Mück · PR #3079*

A second, distinct fix bundled into the same pull request, this time in
`Sequencer::hitCallback`. Software prefetches that signal early completion
to the CPU took a shortcut path that deleted the packet and returned
immediately, bypassing the normal Ruby-port hit callback. As a result, if
the Ruby port was blocked, the retry machinery was never triggered and
stalled requests could wait indefinitely. The fix makes the
early-prefetch-completion path also run the drain-completion test and the
retry-sending logic, so retries are properly issued when the port unblocks.

### 21. Optional `RubyPort` response latency
*2026-04-15 · `9c7707747a`, `10f617fcb7` · Tiago Mück · PR #3085*

This adds a new `response_latency` parameter (in cycles, defaulting to zero)
to `RubyPort`. Previously response packets were always scheduled in the
same cycle as the hit callback, on the assumption that protocol latency was
already accounted for. With this change the response port adds the
configured latency before scheduling the timing response back to the peer,
letting users model extra delay on the response path without modifying the
protocol itself. Leaving the parameter at its default of zero preserves the
original behavior.

### 22. MPAM payload support for the CHI TLM interface
*2026-04-17 · `629c85055d`, `6fd19a7c04` · Giacomo Travaglini*

Two related commits add support for MPAM (Memory System Resource
Partitioning and Monitoring) attributes on CHI TLM transactions. The first
adds a utility that converts the MPAM data carried in a CHI payload into
gem5's native MPAM bundle type, mapping the non-secure flag, partition ID,
and partition-monitoring ID fields — bridging the external ARM CHI TLM
representation and gem5's internal MPAM model. The second exposes the
payload's MPAM field to Python through new pybind11 bindings, so test and
configuration scripts can read and set MPAM attributes on CHI TLM
transactions directly.

### 23. Remove the GPU shadow ROM range
*2026-04-23 · `bc57aff710` · Matthew Poremba · PR #3108*

This removes a long-obsolete "shadow ROM" feature that let the `System`
class declare address ranges (such as a VGA/VBIOS ROM region) which
`RubyPort` would treat specially and exclude from being recognized as
normal physical memory. The feature was intrusive to the `System` class and
is no longer needed, because GPU full-system scripts now load the VBIOS
into the ROM region directly. The patch deletes the shadow-ROM parameter
and accessor from `System`, drops the special check in `RubyPort`, and
removes the VGA ROM setup from the GPU full-system config. It is a cleanup
with no impact on users who were not relying on the removed parameter.

---

## May 2026

### 24. Fix an obsolete `PROTOCOL == 'CHI'` build check
*2026-05-04 · `9c2bda0d5c` · Giacomo Travaglini · PR #3138*

A small build-system fix in the CHI generic protocol's SConscript. After
gem5 gained multi-protocol build support, where several Ruby coherence
protocols can be compiled into one binary, the single `PROTOCOL`
configuration string is no longer the right thing to test. The SConscript
still used the obsolete `PROTOCOL == 'CHI'` check to decide whether to
build the CHI generic sources, which would silently skip them in a
multi-protocol build. The fix switches to the new boolean
`RUBY_PROTOCOL_CHI` configuration variable so the CHI generic code is built
whenever CHI is among the selected protocols. Runtime behavior is unchanged.

### 25. Add support for `RespSepData` in the TLM controller
*2026-05-04 · `c893db2619` · Giacomo Travaglini / Salil Akerkar · PR #3137*

This fixes handling of the CHI `RespSepData` response opcode, the separate
completion response used when data and completion are split — relevant once
early direct-memory-transfer deallocation is enabled. Previously a read
transaction could be considered finished on receiving this response, but
reads actually complete on the following data beats; finishing early meant
later data could no longer be matched to the transaction. The read handler
now explicitly does not finish on a `RespSepData` response, and the opcode
conversion utilities are extended to map this opcode in both directions.
The effect is correct transaction tracking with early deallocation enabled.

### 26. Controller and cache flushing support
*2026-05-08 · `5fb64ac70c`, `9fcf2c313b`, `9cf097c4d5`, `0214260c61` · Giacomo Travaglini*

This set of four commits adds the ability to tear down (flush) Ruby cache
and controller state from Python. One commit exposes flushing of a Ruby
cache to Python; another adds a flush-entries method to the perfect cache
memory; another adds Python APIs on the abstract controller to flush an
entire controller; and the last wires this into the CHI protocol so that
flushing a CHI cache controller actually clears its cache structures. The
motivation is testability: by allowing caches and controllers to be reset
between tests, individual unit tests — for example tests of replacement
policies — become properly isolated and reproducible. These APIs are
intended for debugging and test platforms and do not change normal
simulation behavior unless explicitly invoked.

### 27. Dump network route profiles
*2026-05-11 · `46724147da`, `53cdf9b24f` · Tiago Mück · PR #3134*

This adds a network route-profiling capability to Ruby's SimpleNetwork.
When a new `trace_routes` parameter is enabled, the network records every
unique route — the sequence of source port, intermediate routers, and
destination port — that messages travel, along with hop counts and
per-router delays. At the end of simulation it writes a sorted text file
listing each route with its average delay, message count, and virtual
network. A new route-profiler class implements the bookkeeping, message
buffers and switches are instrumented to feed it timing data, and a debug
flag prints extra routing detail. It is a diagnostic tool for understanding
interconnect congestion and latency hotspots, with no effect unless enabled.

### 28. Implementation of CHI Completer Busy (CBusy)
*2026-05-11 · `7fc45cbeac`, `794b0a29f1`, `c303b38d2a`, `d6c4a47545` · Tiago Mück / Giacomo Travaglini · PR #3133*

This implements the CHI Completer Busy field, by which a transaction
completer signals its current activity level so requesters can decide how
aggressively to generate speculative traffic. One commit adds sequencer
callbacks so protocols can propagate a completer-busy value into generated
response packets. The core commit makes CHI memory and cache controllers
set the busy field in responses based on transaction-buffer occupancy, using
new generator classes that map occupancy bands to busy levels, plus a
tracker that averages the most recent busy value seen from each downstream
responder so a few busy nodes do not skew the result. A third commit
forwards and translates the field through the TLM controller. Note this
patch only generates and propagates CBusy; consuming it to throttle
speculation is left to future work.

### 29. New primitives for the `TlmGenerator.Transaction` class
*2026-05-11 · `ffddfb74f3`, `ef86b9ec35`, `1a54592589` · Giacomo Travaglini · PR #3153*

This adds two primitives to the TLM CHI transaction test interface. The
first, an assertion helper that takes an explicit descriptive string,
complements the existing assertion macro, which no longer requires a string
to be supplied. The second is a timed variant of the existing wait
primitive: instead of waiting indefinitely it specifies a timeout in cycles,
after which the action-evaluation sequence is restarted — useful, for
example, when a test wants to deliberately delay sending a response for a
fixed time. Together these make TLM-based CHI test scripting more
expressive, improving assertion readability and enabling timed behaviors.

### 30. Add basic statistics to the `TlmGenerator`
*2026-05-11 · `9a482fc432`, `9aab54707f` · Giacomo Travaglini · PR #3156*

This adds a basic statistics group to the TLM CHI traffic generator. Three
scalar counters are introduced: the number of transactions sent on the
request channel, the number of `RetryAck` responses received, and the
number of `PCrdGrant` credit-grant responses received. They are incremented
in the generator's send and credit-handling logic and emitted into gem5's
normal stats output. The motivation is observability: when running CHI
verification or traffic tests through the TLM generator, users can now see
how much traffic was generated and how often the protocol applied
backpressure through retries and credit grants.

### 31. Clarify the transaction-ID label in `TlmGenerator` output
*2026-05-11 · `ea649085d1` · Giacomo Travaglini · PR #3154*

A cosmetic logging fix. When an expectation runs in the TLM CHI generator,
the status line it prints previously began with a bare number, which was
ambiguous to log readers. The change prefixes that number with `txn_id=` so
it is clear the value identifies the transaction. There is no functional or
behavioral change — only improved readability of diagnostic output during
TLM CHI test runs.

### 32. Make `Payload.byte_enable` accessible from Python
*2026-05-15 · `c509beb388` · Giacomo Travaglini · PR #3175*

This extends the pybind11 bindings for the TLM CHI payload type by exposing
its byte-enable mask as a read/write property in Python. Previously
Python-driven CHI transaction tests could set fields such as the address
space and logical-processor ID but had no access to the per-byte enable
mask. With this binding added, such tests can construct and verify
partial/masked write transactions. It is a binding-only change with no
effect on C++ simulation behavior.

---

## Summary of themes

- **CHI TLM verification flow** dominates this period: the `TlmGenerator`
  and `TlmController` gained backpressure/credit handling, statistics,
  simulation-exit logic, functional-access handling, new test primitives,
  and several protocol-correctness fixes (`RespSepData`, separate
  `Comp`/`DBIDResp`, build fixes).
- **CHI protocol features**: a new `lpid` field, missing transaction IDs
  filled in, and the Completer Busy (CBusy) backpressure-signaling
  mechanism.
- **Performance**: a large speedup of `NetDest::resize` through caching.
- **Flexibility and testability**: non-power-of-two cache capacities,
  Python APIs for flushing caches and controllers, named TBE-storage stats.
- **Networking**: per-vnet dedicated links for HeteroGarnet XY routing and
  a SimpleNetwork route-profiling tool.
- **Cleanups and small fixes**: removal of the GPU shadow ROM, removal of a
  shadowed member, atomic-packet conversion fixes, MPAM payload plumbing,
  and routine formatting/build maintenance.
