#include "db.h"
#include "handle.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

void pcache_defragment(pcache_handle            handle,
                       pcache_progress_fn       progress_callback,
                       void                    *progress_user_data,
                       bool                     shrink_file,
                       bool                     durable,
                       pcache_defragment_error *error,
                       int                     *sqlite_error,
                       int                     *posix_error) {
    SET_ERR(error, PCACHE_DEFRAGMENT_OK);
    SET_ERR(sqlite_error, SQLITE_OK);
    SET_ERR(posix_error, 0);

    pcache_volume *v = vol_from_handle(handle);
    if (!v) {
        SET_ERR(error, PCACHE_DEFRAGMENT_INVALID_HANDLE);
        return;
    }

    /* ── Load live ROWIDs into memory ── */
    int64_t *rowids          = NULL;
    uint32_t live_cnt        = 0;
    uint32_t live_cap        = 0;
    int64_t  saved_fifo_next = v->fifo_next; /* restore on cancel */

    {
        sqlite3_stmt *s;
        int           rc = sqlite3_prepare_v2(
            v->db, "SELECT rowid FROM pages WHERE id_hash IS NOT NULL ORDER BY rowid ASC", -1, &s, NULL);
        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
            goto unlock;
        }
        while (sqlite3_step(s) == SQLITE_ROW) {
            if (live_cnt >= live_cap) {
                uint32_t cap = live_cap ? live_cap * 2 : 64;
                int64_t *p   = realloc(rowids, cap * sizeof *p);
                if (!p) {
                    sqlite3_finalize(s);
                    free(rowids);
                    goto unlock; /* OOM — leave volume unchanged */
                }
                rowids   = p;
                live_cap = cap;
            }
            rowids[live_cnt++] = sqlite3_column_int64(s, 0);
        }
        sqlite3_finalize(s);
    }

    if (live_cnt == 0) {
        free(rowids);
        goto cleanup_nulls;
    }

    /* ── Relocate pages one by one ── */
    {
        void *buf = malloc(v->config.page_size);
        if (!buf) {
            free(rowids);
            goto unlock;
        }

        for (uint32_t i = 0; i < live_cnt; i++) {
            int64_t r            = rowids[i];
            int64_t next_compact = (int64_t)(i + 1);

            if (r != next_compact) {
                off_t src = (off_t)(r - 1) * v->config.page_size;
                off_t dst = (off_t)(next_compact - 1) * v->config.page_size;

                if (pread(v->data_fd, buf, v->config.page_size, src) != (ssize_t)v->config.page_size) {
                    SET_ERR(posix_error, errno);
                    SET_ERR(error, PCACHE_DEFRAGMENT_IO_ERROR);
                    free(buf);
                    free(rowids);
                    goto unlock;
                }
                if (pwrite(v->data_fd, buf, v->config.page_size, dst) != (ssize_t)v->config.page_size) {
                    SET_ERR(posix_error, errno);
                    SET_ERR(error, PCACHE_DEFRAGMENT_IO_ERROR);
                    free(buf);
                    free(rowids);
                    goto unlock;
                }

                /* Atomically remove the gap row at next_compact (if any) and
                 * slide the live row from r down to next_compact. */
                int rc = db_exec(v->db, "BEGIN IMMEDIATE");
                if (rc == SQLITE_OK) {
                    sqlite3_stmt *s;
                    rc = sqlite3_prepare_v2(v->db, "DELETE FROM pages WHERE rowid=? AND id_hash IS NULL", -1, &s, NULL);
                    if (rc == SQLITE_OK) {
                        sqlite3_bind_int64(s, 1, next_compact);
                        sqlite3_step(s);
                        sqlite3_finalize(s);
                    }
                    rc = sqlite3_prepare_v2(v->db, "UPDATE pages SET rowid=? WHERE rowid=?", -1, &s, NULL);
                    if (rc == SQLITE_OK) {
                        sqlite3_bind_int64(s, 1, next_compact);
                        sqlite3_bind_int64(s, 2, r);
                        rc = sqlite3_step(s);
                        if (rc == SQLITE_DONE)
                            rc = SQLITE_OK;
                        sqlite3_finalize(s);
                    }
                    if (rc == SQLITE_OK)
                        rc = db_exec(v->db, "COMMIT");
                    else
                        db_exec(v->db, "ROLLBACK");
                }
                if (rc != SQLITE_OK) {
                    SET_ERR(sqlite_error, rc);
                    SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
                    free(buf);
                    free(rowids);
                    goto unlock;
                }
            }

            if (progress_callback) {
                double frac = (double)(i + 1) / live_cnt;
                if (!progress_callback(frac, progress_user_data)) {
                    free(buf);
                    free(rowids);
                    v->fifo_next = saved_fifo_next; /* restore on cancel */
                    SET_ERR(error, PCACHE_DEFRAGMENT_CANCELLED);
                    goto unlock;
                }
            }
        }

        free(buf);
        free(rowids);
    }

