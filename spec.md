# libpcache

`libpcache` (persistent cache) is a C library that provides persistent, paged, random-access storage indexed by key. It is designed primarily as a building block for **persistent caches**, although it is suitable for any workload that needs to store and retrieve opaque, fixed-size data pages identified by a unique key.

## Design Goals

The library is designed for **memory-constrained devices**, with targets as low as 128 MB of RAM. All operations avoid unnecessary memory allocations: the runtime per-volume memory footprint is bounded by the prepared SQLite statements and a few small fields, regardless of how many recyclable slots accumulate. Slot reuse on `FIXED` volumes is handled exclusively through SQLite, supported by a dedicated partial index over `id_hash IS NULL` (see [Indexes](#indexes)).

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

## Internal Organization

Shared helper routines used by page writes, deletes, and with-counter generation live in `src/pages_util.c` / `src/pages_util.h`. This includes batch identifier validation and low-level page-file helper logic; public behavior is unchanged by this internal split.

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

Uniqueness is not enforced through a database constraint, thereby avoiding the verification cost associated with a `UNIQUE` constraint on every insert. The `fail_if_exists` parameter of `pcache_put_page` offers an opt-in per-call check performed in code; see that function for details. If duplicates are nonetheless present — a condition that represents a caller error — `pcache_get_page` returns the contents of whichever matching row the database retrieves first; the result is therefore non-deterministic.

#### Indexes

Two partial indexes are defined over the `pages` table:

```sql
CREATE INDEX idx_lookup      ON pages(id_hash) WHERE id_hash IS NOT NULL;
CREATE INDEX idx_free_slots  ON pages(id_hash) WHERE id_hash IS NULL;
```

`idx_lookup` accelerates identifier resolution: it indexes only live pages, providing a compact first-stage filter for lookups by `id_hash`. The full byte-by-byte comparison on `id` then resolves any hash collisions.

`idx_free_slots` accelerates free-slot acquisition on `FIXED` volumes. Because the predicate constrains every entry to share the same `id_hash` value (`NULL`), the index is effectively ordered by `ROWID`, allowing the next free slot to be located with a bounded range scan of the form `WHERE id_hash IS NULL AND rowid > ? ORDER BY rowid LIMIT 1`. Both indexes remain compact: each one only stores entries for the rows it concerns, never the whole table.

The `id` column is not indexed: the `id_hash` pre-filter is sufficient.

#### Row Recycling

When a page is deleted, the `id_hash` and `id` columns are set to `NULL`. The subsequent handling of that row differs by capacity policy.

**On `FIXED` volumes**, the next free slot is located by querying `idx_free_slots` for the lowest `ROWID` whose `id_hash IS NULL`. No in-memory bookkeeping is maintained: the index is the single source of truth for free slots. When several slots are needed within a single batch, queries are issued with a strictly increasing `rowid >` lower bound so each call returns a distinct slot.

**On `FIFO` volumes**, deleted rows are left in place with `NULL` identifiers. Once the volume has reached the steady state described in [FIFO Eviction Algorithm](#fifo-eviction-algorithm), the implicit cursor advances slot-by-slot through the table, so a deleted slot is reused only when the cursor naturally reaches it — at most one full rotation later. Eviction order is therefore always determined by `ROWID`, regardless of whether a slot became free through explicit deletion or organic eviction.

## FIFO Eviction Algorithm

`FIFO` volumes do not maintain an explicit cursor table. The eviction position is derived on demand from the contents of the `pages` table: it is encoded by the position of the unique empty slot relative to its occupied neighbours.

### Invariant

Once a `FIFO` volume has been written to at least `max_pages` times, it holds *exactly* `max_pages − 1` live pages — one slot is always empty and identifies the next write target. During the brief window inside a put transaction, the volume transiently holds `max_pages − 2` live pages (between the eviction of the slot ahead of the cursor and the write of the cursor slot itself), but this state is never observable outside the transaction.

### Cursor Location

The next slot to be written is the lowest `ROWID` whose `id_hash IS NULL` and whose predecessor (with wrap-around, so the predecessor of `1` is `max_pages`) has `id_hash IS NOT NULL`. If the volume contains no occupied rows at all, the cursor is slot `1`.

This rule is unambiguous in every reachable state — fill-up, steady state, and the post-crash situation in which two adjacent slots are both empty — and produces the same answer that an explicit cursor would have stored.

### Write Procedure

- **Fill-up phase** (the empty region forms a contiguous tail at high `ROWID`s, i.e., the volume holds fewer than `max_pages` live pages): the put writes the new row at slot `M + 1`, where `M` is the current live count. If `M + 1 = max_pages`, the same transaction additionally nulls out slot `1`, transitioning the volume to steady state.
- **Steady state**: within a single SQLite transaction the put nulls out the slot immediately after the cursor (`(cursor mod max_pages) + 1`) and writes the cursor slot. After commit, the cursor has effectively advanced by one position.

Both operations are bundled into one transaction so that no intermediate state is observable to concurrent readers and a crash leaves the volume in a recoverable form.

### Auto-Recovery on Open

A correctly shut-down `FIFO` volume always has at least one empty slot once the fill-up phase has ended. A volume found with all `max_pages` slots occupied at open time can therefore only result from a crash between the write of slot `max_pages` and the eviction of slot `1` (the wrap-around transition). The library restores the invariant during `pcache_open` by nulling out slot `1`. The same rule is applied after `pcache_set_max_pages` reductions when they happen to leave the volume fully occupied.

### Duplicate Detection

When `fail_if_exists` is `true` on a put, the standard `idx_lookup` query is executed against the new identifier. If any live row matches, the put fails with `PCACHE_PUT_DUPLICATE_ID`, regardless of whether the matching row would have been the one evicted by this put. No further check is performed when the lookup returns no match.

## Public Interface

The interface is exposed through a single primary header (`libpcache.h`), which gathers all structures, enumerations, and function declarations available to the caller. Error-code enumerations, owing to their semantic autonomy and the size they tend to take on, reside in a dedicated secondary header (`libpcache_errors.h`), transitively included by the primary. All public identifiers follow the *snake_case* convention and are prefixed with `pcache_`, preventing collisions with symbols from other libraries sharing the same global namespace.

### General Conventions

**Descriptor model.** Each open volume is identified by a positive integer, analogous to a POSIX file descriptor. The value zero is reserved to signal failure at open time; any strictly positive value represents a valid descriptor, which must be supplied in all subsequent operations on the volume. There is no fixed limit on the number of simultaneously open handles; the internal table grows dynamically as needed.

**Error propagation.** Public functions do not use their return value to signal operation-level failure. Instead, they accept as a final argument a pointer to a function-specific error enumeration, named `pcache_<function>_error`, into which the outcome of the operation is recorded. Each function therefore exposes only the error conditions it can actually produce, avoiding the conflation of unrelated failure modes into a single catch-all type. When a function may fail due to errors originating in underlying subsystems — typically the SQLite engine or POSIX system calls —, additional pointers are provided, one per possible error source, allowing the caller to inspect the original error code without ambiguity. Any of these pointers may be `NULL` if the caller does not wish to collect the corresponding information.

**Error enumerations.** The values of each `pcache_<function>_error` enumeration are assigned explicitly, so that numeric codes remain stable and can be compared or logged without ambiguity. They adopt descriptive English names, avoiding POSIX-inherited abbreviations (for instance, `PCACHE_OPEN_NO_SUCH_DEVICE_OR_ADDRESS` is used in place of `PCACHE_OPEN_ENXIO`). All such enumerations are declared together in `libpcache_errors.h`.

**Per-operation durability.** Functions that modify the volume accept a boolean `durable` parameter that controls, at the call level, whether the operation waits for `fsync` to complete before returning. When true, it guarantees that the committed state survives a sudden system failure; when false, the call returns as soon as the write has been handed off to the operating system.

### Types and Structures

| Type                     | Description                                                                                                                                                                                                                                                   |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `pcache_handle`          | Alias for `int`. Opaque descriptor associated with an open volume.                                                                                                                                                                                            |
| `pcache_capacity_policy` | Enumeration with the values `PCACHE_CAPACITY_FIXED` and `PCACHE_CAPACITY_FIFO`.                                                                                                                                                                               |
| `pcache_file_pair`       | Pair of pointers to null-terminated UTF-8 strings, corresponding respectively to the path of the index database and the path of the data file.                                                                                                                |
| `pcache_configuration`   | Structure that encapsulates the volume's parameters: capacity policy, page size, maximum capacity in pages, and identifier size.                                                                                                                              |
| `pcache_page_count`      | Structure containing `used` and `free` page counts.                                                                                                                                                                                                                           |
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

typedef struct pcache_page_count {
    uint32_t used;
    uint32_t free;
} pcache_page_count;

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

```c
pcache_handle pcache_open(
    const pcache_file_pair *paths,
    pcache_open_error      *error,
    int                    *sqlite_error,
    int                    *posix_error
);
```

**Closing (`pcache_close`).** Releases the resources associated with a descriptor, orderly closing the connection to the index database and any underlying file descriptors. Both the data file and the SQLite database are fsync'd before the handles are closed, ensuring all data is persisted to stable storage.

```c
void pcache_close(
    pcache_handle       handle,
    pcache_close_error *error,
    int                *sqlite_error,
    int                *posix_error
);
```

### Introspection -- Configuration

**`pcache_inspect_configuration`.** Returns the volume's parameters, obtained from the `metadata` table. It allows the caller to inspect the configuration of a reopened volume without keeping a local copy. On error, the contents of the returned structure are unspecified and the caller must rely on the error output to determine whether the result is meaningful.

```c
pcache_configuration pcache_inspect_configuration(
    pcache_handle                       handle,
    pcache_inspect_configuration_error *error
);
```

### Introspection -- Storage

**`pcache_inspect_page_count`.** Returns the number of used and free pages in the volume. The `used` field contains the count of pages currently stored, and the `free` field contains the count of available slots. On error, the contents of the returned structure are unspecified and the caller must rely on the error output to determine whether the result is meaningful.

```c
pcache_page_count pcache_inspect_page_count(
    pcache_handle                    handle,
    pcache_inspect_page_count_error *error,
    int                             *sqlite_error
);
```

### Page Operations

**`pcache_put_page`.** Stores the contents of a page identified by the supplied `id`. The input buffer must contain at least `page_size` bytes; the identifier must be exactly `id_size` bytes long. On volumes with FIFO policy, an insert that exceeds the capacity triggers eviction of the oldest page, following the procedure described in [FIFO Eviction Algorithm](#fifo-eviction-algorithm).

When `fail_if_exists` is `true`, the library verifies in code that no page with the same identifier already exists before proceeding; if a duplicate is found, the operation fails with a dedicated error code. When `false`, no such check is performed and the caller assumes responsibility for identifier uniqueness.

```c
void pcache_put_page(
    pcache_handle           handle,
    const void             *id,
    const void             *page_data,
    bool                    fail_if_exists,
    bool                    durable,
    pcache_put_error       *error,
    int                    *sqlite_error,
    int                    *posix_error
);
```

**`pcache_put_pages`.** Stores multiple pages in a single call. The `ids` buffer must contain `count * id_size` bytes laid out contiguously; the `pages_data` buffer must contain `count * page_size` bytes. The operation is atomic: either all pages are written successfully, or none are. On volumes with FIFO policy, inserts that exceed the capacity trigger eviction of the oldest pages as needed.

When `fail_if_exists` is `true`, the library verifies that none of the supplied identifiers already exist in the volume before proceeding; if any duplicate is found, the operation fails with a dedicated error code and no pages are written. When `false`, no such check is performed and the caller assumes responsibility for identifier uniqueness across all supplied pages.

```c
void pcache_put_pages(
    pcache_handle            handle,
    size_t                   count,
    const void              *ids,
    const void              *pages_data,
    bool                     fail_if_exists,
    bool                     durable,
    pcache_put_error        *error,
    int                     *sqlite_error,
    int                     *posix_error
);
```

**`pcache_put_pages_with_counter`.** Variant of `pcache_put_pages` that derives each page identifier automatically rather than accepting an explicit `ids` buffer. Starting from `id_base` (a template identifier of exactly `id_size` bytes), it computes `count` identifiers by XORing a `uint32_t` counter — whose initial value is `start` and which increments by one for each successive page — into four consecutive bytes of the template. The byte order in which the counter is written is governed by `endianness`.

The counter occupies bytes at indices `[id_size − 4 − position, id_size − 1 − position]`; `position = 0` places the counter in the last four bytes of the identifier. The `endianness` field accepts `PCACHE_ENDIANNESS_NATIVE` (host byte order), `PCACHE_ENDIANNESS_LITTLE_ENDIAN`, and `PCACHE_ENDIANNESS_BIG_ENDIAN`.


The library validates the following conditions and fails with `PCACHE_PUT_INVALID_ARGUMENT` if any is violated:

- `(size_t)position + 4` must not exceed `id_size` (position out of identifier bounds).
- `start + count` must not exceed `UINT32_MAX + 1` (counter overflow).
- `endianness` must be one of the defined `pcache_endianness` values.

Because counter values are all distinct and XOR with a fixed base preserves distinctness, identifiers within the batch are guaranteed unique by construction; the intra-batch duplicate check is therefore skipped. In all other respects the operation behaves identically to `pcache_put_pages`: it is atomic, honours the FIFO/FIXED capacity policy, and supports optional uniqueness checking against already-stored pages.

```c
typedef enum pcache_endianness {
    PCACHE_ENDIANNESS_NATIVE        = 0,
    PCACHE_ENDIANNESS_LITTLE_ENDIAN = 1,
    PCACHE_ENDIANNESS_BIG_ENDIAN    = 2,
} pcache_endianness;

void pcache_put_pages_with_counter(
    pcache_handle            handle,
    size_t                   count,
    const void              *id_base,
    uint32_t                 start,
    uint32_t                 position,
    pcache_endianness        endianness,
    const void              *pages_data,
    bool                     fail_if_exists,
    bool                     durable,
    pcache_put_error        *error,
    int                     *sqlite_error,
    int                     *posix_error
);
```

**`pcache_get_pages_with_counter`.** Variant of `pcache_get_pages` that derives each page identifier automatically rather than accepting an explicit `ids` buffer, using the same counter-XOR scheme as `pcache_put_pages_with_counter`. Starting from `id_base`, computes `count` identifiers and retrieves the corresponding pages into `pages_buffer`. The `pages_buffer` must have at least `count * page_size` bytes available. The operation is fail-fast: if any computed identifier is not found, the operation fails and the buffer contents are unspecified.

The library validates the same conditions as `pcache_put_pages_with_counter` and fails with `PCACHE_GET_INVALID_ARGUMENT` if any is violated. If the identifier buffer cannot be allocated, the operation fails with `PCACHE_GET_OUT_OF_MEMORY`.

```c
void pcache_get_pages_with_counter(
    pcache_handle            handle,
    size_t                   count,
    const void              *id_base,
    uint32_t                 start,
    uint32_t                 position,
    pcache_endianness        endianness,
    void                    *pages_buffer,
    pcache_get_error        *error,
    int                     *sqlite_error,
    int                     *posix_error
);
```

**`pcache_check_pages_with_counter`.** Variant of `pcache_check_pages` that derives each page identifier automatically using the same counter-XOR scheme as `pcache_put_pages_with_counter`. The caller supplies a `results` array of at least `count` booleans; entry `i` is set to `true` if the computed page exists, `false` otherwise. On a SQLite error, entries from the failed position onward are unspecified.

The library validates the same conditions as `pcache_put_pages_with_counter` and fails with `PCACHE_CHECK_INVALID_ARGUMENT` if any is violated.

```c
void pcache_check_pages_with_counter(
    pcache_handle       handle,
    size_t              count,
    const void         *id_base,
    uint32_t            start,
    uint32_t            position,
    pcache_endianness   endianness,
    bool               *results,
    pcache_check_error *error,
    int                *sqlite_error
);
```

**`pcache_delete_pages_with_counter`.** Variant of `pcache_delete_pages` that derives each page identifier automatically using the same counter-XOR scheme as `pcache_put_pages_with_counter`. Identifiers that are not found in the volume are silently skipped, exactly as in `pcache_delete_pages`. In all other respects the operation behaves identically to `pcache_delete_pages`.

The library validates the same conditions as `pcache_put_pages_with_counter` and fails with `PCACHE_DELETE_INVALID_ARGUMENT` if any is violated. If the identifier buffer cannot be allocated, the operation fails with `PCACHE_DELETE_OUT_OF_MEMORY`.

```c
void pcache_delete_pages_with_counter(
    pcache_handle        handle,
    size_t               count,
    const void          *id_base,
    uint32_t             start,
    uint32_t             position,
    pcache_endianness    endianness,
    bool                 wipe_data_file,
    bool                 durable,
    pcache_delete_error *error,
    int                 *sqlite_error,
    int                 *posix_error
);
```

**`pcache_get_page`.** Retrieves the page associated with the supplied `id`, copying its contents into the buffer provided by the caller, which must have at least `page_size` bytes available.

```c
void pcache_get_page(
    pcache_handle           handle,
    const void             *id,
    void                   *page_buffer,
    pcache_get_error       *error,
    int                    *sqlite_error,
    int                    *posix_error
);
```

**`pcache_get_pages`.** Retrieves multiple pages in a single call. The `ids` buffer must contain `count * id_size` bytes laid out contiguously; the `pages_buffer` must have at least `count * page_size` bytes available. The operation is atomic: if any of the requested pages does not exist, the operation fails and the buffer contents are unspecified.

```c
void pcache_get_pages(
    pcache_handle            handle,
    size_t                   count,
    const void              *ids,
    void                    *pages_buffer,
    pcache_get_error        *error,
    int                     *sqlite_error,
    int                     *posix_error
);
```

#### Endianness and Range Queries

Range methods (`pcache_get_pages_range`, `pcache_check_pages_range`, `pcache_delete_pages_range`) compare identifiers byte-by-byte, which corresponds to SQLite BLOB ordering. This comparison is equivalent to unsigned numerical comparison only when multi-byte integers are stored in **big-endian** byte order: the most-significant byte occupies the lowest address, so `counter_a < counter_b` numerically implies that the byte sequence of `counter_a` is lexicographically less than the byte sequence of `counter_b`.

With little-endian encoding the orderings diverge for values that cross a byte boundary. For example, counter 256 encodes as `{0x00, 0x01, 0x00, 0x00}` in little-endian, which sorts lexicographically *before* counter 1 (`{0x01, 0x00, 0x00, 0x00}`), even though 256 > 1 numerically. A range query intended to cover counter values 1..255 would therefore not include 256, and a query from 1..256 would retrieve 256 before 1.

**Precondition:** the equivalence between numerical and lexicographic order assumes that the counter bytes in `id_base` are all zero. With zero counter bytes, XOR acts as a direct embedding and the ordering is preserved. If any counter byte in `id_base` is non-zero, the XOR may invert bits and break the ordering; the range bounds then require adjustment to compensate.

`PCACHE_ENDIANNESS_NATIVE` depends on the host architecture and is not recommended for any use case where byte-level ordering matters.

**Example:** store 1 000 pages using an 8-byte identifier (4-byte fixed prefix + 4-byte big-endian counter), then retrieve and count pages for counter values 100..199, and finally delete all pages with counter values 500..999.

```c
// id_size = 8: 4-byte fixed prefix + 4-byte big-endian counter (position = 0).
// Counter bytes in id_base are all 0x00, so XOR acts as direct embedding.
uint8_t id_base[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x00};

pcache_put_error put_err;
pcache_put_pages_with_counter(handle, 1000, id_base, /*start=*/0, /*position=*/0,
                              PCACHE_ENDIANNESS_BIG_ENDIAN, pages_data,
                              /*fail_if_exists=*/false, /*durable=*/false,
                              &put_err, NULL, NULL);

// The identifier for counter N is id_base XOR N (big-endian).
// Because the counter bytes of id_base are 0x00, the derived identifiers are:
//   counter 100 (0x00000064) -> {0xDE,0xAD,0xBE,0xEF, 0x00,0x00,0x00,0x64}
//   counter 199 (0x000000C7) -> {0xDE,0xAD,0xBE,0xEF, 0x00,0x00,0x00,0xC7}
// With big-endian the byte sequence is monotonically increasing with the counter,
// so the range bounds are simply the prefix followed by the counter in big-endian.
uint8_t first_100[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x64};
uint8_t last_199[8]  = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0xC7};

// --- Retrieve pages 100..199 ---
uint8_t ids_buf[100 * 8];
uint8_t pages_buf[100 * page_size];
uint32_t retrieved = 0;
pcache_get_error get_err;
pcache_get_pages_range(handle, first_100, last_199,
                       ids_buf, pages_buf, /*buffer_capacity=*/100,
                       &retrieved, &get_err, NULL, NULL);
// retrieved == 100; pages_buf holds the data for counters 100..199 in ascending order.

// --- Count pages 100..199 (without reading page data) ---
uint32_t count_in_range = 0;
pcache_check_error chk_err;
pcache_check_pages_range(handle, first_100, last_199,
                         &count_in_range, &chk_err, NULL);
// count_in_range == 100

// --- Delete pages 500..999 ---
uint8_t first_500[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x01, 0xF4}; // 500 = 0x1F4
uint8_t last_999[8]  = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x03, 0xE7}; // 999 = 0x3E7
pcache_delete_error del_err;
pcache_delete_pages_range(handle, first_500, last_999,
                          /*wipe_data_file=*/false, /*durable=*/false,
                          &del_err, NULL, NULL);
```

**`pcache_get_pages_range`.** Retrieves all pages whose identifier falls within the closed interval [`first`, `last`] (byte-by-byte comparison) into caller-supplied buffers. Pages are returned in ascending identifier order (SQLite BLOB ordering). The caller provides `ids_buffer` (at least `buffer_capacity * id_size` bytes) to receive the identifier of each page, and `pages_buffer` (at least `buffer_capacity * page_size` bytes) to receive the page data. `*count_out` is set to the number of pages retrieved. If the range contains more pages than `buffer_capacity`, the operation fails with `PCACHE_GET_RANGE_BUFFER_TOO_SMALL` and nothing is written to the buffers. If `first` is greater than `last`, the operation fails with `PCACHE_GET_RANGE_INVALID_RANGE`. An empty match is not an error; `*count_out` is set to zero.

```c
void pcache_get_pages_range(
    pcache_handle     handle,
    const void       *first,
    const void       *last,
    void             *ids_buffer,
    void             *pages_buffer,
    uint32_t          buffer_capacity,
    uint32_t         *count_out,
    pcache_get_error *error,
    int              *sqlite_error,
    int              *posix_error
);
```

**`pcache_check_pages_range`.** Counts the pages whose identifier falls within the closed interval [`first`, `last`] (byte-by-byte comparison), writing the count to `*count_out`. If `first` is greater than `last`, the operation fails with `PCACHE_CHECK_RANGE_INVALID_RANGE`. An empty match is not an error; `*count_out` is set to zero.

```c
void pcache_check_pages_range(
    pcache_handle       handle,
    const void         *first,
    const void         *last,
    uint32_t           *count_out,
    pcache_check_error *error,
    int                *sqlite_error
);
```

**`pcache_get_page`.** Retrieves the page associated with the supplied `id`, copying its contents into the buffer provided by the caller, which must have at least `page_size` bytes available.

```c
void pcache_get_page(
    pcache_handle     handle,
    const void       *id,
    void             *page_buffer,
    pcache_get_error *error,
    int              *sqlite_error,
    int              *posix_error
);
```

**`pcache_check_page`.** Returns `true` if a page with the supplied `id` exists in the volume, or `false` otherwise. The operation is serviced entirely against the index database, without touching the data file. On error, the returned value is unspecified and the caller must consult the error output. Implemented as a thin wrapper around `pcache_check_pages` with `count = 1`.

```c
bool pcache_check_page(
    pcache_handle       handle,
    const void         *id,
    pcache_check_error *error,
    int                *sqlite_error
);
```

**`pcache_check_pages`.** Tests the existence of multiple pages in a single lock acquisition. The `ids` buffer must contain `count * id_size` bytes laid out contiguously. The caller supplies a `results` array of at least `count` booleans; entry `i` is set to `true` if the page exists, `false` otherwise. On error, entries from the failed position onward are unspecified; earlier entries will have been filled correctly.

```c
void pcache_check_pages(
    pcache_handle       handle,
    size_t              count,
    const void         *ids,
    bool               *results,
    pcache_check_error *error,
    int                *sqlite_error
);
```

**`pcache_delete_page`.** Marks a page as deleted in the index, recycling its row. If no page with the supplied identifier exists, the call is a silent no-op. When the `wipe_data_file` parameter is true, the corresponding region of the data file is additionally overwritten with zeros, ensuring the contents can no longer be recovered by direct inspection of the file; otherwise, only the index is updated and the old bytes remain in the data file until they are eventually reused. Implemented as a thin wrapper around `pcache_delete_pages` with `count = 1`.

```c
void pcache_delete_page(
    pcache_handle        handle,
    const void          *id,
    bool                 wipe_data_file,
    bool                 durable,
    pcache_delete_error *error,
    int                 *sqlite_error,
    int                 *posix_error
);
```

**`pcache_delete_pages`.** Deletes multiple pages in a single atomic operation. The `ids` buffer must contain `count * id_size` bytes laid out contiguously. Identifiers that are not found in the volume are silently skipped — only the matching pages are deleted — and duplicate identifiers within the batch are likewise tolerated (the second occurrence simply finds nothing to delete). The deletions of the matching pages are committed atomically in a single transaction. When `wipe_data_file` is true, each deleted page's data region is overwritten with zeros; the first wipe failure is reported and no further pages are wiped.

```c
void pcache_delete_pages(
    pcache_handle        handle,
    size_t               count,
    const void          *ids,
    bool                 wipe_data_file,
    bool                 durable,
    pcache_delete_error *error,
    int                 *sqlite_error,
    int                 *posix_error
);
```

**`pcache_delete_pages_range`.** Deletes all pages whose identifier falls within the closed interval [`first`, `last`], using byte-by-byte comparison (equivalent to SQLite BLOB ordering). The operation is equivalent to:

```sql
UPDATE pages SET id_hash = NULL, id = NULL
WHERE id >= first AND id <= last;
```

Because the selection is by range rather than by exact key, an empty match (no pages in the range) is not an error. If `first` is greater than `last` (byte-by-byte comparison), the operation fails with `PCACHE_DELETE_INVALID_RANGE`. When `wipe_data_file` is `true`, each deleted page's data region is overwritten with zeros; the first wipe failure is reported and subsequent pages are not wiped. Both `first` and `last` must be exactly `id_size` bytes.

```c
void pcache_delete_pages_range(
    pcache_handle        handle,
    const void          *first,
    const void          *last,
    bool                 wipe_data_file,
    bool                 durable,
    pcache_delete_error *error,
    int                 *sqlite_error,
    int                 *posix_error
);
```

### Maintenance

**`pcache_defragment`.** Relocates live pages contiguously toward the beginning of the data file, eliminating the gaps left by recycled rows. It accepts a progress *callback* that allows the caller to monitor and, if desired, cancel the operation. When the `shrink_file` argument is true, the data file is truncated to its minimum size after the relocation. If the callback cancels the operation, the volume remains in a consistent state: pages processed up to the cancellation point have been relocated and the index updated accordingly; unprocessed pages remain in their original positions.

On `FIFO` volumes the operation is a no-op: the FIFO eviction order is encoded in the relative position of live and empty slots, so any rearrangement would corrupt that order. The callback is invoked once with `progress = 1.0` and the function returns success without touching the volume.

The relocation is **crash-safe**: for each page that needs to be moved, the operation proceeds as follows:

1. The page data is copied to the destination slot (the source remains intact).
2. Within a single SQLite transaction:
   - The `id_hash` and `id` are read from the source row.
   - Any NULL row at the destination is deleted.
   - A new row is inserted at the destination with the same `id_hash` and `id`.
   - The source row is nulled out.
3. The transaction is committed.

This sequence ensures that at any point during the operation, the index points to valid data. Duplicates of `id_hash`/`id` are permitted and occur transiently during the transaction window; after commit, the destination holds the only live copy. A crash at any point leaves the volume in a recoverable state: either the source remains valid (transaction did not complete) or the destination is valid (transaction completed).

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

**`pcache_set_max_pages`.** Adjusts the volume's maximum capacity, either by growth or by reduction. On `FIXED` volumes, if the reduction leaves live pages beyond the new limit, the library automatically moves only those pages into free slots within `[1, new_max_pages]` — no caller involvement is required. The operation fails with `PCACHE_SET_MAX_PAGES_WOULD_DISCARD_PAGES` only when the total number of live pages exceeds `new_max_pages`, meaning there is not enough room to fit them all; the volume is left unchanged. If a memory allocation for the page-copy buffer fails, the operation fails with `PCACHE_SET_MAX_PAGES_OUT_OF_MEMORY`. The relocation procedure mirrors the one used by `pcache_defragment`: page bytes are copied to the destination slot first; the index is then updated in a single atomic transaction (delete the NULL row at the destination, insert the live row, null out the source). On `FIFO` volumes, the reduction is performed by physically dropping every row with `ROWID > new_max_pages` and truncating the data file to `new_max_pages × page_size`, regardless of whether those rows were live. This favours speed and simplicity over a strict oldest-first eviction order, and may discard pages that are not the oldest. If the post-truncation state happens to be fully occupied, slot `1` is nulled out as part of the same transaction to restore the FIFO invariant (see [Auto-Recovery on Open](#auto-recovery-on-open)).

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

Supports systems where an int is 32 bits, 64 bits or more.

## pcache_repl

A read-eval-print loop (REPL) utility for interacting with libpcache volumes from the command line. It is built as a C++ executable that dynamically links against `libpcache` and uses the [Taywee/args](https://github.com/Taywee/args) library for argument parsing.

Detailed documentation: [spec_repl.md](spec_repl.md)

## Future Work

- Windows support.
- Additional replacement policies, notably those based on popularity counters (*least frequently used*).

## License

Distributed under the MIT license. See [LICENSE](LICENSE).
