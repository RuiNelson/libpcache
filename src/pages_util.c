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

int64_t find_fifo_cursor(pcache_volume *volume, int *sqlite_return_code) {
    sqlite3_stmt *statement = volume->statement_find_fifo_cursor;
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    sqlite3_bind_int64(statement, 1, (int64_t)volume->config.max_pages);
    int rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW) {
        int64_t cursor = sqlite3_column_int64(statement, 0);
        sqlite3_reset(statement);
        return cursor;
    }
    sqlite3_reset(statement);
    if (rc == SQLITE_DONE)
        return 1;
    *sqlite_return_code = rc;
    return -1;
}

/* Lowest occupied rowid strictly greater than `lower_bound` (pass 0 for the
 * global minimum). Returns 0 when no such row exists, -1 on SQLite error. */
static int64_t lowest_occupied_rowid_above(pcache_volume *volume, int64_t lower_bound, int *sqlite_return_code) {
    sqlite3_stmt *statement = NULL;
    int           rc        = sqlite3_prepare_v2(
        volume->db, "SELECT MIN(rowid) FROM pages WHERE id_hash IS NOT NULL AND rowid > ?", -1, &statement, NULL);
    if (rc != SQLITE_OK) {
        *sqlite_return_code = rc;
        return -1;
    }
    sqlite3_bind_int64(statement, 1, lower_bound);
    rc            = sqlite3_step(statement);
    int64_t rowid = 0;
    if (rc == SQLITE_ROW && sqlite3_column_type(statement, 0) != SQLITE_NULL)
        rowid = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    if (rc != SQLITE_ROW) {
        *sqlite_return_code = rc;
        return -1;
    }
    return rowid;
}

int64_t fifo_locate_run_end(pcache_volume *volume, int *sqlite_return_code) {
    int64_t cursor = find_fifo_cursor(volume, sqlite_return_code);
    if (cursor < 0)
        return -1;

    int64_t next_occupied = lowest_occupied_rowid_above(volume, cursor, sqlite_return_code);
    if (next_occupied < 0)
        return -1;
    if (next_occupied > 0)
        return next_occupied - 1;

    /* The run reaches the end of the table: wrap to the lowest occupied rowid. */
    int64_t first_occupied = lowest_occupied_rowid_above(volume, 0, sqlite_return_code);
    if (first_occupied < 0)
        return -1;
    if (first_occupied == 0)
        return 0; /* no live pages at all */
    return first_occupied == 1 ? (int64_t)volume->config.max_pages : first_occupied - 1;
}

