#include "db.h"
#include "handle.h"
#include "macros.h"
#include "pages_util.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <xxhash.h>

void pcache_put_page(pcache_handle     handle,
                     const void       *id,
                     const void       *page_data,
                     bool              fail_if_exists,
                     bool              durable,
                     pcache_put_error *error,
                     int              *sqlite_error,
                     int              *posix_error) {
    pcache_put_pages(handle, 1, id, page_data, fail_if_exists, durable, error, sqlite_error, posix_error);
}

void pcache_get_page(pcache_handle     handle,
                     const void       *id,
                     void             *page_buffer,
                     pcache_get_error *error,
                     int              *sqlite_error,
                     int              *posix_error) {
    pcache_get_pages(handle, 1, id, page_buffer, error, sqlite_error, posix_error);
}

/* Merge FIFO holes left by a delete into the cursor run; reports through the
 * delete error outputs unless a prior failure was already recorded. Returns
 * false on failure (the next open repairs the volume). */
static bool compact_fifo_after_delete(pcache_volume       *volume,
                                      int64_t              run_end,
                                      bool                 prior_failure,
                                      pcache_delete_error *error,
                                      int                 *sqlite_error,
                                      int                 *posix_error) {
    int                 sqlite_rc = 0;
    int                 posix_rc  = 0;
    fifo_compact_result result    = fifo_compact_holes(volume, run_end, &sqlite_rc, &posix_rc);
    if (result == FIFO_COMPACT_OK)
        return true;
    if (!prior_failure) {
        switch (result) {
            case FIFO_COMPACT_SQLITE_ERROR:
                SET_ERR(sqlite_error, sqlite_rc);
                SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
                break;
            case FIFO_COMPACT_IO_ERROR:
                SET_ERR(posix_error, posix_rc);
                SET_ERR(error, PCACHE_DELETE_IO_ERROR);
                break;
            case FIFO_COMPACT_OUT_OF_MEMORY:
                SET_ERR(error, PCACHE_DELETE_OUT_OF_MEMORY);
                break;
            case FIFO_COMPACT_OK:
                break;
        }
    }
    return false;
}

