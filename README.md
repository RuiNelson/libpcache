# libpcache

A library for storing fixed-size pages on disk, each identified by a binary key.

A **volume** is made up of two files: a data file that holds the raw page bytes, and a SQLite database that maps keys to their location in the data file. All functions are thread-safe.

## Main Use Cases

You can use this library for:

- Storing fixed-size chunks of data on disk, looked up by a binary key.
- Building a cache that automatically evicts old entries when it gets full (circular buffer).

You should not use this library for:

- Anything that needs SQL queries or relational data.
- Very small datasets where having two files on disk is too much overhead.

## Main Features

### Two Capacity Policies

Choose at creation time how the volume behaves when it gets full:

- **FIXED** — writes up to a `max_pages` limit.
- **FIFO** — writes beyond `max_pages` automatically evict the oldest page (circular buffer). No explicit deletes are needed to keep the volume from filling up.

### Elastic

You can grow or shrink a volume at any time with `pcache_set_max_pages`. Growing always works. When shrinking a FIXED volume, any live pages that sit beyond the new limit are automatically moved into free slots — no manual defragmentation needed. The operation only fails if the total number of live pages exceeds the new limit (i.e. there is simply no room for them).

### Defragmentable

On FIXED volumes, deleted slots leave gaps in the data file. Call `pcache_defragment` to move all live pages to the front and optionally truncate the file, reclaiming the wasted space. A progress callback lets you track or cancel the operation at any point — the volume stays consistent whether you cancel or not.

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
| `pcache_create` | Create a new volume (two files) on disk. |
| `pcache_open` | Open an existing volume; returns a handle on success. |
| `pcache_close` | Close a volume and free all resources. |

### Introspection

| Function | Description |
|---|---|
| `pcache_inspect_configuration` | Get the configuration the volume was created with. |
| `pcache_inspect_page_count` | Get how many pages are used and how many slots are free. |

### Single-Page Operations

| Function | Description |
|---|---|
| `pcache_put_page` | Write one page. |
| `pcache_get_page` | Read one page. |
| `pcache_check_page` | Check if a page exists (does not read the data file). |
| `pcache_delete_page` | Delete one page; does nothing if the key is not found. |

### Bulk Operations

| Function | Description |
|---|---|
| `pcache_put_pages` | Write many pages in one atomic operation. |
| `pcache_get_pages` | Read many pages; fails if any key is missing. |
| `pcache_check_pages` | Check which of many pages exist. |
| `pcache_delete_pages` | Delete many pages at once; missing keys are ignored. |
| `pcache_put_pages_with_counter` | Write many pages with auto-generated keys. See [README_COUNTER.md](README_COUNTER.md). |
| `pcache_get_pages_with_counter` | Read many pages with auto-generated keys. |
| `pcache_check_pages_with_counter` | Check many pages with auto-generated keys. |
| `pcache_delete_pages_with_counter` | Delete many pages with auto-generated keys. |
| `pcache_get_pages_range` | Read all pages whose key falls in a range, in order. |
| `pcache_check_pages_range` | Count pages whose key falls in a range. |
| `pcache_delete_pages_range` | Delete all pages whose key falls in a range. |

### Maintenance

| Function | Description |
|---|---|
| `pcache_defragment` | Move all pages to the front of the data file to reclaim space (FIXED volumes only). |
| `pcache_set_max_pages` | Change the maximum number of pages the volume can hold. |
| `pcache_preallocate` | Reserve space in the index and/or data file up front. |

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
