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

int main(void) {
    pcache_file_pair paths = {
        .database_path = "my_volume.db",
        .data_path     = "my_volume.dat",
    };
    pcache_configuration config = {
        .capacity_policy = PCACHE_CAPACITY_FIXED,
        .page_size       = 32768,   /* 32 KB */
        .max_pages       = 131072,  /* 4 GB total */
        .id_size         = 36,
    };
    pcache_create_error  create_err;
    pcache_open_error    open_err;
    pcache_put_error     put_err;
    pcache_get_error     get_err;
    pcache_delete_error  del_err;
    pcache_close_error   close_err;
    int                  sqlite_err;
    int                  posix_err;
    unsigned char        id[36]       = {0};
    unsigned char        data[32768]  = {0};
    unsigned char        read_buf[32768];
    pcache_handle        handle;

    /* Create a 4 GB volume with 32 KB pages and 36-byte keys.
     * Both files must not exist yet. */
    pcache_create(&paths, &config, false, false, &create_err, &sqlite_err, &posix_err);
    if (create_err != PCACHE_CREATE_OK) return 1;

    /* Open the volume. */
    handle = pcache_open(&paths, &open_err, &sqlite_err, &posix_err);
    if (!handle) return 1;

    /* Write a page. durable=true waits for the data to reach disk. */
    memset(id,   0xAB, sizeof(id));
    memset(data, 0x42, sizeof(data));
    pcache_put_page(handle, id, data, /*fail_if_exists=*/false, /*durable=*/true,
                    &put_err, &sqlite_err, &posix_err);
    if (put_err != PCACHE_PUT_OK) goto done;

    /* Read the page back using the same key. */
    pcache_get_page(handle, id, read_buf,
                    &get_err, &sqlite_err, &posix_err);

    /* Delete the page. wipe_data_file=false keeps the bytes on disk —
     * only the index entry is removed. */
    pcache_delete_page(handle, id, /*wipe_data_file=*/false, /*durable=*/true,
                       &del_err, &sqlite_err, &posix_err);

done:
    /* Close the volume. */
    pcache_close(handle,
                 &close_err, &sqlite_err, &posix_err);

    return 0;
}
```

## Method Index

### Volume Lifecycle

| Function | Description |
|---|---|
| `pcache_create` | Create a volume. |
| `pcache_open` | Open a volume. |
| `pcache_close` | Close a volume. |

### Introspection

| Function | Description |
|---|---|
| `pcache_inspect_configuration` | Read the volume configuration. |
| `pcache_inspect_page_count` | Get used and free page counts. |

### Single-Page Operations

| Function | Description |
|---|---|
| `pcache_put_page` | Write a page. |
| `pcache_get_page` | Read a page. |
| `pcache_check_page` | Check if a page exists. |
| `pcache_delete_page` | Delete a page. |

### Bulk Operations

| Function | Description |
|---|---|
| `pcache_put_pages` | Write many pages atomically. |
| `pcache_get_pages` | Read many pages. |
| `pcache_check_pages` | Check which pages exist. |
| `pcache_delete_pages` | Delete many pages. |
| `pcache_put_pages_with_counter` | Write many pages with auto-generated keys. See [README_COUNTER.md](README_COUNTER.md). |
| `pcache_get_pages_with_counter` | Read many pages with auto-generated keys. |
| `pcache_check_pages_with_counter` | Check many pages with auto-generated keys. |
| `pcache_delete_pages_with_counter` | Delete many pages with auto-generated keys. |
| `pcache_get_pages_range` | Read all pages in a key range. |
| `pcache_check_pages_range` | Count pages in a key range. |
| `pcache_delete_pages_range` | Delete all pages in a key range. |

### Maintenance

| Function | Description |
|---|---|
| `pcache_defragment` | Pack live pages to the front of the data file. |
| `pcache_set_max_pages` | Resize the volume capacity (enlarge/shrink). |
| `pcache_preallocate` | Pre-allocate index and data file space. |

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

| Platform | Status |
|---|---|
| macOS | Supported |
| Linux | Supported |
| Windows | Coming soon |

## Copyright and Licensing

Copyright © 2026 Rui Nelson. All rights reserved.