/* Caller must hold volume->mutex for the entire call. */
static void _pcache_put_pages(pcache_volume    *volume,
                              size_t            count,
                              const void       *ids,
                              const void       *pages_data,
                              bool              fail_if_exists,
                              bool              durable,
                              pcache_put_error *error,
                              int              *sqlite_error,
                              int              *posix_error,
                              bool              skip_batch_duplicate_check) {
    /*
     * Key design:
     *   - Batch operation: writes multiple pages in a single transaction for efficiency.
     *   - Slot allocation:
     *       * FIXED: reuse the lowest NULL slot via idx_free_slots, INSERT while capacity remains.
     *       * FIFO: fill-up via INSERT; once row_count reaches max_pages, the inserting
     *         iteration also nulls slot 1 (transition); subsequent puts use the implicit
     *         cursor with delete-ahead + UPDATE.
     *   - Index-first durability: page data is written before COMMIT, ensuring the
     *     committed index never points to missing data. On rollback, the slot that
     *     received the partial bytes is reverted to NULL/absent so the bytes are
     *     unreachable, hence no in-memory restore is needed.
     */
    const size_t  id_size   = volume->config.id_size;
    const size_t  page_size = volume->config.page_size;
    const int64_t max_pages = (int64_t)volume->config.max_pages;

    /* ── Duplicate detection within batch ── */
    if (fail_if_exists && !skip_batch_duplicate_check) {
        switch (batch_has_duplicate_ids(ids, count, id_size)) {
            case BATCH_DUP_FOUND:
                SET_ERR(error, PCACHE_PUT_DUPLICATE_ID);
                return;
            case BATCH_DUP_OUT_OF_MEMORY:
                SET_ERR(error, PCACHE_PUT_OUT_OF_MEMORY);
                return;
            case BATCH_DUP_NONE:
                break;
        }
    }

    /* ── Duplicate detection against existing pages (optional) ── */
    if (fail_if_exists) {
        int sqlite_return_code = SQLITE_OK;

        for (size_t idx = 0; idx < count; idx++) {
            const void *id  = (const uint8_t *)ids + idx * id_size;
            int64_t     rid = find_rowid(volume, id, &sqlite_return_code);
            if (rid < 0) {
                SET_ERR(sqlite_error, sqlite_return_code);
                SET_ERR(error, PCACHE_PUT_SQLITE_ERROR);
                return;
            }
            if (rid > 0) {
                SET_ERR(error, PCACHE_PUT_DUPLICATE_ID);
                return;
            }
        }
    }

    /* ── Plan target slots ── */
    /* For each iteration we record:
     *   - do_insert[idx]:           true => INSERT new row (auto rowid)
     *   - target_rowids[idx]:       slot that will hold the new page; for INSERTs
     *                               this is the predicted auto-rowid
     *                               (volume->row_count + running_inserts),
     *                               re-confirmed against sqlite3_last_insert_rowid
     *                               after the INSERT executes
     *   - delete_ahead_rowids[idx]: row to set NULL in the same transaction;
     *                               0 means none. Used for the fill-up transition
     *                               (slot 1) and for steady-state delete-ahead. */
    int64_t *target_rowids       = calloc(count, sizeof(int64_t));
    bool    *do_insert           = calloc(count, sizeof(bool));
    int64_t *delete_ahead_rowids = calloc(count, sizeof(int64_t));
    if (!target_rowids || !do_insert || !delete_ahead_rowids) {
        free(target_rowids);
        free(do_insert);
        free(delete_ahead_rowids);
        SET_ERR(error, PCACHE_PUT_OUT_OF_MEMORY);
        return;
    }

    size_t  inserts_planned    = 0;
    int64_t free_search_cursor = 0; /* monotonic lower bound for idx_free_slots scan */
    int64_t fifo_cursor        = 0; /* 0 = uninitialized; resolved on first steady-state need */
    int     sqlite_return_code;

    for (size_t idx = 0; idx < count; idx++) {
        if (volume->config.capacity_policy == PCACHE_CAPACITY_FIXED) {
            /* FIXED policy: reuse the lowest NULL slot via the idx_free_slots partial index;
             * if no NULL slot remains and capacity allows, INSERT a new row. The monotonic
             * lower bound `free_search_cursor` ensures distinct slots across iterations. */
            sqlite3_stmt *statement = volume->statement_find_free_slot;
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            sqlite3_bind_int64(statement, 1, free_search_cursor);
            int candidate_rc = sqlite3_step(statement);
            if (candidate_rc == SQLITE_ROW) {
                target_rowids[idx] = sqlite3_column_int64(statement, 0);
                free_search_cursor = target_rowids[idx];
                sqlite3_reset(statement);
            } else if (candidate_rc == SQLITE_DONE) {
                sqlite3_reset(statement);
                if (volume->row_count + inserts_planned < volume->config.max_pages) {
                    do_insert[idx] = true;
                    inserts_planned++;
                } else {
                    SET_ERR(error, PCACHE_PUT_CAPACITY_EXCEEDED);
                    free(target_rowids);
                    free(do_insert);
                    free(delete_ahead_rowids);
                    return;
                }
            } else {
                sqlite3_reset(statement);
                SET_ERR(sqlite_error, candidate_rc);
                SET_ERR(error, PCACHE_PUT_SQLITE_ERROR);
                free(target_rowids);
                free(do_insert);
                free(delete_ahead_rowids);
                return;
            }
        } else { /* FIFO */
            if (volume->row_count + inserts_planned < volume->config.max_pages) {
                /* Fill-up phase: INSERT new row. The auto-rowid will be one
                 * past the largest existing rowid, which equals
                 * row_count + inserts_planned + 1 (since fill-up creates no gaps). */
                do_insert[idx]     = true;
                int64_t new_rowid  = (int64_t)(volume->row_count + inserts_planned) + 1;
                target_rowids[idx] = new_rowid;
                inserts_planned++;
                if (new_rowid == max_pages) {
                    /* Transition: nulling slot 1 in the same transaction restores the
                     * one-empty-slot invariant. The implicit cursor for subsequent
                     * iterations is therefore slot 1. */
                    delete_ahead_rowids[idx] = 1;
                    fifo_cursor              = 1;
                }
            } else {
                /* Steady state: cursor points at the unique empty slot; the
                 * delete-ahead slot is the next position (with wrap). */
                if (fifo_cursor == 0) {
                    fifo_cursor = find_fifo_cursor(volume, &sqlite_return_code);
                    if (fifo_cursor < 0) {
                        SET_ERR(sqlite_error, sqlite_return_code);
                        SET_ERR(error, PCACHE_PUT_SQLITE_ERROR);
                        free(target_rowids);
                        free(do_insert);
                        free(delete_ahead_rowids);
                        return;
                    }
                }
                target_rowids[idx]       = fifo_cursor;
                delete_ahead_rowids[idx] = (fifo_cursor % max_pages) + 1;
                fifo_cursor              = delete_ahead_rowids[idx];
            }
        }
    }

    /* ── SQLite transaction ── */
    /* BEGIN IMMEDIATE acquires a write lock immediately, preventing deadlocks. */
    sqlite_return_code = db_exec(volume->db, "BEGIN IMMEDIATE");
    if (sqlite_return_code != SQLITE_OK) {
        SET_ERR(sqlite_error, sqlite_return_code);
        SET_ERR(error, PCACHE_PUT_SQLITE_ERROR);
        free(target_rowids);
        free(do_insert);
        free(delete_ahead_rowids);
        return;
    }

    /*
     * A later target in a FIFO batch can be a slot that was live before this
     * transaction and was only nulled by an earlier delete-ahead step. SQLite
     * rollback restores its identifier, so preserve its page bytes as well.
     */
    page_restore *restores = calloc(count, sizeof *restores);
    if (!restores) {
        db_exec(volume->db, "ROLLBACK");
        SET_ERR(error, PCACHE_PUT_OUT_OF_MEMORY);
        free(target_rowids);
        free(do_insert);
        free(delete_ahead_rowids);
        return;
    }

    sqlite3_stmt *restore_probe = NULL;
    sqlite_return_code =
        sqlite3_prepare_v2(volume->db, "SELECT id_hash FROM pages WHERE rowid=?", -1, &restore_probe, NULL);
    if (sqlite_return_code != SQLITE_OK) {
        db_exec(volume->db, "ROLLBACK");
        SET_ERR(sqlite_error, sqlite_return_code);
        SET_ERR(error, PCACHE_PUT_SQLITE_ERROR);
        free_page_restores(restores, count);
        free(target_rowids);
        free(do_insert);
        free(delete_ahead_rowids);
        return;
    }

    bool restore_capture_ok = true;
    for (size_t idx = 0; idx < count && restore_capture_ok; idx++) {
        if (do_insert[idx])
            continue;

        bool already_captured = false;
        for (size_t previous = 0; previous < idx; previous++) {
            if (restores[previous].needs_restore && restores[previous].rowid == target_rowids[idx]) {
                already_captured = true;
                break;
            }
        }
        if (already_captured)
            continue;

        sqlite3_reset(restore_probe);
        sqlite3_clear_bindings(restore_probe);
        sqlite3_bind_int64(restore_probe, 1, target_rowids[idx]);
        sqlite_return_code = sqlite3_step(restore_probe);
        if (sqlite_return_code == SQLITE_DONE)
            continue;
        if (sqlite_return_code != SQLITE_ROW) {
            SET_ERR(sqlite_error, sqlite_return_code);
            SET_ERR(error, PCACHE_PUT_SQLITE_ERROR);
            restore_capture_ok = false;
            break;
        }
        if (sqlite3_column_type(restore_probe, 0) == SQLITE_NULL)
            continue;

        restores[idx].page_data = malloc(page_size);
        if (!restores[idx].page_data) {
            SET_ERR(error, PCACHE_PUT_OUT_OF_MEMORY);
            restore_capture_ok = false;
            break;
        }
        restores[idx].rowid   = target_rowids[idx];
        off_t   byte_offset   = rowid_to_offset(target_rowids[idx], page_size);
        ssize_t restore_bytes = pread(volume->fd, restores[idx].page_data, page_size, byte_offset);
        if (restore_bytes != (ssize_t)page_size) {
            SET_ERR(posix_error, restore_bytes < 0 ? errno : EIO);
            SET_ERR(error, PCACHE_PUT_IO_ERROR);
            restore_capture_ok = false;
            break;
        }
        restores[idx].needs_restore = true;
    }
    sqlite3_finalize(restore_probe);

    if (!restore_capture_ok) {
        db_exec(volume->db, "ROLLBACK");
        free_page_restores(restores, count);
        free(target_rowids);
        free(do_insert);
        free(delete_ahead_rowids);
        return;
    }

    sqlite3_stmt *statement_insert = volume->statement_insert;
    sqlite3_stmt *statement_update = volume->statement_update_slot;
    sqlite3_stmt *statement_delete = volume->statement_delete;

    bool   transaction_ok = true;
    size_t inserts_done   = 0;

    for (size_t idx = 0; idx < count && transaction_ok; idx++) {
        /* Step 1: delete-ahead (FIFO transition or steady-state eviction). */
        if (delete_ahead_rowids[idx] != 0) {
            sqlite3_reset(statement_delete);
            sqlite3_clear_bindings(statement_delete);
            sqlite3_bind_int64(statement_delete, 1, delete_ahead_rowids[idx]);
            sqlite_return_code = sqlite3_step(statement_delete);
            sqlite3_reset(statement_delete);
            if (sqlite_return_code != SQLITE_DONE) {
                SET_ERR(sqlite_error, sqlite_return_code);
                SET_ERR(error, PCACHE_PUT_SQLITE_ERROR);
                transaction_ok = false;
                break;
            }
        }

        /* Step 2: write the target slot. */
        const void   *id        = (const uint8_t *)ids + idx * id_size;
        unsigned int  hash      = XXH32(id, id_size, 0);
        sqlite3_stmt *statement = do_insert[idx] ? statement_insert : statement_update;
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        sqlite3_bind_int64(statement, 1, (int64_t)(unsigned int)hash);
        sqlite3_bind_blob(statement, 2, id, (int)id_size, SQLITE_STATIC);
        if (!do_insert[idx])
            sqlite3_bind_int64(statement, 3, target_rowids[idx]);

        sqlite_return_code = sqlite3_step(statement);
        if (sqlite_return_code == SQLITE_DONE && do_insert[idx]) {
            target_rowids[idx] = sqlite3_last_insert_rowid(volume->db);
            inserts_done++;
        }
        sqlite3_reset(statement);
        if (sqlite_return_code != SQLITE_DONE) {
            SET_ERR(sqlite_error, sqlite_return_code);
            SET_ERR(error, PCACHE_PUT_SQLITE_ERROR);
            transaction_ok = false;
            break;
        }
    }

    if (!transaction_ok) {
        db_exec(volume->db, "ROLLBACK");
        free_page_restores(restores, count);
        free(target_rowids);
        free(do_insert);
        free(delete_ahead_rowids);
        return;
    }

    /* ── Write page data BEFORE committing transaction ──
     * Each target_rowid was either NULL before the transaction (steady-state
     * cursor, FIXED reuse, fresh INSERT) or did not exist yet (fresh INSERT).
     * If pwrite fails, ROLLBACK reverts the index so nothing in the volume
     * references the partially-written bytes — the slot is again NULL/absent
     * and the orphaned bytes are unreachable. */
    for (size_t i = 0; i < count; i++) {
        const void *page        = (const uint8_t *)pages_data + i * page_size;
        off_t       byte_offset = rowid_to_offset(target_rowids[i], page_size);
        ssize_t     written     = put_pwrite(volume, page, page_size, byte_offset);
        if (written != (ssize_t)page_size) {
            SET_ERR(posix_error, written < 0 ? errno : EIO);
            SET_ERR(error, PCACHE_PUT_IO_ERROR);
            db_exec(volume->db, "ROLLBACK");
            restore_pages(volume, restores, count, posix_error);
            free_page_restores(restores, count);
            free(target_rowids);
            free(do_insert);
            free(delete_ahead_rowids);
            return;
        }
    }

    sqlite_return_code = db_exec(volume->db, "COMMIT");
    if (sqlite_return_code != SQLITE_OK) {
        SET_ERR(sqlite_error, sqlite_return_code);
        SET_ERR(error, PCACHE_PUT_SQLITE_ERROR);
        db_exec(volume->db, "ROLLBACK");
        restore_pages(volume, restores, count, posix_error);
        free_page_restores(restores, count);
        free(target_rowids);
        free(do_insert);
        free(delete_ahead_rowids);
        return;
    }

    /* ── Update in-memory state ── */
    volume->row_count += inserts_done;

    free_page_restores(restores, count);
    free(target_rowids);
    free(do_insert);
    free(delete_ahead_rowids);

    /* ── Optionally sync for durability ── */
    if (durable) {
        if (!sync_if_durable(volume, true, posix_error, sqlite_error)) {
            SET_ERR(error, PCACHE_PUT_IO_ERROR);
        }
    }
}

