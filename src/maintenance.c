#include "db.h"
#include "handle.h"
#include "macros.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

/* Batch relocations to reduce fsync overhead.
 * Each batch processes up to DEFAGMENT_BATCH_SIZE pages in one transaction. */
#define DEFAGMENT_BATCH_SIZE 100

#ifdef PCACHE_TESTING
static size_t s_test_defragment_metadata_fail_after;

void pcache_test_fail_defragment_metadata_after(size_t successful_loads) {
    s_test_defragment_metadata_fail_after = successful_loads;
}
#endif

void pcache_defragment(pcache_handle            handle,
                       pcache_progress_fn       progress_callback,
                       void                    *progress_user_data,
                       bool                     shrink_file,
                       bool                     durable,
                       pcache_defragment_error *error,
                       int                     *sqlite_error,
                       int                     *posix_error) {
    ZERO_3ERR(error, sqlite_error, posix_error);

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_DEFRAGMENT_INVALID_HANDLE);
        return;
    }

    /* On FIFO volumes the eviction order is encoded in the relative position
     * of live and empty slots, so any rearrangement would corrupt that order.
     * Report 100% progress to the callback and exit. */
    if (volume->config.capacity_policy == PCACHE_CAPACITY_FIFO) {
        if (progress_callback)
            progress_callback(1.0, progress_user_data);
        pthread_mutex_unlock(&volume->mutex);
        return;
    }

    /* ── Load live ROWIDs into memory ── */
    int64_t *rowids   = NULL;
    size_t   live_cnt = 0;
    size_t   live_cap = 0;

    {
        sqlite3_stmt *stmt;
        int           sqlite_return_code = sqlite3_prepare_v2(
            volume->db, "SELECT rowid FROM pages WHERE id_hash IS NOT NULL ORDER BY rowid ASC", -1, &stmt, NULL);
        if (sqlite_return_code != SQLITE_OK) {
            SET_ERR(sqlite_error, sqlite_return_code);
            SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
            goto unlock;
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (live_cnt >= live_cap) {
                size_t   cap = live_cap ? live_cap * 2 : 64;
                int64_t *p   = realloc(rowids, cap * sizeof *p);
                if (!p) {
                    sqlite3_finalize(stmt);
                    free(rowids);
                    SET_ERR(error, PCACHE_DEFRAGMENT_OUT_OF_MEMORY);
                    goto unlock; /* OOM — leave volume unchanged */
                }
                rowids   = p;
                live_cap = cap;
            }
            rowids[live_cnt++] = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    if (live_cnt == 0) {
        free(rowids);
        goto cleanup_nulls;
    }

    /* ── Relocate pages one by one ── */
    {
        void *buf = malloc(volume->config.page_size);
        if (!buf) {
            free(rowids);
            SET_ERR(error, PCACHE_DEFRAGMENT_OUT_OF_MEMORY);
            goto unlock;
        }

        typedef struct {
            int64_t src_rowid;
            int64_t dst_rowid;
            int64_t id_hash;
            void   *id_blob;
            int     id_blob_len;
        } relocation_t;

        relocation_t *batch = malloc(DEFAGMENT_BATCH_SIZE * sizeof *batch);
        if (!batch) {
            SET_ERR(error, PCACHE_DEFRAGMENT_OUT_OF_MEMORY);
            free(buf);
            free(rowids);
            goto unlock;
        }

        size_t batch_count        = 0;
        int    sqlite_return_code = SQLITE_OK;

        for (size_t i = 0; i < live_cnt; i++) {
            int64_t rowid        = rowids[i];
            int64_t next_compact = (int64_t)(i + 1);

            if (rowid != next_compact) {
                relocation_t relocation = {
                    .src_rowid   = rowid,
                    .dst_rowid   = next_compact,
                    .id_hash     = 0,
                    .id_blob     = NULL,
                    .id_blob_len = 0,
                };
                sqlite3_stmt *stmt = NULL;

                sqlite_return_code =
                    sqlite3_prepare_v2(volume->db, "SELECT id_hash, id FROM pages WHERE rowid=?", -1, &stmt, NULL);
                if (sqlite_return_code != SQLITE_OK) {
                    SET_ERR(sqlite_error, sqlite_return_code);
                    SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
                    for (size_t k = 0; k < batch_count; k++)
                        free(batch[k].id_blob);
                    free(batch);
                    free(buf);
                    free(rowids);
                    goto unlock;
                }

                sqlite_return_code = sqlite3_bind_int64(stmt, 1, rowid);
                if (sqlite_return_code == SQLITE_OK)
                    sqlite_return_code = sqlite3_step(stmt);
                if (sqlite_return_code != SQLITE_ROW) {
                    sqlite3_finalize(stmt);
                    SET_ERR(sqlite_error, sqlite_return_code == SQLITE_DONE ? SQLITE_CORRUPT : sqlite_return_code);
                    SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
                    for (size_t k = 0; k < batch_count; k++)
                        free(batch[k].id_blob);
                    free(batch);
                    free(buf);
                    free(rowids);
                    goto unlock;
                }

                relocation.id_hash   = sqlite3_column_int64(stmt, 0);
                const void *blob     = sqlite3_column_blob(stmt, 1);
                int         blob_len = sqlite3_column_bytes(stmt, 1);
                if (blob && blob_len > 0) {
#ifdef PCACHE_TESTING
                    if (s_test_defragment_metadata_fail_after == 1) {
                        sqlite3_finalize(stmt);
                        SET_ERR(error, PCACHE_DEFRAGMENT_OUT_OF_MEMORY);
                        for (size_t k = 0; k < batch_count; k++)
                            free(batch[k].id_blob);
                        free(batch);
                        free(buf);
                        free(rowids);
                        goto unlock;
                    }
                    if (s_test_defragment_metadata_fail_after > 1)
                        s_test_defragment_metadata_fail_after--;
#endif
                    relocation.id_blob = malloc(blob_len);
                    if (!relocation.id_blob) {
                        sqlite3_finalize(stmt);
                        SET_ERR(error, PCACHE_DEFRAGMENT_OUT_OF_MEMORY);
                        for (size_t k = 0; k < batch_count; k++)
                            free(batch[k].id_blob);
                        free(batch);
                        free(buf);
                        free(rowids);
                        goto unlock;
                    }
                    memcpy(relocation.id_blob, blob, blob_len);
                    relocation.id_blob_len = blob_len;
                }
                sqlite3_finalize(stmt);

                off_t src = rowid_to_offset(rowid, volume->config.page_size);
                off_t dst = rowid_to_offset(next_compact, volume->config.page_size);

                /* Step 1: Copy page data */
                if (pread(volume->fd, buf, volume->config.page_size, src) != (ssize_t)volume->config.page_size) {
                    SET_ERR(posix_error, errno);
                    SET_ERR(error, PCACHE_DEFRAGMENT_IO_ERROR);
                    free(relocation.id_blob);
                    for (size_t k = 0; k < batch_count; k++)
                        free(batch[k].id_blob);
                    free(batch);
                    free(buf);
                    free(rowids);
                    goto unlock;
                }
                if (pwrite(volume->fd, buf, volume->config.page_size, dst) != (ssize_t)volume->config.page_size) {
                    SET_ERR(posix_error, errno);
                    SET_ERR(error, PCACHE_DEFRAGMENT_IO_ERROR);
                    free(relocation.id_blob);
                    for (size_t k = 0; k < batch_count; k++)
                        free(batch[k].id_blob);
                    free(batch);
                    free(buf);
                    free(rowids);
                    goto unlock;
                }

                batch[batch_count++] = relocation;
            }

            /* Process batch when full or at end of loop */
            if (batch_count >= DEFAGMENT_BATCH_SIZE || (i == live_cnt - 1 && batch_count > 0)) {
                sqlite_return_code = db_exec(volume->db, "BEGIN IMMEDIATE");
                if (sqlite_return_code != SQLITE_OK) {
                    SET_ERR(sqlite_error, sqlite_return_code);
                    SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
                    for (size_t k = 0; k < batch_count; k++)
                        free(batch[k].id_blob);
                    free(batch);
                    free(buf);
                    free(rowids);
                    goto unlock;
                }

                sqlite3_stmt *del_stmt  = NULL;
                sqlite3_stmt *ins_stmt  = NULL;
                sqlite3_stmt *null_stmt = NULL;

                sqlite_return_code = sqlite3_prepare_v2(
                    volume->db, "DELETE FROM pages WHERE rowid=? AND id_hash IS NULL", -1, &del_stmt, NULL);
                if (sqlite_return_code == SQLITE_OK)
                    sqlite_return_code = sqlite3_prepare_v2(
                        volume->db, "INSERT INTO pages(rowid, id_hash, id) VALUES(?, ?, ?)", -1, &ins_stmt, NULL);
                if (sqlite_return_code == SQLITE_OK)
                    sqlite_return_code = sqlite3_prepare_v2(
                        volume->db, "UPDATE pages SET id_hash=NULL, id=NULL WHERE rowid=?", -1, &null_stmt, NULL);

                if (sqlite_return_code == SQLITE_OK) {
                    for (size_t k = 0; k < batch_count; k++) {
                        /* Delete NULL row at destination */
                        sqlite3_reset(del_stmt);
                        sqlite3_clear_bindings(del_stmt);
                        sqlite3_bind_int64(del_stmt, 1, batch[k].dst_rowid);
                        int del_rc = sqlite3_step(del_stmt);
                        if (del_rc != SQLITE_DONE) {
                            sqlite_return_code = sqlite3_errcode(volume->db);
                            break;
                        }

                        /* Insert at destination */
                        sqlite3_reset(ins_stmt);
                        sqlite3_clear_bindings(ins_stmt);
                        sqlite3_bind_int64(ins_stmt, 1, batch[k].dst_rowid);
                        sqlite3_bind_int64(ins_stmt, 2, batch[k].id_hash);
                        if (batch[k].id_blob && batch[k].id_blob_len > 0)
                            sqlite3_bind_blob(ins_stmt, 3, batch[k].id_blob, batch[k].id_blob_len, SQLITE_STATIC);
                        else
                            sqlite3_bind_null(ins_stmt, 3);
                        int step_rc = sqlite3_step(ins_stmt);
                        if (step_rc != SQLITE_DONE && step_rc != SQLITE_ROW) {
                            sqlite_return_code = sqlite3_errcode(volume->db);
                            break;
                        }

                        /* Null out source */
                        sqlite3_reset(null_stmt);
                        sqlite3_clear_bindings(null_stmt);
                        sqlite3_bind_int64(null_stmt, 1, batch[k].src_rowid);
                        step_rc = sqlite3_step(null_stmt);
                        if (step_rc != SQLITE_DONE) {
                            sqlite_return_code = sqlite3_errcode(volume->db);
                            break;
                        }
                    }
                }

                if (del_stmt)
                    sqlite3_finalize(del_stmt);
                if (ins_stmt)
                    sqlite3_finalize(ins_stmt);
                if (null_stmt)
                    sqlite3_finalize(null_stmt);

                /* Free id_blobs */
                for (size_t j = 0; j < batch_count; j++)
                    free(batch[j].id_blob);

                if (sqlite_return_code == SQLITE_OK)
                    sqlite_return_code = db_exec(volume->db, "COMMIT");
                else
                    db_exec(volume->db, "ROLLBACK");

                if (sqlite_return_code != SQLITE_OK) {
                    SET_ERR(sqlite_error, sqlite_return_code);
                    SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
                    free(batch);
                    free(buf);
                    free(rowids);
                    goto unlock;
                }

                batch_count = 0;
            }

            if (progress_callback) {
                double frac = (double)(i + 1) / live_cnt;
                if (!progress_callback(frac, progress_user_data)) {
                    for (size_t k = 0; k < batch_count; k++)
                        free(batch[k].id_blob);
                    free(batch);
                    free(buf);
                    free(rowids);
                    SET_ERR(error, PCACHE_DEFRAGMENT_CANCELLED);
                    goto unlock;
                }
            }
        }
        free(batch);

        free(buf);
        free(rowids);
    }

cleanup_nulls:
    /* Remove all remaining NULL rows and reset in-memory state. */
    {
        int sqlite_return_code = db_exec(volume->db, "BEGIN IMMEDIATE");
        if (sqlite_return_code != SQLITE_OK) {
            SET_ERR(sqlite_error, sqlite_return_code);
            SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
            goto unlock;
        }
        sqlite_return_code = db_exec(volume->db, "DELETE FROM pages WHERE id_hash IS NULL");
        if (sqlite_return_code != SQLITE_OK) {
            SET_ERR(sqlite_error, sqlite_return_code);
            SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
            db_exec(volume->db, "ROLLBACK");
            goto unlock;
        }

        sqlite_return_code = db_exec(volume->db, "COMMIT");
        if (sqlite_return_code != SQLITE_OK) {
            SET_ERR(sqlite_error, sqlite_return_code);
            SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
            goto unlock;
        }
    }

    volume->row_count = live_cnt;

    if (shrink_file) {
        off_t new_size = (off_t)live_cnt * volume->config.page_size;
        if (ftruncate(volume->fd, new_size) != 0) {
            SET_ERR(posix_error, errno);
            SET_ERR(error, PCACHE_DEFRAGMENT_IO_ERROR);
            goto unlock;
        }
    }

    if (durable && error && *error == PCACHE_DEFRAGMENT_OK) {
        if (!sync_if_durable(volume, true, posix_error, sqlite_error))
            SET_ERR(error, PCACHE_DEFRAGMENT_IO_ERROR);
    }

unlock:
    pthread_mutex_unlock(&volume->mutex);
}

#undef DEFAGMENT_BATCH_SIZE

void pcache_set_max_pages(pcache_handle               handle,
                          uint32_t                    new_max_pages,
                          bool                        durable,
                          pcache_set_max_pages_error *error,
                          int                        *sqlite_error,
                          int                        *posix_error) {
    ZERO_3ERR(error, sqlite_error, posix_error);

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_SET_MAX_PAGES_INVALID_HANDLE);
        return;
    }

    uint32_t old_max = volume->config.max_pages;
    if (new_max_pages == old_max)
        goto unlock;

    if (new_max_pages < old_max) {
        if (volume->config.capacity_policy == PCACHE_CAPACITY_FIXED) {
            /* Count live pages beyond the new limit. */
            sqlite3_stmt *stmt;
            int64_t       live_beyond = 0;
            int           rc          = sqlite3_prepare_v2(
                volume->db, "SELECT COUNT(*) FROM pages WHERE id_hash IS NOT NULL AND rowid > ?", -1, &stmt, NULL);
            if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, (int64_t)new_max_pages);
            if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
                live_beyond = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_OK) {
                SET_ERR(sqlite_error, rc);
                SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
                goto unlock;
            }

            if (live_beyond > 0) {
                /* Fail only when the total live count exceeds the new limit. */
                int64_t total_live = 0;
                rc = sqlite3_prepare_v2(
                    volume->db, "SELECT COUNT(*) FROM pages WHERE id_hash IS NOT NULL", -1, &stmt, NULL);
                if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
                    total_live = sqlite3_column_int64(stmt, 0);
                sqlite3_finalize(stmt);
                if (rc != SQLITE_OK) {
                    SET_ERR(sqlite_error, rc);
                    SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
                    goto unlock;
                }
                if (total_live > (int64_t)new_max_pages) {
                    SET_ERR(error, PCACHE_SET_MAX_PAGES_WOULD_DISCARD_PAGES);
                    goto unlock;
                }

                /* Collect destination slots: NULL rows within [1, new_max_pages]. */
                int64_t *dst_slots = malloc((size_t)live_beyond * sizeof *dst_slots);
                if (!dst_slots) {
                    SET_ERR(error, PCACHE_SET_MAX_PAGES_OUT_OF_MEMORY);
                    goto unlock;
                }
                size_t dst_cnt = 0;

                rc = sqlite3_prepare_v2(volume->db,
                    "SELECT rowid FROM pages WHERE id_hash IS NULL AND rowid <= ? ORDER BY rowid ASC",
                    -1, &stmt, NULL);
                if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, (int64_t)new_max_pages);
                if (rc == SQLITE_OK) {
                    while (sqlite3_step(stmt) == SQLITE_ROW && dst_cnt < (size_t)live_beyond)
                        dst_slots[dst_cnt++] = sqlite3_column_int64(stmt, 0);
                }
                sqlite3_finalize(stmt);
                if (rc != SQLITE_OK) {
                    free(dst_slots);
                    SET_ERR(sqlite_error, rc);
                    SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
                    goto unlock;
                }

                /* Collect source pages: live rows beyond new_max_pages. */
                typedef struct {
                    int64_t rowid;
                    int64_t id_hash;
                    void   *id_blob;
                    int     id_len;
                } reloc_src_t;

                reloc_src_t *sources = malloc((size_t)live_beyond * sizeof *sources);
                if (!sources) {
                    free(dst_slots);
                    SET_ERR(error, PCACHE_SET_MAX_PAGES_OUT_OF_MEMORY);
                    goto unlock;
                }
                size_t src_cnt = 0;

                rc = sqlite3_prepare_v2(volume->db,
                    "SELECT rowid, id_hash, id FROM pages WHERE id_hash IS NOT NULL AND rowid > ? ORDER BY rowid ASC",
                    -1, &stmt, NULL);
                if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, (int64_t)new_max_pages);
                if (rc == SQLITE_OK) {
                    while (sqlite3_step(stmt) == SQLITE_ROW && src_cnt < (size_t)live_beyond) {
                        sources[src_cnt].rowid   = sqlite3_column_int64(stmt, 0);
                        sources[src_cnt].id_hash = sqlite3_column_int64(stmt, 1);
                        const void *blob     = sqlite3_column_blob(stmt, 2);
                        int         blob_len = sqlite3_column_bytes(stmt, 2);
                        sources[src_cnt].id_blob = NULL;
                        sources[src_cnt].id_len  = 0;
                        if (blob && blob_len > 0) {
                            sources[src_cnt].id_blob = malloc(blob_len);
                            if (!sources[src_cnt].id_blob) {
                                sqlite3_finalize(stmt);
                                for (size_t k = 0; k < src_cnt; k++)
                                    free(sources[k].id_blob);
                                free(sources);
                                free(dst_slots);
                                SET_ERR(error, PCACHE_SET_MAX_PAGES_OUT_OF_MEMORY);
                                goto unlock;
                            }
                            memcpy(sources[src_cnt].id_blob, blob, blob_len);
                            sources[src_cnt].id_len = blob_len;
                        }
                        src_cnt++;
                    }
                }
                sqlite3_finalize(stmt);
                if (rc != SQLITE_OK) {
                    for (size_t k = 0; k < src_cnt; k++)
                        free(sources[k].id_blob);
                    free(sources);
                    free(dst_slots);
                    SET_ERR(sqlite_error, rc);
                    SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
                    goto unlock;
                }

                /* Copy page data for each (src, dst) pair. */
                void *buf = malloc(volume->config.page_size);
                if (!buf) {
                    for (size_t k = 0; k < src_cnt; k++)
                        free(sources[k].id_blob);
                    free(sources);
                    free(dst_slots);
                    SET_ERR(error, PCACHE_SET_MAX_PAGES_OUT_OF_MEMORY);
                    goto unlock;
                }

                bool io_ok = true;
                for (size_t i = 0; i < src_cnt && io_ok; i++) {
                    off_t src_off = rowid_to_offset(sources[i].rowid, volume->config.page_size);
                    off_t dst_off = rowid_to_offset(dst_slots[i], volume->config.page_size);
                    if (pread(volume->fd, buf, volume->config.page_size, src_off) != (ssize_t)volume->config.page_size ||
                        pwrite(volume->fd, buf, volume->config.page_size, dst_off) != (ssize_t)volume->config.page_size) {
                        SET_ERR(posix_error, errno);
                        SET_ERR(error, PCACHE_SET_MAX_PAGES_IO_ERROR);
                        io_ok = false;
                    }
                }
                free(buf);

                if (!io_ok) {
                    for (size_t k = 0; k < src_cnt; k++)
                        free(sources[k].id_blob);
                    free(sources);
                    free(dst_slots);
                    goto unlock;
                }

                /* Update the index atomically: all relocations in one transaction. */
                int tx_rc = db_exec(volume->db, "BEGIN IMMEDIATE");
                if (tx_rc != SQLITE_OK) {
                    for (size_t k = 0; k < src_cnt; k++)
                        free(sources[k].id_blob);
                    free(sources);
                    free(dst_slots);
                    SET_ERR(sqlite_error, tx_rc);
                    SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
                    goto unlock;
                }

                sqlite3_stmt *del_stmt  = NULL;
                sqlite3_stmt *ins_stmt  = NULL;
                sqlite3_stmt *null_stmt = NULL;

                tx_rc = sqlite3_prepare_v2(volume->db,
                    "DELETE FROM pages WHERE rowid=? AND id_hash IS NULL", -1, &del_stmt, NULL);
                if (tx_rc == SQLITE_OK)
                    tx_rc = sqlite3_prepare_v2(volume->db,
                        "INSERT INTO pages(rowid, id_hash, id) VALUES(?, ?, ?)", -1, &ins_stmt, NULL);
                if (tx_rc == SQLITE_OK)
                    tx_rc = sqlite3_prepare_v2(volume->db,
                        "UPDATE pages SET id_hash=NULL, id=NULL WHERE rowid=?", -1, &null_stmt, NULL);

                for (size_t i = 0; i < src_cnt && tx_rc == SQLITE_OK; i++) {
                    sqlite3_reset(del_stmt);
                    sqlite3_clear_bindings(del_stmt);
                    sqlite3_bind_int64(del_stmt, 1, dst_slots[i]);
                    int step_rc = sqlite3_step(del_stmt);
                    if (step_rc != SQLITE_DONE)
                        tx_rc = sqlite3_errcode(volume->db);

                    if (tx_rc == SQLITE_OK) {
                        sqlite3_reset(ins_stmt);
                        sqlite3_clear_bindings(ins_stmt);
                        sqlite3_bind_int64(ins_stmt, 1, dst_slots[i]);
                        sqlite3_bind_int64(ins_stmt, 2, sources[i].id_hash);
                        if (sources[i].id_blob && sources[i].id_len > 0)
                            sqlite3_bind_blob(ins_stmt, 3, sources[i].id_blob, sources[i].id_len, SQLITE_STATIC);
                        else
                            sqlite3_bind_null(ins_stmt, 3);
                        step_rc = sqlite3_step(ins_stmt);
                        if (step_rc != SQLITE_DONE && step_rc != SQLITE_ROW)
                            tx_rc = sqlite3_errcode(volume->db);
                    }

                    if (tx_rc == SQLITE_OK) {
                        sqlite3_reset(null_stmt);
                        sqlite3_clear_bindings(null_stmt);
                        sqlite3_bind_int64(null_stmt, 1, sources[i].rowid);
                        step_rc = sqlite3_step(null_stmt);
                        if (step_rc != SQLITE_DONE)
                            tx_rc = sqlite3_errcode(volume->db);
                    }
                }

                sqlite3_finalize(del_stmt);
                sqlite3_finalize(ins_stmt);
                sqlite3_finalize(null_stmt);

                if (tx_rc == SQLITE_OK)
                    tx_rc = db_exec(volume->db, "COMMIT");
                else
                    db_exec(volume->db, "ROLLBACK");

                for (size_t k = 0; k < src_cnt; k++)
                    free(sources[k].id_blob);
                free(sources);
                free(dst_slots);

                if (tx_rc != SQLITE_OK) {
                    SET_ERR(sqlite_error, tx_rc);
                    SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
                    goto unlock;
                }
            } /* end if (live_beyond > 0) */

            /* Remove all rows beyond the new limit (now all NULL after relocation). */
            {
                int del_rc =
                    db_exec_bind_int64(volume->db, "DELETE FROM pages WHERE rowid > ?", (int64_t)new_max_pages);
                if (del_rc != SQLITE_OK) {
                    SET_ERR(sqlite_error, del_rc);
                    SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
                    goto unlock;
                }
            }

            if (volume->row_count > new_max_pages)
                volume->row_count = new_max_pages;

        } else { /* FIFO: fast path — drop rows beyond new limit, truncate, auto-recover. */
            int sqlite_return_code = db_exec(volume->db, "BEGIN IMMEDIATE");
            if (sqlite_return_code != SQLITE_OK) {
                SET_ERR(sqlite_error, sqlite_return_code);
                SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
                goto unlock;
            }

            int del_rc = db_exec_bind_int64(volume->db, "DELETE FROM pages WHERE rowid > ?", (int64_t)new_max_pages);
            if (del_rc != SQLITE_OK) {
                db_exec(volume->db, "ROLLBACK");
                SET_ERR(sqlite_error, del_rc);
                SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
                goto unlock;
            }

            /* Auto-recovery: if the surviving rows are all live, null slot 1
             * to restore the FIFO invariant (one empty slot in steady state). */
            sqlite3_stmt *count_stmt = NULL;
            int64_t       live_after = 0;
            int           prep_rc    = sqlite3_prepare_v2(
                volume->db, "SELECT COUNT(*) FROM pages WHERE id_hash IS NOT NULL", -1, &count_stmt, NULL);
            if (prep_rc != SQLITE_OK) {
                db_exec(volume->db, "ROLLBACK");
                SET_ERR(sqlite_error, prep_rc);
                SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
                goto unlock;
            }
            if (sqlite3_step(count_stmt) == SQLITE_ROW)
                live_after = sqlite3_column_int64(count_stmt, 0);
            sqlite3_finalize(count_stmt);

            if ((uint32_t)live_after == new_max_pages) {
                int recover_rc = db_exec(volume->db, "UPDATE pages SET id_hash=NULL, id=NULL WHERE rowid=1");
                if (recover_rc != SQLITE_OK) {
                    db_exec(volume->db, "ROLLBACK");
                    SET_ERR(sqlite_error, recover_rc);
                    SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
                    goto unlock;
                }
            }

            sqlite_return_code = db_exec(volume->db, "COMMIT");
            if (sqlite_return_code != SQLITE_OK) {
                db_exec(volume->db, "ROLLBACK");
                SET_ERR(sqlite_error, sqlite_return_code);
                SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
                goto unlock;
            }

            if (volume->row_count > new_max_pages)
                volume->row_count = new_max_pages;

            off_t new_size = (off_t)new_max_pages * volume->config.page_size;
            if (ftruncate(volume->fd, new_size) != 0) {
                SET_ERR(posix_error, errno);
                SET_ERR(error, PCACHE_SET_MAX_PAGES_IO_ERROR);
                goto unlock;
            }
        }
    }

    /* ── Persist the new max_pages value ── */
    {
        int sqlite_return_code = db_exec(volume->db, "BEGIN IMMEDIATE");
        if (sqlite_return_code == SQLITE_OK) {
            bool ok = db_meta_update_u32(volume->db, "max_pages", new_max_pages);
            if (ok)
                sqlite_return_code = db_exec(volume->db, "COMMIT");
            else {
                db_exec(volume->db, "ROLLBACK");
                sqlite_return_code = sqlite3_errcode(volume->db);
            }
        }
        if (sqlite_return_code != SQLITE_OK) {
            SET_ERR(sqlite_error, sqlite_return_code);
            SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
            goto unlock;
        }
    }

    volume->config.max_pages = new_max_pages;

    if (durable) {
        if (!sync_if_durable(volume, true, posix_error, sqlite_error))
            SET_ERR(error, PCACHE_SET_MAX_PAGES_IO_ERROR);
    }

