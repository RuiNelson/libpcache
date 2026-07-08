#include "db.h"
#include "handle.h"
#include "macros.h"
#include "pages_util.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void pcache_create(const pcache_file_pair     *paths,
                   const pcache_configuration *config,
                   bool                        preallocate_database,
                   bool                        preallocate_datafile,
                   pcache_create_error        *error,
                   int                        *sqlite_error,
                   int                        *posix_error) {
    ZERO_3ERR(error, sqlite_error, posix_error);

    if (!paths || !paths->database_path || !paths->data_path || !config || config->page_size == 0 ||
        config->max_pages == 0 || config->id_size == 0) {
        SET_ERR(error, PCACHE_CREATE_INVALID_ARGUMENT);
        return;
    }

    int db_fd = open(paths->database_path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (db_fd < 0) {
        SET_ERR(posix_error, errno);
        if (errno == EEXIST) {
            SET_ERR(error, PCACHE_CREATE_FILE_EXISTS);
        } else {
            SET_ERR(error, PCACHE_CREATE_IO_ERROR);
        }
        return;
    }

    int data_fd = open(paths->data_path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (data_fd < 0) {
        SET_ERR(posix_error, errno);

        if (errno == EEXIST) {
            SET_ERR(error, PCACHE_CREATE_FILE_EXISTS);
        } else {
            SET_ERR(error, PCACHE_CREATE_IO_ERROR);
        }

        close(db_fd);
        unlink(paths->database_path);
        return;
    }

    /* The db_fd was used solely for the O_EXCL existence check; SQLite will
     * open the file with its own fd.  Close now to avoid a descriptor leak. */
    close(db_fd);

    sqlite3 *db        = NULL;
    int      sqlite_rc = sqlite3_open_v2(paths->database_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (sqlite_rc != SQLITE_OK) {
        SET_ERR(sqlite_error, sqlite_rc);
        SET_ERR(error, PCACHE_CREATE_SQLITE_ERROR);
        close(data_fd);
        unlink(paths->data_path);
        unlink(paths->database_path);
        sqlite3_close(db);
        return;
    }

    db_configure(db);

    /* ── Schema ── */
    const char *ddl = "CREATE TABLE metadata (key TEXT NOT NULL, value BLOB NOT NULL);"
                      "CREATE TABLE pages    (id_hash INTEGER, id BLOB);"
                      "CREATE INDEX idx_lookup     ON pages(id_hash) WHERE id_hash IS NOT NULL;"
                      "CREATE INDEX idx_free_slots ON pages(id_hash) WHERE id_hash IS NULL;";

    sqlite_rc = db_exec(db, ddl);
    if (sqlite_rc != SQLITE_OK) {
        SET_ERR(sqlite_error, sqlite_rc);
        SET_ERR(error, PCACHE_CREATE_SQLITE_ERROR);
        goto fail;
    }

    /* ── Metadata ── */
    {
        const char *policy_str = (config->capacity_policy == PCACHE_CAPACITY_FIFO) ? "FIFO" : "FIXED";
        if (!db_meta_write_u32(db, "version", PCACHE_SCHEMA_VERSION) ||
            !db_meta_write_str(db, "capacity_policy", policy_str) ||
            !db_meta_write_u32(db, "page_size", config->page_size) ||
            !db_meta_write_u32(db, "max_pages", config->max_pages) ||
            !db_meta_write_u32(db, "id_size", config->id_size)) {
            SET_ERR(sqlite_error, sqlite3_errcode(db));
            SET_ERR(error, PCACHE_CREATE_SQLITE_ERROR);
            goto fail;
        }
    }

    /* ── Preallocate database ── */
    if (preallocate_database) {
        sqlite_rc = db_exec(db, "BEGIN");
        if (sqlite_rc == SQLITE_OK) {
            sqlite3_stmt *statement;
            sqlite_rc = sqlite3_prepare_v2(db, "INSERT INTO pages(id_hash,id) VALUES(NULL,NULL)", -1, &statement, NULL);
            if (sqlite_rc == SQLITE_OK) {
                for (size_t idx = 0; idx < config->max_pages && sqlite_rc == SQLITE_OK; idx++) {
                    sqlite_rc = sqlite3_step(statement);
                    if (sqlite_rc == SQLITE_DONE)
                        sqlite_rc = SQLITE_OK;
                    sqlite3_reset(statement);
                }
                sqlite3_finalize(statement);
            }
            if (sqlite_rc == SQLITE_OK)
                sqlite_rc = db_exec(db, "COMMIT");
            else
                db_exec(db, "ROLLBACK");
        }
        if (sqlite_rc != SQLITE_OK) {
            SET_ERR(sqlite_error, sqlite_rc);
            SET_ERR(error, PCACHE_CREATE_SQLITE_ERROR);
            goto fail;
        }
    }

    /* ── Preallocate data file ── */
    if (preallocate_datafile) {
        off_t total = (off_t)config->max_pages * config->page_size;
        if (ftruncate(data_fd, total) != 0) {
            SET_ERR(posix_error, errno);
            SET_ERR(error, PCACHE_CREATE_IO_ERROR);
            goto fail;
        }
    }

    sqlite3_close(db);
    close(data_fd);
    return;

fail:
    sqlite3_close(db);
    close(data_fd);
    unlink(paths->data_path);
    unlink(paths->database_path);
}

pcache_handle
pcache_open(const pcache_file_pair *paths, pcache_open_error *error, int *sqlite_error, int *posix_error) {
    ZERO_3ERR(error, sqlite_error, posix_error);

    if (!paths || !paths->database_path || !paths->data_path) {
        SET_ERR(error, PCACHE_OPEN_NOT_FOUND);
        return 0;
    }

    if (access(paths->database_path, F_OK) != 0 || access(paths->data_path, F_OK) != 0) {
        SET_ERR(posix_error, errno);
        SET_ERR(error, PCACHE_OPEN_NOT_FOUND);
        return 0;
    }

    pcache_volume *volume = allocate_slot();
    if (!volume) {
        SET_ERR(error, PCACHE_OPEN_OUT_OF_MEMORY);
        return 0;
    }

    pthread_mutex_lock(&volume->mutex);

    volume->row_count                  = 0;
    volume->fd                         = -1;
    volume->db                         = NULL;
    volume->statement_find_rowid       = NULL;
    volume->statement_insert           = NULL;
    volume->statement_update_slot      = NULL;
    volume->statement_delete           = NULL;
    volume->statement_find_free_slot   = NULL;
    volume->statement_find_fifo_cursor = NULL;
    volume->wipe_buffer                = NULL;

    volume->fd = open(paths->data_path, O_RDWR);
    if (volume->fd < 0) {
        SET_ERR(posix_error, errno);
        SET_ERR(error, PCACHE_OPEN_IO_ERROR);
        goto fail_locked;
    }

    int sqlite_rc = sqlite3_open_v2(paths->database_path, &volume->db, SQLITE_OPEN_READWRITE, NULL);
    if (sqlite_rc != SQLITE_OK) {
        SET_ERR(sqlite_error, sqlite_rc);
        SET_ERR(error, PCACHE_OPEN_SQLITE_ERROR);
        goto fail_locked;
    }

    db_configure(volume->db);

    if (sqlite3_prepare_v2(
            volume->db, "SELECT rowid FROM pages WHERE id_hash=? AND id=?", -1, &volume->statement_find_rowid, NULL) !=
            SQLITE_OK ||
        sqlite3_prepare_v2(
            volume->db, "INSERT INTO pages(id_hash,id) VALUES(?,?)", -1, &volume->statement_insert, NULL) !=
            SQLITE_OK ||
        sqlite3_prepare_v2(
            volume->db, "UPDATE pages SET id_hash=?,id=? WHERE rowid=?", -1, &volume->statement_update_slot, NULL) !=
            SQLITE_OK ||
        sqlite3_prepare_v2(
            volume->db, "UPDATE pages SET id_hash=NULL,id=NULL WHERE rowid=?", -1, &volume->statement_delete, NULL) !=
            SQLITE_OK ||
        sqlite3_prepare_v2(volume->db,
                           "SELECT rowid FROM pages WHERE id_hash IS NULL AND rowid > ? ORDER BY rowid LIMIT 1",
                           -1,
                           &volume->statement_find_free_slot,
                           NULL) != SQLITE_OK) {
        SET_ERR(sqlite_error, sqlite3_errcode(volume->db));
        SET_ERR(error, PCACHE_OPEN_SQLITE_ERROR);
        goto fail_locked;
    }

    /* ── Read metadata ── */
    uint32_t version = 0, page_size = 0, max_pages = 0, id_size = 0;
    char     policy_str[16] = {0};

    int meta_rc;
    if ((meta_rc = db_meta_read_u32(volume->db, "version", &version)) != SQLITE_ROW ||
        (meta_rc = db_meta_read_str(volume->db, "capacity_policy", policy_str, sizeof policy_str)) != SQLITE_ROW ||
        (meta_rc = db_meta_read_u32(volume->db, "page_size", &page_size)) != SQLITE_ROW ||
        (meta_rc = db_meta_read_u32(volume->db, "max_pages", &max_pages)) != SQLITE_ROW ||
        (meta_rc = db_meta_read_u32(volume->db, "id_size", &id_size)) != SQLITE_ROW) {
        if (meta_rc == SQLITE_DONE || meta_rc == SQLITE_CORRUPT) {
            SET_ERR(error, PCACHE_OPEN_CORRUPT);
        } else {
            SET_ERR(sqlite_error, meta_rc);
            SET_ERR(error, PCACHE_OPEN_SQLITE_ERROR);
        }
        goto fail_locked;
    }

    if (version > PCACHE_SCHEMA_VERSION) {
        SET_ERR(error, PCACHE_OPEN_SCHEMA_VERSION_TOO_HIGH);
        goto fail_locked;
    }

    if (version != PCACHE_SCHEMA_VERSION) {
        SET_ERR(error, PCACHE_OPEN_CORRUPT);
        goto fail_locked;
    }

    if (page_size == 0 || max_pages == 0 || id_size == 0) {
        SET_ERR(error, PCACHE_OPEN_CORRUPT);
        goto fail_locked;
    }

    if (strcmp(policy_str, "FIFO") == 0)
        volume->config.capacity_policy = PCACHE_CAPACITY_FIFO;
    else if (strcmp(policy_str, "FIXED") == 0)
        volume->config.capacity_policy = PCACHE_CAPACITY_FIXED;
    else {
        SET_ERR(error, PCACHE_OPEN_CORRUPT);
        goto fail_locked;
    }
    volume->config.page_size = page_size;
    volume->config.max_pages = max_pages;
    volume->config.id_size   = id_size;

    /* ── Total row count ── */
    /* A silent failure here would leave row_count at 0, sending later FIFO
     * puts down the fill-up path and inserting rows beyond max_pages. */
    {
        sqlite3_stmt *statement = NULL;
        int           count_rc  = sqlite3_prepare_v2(volume->db, "SELECT COUNT(*) FROM pages", -1, &statement, NULL);
        if (count_rc == SQLITE_OK) {
            count_rc = sqlite3_step(statement);
            if (count_rc == SQLITE_ROW) {
                volume->row_count = (size_t)sqlite3_column_int64(statement, 0);
                count_rc          = SQLITE_OK;
            }
            sqlite3_finalize(statement);
        }
        if (count_rc != SQLITE_OK) {
            SET_ERR(sqlite_error, count_rc);
            SET_ERR(error, PCACHE_OPEN_SQLITE_ERROR);
            goto fail_locked;
        }
    }

    /* ── FIFO: prepare implicit-cursor lookup and apply auto-recovery ── */
    if (volume->config.capacity_policy == PCACHE_CAPACITY_FIFO) {
        /* Returns the lowest ROWID where id_hash IS NULL and the predecessor
         * (with wrap-around 1 -> max_pages) has id_hash IS NOT NULL. */
        const char *cursor_sql = "SELECT p.rowid FROM pages p "
                                 "WHERE p.id_hash IS NULL "
                                 "  AND EXISTS (SELECT 1 FROM pages q "
                                 "              WHERE q.rowid = CASE WHEN p.rowid = 1 THEN ?1 ELSE p.rowid - 1 END "
                                 "                AND q.id_hash IS NOT NULL) "
                                 "ORDER BY p.rowid LIMIT 1";
        if (sqlite3_prepare_v2(volume->db, cursor_sql, -1, &volume->statement_find_fifo_cursor, NULL) != SQLITE_OK) {
            SET_ERR(sqlite_error, sqlite3_errcode(volume->db));
            SET_ERR(error, PCACHE_OPEN_SQLITE_ERROR);
            goto fail_locked;
        }

        /* Auto-recovery: if every slot is occupied (only reachable via crash
         * between writing slot max_pages and nulling slot 1), restore the
         * invariant by nulling slot 1. */
        if (volume->row_count == volume->config.max_pages) {
            sqlite3_stmt *count_stmt = NULL;
            int64_t       live_count = 0;
            int           sqlite_rc  = sqlite3_prepare_v2(
                volume->db, "SELECT COUNT(*) FROM pages WHERE id_hash IS NOT NULL", -1, &count_stmt, NULL);
            if (sqlite_rc == SQLITE_OK) {
                if (sqlite3_step(count_stmt) == SQLITE_ROW)
                    live_count = sqlite3_column_int64(count_stmt, 0);
                sqlite3_finalize(count_stmt);
            } else {
                SET_ERR(sqlite_error, sqlite_rc);
                SET_ERR(error, PCACHE_OPEN_SQLITE_ERROR);
                goto fail_locked;
            }

            if ((uint32_t)live_count == volume->config.max_pages) {
                sqlite_rc = db_exec(volume->db, "UPDATE pages SET id_hash=NULL, id=NULL WHERE rowid=1");
                if (sqlite_rc != SQLITE_OK) {
                    SET_ERR(sqlite_error, sqlite_rc);
                    SET_ERR(error, PCACHE_OPEN_SQLITE_ERROR);
                    goto fail_locked;
                }
            }
        }

        /* Auto-recovery: a volume with more than one empty run (crash during
         * delete compaction, or holes left by a pre-compaction library
         * version) violates the single-empty-run invariant that makes the
         * implicit cursor unambiguous. Merge the runs before handing the
         * volume out. */
        {
            bool needs_compaction = false;
            if (volume->row_count == volume->config.max_pages) {
                sqlite3_stmt *runs_stmt  = NULL;
                int64_t       run_starts = 0;
                int runs_rc = sqlite3_prepare_v2(volume->db,
                                                 "SELECT COUNT(*) FROM pages p WHERE p.id_hash IS NULL AND EXISTS ("
                                                 "  SELECT 1 FROM pages q WHERE q.rowid = CASE WHEN p.rowid = 1 THEN ?1"
                                                 "  ELSE p.rowid - 1 END AND q.id_hash IS NOT NULL)",
                                                 -1,
                                                 &runs_stmt,
                                                 NULL);
                if (runs_rc == SQLITE_OK) {
                    sqlite3_bind_int64(runs_stmt, 1, (int64_t)volume->config.max_pages);
                    runs_rc = sqlite3_step(runs_stmt);
                    if (runs_rc == SQLITE_ROW) {
                        run_starts = sqlite3_column_int64(runs_stmt, 0);
                        runs_rc    = SQLITE_OK;
                    }
                    sqlite3_finalize(runs_stmt);
                }
                if (runs_rc != SQLITE_OK) {
                    SET_ERR(sqlite_error, runs_rc);
                    SET_ERR(error, PCACHE_OPEN_SQLITE_ERROR);
                    goto fail_locked;
                }
                needs_compaction = run_starts > 1;
            } else if (volume->row_count > 0) {
                /* Fill-up phase must have no NULL rows at all. */
                sqlite3_stmt *hole_stmt = NULL;
                int           hole_rc   = sqlite3_prepare_v2(
                    volume->db, "SELECT 1 FROM pages WHERE id_hash IS NULL LIMIT 1", -1, &hole_stmt, NULL);
                if (hole_rc == SQLITE_OK) {
                    hole_rc = sqlite3_step(hole_stmt);
                    sqlite3_finalize(hole_stmt);
                    if (hole_rc == SQLITE_ROW) {
                        needs_compaction = true;
                        hole_rc          = SQLITE_OK;
                    } else if (hole_rc == SQLITE_DONE) {
                        hole_rc = SQLITE_OK;
                    }
                }
                if (hole_rc != SQLITE_OK) {
                    SET_ERR(sqlite_error, hole_rc);
                    SET_ERR(error, PCACHE_OPEN_SQLITE_ERROR);
                    goto fail_locked;
                }
            }

            if (needs_compaction) {
                int64_t run_end = 0;
                if (volume->row_count == volume->config.max_pages) {
                    int cursor_rc = 0;
                    run_end       = fifo_locate_run_end(volume, &cursor_rc);
                    if (run_end < 0) {
                        SET_ERR(sqlite_error, cursor_rc);
                        SET_ERR(error, PCACHE_OPEN_SQLITE_ERROR);
                        goto fail_locked;
                    }
                }

                int compact_sqlite_rc = 0;
                int compact_posix_rc  = 0;
                switch (fifo_compact_holes(volume, run_end, &compact_sqlite_rc, &compact_posix_rc)) {
                    case FIFO_COMPACT_OK:
                        break;
                    case FIFO_COMPACT_SQLITE_ERROR:
                        SET_ERR(sqlite_error, compact_sqlite_rc);
                        SET_ERR(error, PCACHE_OPEN_SQLITE_ERROR);
                        goto fail_locked;
                    case FIFO_COMPACT_IO_ERROR:
                        SET_ERR(posix_error, compact_posix_rc);
                        SET_ERR(error, PCACHE_OPEN_IO_ERROR);
                        goto fail_locked;
                    case FIFO_COMPACT_OUT_OF_MEMORY:
                        SET_ERR(error, PCACHE_OPEN_OUT_OF_MEMORY);
                        goto fail_locked;
                }
            }
        }
    }

    pcache_handle h = handle_of(volume);
    pthread_mutex_unlock(&volume->mutex);
    return h;

fail_locked:
    if (volume->statement_find_rowid) {
        sqlite3_finalize(volume->statement_find_rowid);
        volume->statement_find_rowid = NULL;
    }
    if (volume->statement_insert) {
        sqlite3_finalize(volume->statement_insert);
        volume->statement_insert = NULL;
    }
    if (volume->statement_update_slot) {
        sqlite3_finalize(volume->statement_update_slot);
        volume->statement_update_slot = NULL;
    }
    if (volume->statement_delete) {
        sqlite3_finalize(volume->statement_delete);
        volume->statement_delete = NULL;
    }
    if (volume->statement_find_free_slot) {
        sqlite3_finalize(volume->statement_find_free_slot);
        volume->statement_find_free_slot = NULL;
    }
    if (volume->statement_find_fifo_cursor) {
        sqlite3_finalize(volume->statement_find_fifo_cursor);
        volume->statement_find_fifo_cursor = NULL;
    }
    if (volume->wipe_buffer) {
        free(volume->wipe_buffer);
        volume->wipe_buffer = NULL;
    }
    if (volume->db)
        sqlite3_close(volume->db);
    if (volume->fd >= 0)
        close(volume->fd);
    release_slot(volume);
    pthread_mutex_unlock(&volume->mutex);
    return 0;
}

void pcache_close(pcache_handle handle, pcache_close_error *error, int *sqlite_error, int *posix_error) {
    ZERO_3ERR(error, sqlite_error, posix_error);

    pcache_volume *volume = volume_from_handle_for_close(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_CLOSE_INVALID_HANDLE);
        return;
    }

    if (volume->statement_find_rowid) {
        sqlite3_finalize(volume->statement_find_rowid);
        volume->statement_find_rowid = NULL;
    }
    if (volume->statement_insert) {
        sqlite3_finalize(volume->statement_insert);
        volume->statement_insert = NULL;
    }
    if (volume->statement_update_slot) {
        sqlite3_finalize(volume->statement_update_slot);
        volume->statement_update_slot = NULL;
    }
    if (volume->statement_delete) {
        sqlite3_finalize(volume->statement_delete);
        volume->statement_delete = NULL;
    }
    if (volume->statement_find_free_slot) {
        sqlite3_finalize(volume->statement_find_free_slot);
        volume->statement_find_free_slot = NULL;
    }
    if (volume->statement_find_fifo_cursor) {
        sqlite3_finalize(volume->statement_find_fifo_cursor);
        volume->statement_find_fifo_cursor = NULL;
    }
    if (volume->wipe_buffer) {
        free(volume->wipe_buffer);
        volume->wipe_buffer = NULL;
    }

    if (volume->db) {
        int rc = sqlite3_close(volume->db);
        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_CLOSE_SQLITE_ERROR);
        }
        volume->db = NULL;
    }

    if (volume->fd >= 0) {
        if (fsync(volume->fd) != 0) {
            SET_ERR(posix_error, errno);
            if (error && *error == PCACHE_CLOSE_OK)
                SET_ERR(error, PCACHE_CLOSE_IO_ERROR);
        }
        if (close(volume->fd) != 0) {
            SET_ERR(posix_error, errno);
            if (error && *error == PCACHE_CLOSE_OK)
                SET_ERR(error, PCACHE_CLOSE_IO_ERROR);
        }
        volume->fd = -1;
    }

    pthread_mutex_unlock(&volume->mutex);
}