void pcache_put_pages(pcache_handle     handle,
                      size_t            count,
                      const void       *ids,
                      const void       *pages_data,
                      bool              fail_if_exists,
                      bool              durable,
                      pcache_put_error *error,
                      int              *sqlite_error,
                      int              *posix_error) {
    ZERO_3ERR(error, sqlite_error, posix_error);

    if (count == 0)
        return;

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_PUT_INVALID_HANDLE);
        return;
    }

    _pcache_put_pages(volume, count, ids, pages_data, fail_if_exists, durable, error, sqlite_error, posix_error, false);
    pthread_mutex_unlock(&volume->mutex);
}

void pcache_put_pages_with_counter(pcache_handle     handle,
                                   size_t            count,
                                   const void       *id_base,
                                   uint32_t          start,
                                   uint32_t          position,
                                   pcache_endianness endianness,
                                   const void       *pages_data,
                                   bool              fail_if_exists,
                                   bool              durable,
                                   pcache_put_error *error,
                                   int              *sqlite_error,
                                   int              *posix_error) {
    ZERO_3ERR(error, sqlite_error, posix_error);

    if (count == 0)
        return;

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_PUT_INVALID_HANDLE);
        return;
    }

    const size_t id_size = volume->config.id_size;

    size_t counter_offset = 0;
    if (!validate_with_counter_args(id_size, count, start, position, endianness, &counter_offset)) {
        SET_ERR(error, PCACHE_PUT_INVALID_ARGUMENT);
        goto unlock;
    }

    uint8_t *ids = build_with_counter_ids(count, id_size, id_base, start, counter_offset, endianness);
    if (!ids) {
        SET_ERR(error, PCACHE_PUT_OUT_OF_MEMORY);
        goto unlock;
    }

    _pcache_put_pages(volume, count, ids, pages_data, fail_if_exists, durable, error, sqlite_error, posix_error, true);
    free(ids);
    pthread_mutex_unlock(&volume->mutex);
    return;

