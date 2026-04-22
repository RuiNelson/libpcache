#include "db.h"
#include "handle.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <xxhash.h>

void pcache_put_page(pcache_handle          handle,
                     const void            *id,
                     const void            *page_data,
                     bool                   check_id_uniqueness,
                     bool                   durable,
                     pcache_put_page_error *error,
                     int                   *sqlite_error,
                     int                   *posix_error) {
    SET_ERR(error, PCACHE_PUT_PAGE_OK);
    SET_ERR(sqlite_error, SQLITE_OK);
    SET_ERR(posix_error, 0);

    pcache_volume *v = vol_from_handle(handle);
    if (!v) {
        SET_ERR(error, PCACHE_PUT_PAGE_INVALID_HANDLE);
        return;
    }

    /* ── Uniqueness check ── */
    if (check_id_uniqueness) {
        int     sq  = SQLITE_OK;
        int64_t rid = find_rowid(v, id, &sq);
        if (rid < 0) {
            SET_ERR(sqlite_error, sq);
            SET_ERR(error, PCACHE_PUT_PAGE_SQLITE_ERROR);
            goto unlock;
        }
        if (rid > 0) {
            SET_ERR(error, PCACHE_PUT_PAGE_DUPLICATE_ID);
            goto unlock;
        }
    }

    /* ── Determine target ROWID and write mode ── */
    int64_t target_rowid = 0;
    bool    do_insert    = false;

    if (v->config.capacity_policy == PCACHE_CAPACITY_FIXED) {
        if (v->free_list.count > 0) {
            target_rowid = rv_pop(&v->free_list);
        } else if (v->row_count < v->config.max_pages) {
            do_insert = true;
        } else {
            /* Lazy: query for a NULL slot when the free list was not preloaded. */
            sqlite3_stmt *s;
            if (sqlite3_prepare_v2(v->db, "SELECT rowid FROM pages WHERE id_hash IS NULL LIMIT 1", -1, &s, NULL) ==
                SQLITE_OK) {
                if (sqlite3_step(s) == SQLITE_ROW)
                    target_rowid = sqlite3_column_int64(s, 0);
                sqlite3_finalize(s);
            }
            if (target_rowid == 0) {
                SET_ERR(error, PCACHE_PUT_PAGE_CAPACITY_EXCEEDED);
                goto unlock;
            }
        }
    } else { /* FIFO */
        if (v->row_count < v->config.max_pages) {
            do_insert = true;
        } else {
            target_rowid = v->fifo_next;
        }
    }

    /* ── SQLite transaction ── */
    uint32_t      h = XXH32(id, v->config.id_size, 0);
    sqlite3_stmt *s = NULL;
    int           rc;

    rc = db_exec(v->db, "BEGIN IMMEDIATE");
    if (rc != SQLITE_OK) {
        SET_ERR(sqlite_error, rc);
        SET_ERR(error, PCACHE_PUT_PAGE_SQLITE_ERROR);
        goto unlock;
    }

    if (do_insert) {
        rc = sqlite3_prepare_v2(v->db, "INSERT INTO pages(id_hash,id) VALUES(?,?)", -1, &s, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(s, 1, (int64_t)(uint32_t)h);
            sqlite3_bind_blob(s, 2, id, (int)v->config.id_size, SQLITE_STATIC);
            rc = sqlite3_step(s);
            if (rc == SQLITE_DONE) {
                target_rowid = sqlite3_last_insert_rowid(v->db);
                rc           = SQLITE_OK;
            }
            sqlite3_finalize(s);
            s = NULL;
        }
    } else {
        rc = sqlite3_prepare_v2(v->db, "UPDATE pages SET id_hash=?,id=? WHERE rowid=?", -1, &s, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(s, 1, (int64_t)(uint32_t)h);
            sqlite3_bind_blob(s, 2, id, (int)v->config.id_size, SQLITE_STATIC);
            sqlite3_bind_int64(s, 3, target_rowid);
            rc = sqlite3_step(s);
            if (rc == SQLITE_DONE)
                rc = SQLITE_OK;
            sqlite3_finalize(s);
            s = NULL;
        }
    }

    /* Advance the FIFO cursor only on replacement (not on initial insert). */
    if (rc == SQLITE_OK && v->config.capacity_policy == PCACHE_CAPACITY_FIFO && !do_insert) {
        /* Spec: update to (next_rowid % max_pages) + 1 based on fifo_next, not target_rowid */
        int64_t new_next = (v->fifo_next % (int64_t)v->config.max_pages) + 1;
        rc               = sqlite3_prepare_v2(v->db, "UPDATE fifo_cursor SET next_rowid=?", -1, &s, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(s, 1, new_next);
            rc = sqlite3_step(s);
            if (rc == SQLITE_DONE)
                rc = SQLITE_OK;
            sqlite3_finalize(s);
            s = NULL;
        }
    }

    if (rc != SQLITE_OK) {
        SET_ERR(sqlite_error, rc);
        SET_ERR(error, PCACHE_PUT_PAGE_SQLITE_ERROR);
        db_exec(v->db, "ROLLBACK");
        goto unlock;
    }

    /* ── Write page data ── */
    {
        off_t   byte_offset = (off_t)(target_rowid - 1) * v->config.page_size;
        ssize_t written     = pwrite(v->data_fd, page_data, v->config.page_size, byte_offset);
        if (written != (ssize_t)v->config.page_size) {
            SET_ERR(posix_error, errno);
            SET_ERR(error, PCACHE_PUT_PAGE_IO_ERROR);
            db_exec(v->db, "ROLLBACK");
            goto unlock;
        }
    }

    rc = db_exec(v->db, "COMMIT");
    if (rc != SQLITE_OK) {
        SET_ERR(sqlite_error, rc);
        SET_ERR(error, PCACHE_PUT_PAGE_SQLITE_ERROR);
        goto unlock;
    }

    /* ── Update in-memory state ── */
    if (do_insert)
        v->row_count++;
    if (v->config.capacity_policy == PCACHE_CAPACITY_FIFO && !do_insert)
        v->fifo_next = (v->fifo_next % (int64_t)v->config.max_pages) + 1;

    if (durable && error && *error == PCACHE_PUT_PAGE_OK) {
        if (!do_sync(v)) {
            SET_ERR(posix_error, errno);
            SET_ERR(error, PCACHE_PUT_PAGE_IO_ERROR);
        }
    }

unlock:
    pthread_mutex_unlock(&v->mutex);
}