unlock:
    pthread_mutex_unlock(&volume->mutex);
}

void pcache_preallocate(pcache_handle             handle,
                        bool                      preallocate_database,
                        bool                      preallocate_datafile,
                        bool                      durable,
                        pcache_preallocate_error *error,
                        int                      *sqlite_error,
                        int                      *posix_error) {
    ZERO_3ERR(error, sqlite_error, posix_error);

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_PREALLOCATE_INVALID_HANDLE);
        return;
    }

    size_t max   = volume->config.max_pages;
    size_t start = volume->row_count + 1; /* first ROWID not yet in the table */

    /* Guard: only insert rows if they don't already exist in the table.
     * Handles the case where row_count was not updated (e.g., after a
     * capacity increase that didn't preallocate). */
    if (preallocate_database && start <= max) {
        int sqlite_return_code = db_exec(volume->db, "BEGIN");
        if (sqlite_return_code == SQLITE_OK) {
            sqlite3_stmt *insert_stmt;
            sqlite_return_code = sqlite3_prepare_v2(
                volume->db, "INSERT INTO pages(id_hash,id) VALUES(NULL,NULL)", -1, &insert_stmt, NULL);
            if (sqlite_return_code == SQLITE_OK) {
                for (size_t i = start; i <= max && sqlite_return_code == SQLITE_OK; i++) {
                    sqlite_return_code = sqlite3_step(insert_stmt);
                    if (sqlite_return_code == SQLITE_DONE)
                        sqlite_return_code = SQLITE_OK;
                    sqlite3_reset(insert_stmt);
                }
                sqlite3_finalize(insert_stmt);
            }
            if (sqlite_return_code == SQLITE_OK)
                sqlite_return_code = db_exec(volume->db, "COMMIT");
            else
                db_exec(volume->db, "ROLLBACK");
        }
        if (sqlite_return_code != SQLITE_OK) {
            SET_ERR(sqlite_error, sqlite_return_code);
            SET_ERR(error, PCACHE_PREALLOCATE_SQLITE_ERROR);
            goto unlock;
        }

        volume->row_count = max;
    }

    if (preallocate_datafile) {
        off_t target = (off_t)max * volume->config.page_size;
        if (ftruncate(volume->fd, target) != 0) {
            SET_ERR(posix_error, errno);
            SET_ERR(error, PCACHE_PREALLOCATE_IO_ERROR);
            goto unlock;
        }
    }

    if (durable) {
        if (!sync_if_durable(volume, true, posix_error, sqlite_error))
            SET_ERR(error, PCACHE_PREALLOCATE_IO_ERROR);
    }

unlock:
    pthread_mutex_unlock(&volume->mutex);
}
