# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Source of Truth

`project.md` is the source of truth for this project. Before changing any implementation, update `project.md` first to reflect the intended change, then implement accordingly.

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

## Architecture

A volume consists of two files: a **data file** (fixed-size pages laid out sequentially) and a **SQLite index database** that maps page identifiers to byte offsets in the data file. The index has three tables:

- `metadata` — volume configuration (page size, capacity policy, max pages, id size, schema version)
- `pages` — maps `id_hash` (xxHash32) + `id` (opaque blob) to a `ROWID`, where `byte_offset = (ROWID − 1) × page_size`
- `fifo_cursor` — single-row table with `next_rowid`; only present on FIFO volumes

**Capacity policies:**
- `FIXED` — writes beyond `max_pages` fail; deleted rows go to an in-memory free list for O(1) reuse
- `FIFO` — writes beyond `max_pages` evict the oldest page via a circular cursor; deleted rows are *not* added to the free list and are reused only when the cursor naturally reaches them

**Lookup path:** `id_hash` (partial index on non-NULL rows) filters candidates; full byte-for-byte `id` comparison confirms the match.

## Public Interface Conventions

- All public symbols are prefixed `pcache_` in snake_case.
- Handles are `int` (`pcache_handle`); `0` means failure, positive means valid.
- Functions never return errors via their return value. Each function takes a `pcache_<fn>_error *error` out-parameter, plus optional `int *sqlite_error` and `int *posix_error` pointers (any may be `NULL`).
- Write operations take a `bool durable` parameter controlling whether `fsync` is awaited.
- Error enum values are assigned explicitly to preserve binary compatibility.

## Code Style

Formatting is enforced by `.clang-format` (LLVM base, 4-space indent, 120-column limit). Run `clang-format -i <file>` before committing.

## Testing Framework

Tests use [tst](https://github.com/rdentato/tst), a single-header C framework fetched automatically by CMake. Structure:

```c
tstsuite("suite name") {
    tstcase("case name") {
        tstcheck(expr, "message");
    }
}
```

## Dependencies

- **libsqlite3** — system package, dynamically linked
- **xxHash** (v0.8.2) — fetched by CMake via FetchContent, statically linked
- **tst** — fetched by CMake via FetchContent, test-only
