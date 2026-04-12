# Current FTR Issues

## Issue: `ftr_parser` byte-slice loader rejects gem5 compressed FTR files

### Status

- **Observed on:** `m5out/trivial-ftr-20260412-233807/trivial.ftr`
- **Producer:** gem5 in-tree binary FTR writer used by the Chapter 17 final
  project flow
- **External reader:** `ftr_parser` crate `0.3.0`
- **Severity:** medium
- **Impact:** valid gem5-generated FTR files cannot currently be consumed
  through `parse_ftr_from_bytes(...)`

### Symptom

The file `m5out/trivial-ftr-20260412-233807/trivial.ftr` is structurally valid:

- it starts with the expected self-described CBOR tag `d9 d9 f7`
- `file(1)` identifies it as a CBOR container
- `ftr_parser::parse_ftr(path)` succeeds

However, `ftr_parser::parse_ftr_from_bytes(bytes)` fails with:

```text
Incorrect major type!
```

The metadata parsed through the file-backed path looks sane:

- timescale: `Ns`
- streams: `2`
- generators: `8`
- relations: `2532156`

This strongly suggests the file is not corrupted.
The failure is in the loader path.

### Root Cause

gem5 currently writes binary FTR using `ftr::ftr_writer<true>`, which enables
compressed chunks for all non-info sections.

That is the correct behavior for the current writer implementation.

The problem is on the `ftr_parser` side.
In crate version `0.3.0`, the compressed transaction-block path in
`src/ftr_parser.rs` reads the compressed byte string and passes it directly into
the CBOR transaction-block parser without first decompressing it.

As a result, the parser tries to interpret LZ4-compressed bytes as CBOR and
fails with `Incorrect major type!`.

In other words:

- `parse_ftr(path)` works because it defers transaction loading and can later
  use the dedicated file-backed `load_stream_into_memory(...)` path
- `parse_ftr_from_bytes(bytes)` fails because compressed transaction blocks are
  parsed as if they were already uncompressed

### Scope

This issue affects byte-slice loading of compressed FTR files.
It does **not** currently indicate that gem5 is producing malformed FTR output.

At the time of writing, the concrete confirmed failure is:

- `parse_ftr_from_bytes(...)` on gem5-generated compressed FTR traces

The file-backed metadata parse is confirmed working.
Full transaction loading on the trivial trace was not exhaustively validated in
this note, so performance or additional reader-side issues may still exist and
should be tracked separately if observed.

### Current Workaround

Use the file-backed parser entry point:

```rust
let mut ftr = ftr_parser::parse::parse_ftr(path)?;
ftr.load_stream_into_memory(StreamId(1))?;
```

Avoid `parse_ftr_from_bytes(...)` for gem5-generated compressed FTR files until
the reader is fixed.

### Suggested Follow-Up

Patch `ftr_parser` so the compressed transaction-block path mirrors its handling
of other compressed chunk types:

1. read the stored uncompressed size
2. read the compressed byte string
3. decompress with LZ4
4. pass the decompressed buffer to the CBOR transaction-block parser

After that change, rerun the same `trivial.ftr` validation through both:

- `parse_ftr(path)`
- `parse_ftr_from_bytes(bytes)`

Both paths should produce consistent metadata and successful stream loading.
