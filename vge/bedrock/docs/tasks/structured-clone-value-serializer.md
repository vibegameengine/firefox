# Task: structured clone on V8's ValueSerializer

Status: open, deferred by the user on 2026-11-17.

Why: after commit c932ec056f our structured clone of 20000 small objects with typed arrays takes
214-241 ms to write in a worker and about 120 ms to read on the main thread; Chrome takes about 20
and 20. What is left is per-object overhead of the public V8 API (`GetOwnPropertyNames`,
`HasOwnProperty`, `Get`, identity hash for back references) with no single hot spot. Chrome does
the same walk inside V8 (`v8::ValueSerializer` / `ValueDeserializer`) without that overhead.

What it takes:

- Write and read the object graph with `v8::ValueSerializer` and `ValueDeserializer`, with DOM
  objects (Blob, ImageBitmap, MessagePort, transferables, SharedArrayBuffer) through the host
  object delegate, keeping Gecko's `JSStructuredCloneCallbacks` contract for the callers.
- The buffer format changes. Data our own runtime stored in IndexedDB in the old format must
  still be readable (version tag in the header and the old reader kept for old data), or be
  declared disposable.

Test bed: `C:/Projects/gecko-build-logs/bench/jsweb/clone.html` (timing) and `clonecheck.html`
(round trip of every typed array kind, shared buffers, DataView, Map, Set, Date, RegExp, BigInt,
back references).