cleanup_nulls:
    /* Remove all remaining NULL rows and reset in-memory state. */
    {
        int rc = db_exec(v->db, "BEGIN IMMEDIATE");
        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
            goto unlock;
        }
        rc = db_exec(v->db, "DELETE FROM pages WHERE id_hash IS NULL");
        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
            db_exec(v->db, "ROLLBACK");
            goto unlock;
        }

        /* Reset FIFO cursor to the slot immediately after the last live page. */
        if (v->config.capacity_policy == PCACHE_CAPACITY_FIFO) {
            int64_t       new_next = (int64_t)((live_cnt % v->config.max_pages) + 1);
            sqlite3_stmt *s;
            rc = sqlite3_prepare_v2(v->db, "UPDATE fifo_cursor SET next_rowid=?", -1, &s, NULL);
            if (rc == SQLITE_OK) {
                sqlite3_bind_int64(s, 1, new_next);
                rc = sqlite3_step(s);
                if (rc == SQLITE_DONE)
                    rc = SQLITE_OK;
                sqlite3_finalize(s);
            }
            if (rc == SQLITE_OK) {
                v->fifo_next = new_next;
            }
            if (rc != SQLITE_OK) {
                SET_ERR(sqlite_error, rc);
                SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
                db_exec(v->db, "ROLLBACK");
                goto unlock;
            }
        }

        rc = db_exec(v->db, "COMMIT");
        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_DEFRAGMENT_SQLITE_ERROR);
            goto unlock;
        }
    }

    v->row_count = live_cnt;
    rv_free(&v->free_list); /* no free slots remain after full compaction */

    if (shrink_file) {
        off_t new_size = (off_t)live_cnt * v->config.page_size;
        if (ftruncate(v->data_fd, new_size) != 0) {
            SET_ERR(posix_error, errno);
            SET_ERR(error, PCACHE_DEFRAGMENT_IO_ERROR);
            goto unlock;
        }
    }

    if (durable && error && *error == PCACHE_DEFRAGMENT_OK) {
        if (!do_sync(v)) {
            SET_ERR(posix_error, errno);
            SET_ERR(error, PCACHE_DEFRAGMENT_IO_ERROR);
        }
    }

unlock:
    pthread_mutex_unlock(&v->mutex);
}

