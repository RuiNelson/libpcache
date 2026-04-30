# libpcache

Fast, reliable, persistent storage for fixed-size pages — identified by any binary key you choose.

A **volume** is two files working as one: a data file that holds the raw page bytes, and a SQLite index that maps every key to its exact location. Every operation is thread-safe and fully atomic.

## Main Use Cases

- Store fixed-size chunks of data on disk and retrieve them instantly by binary key.
- Build a self-managing cache that evicts old entries automatically when it gets full.

## Main Features

### Two Capacity Policies

Pick the policy that suits your workload at creation time:

- **FIXED** — the volume holds up to `max_pages` pages. Deleted slots are recycled immediately for the next write.
- **FIFO** — the volume acts as a **circular buffer**. When full, the oldest page is evicted automatically to make room for the new one.

### Elastic

Resize a volume at any time with `pcache_set_max_pages`. Growing is always instant. When shrinking a FIXED volume, pages that sit beyond the new limit are moved into free slots automatically.

### Defragmentable

Over time, deletes leave gaps in the data file. One call to `pcache_defragment` packs all live pages to the front and optionally trims the file down to its minimum size. The task can be stopped at any time via a callback.

## Starter Cookbook

```c
#include <string.h>
#include "libpcache.h"

#define PAGE_SIZE_32KB (32 * 1024)

#define GIGABYTE (1024ULL*1024*1024)
#define PAGE_COUNT_4GB ((4 * GIGABYTE) / PAGE_SIZE_32KB)

int main(void) {
    pcache_open_error    open_err;
    pcache_put_error     put_err;
    pcache_get_error     get_err;
    pcache_delete_error  del_err;
    pcache_close_error   close_err;
    int                  sqlite_err;
    int                  posix_err;

    unsigned char        id_buf[36]                 = {0};
    unsigned char        write_buf[PAGE_SIZE_32KB]  = {0};
    unsigned char        read_buf[PAGE_SIZE_32KB];
    pcache_handle        handle;

create:
    /* Create a 4 GB volume with 32 KB pages and 36-byte keys.
     * Both files must not exist yet. */
    pcache_file_pair paths = {
        .database_path = "my_volume.db",
        .data_path     = "my_volume.dat",
    };
    pcache_configuration config = {
        .capacity_policy = PCACHE_CAPACITY_FIXED,
        .page_size       = PAGE_SIZE_32KB,
        .max_pages       = PAGE_COUNT_4GB,
        .id_size         = 36,
    };
    pcache_create_error  create_err;

    pcache_create(&paths, &config, false, false, &create_err, &sqlite_err, &posix_err);
    if (create_err != PCACHE_CREATE_OK) return 1;

open:
    /* Open the volume. */
    handle = pcache_open(&paths, &open_err, &sqlite_err, &posix_err);
    if (!handle) return 1;

write:
    /* Write a page. durable=true waits for the write_buf to reach disk. */
    memset(id_buf,   0xAB, sizeof(id_buf));
    memset(write_buf, 0x42, sizeof(write_buf));
    pcache_put_page(handle, id_buf, write_buf, /*fail_if_exists=*/false, /*durable=*/true,
                    &put_err, &sqlite_err, &posix_err);
    if (put_err != PCACHE_PUT_OK) return 1;

read:
    /* Read the page back using the same key. */
    pcache_get_page(handle, id_buf, read_buf,
                    &get_err, &sqlite_err, &posix_err);
    if (get_err != PCACHE_GET_OK) return 1;

delete:
    /* Delete the page. wipe_data_file=false keeps the bytes on disk —
     * only the index entry is removed. */
    pcache_delete_page(handle, id_buf, /*wipe_data_file=*/false, /*durable=*/true,
                       &del_err, &sqlite_err, &posix_err);
    if (del_err != PCACHE_DELETE_OK) return 1;

close:
    /* Close the volume. */
    pcache_close(handle,
                 &close_err, &sqlite_err, &posix_err);

    return 0;
}
```

## Method Index

### Volume Lifecycle

| Function        | Description      |
| --------------- | ---------------- |
| `pcache_create` | Create a volume. |
| `pcache_open`   | Open a volume.   |
| `pcache_close`  | Close a volume.  |

### Introspection