unlock:
    pthread_mutex_unlock(&volume->mutex);
}

/* Caller must hold volume->mutex for the entire call. */
static void _pcache_get_pages(pcache_volume    *volume,
                              size_t            count,
                              const void       *ids,
                              void             *pages_buffer,
                              pcache_get_error *error,
                              int              *sqlite_error,
                              int              *posix_error) {
    /*
     * Key design:
     *   - Batch read: retrieves multiple pages in a single lock acquisition.
     *   - Two-phase lookup: first finds ROWID via index (SQLite), then reads data
     *     from the flat data file using pread().
     *   - Fail-fast: stops on first error, leaving remaining buffer untouched.
     *   - No transaction needed: reads are lock-free after handle validation.
     */
    int     sqlite_return_code = SQLITE_OK;
    int64_t rowid              = 0;
    bool    all_ok             = true;

    for (size_t idx = 0; idx < count && all_ok; idx++) {
        const void *id = (const uint8_t *)ids + idx * volume->config.id_size;
        rowid          = find_rowid(volume, id, &sqlite_return_code);
        if (rowid < 0) {
            SET_ERR(sqlite_error, sqlite_return_code);
            SET_ERR(error, PCACHE_GET_SQLITE_ERROR);
            all_ok = false;
        } else if (rowid == 0) {
            SET_ERR(error, PCACHE_GET_NOT_FOUND);
            all_ok = false;
        } else {
            /* Calculate byte offset: ROWID 1 maps to offset 0 */
            void   *buffer      = (uint8_t *)pages_buffer + idx * volume->config.page_size;
            off_t   byte_offset = rowid_to_offset(rowid, volume->config.page_size);
            ssize_t bytes_read  = pread(volume->fd, buffer, volume->config.page_size, byte_offset);
            if (bytes_read != (ssize_t)volume->config.page_size) {
                /* A short read (EOF) does not set errno; report EIO explicitly. */
                SET_ERR(posix_error, bytes_read < 0 ? errno : EIO);
                SET_ERR(error, PCACHE_GET_IO_ERROR);
                all_ok = false;
            }
        }
    }
}

void pcache_get_pages(pcache_handle     handle,
                      size_t            count,
                      const void       *ids,
                      void             *pages_buffer,
                      pcache_get_error *error,
                      int              *sqlite_error,
                      int              *posix_error) {
    ZERO_3ERR(error, sqlite_error, posix_error);

    if (count == 0)
        return;

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_GET_INVALID_HANDLE);
        return;
    }

    _pcache_get_pages(volume, count, ids, pages_buffer, error, sqlite_error, posix_error);
    pthread_mutex_unlock(&volume->mutex);
}

void pcache_get_pages_with_counter(pcache_handle     handle,
                                   size_t            count,
                                   const void       *id_base,
                                   uint32_t          start,
                                   uint32_t          position,
                                   pcache_endianness endianness,
                                   void             *pages_buffer,
                                   pcache_get_error *error,
                                   int              *sqlite_error,
                                   int              *posix_error) {
    ZERO_3ERR(error, sqlite_error, posix_error);

    if (count == 0)
        return;

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_GET_INVALID_HANDLE);
        return;
    }

    const size_t id_size = volume->config.id_size;

    size_t counter_offset = 0;
    if (!validate_with_counter_args(id_size, count, start, position, endianness, &counter_offset)) {
        SET_ERR(error, PCACHE_GET_INVALID_ARGUMENT);
        goto unlock;
    }

    uint8_t *ids = build_with_counter_ids(count, id_size, id_base, start, counter_offset, endianness);
    if (!ids) {
        SET_ERR(error, PCACHE_GET_OUT_OF_MEMORY);
        goto unlock;
    }

    _pcache_get_pages(volume, count, ids, pages_buffer, error, sqlite_error, posix_error);
    free(ids);
    pthread_mutex_unlock(&volume->mutex);
    return;