void pcache_set_max_pages(pcache_handle               handle,
                          uint32_t                    new_max_pages,
                          bool                        durable,
                          pcache_set_max_pages_error *error,
                          int                        *sqlite_error,
                          int                        *posix_error) {
    SET_ERR(error, PCACHE_SET_MAX_PAGES_OK);
    SET_ERR(sqlite_error, SQLITE_OK);
    SET_ERR(posix_error, 0);

    pcache_volume *v = vol_from_handle(handle);
    if (!v) {
        SET_ERR(error, PCACHE_SET_MAX_PAGES_INVALID_HANDLE);
        return;
    }

    uint32_t old_max = v->config.max_pages;
    if (new_max_pages == old_max)
        goto unlock;

    if (new_max_pages < old_max) {
        if (v->config.capacity_policy == PCACHE_CAPACITY_FIXED) {
            /* Fail if any live page resides beyond the new limit. */
            sqlite3_stmt *s;
            int64_t       beyond = 0;
            int           rc     = sqlite3_prepare_v2(
                v->db, "SELECT COUNT(*) FROM pages WHERE id_hash IS NOT NULL AND rowid > ?", -1, &s, NULL);
            if (rc == SQLITE_OK) {
                sqlite3_bind_int64(s, 1, (int64_t)new_max_pages);
                if (sqlite3_step(s) == SQLITE_ROW)
                    beyond = sqlite3_column_int64(s, 0);
                sqlite3_finalize(s);
            }
            if (beyond > 0) {
                SET_ERR(error, PCACHE_SET_MAX_PAGES_WOULD_DISCARD_PAGES);
                goto unlock;
            }

            /* Remove NULL rows beyond the new limit. */
            rc = sqlite3_prepare_v2(v->db, "DELETE FROM pages WHERE rowid > ?", -1, &s, NULL);
            if (rc == SQLITE_OK) {
                sqlite3_bind_int64(s, 1, (int64_t)new_max_pages);
                sqlite3_step(s);
                sqlite3_finalize(s);
            }

            /* Trim the free list to entries within the new limit. */
            rowid_vec new_fl = {0};
            for (size_t i = 0; i < v->free_list.count; i++) {
                if (v->free_list.data[i] <= (int64_t)new_max_pages)
                    rv_push(&new_fl, v->free_list.data[i]);
            }
            rv_free(&v->free_list);
            v->free_list = new_fl;

            if (v->row_count > new_max_pages)
                v->row_count = new_max_pages;

        } else { /* FIFO: evict oldest pages in circular order until within limit */
            uint32_t      live = 0;
            sqlite3_stmt *s;
            if (sqlite3_prepare_v2(v->db, "SELECT COUNT(*) FROM pages WHERE id_hash IS NOT NULL", -1, &s, NULL) ==
                SQLITE_OK) {
                if (sqlite3_step(s) == SQLITE_ROW)
                    live = (uint32_t)sqlite3_column_int64(s, 0);
                sqlite3_finalize(s);
            }

            int64_t cursor = v->fifo_next;
            if (live > new_max_pages) {
                uint32_t excess = live - new_max_pages;
                for (uint32_t evicted = 0; evicted < excess;) {
                    sqlite3_stmt *cs;
                    bool          is_live = false;
                    int           rc      = sqlite3_prepare_v2(
                        v->db, "SELECT 1 FROM pages WHERE rowid=? AND id_hash IS NOT NULL", -1, &cs, NULL);
                    if (rc == SQLITE_OK) {
                        sqlite3_bind_int64(cs, 1, cursor);
                        is_live = (sqlite3_step(cs) == SQLITE_ROW);
                        sqlite3_finalize(cs);
                    }
                    if (is_live) {
                        sqlite3_stmt *us;
                        rc = sqlite3_prepare_v2(
                            v->db, "UPDATE pages SET id_hash=NULL,id=NULL WHERE rowid=?", -1, &us, NULL);
                        if (rc == SQLITE_OK) {
                            sqlite3_bind_int64(us, 1, cursor);
                            sqlite3_step(us);
                            sqlite3_finalize(us);
                        }
                        evicted++;
                    }
                    cursor = (cursor % (int64_t)old_max) + 1;
                    if (cursor == v->fifo_next)
                        break; /* safety guard: full circle */
                }
            }

            /* Delete all rows (including live ones) beyond the new limit, and
             * clamp the FIFO cursor to remain within the new boundary. */
            {
                sqlite3_stmt *ds;
                int           rc = sqlite3_prepare_v2(v->db, "DELETE FROM pages WHERE rowid > ?", -1, &ds, NULL);
                if (rc == SQLITE_OK) {
                    sqlite3_bind_int64(ds, 1, (int64_t)new_max_pages);
                    sqlite3_step(ds);
                    sqlite3_finalize(ds);
                }
            }

            /* The local `cursor` has advanced past every evicted slot, so it
             * now points at the new oldest live page (or, when all live pages
             * were evicted, at the slot that would have been evicted next).
             * Use it — not the original v->fifo_next — as the basis for the
             * clamp, otherwise subsequent evictions may skip the true oldest
             * live page. */
            int64_t new_fifo = (live > new_max_pages) ? cursor : v->fifo_next;
            if (new_fifo > (int64_t)new_max_pages)
                new_fifo = ((new_fifo - 1) % (int64_t)new_max_pages) + 1;
            v->fifo_next = new_fifo;

            /* Persist the updated cursor. */
            {
                sqlite3_stmt *cs;
                int           rc = sqlite3_prepare_v2(v->db, "UPDATE fifo_cursor SET next_rowid=?", -1, &cs, NULL);
                if (rc == SQLITE_OK) {
                    sqlite3_bind_int64(cs, 1, new_fifo);
                    sqlite3_step(cs);
                    sqlite3_finalize(cs);
                }
            }

            if (v->row_count > new_max_pages)
                v->row_count = new_max_pages;
        }
    }

    /* ── Persist the new max_pages value ── */
    {
        int rc = db_exec(v->db, "BEGIN IMMEDIATE");
        if (rc == SQLITE_OK) {
            bool ok = db_meta_update_u32(v->db, "max_pages", new_max_pages);
            if (ok)
                rc = db_exec(v->db, "COMMIT");
            else {
                db_exec(v->db, "ROLLBACK");
                rc = sqlite3_errcode(v->db);
            }
        }
        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_SET_MAX_PAGES_SQLITE_ERROR);
            goto unlock;
        }
    }

    v->config.max_pages = new_max_pages;

    if (durable && !do_sync(v)) {
        SET_ERR(posix_error, errno);
        SET_ERR(error, PCACHE_SET_MAX_PAGES_IO_ERROR);
    }

