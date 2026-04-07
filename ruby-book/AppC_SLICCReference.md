# Appendix C: SLICC Quick Reference

This appendix provides a compact reference for the SLICC language: syntax, keywords, built-in types, and generated concepts.
It covers state declarations, transitions, actions, message types, TBE usage, and the relationship between `.slicc` manifest files and individual `.sm` state-machine files.
This complements Chapter 7's deep dive by serving as a lookup card readers can keep open while reading or writing protocol code.

## Known Gotchas

| Gotcha | Symptom | Explanation |
|--------|---------|-------------|
| `mandatoryQueue` is hard-coded | CPU requests silently never reach the controller | The `Sequencer` looks up this exact buffer name. Any other name is ignored. |
| SLICC name-mangling (`TBE` becomes `L1Cache_TBE`) | Type not found errors in TBETable declarations | Types inside `machine()` are prefixed with the machine type. Use `template="<L1Cache_TBE>"` syntax. |
| Missing `setMRU()` calls | Replacement policy silently breaks (evicts wrong blocks) | `setMRU()` must be called on every cache hit and fill to update the replacement policy. |
| `dequeue()` is delayed one cycle | Timing issues, messages seem to linger | `dequeue(clockEdge())` takes effect next cycle, not immediately. This enforces minimum processing latency. |
| Missing `MessageSize` field | Runtime panic | Every message type must include a `MessageSizeType` field or the network cannot calculate bandwidth. |
| Error line numbers are off | Build error points to wrong line | SLICC parser detects errors late. Check bracket matching and preceding lines. |

## Functional Access Contract

Every controller must implement these four functions for debugger reads, binary loading (`RubyPortProxy`), and initialization:

```slicc
// Return the access permission for a block (consult TBE for transient states)
AccessPermission getAccessPermission(Addr addr);

// Update the access permission for a block
void setAccessPermission(Addr addr, AccessPermission perm);

// Read data from controller storage into a packet (cache entry or TBE)
bool functionalRead(Addr addr, Packet *pkt);

// Write data from a packet into all matching storage (cache entry AND TBE)
int functionalWrite(Addr addr, Packet *pkt);
```

`AccessPermission` values: `Invalid`, `NotPresent`, `Busy`, `Read_Only`, `Read_Write`.
SLICC auto-generates `*_State_to_permission()` from state declaration annotations.

## Standard Types

### `NetDest` — Sharer/Owner Bitvector

Tracks sets of machines (typically sharers or owner in a directory).

| Operation | Description |
|-----------|-------------|
| `add(MachineID)` | Add a machine to the set |
| `remove(MachineID)` | Remove a machine from the set |
| `clear()` | Remove all machines |
| `addNetDest(NetDest)` | Union with another set |
| `count()` | Number of machines in the set |
| `isElement(MachineID)` | Test membership |

### `TBETable` — Transient Buffer Entries

Stores per-address transient state for in-flight transactions.
Requires SLICC name-mangled type in template parameter:

```slicc
TBETable<L1Cache_TBE> TBEs, template="<L1Cache_TBE>";
```

### `DirectoryMemory` — Directory Entries

Stores per-address directory state.
Uses lazy allocation — entries are created on first access via `getDirectoryEntry(address)`, not pre-allocated for all of physical memory.

## Buffer Idioms

### Sending Messages

```slicc
enqueue(requestToDir, RequestMessage, latency="L1_REQUEST_LATENCY") {
    out_msg.addr := address;
    out_msg.Type := CoherenceRequestType:GetS;
    out_msg.Destination := mapAddressToMachine(address, MachineType:Directory);
    out_msg.Size := MessageSizeType:Request_Control;
    out_msg.Requestor := machineID;
}
```

### Receiving Messages

```slicc
in_port(responseFromDir, ResponseMessage, responseFromDirOrSibling) {
    if (responseFromDirOrSibling.isReady(clockEdge())) {
        peek(responseFromDirOrSibling, ResponseMessage) {
            // in_msg is available here
            // ... handle message ...
        }
        dequeue(responseFromDirOrSibling, clockEdge());
    }
}
```

### Buffer Management

```slicc
stall;                    // Block this and all behind it (head-of-line blocking)
recycle;                  // Move to queue tail with latency (avoids HOL, wastes BW)
stall_and_wait(address);  // Address-tagged stall (best selectivity)
wakeUpBuffers(address);   // Resume stalled messages for this address
```

## Debugging Idioms

### Runtime Trace Output

```slicc
DPRINTF(RubySlicc, "Controller %s received request for addr %#x in state %s\n",
        machineID, address, state);
```

Enable with `--debug-flags=RubySlicc` on the gem5 command line.

### Transition Annotation

```slicc
APPEND_TRANSITION_COMMENT("acks remaining: ");
APPEND_TRANSITION_COMMENT(tbe.pendingAcks);
```

Attaches text to the transition record for protocol trace analysis.

### SLICC HTML Documentation

Generate navigable protocol documentation at build time with `SLICC_HTML=y`.
Produces per-controller pages showing all states, events, transitions, and actions.
See Appendix A for build instructions.
