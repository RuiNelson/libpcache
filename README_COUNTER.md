# Using the Counter Variants

The `_with_counter` functions let you write, read, check, or delete a run of pages without building an explicit array of keys. Instead, you supply a single base key (`id_base`) and the library derives one key per page by XORing an incrementing `uint32_t` counter into it.

## How the Key Is Derived

```
key[i] = id_base XOR counter_bytes(start + i, position, endianness)
```

The counter occupies exactly 4 bytes inside the key. `position` controls where those bytes sit, counting from the **end** of the key:

- `position = 0` — counter in the last 4 bytes (default).
- `position = 1` — counter ends one byte before the end, i.e. bytes `[id_size-5 .. id_size-2]`.

`endianness` controls the byte order of the counter within those 4 bytes:

| Value | Meaning |
|---|---|
| `PCACHE_ENDIANNESS_LITTLE_ENDIAN` | Least-significant byte first. |
| `PCACHE_ENDIANNESS_BIG_ENDIAN` | Most-significant byte first. |
| `PCACHE_ENDIANNESS_NATIVE` | Host byte order (not portable across machines). |

Because the counter values within a batch are all distinct, the derived keys are guaranteed unique by construction — no duplicate check is performed inside the batch.

## Motivating Example: SHA-256 + Sub-pages

Suppose you are caching a large file identified by its SHA-256 digest. The file is split into fixed-size chunks, and each chunk needs its own key in the volume. A natural layout is:

```
id_base  =  [ SHA-256 digest (32 bytes) | 0x00 0x00 0x00 0x00 (4 bytes) ]
             ↑ identifies the file                ↑ counter goes here
```

With `id_size = 36` and `position = 0`, the library places the counter in the last 4 bytes of each derived key. Because the counter is a `uint32_t`, a single SHA-256 digest can address up to **2³² sub-pages** — over four billion chunks per file. Two different digests can never collide regardless of their counter values, since the first 32 bytes already differ.

This pattern makes the counter a natural fit for sub-page indexing: you compute the digest once, set the last 4 bytes to zero, and let the library generate all sub-page keys in a single call.

## Example

Store 1 000 pages whose keys share the same SHA-256 digest, differing only in the last 4 bytes (the chunk index):

```c
#include <string.h>
#include "libpcache.h"

#define CHUNK_COUNT 1000
#define PAGE_SIZE   32768   /* 32 KB */
#define ID_SIZE     36      /* 32-byte SHA-256 + 4-byte counter */

void store_chunks(pcache_handle handle, const unsigned char sha256[32], const unsigned char *data) {
    /* Base key: SHA-256 digest followed by four zero bytes (counter placeholder). */
    unsigned char id_base[ID_SIZE];
    memcpy(id_base, sha256, 32);
    memset(id_base + 32, 0, 4);

    pcache_put_error put_err;
    pcache_put_pages_with_counter(
        handle,
        CHUNK_COUNT,
        id_base,
        0,                           /* start: chunk index 0 */
        0,                           /* position: counter in the last 4 bytes */
        PCACHE_ENDIANNESS_BIG_ENDIAN,
        data,                        /* CHUNK_COUNT * PAGE_SIZE bytes */
        /*fail_if_exists=*/ false,
        /*durable=*/        true,
        &put_err, NULL, NULL);
}
```

To read those chunks back:

```c
void load_chunks(pcache_handle handle, const unsigned char sha256[32], unsigned char *out) {
    unsigned char id_base[ID_SIZE];
    memcpy(id_base, sha256, 32);
    memset(id_base + 32, 0, 4);

    pcache_get_error get_err;
    pcache_get_pages_with_counter(
        handle,
        CHUNK_COUNT,
        id_base,
        0,
        0,
        PCACHE_ENDIANNESS_BIG_ENDIAN,
        out,   /* must be at least CHUNK_COUNT * PAGE_SIZE bytes */
        &get_err, NULL, NULL);
}
```

## Constraints

| Constraint | Effect if violated |
|---|---|
| `position + 4 > id_size` | Fails with `PCACHE_PUT_INVALID_ARGUMENT` (counter out of bounds). |
| `start + count > UINT32_MAX + 1` | Fails with `PCACHE_PUT_INVALID_ARGUMENT` (counter overflow). |
| Unrecognised `endianness` value | Fails with `PCACHE_PUT_INVALID_ARGUMENT`. |

The same constraints and the same error codes apply to the `_get_`, `_check_`, and `_delete_` variants, substituting the appropriate enum (`PCACHE_GET_INVALID_ARGUMENT`, etc.).
