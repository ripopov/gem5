# Lesson 9: C++ Serialization and Checkpointing

This lesson covers gem5's serialization infrastructure for saving and
restoring simulation state.

The goal is to make four ideas concrete:

1. `Serializable` provides `serialize()` / `unserialize()` hooks that
   persist object state to checkpoint files.
2. Serialization macros (`SERIALIZE_SCALAR`, `UNSERIALIZE_SCALAR`, etc.)
   handle common data types automatically.
3. The drain protocol (from Lesson 2) and serialization work together:
   objects must drain before checkpointing to ensure consistent state.
4. Checkpoint files are human-readable INI sections, one per SimObject.

## Status

Placeholder. Code examples will be authored in a later change.

## Planned scope

1. Implementing `serialize()` and `unserialize()` on a SimObject.
2. Using `SERIALIZE_SCALAR`, `SERIALIZE_ARRAY`, `SERIALIZE_CONTAINER`,
   and their `UNSERIALIZE_*` counterparts.
3. Handling optional fields with `optParamIn()`.
4. Sectioned serialization with `ScopedCheckpointSection`.
5. Checkpoint creation and restore flow end-to-end.
6. Relationship between drain, serialize, and resume.

## Key gem5 classes

### Serializable (`src/sim/serialize.hh`)

- `Serializable`: mixin interface providing `serialize()` / `unserialize()`.
- `CheckpointOut`: output stream wrapper for writing checkpoint sections.
- `CheckpointIn`: input wrapper for reading checkpoint data.
- `ScopedCheckpointSection`: RAII helper for nested INI sections.

### Serialization macros (`src/sim/serialize.hh`)

- `SERIALIZE_SCALAR(name)` / `UNSERIALIZE_SCALAR(name)`: single values.
- `SERIALIZE_ARRAY(name, size)` / `UNSERIALIZE_ARRAY(name, size)`: arrays.
- `SERIALIZE_CONTAINER(name)` / `UNSERIALIZE_CONTAINER(name)`: STL containers.
- `optParamIn(cp, name, var)`: reads a value if present, keeps default
  otherwise (useful for backward compatibility).

## Why this lesson matters for later lessons

Checkpointing is essential for fast-forwarding past boot sequences,
sampling methodologies, and reproducible experiments. Any stateful
component must implement serialization correctly.