void pcache_get_page(pcache_handle          handle,
                     const void            *id,
                     void                  *page_buffer,
                     pcache_get_page_error *error,
                     int                   *sqlite_error,
                     int                   *posix_error) {
    SET_ERR(error, PCACHE_GET_PAGE_OK);
    SET_ERR(sqlite_error, SQLITE_OK);
    SET_ERR(posix_error, 0);

    pcache_volume *v = vol_from_handle(handle);
    if (!v) {
        SET_ERR(error, PCACHE_GET_PAGE_INVALID_HANDLE);
        return;
    }

    int     sq    = SQLITE_OK;
    int64_t rowid = find_rowid(v, id, &sq);
    if (rowid < 0) {
        SET_ERR(sqlite_error, sq);
        SET_ERR(error, PCACHE_GET_PAGE_SQLITE_ERROR);
        goto unlock;
    }
    if (rowid == 0) {
        SET_ERR(error, PCACHE_GET_PAGE_NOT_FOUND);
        goto unlock;
    }

    {
        off_t   byte_offset = (off_t)(rowid - 1) * v->config.page_size;
        ssize_t rd          = pread(v->data_fd, page_buffer, v->config.page_size, byte_offset);
        if (rd != (ssize_t)v->config.page_size) {
            SET_ERR(posix_error, errno);
            SET_ERR(error, PCACHE_GET_PAGE_IO_ERROR);
        }
    }

unlock:
    pthread_mutex_unlock(&v->mutex);
}