pcache_configuration pcache_inspect_configuration(pcache_handle handle, pcache_inspect_configuration_error *error) {
    pcache_configuration cfg = {0};
    SET_ERR(error, PCACHE_INSPECT_CONFIGURATION_OK);

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_INSPECT_CONFIGURATION_INVALID_HANDLE);
        return cfg;
    }

    cfg = volume->config;
    pthread_mutex_unlock(&volume->mutex);
    return cfg;
}

pcache_page_count
pcache_inspect_page_count(pcache_handle handle, pcache_inspect_page_count_error *error, int *sqlite_error) {
    pcache_page_count counts = {0};
    ZERO_2ERR(error, sqlite_error);

    pcache_volume *volume = volume_from_handle(handle);
    if (!volume) {
        SET_ERR(error, PCACHE_INSPECT_PAGE_COUNT_INVALID_HANDLE);
        return counts;
    }

    sqlite3_stmt *statement;
    int           sqlite_rc =
        sqlite3_prepare_v2(volume->db, "SELECT COUNT(*) FROM pages WHERE id_hash IS NOT NULL", -1, &statement, NULL);
    if (sqlite_rc != SQLITE_OK) {
        SET_2ERR(error, sqlite_error, PCACHE_INSPECT_PAGE_COUNT_SQLITE_ERROR, sqlite_rc);
        pthread_mutex_unlock(&volume->mutex);
        return counts;
    }

    sqlite_rc = sqlite3_step(statement);
    if (sqlite_rc != SQLITE_ROW) {
        SET_2ERR(error, sqlite_error, PCACHE_INSPECT_PAGE_COUNT_SQLITE_ERROR, sqlite_rc);
        sqlite3_finalize(statement);
        pthread_mutex_unlock(&volume->mutex);
        return counts;
    }

    counts.used = (uint32_t)sqlite3_column_int64(statement, 0);
    counts.free = volume->config.max_pages - counts.used;
    sqlite3_finalize(statement);

    pthread_mutex_unlock(&volume->mutex);
    return counts;
}