unlock:
    pthread_mutex_unlock(&v->mutex);
}

void pcache_preallocate(pcache_handle             handle,
                        bool                      preallocate_database,
                        bool                      preallocate_datafile,
                        bool                      durable,
                        pcache_preallocate_error *error,
                        int                      *sqlite_error,
                        int                      *posix_error) {
    SET_ERR(error, PCACHE_PREALLOCATE_OK);
    SET_ERR(sqlite_error, SQLITE_OK);
    SET_ERR(posix_error, 0);

    pcache_volume *v = vol_from_handle(handle);
    if (!v) {
        SET_ERR(error, PCACHE_PREALLOCATE_INVALID_HANDLE);
        return;
    }

    uint32_t max   = v->config.max_pages;
    uint32_t start = v->row_count + 1; /* first ROWID not yet in the table */

    /* Guard: only insert rows if they don't already exist in the table.
     * Handles the case where row_count was not updated (e.g., after a
     * capacity increase that didn't preallocate). */
    if (preallocate_database && start <= max) {
        int rc = db_exec(v->db, "BEGIN");
        if (rc == SQLITE_OK) {
            sqlite3_stmt *s;
            rc = sqlite3_prepare_v2(v->db, "INSERT INTO pages(id_hash,id) VALUES(NULL,NULL)", -1, &s, NULL);
            if (rc == SQLITE_OK) {
                for (uint32_t i = start; i <= max && rc == SQLITE_OK; i++) {
                    rc = sqlite3_step(s);
                    if (rc == SQLITE_DONE)
                        rc = SQLITE_OK;
                    sqlite3_reset(s);
                }
                sqlite3_finalize(s);
            }
            if (rc == SQLITE_OK)
                rc = db_exec(v->db, "COMMIT");
            else
                db_exec(v->db, "ROLLBACK");
        }
        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_PREALLOCATE_SQLITE_ERROR);
            goto unlock;
        }

        /* Add newly created NULL slots to the FIXED free list. */
        if (v->config.capacity_policy == PCACHE_CAPACITY_FIXED) {
            for (uint32_t i = start; i <= max; i++)
                rv_push(&v->free_list, (int64_t)i);
        }

        v->row_count = max;
    }

    if (preallocate_datafile) {
        off_t target = (off_t)max * v->config.page_size;
        if (ftruncate(v->data_fd, target) != 0) {
            SET_ERR(posix_error, errno);
            SET_ERR(error, PCACHE_PREALLOCATE_IO_ERROR);
            goto unlock;
        }
    }

    if (durable && !do_sync(v)) {
        SET_ERR(posix_error, errno);
        SET_ERR(error, PCACHE_PREALLOCATE_IO_ERROR);
    }

unlock:
    pthread_mutex_unlock(&v->mutex);
}
