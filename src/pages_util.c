#include "pages_util.h"
#include "db.h"
#include "libpcache.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int compare_blobs_at(const void *blobs, size_t blob_size, size_t idx_a, size_t idx_b) {
    return memcmp((const uint8_t *)blobs + idx_a * blob_size, (const uint8_t *)blobs + idx_b * blob_size, blob_size);
}

static void sift_down(size_t *heap, size_t root, size_t end, const void *blobs, size_t blob_size) {
    while (root * 2 + 1 <= end) {
        size_t child = root * 2 + 1;
        if (child + 1 <= end && compare_blobs_at(blobs, blob_size, heap[child], heap[child + 1]) < 0)
            child++;
        if (compare_blobs_at(blobs, blob_size, heap[root], heap[child]) >= 0)
            return;
        size_t swap = heap[root];
        heap[root]  = heap[child];
        heap[child] = swap;
        root        = child;
    }
}

static void sort_indices_by_blob(size_t *indices, size_t count, const void *blobs, size_t blob_size) {
    if (count < 2)
        return;

    for (size_t start = count / 2; start-- > 0;)
        sift_down(indices, start, count - 1, blobs, blob_size);

    for (size_t end = count - 1; end > 0; end--) {
        size_t swap  = indices[0];
        indices[0]   = indices[end];
        indices[end] = swap;
        sift_down(indices, 0, end - 1, blobs, blob_size);
    }
}

#ifdef PCACHE_TESTING
static size_t s_test_put_pwrite_fail_after;

void pcache_test_fail_put_pwrite_after(size_t successful_writes) {
    s_test_put_pwrite_fail_after = successful_writes;
}
#endif

bool wipe_page_at_rowid(pcache_volume *volume, int64_t rowid) {
    off_t byte_offset = rowid_to_offset(rowid, volume->config.page_size);
    if (!volume->wipe_buffer) {
        volume->wipe_buffer = calloc(1, volume->config.page_size);
    }
    if (!volume->wipe_buffer) {
        return false;
    }
    ssize_t written = pwrite(volume->fd, volume->wipe_buffer, volume->config.page_size, byte_offset);
    if (written != (ssize_t)volume->config.page_size) {
        /* A short write does not set errno; callers report errno, so set EIO. */
        if (written >= 0)
            errno = EIO;
        return false;
    }
    return true;
}

void free_page_restores(page_restore *restores, size_t count) {
    if (!restores)
        return;
    for (size_t idx = 0; idx < count; idx++)
        free(restores[idx].page_data);
    free(restores);
}

bool restore_pages(pcache_volume *volume, page_restore *restores, size_t count, int *posix_error) {
    for (size_t idx = 0; idx < count; idx++) {
        if (!restores[idx].needs_restore)
            continue;
        off_t   byte_offset = rowid_to_offset(restores[idx].rowid, volume->config.page_size);
        ssize_t written     = pwrite(volume->fd, restores[idx].page_data, volume->config.page_size, byte_offset);
        if (written != (ssize_t)volume->config.page_size) {
            if (posix_error && *posix_error == 0)
                *posix_error = written < 0 ? errno : EIO;
            return false;
        }
    }
    return true;
}

ssize_t put_pwrite(pcache_volume *volume, const void *page, size_t page_size, off_t byte_offset) {
#ifdef PCACHE_TESTING
    if (s_test_put_pwrite_fail_after == 1) {
        errno = EIO;
        return -1;
    }
    if (s_test_put_pwrite_fail_after > 1)
        s_test_put_pwrite_fail_after--;
#endif
    return pwrite(volume->fd, page, page_size, byte_offset);
}

static bool is_valid_endianness(pcache_endianness endianness) {
    switch (endianness) {
        case PCACHE_ENDIANNESS_NATIVE:
        case PCACHE_ENDIANNESS_LITTLE_ENDIAN:
        case PCACHE_ENDIANNESS_BIG_ENDIAN:
            return true;
    }

    return false;
}