void pcache_put_pages(pcache_handle           handle,
                      size_t                  count,
                      const void             *ids,
                      const void             *pages_data,
                      bool                    check_id_uniqueness,
                      bool                    durable,
                      pcache_put_pages_error *error,
                      int                    *sqlite_error,
                      int                    *posix_error) {
    SET_ERR(error, PCACHE_PUT_PAGES_OK);
    SET_ERR(sqlite_error, SQLITE_OK);
    SET_ERR(posix_error, 0);

    if (count == 0)
        return;

    pcache_volume *v = vol_from_handle(handle);
    if (!v) {
        SET_ERR(error, PCACHE_PUT_PAGES_INVALID_HANDLE);
        return;
    }

    const uint32_t id_size   = v->config.id_size;
    const uint32_t page_size = v->config.page_size;

    /* ── Uniqueness check ── */
    if (check_id_uniqueness) {
        int sq = SQLITE_OK;

        /* Check for duplicates within the batch */
        for (size_t i = 0; i < count; i++) {
            const void *id_i = (const uint8_t *)ids + i * id_size;
            for (size_t j = i + 1; j < count; j++) {
                const void *id_j = (const uint8_t *)ids + j * id_size;
                if (memcmp(id_i, id_j, id_size) == 0) {
                    SET_ERR(error, PCACHE_PUT_PAGES_DUPLICATE_ID);
                    goto unlock;
                }
            }
        }

        /* Check for duplicates against existing pages */
        for (size_t i = 0; i < count; i++) {
            const void *id  = (const uint8_t *)ids + i * id_size;
            int64_t     rid = find_rowid(v, id, &sq);
            if (rid < 0) {
                SET_ERR(sqlite_error, sq);
                SET_ERR(error, PCACHE_PUT_PAGES_SQLITE_ERROR);
                goto unlock;
            }
            if (rid > 0) {
                SET_ERR(error, PCACHE_PUT_PAGES_DUPLICATE_ID);
                goto unlock;
            }
        }
    }

    /* ── Determine target ROWIDs for all pages ── */
    int64_t *target_rowids = calloc(count, sizeof(int64_t));
    bool    *do_insert     = calloc(count, sizeof(bool));
    if (!target_rowids || !do_insert) {
        free(target_rowids);
        free(do_insert);
        SET_ERR(error, PCACHE_PUT_PAGES_SQLITE_ERROR);
        goto unlock;
    }

    /* Bug-fix 1: use a local cursor so v->fifo_next is only advanced after COMMIT. */
    /* Bug-fix 2: track inserts_planned separately; i includes free-list iterations  */
    /*            that don't consume row-count capacity.                              */
    uint32_t inserts_planned = 0;
    int64_t  fifo_cursor     = v->fifo_next;

    for (size_t i = 0; i < count; i++) {
        if (v->config.capacity_policy == PCACHE_CAPACITY_FIXED) {
            if (v->free_list.count > 0) {
                target_rowids[i] = rv_pop(&v->free_list);
            } else if (v->row_count + inserts_planned < v->config.max_pages) {
                do_insert[i] = true;
                inserts_planned++;
            } else {
                /* Bug-fix 3: loop until we find a NULL slot not already claimed by   */
                /* an earlier iteration (free-list pop or prior lazy query).           */
                int64_t after = 0;
                for (;;) {
                    sqlite3_stmt *sq;
                    int64_t       cand = 0;
                    if (sqlite3_prepare_v2(v->db,
                            "SELECT rowid FROM pages WHERE id_hash IS NULL AND rowid > ?"
                            " ORDER BY rowid LIMIT 1",
                            -1, &sq, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(sq, 1, after);
                        if (sqlite3_step(sq) == SQLITE_ROW)
                            cand = sqlite3_column_int64(sq, 0);
                        sqlite3_finalize(sq);
                    }
                    if (cand == 0)
                        break;
                    bool taken = false;
                    for (size_t k = 0; k < i; k++) {
                        if (!do_insert[k] && target_rowids[k] == cand) {
                            taken = true;
                            after = cand;
                            break;
                        }
                    }
                    if (!taken) {
                        target_rowids[i] = cand;
                        break;
                    }
                }
                if (target_rowids[i] == 0) {
                    SET_ERR(error, PCACHE_PUT_PAGES_CAPACITY_EXCEEDED);
                    free(target_rowids);
                    free(do_insert);
                    goto unlock;
                }
            }
        } else { /* FIFO */
            if (v->row_count + (uint32_t)i < v->config.max_pages) {
                do_insert[i] = true;
            } else {
                target_rowids[i] = fifo_cursor;
                fifo_cursor      = (fifo_cursor % (int64_t)v->config.max_pages) + 1;
            }
        }
    }

    /* ── SQLite transaction ── */
    int rc = db_exec(v->db, "BEGIN IMMEDIATE");
    if (rc != SQLITE_OK) {
        SET_ERR(sqlite_error, rc);
        SET_ERR(error, PCACHE_PUT_PAGES_SQLITE_ERROR);
        free(target_rowids);
        free(do_insert);
        goto unlock;
    }

    sqlite3_stmt *s_ins  = NULL;
    sqlite3_stmt *s_upd  = NULL;
    sqlite3_stmt *s_fifo = NULL;
    bool          tx_ok  = true;

    rc = sqlite3_prepare_v2(v->db, "INSERT INTO pages(id_hash,id) VALUES(?,?)", -1, &s_ins, NULL);
    if (rc != SQLITE_OK) {
        SET_ERR(sqlite_error, rc);
        tx_ok = false;
    }

    if (tx_ok) {
        rc = sqlite3_prepare_v2(v->db, "UPDATE pages SET id_hash=?,id=? WHERE rowid=?", -1, &s_upd, NULL);
        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
            tx_ok = false;
        }
    }

    size_t inserts_done = 0;
    size_t updates_done = 0;

    for (size_t i = 0; i < count && tx_ok; i++) {
        const void   *id   = (const uint8_t *)ids + i * id_size;
        uint32_t      h    = XXH32(id, id_size, 0);
        sqlite3_stmt *stmt = do_insert[i] ? s_ins : s_upd;

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, (int64_t)(uint32_t)h);
        sqlite3_bind_blob(stmt, 2, id, (int)id_size, SQLITE_STATIC);

        if (!do_insert[i]) {
            sqlite3_bind_int64(stmt, 3, target_rowids[i]);
        }

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_PUT_PAGES_SQLITE_ERROR);
            tx_ok = false;
        } else {
            if (do_insert[i]) {
                target_rowids[i] = sqlite3_last_insert_rowid(v->db);
                inserts_done++;
            } else {
                updates_done++;
            }
        }
    }

    /* Update FIFO cursor if any replacements were done */
    if (tx_ok && v->config.capacity_policy == PCACHE_CAPACITY_FIFO && updates_done > 0) {
        rc = sqlite3_prepare_v2(v->db, "UPDATE fifo_cursor SET next_rowid=?", -1, &s_fifo, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(s_fifo, 1, fifo_cursor);
            rc = sqlite3_step(s_fifo);
            if (rc != SQLITE_DONE) {
                SET_ERR(sqlite_error, rc);
                SET_ERR(error, PCACHE_PUT_PAGES_SQLITE_ERROR);
                tx_ok = false;
            }
            sqlite3_finalize(s_fifo);
        } else {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_PUT_PAGES_SQLITE_ERROR);
            tx_ok = false;
        }
    }

    sqlite3_finalize(s_ins);
    sqlite3_finalize(s_upd);

    if (!tx_ok) {
        db_exec(v->db, "ROLLBACK");
        free(target_rowids);
        free(do_insert);
        goto unlock;
    }

    /* ── Write page data ── */
    for (size_t i = 0; i < count; i++) {
        const void *page        = (const uint8_t *)pages_data + i * page_size;
        off_t       byte_offset = (off_t)(target_rowids[i] - 1) * page_size;
        ssize_t     written     = pwrite(v->data_fd, page, page_size, byte_offset);
        if (written != (ssize_t)page_size) {
            SET_ERR(posix_error, errno);
            SET_ERR(error, PCACHE_PUT_PAGES_IO_ERROR);
            db_exec(v->db, "ROLLBACK");
            free(target_rowids);
            free(do_insert);
            goto unlock;
        }
    }

    rc = db_exec(v->db, "COMMIT");
    if (rc != SQLITE_OK) {
        SET_ERR(sqlite_error, rc);
        SET_ERR(error, PCACHE_PUT_PAGES_SQLITE_ERROR);
        free(target_rowids);
        free(do_insert);
        goto unlock;
    }

    /* ── Update in-memory state ── */
    v->row_count += (uint32_t)inserts_done;
    if (v->config.capacity_policy == PCACHE_CAPACITY_FIFO)
        v->fifo_next = fifo_cursor;

    free(target_rowids);
    free(do_insert);

    if (durable && error && *error == PCACHE_PUT_PAGES_OK) {
        if (!do_sync(v)) {
            SET_ERR(posix_error, errno);
            SET_ERR(error, PCACHE_PUT_PAGES_IO_ERROR);
        }
    }