fifo_compact_result
fifo_compact_holes(pcache_volume *volume, int64_t run_end, int *sqlite_return_code, int *posix_return_code) {
    const int64_t max_pages = (int64_t)volume->config.max_pages;
    const size_t  page_size = volume->config.page_size;
    const size_t  id_size   = volume->config.id_size;
    /* Steady state (all rows exist): compact circularly around the cursor run.
     * Fill-up phase: compact toward rowid 1 and drop the trailing NULL rows. */
    const bool circular = volume->row_count == volume->config.max_pages;

    if (!circular)
        run_end = (int64_t)volume->row_count;

    /* ── Collect live rowids in circular age order (oldest first) ── */
    int64_t *rowids   = NULL;
    size_t   live_cnt = 0;
    size_t   live_cap = 0;
    {
        sqlite3_stmt *statement = NULL;
        int           rc =
            sqlite3_prepare_v2(volume->db,
                               "SELECT rowid FROM pages WHERE id_hash IS NOT NULL ORDER BY (rowid - ?1 - 1 + ?2) % ?2",
                               -1,
                               &statement,
                               NULL);
        if (rc != SQLITE_OK) {
            *sqlite_return_code = rc;
            return FIFO_COMPACT_SQLITE_ERROR;
        }
        sqlite3_bind_int64(statement, 1, run_end);
        sqlite3_bind_int64(statement, 2, max_pages);
        while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
            if (live_cnt >= live_cap) {
                size_t   cap   = live_cap ? live_cap * 2 : 64;
                int64_t *grown = realloc(rowids, cap * sizeof *grown);
                if (!grown) {
                    sqlite3_finalize(statement);
                    free(rowids);
                    return FIFO_COMPACT_OUT_OF_MEMORY;
                }
                rowids   = grown;
                live_cap = cap;
            }
            rowids[live_cnt++] = sqlite3_column_int64(statement, 0);
        }
        sqlite3_finalize(statement);
        if (rc != SQLITE_DONE) {
            free(rowids);
            *sqlite_return_code = rc;
            return FIFO_COMPACT_SQLITE_ERROR;
        }
    }

    /* ── Relocate out-of-place pages, one crash-safe transaction each ──
     * The i-th oldest live page belongs at the i-th slot after the run's end.
     * A destination is either a pre-existing hole or a slot vacated by an
     * earlier move, so each transaction must commit before the next byte copy
     * may overwrite its source. */
    fifo_compact_result result = FIFO_COMPACT_OK;
    if (live_cnt > 0) {
        uint8_t      *page_buf         = malloc(page_size);
        uint8_t      *id_buf           = malloc(id_size);
        sqlite3_stmt *select_statement = NULL;
        int           rc =
            sqlite3_prepare_v2(volume->db, "SELECT id_hash, id FROM pages WHERE rowid=?", -1, &select_statement, NULL);
        if (!page_buf || !id_buf) {
            result = FIFO_COMPACT_OUT_OF_MEMORY;
        } else if (rc != SQLITE_OK) {
            *sqlite_return_code = rc;
            result              = FIFO_COMPACT_SQLITE_ERROR;
        }

        for (size_t i = 0; i < live_cnt && result == FIFO_COMPACT_OK; i++) {
            int64_t src = rowids[i];
            int64_t dst = circular ? ((run_end + (int64_t)i) % max_pages) + 1 : (int64_t)i + 1;
            if (src == dst)
                continue;

            /* Fetch the identifier of the source row. */
            sqlite3_reset(select_statement);
            sqlite3_clear_bindings(select_statement);
            sqlite3_bind_int64(select_statement, 1, src);
            rc = sqlite3_step(select_statement);
            if (rc != SQLITE_ROW) {
                *sqlite_return_code = rc == SQLITE_DONE ? SQLITE_CORRUPT : rc;
                result              = FIFO_COMPACT_SQLITE_ERROR;
                break;
            }
            int64_t     id_hash  = sqlite3_column_int64(select_statement, 0);
            const void *id_blob  = sqlite3_column_blob(select_statement, 1);
            int         id_bytes = sqlite3_column_bytes(select_statement, 1);
            if (id_bytes < 0 || (size_t)id_bytes > id_size)
                id_bytes = (int)id_size;
            if (id_blob)
                memcpy(id_buf, id_blob, (size_t)id_bytes);
            else
                id_bytes = 0;
            sqlite3_reset(select_statement);

            /* Copy the page bytes to the destination slot. */
            ssize_t io = pread(volume->fd, page_buf, page_size, rowid_to_offset(src, page_size));
            if (io == (ssize_t)page_size)
                io = pwrite(volume->fd, page_buf, page_size, rowid_to_offset(dst, page_size));
            if (io != (ssize_t)page_size) {
                *posix_return_code = io < 0 ? errno : EIO;
                result             = FIFO_COMPACT_IO_ERROR;
                break;
            }

            /* Move the index row: the source stays valid until the commit. */
            rc = db_exec(volume->db, "BEGIN IMMEDIATE");
            if (rc != SQLITE_OK) {
                *sqlite_return_code = rc;
                result              = FIFO_COMPACT_SQLITE_ERROR;
                break;
            }
            sqlite3_stmt *update_statement = volume->statement_update_slot;
            sqlite3_reset(update_statement);
            sqlite3_clear_bindings(update_statement);
            sqlite3_bind_int64(update_statement, 1, id_hash);
            sqlite3_bind_blob(update_statement, 2, id_buf, id_bytes, SQLITE_STATIC);
            sqlite3_bind_int64(update_statement, 3, dst);
            rc = sqlite3_step(update_statement);
            sqlite3_reset(update_statement);
            if (rc == SQLITE_DONE) {
                sqlite3_stmt *null_statement = volume->statement_delete;
                sqlite3_reset(null_statement);
                sqlite3_clear_bindings(null_statement);
                sqlite3_bind_int64(null_statement, 1, src);
                rc = sqlite3_step(null_statement);
                sqlite3_reset(null_statement);
            }
            if (rc == SQLITE_DONE)
                rc = db_exec(volume->db, "COMMIT");
            else {
                db_exec(volume->db, "ROLLBACK");
            }
            if (rc != SQLITE_OK && rc != SQLITE_DONE) {
                *sqlite_return_code = rc;
                result              = FIFO_COMPACT_SQLITE_ERROR;
                break;
            }
        }

        if (select_statement)
            sqlite3_finalize(select_statement);
        free(page_buf);
        free(id_buf);
    }
    free(rowids);
    if (result != FIFO_COMPACT_OK)
        return result;

    /* ── Fill-up: drop the trailing NULL rows so the table is contiguous ── */
    if (!circular) {
        int rc = db_exec_bind_int64(volume->db, "DELETE FROM pages WHERE rowid > ?", (int64_t)live_cnt);
        if (rc != SQLITE_OK) {
            *sqlite_return_code = rc;
            return FIFO_COMPACT_SQLITE_ERROR;
        }
        volume->row_count = live_cnt;
    }

    return FIFO_COMPACT_OK;
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