unlock:
    pthread_mutex_unlock(&volume->mutex);
}

bool pcache_check_page(pcache_handle handle, const void *id, pcache_check_error *error, int *sqlite_error) {
    bool result = false;
    pcache_check_pages(handle, 1, id, &result, error, sqlite_error);
    return result;
}

/* Caller must hold volume->mutex for the entire call. */
static void _pcache_check_pages(
    pcache_volume *volume, size_t count, const void *ids, bool *results, pcache_check_error *error, int *sqlite_error) {
    int sqlite_return_code = SQLITE_OK;
    for (size_t idx = 0; idx < count; idx++) {
        const void *id    = (const uint8_t *)ids + idx * volume->config.id_size;
        int64_t     rowid = find_rowid(volume, id, &sqlite_return_code);
        if (rowid < 0) {
            SET_2ERR(error, sqlite_error, PCACHE_CHECK_SQLITE_ERROR, sqlite_return_code);
            return;
        }
        results[idx] = rowid > 0;
    }
}

void pcache_check_pages(
    pcache_handle handle, size_t count, const void *ids, bool *results, pcache_check_error *error, int *sqlite_error) {
    /*
     * Key design:
     *   - Lightweight existence check: only queries the index, does not read data.
     *   - Acquires the lock once for the entire batch.
     *   - On error, entries from the failed position onward are unspecified.
     */
    ZERO_2ERR(error, sqlite_error);

    if (count == 0)
        return;

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_CHECK_INVALID_HANDLE);
        return;
    }

    _pcache_check_pages(volume, count, ids, results, error, sqlite_error);
    pthread_mutex_unlock(&volume->mutex);
}

void pcache_check_pages_with_counter(pcache_handle       handle,
                                     size_t              count,
                                     const void         *id_base,
                                     uint32_t            start,
                                     uint32_t            position,
                                     pcache_endianness   endianness,
                                     bool               *results,
                                     pcache_check_error *error,
                                     int                *sqlite_error) {
    ZERO_2ERR(error, sqlite_error);

    if (count == 0)
        return;

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_CHECK_INVALID_HANDLE);
        return;
    }

    const size_t id_size = volume->config.id_size;

    size_t counter_offset = 0;
    if (!validate_with_counter_args(id_size, count, start, position, endianness, &counter_offset)) {
        SET_ERR(error, PCACHE_CHECK_INVALID_ARGUMENT);
        goto unlock;
    }

    uint8_t *ids = build_with_counter_ids(count, id_size, id_base, start, counter_offset, endianness);
    if (!ids) {
        SET_ERR(error, PCACHE_CHECK_OUT_OF_MEMORY);
        goto unlock;
    }

    _pcache_check_pages(volume, count, ids, results, error, sqlite_error);
    free(ids);
    pthread_mutex_unlock(&volume->mutex);
    return;

unlock:
    pthread_mutex_unlock(&volume->mutex);
}

void pcache_delete_page(pcache_handle        handle,
                        const void          *id,
                        bool                 wipe_data_file,
                        bool                 durable,
                        pcache_delete_error *error,
                        int                 *sqlite_error,
                        int                 *posix_error) {
    pcache_delete_pages(handle, 1, id, wipe_data_file, durable, error, sqlite_error, posix_error);
}

/* Caller must hold volume->mutex for the entire call. */
static void _pcache_delete_pages(pcache_volume       *volume,
                                 size_t               count,
                                 const void          *ids,
                                 bool                 wipe_data_file,
                                 bool                 durable,
                                 pcache_delete_error *error,
                                 int                 *sqlite_error,
                                 int                 *posix_error) {
    int64_t *rowids = malloc(count * sizeof(int64_t));
    if (!rowids) {
        SET_ERR(error, PCACHE_DELETE_OUT_OF_MEMORY);
        return;
    }

    /* ── Look up ROWIDs; missing ids are silently skipped, duplicates collapse to one row ── */
    size_t found_count        = 0;
    int    sqlite_return_code = SQLITE_OK;
    for (size_t idx = 0; idx < count; idx++) {
        const void *id    = (const uint8_t *)ids + idx * volume->config.id_size;
        int64_t     rowid = find_rowid(volume, id, &sqlite_return_code);
        if (rowid < 0) {
            SET_ERR(sqlite_error, sqlite_return_code);
            SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
            free(rowids);
            return;
        }
        if (rowid == 0)
            continue; /* not present — silently ignore */
        rowids[found_count++] = rowid;
    }

    if (found_count == 0) {
        free(rowids);
        return;
    }

    /* ── FIFO: anchor the cursor run while the state is still unambiguous ── */
    const bool fifo         = volume->config.capacity_policy == PCACHE_CAPACITY_FIFO;
    int64_t    fifo_run_end = 0;
    if (fifo && volume->row_count == volume->config.max_pages) {
        fifo_run_end = fifo_locate_run_end(volume, &sqlite_return_code);
        if (fifo_run_end < 0) {
            SET_ERR(sqlite_error, sqlite_return_code);
            SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
            free(rowids);
            return;
        }
    }

    /* ── Atomically NULL out all index rows in one transaction ── */
    {
        int sqlite_exec_rc = db_exec(volume->db, "BEGIN IMMEDIATE");
        if (sqlite_exec_rc != SQLITE_OK) {
            SET_ERR(sqlite_error, sqlite_exec_rc);
            SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
            free(rowids);
            return;
        }

        sqlite3_stmt *statement    = volume->statement_delete;
        bool          tx_succeeded = true;

        for (size_t idx = 0; idx < found_count && tx_succeeded; idx++) {
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            sqlite3_bind_int64(statement, 1, rowids[idx]);
            int step_rc = sqlite3_step(statement);
            sqlite3_reset(statement);
            if (step_rc != SQLITE_DONE) {
                SET_ERR(sqlite_error, step_rc);
                SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
                tx_succeeded = false;
            }
        }

        if (!tx_succeeded) {
            db_exec(volume->db, "ROLLBACK");
            free(rowids);
            return;
        }

        sqlite_exec_rc = db_exec(volume->db, "COMMIT");
        if (sqlite_exec_rc != SQLITE_OK) {
            SET_ERR(sqlite_error, sqlite_exec_rc);
            SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
            db_exec(volume->db, "ROLLBACK");
            free(rowids);
            return;
        }
    }

    /* ── Optionally wipe page data in the data file ── */
    bool wipe_succeeded = true;
    if (wipe_data_file) {
        for (size_t idx = 0; idx < found_count; idx++) {
            if (!wipe_page_at_rowid(volume, rowids[idx])) {
                wipe_succeeded = false;
                if (!volume->wipe_buffer) {
                    SET_ERR(error, PCACHE_DELETE_OUT_OF_MEMORY);
                } else {
                    SET_ERR(posix_error, errno);
                    SET_ERR(error, PCACHE_DELETE_IO_ERROR);
                }
                break;
            }
        }
    }

    /* ── FIFO: merge the freshly created holes into the cursor run ── */
    bool compact_ok = true;
    if (fifo)
        compact_ok = compact_fifo_after_delete(volume, fifo_run_end, !wipe_succeeded, error, sqlite_error, posix_error);

    /* ── Optionally sync for durability ── */
    if (durable && wipe_succeeded && compact_ok) {
        if (!sync_if_durable(volume, true, posix_error, sqlite_error)) {
            SET_ERR(error, PCACHE_DELETE_IO_ERROR);
        }
    }

    free(rowids);
}

