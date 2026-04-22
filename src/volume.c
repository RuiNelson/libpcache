#include "db.h"
#include "handle.h"

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
    SET_ERR(error, PCACHE_CREATE_OK);
    SET_ERR(sqlite_error, SQLITE_OK);
    SET_ERR(posix_error, 0);

    if (!paths || !paths->database_path || !paths->data_path || !config || config->page_size == 0 ||
        config->max_pages == 0 || config->id_size == 0) {
        SET_ERR(error, PCACHE_CREATE_INVALID_ARGUMENT);
        return;
    }

    int db_fd = open(paths->database_path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (db_fd < 0) {
        SET_ERR(posix_error, errno);
        if (errno == EEXIST)
            SET_ERR(error, PCACHE_CREATE_FILE_EXISTS);
        else
            SET_ERR(error, PCACHE_CREATE_IO_ERROR);
        return;
    }

    int data_fd = open(paths->data_path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (data_fd < 0) {
        SET_ERR(posix_error, errno);
        if (errno == EEXIST)
            SET_ERR(error, PCACHE_CREATE_FILE_EXISTS);
        else
            SET_ERR(error, PCACHE_CREATE_IO_ERROR);
        close(db_fd);
        unlink(paths->database_path);
        return;
    }

    /* The db_fd was used solely for the O_EXCL existence check; SQLite will
     * open the file with its own fd.  Close now to avoid a descriptor leak. */
    close(db_fd);

    sqlite3 *db = NULL;
    int      rc = sqlite3_open_v2(paths->database_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        SET_ERR(sqlite_error, rc);
        SET_ERR(error, PCACHE_CREATE_SQLITE_ERROR);
        close(data_fd);
        unlink(paths->data_path);
        close(db_fd);
        sqlite3_close(db);
        return;
    }

    db_configure(db);

    /* ── Schema ── */
    const char *ddl = "CREATE TABLE metadata (key TEXT NOT NULL, value BLOB NOT NULL);"
                      "CREATE TABLE pages    (id_hash INTEGER, id BLOB);"
                      "CREATE INDEX idx_lookup ON pages(id_hash) WHERE id_hash IS NOT NULL;";

    rc = db_exec(db, ddl);
    if (rc == SQLITE_OK && config->capacity_policy == PCACHE_CAPACITY_FIFO) {
        rc = db_exec(db, "CREATE TABLE fifo_cursor (next_rowid INTEGER NOT NULL);");
        if (rc == SQLITE_OK)
            rc = db_exec(db, "INSERT INTO fifo_cursor VALUES (1);");
    }
    if (rc != SQLITE_OK) {
        SET_ERR(sqlite_error, rc);
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
        rc = db_exec(db, "BEGIN");
        if (rc == SQLITE_OK) {
            sqlite3_stmt *s;
            rc = sqlite3_prepare_v2(db, "INSERT INTO pages(id_hash,id) VALUES(NULL,NULL)", -1, &s, NULL);
            if (rc == SQLITE_OK) {
                for (uint32_t i = 0; i < config->max_pages && rc == SQLITE_OK; i++) {
                    rc = sqlite3_step(s);
                    if (rc == SQLITE_DONE)
                        rc = SQLITE_OK;
                    sqlite3_reset(s);
                }
                sqlite3_finalize(s);
            }
            if (rc == SQLITE_OK)
                rc = db_exec(db, "COMMIT");
            else
                db_exec(db, "ROLLBACK");
        }
        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
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

pcache_handle pcache_open(const pcache_file_pair *paths,
                          bool                    preload_free_list,
                          pcache_open_error      *error,
                          int                    *sqlite_error,
                          int                    *posix_error) {
    SET_ERR(error, PCACHE_OPEN_OK);
    SET_ERR(sqlite_error, SQLITE_OK);
    SET_ERR(posix_error, 0);

    if (!paths || !paths->database_path || !paths->data_path) {
        SET_ERR(error, PCACHE_OPEN_NOT_FOUND);
        return 0;
    }

    if (access(paths->database_path, F_OK) != 0 || access(paths->data_path, F_OK) != 0) {
        SET_ERR(posix_error, errno);
        SET_ERR(error, PCACHE_OPEN_NOT_FOUND);
        return 0;
    }

    pcache_volume *v = alloc_slot();
    if (!v) {
        SET_ERR(error, PCACHE_OPEN_TOO_MANY_HANDLES);
        return 0;
    }

    memset(&v->free_list, 0, sizeof v->free_list);
    v->row_count = 0;
    v->fifo_next = 1;
    v->data_fd   = -1;
    v->db        = NULL;

    v->data_fd = open(paths->data_path, O_RDWR);
    if (v->data_fd < 0) {
        SET_ERR(posix_error, errno);
        SET_ERR(error, PCACHE_OPEN_IO_ERROR);
        release_slot(v);
        return 0;
    }

    int rc = sqlite3_open_v2(paths->database_path, &v->db, SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK) {
        SET_ERR(sqlite_error, rc);
        SET_ERR(error, PCACHE_OPEN_SQLITE_ERROR);
        close(v->data_fd);
        sqlite3_close(v->db);
        release_slot(v);
        return 0;
    }

    db_configure(v->db);

    /* ── Read metadata ── */
    uint32_t version = 0, page_size = 0, max_pages = 0, id_size = 0;
    char     policy_str[16] = {0};

    if (!db_meta_read_u32(v->db, "version", &version) ||
        !db_meta_read_str(v->db, "capacity_policy", policy_str, sizeof policy_str) ||
        !db_meta_read_u32(v->db, "page_size", &page_size) || !db_meta_read_u32(v->db, "max_pages", &max_pages) ||
        !db_meta_read_u32(v->db, "id_size", &id_size) || version != PCACHE_SCHEMA_VERSION) {
        SET_ERR(error, PCACHE_OPEN_CORRUPT);
        goto fail;
    }

    if (strcmp(policy_str, "FIFO") == 0)
        v->config.capacity_policy = PCACHE_CAPACITY_FIFO;
    else if (strcmp(policy_str, "FIXED") == 0)
        v->config.capacity_policy = PCACHE_CAPACITY_FIXED;
    else {
        SET_ERR(error, PCACHE_OPEN_CORRUPT);
        goto fail;
    }
    v->config.page_size = page_size;
    v->config.max_pages = max_pages;
    v->config.id_size   = id_size;

    /* ── Total row count ── */
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(v->db, "SELECT COUNT(*) FROM pages", -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW)
                v->row_count = (uint32_t)sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }
    }

    /* ── FIFO cursor ── */
    if (v->config.capacity_policy == PCACHE_CAPACITY_FIFO) {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(v->db, "SELECT next_rowid FROM fifo_cursor", -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW)
                v->fifo_next = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }
    }

    /* ── Preload free list (FIXED only) ── */
    /* Note: preload_free_list is ignored on FIFO volumes per API spec. */
    if (v->config.capacity_policy == PCACHE_CAPACITY_FIXED && preload_free_list) {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(v->db, "SELECT rowid FROM pages WHERE id_hash IS NULL ORDER BY rowid", -1, &s, NULL) ==
            SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW)
                rv_push(&v->free_list, sqlite3_column_int64(s, 0));
            sqlite3_finalize(s);
        }
    }
    /* On FIFO volumes, preload_free_list is intentionally ignored. */

    return handle_of(v);

