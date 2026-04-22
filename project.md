# libpcache

`libpcache` (persistent cache) is a C library that provides persistent, paged, random-access storage indexed by key. It is designed primarily as a building block for **persistent caches**, although it is suitable for any workload that needs to store and retrieve opaque, fixed-size data pages identified by a unique key.

## Features

- **Random access by key** — Each page is identified by an opaque value supplied by the caller and can be read or written independently of the others. The library treats the identifier as a byte sequence, attributing no structure or meaning to it.
- **Two capacity policies** — Selected at file creation time:
    - **Fixed**: writes that exceed the configured maximum page count fail with an error.
    - **FIFO**: writes that exceed the configured maximum page count evict the least recently inserted page.
- **Configurable page size** — Fixed per file at creation time.
- **Opaque content** — The library does not interpret the bytes of a page; it merely stores and returns them verbatim.
- **Thread-safe** — All public operations are blocking and internally synchronized; a single *handle* may be shared across multiple *threads* without external locking. The caller is responsible only for ensuring the logical ordering of operations (for example, not reading from a handle before it has been opened).
- **Per-operation durability** — Writes are always committed atomically. The caller decides, on a per-operation basis, whether to wait for `fsync` (ensuring the committed state survives a system crash) or to return as soon as the write has been handed off to the operating system.
- **SQLite-based index** — The mapping from identifiers to locations in the data file is maintained in a SQLite database, offering a portable, inspectable, and well-understood index format.


## Storage Model

A storage instance consists of two files:

- A **data file**, containing fixed-size pages laid out sequentially.
- An **index database** (SQLite), which associates each identifier with the *byte offset* of its page within the data file.

This pair of files is referred to as a *volume*. Any data present in the data file but not referenced by the index database is treated as garbage.

## Access Orientation

The library is primarily designed for *write-once, read-many* (WORM) workloads, in which data is written exactly once and subsequently read with high frequency.

## Index Database Format

The index is represented by a SQLite database comprising two tables with clearly delineated responsibilities:

- **`metadata`** — Stores the volume's configuration parameters as key–value pairs.
- **`pages`** — Maintains the mapping between page identifiers and their respective locations in the data file.

### Table `metadata`

The `metadata` table has two columns: `key` (`TEXT`), which names the parameter, and `value` (`BLOB`), which stores the value opaquely, preserving independence from the underlying data type.

This table is fully populated at volume creation time and carries all the configuration information required for the library to reopen an existing volume without the caller having to supply the original parameters again.

The mandatory entries are as follows:

| Key               | Type   | Description                                                                                                                 |
| ----------------- | ------ | --------------------------------------------------------------------------------------------------------------------------- |
| `version`         | UInt32 | Schema version of the database. The current value is `1`; it will be incremented in future revisions that alter the schema. |
| `capacity_policy` | String | Capacity policy of the volume: `FIXED` or `FIFO`.                                                                           |
| `page_size`       | UInt32 | Size of each page, in bytes.                                                                                                |
| `max_pages`       | UInt32 | Maximum capacity of the volume, expressed in number of pages.                                                               |
| `id_size`         | UInt32 | Size of the page identifier, in bytes (matches the length of the `pages.id` column).                                        |

**Encoding:** numeric values are stored in *little-endian* format; strings are encoded in ASCII with a null terminator.

**Effective capacity:** although the `UInt32` type theoretically bounds page size at 4 GiB and page count at approximately 4.3 × 10⁹, typical values lie in the KiB range per page. By way of illustration, with 8 KiB pages and 4 × 10⁹ pages, a volume's maximum capacity reaches 32 TiB.

### Table `pages`

The `pages` table is structured so that every row has a fixed length — a requirement that improves access locality and simplifies the computation of offsets within the data file.

#### Column `ROWID`

SQLite's intrinsic `ROWID` doubles as the logical page index. The physical byte offset of a page within the data file is given by:

```
byte_offset = (ROWID − 1) × page_size
```

For example, with a page size of 16 KiB (16 384 bytes), the page with `ROWID = 50` occupies the byte range [802 816, 819 199] of the data file.

#### Column `id_hash` (`INTEGER`, 4 bytes, nullable)

A 32-bit hash of the page identifier, computed using the xxHash32 algorithm. It serves as a first-stage filter during identifier lookups (see section **Indexes**), significantly reducing the number of byte-by-byte comparisons required.

#### Column `id` (`BLOB`, fixed length, nullable)

