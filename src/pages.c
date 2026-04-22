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
        v->fifo_next = (target_rowid % (int64_t)v->config.max_pages) + 1;

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