void pcache_delete_pages(pcache_handle        handle,
                         size_t               count,
                         const void          *ids,
                         bool                 wipe_data_file,
                         bool                 durable,
                         pcache_delete_error *error,
                         int                 *sqlite_error,
                         int                 *posix_error) {
    /*
     * Key design:
     *   - Missing ids are silently skipped; the deletion of the matching pages is committed
     *     atomically in a single transaction.
     *   - Index-first: once the transaction commits, the affected pages are unreachable
     *     regardless of what happens to the data file afterward.
     *   - Safe orphans: a committed-but-unwipped slot is still reclaimed on next reuse
     *     or via defragmentation.
     */
    ZERO_3ERR(error, sqlite_error, posix_error);

    if (count == 0)
        return;

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_DELETE_INVALID_HANDLE);
        return;
    }

    _pcache_delete_pages(volume, count, ids, wipe_data_file, durable, error, sqlite_error, posix_error);
    pthread_mutex_unlock(&volume->mutex);
}

void pcache_delete_pages_with_counter(pcache_handle        handle,
                                      size_t               count,
                                      const void          *id_base,
                                      uint32_t             start,
                                      uint32_t             position,
                                      pcache_endianness    endianness,
                                      bool                 wipe_data_file,
                                      bool                 durable,
                                      pcache_delete_error *error,
                                      int                 *sqlite_error,
                                      int                 *posix_error) {
    ZERO_3ERR(error, sqlite_error, posix_error);

    if (count == 0)
        return;

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_DELETE_INVALID_HANDLE);
        return;
    }

    const size_t id_size = volume->config.id_size;

    size_t counter_offset = 0;
    if (!validate_with_counter_args(id_size, count, start, position, endianness, &counter_offset)) {
        SET_ERR(error, PCACHE_DELETE_INVALID_ARGUMENT);
        goto unlock;
    }

    uint8_t *ids = build_with_counter_ids(count, id_size, id_base, start, counter_offset, endianness);
    if (!ids) {
        SET_ERR(error, PCACHE_DELETE_OUT_OF_MEMORY);
        goto unlock;
    }

    _pcache_delete_pages(volume, count, ids, wipe_data_file, durable, error, sqlite_error, posix_error);
    free(ids);
    pthread_mutex_unlock(&volume->mutex);
    return;

unlock:
    pthread_mutex_unlock(&volume->mutex);
}