unlock:
    pthread_mutex_unlock(&v->mutex);
}

void pcache_get_pages(pcache_handle           handle,
                      size_t                  count,
                      const void             *ids,
                      void                   *pages_buffer,
                      pcache_get_pages_error *error,
                      int                    *sqlite_error,
                      int                    *posix_error) {
    SET_ERR(error, PCACHE_GET_PAGES_OK);
    SET_ERR(sqlite_error, SQLITE_OK);
    SET_ERR(posix_error, 0);

    if (count == 0)
        return;

    pcache_volume *v = vol_from_handle(handle);
    if (!v) {
        SET_ERR(error, PCACHE_GET_PAGES_INVALID_HANDLE);
        return;
    }

    int     sq     = SQLITE_OK;
    int64_t rowid  = 0;
    bool    all_ok = true;

    for (size_t i = 0; i < count && all_ok; i++) {
        const void *id = (const uint8_t *)ids + i * v->config.id_size;
        rowid          = find_rowid(v, id, &sq);
        if (rowid < 0) {
            SET_ERR(sqlite_error, sq);
            SET_ERR(error, PCACHE_GET_PAGES_SQLITE_ERROR);
            all_ok = false;
        } else if (rowid == 0) {
            SET_ERR(error, PCACHE_GET_PAGES_NOT_FOUND);
            all_ok = false;
        } else {
            void   *buf         = (uint8_t *)pages_buffer + i * v->config.page_size;
            off_t   byte_offset = (off_t)(rowid - 1) * v->config.page_size;
            ssize_t rd          = pread(v->data_fd, buf, v->config.page_size, byte_offset);
            if (rd != (ssize_t)v->config.page_size) {
                SET_ERR(posix_error, errno);
                SET_ERR(error, PCACHE_GET_PAGES_IO_ERROR);
                all_ok = false;
            }
        }
    }

    pthread_mutex_unlock(&v->mutex);
}