Opaque page identifier, whose length is defined by `metadata.id_size`. It is supplied by the caller on write and must be presented again in any subsequent read operation. The identifier is typically a cryptographic digest (for example, a SHA-256 hash truncated to `id_size` bytes), which makes collisions negligible in practice.

Uniqueness is not enforced through a database constraint, thereby avoiding the verification cost associated with a `UNIQUE` constraint on every insert. The `check_id_uniqueness` parameter of `pcache_put_page` offers an opt-in per-call check performed in code; see that function for details. If duplicates are nonetheless present — a condition that represents a caller error — `pcache_get_page` returns the contents of whichever matching row the database retrieves first; the result is therefore non-deterministic.

#### Indexes

A single partial index is defined over the `pages` table:

```sql
CREATE INDEX idx_lookup ON pages(id_hash) WHERE id_hash IS NOT NULL;
```

By excluding rows where `id_hash IS NULL`, the index contains only live pages and remains compact regardless of how many recyclings have accumulated over the volume's lifetime. The `id` column is not indexed: the `id_hash` pre-filter is sufficient.

#### Row Recycling

When a page is deleted, the `id_hash` and `id` columns are set to `NULL`. The subsequent handling of that row differs by capacity policy.

**On `FIXED` volumes**, deleted rows are added to an in-memory *free list*. Since the volume has a single concurrent writer, allocating a slot reduces to a `pop` on that vector and freeing a slot to a `push`, with no interaction with the index required to locate the free position. The free list may be preloaded at open time (see `pcache_open`) or populated lazily as slots are returned.

**On `FIFO` volumes**, deleted rows are *not* added to the free list. The row is left in place with NULL identifiers and will be naturally overwritten when the `fifo_cursor` reaches it during a subsequent insert. This preserves strict FIFO eviction order: the eviction sequence is always determined by `ROWID`, regardless of whether a slot was explicitly deleted or organically evicted. The trade-off is that a deleted slot is not immediately reusable; it becomes available at most one full rotation of the cursor later.

### Table `fifo_cursor`

Exclusive to volumes with `capacity_policy = FIFO`, this table materializes the "eviction needle": the `ROWID` of the next slot to be overwritten on insert, whether that slot holds a live page (eviction) or a deleted page (natural reuse in insertion order).

The table contains a single row, with a single column:

| Column       | Type      | Description                                                       |
| ------------ | --------- | ----------------------------------------------------------------- |
| `next_rowid` | `INTEGER` | `ROWID` of the page that is the next candidate for FIFO eviction. |

The semantics are strictly circular: after each replacement, the value is updated to `(next_rowid % max_pages) + 1`, wrapping back to `1` upon reaching `max_pages`. Because the initial inserts populate the `ROWID`s sequentially from `1` to `max_pages`, the replacement order naturally coincides with the insertion order, yielding FIFO semantics without requiring per-row timestamps.

The update to `next_rowid` is bundled into the same SQLite transaction that performs the page replacement, ensuring atomicity between the needle's advance and the write of the new contents into the index.

On `FIXED` volumes, this table is not created, avoiding superfluous state.

## Public Interface

The interface is exposed through a single primary header (`libpcache.h`), which gathers all structures, enumerations, and function declarations available to the caller. Error-code enumerations, owing to their semantic autonomy and the size they tend to take on, reside in a dedicated secondary header (`libpcache_errors.h`), transitively included by the primary. All public identifiers follow the *snake_case* convention and are prefixed with `pcache_`, preventing collisions with symbols from other libraries sharing the same global namespace.

### General Conventions

**Descriptor model.** Each open volume is identified by a positive integer, analogous to a POSIX file descriptor. The value zero is reserved to signal failure at open time; any strictly positive value represents a valid descriptor, which must be supplied in all subsequent operations on the volume. There is no fixed limit on the number of simultaneously open handles; the internal table grows dynamically as needed.

**Error propagation.** Public functions do not use their return value to signal operation-level failure. Instead, they accept as a final argument a pointer to a function-specific error enumeration, named `pcache_<function>_error`, into which the outcome of the operation is recorded. Each function therefore exposes only the error conditions it can actually produce, avoiding the conflation of unrelated failure modes into a single catch-all type. When a function may fail due to errors originating in underlying subsystems — typically the SQLite engine or POSIX system calls —, additional pointers are provided, one per possible error source, allowing the caller to inspect the original error code without ambiguity. Any of these pointers may be `NULL` if the caller does not wish to collect the corresponding information.