static void encode_counter_bytes(uint8_t bytes[4], uint32_t counter, pcache_endianness endianness) {
#if defined(__BYTE_ORDER__)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    pcache_endianness endianness_ =
        endianness == PCACHE_ENDIANNESS_LITTLE_ENDIAN ? PCACHE_ENDIANNESS_NATIVE : endianness;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    pcache_endianness endianness_ = endianness == PCACHE_ENDIANNESS_BIG_ENDIAN ? PCACHE_ENDIANNESS_NATIVE : endianness;
#else
    pcache_endianness endianness_ = endianness;
#endif
#else
    pcache_endianness endianness_ = endianness;
#endif

    switch (endianness_) {
        case PCACHE_ENDIANNESS_NATIVE:
            memcpy(bytes, &counter, sizeof(counter));
            return;
        case PCACHE_ENDIANNESS_LITTLE_ENDIAN:
            bytes[0] = (uint8_t)(counter >> 0);
            bytes[1] = (uint8_t)(counter >> 8);
            bytes[2] = (uint8_t)(counter >> 16);
            bytes[3] = (uint8_t)(counter >> 24);
            return;
        case PCACHE_ENDIANNESS_BIG_ENDIAN:
            bytes[0] = (uint8_t)(counter >> 24);
            bytes[1] = (uint8_t)(counter >> 16);
            bytes[2] = (uint8_t)(counter >> 8);
            bytes[3] = (uint8_t)(counter >> 0);
            return;
    }
}

static batch_dup_result batch_has_duplicate_ids_sorted(const void *ids, size_t count, size_t id_size) {
    size_t *indices = malloc(count * sizeof(size_t));
    if (!indices)
        return BATCH_DUP_OUT_OF_MEMORY;

    for (size_t idx = 0; idx < count; idx++)
        indices[idx] = idx;

    sort_indices_by_blob(indices, count, ids, id_size);

    batch_dup_result result = BATCH_DUP_NONE;
    for (size_t idx = 1; idx < count; idx++) {
        const void *prev = (const uint8_t *)ids + indices[idx - 1] * id_size;
        const void *curr = (const uint8_t *)ids + indices[idx] * id_size;
        if (memcmp(prev, curr, id_size) == 0) {
            result = BATCH_DUP_FOUND;
            break;
        }
    }

    free(indices);
    return result;
}

batch_dup_result batch_has_duplicate_ids(const void *ids, size_t count, size_t id_size) {
    if (count < 2)
        return BATCH_DUP_NONE;

    if (count > 1000)
        return batch_has_duplicate_ids_sorted(ids, count, id_size);

    for (size_t idx = 0; idx < count; idx++) {
        const void *id_a = (const uint8_t *)ids + idx * id_size;
        for (size_t j = idx + 1; j < count; j++) {
            const void *id_b = (const uint8_t *)ids + j * id_size;
            if (memcmp(id_a, id_b, id_size) == 0)
                return BATCH_DUP_FOUND;
        }
    }
    return BATCH_DUP_NONE;
}

bool validate_with_counter_args(size_t            id_size,
                                size_t            count,
                                uint32_t          start,
                                uint32_t          position,
                                pcache_endianness endianness,
                                size_t           *counter_offset_out) {
    /* Overflow-safe form of `position + 4 > id_size`: on 32-bit size_t,
     * (size_t)position + 4 can wrap and let an out-of-bounds position through,
     * which would underflow counter_offset and write past the identifier. */
    if (id_size < 4 || (size_t)position > id_size - 4u)
        return false;

    if (!is_valid_endianness(endianness))
        return false;

    /* Avoid overflow: start + count <= UINT32_MAX + 1  ⟺  count - 1 <= UINT32_MAX - start (when count > 0).
     * The naive form (UINT32_MAX - start + 1u) wraps to 0 when start == 0 on 32-bit size_t. */
    if (count > 0 && count - 1 > (size_t)(UINT32_MAX - start))
        return false;

    if (counter_offset_out)
        *counter_offset_out = id_size - 4u - (size_t)position;

    return true;
}

uint8_t *build_with_counter_ids(size_t            count,
                                size_t            id_size,
                                const void       *id_base,
                                uint32_t          start,
                                size_t            counter_offset,
                                pcache_endianness endianness) {
    if (count > 0 && id_size > SIZE_MAX / count)
        return NULL;

    uint8_t *ids = malloc(count * id_size);
    if (!ids)
        return NULL;

    for (size_t idx = 0; idx < count; idx++) {
        uint8_t *id = ids + idx * id_size;
        uint8_t  counter_bytes[4];

        memcpy(id, id_base, id_size);
        encode_counter_bytes(counter_bytes, start + (uint32_t)idx, endianness);

        for (size_t byte_idx = 0; byte_idx < 4; byte_idx++)
            id[counter_offset + byte_idx] ^= counter_bytes[byte_idx];
    }

    return ids;
}