bool pcache_check_page(pcache_handle handle, const void *id, pcache_check_page_error *error, int *sqlite_error) {
    SET_ERR(error, PCACHE_CHECK_PAGE_OK);
    SET_ERR(sqlite_error, SQLITE_OK);

    pcache_volume *v = vol_from_handle(handle);
    if (!v) {
        SET_ERR(error, PCACHE_CHECK_PAGE_INVALID_HANDLE);
        return false;
    }

    int     sq    = SQLITE_OK;
    int64_t rowid = find_rowid(v, id, &sq);
    bool    found = rowid > 0;

    if (rowid < 0) {
        SET_ERR(sqlite_error, sq);
        SET_ERR(error, PCACHE_CHECK_PAGE_SQLITE_ERROR);
        found = false;
    }

    pthread_mutex_unlock(&v->mutex);
    return found;
}

void pcache_delete_page(pcache_handle             handle,
                        const void               *id,
                        bool                      wipe_data_file,
                        bool                      durable,
                        pcache_delete_page_error *error,
                        int                      *sqlite_error,
                        int                      *posix_error) {
    SET_ERR(error, PCACHE_DELETE_PAGE_OK);
    SET_ERR(sqlite_error, SQLITE_OK);
    SET_ERR(posix_error, 0);

    pcache_volume *v = vol_from_handle(handle);
    if (!v) {
        SET_ERR(error, PCACHE_DELETE_PAGE_INVALID_HANDLE);
        return;
    }

    int     sq    = SQLITE_OK;
    int64_t rowid = find_rowid(v, id, &sq);
    if (rowid < 0) {
        SET_ERR(sqlite_error, sq);
        SET_ERR(error, PCACHE_DELETE_PAGE_SQLITE_ERROR);
        goto unlock;
    }
    if (rowid == 0) {
        SET_ERR(error, PCACHE_DELETE_PAGE_NOT_FOUND);
        goto unlock;
    }

    /* Atomically NULL out the index row. */
    {
        int rc = db_exec(v->db, "BEGIN IMMEDIATE");
        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_DELETE_PAGE_SQLITE_ERROR);
            goto unlock;
        }

        sqlite3_stmt *s;
        rc = sqlite3_prepare_v2(v->db, "UPDATE pages SET id_hash=NULL,id=NULL WHERE rowid=?", -1, &s, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(s, 1, rowid);
            rc = sqlite3_step(s);
            if (rc == SQLITE_DONE)
                rc = SQLITE_OK;
            sqlite3_finalize(s);
        }

        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_DELETE_PAGE_SQLITE_ERROR);
            db_exec(v->db, "ROLLBACK");
            goto unlock;
        }

        rc = db_exec(v->db, "COMMIT");
        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_DELETE_PAGE_SQLITE_ERROR);
            goto unlock;
        }
    }

    /* FIXED: return the slot to the free list (best-effort; OOM is silently ignored). */
    if (v->config.capacity_policy == PCACHE_CAPACITY_FIXED)
        rv_push(&v->free_list, rowid);

    /* Wipe data file after the index commit — the page is already unreachable. */
    if (wipe_data_file) {
        off_t    byte_offset = (off_t)(rowid - 1) * v->config.page_size;
        uint8_t *zeros       = calloc(1, v->config.page_size);
        if (zeros) {
            if (pwrite(v->data_fd, zeros, v->config.page_size, byte_offset) != (ssize_t)v->config.page_size) {
                SET_ERR(posix_error, errno);
                SET_ERR(error, PCACHE_DELETE_PAGE_IO_ERROR);
            }
            free(zeros);
        }
    }

    if (durable && error && *error == PCACHE_DELETE_PAGE_OK) {
        if (!do_sync(v)) {
            SET_ERR(posix_error, errno);
            SET_ERR(error, PCACHE_DELETE_PAGE_IO_ERROR);
        }
    }

unlock:
    pthread_mutex_unlock(&v->mutex);
}