**Error enumerations.** The values of each `pcache_<function>_error` enumeration are assigned explicitly, so as to preserve binary compatibility across versions. They adopt descriptive English names, avoiding POSIX-inherited abbreviations (for instance, `PCACHE_OPEN_NO_SUCH_DEVICE_OR_ADDRESS` is used in place of `PCACHE_OPEN_ENXIO`). All such enumerations are declared together in `libpcache_errors.h`.

**Per-operation durability.** Functions that modify the volume accept a boolean `durable` parameter that controls, at the call level, whether the operation waits for `fsync` to complete before returning. When true, it guarantees that the committed state survives a sudden system failure; when false, the call returns as soon as the write has been handed off to the operating system.

### Types and Structures

| Type                     | Description                                                                                                                                                                                                                                                   |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `pcache_handle`          | Alias for `int`. Opaque descriptor associated with an open volume.                                                                                                                                                                                            |
| `pcache_capacity_policy` | Enumeration with the values `PCACHE_CAPACITY_FIXED` and `PCACHE_CAPACITY_FIFO`.                                                                                                                                                                               |
| `pcache_file_pair`       | Pair of pointers to null-terminated UTF-8 strings, corresponding respectively to the path of the index database and the path of the data file.                                                                                                                |
| `pcache_configuration`   | Structure that encapsulates the volume's parameters: capacity policy, page size, maximum capacity in pages, and identifier size.                                                                                                                              |
| `pcache_progress_fn`     | Functor type used in long-running operations. It receives the fraction of work completed (a value in the interval [0, 1]) and an opaque pointer to caller-defined context; it returns `true` to let the operation proceed or `false` to request cancellation. |

Formally:

```c
typedef int pcache_handle;

typedef enum pcache_capacity_policy {
    PCACHE_CAPACITY_FIXED = 0,
    PCACHE_CAPACITY_FIFO  = 1
} pcache_capacity_policy;

typedef struct pcache_file_pair {
    const char *database_path;
    const char *data_path;
} pcache_file_pair;

typedef struct pcache_configuration {
    pcache_capacity_policy capacity_policy;
    uint32_t               page_size;
    uint32_t               max_pages;
    uint32_t               id_size;
} pcache_configuration;

typedef bool (*pcache_progress_fn)(double progress, void *user_data);
```

### Volume Lifecycle

**Creation (`pcache_create`).** Creates on the filesystem the two artifacts that compose a volume, initializes the index database schema, and populates the `metadata` table with the supplied parameters. Fails if either of the two files already exists, ensuring that no existing volume is inadvertently overwritten. The boolean arguments `preallocate_database` and `preallocate_datafile` independently control preallocation of the database (eager insertion of `max_pages` rows with null identifiers) and of the data file (writing blocks of zeros up to the final size).

```c
void pcache_create(
    const pcache_file_pair     *paths,
    const pcache_configuration *config,
    bool                        preallocate_database,
    bool                        preallocate_datafile,
    pcache_create_error        *error,
    int                        *sqlite_error,
    int                        *posix_error
);
```

**Opening (`pcache_open`).** Opens a previously created volume, queries the `metadata` table to retrieve its configuration, and returns a positive integer descriptor. On error, returns zero and records the reason in the supplied error pointers.

When `preload_free_list` is `true`, the library executes a single query at open time to load all recyclable `ROWID`s into an in-memory free list, enabling O(1) slot allocation on subsequent inserts. When `false`, recyclable slots are located on demand by querying the index database, trading insertion speed for a lower memory footprint at open time — useful for large preallocated volumes where the free list would otherwise occupy significant memory.

```c
pcache_handle pcache_open(
    const pcache_file_pair *paths,
    bool                    preload_free_list,
    pcache_open_error      *error,
    int                    *sqlite_error,
    int                    *posix_error
);
```

**Closing (`pcache_close`).** Releases the resources associated with a descriptor, orderly closing the connection to the index database and any underlying file descriptors.

```c
void pcache_close(
    pcache_handle       handle,
    pcache_close_error *error,
    int                *sqlite_error,
    int                *posix_error
);
```

### Introspection

**`pcache_get_configuration`.** Returns the volume's parameters, obtained from the `metadata` table. It allows the caller to inspect the configuration of a reopened volume without keeping a local copy. On error, the contents of the returned structure are unspecified and the caller must rely on the error output to determine whether the result is meaningful.

```c
pcache_configuration pcache_get_configuration(
    pcache_handle                    handle,
    pcache_get_configuration_error  *error,
    int                             *sqlite_error
);
```

### Page Operations