fail:
    sqlite3_close(v->db);
    close(v->data_fd);
    release_slot(v);
    return 0;
}

void pcache_close(pcache_handle handle, pcache_close_error *error, int *sqlite_error, int *posix_error) {
    SET_ERR(error, PCACHE_CLOSE_OK);
    SET_ERR(sqlite_error, SQLITE_OK);
    SET_ERR(posix_error, 0);

    pcache_volume *v = vol_from_handle(handle);
    if (!v) {
        SET_ERR(error, PCACHE_CLOSE_INVALID_HANDLE);
        return;
    }

    /* Flip in_use while still holding v->mutex, so that any concurrent
     * vol_from_handle (which takes v->mutex first, then reads in_use) will
     * observe the close and return NULL. */
    release_slot(v);

    if (v->db) {
        int rc = sqlite3_close(v->db);
        if (rc != SQLITE_OK) {
            SET_ERR(sqlite_error, rc);
            SET_ERR(error, PCACHE_CLOSE_SQLITE_ERROR);
        }
        v->db = NULL;
    }

    if (v->data_fd >= 0) {
        if (close(v->data_fd) != 0) {
            SET_ERR(posix_error, errno);
            if (error && *error == PCACHE_CLOSE_OK)
                SET_ERR(error, PCACHE_CLOSE_IO_ERROR);
        }
        v->data_fd = -1;
    }

    rv_free(&v->free_list);

    pthread_mutex_unlock(&v->mutex);
}

pcache_configuration
pcache_get_configuration(pcache_handle handle, pcache_get_configuration_error *error, int *sqlite_error) {
    pcache_configuration cfg = {0};
    SET_ERR(error, PCACHE_GET_CONFIGURATION_OK);
    SET_ERR(sqlite_error, SQLITE_OK);

    pcache_volume *v = vol_from_handle(handle);
    if (!v) {
        SET_ERR(error, PCACHE_GET_CONFIGURATION_INVALID_HANDLE);
        return cfg;
    }

    cfg = v->config;
    pthread_mutex_unlock(&v->mutex);
    return cfg;
}