| Function                       | Description                    |
| ------------------------------ | ------------------------------ |
| `pcache_inspect_configuration` | Read the volume configuration. |
| `pcache_inspect_page_count`    | Get used and free page counts. |

### Single-Page Operations

| Function             | Description             |
| -------------------- | ----------------------- |
| `pcache_put_page`    | Write a page.           |
| `pcache_get_page`    | Read a page.            |
| `pcache_check_page`  | Check if a page exists. |
| `pcache_delete_page` | Delete a page.          |

### Bulk Operations

Convenient, easy-to-use APIs for operating on many pages at once — atomically when needed.

| Function                           | Description                                                                            |
| ---------------------------------- | -------------------------------------------------------------------------------------- |
| `pcache_put_pages`                 | Write many pages atomically.                                                           |
| `pcache_get_pages`                 | Read many pages.                                                                       |
| `pcache_check_pages`               | Check which pages exist.                                                               |
| `pcache_delete_pages`              | Delete many pages.                                                                     |
| `pcache_put_pages_with_counter`    | Write many pages with auto-generated keys. See [README_COUNTER.md](README_COUNTER.md). |
| `pcache_get_pages_with_counter`    | Read many pages with auto-generated keys.                                              |
| `pcache_check_pages_with_counter`  | Check many pages with auto-generated keys.                                             |
| `pcache_delete_pages_with_counter` | Delete many pages with auto-generated keys.                                            |
| `pcache_get_pages_range`           | Read all pages in a key range.                                                         |
| `pcache_check_pages_range`         | Count pages in a key range.                                                            |
| `pcache_delete_pages_range`        | Delete all pages in a key range.                                                       |

### Maintenance

| Function               | Description                                    |
| ---------------------- | ---------------------------------------------- |
| `pcache_defragment`    | Pack live pages to the front of the data file. |
| `pcache_set_max_pages` | Resize the volume capacity (enlarge/shrink).   |
| `pcache_preallocate`   | Pre-allocate index and data file space.        |

### Interactive REPL

A command-line utility for exploring and testing volumes interactively. See [repl/README.md](repl/README.md).

## How much disk memory does the database need?

The SQLite index stores one row per page: the identifier blob (up to `id_size` bytes) and the byte offset derived from the rowid. The database size is therefore a function of row count and identifier length.

| Capacity   | Page Size  | Total Pages | ID Size  | Database file size |
| ---------- | ---------- | ----------: | -------- | -----------------: |
| 1 GB       | 64 KB      |       16384 | 36 B     |            1.02 MB |
| 1 GB       | **32 KB**  |       32768 | 36 B     |        **2.03 MB** |
| 1 GB       | 64 KB      |       16384 | **72 B** |        **1.61 MB** |
| **2 GB**   | **128 KB** |       16384 | 36 B     |        **1.02 MB** |
| **100 MB** | **1 KB**   |      102400 | 36 B     |        **6.41 MB** |

The key relationships:

- **Denser pages → smaller index.** 2 GB / 128 KB and 1 GB / 64 KB both yield exactly 16 384 pages, so the index is identical (1.02 MB). Halving the page size (1 GB / 32 KB) doubles the entry count and roughly doubles the database.
- **ID length scales linearly.** The per-row overhead is dominated by the BLOB storage for the identifier. Going from 36 B to 72 B doubles that component, increasing the database from 1.02 MB to 1.61 MB (~58% more).
- **Small pages inflate the index disproportionately.** The 100 MB / 1 KB case holds the least data but requires 102 400 rows — more than any other configuration — resulting in a 6.41 MB database for 100 MB of data (6.4% ratio). At the extreme, index overhead can exceed the payload.

For typical workloads with 64 KB+ pages and compact identifiers, the index remains well under 1% of the data file.

## Building Documentation

Install [Doxygen](https://www.doxygen.nl) and run:

```sh
doxygen Doxyfile
```

The HTML output is written to `docs/html/`. Open `docs/html/index.html` in a browser.

## Requirements

- C17 compiler (clang or gcc)
- CMake 3.14 or later
- libsqlite3 (system package, dynamically linked)

## Platform Compatibility

| Platform | Status      |
| -------- | ----------- |
| macOS    | Supported   |
| Linux    | Supported   |
| Windows  | Coming soon |

## Copyright and Licensing

Copyright © 2026 Rui Nelson. All rights reserved.