**`pcache_put_page`.** Stores the contents of a page identified by the supplied `id`. The input buffer must contain at least `page_size` bytes; the identifier must be exactly `id_size` bytes long. On volumes with FIFO policy, an insert that exceeds the capacity triggers eviction of the oldest page, following the procedure described in the section [Table `fifo_cursor`](#table-fifo_cursor).

When `check_id_uniqueness` is `true`, the library verifies in code that no page with the same identifier already exists before proceeding; if a duplicate is found, the operation fails with a dedicated error code. When `false`, no such check is performed and the caller assumes responsibility for identifier uniqueness.

```c
void pcache_put_page(
    pcache_handle           handle,
    const void             *id,
    const void             *page_data,
    bool                    check_id_uniqueness,
    bool                    durable,
    pcache_put_page_error  *error,
    int                    *sqlite_error,
    int                    *posix_error
);
```

**`pcache_get_page`.** Retrieves the page associated with the supplied `id`, copying its contents into the buffer provided by the caller, which must have at least `page_size` bytes available.

```c
void pcache_get_page(
    pcache_handle           handle,
    const void             *id,
    void                   *page_buffer,
    pcache_get_page_error  *error,
    int                    *sqlite_error,
    int                    *posix_error
);
```

**`pcache_check_page`.** Returns `true` if a page with the supplied `id` exists in the volume, or `false` otherwise. The operation is serviced entirely against the index database, without touching the data file. On error, the returned value is unspecified and the caller must consult the error output.

```c
bool pcache_check_page(
    pcache_handle             handle,
    const void               *id,
    pcache_check_page_error  *error,
    int                      *sqlite_error
);
```

**`pcache_delete_page`.** Marks a page as deleted in the index, recycling its row. When the `wipe_data_file` parameter is true, the corresponding region of the data file is additionally overwritten with zeros, ensuring the contents can no longer be recovered by direct inspection of the file; otherwise, only the index is updated and the old bytes remain in the data file until they are eventually reused.

```c
void pcache_delete_page(
    pcache_handle              handle,
    const void                *id,
    bool                       wipe_data_file,
    bool                       durable,
    pcache_delete_page_error  *error,
    int                       *sqlite_error,
    int                       *posix_error
);
```

### Maintenance

**`pcache_defragment`.** Relocates live pages contiguously toward the beginning of the data file, eliminating the gaps left by recycled rows. It accepts a progress *callback* that allows the caller to monitor and, if desired, cancel the operation. When the `shrink_file` argument is true, the data file is truncated to its minimum size after the relocation. If the callback cancels the operation, the volume remains in a consistent state: pages processed up to the cancellation point have been relocated and the index updated accordingly; unprocessed pages remain in their original positions.

```c
void pcache_defragment(
    pcache_handle             handle,
    pcache_progress_fn        progress_callback,
    void                     *progress_user_data,
    bool                      shrink_file,
    bool                      durable,
    pcache_defragment_error  *error,
    int                      *sqlite_error,
    int                      *posix_error
);
```

**`pcache_set_max_pages`.** Adjusts the volume's maximum capacity, either by growth or by reduction. On `FIXED` volumes, the operation fails if the requested reduction would require discarding live pages; on `FIFO` volumes, the reduction silently discards the oldest pages until the new limit is reached, which may involve evicting some pages that are younger than others when the cursor does not align with the oldest live page.

```c
void pcache_set_max_pages(
    pcache_handle                handle,
    uint32_t                     new_max_pages,
    bool                         durable,
    pcache_set_max_pages_error  *error,
    int                         *sqlite_error,
    int                         *posix_error
);
```

**`pcache_preallocate`.** Preallocates space in an already-existing volume, with independent boolean arguments for the database and for the data file. It is intended for scenarios in which the caller chooses to defer preallocation or has just increased the maximum capacity and wishes to materialize the additional space.

```c
void pcache_preallocate(
    pcache_handle             handle,
    bool                      preallocate_database,
    bool                      preallocate_datafile,
    bool                      durable,
    pcache_preallocate_error *error,
    int                      *sqlite_error,
    int                      *posix_error
);
```

## Language and Toolchain

- **Language standard:** C17 (ISO/IEC 9899:2018).
- **Compiler:** clang.
- **Build system:** CMake.

## Dependencies

- **libsqlite3** — dynamically linked. Used for the index database.
- **xxHash** — statically linked. Used for computing the 32-bit page identifier hash filter.

## Supported Platforms

- macOS and Linux.

## Future Work

- Windows support.
- Additional replacement policies, notably those based on popularity counters (*least frequently used*).

## License

Distributed under the MIT license. See [LICENSE](LICENSE).
