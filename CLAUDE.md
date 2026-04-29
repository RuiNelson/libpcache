# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) or any other AI Agent when working with code in this repository.

## Source of Truth

`spec.md` is the source of truth for this project. Before changing any implementation, update `spec.md` first to reflect the intended change, then implement accordingly.

## Current State of the Library

The library is not yet ready for production. We can make all the breaking changes we want to the specification/headers/etc. 

## Build and Test

```bash
# Configure (first time or after CMakeLists.txt changes)
cmake -B build

# Build
cmake --build build

# Run all tests
ctest --test-dir build

# Run a single test binary directly (for verbose output)
./build/tests/pcache_test


```

## Repository Layout

```
include/          # public headers installed with the library
  libpcache.h
  libpcache_errors.h
  module.modulemap
src/              # implementation (not installed)
  libpcache.c     # thin top-level entry point / dispatch
  handle.c / .h   # open-handle table
  db.c / .h       # SQLite helpers
  volume.c        # volume lifecycle (create, open, close)
  pages.c         # page read/write/delete operations
  maintenance.c   # defragment, set_max_pages, preallocate
  internal.h      # shared internal types and macros
tests/
  test_pcache.c   # unit tests (tst framework)
```

The library is compiled as a **shared library** (`libpcache.so`, SOVERSION 1).

## Architecture

A volume consists of two files: a **data file** (fixed-size pages laid out sequentially) and a **SQLite index database** that maps page identifiers to byte offsets in the data file. The index has two tables:

- `metadata` — volume configuration (page size, capacity policy, max pages, id size, schema version)
- `pages` — maps `id_hash` (xxHash32) + `id` (opaque blob) to a `ROWID`, where `byte_offset = (ROWID − 1) × page_size`

**Capacity policies:**
- `FIXED` — writes beyond `max_pages` fail; deleted rows are NULLed in place and the next put reuses the lowest NULL slot, located via the `idx_free_slots` partial index over `id_hash IS NULL`
- `FIFO` — writes beyond `max_pages` evict the oldest page; the eviction position is derived on demand as the lowest empty `ROWID` whose predecessor (with wrap-around) is occupied. No cursor table is maintained.

**Lookup path:** `id_hash` (partial index on non-NULL rows) filters candidates; full byte-for-byte `id` comparison confirms the match.

## Public Interface Conventions

- All public symbols are prefixed `pcache_` in snake_case.
- Handles are `int` (`pcache_handle`); `0` means failure, positive means valid.
- Functions never return errors via their return value. Each function takes a `pcache_<fn>_error *error` out-parameter, plus optional `int *sqlite_error` and `int *posix_error` pointers (any may be `NULL`).
- Write operations take a `bool durable` parameter controlling whether `fsync` is awaited.
- Error enum values are assigned explicitly to preserve binary compatibility.
- Error enumerations live in `libpcache_errors.h`, which is transitively included by `libpcache.h`.

## Code Style

- Never write code in headers. Headers contain only declarations, type definitions, `#define` constants and macros — no function bodies, no `static inline` implementations.
- Use meaningful variable names. Avoid single-letter variables except as loop indices (e.g., `i`, `j`, `k`).
- Formatting is enforced by `.clang-format` (LLVM base, 4-space indent, 120-column limit). Run `clang-format -i <file>` before committing. Use `./format.sh` to format all source files at once.

## Testing Approach

**Use TDD.** Write the test first, confirm it fails, then implement. Every new behaviour or bug fix must be covered by a test before the implementation is written or changed.

## Testing Framework

Tests use [tst](https://github.com/rdentato/tst), a single-header C framework fetched automatically by CMake. Structure:

```c
tstsuite("suite name") {
    tstcase("case name") {
        tstcheck(expr, "message");
    }
}
```

## Hash Collisions

**Be very wary of hash collisions.** xxHash32 has a 32-bit output space — collisions are realistic at scale and trivially constructable on purpose. `id_hash` is only a filter; the authoritative comparison is always the full byte-for-byte `id` check. Any code that uses `id_hash` without also performing the full `id` comparison is a correctness bug. Never short-circuit on hash equality alone.

## Dependencies

- **libsqlite3** — system package, dynamically linked
- **xxHash** (v0.8.2) — fetched by CMake via FetchContent, statically linked
- **tst** — fetched by CMake via FetchContent, test-only