void pcache_delete_pages_range(pcache_handle        handle,
                               const void          *first,
                               const void          *last,
                               bool                 wipe_data_file,
                               bool                 durable,
                               pcache_delete_error *error,
                               int                 *sqlite_error,
                               int                 *posix_error) {
    /*
     * Key design:
     *   - Single transaction: SELECT (when wipe is requested) and UPDATE happen together,
     *     so the set of affected rows is consistent.
     *   - Empty range is a valid no-op; no NOT_FOUND error is emitted.
     *   - BLOB comparison follows SQLite byte ordering, matching the SQL semantics
     *     shown in the spec (id >= first AND id <= last).
     *   - Prepared statements are created on demand; range deletes are infrequent enough
     *     that caching them in the volume struct is not worth the complexity.
     */
    ZERO_3ERR(error, sqlite_error, posix_error);

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_DELETE_INVALID_HANDLE);
        return;
    }

    const size_t id_size = volume->config.id_size;

    if (memcmp(first, last, id_size) > 0) {
        SET_ERR(error, PCACHE_DELETE_INVALID_RANGE);
        goto unlock;
    }

    const bool needs_rowids = wipe_data_file;

    int64_t *rowids        = NULL;
    size_t   rowid_count   = 0;
    size_t   rowid_cap     = 0;
    int64_t  deleted_count = 0;

    /* ── FIFO: anchor the cursor run while the state is still unambiguous ── */
    const bool fifo         = volume->config.capacity_policy == PCACHE_CAPACITY_FIFO;
    int64_t    fifo_run_end = 0;
    if (fifo && volume->row_count == volume->config.max_pages) {
        int cursor_rc = 0;
        fifo_run_end  = fifo_locate_run_end(volume, &cursor_rc);
        if (fifo_run_end < 0) {
            SET_ERR(sqlite_error, cursor_rc);
            SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
            goto unlock;
        }
    }

    /* ── BEGIN IMMEDIATE ── */
    int sqlite_exec_rc = db_exec(volume->db, "BEGIN IMMEDIATE");
    if (sqlite_exec_rc != SQLITE_OK) {
        SET_ERR(sqlite_error, sqlite_exec_rc);
        SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
        goto unlock;
    }

    /* ── Collect ROWIDs inside the transaction (needed for wipe) ── */
    if (needs_rowids) {
        sqlite3_stmt *select_stmt = NULL;
        int           rc =
            sqlite3_prepare_v2(volume->db, "SELECT rowid FROM pages WHERE id >= ? AND id <= ?", -1, &select_stmt, NULL);
        if (rc != SQLITE_OK) {
            db_exec(volume->db, "ROLLBACK");
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
            goto unlock;
        }

        sqlite3_bind_blob(select_stmt, 1, first, (int)id_size, SQLITE_STATIC);
        sqlite3_bind_blob(select_stmt, 2, last, (int)id_size, SQLITE_STATIC);

        rowid_cap = 16;
        rowids    = malloc(rowid_cap * sizeof(int64_t));
        if (!rowids) {
            sqlite3_finalize(select_stmt);
            db_exec(volume->db, "ROLLBACK");
            SET_ERR(error, PCACHE_DELETE_OUT_OF_MEMORY);
            goto unlock;
        }

        while ((rc = sqlite3_step(select_stmt)) == SQLITE_ROW) {
            if (rowid_count == rowid_cap) {
                rowid_cap *= 2;
                int64_t *grown = realloc(rowids, rowid_cap * sizeof(int64_t));
                if (!grown) {
                    sqlite3_finalize(select_stmt);
                    free(rowids);
                    rowids = NULL;
                    db_exec(volume->db, "ROLLBACK");
                    SET_ERR(error, PCACHE_DELETE_OUT_OF_MEMORY);
                    goto unlock;
                }
                rowids = grown;
            }
            rowids[rowid_count++] = sqlite3_column_int64(select_stmt, 0);
        }

        sqlite3_finalize(select_stmt);

        if (rc != SQLITE_DONE) {
            free(rowids);
            rowids = NULL;
            db_exec(volume->db, "ROLLBACK");
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
            goto unlock;
        }
    }

    /* ── NULL out all rows in the range ── */
    {
        sqlite3_stmt *update_stmt = NULL;
        int           rc          = sqlite3_prepare_v2(
            volume->db, "UPDATE pages SET id_hash = NULL, id = NULL WHERE id >= ? AND id <= ?", -1, &update_stmt, NULL);
        if (rc != SQLITE_OK) {
            free(rowids);
            rowids = NULL;
            db_exec(volume->db, "ROLLBACK");
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
            goto unlock;
        }

        sqlite3_bind_blob(update_stmt, 1, first, (int)id_size, SQLITE_STATIC);
        sqlite3_bind_blob(update_stmt, 2, last, (int)id_size, SQLITE_STATIC);
        rc = sqlite3_step(update_stmt);
        if (rc == SQLITE_DONE)
            deleted_count = sqlite3_changes(volume->db);
        sqlite3_finalize(update_stmt);

        if (rc != SQLITE_DONE) {
            free(rowids);
            rowids = NULL;
            db_exec(volume->db, "ROLLBACK");
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
            goto unlock;
        }
    }

    sqlite_exec_rc = db_exec(volume->db, "COMMIT");
    if (sqlite_exec_rc != SQLITE_OK) {
        free(rowids);
        rowids = NULL;
        SET_ERR(sqlite_error, sqlite_exec_rc);
        SET_ERR(error, PCACHE_DELETE_SQLITE_ERROR);
        db_exec(volume->db, "ROLLBACK");
        goto unlock;
    }

    /* ── Optionally wipe page data in the data file ── */
    bool wipe_succeeded = true;
    if (wipe_data_file && rowids) {
        for (size_t idx = 0; idx < rowid_count; idx++) {
            if (!wipe_page_at_rowid(volume, rowids[idx])) {
                wipe_succeeded = false;
                if (!volume->wipe_buffer) {
                    SET_ERR(error, PCACHE_DELETE_OUT_OF_MEMORY);
                } else {
                    SET_ERR(posix_error, errno);
                    SET_ERR(error, PCACHE_DELETE_IO_ERROR);
                }
                break;
            }
        }
    }

    free(rowids);

    /* ── FIFO: merge the freshly created holes into the cursor run ── */
    bool compact_ok = true;
    if (fifo && deleted_count > 0)
        compact_ok = compact_fifo_after_delete(volume, fifo_run_end, !wipe_succeeded, error, sqlite_error, posix_error);

    /* ── Optionally sync for durability ── */
    if (durable && wipe_succeeded && compact_ok) {
        if (!sync_if_durable(volume, true, posix_error, sqlite_error)) {
            SET_ERR(error, PCACHE_DELETE_IO_ERROR);
        }
    }

unlock:
    pthread_mutex_unlock(&volume->mutex);
}

void pcache_check_pages_range(pcache_handle       handle,
                              const void         *first,
                              const void         *last,
                              uint32_t           *count_out,
                              pcache_check_error *error,
                              int                *sqlite_error) {
    ZERO_2ERR(error, sqlite_error);

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_CHECK_INVALID_HANDLE);
        return;
    }

    const size_t id_size = volume->config.id_size;

    /* Reset the output count up front so every return path (including errors)
     * leaves it well-defined, matching pcache_get_pages_range. */
    if (count_out)
        *count_out = 0;

    if (memcmp(first, last, id_size) > 0) {
        SET_ERR(error, PCACHE_CHECK_RANGE_INVALID_RANGE);
        goto unlock;
    }

    if (count_out) {
        sqlite3_stmt *stmt = NULL;
        int           rc =
            sqlite3_prepare_v2(volume->db, "SELECT COUNT(*) FROM pages WHERE id >= ? AND id <= ?", -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            SET_2ERR(error, sqlite_error, PCACHE_CHECK_SQLITE_ERROR, rc);
            goto unlock;
        }
        sqlite3_bind_blob(stmt, 1, first, (int)id_size, SQLITE_STATIC);
        sqlite3_bind_blob(stmt, 2, last, (int)id_size, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
            *count_out = (uint32_t)sqlite3_column_int64(stmt, 0);
        else
            SET_2ERR(error, sqlite_error, PCACHE_CHECK_SQLITE_ERROR, rc);
        sqlite3_finalize(stmt);
    }

unlock:
    pthread_mutex_unlock(&volume->mutex);
}

void pcache_get_pages_range(pcache_handle     handle,
                            const void       *first,
                            const void       *last,
                            void             *ids_buffer,
                            void             *pages_buffer,
                            uint32_t          buffer_capacity,
                            uint32_t         *count_out,
                            pcache_get_error *error,
                            int              *sqlite_error,
                            int              *posix_error) {
    ZERO_3ERR(error, sqlite_error, posix_error);

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_GET_INVALID_HANDLE);
        return;
    }

    const size_t id_size   = volume->config.id_size;
    const size_t page_size = volume->config.page_size;

    /* Reset the output count up front so every return path (including errors)
     * leaves it well-defined, matching pcache_check_pages_range. */
    if (count_out)
        *count_out = 0;

    if (memcmp(first, last, id_size) > 0) {
        SET_ERR(error, PCACHE_GET_RANGE_INVALID_RANGE);
        goto unlock;
    }

    /* Count pages in range to check buffer capacity. */
    uint32_t total_count = 0;
    {
        sqlite3_stmt *count_stmt = NULL;
        int           rc         = sqlite3_prepare_v2(
            volume->db, "SELECT COUNT(*) FROM pages WHERE id >= ? AND id <= ?", -1, &count_stmt, NULL);
        if (rc != SQLITE_OK) {
            SET_2ERR(error, sqlite_error, PCACHE_GET_SQLITE_ERROR, rc);
            goto unlock;
        }
        sqlite3_bind_blob(count_stmt, 1, first, (int)id_size, SQLITE_STATIC);
        sqlite3_bind_blob(count_stmt, 2, last, (int)id_size, SQLITE_STATIC);
        rc = sqlite3_step(count_stmt);
        if (rc == SQLITE_ROW)
            total_count = (uint32_t)sqlite3_column_int64(count_stmt, 0);
        else
            SET_2ERR(error, sqlite_error, PCACHE_GET_SQLITE_ERROR, rc);
        sqlite3_finalize(count_stmt);
        if (error && *error != PCACHE_GET_OK)
            goto unlock;
    }

    if (total_count > buffer_capacity) {
        SET_ERR(error, PCACHE_GET_RANGE_BUFFER_TOO_SMALL);
        goto unlock;
    }

    if (total_count == 0)
        goto unlock;

    /* Retrieve IDs and page data in ascending ID order. */
    {
        sqlite3_stmt *select_stmt = NULL;
        int           rc          = sqlite3_prepare_v2(
            volume->db, "SELECT rowid, id FROM pages WHERE id >= ? AND id <= ? ORDER BY id", -1, &select_stmt, NULL);
        if (rc != SQLITE_OK) {
            SET_2ERR(error, sqlite_error, PCACHE_GET_SQLITE_ERROR, rc);
            goto unlock;
        }
        sqlite3_bind_blob(select_stmt, 1, first, (int)id_size, SQLITE_STATIC);
        sqlite3_bind_blob(select_stmt, 2, last, (int)id_size, SQLITE_STATIC);

        uint32_t idx = 0;
        while ((rc = sqlite3_step(select_stmt)) == SQLITE_ROW) {
            int64_t     rowid      = sqlite3_column_int64(select_stmt, 0);
            const void *id_bytes   = sqlite3_column_blob(select_stmt, 1);
            void       *page_dest  = (uint8_t *)pages_buffer + (size_t)idx * page_size;
            off_t       offset     = rowid_to_offset(rowid, page_size);
            ssize_t     bytes_read = pread(volume->fd, page_dest, page_size, offset);
            if (bytes_read != (ssize_t)page_size) {
                SET_ERR(posix_error, bytes_read < 0 ? errno : EIO);
                SET_ERR(error, PCACHE_GET_IO_ERROR);
                sqlite3_finalize(select_stmt);
                goto unlock;
            }
            memcpy((uint8_t *)ids_buffer + (size_t)idx * id_size, id_bytes, id_size);
            idx++;
        }
        sqlite3_finalize(select_stmt);
        if (rc != SQLITE_DONE) {
            SET_2ERR(error, sqlite_error, PCACHE_GET_SQLITE_ERROR, rc);
            goto unlock;
        }
        if (count_out)
            *count_out = idx;
    }

unlock:
    pthread_mutex_unlock(&volume->mutex);
}
